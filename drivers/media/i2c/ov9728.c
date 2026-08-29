// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2020 Intel Corporation.

#include <linux/acpi.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>

#include <media/v4l2-cci.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-fwnode.h>

#define OV9728_LINK_FREQ_180MHZ		180000000ULL
#define OV9728_SCLK			36000000LL
#define OV9728_MCLK			19200000
/* ov9728 only support 1-lane mipi output */
#define OV9728_DATA_LANES		1
#define OV9728_RGB_DEPTH		10

#define OV9728_NATIVE_WIDTH		1296
#define OV9728_NATIVE_HEIGHT		736

#define OV9728_REG_CHIP_ID		0x300a
#define OV9728_CHIP_ID			0x9728
#define OV9728_DEFAULT_I2C_ADDR		0x36

#define OV9728_REG_MODE_SELECT		0x0100
#define OV9728_MODE_STANDBY		0x00
#define OV9728_MODE_STREAMING		0x01

/* vertical-timings from sensor */
#define OV9728_REG_VTS			0x380e
#define OV9728_VTS_30FPS		0x0368
#define OV9728_VTS_30FPS_MIN		0x0368
#define OV9728_VTS_MAX			0x7fff

/* horizontal-timings from sensor */
#define OV9728_REG_HTS			0x380c

/* Exposure controls from sensor */
#define OV9728_REG_EXPOSURE		0x3500
#define OV9728_EXPOSURE_MIN		4
#define OV9728_EXPOSURE_MAX_MARGIN	4
#define OV9728_EXPOSURE_STEP		1

/* Analog gain controls from sensor */
#define OV9728_REG_ANALOG_GAIN		0x350a
#define OV9728_ANAL_GAIN_MIN		16
#define OV9728_ANAL_GAIN_MAX		248
#define OV9728_ANAL_GAIN_STEP		1

/* Digital gain controls from sensor */
#define OV9728_REG_MWB_R_GAIN		0x5180
#define OV9728_REG_MWB_G_GAIN		0x5182
#define OV9728_REG_MWB_B_GAIN		0x5184
#define OV9728_DGTL_GAIN_MIN		256
#define OV9728_DGTL_GAIN_MAX		1023
#define OV9728_DGTL_GAIN_STEP		1
#define OV9728_DGTL_GAIN_DEFAULT	256

/* Test Pattern Control */
#define OV9728_REG_TEST_PATTERN		0x5080
#define OV9728_TEST_PATTERN_ENABLE	BIT(7)
#define OV9728_TEST_PATTERN_BAR_SHIFT	2

/* Group Access */
#define OV9728_REG_GROUP_ACCESS		0x3208
#define OV9728_GROUP_HOLD_START		0x0
#define OV9728_GROUP_HOLD_END		0x10
#define OV9728_GROUP_HOLD_LAUNCH	0xa0

enum {
	OV9728_LINK_FREQ_180MHZ_INDEX,
};

struct ov9728_reg {
	u16 address;
	u8 val;
};

struct ov9728_reg_list {
	u32 num_of_regs;
	const struct ov9728_reg *regs;
};

struct ov9728_link_freq_config {
	const struct ov9728_reg_list reg_list;
};

struct ov9728_mode {
	/* Frame width in pixels */
	u32 width;

	/* Frame height in pixels */
	u32 height;

	/* Horizontal timining size */
	u32 hts;

	/* Default vertical timining size */
	u32 vts_def;

	/* Min vertical timining size */
	u32 vts_min;

	/* Link frequency needed for this resolution */
	u32 link_freq_index;

	/* Sensor register settings for this resolution */
	const struct ov9728_reg_list reg_list;
};

static const struct ov9728_reg mipi_data_rate_360mbps[] = {
	{0x3030, 0x19},
	{0x3080, 0x02},
	{0x3081, 0x4b},
	{0x3082, 0x04},
	{0x3083, 0x00},
	{0x3084, 0x02},
	{0x3085, 0x01},
	{0x3086, 0x01},
	{0x3089, 0x01},
	{0x308a, 0x00},
	{0x301e, 0x15},
	{0x3103, 0x01},
};

