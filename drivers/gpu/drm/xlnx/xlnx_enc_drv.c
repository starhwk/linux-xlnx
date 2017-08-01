/*
 * Xilinx DRM enc_drv driver
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
#include <drm/drm_crtc_helper.h>
#include <drm/drm_encoder.h>
#include <drm/drm_encoder_slave.h>
#include <drm/drm_mode.h>
#include <drm/drm_of.h>

#include <linux/component.h>
#include <linux/device.h>
#include <linux/list.h>
#include <linux/of_device.h>

/*
 * Overview
 * --------
 *
 * This encoder driver provides the DRM encoder software layer that can be
 * used by child encoder drivers, such as drm bridge or encoder slave.
 * One instance can handle multiple child encoders with different types.
 */

struct xlnx_enc_drv {
	struct device *dev;
	struct drm_device *drm;
	struct list_head encoders;
};

struct xlnx_enc_bridge {
	struct drm_encoder drm_enc;
	struct drm_bridge *drm_bridge;
};

struct xlnx_enc_slave {
	struct drm_encoder_slave drm_slave;
	struct drm_connector drm_conn;
	void *init_data;
	int (*i2c_enc_init)(struct i2c_client *client,
			    struct drm_device *dev,
			    struct drm_encoder_slave *encoder);
	int (*pdev_enc_init)(struct platform_device *pdev,
			     struct drm_device *dev,
			     struct drm_encoder_slave *encoder);
	struct xlnx_enc *enc;
};

struct xlnx_enc {
	struct device *dev;
	union {
		struct xlnx_enc_bridge bridge;
		struct xlnx_enc_slave slave;
	} d;
	int (*init)(struct xlnx_enc *enc, uint32_t possible_crtcs);
	void (*remove)(struct xlnx_enc *enc);
	struct list_head list;
	struct xlnx_enc_drv *enc_drv;
};

/*
 * Slave functions
 */

/*
 * Slave connector functions
 */

static inline struct xlnx_enc_slave *
drm_conn_to_slave(struct drm_connector *drm_conn)
{
	return container_of(drm_conn, struct xlnx_enc_slave, drm_conn);
}

static int xlnx_con_get_modes(struct drm_connector *drm_conn)
{
	struct xlnx_enc_slave *slave = drm_conn_to_slave(drm_conn);
	struct drm_encoder_slave *drm_slave = &slave->drm_slave;
	struct drm_encoder *drm_enc = &drm_slave->base;
	const struct drm_encoder_slave_funcs *drm_sfuncs =
		drm_slave->slave_funcs;
	int count = 0;

	if (drm_sfuncs->get_modes)
		count = drm_sfuncs->get_modes(drm_enc, drm_conn);

	return count;
}

static int xlnx_con_mode_valid(struct drm_connector *drm_conn,
					   struct drm_display_mode *mode)
{
	struct xlnx_enc_slave *slave = drm_conn_to_slave(drm_conn);
	struct drm_encoder_slave *drm_slave = &slave->drm_slave;
	struct drm_encoder *drm_enc = &drm_slave->base;
	const struct drm_encoder_slave_funcs *drm_sfuncs =
		drm_slave->slave_funcs;
	int ret = MODE_OK;

	if (drm_sfuncs->mode_valid)
		ret = drm_sfuncs->mode_valid(drm_enc, mode);

	return ret;
}

static struct drm_encoder *
xlnx_con_best_encoder(struct drm_connector *drm_conn)
{
	struct xlnx_enc_slave *slave = drm_conn_to_slave(drm_conn);
	struct drm_encoder_slave *drm_slave = &slave->drm_slave;
	struct drm_encoder *drm_enc = &drm_slave->base;

	return drm_enc;
}


static const struct drm_connector_helper_funcs xlnx_conn_slave_helper_funcs = {
	.get_modes	= xlnx_con_get_modes,
	.mode_valid	= xlnx_con_mode_valid,
	.best_encoder	= xlnx_con_best_encoder,
};

