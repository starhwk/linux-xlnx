/*
 * Xilinx DRM crtc header
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

#ifndef _XLNX_CRTC_H_
#define _XLNX_CRTC_H_

/**
 * struct xlnx_crtc - Xilinx CRTC device
 * @crtc: DRM CRTC device
 * @list: list node for Xilinx CRTC device list
 * @enable_vblank: Enable vblank
 * @disable_vblank: Disable vblank
 * @cancel_page_flip: Cancel the pending page flip request
 * @get_align: Get the alignment requirement of CRTC device
 * @get_max_width: Get the maximum supported width
 * @get_max_height: Get the maximum supported height
 * @get_format: Get the current format of CRTC device
 * @restore: Restore the states if needed
 */
struct xlnx_crtc {
	struct drm_crtc crtc;
	struct list_head list;
	int (*enable_vblank)(struct xlnx_crtc *crtc);
	void (*disable_vblank)(struct xlnx_crtc *crtc);
	void (*cancel_page_flip)(struct xlnx_crtc *crtc, struct drm_file *file);
	unsigned int (*get_align)(struct xlnx_crtc *crtc);
	int (*get_max_width)(struct xlnx_crtc *crtc);
	int (*get_max_height)(struct xlnx_crtc *crtc);
	uint32_t (*get_format)(struct xlnx_crtc *crtc);
	void (*restore)(struct xlnx_crtc *crtc);
};

/*
 * Helper functions: used within Xlnx DRM
 */

struct xlnx_crtc_helper;

int xlnx_crtc_helper_enable_vblank(struct xlnx_crtc_helper *helper,
				   unsigned int crtc_id);
void xlnx_crtc_helper_disable_vblank(struct xlnx_crtc_helper *helper,
				     unsigned int crtc_id);
void xlnx_crtc_helper_cancel_page_flip(struct xlnx_crtc_helper *helper,
				       struct drm_file *file);
unsigned int xlnx_crtc_helper_get_align(struct xlnx_crtc_helper *helper);
int xlnx_crtc_helper_get_max_width(struct xlnx_crtc_helper *helper);
int xlnx_crtc_helper_get_max_height(struct xlnx_crtc_helper *helper);
uint32_t xlnx_crtc_helper_get_format(struct xlnx_crtc_helper *helper);
void xlnx_crtc_helper_restore(struct xlnx_crtc_helper *helper);
unsigned int xlnx_crtc_helper_get_num_crtcs(struct xlnx_crtc_helper *helper);

struct xlnx_crtc_helper *xlnx_crtc_helper_init(struct drm_device *drm);
void xlnx_crtc_helper_fini(struct drm_device *drm,
			   struct xlnx_crtc_helper *helper);

/*
 * CRTC registration: used by other sub-driver modules
 */

static inline struct xlnx_crtc *to_xlnx_crtc(struct drm_crtc *crtc)
{
	return container_of(crtc, struct xlnx_crtc, crtc);
}

void xlnx_crtc_register(struct drm_device *drm, struct xlnx_crtc *crtc);
void xlnx_crtc_unregister(struct drm_device *drm, struct xlnx_crtc *crtc);

#endif /* _XLNX_CRTC_H_ */