static const struct ov9728_reg mode_1296x736_regs[] = {
	/*
	 * Full-res 1296x736 (native 1MP readout), no binning:
	 * 0x0383/0x0385/0x0387 = 0x01 => X/Y increment = 1 (no subsampling)
	 */
	{0x0344, 0x00},
	{0x0345, 0x00},
	{0x0346, 0x00},
	{0x0347, 0x00},
	{0x0348, 0x05},
	{0x0349, 0x0f},
	{0x034a, 0x02},
	{0x034b, 0xdf},
	{0x034c, 0x05},
	{0x034d, 0x10},
	{0x034e, 0x02},
	{0x034f, 0xe0},
	{0x4908, 0x10},
	{0x4909, 0x04},
	{0x3811, 0x08},
	{0x3813, 0x02},
	{0x0340, 0x03},
	{0x0341, 0x68},
	{0x0342, 0x05},
	{0x0343, 0x60},
	{0x0301, 0x0a},
	{0x0303, 0x02},
	{0x0305, 0x02},
	{0x0307, 0x4b},
	{0x0310, 0x00},
	{0x0202, 0x01},
	{0x0203, 0x80},
	{0x0205, 0x3f},
	{0x0383, 0x01},
	{0x4501, 0x08},
	{0x0385, 0x01},
	{0x0387, 0x01},
	{0x3821, 0x00},
	{0x4501, 0x08},
	{0x3820, 0xa0},
	{0x4801, 0x0f},
	{0x4801, 0x8f},
	{0x4814, 0x2b},
	{0x4307, 0x3a},
	{0x370a, 0x23},
	{0x5000, 0x06},
	{0x5001, 0x73},
};

static const char * const ov9728_test_pattern_menu[] = {
	"Disabled",
	"Standard Color Bar",
	"Top-Bottom Darker Color Bar",
	"Right-Left Darker Color Bar",
	"Bottom-Top Darker Color Bar",
};

static const s64 link_freq_menu_items[] = {
	OV9728_LINK_FREQ_180MHZ,
};

static const struct ov9728_link_freq_config link_freq_configs[] = {
	[OV9728_LINK_FREQ_180MHZ_INDEX] = {
		.reg_list = {
			.num_of_regs = ARRAY_SIZE(mipi_data_rate_360mbps),
			.regs = mipi_data_rate_360mbps,
		}
	},
};

static const struct ov9728_mode supported_modes[] = {
	{
		.width = OV9728_NATIVE_WIDTH,
		.height = OV9728_NATIVE_HEIGHT,
		.hts = 0x0560,
		.vts_def = OV9728_VTS_30FPS,
		.vts_min = OV9728_VTS_30FPS_MIN,
		.reg_list = {
			.num_of_regs = ARRAY_SIZE(mode_1296x736_regs),
			.regs = mode_1296x736_regs,
		},
		.link_freq_index = OV9728_LINK_FREQ_180MHZ_INDEX,
	},
};

struct ov9728 {
	struct device *dev;
	struct regmap *regmap;
	struct clk *clk;
	struct gpio_desc *reset_gpio;
	struct gpio_desc *powerdown_gpio;

	struct v4l2_subdev sd;
	struct media_pad pad;
	struct v4l2_ctrl_handler ctrl_handler;

	/* V4L2 Controls */
	struct v4l2_ctrl *link_freq;
	struct v4l2_ctrl *pixel_rate;
	struct v4l2_ctrl *vblank;
	struct v4l2_ctrl *hblank;
	struct v4l2_ctrl *exposure;

	/* Current mode */
	const struct ov9728_mode *cur_mode;

	/* To serialize asynchronous callbacks */
	struct mutex mutex;
};

static inline struct ov9728 *to_ov9728(struct v4l2_subdev *subdev)
{
	return container_of(subdev, struct ov9728, sd);
}

static u64 to_pixel_rate(u32 f_index)
{
	u64 pixel_rate = link_freq_menu_items[f_index] * 2 * OV9728_DATA_LANES;

	do_div(pixel_rate, OV9728_RGB_DEPTH);

	return pixel_rate;
}

static u64 to_pixels_per_line(u32 hts, u32 f_index)
{
	u64 ppl = hts * to_pixel_rate(f_index);

	do_div(ppl, OV9728_SCLK);

	return ppl;
}

static int ov9728_read_reg(struct ov9728 *ov9728, u16 reg, u16 len, u32 *val)
{
	u32 cci_reg;
	u64 value;
	int ret;

	switch (len) {
	case 1:
		cci_reg = CCI_REG8(reg);
		break;
	case 2:
		cci_reg = CCI_REG16(reg);
		break;
	case 3:
		cci_reg = CCI_REG24(reg);
		break;
	case 4:
		cci_reg = CCI_REG32(reg);
		break;
	default:
		return -EINVAL;
	}

	ret = cci_read(ov9728->regmap, cci_reg, &value, NULL);
	if (!ret)
		*val = value;

	return ret;
}

