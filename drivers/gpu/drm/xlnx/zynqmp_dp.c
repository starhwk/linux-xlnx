/*
 * ZynqMP DisplayPort Driver
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
#include <drm/drm_atomic_helper.h>
#include <drm/drm_crtc.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_dp_helper.h>
#include <drm/drm_of.h>

#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/phy/phy.h>
#include <linux/phy/phy-zynqmp.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/uaccess.h>

#include "zynqmp_disp.h"
#include "zynqmp_dpsub.h"

static uint zynqmp_dp_aux_timeout_ms = 50;
module_param_named(aux_timeout_ms, zynqmp_dp_aux_timeout_ms, uint, 0444);
MODULE_PARM_DESC(aux_timeout_ms, "DP aux timeout value in msec (default: 50)");

/*
 * Some sink with long horizontal front porch can cause some issue
 * with vsync timing. This is to debug the issue and give users some control
 * until it's root-caused. For example, setting this to 400 fixes some problem.
 */
static int zynqmp_dp_debug_hfp = INT_MAX;
module_param_named(debug_hfp, zynqmp_dp_debug_hfp, int, 0644);
MODULE_PARM_DESC(debug_hfp, "horizontal front porch debug");

/* Link configuration registers */
#define ZYNQMP_DP_TX_LINK_BW_SET			0x0
#define ZYNQMP_DP_TX_LANE_CNT_SET			0x4
#define ZYNQMP_DP_TX_ENHANCED_FRAME_EN			0x8
#define ZYNQMP_DP_TX_TRAINING_PATTERN_SET		0xc
#define ZYNQMP_DP_TX_SCRAMBLING_DISABLE			0x14
#define ZYNQMP_DP_TX_DOWNSPREAD_CTL			0x18
#define ZYNQMP_DP_TX_SW_RESET				0x1c
#define ZYNQMP_DP_TX_SW_RESET_STREAM1			BIT(0)
#define ZYNQMP_DP_TX_SW_RESET_STREAM2			BIT(1)
#define ZYNQMP_DP_TX_SW_RESET_STREAM3			BIT(2)
#define ZYNQMP_DP_TX_SW_RESET_STREAM4			BIT(3)
#define ZYNQMP_DP_TX_SW_RESET_AUX			BIT(7)
#define ZYNQMP_DP_TX_SW_RESET_ALL			(ZYNQMP_DP_TX_SW_RESET_STREAM1 | \
							 ZYNQMP_DP_TX_SW_RESET_STREAM2 | \
							 ZYNQMP_DP_TX_SW_RESET_STREAM3 | \
							 ZYNQMP_DP_TX_SW_RESET_STREAM4 | \
							 ZYNQMP_DP_TX_SW_RESET_AUX)

/* Core enable registers */
#define ZYNQMP_DP_TX_ENABLE				0x80
#define ZYNQMP_DP_TX_ENABLE_MAIN_STREAM			0x84
#define ZYNQMP_DP_TX_FORCE_SCRAMBLER_RESET		0xc0
#define ZYNQMP_DP_TX_VERSION				0xf8
#define ZYNQMP_DP_TX_VERSION_MAJOR_MASK			GENMASK(31, 24)
#define ZYNQMP_DP_TX_VERSION_MAJOR_SHIFT		24
#define ZYNQMP_DP_TX_VERSION_MINOR_MASK			GENMASK(23, 16)
#define ZYNQMP_DP_TX_VERSION_MINOR_SHIFT		16
#define ZYNQMP_DP_TX_VERSION_REVISION_MASK		GENMASK(15, 12)
#define ZYNQMP_DP_TX_VERSION_REVISION_SHIFT		12
#define ZYNQMP_DP_TX_VERSION_PATCH_MASK			GENMASK(11, 8)
#define ZYNQMP_DP_TX_VERSION_PATCH_SHIFT		8
#define ZYNQMP_DP_TX_VERSION_INTERNAL_MASK		GENMASK(7, 0)
#define ZYNQMP_DP_TX_VERSION_INTERNAL_SHIFT		0

/* Core ID registers */
#define ZYNQMP_DP_TX_CORE_ID				0xfc
#define ZYNQMP_DP_TX_CORE_ID_MAJOR_MASK			GENMASK(31, 24)
#define ZYNQMP_DP_TX_CORE_ID_MAJOR_SHIFT		24
#define ZYNQMP_DP_TX_CORE_ID_MINOR_MASK			GENMASK(23, 16)
#define ZYNQMP_DP_TX_CORE_ID_MINOR_SHIFT		16
#define ZYNQMP_DP_TX_CORE_ID_REVISION_MASK		GENMASK(15, 8)
#define ZYNQMP_DP_TX_CORE_ID_REVISION_SHIFT		8
#define ZYNQMP_DP_TX_CORE_ID_DIRECTION			GENMASK(1)

/* AUX channel interface registers */
#define ZYNQMP_DP_TX_AUX_COMMAND			0x100
#define ZYNQMP_DP_TX_AUX_COMMAND_CMD_SHIFT		8
#define ZYNQMP_DP_TX_AUX_COMMAND_ADDRESS_ONLY		BIT(12)
#define ZYNQMP_DP_TX_AUX_COMMAND_BYTES_SHIFT		0
#define ZYNQMP_DP_TX_AUX_WRITE_FIFO			0x104
#define ZYNQMP_DP_TX_AUX_ADDRESS			0x108
#define ZYNQMP_DP_TX_CLK_DIVIDER			0x10c
#define ZYNQMP_DP_TX_CLK_DIVIDER_MHZ			1000000
#define ZYNQMP_DP_TX_CLK_DIVIDER_AUX_FILTER_SHIFT	8
#define ZYNQMP_DP_TX_INTR_SIGNAL_STATE			0x130
#define ZYNQMP_DP_TX_INTR_SIGNAL_STATE_HPD		BIT(0)
#define ZYNQMP_DP_TX_INTR_SIGNAL_STATE_REQUEST		BIT(1)
#define ZYNQMP_DP_TX_INTR_SIGNAL_STATE_REPLY		BIT(2)
#define ZYNQMP_DP_TX_INTR_SIGNAL_STATE_REPLY_TIMEOUT	BIT(3)
#define ZYNQMP_DP_TX_AUX_REPLY_DATA			0x134
#define ZYNQMP_DP_TX_AUX_REPLY_CODE			0x138
#define ZYNQMP_DP_TX_AUX_REPLY_CODE_AUX_ACK		(0)
#define ZYNQMP_DP_TX_AUX_REPLY_CODE_AUX_NACK		BIT(0)
#define ZYNQMP_DP_TX_AUX_REPLY_CODE_AUX_DEFER		BIT(1)
#define ZYNQMP_DP_TX_AUX_REPLY_CODE_I2C_ACK		(0)
#define ZYNQMP_DP_TX_AUX_REPLY_CODE_I2C_NACK		BIT(2)
#define ZYNQMP_DP_TX_AUX_REPLY_CODE_I2C_DEFER		BIT(3)
#define ZYNQMP_DP_TX_AUX_REPLY_CNT			0x13c
#define ZYNQMP_DP_TX_AUX_REPLY_CNT_MASK			0xff
#define ZYNQMP_DP_TX_INTR_STATUS			0x140
#define ZYNQMP_DP_TX_INTR_MASK				0x144
#define ZYNQMP_DP_TX_INTR_HPD_IRQ			BIT(0)
#define ZYNQMP_DP_TX_INTR_HPD_EVENT			BIT(1)
#define ZYNQMP_DP_TX_INTR_REPLY_RECV			BIT(2)
#define ZYNQMP_DP_TX_INTR_REPLY_TIMEOUT			BIT(3)
#define ZYNQMP_DP_TX_INTR_HPD_PULSE			BIT(4)
#define ZYNQMP_DP_TX_INTR_EXT_PKT_TXD			BIT(5)
#define ZYNQMP_DP_TX_INTR_LIV_ABUF_UNDRFLW		BIT(12)
#define ZYNQMP_DP_TX_INTR_VBLANK_START			BIT(13)
#define ZYNQMP_DP_TX_INTR_PIXEL0_MATCH			BIT(14)
#define ZYNQMP_DP_TX_INTR_PIXEL1_MATCH			BIT(15)
#define ZYNQMP_DP_TX_INTR_CHBUF_UNDERFLW_MASK		0x3f0000
#define ZYNQMP_DP_TX_INTR_CHBUF_OVERFLW_MASK		0xfc00000
#define ZYNQMP_DP_TX_INTR_CUST_TS_2			BIT(28)
#define ZYNQMP_DP_TX_INTR_CUST_TS			BIT(29)
#define ZYNQMP_DP_TX_INTR_EXT_VSYNC_TS			BIT(30)
#define ZYNQMP_DP_TX_INTR_VSYNC_TS			BIT(31)
#define ZYNQMP_DP_TX_INTR_ALL				(ZYNQMP_DP_TX_INTR_HPD_IRQ | \
							 ZYNQMP_DP_TX_INTR_HPD_EVENT | \
							 ZYNQMP_DP_TX_INTR_REPLY_RECV | \
							 ZYNQMP_DP_TX_INTR_REPLY_TIMEOUT | \
							 ZYNQMP_DP_TX_INTR_HPD_PULSE | \
							 ZYNQMP_DP_TX_INTR_EXT_PKT_TXD | \
							 ZYNQMP_DP_TX_INTR_LIV_ABUF_UNDRFLW | \
							 ZYNQMP_DP_TX_INTR_CHBUF_UNDERFLW_MASK | \
							 ZYNQMP_DP_TX_INTR_CHBUF_OVERFLW_MASK)
#define ZYNQMP_DP_TX_NO_INTR_ALL			(ZYNQMP_DP_TX_INTR_PIXEL0_MATCH | \
							 ZYNQMP_DP_TX_INTR_PIXEL1_MATCH | \
							 ZYNQMP_DP_TX_INTR_CUST_TS_2 | \
							 ZYNQMP_DP_TX_INTR_CUST_TS | \
							 ZYNQMP_DP_TX_INTR_EXT_VSYNC_TS | \
							 ZYNQMP_DP_TX_INTR_VSYNC_TS)
#define ZYNQMP_DP_TX_REPLY_DATA_CNT			0x148
#define ZYNQMP_DP_SUB_TX_INTR_STATUS			0x3a0
#define ZYNQMP_DP_SUB_TX_INTR_MASK			0x3a4
#define ZYNQMP_DP_SUB_TX_INTR_EN			0x3a8
#define ZYNQMP_DP_SUB_TX_INTR_DS			0x3ac

/* Main stream attribute registers */
#define ZYNQMP_DP_TX_MAIN_STREAM_HTOTAL			0x180
#define ZYNQMP_DP_TX_MAIN_STREAM_VTOTAL			0x184
#define ZYNQMP_DP_TX_MAIN_STREAM_POLARITY		0x188
#define ZYNQMP_DP_TX_MAIN_STREAM_POLARITY_HSYNC_SHIFT	0
#define ZYNQMP_DP_TX_MAIN_STREAM_POLARITY_VSYNC_SHIFT	1
#define ZYNQMP_DP_TX_MAIN_STREAM_HSWIDTH		0x18c
#define ZYNQMP_DP_TX_MAIN_STREAM_VSWIDTH		0x190
#define ZYNQMP_DP_TX_MAIN_STREAM_HRES			0x194
#define ZYNQMP_DP_TX_MAIN_STREAM_VRES			0x198
#define ZYNQMP_DP_TX_MAIN_STREAM_HSTART			0x19c
#define ZYNQMP_DP_TX_MAIN_STREAM_VSTART			0x1a0
#define ZYNQMP_DP_TX_MAIN_STREAM_MISC0			0x1a4
#define ZYNQMP_DP_TX_MAIN_STREAM_MISC0_SYNC		BIT(0)
#define ZYNQMP_DP_TX_MAIN_STREAM_MISC0_FORMAT_SHIFT	1
#define ZYNQMP_DP_TX_MAIN_STREAM_MISC0_DYNAMIC_RANGE	BIT(3)
#define ZYNQMP_DP_TX_MAIN_STREAM_MISC0_YCBCR_COLRIMETRY	BIT(4)
#define ZYNQMP_DP_TX_MAIN_STREAM_MISC0_BPC_SHIFT	5
#define ZYNQMP_DP_TX_MAIN_STREAM_MISC1			0x1a8
#define ZYNQMP_DP_TX_MAIN_STREAM_MISC0_INTERLACED_VERT	BIT(0)
#define ZYNQMP_DP_TX_MAIN_STREAM_MISC0_STEREO_VID_SHIFT	1
#define ZYNQMP_DP_TX_M_VID				0x1ac
#define ZYNQMP_DP_TX_TRANSFER_UNIT_SIZE			0x1b0
#define ZYNQMP_DP_TX_DEF_TRANSFER_UNIT_SIZE		64
#define ZYNQMP_DP_TX_N_VID				0x1b4
#define ZYNQMP_DP_TX_USER_PIXEL_WIDTH			0x1b8
#define ZYNQMP_DP_TX_USER_DATA_CNT_PER_LANE		0x1bc
#define ZYNQMP_DP_TX_MIN_BYTES_PER_TU			0x1c4
#define ZYNQMP_DP_TX_FRAC_BYTES_PER_TU			0x1c8
#define ZYNQMP_DP_TX_INIT_WAIT				0x1cc

