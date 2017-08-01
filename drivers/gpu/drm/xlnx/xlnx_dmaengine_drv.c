/*
 * Xilinx DRM CRTC DMA engine driver
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
#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_crtc.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_fourcc.h>

#include <linux/component.h>
#include <linux/device.h>
#include <linux/dmaengine.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_dma.h>
#include <linux/platform_device.h>

#include "xlnx_crtc.h"
#include "xlnx_fb.h"

/*
 * Overview
 * --------
 *
 * This driver intends to support the display pipeline with DMA engine
 * driver by initializing DRM crtc and plane objects.  The driver makes
 * an assumption that it's single plane pipeline, as multi-plane pipeline
 * would require programing beyond the DMA engine interface. Each plane
 * can have up to XLNX_DMA_MAX_CHAN DMA channels to support multi-planar
 * formats.
 */

#define XLNX_DMA_MAX_CHAN	3
/* FIXME: format through API */
#define XLNX_DMA_DEFAULT_FORMAT	DRM_FORMAT_RGB565

struct xlnx_dma_chan {
	struct dma_chan *dma_chan;
	bool is_active;
	struct dma_interleaved_template xt;
	struct data_chunk sgl[1];
};

struct xlnx_dma {
	struct device *dev;
	struct xlnx_crtc xlnx_crtc;
	struct drm_plane plane;
	struct xlnx_dma_chan *chan[XLNX_DMA_MAX_CHAN];
	struct drm_pending_vblank_event *event;
	dma_async_tx_callback callback;
	void *callback_param;
	struct drm_device *drm;
};

/*
 * Xlnx crtc functions
 */

/* TODO: create the common dmaengine helper? */

static inline struct xlnx_dma *crtc_to_dma(struct xlnx_crtc *xlnx_crtc)
{
	return container_of(xlnx_crtc, struct xlnx_dma, xlnx_crtc);
}

static void xlnx_dma_complete(void *param)
{
	struct xlnx_dma *xlnx_dma = param;
	struct drm_device *drm = xlnx_dma->drm;
	struct drm_crtc *crtc = &xlnx_dma->xlnx_crtc.crtc;
	struct drm_pending_vblank_event *event;
	unsigned long flags;

	drm_handle_vblank(drm, 0);

	spin_lock_irqsave(&drm->event_lock, flags);
	event = xlnx_dma->event;
	xlnx_dma->event = NULL;
	if (event) {
		drm_crtc_send_vblank_event(crtc, event);
		drm_crtc_vblank_put(crtc);
	}
	spin_unlock_irqrestore(&drm->event_lock, flags);

}

static int xlnx_dma_enable_vblank(struct xlnx_crtc *xlnx_crtc)
{
	struct xlnx_dma *xlnx_dma = crtc_to_dma(xlnx_crtc);

	/*
	 * Use the complete callback for vblank event assuming the dma engine
	 * starts on the next descriptor upon this event. This may not be safe
	 * assumption for some dma engines.
	 */
	xlnx_dma->callback = xlnx_dma_complete;
	xlnx_dma->callback_param = xlnx_dma;

	return 0;
}

static void xlnx_dma_disable_vblank(struct xlnx_crtc *xlnx_crtc)
{
	struct xlnx_dma *xlnx_dma = crtc_to_dma(xlnx_crtc);

	xlnx_dma->callback = NULL;
	xlnx_dma->callback_param = NULL;
}

static void xlnx_dma_cancel_page_flip(struct xlnx_crtc *xlnx_crtc, struct drm_file *file)
{
	struct xlnx_dma *xlnx_dma = crtc_to_dma(xlnx_crtc);
	struct drm_crtc *crtc = &xlnx_crtc->crtc;
	struct drm_device *drm = crtc->dev;
	struct drm_pending_vblank_event *event;
	unsigned long flags;

	spin_lock_irqsave(&drm->event_lock, flags);
	event = xlnx_dma->event;
	if (event && (event->base.file_priv == file)) {
		xlnx_dma->event = NULL;
		kfree(&event->base);
		drm_crtc_vblank_put(crtc);
	}
	spin_unlock_irqrestore(&drm->event_lock, flags);
}

static uint32_t xlnx_dma_get_format(struct xlnx_crtc *xlnx_crtc)
{
	return XLNX_DMA_DEFAULT_FORMAT;
}

static unsigned int xlnx_dma_get_align(struct xlnx_crtc *xlnx_crtc)
{
	struct xlnx_dma *xlnx_dma = crtc_to_dma(xlnx_crtc);
	return 1 << xlnx_dma->chan[0]->dma_chan->device->copy_align;
}

