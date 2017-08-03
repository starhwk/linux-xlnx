/*
 * Xilinx DRM KMS Driver
 *
 *  Copyright (C) 2013 - 2017 Xilinx, Inc.
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
#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_gem_cma_helper.h>
#include <drm/drm_of.h>

#include <linux/component.h>
#include <linux/device.h>
#include <linux/dma-buf.h>
#include <linux/module.h>
#include <linux/of_graph.h>
#include <linux/platform_device.h>
#include <linux/reservation.h>

#include "xlnx_bridge.h"
#include "xlnx_crtc.h"
#include "xlnx_drv.h"
#include "xlnx_fb.h"
#include "xlnx_gem.h"

#define DRIVER_NAME	"xlnx"
#define DRIVER_DESC	"Xilinx DRM KMS Driver"
#define DRIVER_DATE	"20130509"
#define DRIVER_MAJOR	1
#define DRIVER_MINOR	0

static uint xlnx_fbdev_vres = 2;
module_param_named(fbdev_vres, xlnx_fbdev_vres, uint, 0444);
MODULE_PARM_DESC(fbdev_vres,
		 "fbdev virtual resolution multiplier for fb (default: 2)");

/**
 * struct xlnx_drm - Xilinx DRM private data
 * @drm: DRM core
 * @crtc: Xilinx DRM CRTC helper
 * @bridge: Xilinx DRM bridge helper
 * @fb: DRM fb helper
 * @pdev: platform device
 * @suspend_state: atomic state for suspend / resume
 */
struct xlnx_drm {
	struct drm_device *drm;
	struct xlnx_crtc_helper *crtc;
	struct xlnx_bridge_helper *bridge;
	struct drm_fb_helper *fb;
	struct platform_device *pdev;
	struct drm_atomic_state *suspend_state;
};

/**
 * xlnx_get_crtc_helper - Return the crtc helper instance
 * @drm: DRM device
 *
 * Return: the crtc helper instance
 */
struct xlnx_crtc_helper *xlnx_get_crtc_helper(struct drm_device *drm)
{
	struct xlnx_drm *xlnx_drm = drm->dev_private;

	return xlnx_drm->crtc;
}

/**
 * xlnx_get_bridge_helper - Return the bridge helper instance
 * @drm: DRM device
 *
 * Return: the bridge helper instance
 */
struct xlnx_bridge_helper *xlnx_get_bridge_helper(struct drm_device *drm)
{
	struct xlnx_drm *xlnx_drm = drm->dev_private;

	return xlnx_drm->bridge;
}

/**
 * xlnx_get_align - Return the align requirement through CRTC helper
 * @drm: DRM device
 *
 * Return: the alignment requirement
 */
unsigned int xlnx_get_align(struct drm_device *drm)
{
	struct xlnx_drm *xlnx_drm = drm->dev_private;

	return xlnx_crtc_helper_get_align(xlnx_drm->crtc);
}

/**
 * xlnx_get_format - Return the current format of CRTC
 * @drm: DRM device
 *
 * Return: the current CRTC format
 */
uint32_t xlnx_get_format(struct drm_device *drm)
{
	struct xlnx_drm *xlnx_drm = drm->dev_private;

	return xlnx_crtc_helper_get_format(xlnx_drm->crtc);
}

static void xlnx_output_poll_changed(struct drm_device *drm)
{
	struct xlnx_drm *xlnx_drm = drm->dev_private;

	xlnx_fb_hotplug_event(xlnx_drm->fb);
}

static const struct drm_mode_config_funcs xlnx_mode_config_funcs = {
	.fb_create		= xlnx_fb_create,
	.output_poll_changed	= xlnx_output_poll_changed,
	.atomic_check		= drm_atomic_helper_check,
	.atomic_commit		= drm_atomic_helper_commit,
};

static struct drm_mode_config_helper_funcs xlnx_mode_config_helper_funcs = {
	.atomic_commit_tail = drm_atomic_helper_commit_tail,
};