/* PHY configuration and status registers */
#define ZYNQMP_DP_TX_PHY_CONFIG				0x200
#define ZYNQMP_DP_TX_PHY_CONFIG_PHY_RESET		BIT(0)
#define ZYNQMP_DP_TX_PHY_CONFIG_GTTX_RESET		BIT(1)
#define ZYNQMP_DP_TX_PHY_CONFIG_PHY_PMA_RESET		BIT(8)
#define ZYNQMP_DP_TX_PHY_CONFIG_PHY_PCS_RESET		BIT(9)
#define ZYNQMP_DP_TX_PHY_CONFIG_ALL_RESET		(ZYNQMP_DP_TX_PHY_CONFIG_PHY_RESET | \
							 ZYNQMP_DP_TX_PHY_CONFIG_GTTX_RESET | \
							 ZYNQMP_DP_TX_PHY_CONFIG_PHY_PMA_RESET | \
							 ZYNQMP_DP_TX_PHY_CONFIG_PHY_PCS_RESET)
#define ZYNQMP_DP_TX_PHY_PREEMPHASIS_LANE_0		0x210
#define ZYNQMP_DP_TX_PHY_PREEMPHASIS_LANE_1		0x214
#define ZYNQMP_DP_TX_PHY_PREEMPHASIS_LANE_2		0x218
#define ZYNQMP_DP_TX_PHY_PREEMPHASIS_LANE_3		0x21c
#define ZYNQMP_DP_TX_PHY_VOLTAGE_DIFF_LANE_0		0x220
#define ZYNQMP_DP_TX_PHY_VOLTAGE_DIFF_LANE_1		0x224
#define ZYNQMP_DP_TX_PHY_VOLTAGE_DIFF_LANE_2		0x228
#define ZYNQMP_DP_TX_PHY_VOLTAGE_DIFF_LANE_3		0x22c
#define ZYNQMP_DP_TX_PHY_CLOCK_FEEDBACK_SETTING		0x234
#define ZYNQMP_DP_TX_PHY_CLOCK_FEEDBACK_SETTING_162	0x1
#define ZYNQMP_DP_TX_PHY_CLOCK_FEEDBACK_SETTING_270	0x3
#define ZYNQMP_DP_TX_PHY_CLOCK_FEEDBACK_SETTING_540	0x5
#define ZYNQMP_DP_TX_PHY_POWER_DOWN			0x238
#define ZYNQMP_DP_TX_PHY_POWER_DOWN_LANE_0		BIT(0)
#define ZYNQMP_DP_TX_PHY_POWER_DOWN_LANE_1		BIT(1)
#define ZYNQMP_DP_TX_PHY_POWER_DOWN_LANE_2		BIT(2)
#define ZYNQMP_DP_TX_PHY_POWER_DOWN_LANE_3		BIT(3)
#define ZYNQMP_DP_TX_PHY_POWER_DOWN_ALL			0xf
#define ZYNQMP_DP_TX_PHY_PRECURSOR_LANE_0		0x23c
#define ZYNQMP_DP_TX_PHY_PRECURSOR_LANE_1		0x240
#define ZYNQMP_DP_TX_PHY_PRECURSOR_LANE_2		0x244
#define ZYNQMP_DP_TX_PHY_PRECURSOR_LANE_3		0x248
#define ZYNQMP_DP_TX_PHY_POSTCURSOR_LANE_0		0x24c
#define ZYNQMP_DP_TX_PHY_POSTCURSOR_LANE_1		0x250
#define ZYNQMP_DP_TX_PHY_POSTCURSOR_LANE_2		0x254
#define ZYNQMP_DP_TX_PHY_POSTCURSOR_LANE_3		0x258
#define ZYNQMP_DP_SUB_TX_PHY_PRECURSOR_LANE_0		0x24c
#define ZYNQMP_DP_SUB_TX_PHY_PRECURSOR_LANE_1		0x250
#define ZYNQMP_DP_TX_PHY_STATUS				0x280
#define ZYNQMP_DP_TX_PHY_STATUS_PLL_LOCKED_SHIFT	4
#define ZYNQMP_DP_TX_PHY_STATUS_FPGA_PLL_LOCKED		BIT(6)

/* Audio registers */
#define ZYNQMP_DP_TX_AUDIO_CONTROL			0x300
#define ZYNQMP_DP_TX_AUDIO_CHANNELS			0x304
#define ZYNQMP_DP_TX_AUDIO_INFO_DATA			0x308
#define ZYNQMP_DP_TX_AUDIO_M_AUD			0x328
#define ZYNQMP_DP_TX_AUDIO_N_AUD			0x32c
#define ZYNQMP_DP_TX_AUDIO_EXT_DATA			0x330

#define ZYNQMP_DP_MISC0_RGB				(0)
#define ZYNQMP_DP_MISC0_YCRCB_422			(5 << 1)
#define ZYNQMP_DP_MISC0_YCRCB_444			(6 << 1)
#define ZYNQMP_DP_MISC0_FORMAT_MASK			0xe
#define ZYNQMP_DP_MISC0_BPC_6				(0 << 5)
#define ZYNQMP_DP_MISC0_BPC_8				(1 << 5)
#define ZYNQMP_DP_MISC0_BPC_10				(2 << 5)
#define ZYNQMP_DP_MISC0_BPC_12				(3 << 5)
#define ZYNQMP_DP_MISC0_BPC_16				(4 << 5)
#define ZYNQMP_DP_MISC0_BPC_MASK			0xe0
#define ZYNQMP_DP_MISC1_Y_ONLY				(1 << 7)

#define ZYNQMP_DP_MAX_LANES				2
#define ZYNQMP_MAX_FREQ					3000000

#define DP_REDUCED_BIT_RATE				162000
#define DP_HIGH_BIT_RATE				270000
#define DP_HIGH_BIT_RATE2				540000
#define DP_MAX_TRAINING_TRIES				5
#define DP_V1_2						0x12

/**
 * struct zynqmp_dp_link_config - Common link config between source and sink
 * @max_rate: maximum link rate
 * @max_lanes: maximum number of lanes
 */
struct zynqmp_dp_link_config {
	int max_rate;
	u8 max_lanes;
};

/**
 * struct zynqmp_dp_mode - Configured mode of DisplayPort
 * @bw_code: code for bandwidth(link rate)
 * @lane_cnt: number of lanes
 * @pclock: pixel clock frequency of current mode
 * @fmt: format identifier string
 */
struct zynqmp_dp_mode {
	u8 bw_code;
	u8 lane_cnt;
	int pclock;
	const char *fmt;
};

/**
 * struct zynqmp_dp_config - Configuration of DisplayPort from DTS
 * @misc0: misc0 configuration (per DP v1.2 spec)
 * @misc1: misc1 configuration (per DP v1.2 spec)
 * @bpp: bits per pixel
 * @bpc: bits per component
 * @num_colors: number of color components
 */
struct zynqmp_dp_config {
	u8 misc0;
	u8 misc1;
	u8 bpp;
	u8 bpc;
	u8 num_colors;
};

/**
 * struct zynqmp_dp - Xilinx DisplayPort core
 * @encoder: the drm encoder structure
 * @connector: the drm connector structure
 * @sync_prop: synchronous mode property
 * @bpc_prop: bpc mode property
 * @dev: device structure
 * @dpsub: Display subsystem
 * @drm: DRM core
 * @iomem: device I/O memory for register access
 * @config: IP core configuration from DTS
 * @aux: aux channel
 * @phy: PHY handles for DP lanes
 * @hpd_work: hot plug detection worker
 * @dpms: current dpms state
 * @dpcd: DP configuration data from currently connected sink device
 * @link_config: common link configuration between IP core and sink device
 * @mode: current mode between IP core and sink device
 * @train_set: set of training data
 */
struct zynqmp_dp {
	struct drm_encoder encoder;
	struct drm_connector connector;
	struct drm_property *sync_prop;
	struct drm_property *bpc_prop;
	struct device *dev;
	struct zynqmp_dpsub *dpsub;
	struct drm_device *drm;
	void __iomem *iomem;

	struct zynqmp_dp_config config;
	struct drm_dp_aux aux;
	struct phy *phy[ZYNQMP_DP_MAX_LANES];
	struct delayed_work hpd_work;

	int dpms;
	u8 dpcd[DP_RECEIVER_CAP_SIZE];
	struct zynqmp_dp_link_config link_config;
	struct zynqmp_dp_mode mode;
	u8 train_set[ZYNQMP_DP_MAX_LANES];
};

static inline struct zynqmp_dp *encoder_to_dp(struct drm_encoder *encoder)
{
	return container_of(encoder, struct zynqmp_dp, encoder);
}

static inline struct zynqmp_dp *connector_to_dp(struct drm_connector *connector)
{
	return container_of(connector, struct zynqmp_dp, connector);
}

static void zynqmp_dp_write(void __iomem *base, int offset, u32 val)
{
	writel(val, base + offset);
}

static u32 zynqmp_dp_read(void __iomem *base, int offset)
{
	return readl(base + offset);
}

static void zynqmp_dp_clr(void __iomem *base, int offset, u32 clr)
{
	zynqmp_dp_write(base, offset, zynqmp_dp_read(base, offset) & ~clr);
}

static void zynqmp_dp_set(void __iomem *base, int offset, u32 set)
{
	zynqmp_dp_write(base, offset, zynqmp_dp_read(base, offset) | set);
}

/*
 * Debugfs functions
 */

#ifdef CONFIG_DRM_ZYNQMP_DP_DEBUG_FS

#define ZYNQMP_DP_DEBUGFS_READ_MAX_SIZE	32UL
#define ZYNQMP_DP_DEBUGFS_UINT8_MAX_STR	"255"
#define IN_RANGE(x, min, max) ({		\
		typeof(x) _x = (x);		\
		_x >= (min) && _x <= (max); })

/* Match zynqmp_dp_testcases vs debugfs_reqs[] entry */
enum zynqmp_dp_testcases {
	DP_TC_LINK_RATE,
	DP_TC_LANE_COUNT,
	DP_TC_OUTPUT_FMT,
	DP_TC_NONE
};

struct zynqmp_dp_debugfs {
	enum zynqmp_dp_testcases testcase;
	u8 link_rate;
	u8 lane_cnt;
	u8 old_output_fmt;
	struct zynqmp_dp *dp;
};

static struct dentry *zynqmp_dp_debugfs_dir;
static struct zynqmp_dp_debugfs dp_debugfs;
struct zynqmp_dp_debugfs_request {
	const char *req;
	enum zynqmp_dp_testcases tc;
	ssize_t (*read_handler)(char **kern_buff);
	ssize_t (*write_handler)(char **cmd);
};

