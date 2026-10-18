/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2025 Intel Corporation
 */

#ifndef _XE_VRAM_TYPES_H_
#define _XE_VRAM_TYPES_H_

#if IS_ENABLED(CONFIG_DRM_XE_PAGEMAP)
#include <drm/drm_pagemap.h>
#endif

#include "xe_ttm_vram_mgr_types.h"

struct dmem_cgroup_region;
struct p2pdma_provider;
struct workqueue_struct;
struct xe_device;
struct xe_migrate;

/**
 * struct xe_vram_region - memory region structure
 * This is used to describe a memory region in xe
 * device, such as HBM memory or CXL extension memory.
 */
struct xe_vram_region {
	/** @xe: Back pointer to xe device */
	struct xe_device *xe;
	/**
	 * @id: VRAM region instance id
	 *
	 * The value should be unique for VRAM region.
	 */
	u8 id;
	/** @io_start: IO start address of this VRAM instance */
	resource_size_t io_start;
	/**
	 * @io_size: IO size of this VRAM instance
	 *
	 * This represents how much of this VRAM we can access
	 * via the CPU through the VRAM BAR. This can be smaller
	 * than @usable_size, in which case only part of VRAM is CPU
	 * accessible (typically the first 256M). This
	 * configuration is known as small-bar.
	 */
	resource_size_t io_size;
	/** @dpa_base: This memory regions's DPA (device physical address) base */
	resource_size_t dpa_base;
	/**
	 * @usable_size: usable size of VRAM
	 *
	 * Usable size of VRAM excluding reserved portions
	 * (e.g stolen mem)
	 */
	resource_size_t usable_size;
	/**
	 * @actual_physical_size: Actual VRAM size
	 *
	 * Actual VRAM size including reserved portions
	 * (e.g stolen mem)
	 */
	resource_size_t actual_physical_size;
	/** @mapping: pointer to VRAM mappable space */
	void __iomem *mapping;
	/** @ttm: VRAM TTM manager */
	struct xe_ttm_vram_mgr ttm;
	/** @placement: TTM placement dedicated for this region */
	u32 placement;
	/**
	 * @p2p_provider: PCI P2PDMA provider for the BAR backing this region
	 *
	 * NULL when the BAR cannot be used for peer-to-peer DMA, in which
	 * case VRAM is never exported to dma-buf importers directly.
	 */
	struct p2pdma_provider *p2p_provider;
	/**
	 * @p2p_carveout_start: Offset of the PCI P2PDMA carve-out within this
	 * region
	 *
	 * The carve-out is VRAM at the top of the region that TTM never
	 * manages. It has ZONE_DEVICE pages, so userspace can allocate chunks
	 * of it with DRM_IOCTL_XE_P2P_ALLOC, use them as O_DIRECT targets and
	 * bind the same mappings to the GPU as userptrs.
	 */
	resource_size_t p2p_carveout_start;
	/** @p2p_carveout_size: Size of the carve-out, 0 when there is none */
	resource_size_t p2p_carveout_size;
	/** @p2p_carveout_cg: dmem cgroup region carve-out chunks are charged to */
	struct dmem_cgroup_region *p2p_carveout_cg;
	/** @p2p_chunks: Carve-out chunks allocated through the render node */
	struct list_head p2p_chunks;
	/**
	 * @p2p_lock: Protects @p2p_chunks and @p2p_suspending, and orders
	 * increments of @p2p_live against the suspend check
	 */
	struct mutex p2p_lock;
	/**
	 * @p2p_live: Chunks whose memory hasn't fully gone back to the pool.
	 * System suspend is refused while it is non-zero.
	 */
	atomic_t p2p_live;
	/** @p2p_suspending: Set while a system suspend is in progress */
	bool p2p_suspending;
	/** @p2p_wq: Releases chunks once all their memory is back in the pool */
	struct workqueue_struct *p2p_wq;
#if IS_ENABLED(CONFIG_DRM_XE_PAGEMAP)
	/** @migrate: Back pointer to migrate */
	struct xe_migrate *migrate;
	/** @dpagemap_cache: drm_pagemap cache. */
	struct drm_pagemap_cache *dpagemap_cache;
#endif
};

#endif