/*
 * DRM plane functions
 */

static inline struct xlnx_dma *plane_to_dma(struct drm_plane *plane)
{
	return container_of(plane, struct xlnx_dma, plane);
}

static int xlnx_dma_plane_prepare_fb(struct drm_plane *plane,
				 struct drm_plane_state *new_state)
{
	return 0;
}

static void xlnx_dma_plane_cleanup_fb(struct drm_plane *plane,
				  struct drm_plane_state *old_state)
{
	/* TODO: implement */
}

#define XLNX_ATOMIC	0
#if XLNX_ATOMIC
static int xlnx_dma_plane_atomic_check(struct drm_plane *plane,
		struct drm_plane_state *state)
{
	/* TODO: implement */
	return 0;
}

static void xlnx_dma_plane_atomic_update(struct drm_plane *plane,
				     struct drm_plane_state *old_state)
{
	/* TODO: implement */
}

static void xlnx_dma_plane_atomic_disable(struct drm_plane *plane,
				     struct drm_plane_state *old_state)
{
	/* TODO: implement */
}
#endif

static const struct drm_plane_helper_funcs xlnx_dma_plane_helper_funcs = {
	.prepare_fb	= xlnx_dma_plane_prepare_fb,
	.cleanup_fb	= xlnx_dma_plane_cleanup_fb,
#if XLNX_ATOMIC
	.atomic_check	= xlnx_dma_plane_atomic_check,
	.atomic_update	= xlnx_dma_plane_atomic_update,
	.atomic_disable	= xlnx_dma_plane_atomic_disable,
#endif
};

static void xlnx_dma_plane_enable(struct drm_plane *plane)
{
	struct xlnx_dma *xlnx_dma = plane_to_dma(plane);
	struct dma_async_tx_descriptor *desc;
	enum dma_ctrl_flags flags;
	unsigned int i;

	for (i = 0; i < XLNX_DMA_MAX_CHAN; i++) {
		struct xlnx_dma_chan *xlnx_dma_chan = xlnx_dma->chan[i];
		struct dma_chan *dma_chan = xlnx_dma_chan->dma_chan;
		struct dma_interleaved_template *xt = &xlnx_dma_chan->xt;

		if (xlnx_dma_chan && xlnx_dma_chan->is_active) {
			flags = DMA_CTRL_ACK | DMA_PREP_INTERRUPT;
			desc = dmaengine_prep_interleaved_dma(dma_chan, xt,
							      flags);
			if (!desc) {
				dev_err(xlnx_dma->dev, "failed to prepare DMA descriptor\n");
				return;
			}
			desc->callback = xlnx_dma->callback;
			desc->callback_param = xlnx_dma->callback_param;

			dmaengine_submit(desc);
			dma_async_issue_pending(xlnx_dma_chan->dma_chan);
		}
	}
}

static int xlnx_dma_plane_disable(struct drm_plane *plane)
{
	struct xlnx_dma *xlnx_dma = plane_to_dma(plane);
	unsigned int i;

	for (i = 0; i < XLNX_DMA_MAX_CHAN; i++) {
		struct xlnx_dma_chan *xlnx_dma_chan = xlnx_dma->chan[i];
		if (xlnx_dma_chan->dma_chan && xlnx_dma_chan->is_active) {
			dmaengine_terminate_all(xlnx_dma_chan->dma_chan);
		}
	}

	return 0;
}

static int xlnx_dma_plane_mode_set(struct drm_plane *plane,
			      struct drm_framebuffer *fb,
			      int crtc_x, int crtc_y,
			      unsigned int crtc_w, unsigned int crtc_h,
			      uint32_t src_x, uint32_t src_y,
			      uint32_t src_w, uint32_t src_h)
{
	struct xlnx_dma *xlnx_dma = plane_to_dma(plane);
	dma_addr_t paddr;
	size_t offset;
	unsigned int hsub, vsub, i;

	hsub = drm_format_horz_chroma_subsampling(fb->pixel_format);
	vsub = drm_format_vert_chroma_subsampling(fb->pixel_format);

	for (i = 0; i < drm_format_num_planes(fb->pixel_format); i++) {
		struct xlnx_dma_chan *xlnx_dma_chan = xlnx_dma->chan[i];
		unsigned int width = src_w / (i ? hsub : 1);
		unsigned int height = src_h / (i ? vsub : 1);
		unsigned int cpp = drm_format_plane_cpp(fb->pixel_format, i);

		paddr = xlnx_fb_get_paddr(fb, i);
		if (!paddr) {
			dev_err(xlnx_dma->dev, "failed to get a paddr\n");
			return -EINVAL;
		}

		xlnx_dma_chan->xt.numf = height;
		xlnx_dma_chan->sgl[0].size = width * cpp;
		xlnx_dma_chan->sgl[0].icg = fb->pitches[i] -
					   xlnx_dma_chan->sgl[0].size;
		offset = src_x * cpp + src_y * fb->pitches[i];
		offset += fb->offsets[i];
		xlnx_dma_chan->xt.src_start = paddr + offset;
		xlnx_dma_chan->xt.frame_size = 1;
		xlnx_dma_chan->xt.dir = DMA_MEM_TO_DEV;
		xlnx_dma_chan->xt.src_sgl = true;
		xlnx_dma_chan->xt.dst_sgl = false;
		xlnx_dma_chan->is_active = true;
	}

	for (; i < XLNX_DMA_MAX_CHAN; i++)
		xlnx_dma->chan[i]->is_active = false;

	/* TODO: set format */

	return 0;
}