static s64 zynqmp_dp_debugfs_argument_value(char *arg)
{
	s64 value;

	if (!arg)
		return -1;

	if (!kstrtos64(arg, 0, &value))
		return value;

	return -1;
}

static int zynqmp_dp_update_output_format(u8 output_fmt, u32 num_colors)
{
	struct zynqmp_dp *dp = dp_debugfs.dp;
	struct zynqmp_dp_config *config = &dp->config;
	u32 bpc;
	u8 bpc_bits = (config->misc0 & ZYNQMP_DP_MISC0_BPC_MASK);
	bool misc1 = output_fmt & ZYNQMP_DP_MISC1_Y_ONLY ? true : false;

	switch (bpc_bits) {
	case ZYNQMP_DP_MISC0_BPC_6:
		bpc = 6;
		break;
	case ZYNQMP_DP_MISC0_BPC_8:
		bpc = 8;
		break;
	case ZYNQMP_DP_MISC0_BPC_10:
		bpc = 10;
		break;
	case ZYNQMP_DP_MISC0_BPC_12:
		bpc = 12;
		break;
	case ZYNQMP_DP_MISC0_BPC_16:
		bpc = 16;
		break;
	default:
		dev_err(dp->dev, "Invalid bpc count for misc0\n");
		return -EINVAL;
	}

	/* clear old format */
	config->misc0 &= ~ZYNQMP_DP_MISC0_FORMAT_MASK;
	config->misc1 &= ~ZYNQMP_DP_MISC1_Y_ONLY;

	if (misc1) {
		config->misc1 |= output_fmt;
		zynqmp_dp_write(dp->iomem, ZYNQMP_DP_TX_MAIN_STREAM_MISC1,
				config->misc1);
	} else {
		config->misc0 |= output_fmt;
		zynqmp_dp_write(dp->iomem, ZYNQMP_DP_TX_MAIN_STREAM_MISC0,
				config->misc0);
	}
	config->bpp = num_colors * bpc;

	return 0;
}

static ssize_t zynqmp_dp_debugfs_max_linkrate_write(char **dp_test_arg)
{
	char *link_rate_arg;
	s64 link_rate;

	link_rate_arg = strsep(dp_test_arg, " ");
	link_rate = zynqmp_dp_debugfs_argument_value(link_rate_arg);
	if (link_rate < 0 || (link_rate != DP_HIGH_BIT_RATE2 &&
			      link_rate != DP_HIGH_BIT_RATE &&
			      link_rate != DP_REDUCED_BIT_RATE))
		return -EINVAL;

	dp_debugfs.link_rate = drm_dp_link_rate_to_bw_code(link_rate);
	dp_debugfs.testcase = DP_TC_LINK_RATE;

	return 0;
}

static ssize_t zynqmp_dp_debugfs_max_lanecnt_write(char **dp_test_arg)
{
	char *lane_cnt_arg;
	s64 lane_count;

	lane_cnt_arg = strsep(dp_test_arg, " ");
	lane_count = zynqmp_dp_debugfs_argument_value(lane_cnt_arg);
	if (lane_count < 0 || !IN_RANGE(lane_count, 1,
					ZYNQMP_DP_MAX_LANES))
		return -EINVAL;

	dp_debugfs.lane_cnt = lane_count;
	dp_debugfs.testcase = DP_TC_LANE_COUNT;

	return 0;
}

static ssize_t zynqmp_dp_debugfs_output_display_format_write(char **dp_test_arg)
{
	int ret;
	struct zynqmp_dp *dp = dp_debugfs.dp;
	char *output_format;
	u8 output_fmt;
	u32 num_colors;

	/* Read the value from an user value */
	output_format = strsep(dp_test_arg, " ");

	if (strncmp(output_format, "rgb", 3) == 0) {
		output_fmt = ZYNQMP_DP_MISC0_RGB;
		num_colors = 3;
	} else if (strncmp(output_format, "ycbcr422", 8) == 0) {
		output_fmt = ZYNQMP_DP_MISC0_YCRCB_422;
		num_colors = 2;
	} else if (strncmp(output_format, "ycbcr444", 8) == 0) {
		output_fmt = ZYNQMP_DP_MISC0_YCRCB_444;
		num_colors = 3;
	} else if (strncmp(output_format, "yonly", 5) == 0) {
		output_fmt = ZYNQMP_DP_MISC1_Y_ONLY;
		num_colors = 1;
	} else {
		dev_err(dp->dev, "Invalid output format\n");
		return -EINVAL;
	}

	if (dp->config.misc1 & ZYNQMP_DP_MISC1_Y_ONLY)
		dp_debugfs.old_output_fmt = ZYNQMP_DP_MISC1_Y_ONLY;
	else
		dp_debugfs.old_output_fmt = dp->config.misc0 &
					    ZYNQMP_DP_MISC0_FORMAT_MASK;

	ret = zynqmp_dp_update_output_format(output_fmt, num_colors);
	if (!ret)
		dp_debugfs.testcase = DP_TC_OUTPUT_FMT;
	return ret;
}

static ssize_t zynqmp_dp_debugfs_max_linkrate_read(char **kern_buff)
{
	struct zynqmp_dp *dp = dp_debugfs.dp;
	size_t output_str_len;
	u8 dpcd_link_bw;
	int ret;

	dp_debugfs.testcase = DP_TC_NONE;
	dp_debugfs.link_rate = 0;

	/* Getting Sink Side Link Rate */
	ret = drm_dp_dpcd_readb(&dp->aux, DP_LINK_BW_SET, &dpcd_link_bw);
	if (ret < 0) {
		dev_err(dp->dev, "Failed to read link rate via AUX.\n");
		kfree(*kern_buff);
		return ret;
	}

	output_str_len = strlen(ZYNQMP_DP_DEBUGFS_UINT8_MAX_STR);
	output_str_len = min(ZYNQMP_DP_DEBUGFS_READ_MAX_SIZE, output_str_len);
	snprintf(*kern_buff, output_str_len, "%u", dpcd_link_bw);

	return 0;
}

static ssize_t zynqmp_dp_debugfs_max_lanecnt_read(char **kern_buff)
{
	struct zynqmp_dp *dp = dp_debugfs.dp;
	size_t output_str_len;
	u8 dpcd_lane_cnt;
	int ret;

	dp_debugfs.testcase = DP_TC_NONE;
	dp_debugfs.lane_cnt = 0;

	/* Getting Sink Side Lane Count */
	ret = drm_dp_dpcd_readb(&dp->aux, DP_LANE_COUNT_SET, &dpcd_lane_cnt);
	if (ret < 0) {
		dev_err(dp->dev, "Failed to read link rate via AUX.\n");
		kfree(*kern_buff);
		return ret;
	}

	dpcd_lane_cnt &= DP_LANE_COUNT_MASK;
	output_str_len = strlen(ZYNQMP_DP_DEBUGFS_UINT8_MAX_STR);
	output_str_len = min(ZYNQMP_DP_DEBUGFS_READ_MAX_SIZE, output_str_len);
	snprintf(*kern_buff, output_str_len, "%u", dpcd_lane_cnt);

	return 0;
}

static ssize_t
zynqmp_dp_debugfs_output_display_format_read(char **kern_buff)
{
	int ret;
	struct zynqmp_dp *dp = dp_debugfs.dp;
	u8 old_output_fmt = dp_debugfs.old_output_fmt;
	size_t output_str_len;
	u32 num_colors;

	dp_debugfs.testcase = DP_TC_NONE;

	if (old_output_fmt == ZYNQMP_DP_MISC0_RGB) {
		num_colors = 3;
	} else if (old_output_fmt == ZYNQMP_DP_MISC0_YCRCB_422) {
		num_colors = 2;
	} else if (old_output_fmt == ZYNQMP_DP_MISC0_YCRCB_444) {
		num_colors = 3;
	} else if (old_output_fmt == ZYNQMP_DP_MISC1_Y_ONLY) {
		num_colors = 1;
	} else {
		dev_err(dp->dev, "Invalid output format in misc0\n");
		return -EINVAL;
	}

	ret = zynqmp_dp_update_output_format(old_output_fmt, num_colors);
	if (ret)
		return ret;

	output_str_len = strlen("Success");
	output_str_len = min(ZYNQMP_DP_DEBUGFS_READ_MAX_SIZE, output_str_len);
	snprintf(*kern_buff, output_str_len, "%s", "Success");

	return 0;
}

/* Match zynqmp_dp_testcases vs dp_debugfs_reqs[] entry */
static struct zynqmp_dp_debugfs_request debugfs_reqs[] = {
	{"LINK_RATE", DP_TC_LINK_RATE,
			zynqmp_dp_debugfs_max_linkrate_read,
			zynqmp_dp_debugfs_max_linkrate_write},
	{"LANE_COUNT", DP_TC_LANE_COUNT,
			zynqmp_dp_debugfs_max_lanecnt_read,
			zynqmp_dp_debugfs_max_lanecnt_write},
	{"OUTPUT_DISPLAY_FORMAT", DP_TC_OUTPUT_FMT,
			zynqmp_dp_debugfs_output_display_format_read,
			zynqmp_dp_debugfs_output_display_format_write},
};

static ssize_t zynqmp_dp_debugfs_read(struct file *f, char __user *buf,
				      size_t size, loff_t *pos)
{
	char *kern_buff = NULL;
	size_t kern_buff_len, out_str_len;
	int ret;

	if (size <= 0)
		return -EINVAL;

	if (*pos != 0)
		return 0;

	kern_buff = kzalloc(ZYNQMP_DP_DEBUGFS_READ_MAX_SIZE, GFP_KERNEL);
	if (!kern_buff) {
		dp_debugfs.testcase = DP_TC_NONE;
		return -ENOMEM;
	}

	if (dp_debugfs.testcase == DP_TC_NONE) {
		out_str_len = strlen("No testcase executed");
		out_str_len = min(ZYNQMP_DP_DEBUGFS_READ_MAX_SIZE, out_str_len);
		snprintf(kern_buff, out_str_len, "%s", "No testcase executed");
	} else {
		ret = debugfs_reqs[dp_debugfs.testcase].read_handler(
				&kern_buff);
		if (ret) {
			kfree(kern_buff);
			return ret;
		}
	}

	kern_buff_len = strlen(kern_buff);
	size = min(size, kern_buff_len);

	ret = copy_to_user(buf, kern_buff, size);

	kfree(kern_buff);
	if (ret)
		return ret;

	*pos = size + 1;
	return size;
}

static ssize_t
zynqmp_dp_debugfs_write(struct file *f, const char __user *buf,
			size_t size, loff_t *pos)
{
	char *kern_buff;
	char *dp_test_req;
	int ret;
	int i;

	if (*pos != 0 || size <= 0)
		return -EINVAL;

	if (dp_debugfs.testcase != DP_TC_NONE)
		return -EBUSY;

	kern_buff = kzalloc(size, GFP_KERNEL);
	if (!kern_buff)
		return -ENOMEM;

	ret = strncpy_from_user(kern_buff, buf, size);
	if (ret < 0) {
		kfree(kern_buff);
		return ret;
	}

	/* Read the testcase name and argument from an user request */
	dp_test_req = strsep(&kern_buff, " ");

	for (i = 0; i < ARRAY_SIZE(debugfs_reqs); i++) {
		if (!strcasecmp(dp_test_req, debugfs_reqs[i].req))
			if (!debugfs_reqs[i].write_handler(&kern_buff)) {
				kfree(kern_buff);
				return size;
			}
	}

	kfree(kern_buff);
	return -EINVAL;
}

static const struct file_operations fops_zynqmp_dp_dbgfs = {
	.owner = THIS_MODULE,
	.read = zynqmp_dp_debugfs_read,
	.write = zynqmp_dp_debugfs_write,
};