static enum drm_connector_status
xlnx_con_detect(struct drm_connector *drm_conn, bool force)
{
	struct xlnx_enc_slave *slave = drm_conn_to_slave(drm_conn);
	struct drm_encoder_slave *drm_slave = &slave->drm_slave;
	struct drm_encoder *drm_enc = &drm_slave->base;
	const struct drm_encoder_slave_funcs *drm_sfuncs =
		drm_slave->slave_funcs;
	enum drm_connector_status status = connector_status_unknown;

	if (drm_sfuncs->detect)
		status = drm_sfuncs->detect(drm_enc, drm_conn);

	/* some connector ignores the first hpd, so try again if forced */
	if (force && (status != connector_status_connected))
		status = drm_sfuncs->detect(drm_enc, drm_conn);

	return status;
}

void xlnx_con_destroy(struct drm_connector *drm_conn)
{
	drm_connector_unregister(drm_conn);
	drm_connector_cleanup(drm_conn);
}

static const struct drm_connector_funcs xlnx_conn_slave_funcs = {
	.dpms		= drm_helper_connector_dpms,
	.fill_modes	= drm_helper_probe_single_connector_modes,
	.detect		= xlnx_con_detect,
	.destroy	= xlnx_con_destroy,
};

/*
 * Slave encoder functions
 */

static inline struct xlnx_enc_slave *
drm_enc_to_slave(struct drm_encoder *drm_enc)
{
	struct drm_encoder_slave *drm_slave = to_encoder_slave(drm_enc);
	return container_of(drm_slave, struct xlnx_enc_slave, drm_slave);
}

static void xlnx_enc_dpms(struct drm_encoder *drm_enc, int dpms)
{
	struct drm_encoder_slave *drm_slave = to_encoder_slave(drm_enc);
	const struct drm_encoder_slave_funcs *drm_sfuncs =
		drm_slave->slave_funcs;

	if (drm_sfuncs->dpms)
		drm_sfuncs->dpms(drm_enc, dpms);
}

static bool
xlnx_enc_mode_fixup(struct drm_encoder *drm_enc,
		    const struct drm_display_mode *mode,
		    struct drm_display_mode *adjusted_mode)
{
	struct drm_encoder_slave *drm_slave = to_encoder_slave(drm_enc);
	const struct drm_encoder_slave_funcs *drm_sfuncs =
		drm_slave->slave_funcs;
	bool ret = true;

	if (drm_sfuncs->mode_fixup)
		ret = drm_sfuncs->mode_fixup(drm_enc, mode, adjusted_mode);

	return ret;
}

static void xlnx_enc_mode_set(struct drm_encoder *drm_enc,
			      struct drm_display_mode *mode,
			      struct drm_display_mode *adjusted_mode)
{
	struct drm_encoder_slave *drm_slave = to_encoder_slave(drm_enc);
	const struct drm_encoder_slave_funcs *drm_sfuncs =
		drm_slave->slave_funcs;

	if (drm_sfuncs->mode_set)
		drm_sfuncs->mode_set(drm_enc, mode, adjusted_mode);
}

static void xlnx_enc_commit(struct drm_encoder *drm_enc)
{
	xlnx_enc_dpms(drm_enc, DRM_MODE_DPMS_ON);
}

static void xlnx_enc_prepare(struct drm_encoder *drm_enc)
{
	xlnx_enc_dpms(drm_enc, DRM_MODE_DPMS_OFF);
}

static struct drm_crtc *
xlnx_enc_get_crtc(struct drm_encoder *drm_enc)
{
	return drm_enc->crtc;
}

static const struct drm_encoder_helper_funcs xlnx_enc_slave_helper_funcs = {
	.dpms		= xlnx_enc_dpms,
	.mode_fixup	= xlnx_enc_mode_fixup,
	.mode_set	= xlnx_enc_mode_set,
	.prepare	= xlnx_enc_prepare,
	.commit		= xlnx_enc_commit,
	.get_crtc	= xlnx_enc_get_crtc,

};

void xlnx_enc_destroy(struct drm_encoder *drm_enc)
{
	struct xlnx_enc_slave *slave = drm_enc_to_slave(drm_enc);

	drm_encoder_cleanup(drm_enc);
	put_device(slave->enc->dev);
}