static int ov9728_read_chip_id_at_addr(struct i2c_adapter *adapter, u16 addr,
					       u32 *val)
{
	u8 addr_buf[2] = {
		OV9728_REG_CHIP_ID >> 8,
		OV9728_REG_CHIP_ID & 0xff,
	};
	u8 data_buf[2] = { 0 };
	struct i2c_msg msgs[2] = {
		{
			.addr = addr,
			.flags = 0,
			.len = sizeof(addr_buf),
			.buf = addr_buf,
		}, {
			.addr = addr,
			.flags = I2C_M_RD,
			.len = sizeof(data_buf),
			.buf = data_buf,
		},
	};
	int ret;

	ret = i2c_transfer(adapter, msgs, ARRAY_SIZE(msgs));
	if (ret != ARRAY_SIZE(msgs))
		return ret < 0 ? ret : -EIO;

	*val = (data_buf[0] << 8) | data_buf[1];

	return 0;
}

static int ov9728_write_reg(struct ov9728 *ov9728, u16 reg, u16 len, u32 val)
{
	u32 cci_reg;

	switch (len) {
	case 1:
		cci_reg = CCI_REG8(reg);
		break;
	case 2:
		cci_reg = CCI_REG16(reg);
		break;
	case 3:
		cci_reg = CCI_REG24(reg);
		break;
	case 4:
		cci_reg = CCI_REG32(reg);
		break;
	default:
		return -EINVAL;
	}

	return cci_write(ov9728->regmap, cci_reg, val, NULL);
}

static int ov9728_write_reg_list(struct ov9728 *ov9728,
				 const struct ov9728_reg_list *r_list)
{
	unsigned int i;
	int ret;

	for (i = 0; i < r_list->num_of_regs; i++) {
		ret = ov9728_write_reg(ov9728, r_list->regs[i].address, 1,
				       r_list->regs[i].val);
		if (ret) {
			dev_err_ratelimited(ov9728->dev,
					    "write reg 0x%4.4x return err = %d",
					    r_list->regs[i].address, ret);
			return ret;
		}
	}

	return 0;
}

static int ov9728_update_digital_gain(struct ov9728 *ov9728, u32 d_gain)
{
	int ret;

	ret = ov9728_write_reg(ov9728, OV9728_REG_GROUP_ACCESS, 1,
			       OV9728_GROUP_HOLD_START);
	if (ret)
		return ret;

	ret = ov9728_write_reg(ov9728, OV9728_REG_MWB_R_GAIN, 2, d_gain);
	if (ret)
		return ret;

	ret = ov9728_write_reg(ov9728, OV9728_REG_MWB_G_GAIN, 2, d_gain);
	if (ret)
		return ret;

	ret = ov9728_write_reg(ov9728, OV9728_REG_MWB_B_GAIN, 2, d_gain);
	if (ret)
		return ret;

	ret = ov9728_write_reg(ov9728, OV9728_REG_GROUP_ACCESS, 1,
			       OV9728_GROUP_HOLD_END);
	if (ret)
		return ret;

	ret = ov9728_write_reg(ov9728, OV9728_REG_GROUP_ACCESS, 1,
			       OV9728_GROUP_HOLD_LAUNCH);
	return ret;
}

static int ov9728_test_pattern(struct ov9728 *ov9728, u32 pattern)
{
	if (pattern)
		pattern = (pattern - 1) << OV9728_TEST_PATTERN_BAR_SHIFT |
			OV9728_TEST_PATTERN_ENABLE;

	return ov9728_write_reg(ov9728, OV9728_REG_TEST_PATTERN, 1, pattern);
}

static int ov9728_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct ov9728 *ov9728 = container_of(ctrl->handler,
					     struct ov9728, ctrl_handler);
	s64 exposure_max;
	int ret = 0;

	/* Propagate change of current control to all related controls */
	if (ctrl->id == V4L2_CID_VBLANK) {
		/* Update max exposure while meeting expected vblanking */
		exposure_max = ov9728->cur_mode->height + ctrl->val -
			OV9728_EXPOSURE_MAX_MARGIN;
		__v4l2_ctrl_modify_range(ov9728->exposure,
					 ov9728->exposure->minimum,
					 exposure_max, ov9728->exposure->step,
					 exposure_max);
	}

	/* V4L2 controls values will be applied only when power is already up */
	if (!pm_runtime_get_if_in_use(ov9728->dev))
		return 0;

	switch (ctrl->id) {
	case V4L2_CID_ANALOGUE_GAIN:
		ret = ov9728_write_reg(ov9728, OV9728_REG_ANALOG_GAIN,
				       2, ctrl->val);
		break;

	case V4L2_CID_DIGITAL_GAIN:
		ret = ov9728_update_digital_gain(ov9728, ctrl->val);
		break;

	case V4L2_CID_EXPOSURE:
		/* 4 least significant bits of expsoure are fractional part */
		ret = ov9728_write_reg(ov9728, OV9728_REG_EXPOSURE,
				       3, ctrl->val << 4);
		break;

	case V4L2_CID_VBLANK:
		ret = ov9728_write_reg(ov9728, OV9728_REG_VTS, 2,
				       ov9728->cur_mode->height + ctrl->val);
		break;

	case V4L2_CID_TEST_PATTERN:
		ret = ov9728_test_pattern(ov9728, ctrl->val);
		break;

	default:
		ret = -EINVAL;
		break;
	}

	pm_runtime_put(ov9728->dev);

	return ret;
}