static int zynqmp_dp_debugfs_init(struct zynqmp_dp *dp)
{
	int err;
	struct dentry *zynqmp_dp_debugfs_file;

	dp_debugfs.testcase = DP_TC_NONE;
	dp_debugfs.dp = dp;

	zynqmp_dp_debugfs_dir = debugfs_create_dir("dp", NULL);
	if (!zynqmp_dp_debugfs_dir) {
		dev_err(dp->dev, "debugfs_create_dir failed\n");
		return -ENODEV;
	}

	zynqmp_dp_debugfs_file =
		debugfs_create_file("testcase", 0444, zynqmp_dp_debugfs_dir,
				    NULL, &fops_zynqmp_dp_dbgfs);
	if (!zynqmp_dp_debugfs_file) {
		dev_err(dp->dev, "debugfs_create_file testcase failed\n");
		err = -ENODEV;
		goto err_dbgfs;
	}
	return 0;

err_dbgfs:
	debugfs_remove_recursive(zynqmp_dp_debugfs_dir);
	zynqmp_dp_debugfs_dir = NULL;
	return err;
}

static void zynqmp_dp_debugfs_exit(struct zynqmp_dp *dp)
{
	debugfs_remove_recursive(zynqmp_dp_debugfs_dir);
	zynqmp_dp_debugfs_dir = NULL;
}

static void zynqmp_dp_debugfs_mode_config(struct zynqmp_dp *dp)
{
	dp->mode.bw_code =
		dp_debugfs.link_rate ? dp_debugfs.link_rate : dp->mode.bw_code;
	dp->mode.lane_cnt =
		dp_debugfs.lane_cnt ? dp_debugfs.lane_cnt : dp->mode.lane_cnt;
}

#else /* DRM_ZYNQMP_DP_DEBUG_FS */

static int zynqmp_dp_debugfs_init(struct zynqmp_dp *dp)
{
	return 0;
}

static void zynqmp_dp_debugfs_mode_config(struct zynqmp_dp *dp)
{
}

#endif /* DRM_ZYNQMP_DP_DEBUG_FS */

/*
 * Internal functions: used by zynqmp_disp.c
 */

/**
 * zynqmp_dp_update_bpp - Update the current bpp config
 * @dp: DisplayPort IP core structure
 *
 * Update the current bpp based on the color format: bpc & num_colors.
 * Any function that changes bpc or num_colors should call this
 * to keep the bpp value in sync.
 */
static void zynqmp_dp_update_bpp(struct zynqmp_dp *dp)
{
	struct zynqmp_dp_config *config = &dp->config;

	config->bpp = dp->config.bpc * dp->config.num_colors;
}

/**
 * zynqmp_dp_set_color - Set the color
 * @dp: DisplayPort IP core structure
 * @color: color string, from zynqmp_disp_color_enum
 *
 * Update misc register values based on @color string.
 *
 * Return: 0 on success, or -EINVAL.
 */
int zynqmp_dp_set_color(struct zynqmp_dp *dp, const char *color)
{
	struct zynqmp_dp_config *config = &dp->config;

	config->misc0 &= ~ZYNQMP_DP_MISC0_FORMAT_MASK;
	config->misc1 &= ~ZYNQMP_DP_MISC1_Y_ONLY;
	if (strcmp(color, "rgb") == 0) {
		config->misc0 |= ZYNQMP_DP_MISC0_RGB;
		config->num_colors = 3;
	} else if (strcmp(color, "ycrcb422") == 0) {
		config->misc0 |= ZYNQMP_DP_MISC0_YCRCB_422;
		config->num_colors = 2;
	} else if (strcmp(color, "ycrcb444") == 0) {
		config->misc0 |= ZYNQMP_DP_MISC0_YCRCB_444;
		config->num_colors = 3;
	} else if (strcmp(color, "yonly") == 0) {
		config->misc1 |= ZYNQMP_DP_MISC1_Y_ONLY;
		config->num_colors = 1;
	} else {
		dev_err(dp->dev, "Invalid colormetry in DT\n");
		return -EINVAL;
	}
	zynqmp_dp_update_bpp(dp);

	return 0;
}

/**
 * zynqmp_dp_enable_vblank - Enable vblank
 * @dp: DisplayPort IP core structure
 *
 * Enable vblank interrupt
 */
void zynqmp_dp_enable_vblank(struct zynqmp_dp *dp)
{
	zynqmp_dp_write(dp->iomem, ZYNQMP_DP_SUB_TX_INTR_EN,
			ZYNQMP_DP_TX_INTR_VBLANK_START);
}

/**
 * zynqmp_dp_disable_vblank - Disable vblank
 * @dp: DisplayPort IP core structure
 *
 * Disable vblank interrupt
 */
void zynqmp_dp_disable_vblank(struct zynqmp_dp *dp)
{
	zynqmp_dp_write(dp->iomem, ZYNQMP_DP_SUB_TX_INTR_DS,
			ZYNQMP_DP_TX_INTR_VBLANK_START);
}

/*
 * DP PHY functions
 */

/**
 * zynqmp_dp_init_phy - Initialize the phy
 * @dp: DisplayPort IP core structure
 *
 * Initialize the phy.
 *
 * Return: 0 if the phy instances are initialized correctly, or the error code
 * returned from the callee functions.
 */
static int zynqmp_dp_init_phy(struct zynqmp_dp *dp)
{
	unsigned int i;
	int ret;

	for (i = 0; i < ZYNQMP_DP_MAX_LANES; i++) {
		ret = phy_init(dp->phy[i]);
		if (ret) {
			dev_err(dp->dev, "failed to init phy lane %d\n", i);
			return ret;
		}
	}

	zynqmp_dp_write(dp->iomem, ZYNQMP_DP_SUB_TX_INTR_DS,
			ZYNQMP_DP_TX_INTR_ALL);
	zynqmp_dp_clr(dp->iomem, ZYNQMP_DP_TX_PHY_CONFIG,
		      ZYNQMP_DP_TX_PHY_CONFIG_ALL_RESET);

	/* Wait for PLL to be locked for the primary (1st) lane */
	if (dp->phy[0]) {
		ret = xpsgtr_wait_pll_lock(dp->phy[0]);
		if (ret) {
			dev_err(dp->dev, "failed to lock pll\n");
			return ret;
		}
	}

	return 0;
}

/**
 * zynqmp_dp_exit_phy - Exit the phy
 * @dp: DisplayPort IP core structure
 *
 * Exit the phy.
 */
static void zynqmp_dp_exit_phy(struct zynqmp_dp *dp)
{
	unsigned int i;
	int ret;

	for (i = 0; i < ZYNQMP_DP_MAX_LANES; i++) {
		ret = phy_exit(dp->phy[i]);
		if (ret)
			dev_err(dp->dev, "failed to exit phy(%d) %d\n", i, ret);
	}
}

/**
 * zynqmp_dp_phy_ready - Check if PHY is ready
 * @dp: DisplayPort IP core structure
 *
 * Check if PHY is ready. If PHY is not ready, wait 1ms to check for 100 times.
 * This amount of delay was suggested by IP designer.
 *
 * Return: 0 if PHY is ready, or -ENODEV if PHY is not ready.
 */
static int zynqmp_dp_phy_ready(struct zynqmp_dp *dp)
{
	u32 i, reg, ready;

	ready = (1 << ZYNQMP_DP_MAX_LANES) - 1;

	/* Wait for 100 * 1ms. This should be enough time for PHY to be ready */
	for (i = 0; ; i++) {
		reg = zynqmp_dp_read(dp->iomem, ZYNQMP_DP_TX_PHY_STATUS);
		if ((reg & ready) == ready)
			return 0;

		if (i == 100) {
			dev_err(dp->dev, "PHY isn't ready\n");
			return -ENODEV;
		}

		usleep_range(1000, 1100);
	}

	return 0;
}

/*
 * DP functions
 */

/**
 * zynqmp_dp_max_rate - Calculate and return available max pixel clock
 * @link_rate: link rate (Kilo-bytes / sec)
 * @lane_num: number of lanes
 * @bpp: bits per pixel
 *
 * Return: max pixel clock (KHz) supported by current link config.
 */
static inline int zynqmp_dp_max_rate(int link_rate, u8 lane_num, u8 bpp)
{
	return link_rate * lane_num * 8 / bpp;
}

/**
 * zynqmp_dp_mode_configure - Configure the link values
 * @dp: DisplayPort IP core structure
 * @pclock: pixel clock for requested display mode
 * @current_bw: current link rate
 *
 * Find the link configuration values, rate and lane count for requested pixel
 * clock @pclock. The @pclock is stored in the mode to be used in other
 * functions later. The returned rate is downshifted from the current rate
 * @current_bw.
 *
 * Return: Current link rate code, or -EINVAL.
 */
static int zynqmp_dp_mode_configure(struct zynqmp_dp *dp, int pclock,
				    u8 current_bw)
{
	int max_rate = dp->link_config.max_rate;
	u8 bws[3] = { DP_LINK_BW_1_62, DP_LINK_BW_2_7, DP_LINK_BW_5_4 };
	u8 max_lanes = dp->link_config.max_lanes;
	u8 max_link_rate_code = drm_dp_link_rate_to_bw_code(max_rate);
	u8 bpp = dp->config.bpp;
	u8 lane_cnt;
	s8 i;

	for (i = ARRAY_SIZE(bws) - 1; i >= 0; i--) {
		if (current_bw && bws[i] >= current_bw)
			continue;

		if (bws[i] <= max_link_rate_code)
			break;
	}

	for (lane_cnt = 1; lane_cnt <= max_lanes; lane_cnt <<= 1) {
		int bw;
		u32 rate;

		bw = drm_dp_bw_code_to_link_rate(bws[i]);
		rate = zynqmp_dp_max_rate(bw, lane_cnt, bpp);
		if (pclock <= rate) {
			dp->mode.bw_code = bws[i];
			dp->mode.lane_cnt = lane_cnt;
			dp->mode.pclock = pclock;
			zynqmp_dp_debugfs_mode_config(dp);
			return dp->mode.bw_code;
		}
	}

	dev_err(dp->dev, "failed to configure link values\n");

	return -EINVAL;
}

/**
 * zynqmp_dp_adjust_train - Adjust train values
 * @dp: DisplayPort IP core structure
 * @link_status: link status from sink which contains requested training values
 */
static void zynqmp_dp_adjust_train(struct zynqmp_dp *dp,
				   u8 link_status[DP_LINK_STATUS_SIZE])
{
	u8 *train_set = dp->train_set;
	u8 voltage = 0, preemphasis = 0;
	u8 i;

	for (i = 0; i < dp->mode.lane_cnt; i++) {
		u8 v = drm_dp_get_adjust_request_voltage(link_status, i);
		u8 p = drm_dp_get_adjust_request_pre_emphasis(link_status, i);

		if (v > voltage)
			voltage = v;

		if (p > preemphasis)
			preemphasis = p;
	}

	if (voltage >= DP_TRAIN_VOLTAGE_SWING_LEVEL_3)
		voltage |= DP_TRAIN_MAX_SWING_REACHED;

	if (preemphasis >= DP_TRAIN_PRE_EMPH_LEVEL_2)
		preemphasis |= DP_TRAIN_MAX_PRE_EMPHASIS_REACHED;

	for (i = 0; i < dp->mode.lane_cnt; i++)
		train_set[i] = voltage | preemphasis;
}

/**
 * zynqmp_dp_update_vs_emph - Update the training values
 * @dp: DisplayPort IP core structure
 *
 * Update the training values based on the request from sink. The mapped values
 * are predefined, and values(vs, pe, pc) are from the device manual.
 *
 * Return: 0 if vs and emph are updated successfully, or the error code returned
 * by drm_dp_dpcd_write().
 */
static int zynqmp_dp_update_vs_emph(struct zynqmp_dp *dp)
{
	u8 *train_set = dp->train_set;
	u8 i, v_level, p_level;
	int ret;

	ret = drm_dp_dpcd_write(&dp->aux, DP_TRAINING_LANE0_SET, train_set,
				dp->mode.lane_cnt);
	if (ret < 0)
		return ret;

	for (i = 0; i < dp->mode.lane_cnt; i++) {
		u32 reg = ZYNQMP_DP_SUB_TX_PHY_PRECURSOR_LANE_0 + i * 4;

		v_level = (train_set[i] & DP_TRAIN_VOLTAGE_SWING_MASK) >>
			  DP_TRAIN_VOLTAGE_SWING_SHIFT;
		p_level = (train_set[i] & DP_TRAIN_PRE_EMPHASIS_MASK) >>
			  DP_TRAIN_PRE_EMPHASIS_SHIFT;

		xpsgtr_margining_factor(dp->phy[i], p_level, v_level);
		xpsgtr_override_deemph(dp->phy[i], p_level, v_level);
		zynqmp_dp_write(dp->iomem, reg, 0x2);
	}

	return 0;
}