static const struct drm_encoder_funcs xlnx_enc_slave_funcs = {
	.destroy	= xlnx_enc_destroy,
};

static int
xlnx_enc_drv_slave_init(struct xlnx_enc *enc, uint32_t possible_crtcs)
{
	struct device *dev = enc->dev;
	struct drm_device *drm = enc->enc_drv->drm;
	struct xlnx_enc_slave *slave = &enc->d.slave;
	struct drm_encoder_slave *drm_slave = &slave->drm_slave;
	struct drm_encoder *drm_enc = &drm_slave->base;
	struct drm_connector *drm_conn = &slave->drm_conn;
	int ret;

	/* Use the encoder nanem instead of the type as the type is unknown */
	ret = drm_encoder_init(drm, drm_enc, &xlnx_enc_slave_funcs, 0,
			       "Xlnx slave encoder");
	if (ret) {
		dev_err(dev, "failed to init the DRM encoder\n");
		goto error;
	}
	drm_encoder_helper_add(drm_enc, &xlnx_enc_slave_helper_funcs);

	if (slave->i2c_enc_init)
		ret = slave->i2c_enc_init(slave->init_data, drm, drm_slave);
	else
		ret = slave->pdev_enc_init(slave->init_data, drm, drm_slave);
	if (ret) {
		dev_err(dev, "failed to init slave encoder\n");
		goto error;
	}

	if (!drm_slave->slave_funcs) {
		dev_err(dev, "there's no encoder slave function\n");
		ret = -ENODEV;
		goto error_enc;
	}

	/* Slave connector type is unknown at this point */
	ret = drm_connector_init(drm, drm_conn, &xlnx_conn_slave_funcs,
				 DRM_MODE_CONNECTOR_Unknown);
	if (ret) {
		dev_err(dev, "failed to initialize connector\n");
		goto error_enc;
	}

	drm_connector_helper_add(drm_conn, &xlnx_conn_slave_helper_funcs);
	ret = drm_connector_register(drm_conn);
	if (ret) {
		dev_err(dev, "failed to register a connector\n");
		goto error_con;
	}

	ret = drm_mode_connector_attach_encoder(drm_conn, drm_enc);
	if (ret) {
		dev_err(dev, "failed to attach connector to encoder\n");
		goto error_con_reg;
	}
	slave->enc = enc;

	return 0;

error_con_reg:
	drm_connector_unregister(drm_conn);
error_con:
	drm_connector_cleanup(drm_conn);
error_enc:
	drm_encoder_cleanup(drm_enc);
error:
	return ret;
}

static void xlnx_enc_drv_slave_remove(struct xlnx_enc *enc)
{
	struct xlnx_enc_slave *slave = &enc->d.slave;
	struct drm_encoder_slave *drm_slave = &slave->drm_slave;
	struct drm_encoder *drm_enc = &drm_slave->base;
	struct drm_connector *drm_conn = &slave->drm_conn;

	drm_connector_unregister(drm_conn);
	drm_connector_cleanup(drm_conn);
	drm_encoder_cleanup(drm_enc);
}

static int xlnx_enc_drv_slave_find(struct xlnx_enc *enc, struct device_node *np)
{
	struct device *dev = enc->enc_drv->dev;
	struct i2c_client *i2c_slv;

	i2c_slv = of_find_i2c_device_by_node(np);
	if (i2c_slv && i2c_slv->dev.driver) {
		struct i2c_driver *i2c_drv;
		struct drm_i2c_encoder_driver *drm_i2c_drv;

		i2c_drv = to_i2c_driver(i2c_slv->dev.driver);
		drm_i2c_drv = to_drm_i2c_encoder_driver(i2c_drv);
		if (!drm_i2c_drv) {
			dev_err(dev, "failed to get the i2c slave\n");
			return-EPROBE_DEFER;
		}

		enc->dev = &i2c_slv->dev;
		enc->d.slave.init_data = i2c_slv;
		enc->d.slave.i2c_enc_init = drm_i2c_drv->encoder_init;
	} else {
		struct platform_device *platform_slv;
		struct device_driver *device_drv;
		struct platform_driver *platform_drv;
		struct drm_platform_encoder_driver *drm_platform_drv;

		platform_slv = of_find_device_by_node(np);
		if (!platform_slv)
			return -EPROBE_DEFER;

		device_drv = platform_slv->dev.driver;
		if (!device_drv)
			return -EPROBE_DEFER;

		platform_drv = to_platform_driver(device_drv);
		drm_platform_drv = to_drm_platform_encoder_driver(platform_drv);
		if (!drm_platform_drv) {
			dev_err(dev, "failed to get the platform slave\n");
			return -EPROBE_DEFER;
		}

		enc->dev = &platform_slv->dev;
		enc->d.slave.init_data = platform_slv;
		enc->d.slave.pdev_enc_init = drm_platform_drv->encoder_init;
	}

	return 0;
}