static int xlnx_enable_vblank(struct drm_device *drm, unsigned int crtc)
{
	struct xlnx_drm *xlnx_drm = drm->dev_private;

	return xlnx_crtc_helper_enable_vblank(xlnx_drm->crtc, crtc);
}

static void xlnx_disable_vblank(struct drm_device *drm, unsigned int crtc)
{
	struct xlnx_drm *xlnx_drm = drm->dev_private;

	xlnx_crtc_helper_disable_vblank(xlnx_drm->crtc, crtc);
}

static void xlnx_mode_config_init(struct drm_device *drm)
{
	struct xlnx_drm *xlnx_drm = drm->dev_private;
	struct xlnx_crtc_helper *crtc = xlnx_drm->crtc;

	drm->mode_config.min_width = 0;
	drm->mode_config.min_height = 0;
	drm->mode_config.max_width = xlnx_crtc_helper_get_max_width(crtc);
	drm->mode_config.max_height = xlnx_crtc_helper_get_max_height(crtc);
}

static int xlnx_load(struct drm_device *drm, unsigned long flags)
{
	struct xlnx_drm *xlnx_drm;
	struct platform_device *pdev = drm->platformdev;
	unsigned int align;
	int depth, bpp, ret;
	unsigned int num_crtcs;

	xlnx_drm = devm_kzalloc(drm->dev, sizeof(*xlnx_drm), GFP_KERNEL);
	if (!xlnx_drm)
		return -ENOMEM;

	drm_mode_config_init(drm);
	drm->mode_config.funcs = &xlnx_mode_config_funcs;
	drm->mode_config.helper_private = &xlnx_mode_config_helper_funcs;

	ret = drm_vblank_init(drm, 1);
	if (ret) {
		dev_err(&pdev->dev, "failed to initialize vblank\n");
		goto err_out;
	}

	drm->irq_enabled = 1;
	drm->dev_private = xlnx_drm;
	xlnx_drm->drm = drm;
	drm_kms_helper_poll_init(drm);
	platform_set_drvdata(pdev, xlnx_drm);

	xlnx_drm->crtc = xlnx_crtc_helper_init(drm);
	if (IS_ERR(xlnx_drm->crtc)) {
		ret = PTR_ERR(xlnx_drm->crtc);
		goto err_vblank;
	}

	xlnx_drm->bridge = xlnx_bridge_helper_init(drm);
	if (IS_ERR(xlnx_drm->bridge)) {
		ret = PTR_ERR(xlnx_drm->bridge);
		goto err_crtc;
	}

	ret = component_bind_all(drm->dev, drm);
	if (ret)
		goto err_bridge;

	xlnx_mode_config_init(drm);
	drm_mode_config_reset(drm);
	num_crtcs = xlnx_crtc_helper_get_num_crtcs(xlnx_drm->crtc);
	drm_fb_get_bpp_depth(xlnx_crtc_helper_get_format(xlnx_drm->crtc),
			     &depth, &bpp);
	if (bpp) {
		align = xlnx_crtc_helper_get_align(xlnx_drm->crtc);
		xlnx_drm->fb = xlnx_fb_init(drm, bpp, num_crtcs, 4, align,
					    xlnx_fbdev_vres);
		if (IS_ERR(xlnx_drm->fb)) {
			dev_err(&pdev->dev, "failed to initialize drm fb\n");
			goto err_component;
		}
	} else {
		dev_info(&pdev->dev, "fbdev is not initialized\n");
	}

	return 0;

err_component:
	component_unbind_all(drm->dev, drm);
err_bridge:
	xlnx_bridge_helper_fini(drm, xlnx_drm->bridge);
err_crtc:
	xlnx_crtc_helper_fini(drm, xlnx_drm->crtc);
err_vblank:
	drm_vblank_cleanup(drm);
err_out:
	drm_mode_config_cleanup(drm);
	return ret;
}

