/*
 * DRM KMS support for Xilinx pipelines
 *
 *  Copyright (C) 2015 Xilinx, Inc.
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

#define DRIVER_NAME	"xlnx_drm"
#define DRIVER_DESC	"Xilinx DRM KMS support"
#define DRIVER_DATE	"20151125"
#define DRIVER_MAJOR	1
#define DRIVER_MINOR	0

struct xlnx_drm_private {
	struct drm_device *drm;
};

static int xlnx_drm_enable_vblank(struct drm_device *drm, int crtc)
{
	return 0;
}

static void xlnx_drm_disable_vblank(struct drm_device *drm, int crtc)
{
}

static int xlnx_drm_load(struct drm_device *drm, unsigned long flags)
{
	struct xilinx_drm_private *private;

	private = devm_kzalloc(drm->dev, sizeof(*private), GFP_KERNEL);
	if (!private)
		return -ENOMEM;

	drm->dev_private = private;
	private->drm = drm;

	return 0;
}

static int xlnx_drm_unload(struct drm_device *drm)
{
	return 0;
}

static void xlnx_drm_preclose(struct drm_device *drm, struct drm_file *file)
{
}

static void xlnx_drm_lastclose(struct drm_device *drm)
{
}

static const struct file_operations xlnx_drm_fops = {
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
					  DRIVER_PRIME,
	.load				= xlnx_drm_load,
	.unload				= xlnx_drm_unload,
	.preclose			= xlnx_drm_preclose,
	.lastclose			= xlnx_drm_lastclose,
	.set_busid			= drm_platform_set_busid,

	.get_vblank_counter		= drm_vblank_count,
	.enable_vblank			= xlnx_drm_enable_vblank,
	.disable_vblank			= xlnx_drm_disable_vblank,

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
	.dumb_create			= drm_gem_cma_dumb_create,
	.dumb_map_offset		= drm_gem_cma_dumb_map_offset,
	.dumb_destroy			= drm_gem_dumb_destroy,

	.fops				= &xlnx_drm_fops,

	.name				= DRIVER_NAME,
	.desc				= DRIVER_DESC,
	.date				= DRIVER_DATE,
	.major				= DRIVER_MAJOR,
	.minor				= DRIVER_MINOR,
};

/*
 * Component framework support
 */

static int xlnx_drm_bind(struct device *dev)
{
	return drm_platform_init(&xlnx_driver, to_platform_device(dev));
}

static void xlnx_drm_unbind(struct device *dev)
{
	drm_put_dev(platform_get_drvdata(to_platform_device(dev)));
}

static const struct component_master_ops xlnx_drm_ops = {
	.bind	= xlnx_drm_bind,
	.unbind	= xlnx_drm_unbind,
};


static int compare_of(struct device *dev, void *data)
{
	return dev->of_node == data;
}

static int add_components(struct device *dev, struct component_match **matchptr,
		const char *name)
{
	struct device_node *np = dev->of_node;
	unsigned i;

	for (i = 0; ; i++) {
		struct device_node *node;

		node = of_parse_phandle(np, name, i);
		if (!node)
			break;

		component_match_add(dev, matchptr, compare_of, node);
	}

	return 0;
}

/*
 * Initialization
 */

static int xlnx_drm_platform_probe(struct platform_device *pdev)
{
	struct component_match *match = NULL;

	add_components(&pdev->dev, &match, "subdev");
	return component_master_add_with_match(&pdev->dev, &xlnx_drm_ops, match);
}

static int xlnx_drm_platform_remove(struct platform_device *pdev)
{
	component_master_del(&pdev->dev, &xlnx_drm_ops);
	return 0;
}

static const struct of_device_id xlnx_drm_of_match[] = {
	{ .compatible = "xlnx,drm", },
	{ /* end of table */ },
};
MODULE_DEVICE_TABLE(of, xlnx_drm_of_match);

static struct platform_driver xlnx_drm_private_driver = {
	.probe			= xlnx_drm_platform_probe,
	.remove			= xlnx_drm_platform_remove,
	.driver			= {
		.name		= "xilinx-drm",
		.pm		= &xlnx_drm_pm_ops,
		.of_match_table	= xlnx_drm_of_match,
	},
};

module_platform_driver(xlnx_drm_private_driver);

MODULE_AUTHOR("Xilinx, Inc.");
MODULE_DESCRIPTION("Xilinx DRM KMS Driver");
MODULE_LICENSE("GPL v2");