/*
 * Bridge functions
 */

static const struct drm_encoder_helper_funcs xlnx_enc_bridge_helper_funcs = {
};

static const struct drm_encoder_funcs xlnx_enc_bridge_funcs = {
};


static int
xlnx_enc_drv_bridge_init(struct xlnx_enc *enc, uint32_t possible_crtcs)
{
	struct device *dev = enc->dev;
	struct drm_device *drm = enc->enc_drv->drm;
	struct xlnx_enc_bridge *bridge = &enc->d.bridge;
	struct drm_bridge *drm_bridge = bridge->drm_bridge;
	struct drm_encoder *drm_enc = &bridge->drm_enc;
	int ret;

	/* Use the encoder nanem instead of the type as the type is unknown */
	ret = drm_encoder_init(drm, drm_enc, &xlnx_enc_slave_funcs, 0,
			       "Xlnx slave encoder");
	if (ret) {
		dev_err(dev, "failed to init the DRM encoder\n");
		return ret;
	}
	drm_encoder_helper_add(drm_enc, &xlnx_enc_bridge_helper_funcs);
	drm_bridge->encoder = drm_enc;
	drm_enc->bridge = drm_bridge;
	ret = drm_bridge_attach(drm, drm_bridge);
	if (ret) {
		dev_err(dev, "failed to attach the DRM encoder\n");
		drm_encoder_cleanup(drm_enc);
		return ret;
	}

	return 0;
}

static void xlnx_enc_drv_bridge_remove(struct xlnx_enc *enc)
{
	struct xlnx_enc_bridge *bridge = &enc->d.bridge;
	struct drm_bridge *drm_bridge = bridge->drm_bridge;
	struct drm_encoder *drm_enc = &bridge->drm_enc;

	drm_bridge_detach(drm_bridge);
	drm_encoder_cleanup(drm_enc);
}

/*
 * Encoder driver  functions
 */

static void xlnx_enc_drv_remove_encoders(struct xlnx_enc_drv *enc_drv)
{
	struct xlnx_enc *enc, *next;

	list_for_each_entry_safe(enc, next, &enc_drv->encoders, list)
		list_del(&enc->list);
};

static int xlnx_enc_drv_add_slave(struct xlnx_enc_drv *enc_drv)
{
	struct device *dev = enc_drv->dev;
	struct device_node *dev_np = dev->of_node, *slave_np;
	struct xlnx_enc *enc;
	unsigned int i = 0;
	int ret;

	if (!of_find_property(dev_np, "xlnx,slave", NULL)) {
		dev_err(dev, "no xlnx,bridge property in DT\n");
		return 0;
	}

	while ((slave_np = of_parse_phandle(dev_np, "xlnx,slave", i++))) {
		enc = devm_kzalloc(dev, sizeof(*enc), GFP_KERNEL);
		if (!enc)
			return -ENOMEM;
		ret = xlnx_enc_drv_slave_find(enc, slave_np);
		if (ret)
			return ret;
		enc->init = xlnx_enc_drv_slave_init;
		enc->remove = xlnx_enc_drv_slave_remove;
		enc->enc_drv = enc_drv;
		list_add(&enc->list, &enc_drv->encoders);
	}

	return 0;
}