/**
 * zynqmp_dp_link_train_cr - Train clock recovery
 * @dp: DisplayPort IP core structure
 *
 * Return: 0 if clock recovery train is done successfully, or corresponding
 * error code.
 */
static int zynqmp_dp_link_train_cr(struct zynqmp_dp *dp)
{
	u8 link_status[DP_LINK_STATUS_SIZE];
	u8 lane_cnt = dp->mode.lane_cnt;
	u8 vs = 0, tries = 0;
	u16 max_tries, i;
	bool cr_done;
	int ret;

	ret = drm_dp_dpcd_writeb(&dp->aux, DP_TRAINING_PATTERN_SET,
				 DP_TRAINING_PATTERN_1 |
				 DP_LINK_SCRAMBLING_DISABLE);
	if (ret < 0)
		return ret;

	zynqmp_dp_write(dp->iomem, ZYNQMP_DP_TX_TRAINING_PATTERN_SET,
			DP_TRAINING_PATTERN_1);

	/* 256 loops should be maximum iterations for 4 lanes and 4 values.
	 * So, This loop should exit before 512 iterations
	 */
	for (max_tries = 0; max_tries < 512; max_tries++) {
		ret = zynqmp_dp_update_vs_emph(dp);
		if (ret)
			return ret;

		drm_dp_link_train_clock_recovery_delay(dp->dpcd);
		ret = drm_dp_dpcd_read_link_status(&dp->aux, link_status);
		if (ret < 0)
			return ret;

		cr_done = drm_dp_clock_recovery_ok(link_status, lane_cnt);
		if (cr_done)
			break;

		for (i = 0; i < lane_cnt; i++)
			if (!(dp->train_set[i] & DP_TRAIN_MAX_SWING_REACHED))
				break;
		if (i == lane_cnt)
			break;

		if ((dp->train_set[0] & DP_TRAIN_VOLTAGE_SWING_MASK) == vs)
			tries++;
		else
			tries = 0;

		if (tries == DP_MAX_TRAINING_TRIES)
			break;

		vs = dp->train_set[0] & DP_TRAIN_VOLTAGE_SWING_MASK;
		zynqmp_dp_adjust_train(dp, link_status);
	}

	if (!cr_done)
		return -EIO;

	return 0;
}

/**
 * zynqmp_dp_link_train_ce - Train channel equalization
 * @dp: DisplayPort IP core structure
 *
 * Return: 0 if channel equalization train is done successfully, or
 * corresponding error code.
 */
static int zynqmp_dp_link_train_ce(struct zynqmp_dp *dp)
{
	u8 link_status[DP_LINK_STATUS_SIZE];
	u8 lane_cnt = dp->mode.lane_cnt;
	u32 pat, tries;
	int ret;
	bool ce_done;

	if (dp->dpcd[DP_DPCD_REV] >= DP_V1_2 &&
	    dp->dpcd[DP_MAX_LANE_COUNT] & DP_TPS3_SUPPORTED)
		pat = DP_TRAINING_PATTERN_3;
	else
		pat = DP_TRAINING_PATTERN_2;

	ret = drm_dp_dpcd_writeb(&dp->aux, DP_TRAINING_PATTERN_SET,
				 pat | DP_LINK_SCRAMBLING_DISABLE);
	if (ret < 0)
		return ret;

	zynqmp_dp_write(dp->iomem, ZYNQMP_DP_TX_TRAINING_PATTERN_SET, pat);

	for (tries = 0; tries < DP_MAX_TRAINING_TRIES; tries++) {
		ret = zynqmp_dp_update_vs_emph(dp);
		if (ret)
			return ret;

		drm_dp_link_train_channel_eq_delay(dp->dpcd);
		ret = drm_dp_dpcd_read_link_status(&dp->aux, link_status);
		if (ret < 0)
			return ret;

		ce_done = drm_dp_channel_eq_ok(link_status, lane_cnt);
		if (ce_done)
			break;

		zynqmp_dp_adjust_train(dp, link_status);
	}

	if (!ce_done)
		return -EIO;

	return 0;
}

/**
 * zynqmp_dp_link_train - Train the link
 * @dp: DisplayPort IP core structure
 *
 * Return: 0 if all trains are done successfully, or corresponding error code.
 */
static int zynqmp_dp_train(struct zynqmp_dp *dp)
{
	u32 reg;
	u8 bw_code = dp->mode.bw_code;
	u8 lane_cnt = dp->mode.lane_cnt;
	u8 aux_lane_cnt = lane_cnt;
	bool enhanced;
	int ret;

	zynqmp_dp_write(dp->iomem, ZYNQMP_DP_TX_LANE_CNT_SET, lane_cnt);
	enhanced = drm_dp_enhanced_frame_cap(dp->dpcd);
	if (enhanced) {
		zynqmp_dp_write(dp->iomem, ZYNQMP_DP_TX_ENHANCED_FRAME_EN, 1);
		aux_lane_cnt |= DP_LANE_COUNT_ENHANCED_FRAME_EN;
	}

	if (dp->dpcd[3] & 0x1) {
		zynqmp_dp_write(dp->iomem, ZYNQMP_DP_TX_DOWNSPREAD_CTL, 1);
		drm_dp_dpcd_writeb(&dp->aux, DP_DOWNSPREAD_CTRL,
				   DP_SPREAD_AMP_0_5);
	} else {
		zynqmp_dp_write(dp->iomem, ZYNQMP_DP_TX_DOWNSPREAD_CTL, 0);
		drm_dp_dpcd_writeb(&dp->aux, DP_DOWNSPREAD_CTRL, 0);
	}

	ret = drm_dp_dpcd_writeb(&dp->aux, DP_LANE_COUNT_SET, aux_lane_cnt);
	if (ret < 0) {
		dev_err(dp->dev, "failed to set lane count\n");
		return ret;
	}

	ret = drm_dp_dpcd_writeb(&dp->aux, DP_MAIN_LINK_CHANNEL_CODING_SET,
				 DP_SET_ANSI_8B10B);
	if (ret < 0) {
		dev_err(dp->dev, "failed to set ANSI 8B/10B encoding\n");
		return ret;
	}

	ret = drm_dp_dpcd_writeb(&dp->aux, DP_LINK_BW_SET, bw_code);
	if (ret < 0) {
		dev_err(dp->dev, "failed to set DP bandwidth\n");
		return ret;
	}

	zynqmp_dp_write(dp->iomem, ZYNQMP_DP_TX_LINK_BW_SET, bw_code);
	switch (bw_code) {
	case DP_LINK_BW_1_62:
		reg = ZYNQMP_DP_TX_PHY_CLOCK_FEEDBACK_SETTING_162;
		break;
	case DP_LINK_BW_2_7:
		reg = ZYNQMP_DP_TX_PHY_CLOCK_FEEDBACK_SETTING_270;
		break;
	case DP_LINK_BW_5_4:
	default:
		reg = ZYNQMP_DP_TX_PHY_CLOCK_FEEDBACK_SETTING_540;
		break;
	}

	zynqmp_dp_write(dp->iomem, ZYNQMP_DP_TX_PHY_CLOCK_FEEDBACK_SETTING,
			reg);
	ret = zynqmp_dp_phy_ready(dp);
	if (ret < 0)
		return ret;

	zynqmp_dp_write(dp->iomem, ZYNQMP_DP_TX_SCRAMBLING_DISABLE, 1);
	memset(dp->train_set, 0, 4);
	ret = zynqmp_dp_link_train_cr(dp);
	if (ret)
		return ret;

	ret = zynqmp_dp_link_train_ce(dp);
	if (ret)
		return ret;

	zynqmp_dp_write(dp->iomem, ZYNQMP_DP_TX_TRAINING_PATTERN_SET,
			DP_TRAINING_PATTERN_DISABLE);
	ret = drm_dp_dpcd_writeb(&dp->aux, DP_TRAINING_PATTERN_SET,
				 DP_TRAINING_PATTERN_DISABLE);
	if (ret < 0) {
		dev_err(dp->dev, "failed to disable training pattern\n");
		return ret;
	}

	zynqmp_dp_write(dp->iomem, ZYNQMP_DP_TX_SCRAMBLING_DISABLE, 0);

	return 0;
}

/**
 * zynqmp_dp_train_loop - Downshift the link rate during training
 * @dp: DisplayPort IP core structure
 *
 * Train the link by downshifting the link rate if training is not successful.
 */
static void zynqmp_dp_train_loop(struct zynqmp_dp *dp)
{
	struct zynqmp_dp_mode *mode = &dp->mode;
	u8 bw = mode->bw_code;
	int ret;

	do {
		ret = zynqmp_dp_train(dp);
		if (!ret)
			return;

		ret = zynqmp_dp_mode_configure(dp, mode->pclock, bw);
		if (ret < 0)
			return;
		bw = ret;
	} while (bw >= DP_LINK_BW_1_62);

	dev_err(dp->dev, "failed to train the DP link\n");
}

/*
 * DP Aux functions
 */

#define AUX_READ_BIT	0x1

/**
 * zynqmp_dp_aux_cmd_submit - Submit aux command
 * @dp: DisplayPort IP core structure
 * @cmd: aux command
 * @addr: aux address
 * @buf: buffer for command data
 * @bytes: number of bytes for @buf
 * @reply: reply code to be returned
 *
 * Submit an aux command. All aux related commands, native or i2c aux
 * read/write, are submitted through this function. The function is mapped to
 * the transfer function of struct drm_dp_aux. This function involves in
 * multiple register reads/writes, thus synchronization is needed, and it is
 * done by drm_dp_helper using @hw_mutex. The calling thread goes into sleep
 * if there's no immediate reply to the command submission. The reply code is
 * returned at @reply if @reply != NULL.
 *
 * Return: 0 if the command is submitted properly, or corresponding error code:
 * -EBUSY when there is any request already being processed
 * -ETIMEDOUT when receiving reply is timed out
 * -EIO when received bytes are less than requested
 */
static int zynqmp_dp_aux_cmd_submit(struct zynqmp_dp *dp, u32 cmd, u16 addr,
				    u8 *buf, u8 bytes, u8 *reply)
{
	bool is_read = (cmd & AUX_READ_BIT) ? true : false;
	void __iomem *iomem = dp->iomem;
	u32 reg, i;

	reg = zynqmp_dp_read(iomem, ZYNQMP_DP_TX_INTR_SIGNAL_STATE);
	if (reg & ZYNQMP_DP_TX_INTR_SIGNAL_STATE_REQUEST)
		return -EBUSY;

	zynqmp_dp_write(iomem, ZYNQMP_DP_TX_AUX_ADDRESS, addr);
	if (!is_read)
		for (i = 0; i < bytes; i++)
			zynqmp_dp_write(iomem, ZYNQMP_DP_TX_AUX_WRITE_FIFO,
					buf[i]);

	reg = cmd << ZYNQMP_DP_TX_AUX_COMMAND_CMD_SHIFT;
	if (!buf || !bytes)
		reg |= ZYNQMP_DP_TX_AUX_COMMAND_ADDRESS_ONLY;
	else
		reg |= (bytes - 1) << ZYNQMP_DP_TX_AUX_COMMAND_BYTES_SHIFT;
	zynqmp_dp_write(iomem, ZYNQMP_DP_TX_AUX_COMMAND, reg);

	/* Wait for reply to be delivered upto 2ms */
	for (i = 0; ; i++) {
		reg = zynqmp_dp_read(iomem, ZYNQMP_DP_TX_INTR_SIGNAL_STATE);
		if (reg & ZYNQMP_DP_TX_INTR_SIGNAL_STATE_REPLY)
			break;

		if (reg & ZYNQMP_DP_TX_INTR_SIGNAL_STATE_REPLY_TIMEOUT ||
		    i == 2)
			return -ETIMEDOUT;

		usleep_range(1000, 1100);
	}

	reg = zynqmp_dp_read(iomem, ZYNQMP_DP_TX_AUX_REPLY_CODE);
	if (reply)
		*reply = reg;

	if (is_read &&
	    (reg == ZYNQMP_DP_TX_AUX_REPLY_CODE_AUX_ACK ||
	     reg == ZYNQMP_DP_TX_AUX_REPLY_CODE_I2C_ACK)) {
		reg = zynqmp_dp_read(iomem, ZYNQMP_DP_TX_REPLY_DATA_CNT);
		if ((reg & ZYNQMP_DP_TX_AUX_REPLY_CNT_MASK) != bytes)
			return -EIO;

		for (i = 0; i < bytes; i++) {
			buf[i] = zynqmp_dp_read(iomem,
						ZYNQMP_DP_TX_AUX_REPLY_DATA);
		}
	}

	return 0;
}

