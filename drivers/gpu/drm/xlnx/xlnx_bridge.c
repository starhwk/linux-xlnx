/*
 * Xilinx DRM bridge driver
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

#include "xlnx_bridge.h"
#include "xlnx_drv.h"

/*
 * Overview
 * --------
 *
 * Similar to drm bridge, but this can be used by any DRM driver.
 * The client should call the functions explicitly where it's needed,
 * as opposed to drm bridge functions which are called implicitly
 * by DRM core.
 * One Xlnx bridge can be owned by one driver at a time.
 */

/**
 * struct xlnx_bridge_helper - Xilinx bridge helper
 * @xlnx_bridges: list of Xilinx bridges
 * @lock: lock to protect @xlnx_crtcs
 * @drm: back pointer to DRM core
 */
struct xlnx_bridge_helper {
	struct list_head xlnx_bridges;
	struct mutex lock; /* lock for @xlnx_bridges */
	struct drm_device *drm;
};

/*
 * Internal functions: used by Xlnx DRM
 */

/**
 * xlnx_bridge_helper_init - Initialize the bridge helper
 * @drm: DRM device
 *
 * Allocate and the initialize the bridge helper.
 *
 * Return: struct xlnx_bridge_helper, or -ENOMEM for out of memory
 */
struct xlnx_bridge_helper *xlnx_bridge_helper_init(struct drm_device *drm)
{
	struct xlnx_bridge_helper *helper;

	helper = devm_kzalloc(drm->dev, sizeof(*helper), GFP_KERNEL);
	if (!helper)
		return ERR_PTR(-ENOMEM);

	INIT_LIST_HEAD(&helper->xlnx_bridges);
	mutex_init(&helper->lock);
	helper->drm = drm;

	return helper;
}

/**
 * xlnx_bridge_helper_fini - Release the bridge helper
 * @drm: DRM device
 * @helper: bridge helper
 *
 * Release the bridge helper.
 */
void xlnx_bridge_helper_fini(struct drm_device *drm,
			     struct xlnx_bridge_helper *helper)
{
	if (WARN_ON(helper->drm != drm))
		return;

	if (WARN_ON(!list_empty(&helper->xlnx_bridges)))
		return;

	mutex_destroy(&helper->lock);
	devm_kfree(drm->dev, helper);
}

/*
 * Client functions
 */

/**
 * xlnx_bridge_enable - Enable the bridge
 * @bridge: bridge to enable
 *
 * Enable bridge.
 *
 * Return: 0 on success. -ENOENT if no callback, or return code from callback.
 */
int xlnx_bridge_enable(struct xlnx_bridge *bridge)
{
	if (!bridge)
		return 0;

	if (bridge->enable)
		return bridge->enable(bridge);

	return -ENOENT;
}
EXPORT_SYMBOL(xlnx_bridge_enable);

/**
 * xlnx_bridge_Disable - Disable the bridge
 * @bridge: bridge to disable
 *
 * Disable bridge.
 */
void xlnx_bridge_disable(struct xlnx_bridge *bridge)
{
	if (!bridge)
		return;

	if (bridge->disable)
		bridge->disable(bridge);
}
EXPORT_SYMBOL(xlnx_bridge_disable);

/**
 * xlnx_bridge_set - Set the bridge
 * @bridge: bridge to set
 * @width: width
 * @height: height
 * @format: bus format (ex, MEDIA_BUS_FMT_*);
 *
 * Set the bridge with height / width / format.
 *
 * Return: 0 on success. -ENOENT if no callback, or return code from callback.
 */
int xlnx_bridge_set(struct xlnx_bridge *bridge,
		    u32 width, u32 height, u32 format)
{
	if (!bridge)
		return 0;

	if (bridge->set)
		return bridge->set(bridge, width, height, format);

	return -ENOENT;
}
EXPORT_SYMBOL(xlnx_bridge_set);