static int xlnx_enc_drv_add_bridge(struct xlnx_enc_drv *enc_drv)
{
	struct device *dev = enc_drv->dev;
	struct device_node *dev_np = dev->of_node, *bridge_np;
	struct xlnx_enc *enc;
	unsigned int i = 0;

	if (!of_find_property(dev_np, "xlnx,bridge", NULL)) {
		dev_err(dev, "no xlnx,bridge property in DT\n");
		return 0;
	}

	while ((bridge_np = of_parse_phandle(dev_np, "xlnx,bridge", i++))) {
		enc = devm_kzalloc(dev, sizeof(*enc), GFP_KERNEL);
		if (!enc) {
			xlnx_enc_drv_remove_encoders(enc_drv);
			return -ENOMEM;
		}
		enc->d.bridge.drm_bridge = of_drm_find_bridge(bridge_np);
		if (!enc->d.bridge.drm_bridge) {
			xlnx_enc_drv_remove_encoders(enc_drv);
			return -EPROBE_DEFER;
		}
		enc->init = xlnx_enc_drv_bridge_init;
		enc->remove = xlnx_enc_drv_bridge_remove;
		enc->enc_drv = enc_drv;
		list_add(&enc->list, &enc_drv->encoders);
	}

	return 0;
}

static int xlnx_enc_drv_bind(struct device *dev, struct device *master,
			     void *data)
{
	struct drm_device *drm = data;
	struct xlnx_enc_drv *enc_drv = dev_get_drvdata(dev);
	struct xlnx_enc *enc;
	uint32_t possible_crtcs;
	int ret;

	possible_crtcs = drm_of_find_possible_crtcs(drm, dev->of_node);
	list_for_each_entry(enc, &enc_drv->encoders, list) {
		ret = enc->init(enc, possible_crtcs);
		if (ret)
			return ret;
	}

	enc_drv->drm = drm;

	return 0;
}

static void xlnx_enc_drv_unbind(struct device *dev, struct device *master,
				void *data)
{
	struct xlnx_enc_drv *enc_drv = dev_get_drvdata(dev);
	struct xlnx_enc *enc;

	list_for_each_entry(enc, &enc_drv->encoders, list)
		enc->remove(enc);
	xlnx_enc_drv_remove_encoders(enc_drv);
}

static const struct component_ops xlnx_enc_drv_component_ops = {
	.bind	= xlnx_enc_drv_bind,
	.unbind	= xlnx_enc_drv_unbind,
};

static int xlnx_enc_drv_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct xlnx_enc_drv *enc_drv;
	int ret;

	enc_drv = devm_kzalloc(dev, sizeof(*enc_drv), GFP_KERNEL);
	if (!enc_drv)
		return -ENOMEM;
	enc_drv->dev = dev;
	platform_set_drvdata(pdev, enc_drv);
	INIT_LIST_HEAD(&enc_drv->encoders);

	ret = xlnx_enc_drv_add_bridge(enc_drv);
	if (ret)
		return ret;

	ret = xlnx_enc_drv_add_slave(enc_drv);
	if (ret)
		return ret;

	return component_add(dev, &xlnx_enc_drv_component_ops);
}

static int xlnx_enc_drv_remove(struct platform_device *pdev)
{
	struct xlnx_enc_drv *enc_drv = platform_get_drvdata(pdev);

	component_del(&pdev->dev, &xlnx_enc_drv_component_ops);
	xlnx_enc_drv_remove_encoders(enc_drv);

	return 0;
}

static const struct of_device_id xlnx_enc_drv_of_match[] = {
	{ .compatible = "xlnx,drm-enc-drv"},
	{ }
};
MODULE_DEVICE_TABLE(of, xlnx_enc_drv_of_match);

static struct platform_driver xlnx_enc_drv_driver = {
	.probe = xlnx_enc_drv_probe,
	.remove = xlnx_enc_drv_remove,
	.driver = {
		.name = "xlnx-drm-enc-drv",
		.owner = THIS_MODULE,
		.of_match_table = xlnx_enc_drv_of_match,
	},
};

module_platform_driver(xlnx_enc_drv_driver);

MODULE_AUTHOR("Xilinx, Inc.");
MODULE_DESCRIPTION("Xilinx Encoder Driver");
MODULE_LICENSE("GPL v2");
