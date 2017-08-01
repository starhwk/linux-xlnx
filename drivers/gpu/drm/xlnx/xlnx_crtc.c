/*
 * Xilinx DRM crtc driver
 *
 *  Copyright (C) 2017 Xilinx, Inc.
 *
 *  Author: Hyun Woo Kwon <hyun.kwon@xilinx.com>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <drm/drmP.h>

#include <linux/list.h>

#include "xlnx_crtc.h"
#include "xlnx_drv.h"

/*
 * Overview
 * --------
 *
 * The Xilinx CRTC layer is to enable the custom interface to CRTC drivers.
 * The interface is used by Xilinx DRM driver where it needs CRTC
 * functionailty. CRTC drivers should attach the desired callbacks
 * to struct xlnx_crtc and register the xlnx_crtc with correcsponding
 * drm_device. It's highly recommended CRTC drivers register all callbacks
 * even though many of them are optional.
 * The CRTC helper simply walks through the registered CRTC device,
 * and call the callbacks.
 */

/**
 * struct xlnx_crtc_helper - Xilinx CRTC helper
 * @xlnx_crtcs: list of Xilinx CRTC devices
 * @lock: lock to protect @xlnx_crtcs
 * @drm: back pointer to DRM core
 * @num_crtcs: number of registered Xilinx CRTC device
 */
struct xlnx_crtc_helper {
	struct list_head xlnx_crtcs;
	struct mutex lock; /* lock for @xlnx_crtcs */
	struct drm_device *drm;
	unsigned int num_crtcs;
};

#define XLNX_CRTC_MAX_HEIGHT_WIDTH	UINT_MAX

int xlnx_crtc_helper_enable_vblank(struct xlnx_crtc_helper *helper,
				   unsigned int crtc_id)
{
	struct xlnx_crtc *crtc;

	list_for_each_entry(crtc, &helper->xlnx_crtcs, list)
		if (drm_crtc_index(&crtc->crtc) == crtc_id)
			if (crtc->enable_vblank)
				return crtc->enable_vblank(crtc);
	return -ENODEV;
}

void xlnx_crtc_helper_disable_vblank(struct xlnx_crtc_helper *helper,
				     unsigned int crtc_id)
{
	struct xlnx_crtc *crtc;

	list_for_each_entry(crtc, &helper->xlnx_crtcs, list) {
		if (drm_crtc_index(&crtc->crtc) == crtc_id) {
			if (crtc->disable_vblank)
				crtc->disable_vblank(crtc);
			return;
		}
	}
}

void xlnx_crtc_helper_cancel_page_flip(struct xlnx_crtc_helper *helper,
				       struct drm_file *file)
{
	struct xlnx_crtc *crtc;

	list_for_each_entry(crtc, &helper->xlnx_crtcs, list)
		if (crtc->cancel_page_flip)
			crtc->cancel_page_flip(crtc, file);
}

unsigned int xlnx_crtc_helper_get_align(struct xlnx_crtc_helper *helper)
{
	struct xlnx_crtc *crtc;
	unsigned int align = 1, tmp;

	list_for_each_entry(crtc, &helper->xlnx_crtcs, list) {
		if (crtc->get_align) {
			tmp = crtc->get_align(crtc);
			align = ALIGN(align, tmp);
		}
	}

	return align;
}

int xlnx_crtc_helper_get_max_width(struct xlnx_crtc_helper *helper)
{
	struct xlnx_crtc *crtc;
	unsigned int width = XLNX_CRTC_MAX_HEIGHT_WIDTH, tmp;

	list_for_each_entry(crtc, &helper->xlnx_crtcs, list) {
		if (crtc->get_max_width) {
			tmp = crtc->get_max_width(crtc);
			width = min(width, tmp);
		}
	}

	return width;
}

int xlnx_crtc_helper_get_max_height(struct xlnx_crtc_helper *helper)
{
	struct xlnx_crtc *crtc;
	unsigned int height = XLNX_CRTC_MAX_HEIGHT_WIDTH, tmp;

	list_for_each_entry(crtc, &helper->xlnx_crtcs, list) {
		if (crtc->get_max_height) {
			tmp = crtc->get_max_height(crtc);
			height = min(height, tmp);
		}
	}

	return height;
}

uint32_t xlnx_crtc_helper_get_format(struct xlnx_crtc_helper *helper)
{
	struct xlnx_crtc *crtc;
	u32 format = 0, tmp;

	list_for_each_entry(crtc, &helper->xlnx_crtcs, list) {
		if (crtc->get_format) {
			tmp = crtc->get_format(crtc);
			if (format && format != tmp)
				return 0;
			format = tmp;
		}
	}

	return format;
}

void xlnx_crtc_helper_restore(struct xlnx_crtc_helper *helper)
{
	struct xlnx_crtc *crtc;

	list_for_each_entry(crtc, &helper->xlnx_crtcs, list)
		if (crtc->restore)
			crtc->restore(crtc);
}

unsigned int xlnx_crtc_helper_get_num_crtcs(struct xlnx_crtc_helper *helper)
{
	return helper->num_crtcs;
}

struct xlnx_crtc_helper *xlnx_crtc_helper_init(struct drm_device *drm)
{
	struct xlnx_crtc_helper *helper;

	helper = devm_kzalloc(drm->dev, sizeof(*helper), GFP_KERNEL);
	if (!helper)
		return ERR_PTR(-ENOMEM);

	INIT_LIST_HEAD(&helper->xlnx_crtcs);
	mutex_init(&helper->lock);
	helper->drm = drm;

	return helper;
}

void xlnx_crtc_helper_fini(struct drm_device *drm,
			   struct xlnx_crtc_helper *helper)
{
	if (WARN_ON(helper->drm != drm))
		return;

	if (WARN_ON(!list_empty(&helper->xlnx_crtcs)))
		return;

	mutex_destroy(&helper->lock);
	devm_kfree(drm->dev, helper);
}

void xlnx_crtc_register(struct drm_device *drm, struct xlnx_crtc *crtc)
{
	struct xlnx_crtc_helper *helper = xlnx_get_crtc_helper(drm);

	mutex_lock(&helper->lock);
	list_add_tail(&crtc->list, &helper->xlnx_crtcs);
	helper->num_crtcs++;
	mutex_unlock(&helper->lock);
}
EXPORT_SYMBOL_GPL(xlnx_crtc_register);

void xlnx_crtc_unregister(struct drm_device *drm, struct xlnx_crtc *crtc)
{
	struct xlnx_crtc_helper *helper = xlnx_get_crtc_helper(drm);

	mutex_lock(&helper->lock);
	list_del(&crtc->list);
	helper->num_crtcs--;
	mutex_unlock(&helper->lock);
}
EXPORT_SYMBOL_GPL(xlnx_crtc_unregister);
