// SPDX-License-Identifier: MIT

#include "xe_p2p_carveout.h"

#include <linux/anon_inodes.h>
#include <linux/cgroup_dmem.h>
#include <linux/file.h>
#include <linux/mm.h>
#include <linux/pci.h>
#include <linux/pci-p2pdma.h>

#include <drm/drm_drv.h>
#include <drm/drm_managed.h>
#include <drm/drm_pagemap.h>
#include <drm/drm_print.h>
#include <uapi/drm/xe_drm.h>

#include "regs/xe_bars.h"
#include "xe_device.h"
#include "xe_macros.h"
#include "xe_module.h"
#include "xe_pm.h"
#include "xe_svm.h"
#include "xe_vram_types.h"

#define P2P_FAULT_BATCH	64

struct xe_p2p_chunk {
	struct pci_p2pmem_pages pages;
	struct work_struct release_work;
	refcount_t refs;
	struct xe_device *xe;
	struct list_head link;
	struct rw_semaphore lock;
	struct address_space *mapping;
	struct dmem_cgroup_pool_state *pool;
	void *addr;
	size_t size;
};

static struct xe_vram_region *p2p_vram(struct xe_device *xe)
{
	return xe_device_get_root_tile(xe)->mem.vram;
}

static void p2p_destroy_wq(void *wq)
{
	destroy_workqueue(wq);
}

/**
 * xe_p2p_carveout_init() - Hand the top of VRAM to PCI P2PDMA
 * @tile: the tile being initialized
 *
 * Must run before the tile's TTM VRAM manager is initialized. On the root
 * tile, take xe_modparam.p2p_carveout_mb of VRAM, rounded up to 2MiB, off the
 * top of VRAM so that TTM never manages it, and register the matching part of
 * the LMEM BAR with pci_p2pdma_add_resource(). The range gets ZONE_DEVICE
 * pages, which userspace allocates in chunks with DRM_IOCTL_XE_P2P_ALLOC.
 *
 * The carve-out needs a BAR covering all of VRAM and isn't offered on
 * platforms that need 64K VRAM pages. The memory stays unpublished and
 * private, so other P2PDMA users and the p2pmem sysfs allocator never get it.
 *
 * Failing to set up the carve-out is not fatal. The device then runs without
 * one and TTM manages all of VRAM.
 *
 * Return: 0 on success, negative error code on failure.
 */
int xe_p2p_carveout_init(struct xe_tile *tile)
{
	struct xe_device *xe = tile_to_xe(tile);
	struct pci_dev *pdev = to_pci_dev(xe->drm.dev);
	struct xe_vram_region *vram = tile->mem.vram;
	u64 size = ALIGN((u64)xe_modparam.p2p_carveout_mb * SZ_1M, SZ_2M);
	struct dmem_cgroup_region *cg;
	u64 start;
	int err;

	if (!IS_DGFX(xe) || !size || tile != xe_device_get_root_tile(xe) ||
	    !vram || !vram->p2p_provider)
		return 0;

	if (xe->info.vram_flags & XE_VRAM_FLAGS_NEED64K) {
		drm_warn(&xe->drm, "P2P carve-out is not supported on this platform\n");
		return 0;
	}

	if (vram->io_size < vram->usable_size) {
		drm_warn(&xe->drm, "P2P carve-out needs a BAR covering all of VRAM\n");
		return 0;
	}

	start = ALIGN_DOWN(vram->usable_size, SZ_2M);
	if (size >= start) {
		drm_warn(&xe->drm, "P2P carve-out of %lluMiB doesn't fit in VRAM\n",
			 size / SZ_1M);
		return 0;
	}
	start -= size;

	err = drmm_mutex_init(&xe->drm, &vram->p2p_lock);
	if (err)
		return err;
	INIT_LIST_HEAD(&vram->p2p_chunks);

	vram->p2p_wq = alloc_workqueue("xe-p2p", WQ_UNBOUND, 0);
	if (!vram->p2p_wq)
		return -ENOMEM;

	err = devm_add_action_or_reset(xe->drm.dev, p2p_destroy_wq, vram->p2p_wq);
	if (err)
		return err;

	pci_p2pmem_set_private(pdev, true);
	err = pci_p2pdma_add_resource(pdev, LMEM_BAR, size,
				      vram->io_start + start -
				      pci_resource_start(pdev, LMEM_BAR));
	if (err) {
		drm_warn(&xe->drm, "failed to register P2P carve-out: %pe\n",
			 ERR_PTR(err));
		return 0;
	}

	cg = drmm_cgroup_register_region(&xe->drm, "p2p",
					 &(struct dmem_cgroup_init){
						.size = size,
					 });
	if (IS_ERR(cg)) {
		drm_warn(&xe->drm, "P2P carve-out has no cgroup region: %pe\n", cg);
		cg = NULL;
	}

	vram->p2p_carveout_cg = cg;
	vram->p2p_carveout_start = start;
	vram->p2p_carveout_size = size;
	drm_info(&xe->drm, "%lluMiB P2P carve-out at VRAM offset %#llx\n",
		 size / SZ_1M, start);
	return 0;
}

