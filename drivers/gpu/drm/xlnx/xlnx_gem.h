// SPDX-License-Identifier: GPL-2.0
/*
 * Xilinx DRM KMS GEM helper header
 *
 *  Copyright (C) 2015 - 2018 Xilinx, Inc.
 *
 *  Author: Hyun Woo Kwon <hyun.kwon@xilinx.com>
 */

#ifndef _XLNX_GEM_H_
#define _XLNX_GEM_H_

struct drm_device;
struct drm_file;
struct drm_mode_create_dumb;

int xlnx_gem_cma_dumb_create(struct drm_file *file_priv,
			     struct drm_device *drm,
			     struct drm_mode_create_dumb *args);

#endif /* _XLNX_GEM_H_ */
