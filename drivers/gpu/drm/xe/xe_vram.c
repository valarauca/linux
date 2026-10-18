// SPDX-License-Identifier: MIT
/*
 * Copyright © 2021-2024 Intel Corporation
 */

#include <kunit/visibility.h>
#include <linux/cgroup_dmem.h>
#include <linux/pci.h>
#include <linux/pci-p2pdma.h>

#include <drm/drm_drv.h>
#include <drm/drm_managed.h>
#include <drm/drm_pagemap.h>
#include <drm/drm_print.h>
#include <drm/drm_vma_manager.h>

#include "regs/xe_bars.h"
#include "regs/xe_gt_regs.h"
#include "regs/xe_regs.h"
#include "xe_assert.h"
#include "xe_bo.h"
#include "xe_device.h"
#include "xe_force_wake.h"
#include "xe_gt_mcr.h"
#include "xe_mmio.h"
#include "xe_module.h"
#include "xe_pm.h"
#include "xe_sriov.h"
#include "xe_svm.h"
#include "xe_tile_sriov_vf.h"
#include "xe_ttm_vram_mgr.h"
#include "xe_vram.h"
#include "xe_vram_types.h"

static bool resource_is_valid(struct pci_dev *pdev, int bar)
{
	if (!pci_resource_flags(pdev, bar))
		return false;

	if (pci_resource_flags(pdev, bar) & IORESOURCE_UNSET)
		return false;

	if (!pci_resource_len(pdev, bar))
		return false;

	return true;
}

static int determine_lmem_bar_p2p(struct xe_device *xe, struct xe_vram_region *lmem_bar)
{
	struct pci_dev *pdev = to_pci_dev(xe->drm.dev);
	int err;

	lmem_bar->p2p_provider = NULL;

	err = pcim_p2pdma_init(pdev);
	if (err == -EOPNOTSUPP) {
		drm_info(&xe->drm, "VRAM BAR is not peer-to-peer capable\n");
		return 0;
	}
	if (err)
		return err;

	lmem_bar->p2p_provider = pcim_p2pdma_provider(pdev, LMEM_BAR);
	return 0;
}

static int determine_lmem_bar_size(struct xe_device *xe, struct xe_vram_region *lmem_bar)
{
	struct pci_dev *pdev = to_pci_dev(xe->drm.dev);
	int err;

	if (!resource_is_valid(pdev, LMEM_BAR)) {
		drm_err(&xe->drm, "pci resource is not valid\n");
		return -ENXIO;
	}

	lmem_bar->io_start = pci_resource_start(pdev, LMEM_BAR);
	lmem_bar->io_size = pci_resource_len(pdev, LMEM_BAR);
	if (!lmem_bar->io_size)
		return -EIO;

	err = determine_lmem_bar_p2p(xe, lmem_bar);
	if (err)
		return err;

	/* XXX: Need to change when xe link code is ready */
	lmem_bar->dpa_base = 0;

	/* set up a map to the total memory area. */
	lmem_bar->mapping = devm_ioremap_wc(&pdev->dev, lmem_bar->io_start, lmem_bar->io_size);

	return 0;
}

static int get_flat_ccs_offset(struct xe_gt *gt, u64 tile_size, u64 *poffset)
{
	struct xe_device *xe = gt_to_xe(gt);
	u64 offset;
	u32 reg;

	CLASS(xe_force_wake, fw_ref)(gt_to_fw(gt), XE_FW_GT);
	if (!fw_ref.domains)
		return -ETIMEDOUT;

	if (GRAPHICS_VER(xe) >= 20) {
		u64 ccs_size = tile_size / 512;
		u64 offset_hi, offset_lo;
		u32 nodes, num_enabled;

		reg = xe_mmio_read32(&gt->mmio, MIRROR_FUSE3);
		nodes = REG_FIELD_GET(XE2_NODE_ENABLE_MASK, reg);
		num_enabled = hweight32(nodes); /* Number of enabled l3 nodes */

		reg = xe_gt_mcr_unicast_read_any(gt, XE2_FLAT_CCS_BASE_RANGE_LOWER);
		offset_lo = REG_FIELD_GET(XE2_FLAT_CCS_BASE_LOWER_ADDR_MASK, reg);

		reg = xe_gt_mcr_unicast_read_any(gt, XE2_FLAT_CCS_BASE_RANGE_UPPER);
		offset_hi = REG_FIELD_GET(XE2_FLAT_CCS_BASE_UPPER_ADDR_MASK, reg);

		offset = offset_hi << 32; /* HW view bits 39:32 */
		offset |= offset_lo << 6; /* HW view bits 31:6 */
		offset *= num_enabled; /* convert to SW view */

		drm_info(&xe->drm, "FLAT_CCS base:%llx, aligned:%s\n", offset,
			 str_yes_no(IS_ALIGNED(offset, SZ_128K)));

		/*
		 * Everything below this offset is handed to the VRAM
		 * allocator, so it has to be the *first* address the
		 * compression hardware owns, rounded down.  Rounding it up
		 * publishes CCS storage as free memory.
		 */
		offset = round_down(offset, SZ_4K);

		/*
		 * CCS storage must not run into GSM.  The old check compared
		 * the offset against GSMBASE - ccs_size for equality, which
		 * could not fail: that value is 128K aligned, so it agreed
		 * with the rounded-up offset even when the base was not 128K
		 * aligned - exactly the case this fixes.
		 */
		xe_assert_msg(xe, offset + ccs_size <=
			      xe_mmio_read64_2x32(&gt_to_tile(gt)->mmio, GSMBASE),
			      "CCS overlaps GSM.\n");
	} else {
		reg = xe_gt_mcr_unicast_read_any(gt, XEHP_FLAT_CCS_BASE_ADDR);
		offset = (u64)REG_FIELD_GET(XEHP_FLAT_CCS_PTR, reg) * SZ_64K;
	}

	*poffset = offset;

	return 0;
}