static const struct v4l2_ctrl_ops ov9728_ctrl_ops = {
	.s_ctrl = ov9728_set_ctrl,
};

static int ov9728_init_controls(struct ov9728 *ov9728)
{
	struct v4l2_ctrl_handler *ctrl_hdlr;
	const struct ov9728_mode *cur_mode;
	s64 exposure_max, h_blank, pixel_rate;
	u32 vblank_min, vblank_max, vblank_default;
	int ret, size;

	ctrl_hdlr = &ov9728->ctrl_handler;
	ret = v4l2_ctrl_handler_init(ctrl_hdlr, 8);
	if (ret)
		return ret;

	ctrl_hdlr->lock = &ov9728->mutex;
	cur_mode = ov9728->cur_mode;
	size = ARRAY_SIZE(link_freq_menu_items);
	ov9728->link_freq = v4l2_ctrl_new_int_menu(ctrl_hdlr, &ov9728_ctrl_ops,
						   V4L2_CID_LINK_FREQ,
						   size - 1, 0,
						   link_freq_menu_items);
	if (ov9728->link_freq)
		ov9728->link_freq->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	pixel_rate = to_pixel_rate(OV9728_LINK_FREQ_180MHZ_INDEX);
	ov9728->pixel_rate = v4l2_ctrl_new_std(ctrl_hdlr, &ov9728_ctrl_ops,
					       V4L2_CID_PIXEL_RATE, 0,
					       pixel_rate, 1, pixel_rate);
	vblank_min = cur_mode->vts_min - cur_mode->height;
	vblank_max = OV9728_VTS_MAX - cur_mode->height;
	vblank_default = cur_mode->vts_def - cur_mode->height;
	ov9728->vblank = v4l2_ctrl_new_std(ctrl_hdlr, &ov9728_ctrl_ops,
					   V4L2_CID_VBLANK, vblank_min,
					   vblank_max, 1, vblank_default);
	h_blank = to_pixels_per_line(cur_mode->hts, cur_mode->link_freq_index);
	h_blank -= cur_mode->width;
	ov9728->hblank = v4l2_ctrl_new_std(ctrl_hdlr, &ov9728_ctrl_ops,
					   V4L2_CID_HBLANK, h_blank, h_blank, 1,
					   h_blank);
	if (ov9728->hblank)
		ov9728->hblank->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	v4l2_ctrl_new_std(ctrl_hdlr, &ov9728_ctrl_ops, V4L2_CID_ANALOGUE_GAIN,
			  OV9728_ANAL_GAIN_MIN, OV9728_ANAL_GAIN_MAX,
			  OV9728_ANAL_GAIN_STEP, OV9728_ANAL_GAIN_MIN);
	v4l2_ctrl_new_std(ctrl_hdlr, &ov9728_ctrl_ops, V4L2_CID_DIGITAL_GAIN,
			  OV9728_DGTL_GAIN_MIN, OV9728_DGTL_GAIN_MAX,
			  OV9728_DGTL_GAIN_STEP, OV9728_DGTL_GAIN_DEFAULT);
	exposure_max = ov9728->cur_mode->vts_def - OV9728_EXPOSURE_MAX_MARGIN;
	ov9728->exposure = v4l2_ctrl_new_std(ctrl_hdlr, &ov9728_ctrl_ops,
					     V4L2_CID_EXPOSURE,
					     OV9728_EXPOSURE_MIN, exposure_max,
					     OV9728_EXPOSURE_STEP,
					     exposure_max);
	v4l2_ctrl_new_std_menu_items(ctrl_hdlr, &ov9728_ctrl_ops,
				     V4L2_CID_TEST_PATTERN,
				     ARRAY_SIZE(ov9728_test_pattern_menu) - 1,
				     0, 0, ov9728_test_pattern_menu);
	if (ctrl_hdlr->error)
		return ctrl_hdlr->error;

	ov9728->sd.ctrl_handler = ctrl_hdlr;

	return 0;
}