/**
 * xe_p2p_carveout_available() - Check whether the device has a P2P carve-out
 * @xe: the &xe_device
 *
 * Return: true if xe_p2p_carveout_init() set up a carve-out
 */
bool xe_p2p_carveout_available(struct xe_device *xe)
{
	struct xe_vram_region *vram = p2p_vram(xe);

	return vram && vram->p2p_carveout_size;
}

static void p2p_chunk_put(struct xe_p2p_chunk *chunk)
{
	if (refcount_dec_and_test(&chunk->refs)) {
		drm_dev_put(&chunk->xe->drm);
		kfree(chunk);
	}
}

static void p2p_chunk_release_work(struct work_struct *work)
{
	struct xe_p2p_chunk *chunk =
		container_of(work, struct xe_p2p_chunk, release_work);

	dmem_cgroup_uncharge(chunk->pool, chunk->size);
	xe_pm_runtime_put(chunk->xe);
	p2p_chunk_put(chunk);
}

static void p2p_chunk_pages_released(struct pci_p2pmem_pages *pages)
{
	struct xe_p2p_chunk *chunk =
		container_of(pages, struct xe_p2p_chunk, pages);

	queue_work(p2p_vram(chunk->xe)->p2p_wq, &chunk->release_work);
}

static void p2p_chunk_put_pages(struct xe_p2p_chunk *chunk)
{
	lockdep_assert_held_write(&chunk->lock);

	if (!chunk->addr)
		return;

	pci_p2pmem_put_pages(chunk->addr, chunk->size);
	chunk->addr = NULL;
}

/**
 * xe_p2p_carveout_revoke() - Take back every allocated carve-out chunk
 * @xe: the &xe_device
 *
 * Drop every chunk's own page references and zap its userspace mappings, so
 * that later accesses fault. Called once the device is unplugged, so unbind
 * doesn't wait on chunk fds that userspace still holds, and during system
 * suspend while the GuC can still process the resulting userptr
 * invalidations, since VRAM contents don't survive suspend. Each chunk's
 * memory, runtime PM reference and cgroup charge are released once in-flight
 * I/O has dropped its pins too.
 */
void xe_p2p_carveout_revoke(struct xe_device *xe)
{
	struct xe_vram_region *vram;
	struct xe_p2p_chunk *chunk;

	if (!xe_p2p_carveout_available(xe))
		return;

	vram = p2p_vram(xe);
	mutex_lock(&vram->p2p_lock);
	list_for_each_entry(chunk, &vram->p2p_chunks, link) {
		down_write(&chunk->lock);
		p2p_chunk_put_pages(chunk);
		up_write(&chunk->lock);
		unmap_mapping_range(chunk->mapping, 0, 0, 1);
	}
	mutex_unlock(&vram->p2p_lock);
}

static struct page *p2p_chunk_page(struct xe_p2p_chunk *chunk,
				   struct vm_area_struct *vma,
				   unsigned long addr)
{
	pgoff_t pgoff = vma->vm_pgoff + ((addr - vma->vm_start) >> PAGE_SHIFT);

	return virt_to_page(chunk->addr + (pgoff << PAGE_SHIFT));
}

static void p2p_chunk_insert_range(struct xe_p2p_chunk *chunk,
				   struct vm_area_struct *vma,
				   unsigned long start, unsigned long end)
{
	struct page *pages[P2P_FAULT_BATCH];

	while (start < end) {
		unsigned long nr = min_t(unsigned long, P2P_FAULT_BATCH,
					 (end - start) >> PAGE_SHIFT);
		unsigned long left = nr;
		unsigned long i;

		for (i = 0; i < nr; i++)
			pages[i] = p2p_chunk_page(chunk, vma, start + i * PAGE_SIZE);

		if (vm_insert_pages(vma, start, pages, &left))
			return;
		start += nr * PAGE_SIZE;
	}
}