static int xlnx_unload(struct drm_device *drm)
{
	struct xlnx_drm *xlnx_drm = drm->dev_private;

	xlnx_fb_fini(xlnx_drm->fb);
	component_unbind_all(drm->dev, drm);
	xlnx_bridge_helper_fini(drm, xlnx_drm->bridge);
	xlnx_crtc_helper_fini(drm, xlnx_drm->crtc);
	drm_vblank_cleanup(drm);
	drm_kms_helper_poll_fini(drm);
	drm_mode_config_cleanup(drm);

	return 0;
}

static void xlnx_preclose(struct drm_device *drm, struct drm_file *file)
{
	struct xlnx_drm *xlnx_drm = drm->dev_private;

	xlnx_crtc_helper_cancel_page_flip(xlnx_drm->crtc, file);
}

static void xlnx_lastclose(struct drm_device *drm)
{
	struct xlnx_drm *xlnx_drm = drm->dev_private;

	xlnx_crtc_helper_restore(xlnx_drm->crtc);
	xlnx_fb_restore_mode(xlnx_drm->fb);
}

static const struct file_operations xlnx_fops = {
	.owner		= THIS_MODULE,
	.open		= drm_open,
	.release	= drm_release,
	.unlocked_ioctl	= drm_ioctl,
	.mmap		= drm_gem_cma_mmap,
	.poll		= drm_poll,
	.read		= drm_read,
#ifdef CONFIG_COMPAT
	.compat_ioctl	= drm_compat_ioctl,
#endif
	.llseek		= noop_llseek,
};

static struct drm_driver xlnx_drm_driver = {
	.driver_features		= DRIVER_MODESET | DRIVER_GEM |
					  DRIVER_ATOMIC | DRIVER_PRIME,
	.load				= xlnx_load,
	.unload				= xlnx_unload,
	.preclose			= xlnx_preclose,
	.lastclose			= xlnx_lastclose,

	.get_vblank_counter		= drm_vblank_no_hw_counter,
	.enable_vblank			= xlnx_enable_vblank,
	.disable_vblank			= xlnx_disable_vblank,

	.prime_handle_to_fd		= drm_gem_prime_handle_to_fd,
	.prime_fd_to_handle		= drm_gem_prime_fd_to_handle,
	.gem_prime_export		= drm_gem_prime_export,
	.gem_prime_import		= drm_gem_prime_import,
	.gem_prime_get_sg_table		= drm_gem_cma_prime_get_sg_table,
	.gem_prime_import_sg_table	= drm_gem_cma_prime_import_sg_table,
	.gem_prime_vmap			= drm_gem_cma_prime_vmap,
	.gem_prime_vunmap		= drm_gem_cma_prime_vunmap,
	.gem_prime_mmap			= drm_gem_cma_prime_mmap,
	.gem_free_object		= drm_gem_cma_free_object,
	.gem_vm_ops			= &drm_gem_cma_vm_ops,
	.dumb_create			= xlnx_gem_cma_dumb_create,
	.dumb_map_offset		= drm_gem_cma_dumb_map_offset,
	.dumb_destroy			= drm_gem_dumb_destroy,

	.fops				= &xlnx_fops,

	.name				= DRIVER_NAME,
	.desc				= DRIVER_DESC,
	.date				= DRIVER_DATE,
	.major				= DRIVER_MAJOR,
	.minor				= DRIVER_MINOR,
};

static int xlnx_bind(struct device *dev)
{
	return drm_platform_init(&xlnx_drm_driver, to_platform_device(dev));
}

static void xlnx_unbind(struct device *dev)
{
	struct xlnx_drm *xlnx_drm = dev_get_drvdata(dev);

	drm_put_dev(xlnx_drm->drm);
}

static const struct component_master_ops xlnx_master_ops = {
	.bind	= xlnx_bind,
	.unbind	= xlnx_unbind,
};