static void ov9728_update_pad_format(const struct ov9728_mode *mode,
				     struct v4l2_mbus_framefmt *fmt)
{
	fmt->width = mode->width;
	fmt->height = mode->height;
	fmt->code = MEDIA_BUS_FMT_SGRBG10_1X10;
	fmt->field = V4L2_FIELD_NONE;
	fmt->colorspace = V4L2_COLORSPACE_RAW;
	fmt->ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
	fmt->quantization = V4L2_QUANTIZATION_FULL_RANGE;
	fmt->xfer_func = V4L2_XFER_FUNC_NONE;
}

static int ov9728_start_streaming(struct ov9728 *ov9728)
{
	const struct ov9728_reg_list *reg_list;
	int link_freq_index, ret;

	link_freq_index = ov9728->cur_mode->link_freq_index;
	reg_list = &link_freq_configs[link_freq_index].reg_list;
	ret = ov9728_write_reg_list(ov9728, reg_list);
	if (ret) {
		dev_err(ov9728->dev, "failed to set plls");
		return ret;
	}

	reg_list = &ov9728->cur_mode->reg_list;
	ret = ov9728_write_reg_list(ov9728, reg_list);
	if (ret) {
		dev_err(ov9728->dev, "failed to set mode");
		return ret;
	}

	ret = __v4l2_ctrl_handler_setup(ov9728->sd.ctrl_handler);
	if (ret)
		return ret;

	ret = ov9728_write_reg(ov9728, OV9728_REG_MODE_SELECT,
			       1, OV9728_MODE_STREAMING);
	if (ret)
		dev_err(ov9728->dev, "failed to start stream");

	return ret;
}

static void ov9728_stop_streaming(struct ov9728 *ov9728)
{
	if (ov9728_write_reg(ov9728, OV9728_REG_MODE_SELECT,
			     1, OV9728_MODE_STANDBY))
		dev_err(ov9728->dev, "failed to stop stream");
}

static int ov9728_enable_streams(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state, u32 pad,
				 u64 streams_mask)
{
	struct ov9728 *ov9728 = to_ov9728(sd);
	int ret;

	mutex_lock(&ov9728->mutex);

	ret = pm_runtime_resume_and_get(ov9728->dev);
	if (ret < 0)
		goto unlock;

	ret = ov9728_start_streaming(ov9728);
	if (ret) {
		ov9728_stop_streaming(ov9728);
		pm_runtime_put(ov9728->dev);
	}

unlock:
	mutex_unlock(&ov9728->mutex);

	return ret;
}

static int ov9728_disable_streams(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state, u32 pad,
				  u64 streams_mask)
{
	struct ov9728 *ov9728 = to_ov9728(sd);

	mutex_lock(&ov9728->mutex);
	ov9728_stop_streaming(ov9728);
	pm_runtime_put(ov9728->dev);
	mutex_unlock(&ov9728->mutex);

	return 0;
}

static int ov9728_set_format(struct v4l2_subdev *sd,
			     struct v4l2_subdev_state *sd_state,
			     struct v4l2_subdev_format *fmt)
{
	struct ov9728 *ov9728 = to_ov9728(sd);
	const struct ov9728_mode *mode;
	s32 vblank_def, h_blank;

	mode = v4l2_find_nearest_size(supported_modes,
				      ARRAY_SIZE(supported_modes), width,
				      height, fmt->format.width,
				      fmt->format.height);

	mutex_lock(&ov9728->mutex);
	ov9728_update_pad_format(mode, &fmt->format);
	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY) {
		*v4l2_subdev_state_get_format(sd_state, fmt->pad) = fmt->format;
	} else {
		ov9728->cur_mode = mode;
		__v4l2_ctrl_s_ctrl(ov9728->link_freq, mode->link_freq_index);
		__v4l2_ctrl_s_ctrl_int64(ov9728->pixel_rate,
					 to_pixel_rate(mode->link_freq_index));

		/* Update limits and set FPS to default */
		vblank_def = mode->vts_def - mode->height;
		__v4l2_ctrl_modify_range(ov9728->vblank,
					 mode->vts_min - mode->height,
					 OV9728_VTS_MAX - mode->height, 1,
					 vblank_def);
		__v4l2_ctrl_s_ctrl(ov9728->vblank, vblank_def);
		h_blank = to_pixels_per_line(mode->hts, mode->link_freq_index) -
			mode->width;
		__v4l2_ctrl_modify_range(ov9728->hblank, h_blank, h_blank, 1,
					 h_blank);
	}

	mutex_unlock(&ov9728->mutex);

	return 0;
}

static int ov9728_get_format(struct v4l2_subdev *sd,
			     struct v4l2_subdev_state *sd_state,
			     struct v4l2_subdev_format *fmt)
{
	struct ov9728 *ov9728 = to_ov9728(sd);

