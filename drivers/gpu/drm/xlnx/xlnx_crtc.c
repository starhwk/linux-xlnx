// SPDX-License-Identifier: GPL-2.0
/*
 * Xilinx DRM crtc driver
 *
 *  Copyright (C) 2017 - 2018 Xilinx, Inc.
 *
 *  Author: Hyun Woo Kwon <hyun.kwon@xilinx.com>
 */

#include <drm/drmP.h>

#include "xlnx_crtc.h"
#include "xlnx_drv.h"

/*
 * Overview
 * --------
 *
 * The Xilinx CRTC layer is to enable the custom interface to CRTC drivers.
 * The interface is used by Xilinx DRM driver where it needs CRTC
 * functionality. CRTC drivers should attach the desired callbacks
 * to struct xlnx_crtc and register the xlnx_crtc with corresponding
 * drm_device. It's highly recommended CRTC drivers register all callbacks
 * even though many of them are optional.
 * The CRTC helper simply walks through the registered CRTC device,
 * and calls the callbacks.
 */

unsigned int xlnx_crtc_helper_get_align(struct xlnx_crtc_helper *helper)
{
	struct xlnx_crtc *crtc;
	unsigned int align = 1;

	mutex_lock(&helper->lock);
	list_for_each_entry(crtc, &helper->xlnx_crtcs, list) {
		if (crtc->ops->get_align)
			align = ALIGN(align, crtc->ops->get_align(crtc));
	}
	mutex_unlock(&helper->lock);

	return align;
}

u64 xlnx_crtc_helper_get_dma_mask(struct xlnx_crtc_helper *helper)
{
	struct xlnx_crtc *crtc;
	u64 mask = DMA_BIT_MASK(sizeof(dma_addr_t) * 8);

	mutex_lock(&helper->lock);
	list_for_each_entry(crtc, &helper->xlnx_crtcs, list) {
		if (crtc->ops->get_dma_mask)
			mask = min(mask, crtc->ops->get_dma_mask(crtc));
	}
	mutex_unlock(&helper->lock);

	return mask;
}

int xlnx_crtc_helper_get_max_width(struct xlnx_crtc_helper *helper)
{
	struct xlnx_crtc *crtc;
	int width = S32_MAX;

	mutex_lock(&helper->lock);
	list_for_each_entry(crtc, &helper->xlnx_crtcs, list) {
		if (crtc->ops->get_max_width)
			width = min(width, crtc->ops->get_max_width(crtc));
	}
	mutex_unlock(&helper->lock);

	return width;
}

int xlnx_crtc_helper_get_max_height(struct xlnx_crtc_helper *helper)
{
	struct xlnx_crtc *crtc;
	int height = S32_MAX;

	mutex_lock(&helper->lock);
	list_for_each_entry(crtc, &helper->xlnx_crtcs, list) {
		if (crtc->ops->get_max_height)
			height = min(height, crtc->ops->get_max_height(crtc));
	}
	mutex_unlock(&helper->lock);

	return height;
}

u32 xlnx_crtc_helper_get_format(struct xlnx_crtc_helper *helper)
{
	struct xlnx_crtc *crtc;
	u32 result = 0, format;

	mutex_lock(&helper->lock);
	list_for_each_entry(crtc, &helper->xlnx_crtcs, list) {
		if (crtc->ops->get_format) {
			format = crtc->ops->get_format(crtc);
			if (result && result != format) {
				mutex_unlock(&helper->lock);
				return 0;
			}
			result = format;
		}
	}
	mutex_unlock(&helper->lock);

	return result;
}

u32 xlnx_crtc_helper_get_cursor_width(struct xlnx_crtc_helper *helper)
{
	struct xlnx_crtc *crtc;
	u32 width = U32_MAX;

	mutex_lock(&helper->lock);
	list_for_each_entry(crtc, &helper->xlnx_crtcs, list) {
		if (crtc->ops->get_cursor_width)
			width = min(width, crtc->ops->get_cursor_width(crtc));
	}
	mutex_unlock(&helper->lock);

	return width;
}

u32 xlnx_crtc_helper_get_cursor_height(struct xlnx_crtc_helper *helper)
{
	struct xlnx_crtc *crtc;
	u32 height = U32_MAX;

	mutex_lock(&helper->lock);
	list_for_each_entry(crtc, &helper->xlnx_crtcs, list) {
		if (crtc->ops->get_cursor_height)
			height = min(height,
				     crtc->ops->get_cursor_height(crtc));
	}
	mutex_unlock(&helper->lock);

	return height;
}

void xlnx_crtc_helper_init(struct xlnx_crtc_helper *helper)
{
	INIT_LIST_HEAD(&helper->xlnx_crtcs);
	mutex_init(&helper->lock);
}

void xlnx_crtc_helper_fini(struct xlnx_crtc_helper *helper)
{
	if (WARN_ON(!list_empty(&helper->xlnx_crtcs)))
		return;

	mutex_destroy(&helper->lock);
}

void xlnx_crtc_register(struct drm_device *drm, struct xlnx_crtc *crtc)
{
	struct xlnx_crtc_helper *helper = xlnx_get_crtc_helper(drm);

	mutex_lock(&helper->lock);
	list_add_tail(&crtc->list, &helper->xlnx_crtcs);
	mutex_unlock(&helper->lock);
}
EXPORT_SYMBOL_GPL(xlnx_crtc_register);

void xlnx_crtc_unregister(struct drm_device *drm, struct xlnx_crtc *crtc)
{
	struct xlnx_crtc_helper *helper = xlnx_get_crtc_helper(drm);

	mutex_lock(&helper->lock);
	list_del(&crtc->list);
	mutex_unlock(&helper->lock);
}
EXPORT_SYMBOL_GPL(xlnx_crtc_unregister);