static ssize_t
zynqmp_dp_aux_transfer(struct drm_dp_aux *aux, struct drm_dp_aux_msg *msg)
{
	struct zynqmp_dp *dp = container_of(aux, struct zynqmp_dp, aux);
	int ret;
	unsigned int i, iter;

	/* Number of loops = timeout in msec / aux delay (400 usec) */
	iter = zynqmp_dp_aux_timeout_ms * 1000 / 400;
	iter = iter ? iter : 1;

	for (i = 0; i < iter; i++) {
		ret = zynqmp_dp_aux_cmd_submit(dp, msg->request, msg->address,
					       msg->buffer, msg->size,
					       &msg->reply);
		if (!ret) {
			dev_dbg(dp->dev, "aux %d retries\n", i);
			return msg->size;
		}

		usleep_range(400, 500);
	}

	dev_dbg(dp->dev, "failed to do aux transfer (%d)\n", ret);

	return ret;
}

/**
 * zynqmp_dp_init_aux - Initialize the DP aux
 * @dp: DisplayPort IP core structure
 *
 * Initialize the DP aux. The aux clock is derived from the axi clock, so
 * this function gets the axi clock frequency and calculates the filter
 * value. Additionally, the interrupts and transmitter are enabled.
 *
 * Return: 0 on success, error value otherwise
 */
static int zynqmp_dp_init_aux(struct zynqmp_dp *dp)
{
	unsigned int rate;
	u32 reg, w;

	rate = zynqmp_disp_get_apb_clk_rate(dp->dpsub->disp);
	if (rate < ZYNQMP_DP_TX_CLK_DIVIDER_MHZ) {
		dev_err(dp->dev, "aclk should be higher than 1MHz\n");
		return -EINVAL;
	}

	/* Allowable values for this register are: 8, 16, 24, 32, 40, 48 */
	for (w = 8; w <= 48; w += 8) {
		/* AUX pulse width should be between 0.4 to 0.6 usec */
		if (w >= (4 * rate / 10000000) &&
		    w <= (6 * rate / 10000000))
			break;
	}

	if (w > 48) {
		dev_err(dp->dev, "aclk frequency too high\n");
		return -EINVAL;
	}
	reg = w << ZYNQMP_DP_TX_CLK_DIVIDER_AUX_FILTER_SHIFT;
	reg |= rate / ZYNQMP_DP_TX_CLK_DIVIDER_MHZ;
	zynqmp_dp_write(dp->iomem, ZYNQMP_DP_TX_CLK_DIVIDER, reg);
	zynqmp_dp_write(dp->iomem, ZYNQMP_DP_SUB_TX_INTR_EN,
			ZYNQMP_DP_TX_INTR_ALL);
	zynqmp_dp_write(dp->iomem, ZYNQMP_DP_SUB_TX_INTR_DS,
			ZYNQMP_DP_TX_NO_INTR_ALL);
	zynqmp_dp_write(dp->iomem, ZYNQMP_DP_TX_ENABLE, 1);

	return 0;
}

/**
 * zynqmp_dp_exit_aux - De-initialize the DP aux
 * @dp: DisplayPort IP core structure
 *
 * De-initialize the DP aux. Disable all interrupts which are enabled
 * through aux initialization, as well as the transmitter.
 */
static void zynqmp_dp_exit_aux(struct zynqmp_dp *dp)
{
	zynqmp_dp_write(dp->iomem, ZYNQMP_DP_TX_ENABLE, 0);
	zynqmp_dp_write(dp->iomem, ZYNQMP_DP_SUB_TX_INTR_DS, 0xffffffff);
}

/*
 * Generic DP functions
 */

/**
 * zynqmp_dp_update_misc - Write the misc registers
 * @dp: DisplayPort IP core structure
 *
 * The misc register values are stored in the structure, and this
 * function applies the values into the registers.
 */
static void zynqmp_dp_update_misc(struct zynqmp_dp *dp)
{
	zynqmp_dp_write(dp->iomem, ZYNQMP_DP_TX_MAIN_STREAM_MISC0,
			dp->config.misc0);
	zynqmp_dp_write(dp->iomem, ZYNQMP_DP_TX_MAIN_STREAM_MISC1,
			dp->config.misc1);
}

/**
 * zynqmp_dp_set_sync_mode - Set the sync mode bit in the software misc state
 * @dp: DisplayPort IP core structure
 * @mode: flag if the sync mode should be on or off
 *
 * Set the bit in software misc state. To apply to hardware,
 * zynqmp_dp_update_misc() should be called.
 */
static void zynqmp_dp_set_sync_mode(struct zynqmp_dp *dp, bool mode)
{
	struct zynqmp_dp_config *config = &dp->config;

	if (mode)
		config->misc0 |= ZYNQMP_DP_TX_MAIN_STREAM_MISC0_SYNC;
	else
		config->misc0 &= ~ZYNQMP_DP_TX_MAIN_STREAM_MISC0_SYNC;
}

/**
 * zynqmp_dp_get_sync_mode - Get the sync mode state
 * @dp: DisplayPort IP core structure
 *
 * Return: true if the sync mode is on, or false
 */
static bool zynqmp_dp_get_sync_mode(struct zynqmp_dp *dp)
{
	struct zynqmp_dp_config *config = &dp->config;

	return !!(config->misc0 & ZYNQMP_DP_TX_MAIN_STREAM_MISC0_SYNC);
}

/**
 * zynqmp_dp_set_bpc - Set bpc value in software misc state
 * @dp: DisplayPort IP core structure
 * @bpc: bits per component
 *
 * Return: 0 on success, or the fallback bpc value
 */
static u8 zynqmp_dp_set_bpc(struct zynqmp_dp *dp, u8 bpc)
{
	struct zynqmp_dp_config *config = &dp->config;
	u8 ret = 0;

	if (dp->connector.display_info.bpc &&
	    dp->connector.display_info.bpc != bpc) {
		dev_err(dp->dev, "requested bpc (%u) != display info (%u)\n",
			bpc, dp->connector.display_info.bpc);
		bpc = dp->connector.display_info.bpc;
	}

	config->misc0 &= ~ZYNQMP_DP_MISC0_BPC_MASK;
	switch (bpc) {
	case 6:
		config->misc0 |= ZYNQMP_DP_MISC0_BPC_6;
		break;
	case 8:
		config->misc0 |= ZYNQMP_DP_MISC0_BPC_8;
		break;
	case 10:
		config->misc0 |= ZYNQMP_DP_MISC0_BPC_10;
		break;
	case 12:
		config->misc0 |= ZYNQMP_DP_MISC0_BPC_12;
		break;
	case 16:
		config->misc0 |= ZYNQMP_DP_MISC0_BPC_16;
		break;
	default:
		dev_err(dp->dev, "Not supported bpc (%u). fall back to 8bpc\n",
			bpc);
		config->misc0 |= ZYNQMP_DP_MISC0_BPC_8;
		ret = 8;
		break;
	}
	config->bpc = bpc;
	zynqmp_dp_update_bpp(dp);

	return ret;
}

/**
 * zynqmp_dp_get_bpc - Set bpc value from software state
 * @dp: DisplayPort IP core structure
 *
 * Return: current bpc value
 */
static u8 zynqmp_dp_get_bpc(struct zynqmp_dp *dp)
{
	return dp->config.bpc;
}

/**
 * zynqmp_dp_encoder_mode_set_transfer_unit - Set the transfer unit values
 * @dp: DisplayPort IP core structure
 * @mode: requested display mode
 *
 * Set the transfer unit, and caculate all transfer unit size related values.
 * Calculation is based on DP and IP core specification.
 */
static void
zynqmp_dp_encoder_mode_set_transfer_unit(struct zynqmp_dp *dp,
					 struct drm_display_mode *mode)
{
	u32 tu = ZYNQMP_DP_TX_DEF_TRANSFER_UNIT_SIZE;
	u32 bw, vid_kbytes, avg_bytes_per_tu, init_wait;

	/* Use the max transfer unit size (default) */
	zynqmp_dp_write(dp->iomem, ZYNQMP_DP_TX_TRANSFER_UNIT_SIZE, tu);

	vid_kbytes = mode->clock * (dp->config.bpp / 8);
	bw = drm_dp_bw_code_to_link_rate(dp->mode.bw_code);
	avg_bytes_per_tu = vid_kbytes * tu / (dp->mode.lane_cnt * bw / 1000);
	zynqmp_dp_write(dp->iomem, ZYNQMP_DP_TX_MIN_BYTES_PER_TU,
			avg_bytes_per_tu / 1000);
	zynqmp_dp_write(dp->iomem, ZYNQMP_DP_TX_FRAC_BYTES_PER_TU,
			avg_bytes_per_tu % 1000);

	/* Configure the initial wait cycle based on transfer unit size */
	if (tu < (avg_bytes_per_tu / 1000))
		init_wait = 0;
	else if ((avg_bytes_per_tu / 1000) <= 4)
		init_wait = tu;
	else
		init_wait = tu - avg_bytes_per_tu / 1000;

	zynqmp_dp_write(dp->iomem, ZYNQMP_DP_TX_INIT_WAIT, init_wait);
}

/**
 * zynqmp_dp_encoder_mode_set_stream - Configure the main stream
 * @dp: DisplayPort IP core structure
 * @mode: requested display mode
 *
 * Configure the main stream based on the requested mode @mode. Calculation is
 * based on IP core specification.
 */
void zynqmp_dp_encoder_mode_set_stream(struct zynqmp_dp *dp,
				       struct drm_display_mode *mode)
{
	void __iomem *iomem = dp->iomem;
	u8 lane_cnt = dp->mode.lane_cnt;
	u32 reg, wpl;
	unsigned int rate;

	zynqmp_dp_write(iomem, ZYNQMP_DP_TX_MAIN_STREAM_HTOTAL, mode->htotal);
	zynqmp_dp_write(iomem, ZYNQMP_DP_TX_MAIN_STREAM_VTOTAL, mode->vtotal);
	zynqmp_dp_write(iomem, ZYNQMP_DP_TX_MAIN_STREAM_POLARITY,
			(!!(mode->flags & DRM_MODE_FLAG_PVSYNC) <<
			 ZYNQMP_DP_TX_MAIN_STREAM_POLARITY_VSYNC_SHIFT) |
			(!!(mode->flags & DRM_MODE_FLAG_PHSYNC) <<
			 ZYNQMP_DP_TX_MAIN_STREAM_POLARITY_HSYNC_SHIFT));
	zynqmp_dp_write(iomem, ZYNQMP_DP_TX_MAIN_STREAM_HSWIDTH,
			mode->hsync_end - mode->hsync_start);
	zynqmp_dp_write(iomem, ZYNQMP_DP_TX_MAIN_STREAM_VSWIDTH,
			mode->vsync_end - mode->vsync_start);
	zynqmp_dp_write(iomem, ZYNQMP_DP_TX_MAIN_STREAM_HRES, mode->hdisplay);
	zynqmp_dp_write(iomem, ZYNQMP_DP_TX_MAIN_STREAM_VRES, mode->vdisplay);
	zynqmp_dp_write(iomem, ZYNQMP_DP_TX_MAIN_STREAM_HSTART,
			mode->htotal - mode->hsync_start);
	zynqmp_dp_write(iomem, ZYNQMP_DP_TX_MAIN_STREAM_VSTART,
			mode->vtotal - mode->vsync_start);