static int xlnx_dma_plane_update(struct drm_plane *plane,
				 struct drm_crtc *crtc,
				 struct drm_framebuffer *fb,
				 int crtc_x, int crtc_y,
				 unsigned int crtc_w, unsigned int crtc_h,
				 uint32_t src_x, uint32_t src_y,
				 uint32_t src_w, uint32_t src_h)
{
	struct xlnx_dma *xlnx_dma = plane_to_dma(plane);
	int ret;

	ret = xlnx_dma_plane_mode_set(plane, fb,
				      crtc_x, crtc_y, crtc_w, crtc_h,
				      src_x >> 16, src_y >> 16,
				      src_w >> 16, src_h >> 16);
	if (ret) {
		dev_err(xlnx_dma->dev, "failed to mode-set a plane\n");
		return ret;
	}

	xlnx_dma_plane_enable(plane);

	return 0;
}

static void xlnx_dma_plane_destroy(struct drm_plane *plane)
{
	/* TODO: disable? */
	drm_plane_cleanup(plane);
}

static struct drm_plane_funcs xlnx_dma_plane_funcs = {
	.update_plane	= xlnx_dma_plane_update,
	.disable_plane	= xlnx_dma_plane_disable,
	.destroy	= xlnx_dma_plane_destroy,
};

/*
 * DRM crtc functions
 */

static inline struct xlnx_dma *drm_crtc_to_dma(struct drm_crtc *crtc)
{
	struct xlnx_crtc *xlnx_crtc = to_xlnx_crtc(crtc);
	return crtc_to_dma(xlnx_crtc);
}

static void xlnx_dma_crtc_dpms(struct drm_crtc *crtc, int dpms)
{
	switch (dpms) {
	case DRM_MODE_DPMS_ON:
		xlnx_dma_plane_enable(crtc->primary);
		return;
	default:
		xlnx_dma_plane_disable(crtc->primary);
		return;
	}
}

static void xlnx_dma_crtc_prepare(struct drm_crtc *crtc)
{
	xlnx_dma_crtc_dpms(crtc, DRM_MODE_DPMS_OFF);
}

static void xlnx_dma_crtc_commit(struct drm_crtc *crtc)
{
	xlnx_dma_crtc_dpms(crtc, DRM_MODE_DPMS_ON);
}

static bool xlnx_dma_crtc_mode_fixup(struct drm_crtc *crtc,
				     const struct drm_display_mode *mode,
				     struct drm_display_mode *adjusted_mode)
{
	/* no op */
	return true;
}

static int xlnx_dma_crtc_mode_set(struct drm_crtc *crtc,
				  struct drm_display_mode *mode,
				  struct drm_display_mode *adjusted_mode,
				  int x, int y,
				  struct drm_framebuffer *old_fb)
{
	struct xlnx_dma *xlnx_dma = drm_crtc_to_dma(crtc);
	int ret;

	ret = xlnx_dma_plane_mode_set(crtc->primary,
				      crtc->primary->fb, 0, 0,
				      adjusted_mode->hdisplay,
				      adjusted_mode->vdisplay,
				      x, y,
				      adjusted_mode->hdisplay,
				      adjusted_mode->vdisplay);
	if (ret) {
		dev_err(xlnx_dma->dev, "failed to mode set a plane\n");
		return ret;
	}

	return 0;
}

static int xlnx_dma_crtc_mode_set_base(struct drm_crtc *crtc,
					 int x, int y,
					 struct drm_framebuffer *fb)
{
	struct xlnx_dma *xlnx_dma = drm_crtc_to_dma(crtc);
	int ret;

