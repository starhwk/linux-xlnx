// SPDX-License-Identifier: GPL-2.0
/*
 * Xilinx DRM KMS Driver
 *
 *  Copyright (C) 2013 - 2018 Xilinx, Inc.
 *
 *  Author: Hyun Woo Kwon <hyun.kwon@xilinx.com>
 */

#include <drm/drmP.h>
#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_fb_helper.h>
#include <drm/drm_gem_cma_helper.h>
#include <drm/drm_of.h>

#include <linux/component.h>
#include <linux/device.h>
#include <linux/dma-buf.h>
#include <linux/module.h>
#include <linux/of_graph.h>

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

/**
 * struct xlnx_drm - Xilinx DRM private data
 * @drm: DRM device
 * @crtc_helper: Helper to access Xilinx CRTCs
 * @fb: DRM fb helper
 * @master: logical master device for pipeline
 * @suspend_state: atomic state for suspend / resume
 * @is_master: A flag to indicate if this instance is fake master
 */
struct xlnx_drm {
	struct drm_device *drm;
	struct xlnx_crtc_helper crtc_helper;
	struct drm_fb_helper *fb;
	struct device *master;
	struct drm_atomic_state *suspend_state;
	bool is_master;
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

	return &xlnx_drm->crtc_helper;
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

	return xlnx_crtc_helper_get_align(&xlnx_drm->crtc_helper);
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

	return xlnx_crtc_helper_get_format(&xlnx_drm->crtc_helper);
}

static void xlnx_output_poll_changed(struct drm_device *drm)
{
	struct xlnx_drm *xlnx_drm = drm->dev_private;

	if (xlnx_drm->fb)
		drm_fb_helper_hotplug_event(xlnx_drm->fb);
}

static const struct drm_mode_config_funcs xlnx_mode_config_funcs = {
	.fb_create		= xlnx_fb_create,
	.output_poll_changed	= xlnx_output_poll_changed,
	.atomic_check		= drm_atomic_helper_check,
	.atomic_commit		= drm_atomic_helper_commit,
};

static void xlnx_mode_config_init(struct drm_device *drm)
{
	struct xlnx_drm *xlnx_drm = drm->dev_private;
	struct xlnx_crtc_helper *crtc_helper = &xlnx_drm->crtc_helper;

	drm->mode_config.min_width = 0;
	drm->mode_config.min_height = 0;
	drm->mode_config.max_width =
		xlnx_crtc_helper_get_max_width(crtc_helper);
	drm->mode_config.max_height =
		xlnx_crtc_helper_get_max_height(crtc_helper);
	drm->mode_config.cursor_width =
		xlnx_crtc_helper_get_cursor_width(crtc_helper);
	drm->mode_config.cursor_height =
		xlnx_crtc_helper_get_cursor_height(crtc_helper);
}

static int xlnx_drm_open(struct drm_device *dev, struct drm_file *file)
{
	struct xlnx_drm *xlnx_drm = dev->dev_private;

	/* This is a hacky way to allow the root user to run as a master */
	if (!(drm_is_primary_client(file) && !dev->master) &&
	    !file->is_master && capable(CAP_SYS_ADMIN)) {
		file->is_master = 1;
		xlnx_drm->is_master = true;
	}

	return 0;
}

static int xlnx_drm_release(struct inode *inode, struct file *filp)
{
	struct drm_file *file = filp->private_data;
	struct drm_minor *minor = file->minor;
	struct drm_device *drm = minor->dev;
	struct xlnx_drm *xlnx_drm = drm->dev_private;

	if (xlnx_drm->is_master) {
		xlnx_drm->is_master = false;
		file->is_master = 0;
	}

	return drm_release(inode, filp);
}

static void xlnx_lastclose(struct drm_device *drm)
{
	struct xlnx_drm *xlnx_drm = drm->dev_private;

	if (xlnx_drm->fb)
		drm_fb_helper_restore_fbdev_mode_unlocked(xlnx_drm->fb);
}

static const struct file_operations xlnx_fops = {
	.owner		= THIS_MODULE,
	.open		= drm_open,
	.release	= xlnx_drm_release,
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
	.open				= xlnx_drm_open,
	.lastclose			= xlnx_lastclose,

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
	.dumb_destroy			= drm_gem_dumb_destroy,

	.fops				= &xlnx_fops,

	.name				= DRIVER_NAME,
	.desc				= DRIVER_DESC,
	.date				= DRIVER_DATE,
	.major				= DRIVER_MAJOR,
	.minor				= DRIVER_MINOR,
};