	/* In synchronous mode, set the diviers */
	if (dp->config.misc0 & ZYNQMP_DP_TX_MAIN_STREAM_MISC0_SYNC) {
		reg = drm_dp_bw_code_to_link_rate(dp->mode.bw_code);
		zynqmp_dp_write(iomem, ZYNQMP_DP_TX_N_VID, reg);
		zynqmp_dp_write(iomem, ZYNQMP_DP_TX_M_VID, mode->clock);
		rate = zynqmp_disp_get_aud_clk_rate(dp->dpsub->disp);
		if (rate) {
			dev_dbg(dp->dev, "Audio rate: %d\n", rate / 512);
			zynqmp_dp_write(iomem, ZYNQMP_DP_TX_AUDIO_N_AUD, reg);
			zynqmp_dp_write(iomem, ZYNQMP_DP_TX_AUDIO_M_AUD,
					rate / 1000);
		}
	}

	/* Only 2 channel audio is supported now */
	if (zynqmp_disp_aud_enabled(dp->dpsub->disp))
		zynqmp_dp_write(iomem, ZYNQMP_DP_TX_AUDIO_CHANNELS, 1);

	zynqmp_dp_write(iomem, ZYNQMP_DP_TX_USER_PIXEL_WIDTH, 1);

	/* Translate to the native 16 bit datapath based on IP core spec */
	wpl = (mode->hdisplay * dp->config.bpp + 15) / 16;
	reg = wpl + wpl % lane_cnt - lane_cnt;
	zynqmp_dp_write(iomem, ZYNQMP_DP_TX_USER_DATA_CNT_PER_LANE, reg);
}

/*
 * DRM property functions
 */

static struct drm_prop_enum_list zynqmp_dp_bpc_enum[] = {
	{ 6, "6BPC" },
	{ 8, "8BPC" },
	{ 10, "10BPC" },
	{ 12, "12BPC" },
};

static void
zynqmp_dp_attach_property(struct zynqmp_dp *dp, struct drm_mode_object *obj)
{
	struct zynqmp_dp_config *config = &dp->config;
	u8 ret;

	config->misc0 &= ~ZYNQMP_DP_TX_MAIN_STREAM_MISC0_SYNC;
	drm_object_attach_property(obj, dp->sync_prop, false);
	ret = zynqmp_dp_set_bpc(dp, 8);
	drm_object_attach_property(obj, dp->bpc_prop, ret ? ret : 8);
	zynqmp_dp_update_bpp(dp);
}

static void zynqmp_dp_create_property(struct zynqmp_dp *dp)
{
	int num;

	dp->sync_prop = drm_property_create_bool(dp->drm, 0, "sync");
	num = ARRAY_SIZE(zynqmp_dp_bpc_enum);
	dp->bpc_prop = drm_property_create_enum(dp->drm, 0, "bpc",
						zynqmp_dp_bpc_enum, num);
}

static void zynqmp_dp_destroy_property(struct zynqmp_dp *dp)
{
	drm_property_destroy(dp->drm, dp->bpc_prop);
	drm_property_destroy(dp->drm, dp->sync_prop);
}

/*
 * DRM connector functions
 */

static enum drm_connector_status
zynqmp_dp_connector_detect(struct drm_connector *connector, bool force)
{
	struct zynqmp_dp *dp = connector_to_dp(connector);
	struct zynqmp_dp_link_config *link_config = &dp->link_config;
	u32 state, i;
	int ret;

	/*
	 * This is from heuristic. It takes some delay (ex, 100 ~ 500 msec) to
	 * get the HPD signal with some monitors.
	 */
	for (i = 0; i < 10; i++) {
		state = zynqmp_dp_read(dp->iomem, ZYNQMP_DP_TX_INTR_SIGNAL_STATE);
		if (state & ZYNQMP_DP_TX_INTR_SIGNAL_STATE_HPD)
			break;
		msleep(100);
	}

	if (state & ZYNQMP_DP_TX_INTR_SIGNAL_STATE_HPD) {
		ret = drm_dp_dpcd_read(&dp->aux, 0x0, dp->dpcd,
				       sizeof(dp->dpcd));
		if (ret < 0) {
			dev_dbg(dp->dev, "DPCD read failes");
			return connector_status_disconnected;
		}

		link_config->max_rate = min_t(int,
					      drm_dp_max_link_rate(dp->dpcd),
					      DP_HIGH_BIT_RATE2);
		link_config->max_lanes = min_t(u8,
					       drm_dp_max_lane_count(dp->dpcd),
					       ZYNQMP_DP_MAX_LANES);

		return connector_status_connected;
	}

	return connector_status_disconnected;
}

static int zynqmp_dp_connector_get_modes(struct drm_connector *connector)
{
	struct zynqmp_dp *dp = connector_to_dp(connector);
	struct edid *edid;
	int ret;

	edid = drm_get_edid(connector, &dp->aux.ddc);
	if (!edid)
		return 0;

	drm_mode_connector_update_edid_property(connector, edid);
	ret = drm_add_edid_modes(connector, edid);
	kfree(edid);

	return ret;
}

static struct drm_encoder *
zynqmp_dp_connector_best_encoder(struct drm_connector *connector)
{
	struct zynqmp_dp *dp = connector_to_dp(connector);

	return &dp->encoder;
}

static int zynqmp_dp_connector_mode_valid(struct drm_connector *connector,
					  struct drm_display_mode *mode)
{
	struct zynqmp_dp *dp = connector_to_dp(connector);
	u8 max_lanes = dp->link_config.max_lanes;
	u8 bpp = dp->config.bpp;
	int max_rate = dp->link_config.max_rate;
	int rate;

	if (mode->clock > ZYNQMP_MAX_FREQ) {
		dev_dbg(dp->dev, "filtered the mode, %s,for high pixel rate\n",
			mode->name);
		drm_mode_debug_printmodeline(mode);
		return MODE_CLOCK_HIGH;
	}

	/* Check with link rate and lane count */
	rate = zynqmp_dp_max_rate(max_rate, max_lanes, bpp);
	if (mode->clock > rate) {
		dev_dbg(dp->dev, "filtered the mode, %s,for high pixel rate\n",
			mode->name);
		drm_mode_debug_printmodeline(mode);
		return MODE_CLOCK_HIGH;
	}

	return MODE_OK;
}

static void zynqmp_dp_connector_destroy(struct drm_connector *connector)
{
	drm_connector_unregister(connector);
	drm_connector_cleanup(connector);
}

static int
zynqmp_dp_connector_atomic_set_property(struct drm_connector *connector,
					struct drm_connector_state *state,
					struct drm_property *property,
					uint64_t val)
{
	struct zynqmp_dp *dp = connector_to_dp(connector);
	int ret;

	if (property == dp->sync_prop) {
		zynqmp_dp_set_sync_mode(dp, val);
	} else if (property == dp->bpc_prop) {
		u8 bpc;

		bpc = zynqmp_dp_set_bpc(dp, val);
		if (bpc) {
			drm_object_property_set_value(&connector->base,
						      property, bpc);
			ret = -EINVAL;
		}
	} else {
		return -EINVAL;
	}

	return 0;
}

static int
zynqmp_dp_connector_atomic_get_property(struct drm_connector *connector,
					const struct drm_connector_state *state,
					struct drm_property *property,
					uint64_t *val)
{
	struct zynqmp_dp *dp = connector_to_dp(connector);

	if (property == dp->sync_prop)
		*val = zynqmp_dp_get_sync_mode(dp);
	else if (property == dp->bpc_prop)
		*val =  zynqmp_dp_get_bpc(dp);
	else
		return -EINVAL;

	return 0;
}

static const struct drm_connector_funcs zynqmp_dp_connector_funcs = {
	.dpms			= drm_atomic_helper_connector_dpms,
	.detect			= zynqmp_dp_connector_detect,
	.fill_modes		= drm_helper_probe_single_connector_modes,
	.destroy		= zynqmp_dp_connector_destroy,
	.atomic_duplicate_state	= drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state	= drm_atomic_helper_connector_destroy_state,
	.reset			= drm_atomic_helper_connector_reset,
	.atomic_set_property	= zynqmp_dp_connector_atomic_set_property,
	.atomic_get_property	= zynqmp_dp_connector_atomic_get_property,
	.set_property		= drm_atomic_helper_connector_set_property,
};

static struct drm_connector_helper_funcs zynqmp_dp_connector_helper_funcs = {
	.get_modes	= zynqmp_dp_connector_get_modes,
	.best_encoder	= zynqmp_dp_connector_best_encoder,
	.mode_valid	= zynqmp_dp_connector_mode_valid,
};

/*
 * DRM encoder functions
 */

static void zynqmp_dp_encoder_enable(struct drm_encoder *encoder)
{
	struct zynqmp_dp *dp = encoder_to_dp(encoder);
	void __iomem *iomem = dp->iomem;
	unsigned int i;
	int ret;

	pm_runtime_get_sync(dp->dev);
	zynqmp_dp_update_misc(dp);
	if (zynqmp_disp_aud_enabled(dp->dpsub->disp))
		zynqmp_dp_write(iomem, ZYNQMP_DP_TX_AUDIO_CONTROL, 1);
	zynqmp_dp_write(iomem, ZYNQMP_DP_TX_PHY_POWER_DOWN, 0);
	for (i = 0; i < 3; i++) {
		ret = drm_dp_dpcd_writeb(&dp->aux, DP_SET_POWER,
					 DP_SET_POWER_D0);
		if (ret == 1)
			break;
		usleep_range(300, 500);
	}

	if (ret != 1)
		dev_dbg(dp->dev, "DP aux failed\n");
	else
		zynqmp_dp_train_loop(dp);
	zynqmp_dp_write(iomem, ZYNQMP_DP_TX_SW_RESET,
			ZYNQMP_DP_TX_SW_RESET_ALL);
	zynqmp_dp_write(iomem, ZYNQMP_DP_TX_ENABLE_MAIN_STREAM, 1);
}

static void zynqmp_dp_encoder_disable(struct drm_encoder *encoder)
{
	struct zynqmp_dp *dp = encoder_to_dp(encoder);
	void __iomem *iomem = dp->iomem;

	cancel_delayed_work(&dp->hpd_work);
	zynqmp_dp_write(iomem, ZYNQMP_DP_TX_ENABLE_MAIN_STREAM, 0);
	drm_dp_dpcd_writeb(&dp->aux, DP_SET_POWER, DP_SET_POWER_D3);
	zynqmp_dp_write(iomem, ZYNQMP_DP_TX_PHY_POWER_DOWN,
			ZYNQMP_DP_TX_PHY_POWER_DOWN_ALL);
	if (zynqmp_disp_aud_enabled(dp->dpsub->disp))
		zynqmp_dp_write(iomem, ZYNQMP_DP_TX_AUDIO_CONTROL, 0);
	pm_runtime_put_sync(dp->dev);
}

static void
zynqmp_dp_encoder_atomic_mode_set(struct drm_encoder *encoder,
				  struct drm_crtc_state *crtc_state,
				  struct drm_connector_state *connector_state)
{
	struct zynqmp_dp *dp = encoder_to_dp(encoder);
	struct drm_display_mode *mode = &crtc_state->mode;
	struct drm_display_mode *adjusted_mode = &crtc_state->adjusted_mode;
	u8 max_lanes = dp->link_config.max_lanes;
	u8 bpp = dp->config.bpp;
	int rate, max_rate = dp->link_config.max_rate;
	int ret;

	/* Check again as bpp or format might have been chagned */
	rate = zynqmp_dp_max_rate(max_rate, max_lanes, bpp);
	if (mode->clock > rate) {
		dev_err(dp->dev, "the mode, %s,has too high pixel rate\n",
			mode->name);
		drm_mode_debug_printmodeline(mode);
	}