/*
 * tile_vram_size() - Collect vram size and offset information
 * @tile: tile to get info for
 * @vram_size: available vram (size - device reserved portions)
 * @tile_size: actual vram size
 * @tile_offset: physical start point in the vram address space
 *
 * There are 4 places for size information:
 * - io size (from pci_resource_len of LMEM bar) (only used for small bar and DG1)
 * - TILEx size (actual vram size)
 * - GSMBASE offset (TILEx - "stolen")
 * - CSSBASE offset (TILEx - CSS space necessary)
 *
 * CSSBASE is always a lower/smaller offset then GSMBASE.
 *
 * The actual available size of memory is to the CCS or GSM base.
 * NOTE: multi-tile bases will include the tile offset.
 *
 */
static int tile_vram_size(struct xe_tile *tile, u64 *vram_size,
			  u64 *tile_size, u64 *tile_offset)
{
	struct xe_device *xe = tile_to_xe(tile);
	struct xe_gt *gt = tile->primary_gt;
	u64 offset;
	u32 reg;

	if (IS_SRIOV_VF(xe)) {
		struct xe_tile *t;
		int id;

		offset = 0;
		for_each_tile(t, xe, id)
			for_each_if(t->id < tile->id)
				offset += xe_tile_sriov_vf_lmem(t);

		*tile_size = xe_tile_sriov_vf_lmem(tile);
		*vram_size = *tile_size;
		*tile_offset = offset;

		return 0;
	}

	/* actual size */
	if (unlikely(xe->info.platform == XE_DG1)) {
		*tile_size = pci_resource_len(to_pci_dev(xe->drm.dev), LMEM_BAR);
		*tile_offset = 0;
	} else {
		reg = xe_mmio_read32(&tile->mmio, SG_TILE_ADDR_RANGE(tile->id));
		*tile_size = (u64)REG_FIELD_GET(GENMASK(17, 8), reg) * SZ_1G;
		*tile_offset = (u64)REG_FIELD_GET(GENMASK(7, 1), reg) * SZ_1G;
	}

	/* minus device usage */
	if (xe->info.has_flat_ccs) {
		int ret = get_flat_ccs_offset(gt, *tile_size, &offset);

		if (ret)
			return ret;
	} else {
		offset = xe_mmio_read64_2x32(&tile->mmio, GSMBASE);
	}

	/* remove the tile offset so we have just the available size */
	*vram_size = offset - *tile_offset;

	return 0;
}

static void vram_fini(void *arg)
{
	struct xe_device *xe = arg;
	struct xe_tile *tile;
	int id;

	xe->mem.vram->mapping = NULL;

	for_each_tile(tile, xe, id) {
		tile->mem.vram->mapping = NULL;
		if (tile->mem.kernel_vram)
			tile->mem.kernel_vram->mapping = NULL;
	}
}

struct xe_vram_region *xe_vram_region_alloc(struct xe_device *xe, u8 id, u32 placement)
{
	struct xe_vram_region *vram;
	struct drm_device *drm = &xe->drm;

	xe_assert(xe, id < xe->info.tile_count);

	vram = drmm_kzalloc(drm, sizeof(*vram), GFP_KERNEL);
	if (!vram)
		return NULL;