static int xlnx_bind(struct device *master)
{
	struct xlnx_drm *xlnx_drm;
	struct drm_device *drm;
	const struct drm_format_info *info;
	int ret;
	u32 format;

	drm = drm_dev_alloc(&xlnx_drm_driver, master->parent);
	if (IS_ERR(drm))
		return PTR_ERR(drm);

	xlnx_drm = devm_kzalloc(drm->dev, sizeof(*xlnx_drm), GFP_KERNEL);
	if (!xlnx_drm) {
		ret = -ENOMEM;
		goto err_drm;
	}

	drm_mode_config_init(drm);
	drm->mode_config.funcs = &xlnx_mode_config_funcs;

	ret = drm_vblank_init(drm, 1);
	if (ret) {
		dev_err(master->parent, "failed to initialize vblank\n");
		goto err_xlnx_drm;
	}

	drm->irq_enabled = 1;
	drm->dev_private = xlnx_drm;
	xlnx_drm->drm = drm;
	xlnx_drm->master = master;
	drm_kms_helper_poll_init(drm);
	dev_set_drvdata(master, xlnx_drm);

	xlnx_crtc_helper_init(&xlnx_drm->crtc_helper);

	ret = component_bind_all(master, drm);
	if (ret)
		goto err_crtc;

	xlnx_mode_config_init(drm);
	drm_mode_config_reset(drm);
	dma_set_mask(drm->dev,
		     xlnx_crtc_helper_get_dma_mask(&xlnx_drm->crtc_helper));

	format = xlnx_crtc_helper_get_format(&xlnx_drm->crtc_helper);
	info = drm_format_info(format);
	if (info && info->depth && info->cpp[0]) {
		unsigned int align;

		align = xlnx_crtc_helper_get_align(&xlnx_drm->crtc_helper);
		xlnx_drm->fb = xlnx_fb_init(drm, info->cpp[0] * 8, 1, align);
		if (IS_ERR(xlnx_drm->fb)) {
			dev_err(master->parent,
				"failed to initialize drm fb\n");
			xlnx_drm->fb = NULL;
		}
	} else {
		/* fbdev emulation is optional */
		dev_info(master->parent, "fbdev is not initialized\n");
	}

	ret = drm_dev_register(drm, 0);
	if (ret < 0)
		goto err_fb;

	return 0;

err_fb:
	if (xlnx_drm->fb)
		xlnx_fb_fini(xlnx_drm->fb);
	component_unbind_all(drm->dev, drm);
err_crtc:
	xlnx_crtc_helper_fini(&xlnx_drm->crtc_helper);
err_xlnx_drm:
	drm_mode_config_cleanup(drm);
err_drm:
	drm_dev_unref(drm);
	return ret;
}

static void xlnx_unbind(struct device *master)
{
	struct xlnx_drm *xlnx_drm = dev_get_drvdata(master);
	struct drm_device *drm = xlnx_drm->drm;

	drm_dev_unregister(drm);
	if (xlnx_drm->fb)
		xlnx_fb_fini(xlnx_drm->fb);
	component_unbind_all(master, drm);
	xlnx_crtc_helper_fini(&xlnx_drm->crtc_helper);
	drm_kms_helper_poll_fini(drm);
	drm_mode_config_cleanup(drm);
	drm_dev_unref(drm);
}

static const struct component_master_ops xlnx_master_ops = {
	.bind	= xlnx_bind,
	.unbind	= xlnx_unbind,
};

static int xlnx_compare_of(struct device *dev, void *data)
{
	return dev->of_node == data;
}

static int xlnx_probe(struct device *master_dev)
{
	struct device *dev = master_dev->parent;
	struct device_node *ep, *port, *remote, *parent;
	struct component_match *match = NULL;
	int i;

	if (!dev->of_node)
		return -EINVAL;

	component_match_add(master_dev, &match, xlnx_compare_of, dev->of_node);

	for (i = 0; ; i++) {
		port = of_parse_phandle(dev->of_node, "ports", i);
		if (!port)
			break;

		parent = of_get_parent(port);
		of_node_put(port);
		if (!of_node_cmp(parent->name, "ports")) {
			port = parent;
			parent = of_get_parent(parent);
			of_node_put(port);
		}

		if (!of_device_is_available(parent)) {
			of_node_put(parent);
			continue;
		}

		component_match_add(master_dev, &match, xlnx_compare_of,
				    parent);
		of_node_put(parent);
	}

	parent = dev->of_node;
	for (i = 0; ; i++) {
		parent = of_node_get(parent);
		if (!of_device_is_available(parent)) {
			of_node_put(parent);
			continue;
		}

		for_each_endpoint_of_node(parent, ep) {
			remote = of_graph_get_remote_port_parent(ep);
			if (!remote || !of_device_is_available(remote) ||
			    remote == dev->of_node) {
				of_node_put(remote);
				continue;
			} else if (!of_device_is_available(remote->parent)) {
				dev_warn(dev, "parent dev of %s unavailable\n",
					 remote->full_name);
				of_node_put(remote);
				continue;
			}
			component_match_add(master_dev, &match, xlnx_compare_of,
					    remote);
			of_node_put(remote);
		}
		of_node_put(parent);

		port = of_parse_phandle(dev->of_node, "ports", i);
		if (!port)
			break;

		parent = of_get_parent(port);
		of_node_put(port);
		if (!of_node_cmp(parent->name, "ports")) {
			port = parent;
			parent = of_get_parent(parent);
			of_node_put(port);
		}
	}

	return component_master_add_with_match(master_dev, &xlnx_master_ops,
					       match);
}