	ret = zynqmp_dp_mode_configure(dp, adjusted_mode->clock, 0);
	if (ret < 0)
		return;

	zynqmp_dp_encoder_mode_set_transfer_unit(dp, adjusted_mode);

}

#define ZYNQMP_DP_MIN_H_BACKPORCH	20

static int
zynqmp_dp_encoder_atomic_check(struct drm_encoder *encoder,
			       struct drm_crtc_state *crtc_state,
			       struct drm_connector_state *conn_state)
{
	struct drm_display_mode *mode = &crtc_state->mode;
	struct drm_display_mode *adjusted_mode = &crtc_state->adjusted_mode;
	int diff = mode->htotal - mode->hsync_end;

	/*
	 * ZynqMP DP requires horizontal backporch to be greater than 12.
	 * This limitation may not be compatible with the sink device.
	 */
	if (diff < ZYNQMP_DP_MIN_H_BACKPORCH) {
		int vrefresh = (adjusted_mode->clock * 1000) /
			       (adjusted_mode->vtotal * adjusted_mode->htotal);

		dev_dbg(encoder->dev->dev, "hbackporch adjusted: %d to %d",
			diff, ZYNQMP_DP_MIN_H_BACKPORCH - diff);
		diff = ZYNQMP_DP_MIN_H_BACKPORCH - diff;
		adjusted_mode->htotal += diff;
		adjusted_mode->clock = adjusted_mode->vtotal *
				       adjusted_mode->htotal * vrefresh / 1000;
	}

	diff = mode->hsync_start - mode->hdisplay;
	if (mode->hsync_start - mode->hdisplay > zynqmp_dp_debug_hfp) {
		int vrefresh = (adjusted_mode->clock * 1000) /
			(adjusted_mode->vtotal * adjusted_mode->htotal);

		diff = diff - zynqmp_dp_debug_hfp;
		adjusted_mode->htotal -= diff;
		adjusted_mode->hsync_end -= diff;
		adjusted_mode->hsync_start -= diff;
		adjusted_mode->clock = adjusted_mode->vtotal *
		adjusted_mode->htotal * vrefresh / 1000;
	}

	return 0;
}

static const struct drm_encoder_funcs zynqmp_dp_encoder_funcs = {
	.destroy = drm_encoder_cleanup,
};

static const struct drm_encoder_helper_funcs zynqmp_dp_encoder_helper_funcs = {
	.enable			= zynqmp_dp_encoder_enable,
	.disable		= zynqmp_dp_encoder_disable,
	.atomic_mode_set	= zynqmp_dp_encoder_atomic_mode_set,
	.atomic_check		= zynqmp_dp_encoder_atomic_check,
};

/*
 * Component functions
 */

int zynqmp_dp_bind(struct device *dev, struct device *master, void *data)
{
	struct zynqmp_dpsub *dpsub = dev_get_drvdata(dev);
	struct zynqmp_dp *dp = dpsub->dp;
	struct drm_encoder *encoder = &dp->encoder;
	struct drm_connector *connector = &dp->connector;
	struct drm_device *drm = data;
	struct device_node *port;
	int ret;

	encoder->possible_crtcs |= zynqmp_disp_get_crtc_mask(dpsub->disp);
	for_each_child_of_node(dev->of_node, port) {
		if (!port->name || of_node_cmp(port->name, "port"))
			continue;
		encoder->possible_crtcs |= drm_of_find_possible_crtcs(drm,
								      port);
	}
	drm_encoder_init(drm, encoder, &zynqmp_dp_encoder_funcs,
			 DRM_MODE_ENCODER_TMDS, NULL);
	drm_encoder_helper_add(encoder, &zynqmp_dp_encoder_helper_funcs);

	connector->polled = DRM_CONNECTOR_POLL_HPD;
	ret = drm_connector_init(encoder->dev, connector,
				 &zynqmp_dp_connector_funcs,
				 DRM_MODE_CONNECTOR_DisplayPort);
	if (ret) {
		dev_err(dp->dev, "failed to initialize the drm connector");
		goto error_encoder;
	}

	drm_connector_helper_add(connector, &zynqmp_dp_connector_helper_funcs);
	drm_connector_register(connector);
	drm_mode_connector_attach_encoder(connector, encoder);
	connector->dpms = DRM_MODE_DPMS_OFF;

	dp->drm = drm;
	zynqmp_dp_create_property(dp);
	zynqmp_dp_attach_property(dp, &connector->base);

	/* This enables interrupts, so should be called after DRM init */
	ret = zynqmp_dp_init_aux(dp);
	if (ret) {
		dev_err(dp->dev, "failed to initialize DP aux");
		goto error_prop;
	}

	return 0;

error_prop:
	zynqmp_dp_destroy_property(dp);
	zynqmp_dp_connector_destroy(&dp->connector);
error_encoder:
	drm_encoder_cleanup(&dp->encoder);
	return ret;
}

void zynqmp_dp_unbind(struct device *dev, struct device *master, void *data)
{
	struct zynqmp_dpsub *dpsub = dev_get_drvdata(dev);
	struct zynqmp_dp *dp = dpsub->dp;

	zynqmp_dp_exit_aux(dp);
	zynqmp_dp_destroy_property(dp);
	zynqmp_dp_connector_destroy(&dp->connector);
	drm_encoder_cleanup(&dp->encoder);
}

/*
 * Platform functions
 */

static void zynqmp_dp_hpd_work_func(struct work_struct *work)
{
	struct zynqmp_dp *dp;

	dp = container_of(work, struct zynqmp_dp, hpd_work.work);

	if (dp->drm)
		drm_helper_hpd_irq_event(dp->drm);
}

static irqreturn_t zynqmp_dp_irq_handler(int irq, void *data)
{
	struct zynqmp_dp *dp = (struct zynqmp_dp *)data;
	struct zynqmp_disp *disp = dp->dpsub->disp;
	u32 status, mask;

	status = zynqmp_dp_read(dp->iomem, ZYNQMP_DP_SUB_TX_INTR_STATUS);
	mask = zynqmp_dp_read(dp->iomem, ZYNQMP_DP_SUB_TX_INTR_MASK);
	if (!(status & ~mask))
		return IRQ_NONE;

	/* dbg for diagnostic, but not much that the driver can do */
	if (status & ZYNQMP_DP_TX_INTR_CHBUF_UNDERFLW_MASK)
		dev_dbg_ratelimited(dp->dev, "underflow interrupt\n");
	if (status & ZYNQMP_DP_TX_INTR_CHBUF_OVERFLW_MASK)
		dev_dbg_ratelimited(dp->dev, "overflow interrupt\n");

	zynqmp_dp_write(dp->iomem, ZYNQMP_DP_SUB_TX_INTR_STATUS, status);

	/* The DP vblank will not be enabled with remote crtc device */
	if (status & ZYNQMP_DP_TX_INTR_VBLANK_START)
		zynqmp_disp_handle_vblank(disp);

	if (status & ZYNQMP_DP_TX_INTR_HPD_EVENT)
		schedule_delayed_work(&dp->hpd_work, 0);

	if (status & ZYNQMP_DP_TX_INTR_HPD_IRQ) {
		u8 status[DP_LINK_STATUS_SIZE + 2];

		drm_dp_dpcd_read(&dp->aux, DP_SINK_COUNT, status,
				 DP_LINK_STATUS_SIZE + 2);

		if (status[4] & DP_LINK_STATUS_UPDATED ||
		    !drm_dp_clock_recovery_ok(&status[2], dp->mode.lane_cnt) ||
		    !drm_dp_channel_eq_ok(&status[2], dp->mode.lane_cnt)) {
			zynqmp_dp_train_loop(dp);
		}
	}

	return IRQ_HANDLED;
}

int zynqmp_dp_probe(struct platform_device *pdev)
{
	struct zynqmp_dpsub *dpsub;
	struct zynqmp_dp *dp;
	struct resource *res;
	unsigned int i;
	int irq, ret;

	dp = devm_kzalloc(&pdev->dev, sizeof(*dp), GFP_KERNEL);
	if (!dp)
		return -ENOMEM;

	dp->dpms = DRM_MODE_DPMS_OFF;
	dp->dev = &pdev->dev;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "dp");
	dp->iomem = devm_ioremap_resource(dp->dev, res);
	if (IS_ERR(dp->iomem))
		return PTR_ERR(dp->iomem);

	zynqmp_dp_write(dp->iomem, ZYNQMP_DP_TX_PHY_POWER_DOWN,
			ZYNQMP_DP_TX_PHY_POWER_DOWN_ALL);
	zynqmp_dp_set(dp->iomem, ZYNQMP_DP_TX_PHY_CONFIG,
		      ZYNQMP_DP_TX_PHY_CONFIG_ALL_RESET);
	zynqmp_dp_write(dp->iomem, ZYNQMP_DP_TX_FORCE_SCRAMBLER_RESET, 1);
	zynqmp_dp_write(dp->iomem, ZYNQMP_DP_TX_ENABLE, 0);

	for (i = 0; i < ZYNQMP_DP_MAX_LANES; i++) {
		char phy_name[16];

		snprintf(phy_name, sizeof(phy_name), "dp-phy%d", i);
		dp->phy[i] = devm_phy_get(dp->dev, phy_name);
		if (IS_ERR(dp->phy[i])) {
			dev_err(dp->dev, "failed to get phy lane\n");
			ret = PTR_ERR(dp->phy[i]);
			dp->phy[i] = NULL;
			return ret;
		}
	}

	ret = zynqmp_dp_init_phy(dp);
	if (ret)
		goto error_phy;

	dp->aux.name = "ZynqMP DP AUX";
	dp->aux.dev = dp->dev;
	dp->aux.transfer = zynqmp_dp_aux_transfer;
	ret = drm_dp_aux_register(&dp->aux);
	if (ret < 0) {
		dev_err(dp->dev, "failed to initialize DP aux\n");
		goto error;
	}

	irq = platform_get_irq(pdev, 0);
	if (irq < 0) {
		ret = irq;
		goto error;
	}

	INIT_DELAYED_WORK(&dp->hpd_work, zynqmp_dp_hpd_work_func);
	ret = devm_request_threaded_irq(dp->dev, irq, NULL,
					zynqmp_dp_irq_handler, IRQF_ONESHOT,
					dev_name(dp->dev), dp);
	if (ret < 0)
		goto error;

	dpsub = platform_get_drvdata(pdev);
	dpsub->dp = dp;
	dp->dpsub = dpsub;
	zynqmp_dp_debugfs_init(dp);

	return 0;

error:
	drm_dp_aux_unregister(&dp->aux);
error_phy:
	zynqmp_dp_exit_phy(dp);
	return ret;
}

int zynqmp_dp_remove(struct platform_device *pdev)
{
	struct zynqmp_dpsub *dpsub = platform_get_drvdata(pdev);
	struct zynqmp_dp *dp = dpsub->dp;

	zynqmp_dp_debugfs_exit(dp);
	cancel_delayed_work_sync(&dp->hpd_work);
	zynqmp_dp_write(dp->iomem, ZYNQMP_DP_TX_ENABLE, 0);
	drm_dp_aux_unregister(&dp->aux);
	zynqmp_dp_exit_phy(dp);
	dpsub->dp = NULL;

	return 0;
}

/*
 * PM functions
 */

int zynqmp_dp_pm_suspend(struct device *dev)
{
	struct zynqmp_dpsub *dpsub = dev_get_drvdata(dev);

	zynqmp_dp_exit_aux(dpsub->dp);
	zynqmp_dp_exit_phy(dpsub->dp);

	return 0;
}

int zynqmp_dp_pm_resume(struct device *dev)
{
	struct zynqmp_dpsub *dpsub = dev_get_drvdata(dev);

	zynqmp_dp_init_phy(dpsub->dp);
	zynqmp_dp_init_aux(dpsub->dp);
	drm_helper_hpd_irq_event(dpsub->dp->connector.dev);

	return 0;
}