	vram->xe = xe;
	vram->id = id;
	vram->placement = placement;
#if defined(CONFIG_DRM_XE_PAGEMAP)
	vram->migrate = xe->tiles[id].migrate;
#endif
	return vram;
}

static void print_vram_region_info(struct xe_device *xe, struct xe_vram_region *vram)
{
	struct drm_device *drm = &xe->drm;

	if (vram->io_size < vram->usable_size)
		drm_info(drm, "Small BAR device\n");

	drm_info(drm,
		 "VRAM[%u]: Actual physical size %pa, usable size exclude stolen %pa, CPU accessible size %pa\n",
		 vram->id, &vram->actual_physical_size, &vram->usable_size, &vram->io_size);
	drm_info(drm, "VRAM[%u]: DPA range: [%pa-%llx], io range: [%pa-%llx]\n",
		 vram->id, &vram->dpa_base, vram->dpa_base + (u64)vram->actual_physical_size,
		 &vram->io_start, vram->io_start + (u64)vram->io_size);
}

static int vram_region_init(struct xe_device *xe, struct xe_vram_region *vram,
			    struct xe_vram_region *lmem_bar, u64 offset, u64 usable_size,
			    u64 region_size, resource_size_t remain_io_size)
{
	/* Check if VRAM region is already initialized */
	if (vram->mapping)
		return 0;

	vram->actual_physical_size = region_size;
	vram->io_start = lmem_bar->io_start + offset;
	vram->io_size = min_t(u64, usable_size, remain_io_size);

	if (!vram->io_size) {
		drm_err(&xe->drm, "Tile without any CPU visible VRAM. Aborting.\n");
		return -ENODEV;
	}

	vram->dpa_base = lmem_bar->dpa_base + offset;
	vram->mapping = lmem_bar->mapping + offset;
	vram->usable_size = usable_size;
	vram->p2p_provider = lmem_bar->p2p_provider;

	print_vram_region_info(xe, vram);

	return 0;
}

/**
 * xe_map_resource_to_region - Map ttm resource to vram memory region
 * @res: The ttm resource
 *
 * Get vram memory region using vram memory manager managing this resource
 *
 * Returns: pointer to xe_vram_region
 */
struct xe_vram_region *xe_map_resource_to_region(struct ttm_resource *res)
{
	struct xe_device *xe = ttm_to_xe_device(res->bo->bdev);
	struct ttm_resource_manager *mgr;
	struct xe_ttm_vram_mgr *vram_mgr;

	xe_assert(xe, mem_type_is_vram(res->mem_type));
	mgr = ttm_manager_type(&xe->ttm, res->mem_type);
	vram_mgr = to_xe_ttm_vram_mgr(mgr);

	return container_of(vram_mgr, struct xe_vram_region, ttm);
}

/**
 * xe_vram_probe() - Probe VRAM configuration
 * @xe: the &xe_device
 *
 * Collect VRAM size and offset information for all tiles.
 *
 * Return: 0 on success, error code on failure
 */
int xe_vram_probe(struct xe_device *xe)
{
	struct xe_tile *tile;
	struct xe_vram_region lmem_bar;
	resource_size_t remain_io_size;
	u64 available_size = 0;
	u64 total_size = 0;
	int err;
	u8 id;

	if (!IS_DGFX(xe))
		return 0;

	err = determine_lmem_bar_size(xe, &lmem_bar);
	if (err)
		return err;
	drm_info(&xe->drm, "VISIBLE VRAM: %pa, %pa\n", &lmem_bar.io_start, &lmem_bar.io_size);

	remain_io_size = lmem_bar.io_size;

	for_each_tile(tile, xe, id) {
		u64 region_size;
		u64 usable_size;
		u64 tile_offset;

		err = tile_vram_size(tile, &usable_size, &region_size, &tile_offset);
		if (err)
			return err;

		total_size += region_size;
		available_size += usable_size;

		err = vram_region_init(xe, tile->mem.vram, &lmem_bar, tile_offset, usable_size,
				       region_size, remain_io_size);
		if (err)
			return err;

		if (total_size > lmem_bar.io_size) {
			drm_info(&xe->drm, "VRAM: %pa is larger than resource %pa\n",
				 &total_size, &lmem_bar.io_size);
		}

		remain_io_size -= min_t(u64, tile->mem.vram->actual_physical_size, remain_io_size);
	}

	err = vram_region_init(xe, xe->mem.vram, &lmem_bar, 0, available_size, total_size,
			       lmem_bar.io_size);
	if (err)
		return err;

	return devm_add_action_or_reset(xe->drm.dev, vram_fini, xe);
}

struct xe_p2p_chunk {
	struct dmem_cgroup_pool_state *pool;
	size_t size;
	refcount_t refs;
};

