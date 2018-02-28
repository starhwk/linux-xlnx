// SPDX-License-Identifier: GPL-2.0
/*
 * ZynqMP Display Driver
 *
 *  Copyright (C) 2017 - 2018 Xilinx, Inc.
 *
 *  Author: Hyun Woo Kwon <hyun.kwon@xilinx.com>
 */

#ifndef _ZYNQMP_DISP_H_
#define _ZYNQMP_DISP_H_

struct device;
struct platform_device;
struct zynqmp_disp;

void zynqmp_disp_handle_vblank(struct zynqmp_disp *disp);
unsigned int zynqmp_disp_get_apb_clk_rate(struct zynqmp_disp *disp);
bool zynqmp_disp_aud_enabled(struct zynqmp_disp *disp);
unsigned int zynqmp_disp_get_aud_clk_rate(struct zynqmp_disp *disp);
uint32_t zynqmp_disp_get_crtc_mask(struct zynqmp_disp *disp);

int zynqmp_disp_bind(struct device *dev, struct device *master, void *data);
void zynqmp_disp_unbind(struct device *dev, struct device *master, void *data);

int zynqmp_disp_probe(struct platform_device *pdev);
int zynqmp_disp_remove(struct platform_device *pdev);

#endif /* _ZYNQMP_DISP_H_ */