	ret = xlnx_dma_plane_mode_set(crtc->primary,
					fb, 0, 0,
					crtc->hwmode.hdisplay,
					crtc->hwmode.vdisplay,
					x, y,
					crtc->hwmode.hdisplay,
					crtc->hwmode.vdisplay);
	if (ret) {
		dev_err(xlnx_dma->dev, "failed to mode set a plane\n");
		return ret;
	}

	xlnx_dma_crtc_commit(crtc);

	return 0;
}

static void xlnx_dma_crtc_load_lut(struct drm_crtc *crtc)
{
	/* no op */
}

#if XLNX_ATOMIC
static int xlnx_dma_crtc_atomic_check(struct drm_crtc *crtc,
				   struct drm_crtc_state *state)
{
	/* TODO: implement */
	return drm_atomic_add_affected_planes(state->state, crtc);
}

static void xlnx_dma_crtc_atomic_begin(struct drm_crtc *crtc,
				    struct drm_crtc_state *old_crtc_state)
{
	/* TODO: implement */
}

static void xlnx_dma_crtc_atomic_flush(struct drm_crtc *crtc,
				    struct drm_crtc_state *old_crtc_state)
{
	/* TODO: implement */
}
#endif

static struct drm_crtc_helper_funcs xlnx_dma_crtc_helper_funcs = {
	.dpms		= xlnx_dma_crtc_dpms,
	.prepare	= xlnx_dma_crtc_prepare,
	.commit		= xlnx_dma_crtc_commit,
	.mode_fixup	= xlnx_dma_crtc_mode_fixup,
	.mode_set	= xlnx_dma_crtc_mode_set,
	.mode_set_base	= xlnx_dma_crtc_mode_set_base,
	.load_lut	= xlnx_dma_crtc_load_lut,
#if XLNX_ATOMIC
	.atomic_check	= xlnx_dma_crtc_atomic_check,
	.atomic_begin	= xlnx_dma_crtc_atomic_begin,
	.atomic_flush	= xlnx_dma_crtc_atomic_flush,
#endif
};

static void xlnx_dma_crtc_destroy(struct drm_crtc *crtc)
{
	/* TODO: disable? */
	drm_crtc_cleanup(crtc);
}

static int xlnx_dma_crtc_page_flip(struct drm_crtc *crtc,
				   struct drm_framebuffer *fb,
				   struct drm_pending_vblank_event *event,
				   uint32_t page_flip_flags)
{
	struct xlnx_dma *xlnx_dma = drm_crtc_to_dma(crtc);
	struct drm_device *drm = crtc->dev;
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&drm->event_lock, flags);
	if (xlnx_dma->event != NULL) {
		spin_unlock_irqrestore(&drm->event_lock, flags);
		return -EBUSY;
	}
	spin_unlock_irqrestore(&drm->event_lock, flags);

	ret = xlnx_dma_plane_mode_set(crtc->primary,
				      fb, 0, 0,
				      crtc->hwmode.hdisplay,
				      crtc->hwmode.vdisplay,
				      crtc->x, crtc->y,
				      crtc->hwmode.hdisplay,
				      crtc->hwmode.vdisplay);
	if (ret) {
		dev_err(xlnx_dma->dev, "failed to mode set a plane\n");
		return ret;
	}

	xlnx_dma_crtc_commit(crtc);
	crtc->primary->fb = fb;

	if (event) {
		event->pipe = 0;
		drm_crtc_vblank_get(crtc);
		spin_lock_irqsave(&drm->event_lock, flags);
		xlnx_dma->event = event;
		spin_unlock_irqrestore(&drm->event_lock, flags);
	}

	return 0;
}

static struct drm_crtc_funcs xlnx_dma_crtc_funcs = {
	.destroy	= xlnx_dma_crtc_destroy,
	.set_config	= drm_crtc_helper_set_config,
	.page_flip	= xlnx_dma_crtc_page_flip,
};