static int xlnx_of_component_probe(struct device *dev,
				   int (*compare_of)(struct device *, void *),
				   const struct component_master_ops *m_ops)
{
	struct device_node *ep, *port, *remote;
	struct component_match *match = NULL;
	int i;

	if (!dev->of_node)
		return -EINVAL;

	for (i = 0; ; i++) {
		port = of_parse_phandle(dev->of_node, "ports", i);
		if (!port)
			break;

		if (!of_device_is_available(port->parent)) {
			of_node_put(port);
			continue;
		}

		component_match_add(dev, &match, compare_of, port->parent);
		of_node_put(port);
	}

	if (i == 0) {
		dev_err(dev, "missing 'ports' property\n");
		return -ENODEV;
	}

	if (!match) {
		dev_err(dev, "no available port\n");
		return -ENODEV;
	}

	for (i = 0; ; i++) {
		port = of_parse_phandle(dev->of_node, "ports", i);
		if (!port)
			break;

		if (!of_device_is_available(port->parent)) {
			of_node_put(port);
			continue;
		}

		for_each_child_of_node(port, ep) {
			remote = of_graph_get_remote_port_parent(ep);
			if (!remote || !of_device_is_available(remote)) {
				of_node_put(remote);
				continue;
			} else if (!of_device_is_available(remote->parent)) {
				dev_warn(dev, "parent dev of %s unavailable\n",
					 remote->full_name);
				of_node_put(remote);
				continue;
			}
			component_match_add(dev, &match, compare_of, remote);
			of_node_put(remote);
		}
		of_node_put(port);
	}

	return component_master_add_with_match(dev, m_ops, match);
}

static int xlnx_compare_of(struct device *dev, void *data)
{
	return dev->of_node == data;
}

static int xlnx_platform_probe(struct platform_device *pdev)
{
	return xlnx_of_component_probe(&pdev->dev, xlnx_compare_of,
				       &xlnx_master_ops);
}

static int xlnx_platform_remove(struct platform_device *pdev)
{
	component_master_del(&pdev->dev, &xlnx_master_ops);
	return 0;
}

static void xlnx_platform_shutdown(struct platform_device *pdev)
{
	struct xlnx_drm *xlnx_drm = platform_get_drvdata(pdev);

	drm_put_dev(xlnx_drm->drm);
}

static int __maybe_unused xlnx_pm_suspend(struct device *dev)
{
	struct xlnx_drm *xlnx_drm = dev_get_drvdata(dev);
	struct drm_device *drm = xlnx_drm->drm;

	drm_kms_helper_poll_disable(drm);

	xlnx_drm->suspend_state = drm_atomic_helper_suspend(drm);
	if (IS_ERR(xlnx_drm->suspend_state)) {
		drm_kms_helper_poll_enable(drm);
		return PTR_ERR(xlnx_drm->suspend_state);
	}

	return 0;
}

static int __maybe_unused xlnx_pm_resume(struct device *dev)
{
	struct xlnx_drm *xlnx_drm = dev_get_drvdata(dev);
	struct drm_device *drm = xlnx_drm->drm;

	drm_atomic_helper_resume(drm, xlnx_drm->suspend_state);
	drm_kms_helper_poll_enable(drm);

	return 0;
}

static const struct dev_pm_ops xlnx_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(xlnx_pm_suspend, xlnx_pm_resume)
};

static const struct of_device_id xlnx_of_match[] = {
	{ .compatible = "xlnx,kms", },
	{ /* end of table */ },
};
MODULE_DEVICE_TABLE(of, xlnx_of_match);

static struct platform_driver xlnx_driver = {
	.probe			= xlnx_platform_probe,
	.remove			= xlnx_platform_remove,
	.shutdown		= xlnx_platform_shutdown,
	.driver			= {
		.name		= "xlnx-drm",
		.pm		= &xlnx_pm_ops,
		.of_match_table	= xlnx_of_match,
	},
};

module_platform_driver(xlnx_driver);

MODULE_AUTHOR("Xilinx, Inc.");
MODULE_DESCRIPTION("Xilinx DRM KMS Driver");
MODULE_LICENSE("GPL v2");
