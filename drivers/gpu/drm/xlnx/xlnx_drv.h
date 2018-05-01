// SPDX-License-Identifier: GPL-2.0
/*
 * Xilinx DRM KMS Header for Xilinx
 *
 *  Copyright (C) 2013 - 2018 Xilinx, Inc.
 *
 *  Author: Hyun Woo Kwon <hyunk@xilinx.com>
 */

#ifndef _XLNX_DRV_H_
#define _XLNX_DRV_H_

struct device;
struct drm_device;
struct xlnx_crtc_helper;

struct device *xlnx_drm_pipeline_init(struct device *parent);
void xlnx_drm_pipeline_exit(struct device *pipeline);

uint32_t xlnx_get_format(struct drm_device *drm);
unsigned int xlnx_get_align(struct drm_device *drm);
struct xlnx_crtc_helper *xlnx_get_crtc_helper(struct drm_device *drm);
struct xlnx_bridge_helper *xlnx_get_bridge_helper(struct drm_device *drm);

#endif /* _XLNX_DRV_H_ */