	mutex_lock(&ov9728->mutex);
	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY)
		fmt->format = *v4l2_subdev_state_get_format(sd_state,
						    fmt->pad);
	else
		ov9728_update_pad_format(ov9728->cur_mode, &fmt->format);

	mutex_unlock(&ov9728->mutex);

	return 0;
}

static int ov9728_enum_mbus_code(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *sd_state,
				 struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->index > 0)
		return -EINVAL;

	code->code = MEDIA_BUS_FMT_SGRBG10_1X10;

	return 0;
}

static int ov9728_enum_frame_size(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *sd_state,
				  struct v4l2_subdev_frame_size_enum *fse)
{
	if (fse->index >= ARRAY_SIZE(supported_modes))
		return -EINVAL;

	if (fse->code != MEDIA_BUS_FMT_SGRBG10_1X10)
		return -EINVAL;

	fse->min_width = supported_modes[fse->index].width;
	fse->max_width = fse->min_width;
	fse->min_height = supported_modes[fse->index].height;
	fse->max_height = fse->min_height;

	return 0;
}

static int ov9728_get_selection(struct v4l2_subdev *sd,
				struct v4l2_subdev_state *sd_state,
				struct v4l2_subdev_selection *sel)
{
	if (sel->pad)
		return -EINVAL;

	switch (sel->target) {
	case V4L2_SEL_TGT_CROP:
	case V4L2_SEL_TGT_CROP_DEFAULT:
	case V4L2_SEL_TGT_CROP_BOUNDS:
	case V4L2_SEL_TGT_NATIVE_SIZE:
		sel->r.left = 0;
		sel->r.top = 0;
		sel->r.width = OV9728_NATIVE_WIDTH;
		sel->r.height = OV9728_NATIVE_HEIGHT;
		return 0;
	default:
		return -EINVAL;
	}
}

static int ov9728_init_state(struct v4l2_subdev *sd,
			     struct v4l2_subdev_state *state)
{
	ov9728_update_pad_format(&supported_modes[0],
				 v4l2_subdev_state_get_format(state, 0));

	return 0;
}

static const struct v4l2_subdev_video_ops ov9728_video_ops = {
	.s_stream = v4l2_subdev_s_stream_helper,
};

static const struct v4l2_subdev_pad_ops ov9728_pad_ops = {
	.set_fmt = ov9728_set_format,
	.get_fmt = ov9728_get_format,
	.enum_mbus_code = ov9728_enum_mbus_code,
	.enum_frame_size = ov9728_enum_frame_size,
	.get_selection = ov9728_get_selection,
	.enable_streams = ov9728_enable_streams,
	.disable_streams = ov9728_disable_streams,
};

static const struct v4l2_subdev_ops ov9728_subdev_ops = {
	.video = &ov9728_video_ops,
	.pad = &ov9728_pad_ops,
};

static const struct media_entity_operations ov9728_subdev_entity_ops = {
	.link_validate = v4l2_subdev_link_validate,
};

static const struct v4l2_subdev_internal_ops ov9728_internal_ops = {
	.init_state = ov9728_init_state,
};

static int ov9728_power_on(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct ov9728 *ov9728 = to_ov9728(sd);
	int ret;

	ret = clk_prepare_enable(ov9728->clk);
	if (ret)
		return ret;

	gpiod_set_value_cansleep(ov9728->powerdown_gpio, 0);
	gpiod_set_value_cansleep(ov9728->reset_gpio, 0);
	usleep_range(20000, 25000);

	return 0;
}

static int ov9728_power_off(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct ov9728 *ov9728 = to_ov9728(sd);

	gpiod_set_value_cansleep(ov9728->reset_gpio, 1);
	gpiod_set_value_cansleep(ov9728->powerdown_gpio, 1);

	clk_disable_unprepare(ov9728->clk);

	return 0;
}

static int ov9728_identify_module(struct ov9728 *ov9728)
{
	struct i2c_client *client = v4l2_get_subdevdata(&ov9728->sd);
	int ret;
	u32 val;

	ret = ov9728_read_reg(ov9728, OV9728_REG_CHIP_ID, 2, &val);
	if (ret) {
		if (ret == -EREMOTEIO && client->addr != OV9728_DEFAULT_I2C_ADDR) {
			int alt_ret;
			u32 alt_val;

			alt_ret = ov9728_read_chip_id_at_addr(client->adapter,
							      OV9728_DEFAULT_I2C_ADDR,
							      &alt_val);
			if (alt_ret)
				dev_info(ov9728->dev,
					 "no chip-id response at fallback I2C address 0x%02x: %d\n",
					 OV9728_DEFAULT_I2C_ADDR, alt_ret);
			else
				dev_info(ov9728->dev,
					 "fallback I2C address 0x%02x returned chip id 0x%04x\n",
					 OV9728_DEFAULT_I2C_ADDR, alt_val);
		}

		return ret;
	}

	if (val != OV9728_CHIP_ID) {
		dev_err(ov9728->dev, "chip id mismatch: %x!=%x",
			OV9728_CHIP_ID, val);
		return -ENXIO;
	}

	return 0;
}

