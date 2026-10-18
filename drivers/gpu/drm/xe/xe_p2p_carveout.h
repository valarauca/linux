/* SPDX-License-Identifier: MIT */

#ifndef _XE_P2P_CARVEOUT_H_
#define _XE_P2P_CARVEOUT_H_

#include <linux/types.h>

enum dma_data_direction;
struct drm_device;
struct drm_file;
struct drm_pagemap_addr;
struct page;
struct xe_device;
struct xe_tile;

int xe_p2p_carveout_init(struct xe_tile *tile);
bool xe_p2p_carveout_available(struct xe_device *xe);
void xe_p2p_carveout_revoke(struct xe_device *xe);
int xe_p2p_carveout_map_page(struct drm_device *drm, struct page *page,
			     unsigned int order, enum dma_data_direction dir,
			     struct drm_pagemap_addr *addr);
int xe_p2p_alloc_ioctl(struct drm_device *dev, void *data,
		       struct drm_file *file);

#endif