static void p2p_carveout_pm_put(void *arg)
{
	xe_pm_runtime_put(arg);
}

/**
 * xe_vram_init_p2p_carveout() - Hand the top of VRAM to PCI P2PDMA
 * @tile: the tile being initialized
 *
 * Must run before the tile's TTM VRAM manager is initialized. On the root
 * tile, take xe_modparam.p2p_carveout_mb of VRAM, rounded up to 2MiB, off the
 * top of VRAM so that TTM never manages it, and register the matching part of
 * the LMEM BAR with pci_p2pdma_add_resource(). The range gets ZONE_DEVICE
 * pages, which userspace allocates through xe_vram_p2p_carveout_mmap().
 *
 * The carve-out needs a BAR covering all of VRAM and isn't offered on
 * platforms that need 64K VRAM pages. It keeps the device out of runtime
 * suspend for as long as it exists, because the memory it hands out can be a
 * DMA target at any time.
 *
 * Failing to set up the carve-out is not fatal. The device then runs without
 * one and TTM manages all of VRAM.
 *
 * Return: 0 on success, negative error code on failure.
 */
int xe_vram_init_p2p_carveout(struct xe_tile *tile)
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

	xe_pm_runtime_get_noresume(xe);
	err = devm_add_action_or_reset(xe->drm.dev, p2p_carveout_pm_put, xe);
	if (err)
		return err;

	err = pci_p2pdma_add_resource(pdev, LMEM_BAR, size,
				      vram->io_start + start -
				      pci_resource_start(pdev, LMEM_BAR));
	if (err) {
		drm_warn(&xe->drm, "failed to register P2P carve-out: %pe\n",
			 ERR_PTR(err));
		devm_release_action(xe->drm.dev, p2p_carveout_pm_put, xe);
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
 * xe_vram_has_p2p_carveout() - Check whether the device has a P2P carve-out
 * @xe: the &xe_device
 *
 * Return: true if xe_vram_init_p2p_carveout() set up a carve-out
 */
bool xe_vram_has_p2p_carveout(struct xe_device *xe)
{
	struct xe_vram_region *vram = xe_device_get_root_tile(xe)->mem.vram;

	return vram && vram->p2p_carveout_size;
}

static void p2p_chunk_open(struct vm_area_struct *vma)
{
	struct xe_p2p_chunk *chunk = vma->vm_private_data;

	refcount_inc(&chunk->refs);
}

static void p2p_chunk_close(struct vm_area_struct *vma)
{
	struct xe_p2p_chunk *chunk = vma->vm_private_data;

	if (refcount_dec_and_test(&chunk->refs)) {
		dmem_cgroup_uncharge(chunk->pool, chunk->size);
		kfree(chunk);
	}
}

static const struct vm_operations_struct p2p_chunk_vm_ops = {
	.open = p2p_chunk_open,
	.close = p2p_chunk_close,
};

static void p2p_chunk_clear(void *data, phys_addr_t addr, size_t len)
{
	struct xe_vram_region *vram = data;
	void __iomem *va = vram->mapping + (addr - vram->io_start);
	size_t off;

	for (off = 0; off < len; off += SZ_2M) {
		memset_io(va + off, 0, min_t(size_t, len - off, SZ_2M));
		cond_resched();
	}
}

/**
 * xe_vram_p2p_carveout_mmap() - Map a new chunk of the P2P carve-out
 * @xe: the &xe_device
 * @vma: the shared userspace mapping to fill
 *
 * Allocate a zeroed chunk of the P2P carve-out the size of @vma, charge it to
 * the caller's dmem cgroup and map its pages write-combined, so the mapping
 * works as an O_DIRECT buffer and as a userptr. Access is governed by the DRM
 * file the mapping was made through. The chunk is freed once every mapping of
 * it is gone and no I/O holds its pages. Unplugging the device zaps the
 * mapping along with every other mapping of the DRM file.
 *
 * A mapping must not reach the GEM fake offset range, so each chunk is limited
 * to a little under 4GiB on 64-bit kernels.
 *
 * Return: 0 on success, negative error code on failure.
 */
int xe_vram_p2p_carveout_mmap(struct xe_device *xe, struct vm_area_struct *vma)
{
	struct xe_vram_region *vram = xe_device_get_root_tile(xe)->mem.vram;
	struct xe_p2p_chunk *chunk;
	int idx, err;

	if (!xe_vram_has_p2p_carveout(xe))
		return -ENODEV;

	if (vma->vm_pgoff + vma_pages(vma) > DRM_FILE_PAGE_OFFSET_START)
		return -EINVAL;

	chunk = kzalloc_obj(*chunk, GFP_KERNEL_ACCOUNT);
	if (!chunk)
		return -ENOMEM;
	chunk->size = vma->vm_end - vma->vm_start;
	refcount_set(&chunk->refs, 1);

	if (vram->p2p_carveout_cg &&
	    dmem_cgroup_try_charge(vram->p2p_carveout_cg, chunk->size,
				   &chunk->pool, NULL)) {
		err = -ENOMEM;
		goto err_free;
	}

	if (!drm_dev_enter(&xe->drm, &idx)) {
		err = -ENODEV;
		goto err_uncharge;
	}

	vm_flags_set(vma, VM_DONTEXPAND | VM_DONTDUMP);
	vma->vm_page_prot = pgprot_writecombine(vm_get_page_prot(vma->vm_flags));
	err = pci_p2pmem_alloc_mmap(to_pci_dev(xe->drm.dev), vma,
				    p2p_chunk_clear, vram);
	drm_dev_exit(idx);
	if (err)
		goto err_uncharge;

	vma->vm_private_data = chunk;
	vma->vm_ops = &p2p_chunk_vm_ops;
	return 0;

err_uncharge:
	dmem_cgroup_uncharge(chunk->pool, chunk->size);
err_free:
	kfree(chunk);
	return err;
}

/**
 * xe_vram_p2p_carveout_suspend() - Invalidate userspace carve-out mappings
 * @xe: the &xe_device
 *
 * VRAM loses its contents across system suspend and the carve-out is not
 * backed up. Zap every userspace mapping of it, so that later CPU access
 * faults and userptr binds of it fail instead of silently reading garbage
 * after resume.
 */
void xe_vram_p2p_carveout_suspend(struct xe_device *xe)
{
	if (!xe_vram_has_p2p_carveout(xe))
		return;

	unmap_mapping_range(xe->drm.anon_inode->i_mapping,
			    XE_P2P_CARVEOUT_MMAP_OFFSET,
			    ((loff_t)DRM_FILE_PAGE_OFFSET_START << PAGE_SHIFT) -
			    XE_P2P_CARVEOUT_MMAP_OFFSET, 1);
}

/**
 * xe_vram_p2pdma_map() - Translate a PCI P2PDMA page for GPU access
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
int xe_vram_p2pdma_map(struct drm_device *drm, struct page *page,
		       unsigned int order, enum dma_data_direction dir,
		       struct drm_pagemap_addr *addr)
{
	struct pci_p2pdma_map_state state = {};
	phys_addr_t phys = page_to_phys(page);
	enum pci_p2pdma_map_type type;

	type = pci_p2pdma_state(&state, drm->dev, page);
	if (state.mem && state.mem->owner == drm->dev) {
		struct xe_vram_region *vram =
			xe_device_get_root_tile(to_xe_device(drm))->mem.vram;

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

/**
 * xe_vram_region_io_start - Get the IO start of a VRAM region
 * @vram: the VRAM region
 *
 * Return: the IO start of the VRAM region, or 0 if not valid
 */
resource_size_t xe_vram_region_io_start(const struct xe_vram_region *vram)
{
	return vram ? vram->io_start : 0;
}

/**
 * xe_vram_region_io_size - Get the IO size of a VRAM region
 * @vram: the VRAM region
 *
 * Return: the IO size of the VRAM region, or 0 if not valid
 */
resource_size_t xe_vram_region_io_size(const struct xe_vram_region *vram)
{
	return vram ? vram->io_size : 0;
}

/**
 * xe_vram_region_dpa_base - Get the DPA base of a VRAM region
 * @vram: the VRAM region
 *
 * Return: the DPA base of the VRAM region, or 0 if not valid
 */
resource_size_t xe_vram_region_dpa_base(const struct xe_vram_region *vram)
{
	return vram ? vram->dpa_base : 0;
}

/**
 * xe_vram_region_usable_size - Get the usable size of a VRAM region
 * @vram: the VRAM region
 *
 * Return: the usable size of the VRAM region, or 0 if not valid
 */
resource_size_t xe_vram_region_usable_size(const struct xe_vram_region *vram)
{
	return vram ? vram->usable_size : 0;
}

/**
 * xe_vram_region_actual_physical_size - Get the actual physical size of a VRAM region
 * @vram: the VRAM region
 *
 * Return: the actual physical size of the VRAM region, or 0 if not valid
 */
resource_size_t xe_vram_region_actual_physical_size(const struct xe_vram_region *vram)
{
	return vram ? vram->actual_physical_size : 0;
}
EXPORT_SYMBOL_IF_KUNIT(xe_vram_region_actual_physical_size);