static int ov9728_check_hwcfg(struct device *dev)
{
	struct fwnode_handle *ep;
	struct fwnode_handle *fwnode = dev_fwnode(dev);
	struct v4l2_fwnode_endpoint bus_cfg = {
		.bus_type = V4L2_MBUS_CSI2_DPHY
	};
	int ret;
	unsigned int i, j;

	if (!fwnode)
		return -ENXIO;

	ep = fwnode_graph_get_next_endpoint(fwnode, NULL);
	if (!ep) {
		dev_info(dev, "no firmware endpoint, using ACPI defaults: %u lane, %lld Hz link frequency\n",
			 OV9728_DATA_LANES,
			 link_freq_menu_items[OV9728_LINK_FREQ_180MHZ_INDEX]);
		return 0;
	}

	ret = v4l2_fwnode_endpoint_alloc_parse(ep, &bus_cfg);
	fwnode_handle_put(ep);
	if (ret)
		return ret;

	if (bus_cfg.bus.mipi_csi2.num_data_lanes != OV9728_DATA_LANES) {
		dev_err(dev, "expected %u data lane, got %u",
			OV9728_DATA_LANES,
			bus_cfg.bus.mipi_csi2.num_data_lanes);
		ret = -EINVAL;
		goto check_hwcfg_error;
	}

	if (!bus_cfg.nr_of_link_frequencies) {
		dev_err(dev, "no link frequencies defined");
		ret = -EINVAL;
		goto check_hwcfg_error;
	}

	for (i = 0; i < ARRAY_SIZE(link_freq_menu_items); i++) {
		for (j = 0; j < bus_cfg.nr_of_link_frequencies; j++) {
			if (link_freq_menu_items[i] ==
			    bus_cfg.link_frequencies[j])
				break;
		}

		if (j == bus_cfg.nr_of_link_frequencies) {
			dev_err(dev, "no link frequency %lld supported",
				link_freq_menu_items[i]);
			ret = -EINVAL;
			goto check_hwcfg_error;
		}
	}

check_hwcfg_error:
	v4l2_fwnode_endpoint_free(&bus_cfg);

	return ret;
}

static void ov9728_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct ov9728 *ov9728 = to_ov9728(sd);

	v4l2_async_unregister_subdev(sd);
	v4l2_subdev_cleanup(sd);
	media_entity_cleanup(&sd->entity);
	v4l2_ctrl_handler_free(sd->ctrl_handler);
	pm_runtime_disable(ov9728->dev);
	if (!pm_runtime_status_suspended(ov9728->dev))
		ov9728_power_off(ov9728->dev);
	pm_runtime_set_suspended(ov9728->dev);
	mutex_destroy(&ov9728->mutex);
}