static int xlnx_dma_bind(struct device *dev, struct device *master,
			     void *data)
{
	struct drm_device *drm = data;
	struct xlnx_dma *xlnx_dma = dev_get_drvdata(dev);
	uint32_t fmt = XLNX_DMA_DEFAULT_FORMAT;
	int ret;

	/* FIXME: format needs to come through some API. dmaengine extention? */
	ret = drm_universal_plane_init(drm, &xlnx_dma->plane, 0,
				       &xlnx_dma_plane_funcs, &fmt, 1,
				       DRM_PLANE_TYPE_PRIMARY, NULL);
	if (ret) {
		/* TODO: clean up plane? */
		return ret;
	}

	ret = drm_crtc_init_with_planes(drm, &xlnx_dma->xlnx_crtc.crtc,
					&xlnx_dma->plane, NULL,
					&xlnx_dma_crtc_funcs, NULL);
	if (ret) {
		/* TODO: clean up */
		drm_plane_cleanup(&xlnx_dma->plane);
		return ret;
	}
	drm_crtc_helper_add(&xlnx_dma->xlnx_crtc.crtc, &xlnx_dma_crtc_helper_funcs);

	xlnx_dma->xlnx_crtc.enable_vblank = &xlnx_dma_enable_vblank;
	xlnx_dma->xlnx_crtc.disable_vblank = &xlnx_dma_disable_vblank;
	xlnx_dma->xlnx_crtc.cancel_page_flip = &xlnx_dma_cancel_page_flip;
	xlnx_dma->xlnx_crtc.get_format = &xlnx_dma_get_format;
	xlnx_dma->xlnx_crtc.get_align = &xlnx_dma_get_align;
	xlnx_dma->drm = drm;

	return 0;
}

static void xlnx_dma_unbind(struct device *dev, struct device *master,
				void *data)
{
	struct xlnx_dma *xlnx_dma = dev_get_drvdata(dev);

	drm_plane_cleanup(&xlnx_dma->plane);
	drm_crtc_cleanup(&xlnx_dma->xlnx_crtc.crtc);
}

static const struct component_ops xlnx_dma_component_ops = {
	.bind	= xlnx_dma_bind,
	.unbind	= xlnx_dma_unbind,
};

static int xlnx_dma_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct xlnx_dma *xlnx_dma;
	unsigned int i;
	int ret;

	xlnx_dma = devm_kzalloc(dev, sizeof(*xlnx_dma), GFP_KERNEL);
	if (!xlnx_dma)
		return -ENOMEM;

	for (i = 0; i < XLNX_DMA_MAX_CHAN; i++) {
		struct dma_chan *dma_chan;
		struct xlnx_dma_chan *xlnx_dma_chan;
		char temp[16];

		snprintf(temp, sizeof(temp), "dma%d", i);
		dma_chan = of_dma_request_slave_channel(dev->of_node,
								  "dma");
		if (IS_ERR(dma_chan)) {
			dev_err(dev, "failed to request dma channel\n");
			ret = PTR_ERR(dma_chan);
			return ret;
		}

		xlnx_dma_chan = devm_kzalloc(dev, sizeof(*xlnx_dma_chan), GFP_KERNEL);
		if (!xlnx_dma) {
			/* TODO: clean up dma chan */
			return -ENOMEM;
		}
		xlnx_dma_chan->dma_chan = dma_chan;
		xlnx_dma->chan[i] = xlnx_dma_chan;
	}
	xlnx_dma->dev = dev;
	platform_set_drvdata(pdev, xlnx_dma);

	/* TODO: check return code from component add and  clean up dma chan */
	return component_add(dev, &xlnx_dma_component_ops);
}

static int xlnx_dma_remove(struct platform_device *pdev)
{
	struct xlnx_dma *xlnx_dma = platform_get_drvdata(pdev);
	unsigned int i;

	for (i = 0; i < XLNX_DMA_MAX_CHAN; i++) {
		if (xlnx_dma->chan[i]) {
			/* Make sure the channel is terminated before release */
			dmaengine_terminate_all(xlnx_dma->chan[i]->dma_chan);
			dma_release_channel(xlnx_dma->chan[i]->dma_chan);
		}
	}

	component_del(&pdev->dev, &xlnx_dma_component_ops);

	return 0;
}

static const struct of_device_id xlnx_dma_of_match[] = {
	{ .compatible = "xlnx,drm-dmaengine-drv"},
	{ }
};
MODULE_DEVICE_TABLE(of, xlnx_dma_of_match);

static struct platform_driver xlnx_dma_driver = {
	.probe = xlnx_dma_probe,
	.remove = xlnx_dma_remove,
	.driver = {
		.name = "xlnx-drm-dmaengine-drv",
		.owner = THIS_MODULE,
		.of_match_table = xlnx_dma_of_match,
	},
};

module_platform_driver(xlnx_dma_driver);

MODULE_AUTHOR("Xilinx, Inc.");
MODULE_DESCRIPTION("Xilinx DRM DMA engine Driver");
MODULE_LICENSE("GPL v2");