static int xlnx_remove(struct device *dev)
{
	component_master_del(dev, &xlnx_master_ops);
	return 0;
}

static void xlnx_shutdown(struct device *dev)
{
	component_master_del(dev, &xlnx_master_ops);
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

static int xlnx_drv_bus_match(struct device *dev, struct device_driver *drv)
{
	return !strncmp(dev_name(dev), drv->name, strlen(drv->name));
}

struct bus_type xlnx_driver_bus_type = {
	.name   = "xlnx-drm-bus",
	.match  = &xlnx_drv_bus_match,
};

static struct device_driver xlnx_driver = {
	.probe		= xlnx_probe,
	.remove		= xlnx_remove,
	.shutdown	= xlnx_shutdown,
	.name		= "xlnx-drm",
	.pm		= &xlnx_pm_ops,
	.bus		= &xlnx_driver_bus_type,
	.owner		= THIS_MODULE,
};

/* bitmap for master id */
static u32 xlnx_master_ids = GENMASK(31, 0);

static void xlnx_master_release(struct device *dev)
{
	kfree(dev);
}

/**
 * xlnx_drm_pipeline_init - Initialize the drm pipeline for the device
 * @dev: The device to initialize the drm pipeline device
 *
 * This function initializes the drm pipeline device, struct drm_device,
 * on @dev by creating a logical master device. The logical device acts
 * as a master device to bind slave devices and represents the entire
 * pipeline.
 * The logical master uses the port bindings of the calling device to
 * figure out the pipeline topology.
 *
 * Return: the logical master device if the drm device is initialized
 * on @dev. Error code otherwise.
 */
struct device *xlnx_drm_pipeline_init(struct device *dev)
{
	struct device *master;
	int id, ret;

	id = ffs(xlnx_master_ids);
	if (!id)
		return ERR_PTR(-ENOSPC);

	master = kzalloc(sizeof(*master), GFP_KERNEL);
	if (!master)
		return ERR_PTR(-ENOMEM);
	device_initialize(master);
	master->parent = dev;
	master->bus = &xlnx_driver_bus_type;
	master->release = xlnx_master_release;

	ret = dev_set_name(master, "xlnx-drm.%d", id);
	if (ret)
		goto err_kfree;

	ret = device_add(master);
	if (ret)
		goto err_kfree;

	xlnx_master_ids &= ~BIT(master->id);
	return master;

err_kfree:
	kfree(master);
	return ERR_PTR(ret);
}
EXPORT_SYMBOL_GPL(xlnx_drm_pipeline_init);

/**
 * xlnx_drm_pipeline_exit - Release the drm pipeline for the device
 * @master: The master pipeline device to release
 *
 * Release the logical pipeline device returned by xlnx_drm_pipeline_init().
 */
void xlnx_drm_pipeline_exit(struct device *master)
{
	xlnx_master_ids |= BIT(master->id);
	device_unregister(master);
}
EXPORT_SYMBOL_GPL(xlnx_drm_pipeline_exit);

static int __init xlnx_drm_drv_init(void)
{
	int ret;

	xlnx_bridge_helper_init();
	ret = bus_register(&xlnx_driver_bus_type);
	if (ret)
		return ret;

	ret = driver_register(&xlnx_driver);
	if (ret) {
		bus_unregister(&xlnx_driver_bus_type);
		return ret;
	}

	return 0;
}

static void __exit xlnx_drm_drv_exit(void)
{
	bus_unregister(&xlnx_driver_bus_type);
	driver_unregister(&xlnx_driver);
	xlnx_bridge_helper_fini();
}

module_init(xlnx_drm_drv_init);
module_exit(xlnx_drm_drv_exit);

MODULE_AUTHOR("Xilinx, Inc.");
MODULE_DESCRIPTION("Xilinx DRM KMS Driver");
MODULE_LICENSE("GPL v2");