static vm_fault_t p2p_chunk_fault(struct vm_fault *vmf)
{
	struct vm_area_struct *vma = vmf->vma;
	struct xe_p2p_chunk *chunk = vma->vm_file->private_data;
	unsigned long window = ALIGN_DOWN(vmf->address, PMD_SIZE);
	unsigned long start = max(window, vma->vm_start);
	unsigned long end = min(window + PMD_SIZE, vma->vm_end);
	vm_fault_t ret = VM_FAULT_SIGBUS;
	int idx;

	if (!drm_dev_enter(&chunk->xe->drm, &idx))
		return VM_FAULT_SIGBUS;

	down_read(&chunk->lock);
	if (!chunk->addr)
		goto out;

	ret = vmf_insert_page_mkwrite(vmf, p2p_chunk_page(chunk, vma, vmf->address),
				      false);
	if (ret != VM_FAULT_NOPAGE)
		goto out;

	p2p_chunk_insert_range(chunk, vma, start, vmf->address);
	p2p_chunk_insert_range(chunk, vma, vmf->address + PAGE_SIZE, end);
out:
	up_read(&chunk->lock);
	drm_dev_exit(idx);
	return ret;
}

static const struct vm_operations_struct p2p_chunk_vm_ops = {
	.fault = p2p_chunk_fault,
};

static int p2p_chunk_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct xe_p2p_chunk *chunk = file->private_data;

	if (!(vma->vm_flags & VM_MAYSHARE))
		return -EINVAL;

	if (vma->vm_pgoff + vma_pages(vma) > chunk->size >> PAGE_SHIFT)
		return -EINVAL;

	vm_flags_set(vma, VM_MIXEDMAP | VM_DONTEXPAND | VM_DONTDUMP);
	vma->vm_page_prot = pgprot_writecombine(vm_get_page_prot(vma->vm_flags));
	vma->vm_ops = &p2p_chunk_vm_ops;
	return 0;
}

static int p2p_chunk_release(struct inode *inode, struct file *file)
{
	struct xe_p2p_chunk *chunk = file->private_data;
	struct xe_vram_region *vram = p2p_vram(chunk->xe);

	mutex_lock(&vram->p2p_lock);
	down_write(&chunk->lock);
	p2p_chunk_put_pages(chunk);
	up_write(&chunk->lock);
	list_del_init(&chunk->link);
	mutex_unlock(&vram->p2p_lock);

	p2p_chunk_put(chunk);
	return 0;
}

static const struct file_operations p2p_chunk_fops = {
	.owner = THIS_MODULE,
	.mmap = p2p_chunk_mmap,
	.get_unmapped_area = thp_get_unmapped_area,
	.release = p2p_chunk_release,
};

static int p2p_chunk_clear(struct xe_vram_region *vram,
			   struct xe_p2p_chunk *chunk)
{
	phys_addr_t phys = page_to_phys(virt_to_page(chunk->addr));
	size_t off;
	int idx;

	for (off = 0; off < chunk->size; off += SZ_2M) {
		if (fatal_signal_pending(current))
			return -EINTR;

		if (!drm_dev_enter(&chunk->xe->drm, &idx))
			return -ENODEV;

		memset_io(vram->mapping + (phys - vram->io_start) + off, 0,
			  min_t(size_t, chunk->size - off, SZ_2M));
		drm_dev_exit(idx);
		cond_resched();
	}

	return 0;
}

/**
 * xe_p2p_alloc_ioctl() - Allocate a chunk of the P2P carve-out
 * @dev: the DRM device
 * @data: the &struct drm_xe_p2p_alloc arguments
 * @file: the DRM file of the caller
 *
 * Allocate a zeroed chunk of the carve-out, charge it to the caller's dmem
 * cgroup and return a new fd for it. Mapping the fd gives write-combined,
 * page-backed access that is filled in on fault, so the mapping works as an
 * O_DIRECT buffer and as a userptr. The chunk's memory goes back to the pool
 * once the fd and every mapping of it are gone and in-flight I/O has dropped
 * its pins. Until then the chunk keeps its cgroup charge and keeps the device
 * out of runtime suspend.
 *
 * Return: 0 on success, negative error code on failure.
 */
int xe_p2p_alloc_ioctl(struct drm_device *dev, void *data,
		       struct drm_file *file)
{
	struct xe_device *xe = to_xe_device(dev);
	struct drm_xe_p2p_alloc *args = data;
	struct xe_vram_region *vram;
	struct xe_p2p_chunk *chunk;
	struct file *filp;
	int fd, idx, err;

	if (XE_IOCTL_DBG(xe, args->extensions || args->flags ||
			 args->reserved[0] || args->reserved[1]))
		return -EINVAL;