static int ov9728_probe(struct i2c_client *client)
{
	struct ov9728 *ov9728;
	unsigned long freq;
	int ret;

	ret = ov9728_check_hwcfg(&client->dev);
	if (ret) {
		dev_err(&client->dev, "failed to check HW configuration: %d",
			ret);
		return ret;
	}

	ov9728 = devm_kzalloc(&client->dev, sizeof(*ov9728), GFP_KERNEL);
	if (!ov9728)
		return -ENOMEM;

	ov9728->dev = &client->dev;

	ov9728->clk = devm_v4l2_sensor_clk_get(ov9728->dev, NULL);
	if (IS_ERR(ov9728->clk))
		return dev_err_probe(ov9728->dev, PTR_ERR(ov9728->clk),
				     "failed to get clock\n");

	freq = clk_get_rate(ov9728->clk);
	if (freq != OV9728_MCLK)
		return dev_err_probe(ov9728->dev, -EINVAL,
				     "external clock %lu is not supported",
				     freq);

	ov9728->reset_gpio = devm_gpiod_get_optional(ov9728->dev, "reset",
						      GPIOD_OUT_HIGH);
	if (IS_ERR(ov9728->reset_gpio))
		return dev_err_probe(ov9728->dev, PTR_ERR(ov9728->reset_gpio),
				     "failed to get reset GPIO\n");

	ov9728->powerdown_gpio = devm_gpiod_get_optional(ov9728->dev, "powerdown",
							  GPIOD_OUT_HIGH);
	if (IS_ERR(ov9728->powerdown_gpio))
		return dev_err_probe(ov9728->dev, PTR_ERR(ov9728->powerdown_gpio),
				     "failed to get powerdown GPIO\n");

	dev_info(ov9728->dev, "GPIOs: reset=%s powerdown=%s\n",
		 ov9728->reset_gpio ? "present" : "absent",
		 ov9728->powerdown_gpio ? "present" : "absent");

	v4l2_i2c_subdev_init(&ov9728->sd, client, &ov9728_subdev_ops);
	ov9728->regmap = devm_cci_regmap_init_i2c(client, 16);
	if (IS_ERR(ov9728->regmap))
		return dev_err_probe(ov9728->dev, PTR_ERR(ov9728->regmap),
				     "failed to initialize CCI\n");

	pm_runtime_enable(ov9728->dev);

	ret = pm_runtime_resume_and_get(ov9728->dev);
	if (ret)
		goto probe_error_pm_disable;

	dev_info(ov9728->dev, "probing sensor at I2C address 0x%02x\n",
		 client->addr);

	ret = ov9728_identify_module(ov9728);
	if (ret) {
		dev_err(ov9728->dev, "failed to find sensor: %d", ret);
		goto probe_error_pm_put;
	}

	mutex_init(&ov9728->mutex);
	ov9728->cur_mode = &supported_modes[0];
	ret = ov9728_init_controls(ov9728);
	if (ret) {
		dev_err(ov9728->dev, "failed to init controls: %d", ret);
		goto probe_error_v4l2_ctrl_handler_free;
	}

	ov9728->sd.internal_ops = &ov9728_internal_ops;
	ov9728->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	ov9728->sd.entity.ops = &ov9728_subdev_entity_ops;
	ov9728->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;
	ov9728->pad.flags = MEDIA_PAD_FL_SOURCE;
	ret = media_entity_pads_init(&ov9728->sd.entity, 1, &ov9728->pad);
	if (ret) {
		dev_err(ov9728->dev, "failed to init entity pads: %d", ret);
		goto probe_error_v4l2_ctrl_handler_free;
	}

	ret = v4l2_subdev_init_finalize(&ov9728->sd);
	if (ret) {
		dev_err(ov9728->dev, "failed to finalize subdevice: %d", ret);
		goto probe_error_media_entity_cleanup;
	}

	pm_runtime_set_autosuspend_delay(ov9728->dev, 1000);
	pm_runtime_use_autosuspend(ov9728->dev);

	ret = v4l2_async_register_subdev_sensor(&ov9728->sd);
	if (ret < 0) {
		dev_err(ov9728->dev, "failed to register V4L2 subdev: %d",
			ret);
		goto probe_error_subdev_cleanup_pm;
	}

	pm_runtime_put_autosuspend(ov9728->dev);

	return 0;

probe_error_subdev_cleanup_pm:
	v4l2_subdev_cleanup(&ov9728->sd);

probe_error_media_entity_cleanup:
	media_entity_cleanup(&ov9728->sd.entity);

probe_error_v4l2_ctrl_handler_free:
	v4l2_ctrl_handler_free(ov9728->sd.ctrl_handler);
	mutex_destroy(&ov9728->mutex);

probe_error_pm_put:
	pm_runtime_put_sync_suspend(ov9728->dev);

probe_error_pm_disable:
	pm_runtime_disable(ov9728->dev);
	pm_runtime_set_suspended(ov9728->dev);

	return ret;
}

static const struct dev_pm_ops ov9728_pm_ops = {
	SET_RUNTIME_PM_OPS(ov9728_power_off, ov9728_power_on, NULL)
};

static const struct acpi_device_id ov9728_acpi_ids[] = {
	{ "OVTI9728", },
	{}
};

MODULE_DEVICE_TABLE(acpi, ov9728_acpi_ids);

static struct i2c_driver ov9728_i2c_driver = {
	.driver = {
		.name = "ov9728",
		.acpi_match_table = ov9728_acpi_ids,
		.pm = &ov9728_pm_ops,
	},
	.probe = ov9728_probe,
	.remove = ov9728_remove,
};

module_i2c_driver(ov9728_i2c_driver);

MODULE_AUTHOR("Qiu, Tianshu <tian.shu.qiu@intel.com>");
MODULE_AUTHOR("Bingbu Cao <bingbu.cao@intel.com>");
MODULE_DESCRIPTION("OmniVision OV9728 sensor driver (frankenstein scaffold)");
MODULE_LICENSE("GPL v2");