/**
 * xlnx_bridge_get_fmts - Get the supported formats
 * @bridge: bridge to set
 * @fmts: pointer to formats
 * @count: pointer to format count
 *
 * Get the list of supported bus formats.
 *
 * Return: 0 on success. -ENOENT if no callback, or return code from callback.
 */
int xlnx_bridge_get_fmts(struct xlnx_bridge *bridge,
			 const u32 **fmts, u32 *count)
{
	if (!bridge)
		return 0;

	if (bridge->get_fmts)
		return bridge->get_fmts(bridge, fmts, count);

	return -ENOENT;
}
EXPORT_SYMBOL(xlnx_bridge_get_fmts);

/**
 * of_xlnx_bridge_get - Get the corresponding Xlnx bridge instance
 * @drm: DRM device
 * @bridge_np: The device node of the bridge device
 *
 * The function walks through the Xlnx bridge list of @drm, and return
 * if any registered bridge matches the device node. The returned
 * bridge will not be accesible by others.
 *
 * Return: the matching Xlnx bridge instance, or NULL
 */
struct xlnx_bridge *of_xlnx_bridge_get(struct drm_device *drm,
				       struct device_node *bridge_np)
{
	struct xlnx_bridge_helper *helper = xlnx_get_bridge_helper(drm);
	struct xlnx_bridge *found = NULL;
	struct xlnx_bridge *bridge;

	mutex_lock(&helper->lock);
	list_for_each_entry(bridge, &helper->xlnx_bridges, list) {
		if (bridge->of_node == bridge_np && !bridge->owned) {
			found = bridge;
			bridge->owned = true;
			break;
		}
	}
	mutex_unlock(&helper->lock);

	return found;
}
EXPORT_SYMBOL_GPL(of_xlnx_bridge_get);

/**
 * of_xlnx_bridge_put - Put the Xlnx bridge instance
 * @drm: DRM device
 * @bridge: Xlnx bridge instance to release
 *
 * Return the @bridge. After this, the bridge will be available for
 * other drivers to use.
 */
void of_xlnx_bridge_put(struct drm_device *drm, struct xlnx_bridge *bridge)
{
	struct xlnx_bridge_helper *helper = xlnx_get_bridge_helper(drm);

	mutex_lock(&helper->lock);
	WARN_ON(!bridge->owned);
	bridge->owned = false;
	mutex_unlock(&helper->lock);
}
EXPORT_SYMBOL_GPL(of_xlnx_bridge_put);

/*
 * Provider functions
 */

/**
 * xlnx_bridge_register - Register the bridge instance
 * @drm: DRM device
 * @bridge: Xlnx bridge instance to register
 *
 * Register @bridge to be available for clients.
 */
void xlnx_bridge_register(struct drm_device *drm, struct xlnx_bridge *bridge)
{
	struct xlnx_bridge_helper *helper = xlnx_get_bridge_helper(drm);

	mutex_lock(&helper->lock);
	WARN_ON(!bridge->of_node);
	bridge->owned = false;
	list_add_tail(&bridge->list, &helper->xlnx_bridges);
	mutex_unlock(&helper->lock);
}
EXPORT_SYMBOL_GPL(xlnx_bridge_register);

/**
 * xlnx_bridge_unregister - Unregister the bridge instance
 * @drm: DRM device
 * @bridge: Xlnx bridge instance to unregister
 *
 * Unregister @bridge. The bridge shouldn't be owned by any client
 * at this point.
 */
void xlnx_bridge_unregister(struct drm_device *drm, struct xlnx_bridge *bridge)
{
	struct xlnx_bridge_helper *helper = xlnx_get_bridge_helper(drm);

	mutex_lock(&helper->lock);
	WARN_ON(bridge->owned);
	list_del(&bridge->list);
	mutex_unlock(&helper->lock);
}
EXPORT_SYMBOL_GPL(xlnx_bridge_unregister);