	if (XE_IOCTL_DBG(xe, !args->size || !PAGE_ALIGNED(args->size)))
		return -EINVAL;

	if (!xe_p2p_carveout_available(xe))
		return -ENODEV;

	vram = p2p_vram(xe);
	if (args->size > vram->p2p_carveout_size)
		return -ENOMEM;

	chunk = kzalloc_obj(*chunk, GFP_KERNEL_ACCOUNT);
	if (!chunk)
		return -ENOMEM;
	chunk->pages.release = p2p_chunk_pages_released;
	INIT_WORK(&chunk->release_work, p2p_chunk_release_work);
	refcount_set(&chunk->refs, 1);
	chunk->xe = xe;
	INIT_LIST_HEAD(&chunk->link);
	init_rwsem(&chunk->lock);
	chunk->size = args->size;

	if (vram->p2p_carveout_cg &&
	    dmem_cgroup_try_charge(vram->p2p_carveout_cg, chunk->size,
				   &chunk->pool, NULL)) {
		kfree(chunk);
		return -ENOMEM;
	}

	drm_dev_get(dev);
	xe_pm_runtime_get(xe);
	chunk->addr = pci_p2pmem_alloc_pages(to_pci_dev(dev->dev), chunk->size,
					     min_t(size_t, SZ_2M,
						   rounddown_pow_of_two(chunk->size)),
					     &chunk->pages);
	if (IS_ERR(chunk->addr)) {
		err = PTR_ERR(chunk->addr);
		xe_pm_runtime_put(xe);
		dmem_cgroup_uncharge(chunk->pool, chunk->size);
		p2p_chunk_put(chunk);
		return err;
	}
	refcount_inc(&chunk->refs);

	err = p2p_chunk_clear(vram, chunk);
	if (err)
		goto err_pages;

	fd = get_unused_fd_flags(O_CLOEXEC);
	if (fd < 0) {
		err = fd;
		goto err_pages;
	}

	filp = anon_inode_create_getfile("xe_p2p", &p2p_chunk_fops, chunk,
					 O_RDWR, NULL);
	if (IS_ERR(filp)) {
		err = PTR_ERR(filp);
		put_unused_fd(fd);
		goto err_pages;
	}
	chunk->mapping = filp->f_mapping;

	if (!drm_dev_enter(dev, &idx)) {
		put_unused_fd(fd);
		fput(filp);
		return -ENODEV;
	}
	mutex_lock(&vram->p2p_lock);
	list_add(&chunk->link, &vram->p2p_chunks);
	mutex_unlock(&vram->p2p_lock);
	drm_dev_exit(idx);

	fd_install(fd, filp);
	args->fd = fd;
	return 0;

err_pages:
	pci_p2pmem_put_pages(chunk->addr, chunk->size);
	p2p_chunk_put(chunk);
	return err;
}

/**
 * xe_p2p_carveout_map_page() - Translate a PCI P2PDMA page for GPU access
 * @drm: the DRM device that accesses the page
 * @page: the P2PDMA page
 * @order: the order of @page
 * @dir: the DMA direction
 * @addr: returns the encoded address
 *
 * Pages of this device's carve-out are mapped by VRAM device physical
 * address. Pages of another device are mapped by bus address when the PCI
 * topology lets the two devices reach each other without the host bridge.
 * Any other route needs a DMA mapping, which drm_gpusvm never unmaps, so it is
 * refused.
 *
 * Return: 0 on success, -EOPNOTSUPP when the page can't be mapped.
 */
int xe_p2p_carveout_map_page(struct drm_device *drm, struct page *page,
			     unsigned int order, enum dma_data_direction dir,
			     struct drm_pagemap_addr *addr)
{
	struct pci_p2pdma_map_state state = {};
	phys_addr_t phys = page_to_phys(page);
	enum pci_p2pdma_map_type type;

	type = pci_p2pdma_state(&state, drm->dev, page);
	if (state.mem && state.mem->owner == drm->dev) {
		struct xe_vram_region *vram = p2p_vram(to_xe_device(drm));

		*addr = drm_pagemap_addr_encode(vram->dpa_base + phys -
						vram->io_start,
						XE_INTERCONNECT_VRAM, order, dir);
		return 0;
	}

	if (type != PCI_P2PDMA_MAP_BUS_ADDR)
		return -EOPNOTSUPP;

	*addr = drm_pagemap_addr_encode(pci_p2pdma_bus_addr_map(state.mem, phys),
					XE_INTERCONNECT_P2P, order, dir);
	return 0;
}
