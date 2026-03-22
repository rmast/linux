// SPDX-License-Identifier: GPL-2.0-only
/*
 * mt9m114.c onsemi MT9M114 sensor driver
 *
 * Copyright (c) 2020-2023 Laurent Pinchart <laurent.pinchart@ideasonboard.com>
 * Copyright (c) 2012 Analog Devices Inc.
 *
 * Almost complete rewrite of work by Scott Jiang <Scott.Jiang.Linux@gmail.com>
 * itself based on work from Andrew Chew <achew@nvidia.com>.
 */

#include <linux/clk.h>
#include <linux/acpi.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/jiffies.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pm_runtime.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/types.h>
#include <linux/videodev2.h>
#include <linux/workqueue.h>

#include <media/v4l2-async.h>
#include <media/v4l2-cci.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-mediabus.h>
#include <media/v4l2-subdev.h>

#include "aptina-pll.h"

/* Sysctl registers */
#define MT9M114_CHIP_ID					CCI_REG16(0x0000)
#define MT9M114_COMMAND_REGISTER			CCI_REG16(0x0080)
#define MT9M114_COMMAND_REGISTER_APPLY_PATCH			BIT(0)
#define MT9M114_COMMAND_REGISTER_SET_STATE			BIT(1)
#define MT9M114_COMMAND_REGISTER_REFRESH			BIT(2)
#define MT9M114_COMMAND_REGISTER_WAIT_FOR_EVENT			BIT(3)
#define MT9M114_COMMAND_REGISTER_OK				BIT(15)
#define MT9M114_RESET_AND_MISC_CONTROL			CCI_REG16(0x001a)
#define MT9M114_RESET_SOC					BIT(0)
#define MT9M114_PAD_SLEW				CCI_REG16(0x001e)
#define MT9M114_PAD_SLEW_MIN					0
#define MT9M114_PAD_SLEW_MAX					7
#define MT9M114_PAD_SLEW_DEFAULT				7
#define MT9M114_PAD_CONTROL				CCI_REG16(0x0032)

/* XDMA registers */
#define MT9M114_ACCESS_CTL_STAT				CCI_REG16(0x0982)
#define MT9M114_PHYSICAL_ADDRESS_ACCESS			CCI_REG16(0x098a)
#define MT9M114_LOGICAL_ADDRESS_ACCESS			CCI_REG16(0x098e)
#define MT9M114_MCU_VARIABLE_DATA0			CCI_REG16(0x0990)

/* Sensor Core registers */
#define MT9M114_FRAME_LENGTH_LINES				CCI_REG16(0x300a)
#define MT9M114_COARSE_INTEGRATION_TIME			CCI_REG16(0x3012)
#define MT9M114_FINE_INTEGRATION_TIME			CCI_REG16(0x3014)
#define MT9M114_RESET_REGISTER				CCI_REG16(0x301a)
#define MT9M114_RESET_REGISTER_LOCK_REG				BIT(3)
#define MT9M114_RESET_REGISTER_MASK_BAD				BIT(9)
#define MT9M114_SENSOR_READ_MODE			CCI_REG16(0x3040)
#define MT9M114_SENSOR_READ_MODE_2X2_SUMMING			BIT(10)
#define MT9M114_FLASH					CCI_REG16(0x3046)
#define MT9M114_GREEN1_GAIN				CCI_REG16(0x3056)
#define MT9M114_BLUE_GAIN				CCI_REG16(0x3058)
#define MT9M114_RED_GAIN				CCI_REG16(0x305a)
#define MT9M114_GREEN2_GAIN				CCI_REG16(0x305c)
#define MT9M114_GLOBAL_GAIN				CCI_REG16(0x305e)
#define MT9M114_GAIN_DIGITAL_GAIN(n)				((n) << 12)
#define MT9M114_GAIN_DIGITAL_GAIN_MASK				(0xf << 12)
#define MT9M114_GAIN_ANALOG_GAIN(n)				((n) << 0)
#define MT9M114_GAIN_ANALOG_GAIN_MASK				(0xff << 0)
#define MT9M114_CUSTOMER_REV				CCI_REG16(0x31fe)

/* Monitor registers */
#define MT9M114_MON_MAJOR_VERSION			CCI_REG16(0x8000)
#define MT9M114_MON_MINOR_VERSION			CCI_REG16(0x8002)
#define MT9M114_MON_RELEASE_VERSION			CCI_REG16(0x8004)

/* Auto-Exposure Track registers */
#define MT9M114_AE_TRACK_ALGO				CCI_REG16(0xa804)
#define MT9M114_AE_TRACK_EXEC_AUTOMATIC_EXPOSURE		BIT(0)
#define MT9M114_AE_TRACK_AE_TRACKING_DAMPENING_SPEED	CCI_REG8(0xa80a)
#define MT9M114_AE_RULE_ALGO				CCI_REG16(0xa404)

/* Low-light enhancement registers (from android-ia) */
#define MT9M114_AE_TRACK_MODE				CCI_REG8(0xa800)
#define MT9M114_AE_TRACK_MODE_AUTO_ENABLE			BIT(0)
#define MT9M114_AE_WEIGHT_TABLE_BASE			CCI_REG8(0x3190)
#define MT9M114_AE_TRACK_SPEED				CCI_REG8(0x31ac)
#define MT9M114_GROUPED_PARAMETER_HOLD			CCI_REG16(0x8404)
#define MT9M114_GROUPED_PARAMETER_HOLD_ENABLE			0x0100
#define MT9M114_GROUPED_PARAMETER_HOLD_DISABLE			0x0000
#define MT9M114_AE_TRACK_SPEED_NORMAL				0x00

/* Color Correction Matrix registers */
#define MT9M114_CCM_ALGO				CCI_REG16(0xb404)
#define MT9M114_CCM_EXEC_CALC_CCM_MATRIX			BIT(4)
#define MT9M114_CCM_DELTA_GAIN				CCI_REG8(0xb42a)

/* Camera Control registers */
#define MT9M114_CAM_SENSOR_CFG_Y_ADDR_START		CCI_REG16(0xc800)
#define MT9M114_CAM_SENSOR_CFG_X_ADDR_START		CCI_REG16(0xc802)
#define MT9M114_CAM_SENSOR_CFG_Y_ADDR_END		CCI_REG16(0xc804)
#define MT9M114_CAM_SENSOR_CFG_X_ADDR_END		CCI_REG16(0xc806)
#define MT9M114_CAM_SENSOR_CFG_PIXCLK			CCI_REG32(0xc808)
#define MT9M114_CAM_SENSOR_CFG_ROW_SPEED		CCI_REG16(0xc80c)
#define MT9M114_CAM_SENSOR_CFG_FINE_INTEG_TIME_MIN	CCI_REG16(0xc80e)
#define MT9M114_CAM_SENSOR_CFG_FINE_INTEG_TIME_MAX	CCI_REG16(0xc810)
#define MT9M114_CAM_SENSOR_CFG_FRAME_LENGTH_LINES	CCI_REG16(0xc812)
#define MT9M114_CAM_SENSOR_CFG_FRAME_LENGTH_LINES_MAX		65535
#define MT9M114_CAM_SENSOR_CFG_LINE_LENGTH_PCK		CCI_REG16(0xc814)
#define MT9M114_CAM_SENSOR_CFG_LINE_LENGTH_PCK_MAX		8191
#define MT9M114_CAM_SENSOR_CFG_FINE_CORRECTION		CCI_REG16(0xc816)
#define MT9M114_CAM_SENSOR_CFG_CPIPE_LAST_ROW		CCI_REG16(0xc818)
#define MT9M114_CAM_SENSOR_CFG_REG_0_DATA		CCI_REG16(0xc826)
#define MT9M114_CAM_SENSOR_CONTROL_READ_MODE		CCI_REG16(0xc834)
#define MT9M114_CAM_SENSOR_CONTROL_COARSE_INTEGRATION_TIME	CCI_REG16(0xc83c)
#define MT9M114_CAM_SENSOR_CONTROL_HORZ_MIRROR_EN		BIT(0)
#define MT9M114_CAM_SENSOR_CONTROL_VERT_FLIP_EN			BIT(1)
#define MT9M114_CAM_SENSOR_CONTROL_X_READ_OUT_NORMAL		(0 << 4)
#define MT9M114_CAM_SENSOR_CONTROL_X_READ_OUT_SKIPPING		(1 << 4)
#define MT9M114_CAM_SENSOR_CONTROL_X_READ_OUT_AVERAGE		(2 << 4)
#define MT9M114_CAM_SENSOR_CONTROL_X_READ_OUT_SUMMING		(3 << 4)
#define MT9M114_CAM_SENSOR_CONTROL_X_READ_OUT_MASK		(3 << 4)
#define MT9M114_CAM_SENSOR_CONTROL_Y_READ_OUT_NORMAL		(0 << 8)
#define MT9M114_CAM_SENSOR_CONTROL_Y_READ_OUT_SKIPPING		(1 << 8)
#define MT9M114_CAM_SENSOR_CONTROL_Y_READ_OUT_SUMMING		(3 << 8)
#define MT9M114_CAM_SENSOR_CONTROL_Y_READ_OUT_MASK		(3 << 8)
#define MT9M114_CAM_SENSOR_CONTROL_ANALOG_GAIN		CCI_REG16(0xc836)
#define MT9M114_CAM_SENSOR_CONTROL_COARSE_INTEGRATION_TIME	CCI_REG16(0xc83c)
#define MT9M114_CAM_SENSOR_CONTROL_FINE_INTEGRATION_TIME	CCI_REG16(0xc83e)
#define MT9M114_CAM_MODE_SELECT				CCI_REG8(0xc84c)
#define MT9M114_CAM_MODE_SELECT_NORMAL				(0 << 0)
#define MT9M114_CAM_MODE_SELECT_LENS_CALIBRATION		(1 << 0)
#define MT9M114_CAM_MODE_SELECT_TEST_PATTERN			(2 << 0)
#define MT9M114_CAM_MODE_TEST_PATTERN_SELECT		CCI_REG8(0xc84d)
#define MT9M114_CAM_MODE_TEST_PATTERN_SELECT_SOLID		(1 << 0)
#define MT9M114_CAM_MODE_TEST_PATTERN_SELECT_SOLID_BARS		(4 << 0)
#define MT9M114_CAM_MODE_TEST_PATTERN_SELECT_RANDOM		(5 << 0)
#define MT9M114_CAM_MODE_TEST_PATTERN_SELECT_FADING_BARS	(8 << 0)
#define MT9M114_CAM_MODE_TEST_PATTERN_SELECT_WALKING_1S_10B	(10 << 0)
#define MT9M114_CAM_MODE_TEST_PATTERN_SELECT_WALKING_1S_8B	(11 << 0)
#define MT9M114_CAM_MODE_TEST_PATTERN_RED		CCI_REG16(0xc84e)
#define MT9M114_CAM_MODE_TEST_PATTERN_GREEN		CCI_REG16(0xc850)
#define MT9M114_CAM_MODE_TEST_PATTERN_BLUE		CCI_REG16(0xc852)
#define MT9M114_CAM_CROP_WINDOW_XOFFSET			CCI_REG16(0xc854)
#define MT9M114_CAM_CROP_WINDOW_YOFFSET			CCI_REG16(0xc856)
#define MT9M114_CAM_CROP_WINDOW_WIDTH			CCI_REG16(0xc858)
#define MT9M114_CAM_CROP_WINDOW_HEIGHT			CCI_REG16(0xc85a)
#define MT9M114_CAM_CROP_CROPMODE			CCI_REG8(0xc85c)
#define MT9M114_CAM_CROP_MODE_AE_AUTO_CROP_EN			BIT(0)
#define MT9M114_CAM_CROP_MODE_AWB_AUTO_CROP_EN			BIT(1)
#define MT9M114_CAM_OUTPUT_WIDTH			CCI_REG16(0xc868)
#define MT9M114_CAM_OUTPUT_HEIGHT			CCI_REG16(0xc86a)
#define MT9M114_CAM_OUTPUT_FORMAT			CCI_REG16(0xc86c)
#define MT9M114_CAM_OUTPUT_FORMAT_SWAP_RED_BLUE			BIT(0)
#define MT9M114_CAM_OUTPUT_FORMAT_SWAP_BYTES			BIT(1)
#define MT9M114_CAM_OUTPUT_FORMAT_MONO_ENABLE			BIT(2)
#define MT9M114_CAM_OUTPUT_FORMAT_BT656_ENABLE			BIT(3)
#define MT9M114_CAM_OUTPUT_FORMAT_BT656_CROP_SCALE_DISABLE	BIT(4)
#define MT9M114_CAM_OUTPUT_FORMAT_FVLV_DISABLE			BIT(5)
#define MT9M114_CAM_OUTPUT_FORMAT_FORMAT_YUV			(0 << 8)
#define MT9M114_CAM_OUTPUT_FORMAT_FORMAT_RGB			(1 << 8)
#define MT9M114_CAM_OUTPUT_FORMAT_FORMAT_BAYER			(2 << 8)
#define MT9M114_CAM_OUTPUT_FORMAT_FORMAT_NONE			(3 << 8)
#define MT9M114_CAM_OUTPUT_FORMAT_FORMAT_MASK			(3 << 8)
#define MT9M114_CAM_OUTPUT_FORMAT_BAYER_FORMAT_RAWR10		(0 << 10)
#define MT9M114_CAM_OUTPUT_FORMAT_BAYER_FORMAT_PRELSC_8_2	(1 << 10)
#define MT9M114_CAM_OUTPUT_FORMAT_BAYER_FORMAT_POSTLSC_8_2	(2 << 10)
#define MT9M114_CAM_OUTPUT_FORMAT_BAYER_FORMAT_PROCESSED8	(3 << 10)
#define MT9M114_CAM_OUTPUT_FORMAT_BAYER_FORMAT_MASK		(3 << 10)
#define MT9M114_CAM_OUTPUT_FORMAT_RGB_FORMAT_565RGB		(0 << 12)
#define MT9M114_CAM_OUTPUT_FORMAT_RGB_FORMAT_555RGB		(1 << 12)
#define MT9M114_CAM_OUTPUT_FORMAT_RGB_FORMAT_444xRGB		(2 << 12)
#define MT9M114_CAM_OUTPUT_FORMAT_RGB_FORMAT_444RGBx		(3 << 12)
#define MT9M114_CAM_OUTPUT_FORMAT_RGB_FORMAT_MASK		(3 << 12)
#define MT9M114_CAM_OUTPUT_FORMAT_YUV			CCI_REG16(0xc86e)
#define MT9M114_CAM_OUTPUT_FORMAT_YUV_CLIP			BIT(5)
#define MT9M114_CAM_OUTPUT_FORMAT_YUV_AUV_OFFSET		BIT(4)
#define MT9M114_CAM_OUTPUT_FORMAT_YUV_SELECT_601		BIT(3)
#define MT9M114_CAM_OUTPUT_FORMAT_YUV_NORMALISE			BIT(2)
#define MT9M114_CAM_OUTPUT_FORMAT_YUV_SAMPLING_EVEN_UV		(0 << 0)
#define MT9M114_CAM_OUTPUT_FORMAT_YUV_SAMPLING_ODD_UV		(1 << 0)
#define MT9M114_CAM_OUTPUT_FORMAT_YUV_SAMPLING_EVENU_ODDV	(2 << 0)
#define MT9M114_CAM_OUTPUT_Y_OFFSET			CCI_REG8(0xc870)
#define MT9M114_CAM_AET_AEMODE				CCI_REG8(0xc878)
#define MT9M114_CAM_AET_EXEC_SET_INDOOR				BIT(0)
#define MT9M114_CAM_AET_DISCRETE_FRAMERATE			BIT(1)
#define MT9M114_CAM_AET_ADAPTATIVE_TARGET_LUMA			BIT(2)
#define MT9M114_CAM_AET_ADAPTATIVE_SKIP_FRAMES			BIT(3)
#define MT9M114_CAM_AET_SKIP_FRAMES			CCI_REG8(0xc879)
#define MT9M114_CAM_AET_TARGET_AVERAGE_LUMA		CCI_REG8(0xc87a)
#define MT9M114_CAM_AET_TARGET_AVERAGE_LUMA_DARK	CCI_REG8(0xc87b)
#define MT9M114_CAM_AET_BLACK_CLIPPING_TARGET		CCI_REG16(0xc87c)
#define MT9M114_CAM_AET_AE_MIN_VIRT_INT_TIME_PCLK	CCI_REG16(0xc87e)
#define MT9M114_CAM_AET_AE_MIN_VIRT_DGAIN		CCI_REG16(0xc880)
#define MT9M114_CAM_AET_AE_MAX_VIRT_DGAIN		CCI_REG16(0xc882)
#define MT9M114_CAM_AET_AE_MIN_VIRT_AGAIN		CCI_REG16(0xc884)
#define MT9M114_CAM_AET_AE_MAX_VIRT_AGAIN		CCI_REG16(0xc886)
#define MT9M114_CAM_AET_AE_VIRT_GAIN_TH_EG		CCI_REG16(0xc888)
#define MT9M114_CAM_AET_AE_EG_GATE_PERCENTAGE		CCI_REG8(0xc88a)
#define MT9M114_CAM_AET_FLICKER_FREQ_HZ			CCI_REG8(0xc88b)
#define MT9M114_CAM_AET_MAX_FRAME_RATE			CCI_REG16(0xc88c)
#define MT9M114_CAM_AET_MIN_FRAME_RATE			CCI_REG16(0xc88e)
#define MT9M114_CAM_AET_TARGET_GAIN			CCI_REG16(0xc890)
#define MT9M114_CAM_AWB_CCM_L(n)			CCI_REG16(0xc892 + (n) * 2)
#define MT9M114_CAM_AWB_CCM_M(n)			CCI_REG16(0xc8a4 + (n) * 2)
#define MT9M114_CAM_AWB_CCM_R(n)			CCI_REG16(0xc8b6 + (n) * 2)
#define MT9M114_CAM_AWB_CCM_L_RG_GAIN			CCI_REG16(0xc8c8)
#define MT9M114_CAM_AWB_CCM_L_BG_GAIN			CCI_REG16(0xc8ca)
#define MT9M114_CAM_AWB_CCM_M_RG_GAIN			CCI_REG16(0xc8cc)
#define MT9M114_CAM_AWB_CCM_M_BG_GAIN			CCI_REG16(0xc8ce)
#define MT9M114_CAM_AWB_CCM_R_RG_GAIN			CCI_REG16(0xc8d0)
#define MT9M114_CAM_AWB_CCM_R_BG_GAIN			CCI_REG16(0xc8d2)
#define MT9M114_CAM_AWB_CCM_L_CTEMP			CCI_REG16(0xc8d4)
#define MT9M114_CAM_AWB_CCM_M_CTEMP			CCI_REG16(0xc8d6)
#define MT9M114_CAM_AWB_CCM_R_CTEMP			CCI_REG16(0xc8d8)
#define MT9M114_CAM_AWB_AWB_XSCALE			CCI_REG8(0xc8f2)
#define MT9M114_CAM_AWB_AWB_YSCALE			CCI_REG8(0xc8f3)
#define MT9M114_CAM_AWB_AWB_WEIGHTS(n)			CCI_REG16(0xc8f4 + (n) * 2)
#define MT9M114_CAM_AWB_AWB_XSHIFT_PRE_ADJ		CCI_REG16(0xc904)
#define MT9M114_CAM_AWB_AWB_YSHIFT_PRE_ADJ		CCI_REG16(0xc906)
#define MT9M114_CAM_AWB_AWBMODE				CCI_REG8(0xc909)
#define MT9M114_CAM_AWB_MODE_AUTO				BIT(1)
#define MT9M114_CAM_AWB_MODE_EXCLUSIVE_AE			BIT(0)
#define MT9M114_CAM_AWB_K_R_L				CCI_REG8(0xc90c)
#define MT9M114_CAM_AWB_K_G_L				CCI_REG8(0xc90d)
#define MT9M114_CAM_AWB_K_B_L				CCI_REG8(0xc90e)
#define MT9M114_CAM_AWB_K_R_R				CCI_REG8(0xc90f)
#define MT9M114_CAM_AWB_K_G_R				CCI_REG8(0xc910)
#define MT9M114_CAM_AWB_K_B_R				CCI_REG8(0xc911)
#define MT9M114_CAM_STAT_AWB_CLIP_WINDOW_XSTART		CCI_REG16(0xc914)
#define MT9M114_CAM_STAT_AWB_CLIP_WINDOW_YSTART		CCI_REG16(0xc916)
#define MT9M114_CAM_STAT_AWB_CLIP_WINDOW_XEND		CCI_REG16(0xc918)
#define MT9M114_CAM_STAT_AWB_CLIP_WINDOW_YEND		CCI_REG16(0xc91a)
#define MT9M114_CAM_STAT_AE_INITIAL_WINDOW_XSTART	CCI_REG16(0xc91c)
#define MT9M114_CAM_STAT_AE_INITIAL_WINDOW_YSTART	CCI_REG16(0xc91e)
#define MT9M114_CAM_STAT_AE_INITIAL_WINDOW_XEND		CCI_REG16(0xc920)
#define MT9M114_CAM_STAT_AE_INITIAL_WINDOW_YEND		CCI_REG16(0xc922)
#define MT9M114_CAM_LL_LLMODE				CCI_REG16(0xc924)
#define MT9M114_CAM_LL_START_BRIGHTNESS			CCI_REG16(0xc926)
#define MT9M114_CAM_LL_STOP_BRIGHTNESS			CCI_REG16(0xc928)
#define MT9M114_CAM_LL_START_SATURATION			CCI_REG8(0xc92a)
#define MT9M114_CAM_LL_END_SATURATION			CCI_REG8(0xc92b)
#define MT9M114_CAM_LL_START_DESATURATION		CCI_REG8(0xc92c)
#define MT9M114_CAM_LL_END_DESATURATION			CCI_REG8(0xc92d)
#define MT9M114_CAM_LL_START_DEMOSAICING		CCI_REG8(0xc92e)
#define MT9M114_CAM_LL_START_AP_GAIN			CCI_REG8(0xc92f)
#define MT9M114_CAM_LL_START_AP_THRESH			CCI_REG8(0xc930)
#define MT9M114_CAM_LL_STOP_DEMOSAICING			CCI_REG8(0xc931)
#define MT9M114_CAM_LL_STOP_AP_GAIN			CCI_REG8(0xc932)
#define MT9M114_CAM_LL_STOP_AP_THRESH			CCI_REG8(0xc933)
#define MT9M114_CAM_LL_START_NR_RED			CCI_REG8(0xc934)
#define MT9M114_CAM_LL_START_NR_GREEN			CCI_REG8(0xc935)
#define MT9M114_CAM_LL_START_NR_BLUE			CCI_REG8(0xc936)
#define MT9M114_CAM_LL_START_NR_THRESH			CCI_REG8(0xc937)
#define MT9M114_CAM_LL_STOP_NR_RED			CCI_REG8(0xc938)
#define MT9M114_CAM_LL_STOP_NR_GREEN			CCI_REG8(0xc939)
#define MT9M114_CAM_LL_STOP_NR_BLUE			CCI_REG8(0xc93a)
#define MT9M114_CAM_LL_STOP_NR_THRESH			CCI_REG8(0xc93b)
#define MT9M114_CAM_LL_START_CONTRAST_BM		CCI_REG16(0xc93c)
#define MT9M114_CAM_LL_STOP_CONTRAST_BM			CCI_REG16(0xc93e)
#define MT9M114_CAM_LL_GAMMA				CCI_REG16(0xc940)
#define MT9M114_CAM_LL_START_CONTRAST_GRADIENT		CCI_REG8(0xc942)
#define MT9M114_CAM_LL_STOP_CONTRAST_GRADIENT		CCI_REG8(0xc943)
#define MT9M114_CAM_LL_START_CONTRAST_LUMA_PERCENTAGE	CCI_REG8(0xc944)
#define MT9M114_CAM_LL_STOP_CONTRAST_LUMA_PERCENTAGE	CCI_REG8(0xc945)
#define MT9M114_CAM_LL_START_GAIN_METRIC		CCI_REG16(0xc946)
#define MT9M114_CAM_LL_STOP_GAIN_METRIC			CCI_REG16(0xc948)
#define MT9M114_CAM_LL_START_FADE_TO_BLACK_LUMA		CCI_REG16(0xc94a)
#define MT9M114_CAM_LL_STOP_FADE_TO_BLACK_LUMA		CCI_REG16(0xc94c)
#define MT9M114_CAM_LL_CLUSTER_DC_TH_BM			CCI_REG16(0xc94e)
#define MT9M114_CAM_LL_CLUSTER_DC_GATE_PERCENTAGE	CCI_REG8(0xc950)
#define MT9M114_CAM_LL_SUMMING_SENSITIVITY_FACTOR	CCI_REG8(0xc951)
#define MT9M114_CAM_LL_START_TARGET_LUMA_BM		CCI_REG16(0xc952)
#define MT9M114_CAM_LL_STOP_TARGET_LUMA_BM		CCI_REG16(0xc954)
#define MT9M114_CAM_PGA_PGA_CONTROL			CCI_REG16(0xc95e)
#define MT9M114_CAM_SYSCTL_PLL_ENABLE			CCI_REG8(0xc97e)
#define MT9M114_CAM_SYSCTL_PLL_ENABLE_VALUE			BIT(0)
#define MT9M114_CAM_SYSCTL_PLL_DISABLE_VALUE			0x00
#define MT9M114_CAM_SYSCTL_PLL_DIVIDER_M_N		CCI_REG16(0xc980)
#define MT9M114_CAM_SYSCTL_PLL_DIVIDER_VALUE(m, n)		((((n) - 1) << 8) | (m))
#define MT9M114_CAM_SYSCTL_PLL_DIVIDER_P		CCI_REG16(0xc982)
#define MT9M114_CAM_SYSCTL_PLL_DIVIDER_P_VALUE(p)		(((p) - 1) << 8)
#define MT9M114_CAM_PORT_OUTPUT_CONTROL			CCI_REG16(0xc984)
#define MT9M114_CAM_PORT_PORT_SELECT_PARALLEL			(0 << 0)
#define MT9M114_CAM_PORT_PORT_SELECT_MIPI			(1 << 0)
#define MT9M114_CAM_PORT_CLOCK_SLOWDOWN				BIT(3)
#define MT9M114_CAM_PORT_TRUNCATE_RAW_BAYER			BIT(4)
#define MT9M114_CAM_PORT_PIXCLK_GATE				BIT(5)
#define MT9M114_CAM_PORT_CONT_MIPI_CLK				BIT(6)
#define MT9M114_CAM_PORT_CHAN_NUM(vc)				((vc) << 8)
#define MT9M114_CAM_PORT_MIPI_TIMING_T_HS_ZERO		CCI_REG16(0xc988)
#define MT9M114_CAM_PORT_MIPI_TIMING_T_HS_ZERO_VALUE(n)		((n) << 8)
#define MT9M114_CAM_PORT_MIPI_TIMING_T_HS_EXIT_TRAIL	CCI_REG16(0xc98a)
#define MT9M114_CAM_PORT_MIPI_TIMING_T_HS_EXIT_VALUE(n)		((n) << 8)
#define MT9M114_CAM_PORT_MIPI_TIMING_T_HS_TRAIL_VALUE(n)	((n) << 0)
#define MT9M114_CAM_PORT_MIPI_TIMING_T_CLK_POST_PRE	CCI_REG16(0xc98c)
#define MT9M114_CAM_PORT_MIPI_TIMING_T_CLK_POST_VALUE(n)	((n) << 8)
#define MT9M114_CAM_PORT_MIPI_TIMING_T_CLK_PRE_VALUE(n)		((n) << 0)
#define MT9M114_CAM_PORT_MIPI_TIMING_T_CLK_TRAIL_ZERO	CCI_REG16(0xc98e)
#define MT9M114_CAM_PORT_MIPI_TIMING_T_CLK_TRAIL_VALUE(n)	((n) << 8)
#define MT9M114_CAM_PORT_MIPI_TIMING_T_CLK_ZERO_VALUE(n)	((n) << 0)

/* System Manager registers */
#define MT9M114_SYSMGR_NEXT_STATE			CCI_REG8(0xdc00)
#define MT9M114_SYSMGR_CURRENT_STATE			CCI_REG8(0xdc01)
#define MT9M114_SYSMGR_CMD_STATUS			CCI_REG8(0xdc02)

/* Patch Loader registers */
#define MT9M114_PATCHLDR_LOADER_ADDRESS			CCI_REG16(0xe000)
#define MT9M114_PATCHLDR_PATCH_ID			CCI_REG16(0xe002)
#define MT9M114_PATCHLDR_FIRMWARE_ID			CCI_REG32(0xe004)
#define MT9M114_PATCHLDR_APPLY_STATUS			CCI_REG8(0xe008)
#define MT9M114_PATCHLDR_NUM_PATCHES			CCI_REG8(0xe009)
#define MT9M114_PATCHLDR_PATCH_ID_0			CCI_REG16(0xe00a)
#define MT9M114_PATCHLDR_PATCH_ID_1			CCI_REG16(0xe00c)
#define MT9M114_PATCHLDR_PATCH_ID_2			CCI_REG16(0xe00e)
#define MT9M114_PATCHLDR_PATCH_ID_3			CCI_REG16(0xe010)
#define MT9M114_PATCHLDR_PATCH_ID_4			CCI_REG16(0xe012)
#define MT9M114_PATCHLDR_PATCH_ID_5			CCI_REG16(0xe014)
#define MT9M114_PATCHLDR_PATCH_ID_6			CCI_REG16(0xe016)
#define MT9M114_PATCHLDR_PATCH_ID_7			CCI_REG16(0xe018)

/* SYS_STATE values (for SYSMGR_NEXT_STATE and SYSMGR_CURRENT_STATE) */
#define MT9M114_SYS_STATE_ENTER_CONFIG_CHANGE		0x28
#define MT9M114_SYS_STATE_STREAMING			0x31
#define MT9M114_SYS_STATE_START_STREAMING		0x34
#define MT9M114_SYS_STATE_ENTER_SUSPEND			0x40
#define MT9M114_SYS_STATE_SUSPENDED			0x41
#define MT9M114_SYS_STATE_ENTER_STANDBY			0x50
#define MT9M114_SYS_STATE_STANDBY			0x52
#define MT9M114_SYS_STATE_LEAVE_STANDBY			0x54

/* Result status of last SET_STATE comamnd */
#define MT9M114_SET_STATE_RESULT_ENOERR			0x00
#define MT9M114_SET_STATE_RESULT_EINVAL			0x0c
#define MT9M114_SET_STATE_RESULT_ENOSPC			0x0d

/*
 * The minimum amount of horizontal and vertical blanking is undocumented. The
 * minimum values that have been seen in register lists are 303 and 21, use
 * them.
 *
 * Set the default to achieve full resolution (1296x976 analog crop
 * rectangle, 1280x960 output size) at 30fps with a 48 MHz pixclock.
 */
#define MT9M114_MIN_HBLANK				303
#define MT9M114_MIN_VBLANK				21
#define MT9M114_DEF_HBLANK				308
#define MT9M114_DEF_VBLANK				21

/* Extended VBLANK range for low-light mode (VTS up to ~30000 lines) */
#define MT9M114_MAX_VBLANK_LOWLIGHT			29024U  /* Allow 2+ second exposures */
#define MT9M114_MAX_EXPOSURE_LOWLIGHT			29998U  /* VTS - 2 (hardware requirement) */

#define MT9M114_DEF_FRAME_RATE				30
#define MT9M114_MAX_FRAME_RATE				120
#define MT9M114_MIN_FRAME_RATE_FLOOR			2

#define MT9M114_SMART_METER_INTERVAL_MS			500
#define MT9M114_SMART_METER_HYSTERESIS			(2 * HZ)
#define MT9M114_SMART_METER_BACKLIT_DELTA		24
#define MT9M114_SMART_METER_BACKLIT_SCENE_MIN		64
#define MT9M114_SMART_METER_UNIFORM_DELTA		8

#define MT9M114_DEEP_LOWLIGHT_INTERVAL_MS		500

#define MT9M114_SMART_AE_STATS_AVG_LUMA			CCI_REG8(0x3108)
#define MT9M114_SMART_AE_STATS_CENTER_LUMA		CCI_REG8(0x310a)
#define MT9M114_SMART_BLC_CTRL				CCI_REG8(0x3102)
#define MT9M114_SMART_BLC_ENABLE			BIT(0)

#define MT9M114_DEEP_LOWLIGHT_BINNED_WIDTH		648U
#define MT9M114_DEEP_LOWLIGHT_BINNED_HEIGHT_720		368U
#define MT9M114_DEEP_LOWLIGHT_BINNED_HEIGHT_960		488U
#define MT9M114_DEEP_LOWLIGHT_BINNED_HEIGHT_976		488U
#define MT9M114_DEEP_LOWLIGHT_OUTPUT_WIDTH		1280U
#define MT9M114_DEEP_LOWLIGHT_OUTPUT_WIDTH_1288		1288U
#define MT9M114_DEEP_LOWLIGHT_OUTPUT_WIDTH_1296		1296U
#define MT9M114_DEEP_LOWLIGHT_OUTPUT_HEIGHT_720		720U
#define MT9M114_DEEP_LOWLIGHT_OUTPUT_HEIGHT_960		960U
#define MT9M114_DEEP_LOWLIGHT_OUTPUT_HEIGHT_968		968U
#define MT9M114_DEEP_LOWLIGHT_OUTPUT_HEIGHT_976		976U

#define MT9M114_DEEP_LOWLIGHT_AP_GAIN_SOFT		0x04
#define MT9M114_DEEP_LOWLIGHT_AP_THRESH_SOFT		0x10
#define MT9M114_DEEP_LOWLIGHT_DEMOSAIC_SOFT		0x50

#define MT9M114_DEEP_LOWLIGHT_GAIN_ENTER_PCT		95U
#define MT9M114_DEEP_LOWLIGHT_GAIN_EXIT_PCT		70U
#define MT9M114_DEEP_LOWLIGHT_GAIN_EXIT_RESERVE_PCT	50U
#define MT9M114_DEEP_LOWLIGHT_GAIN_ON_SATURATION_PCT	80U
#define MT9M114_DEEP_LOWLIGHT_GAIN_OFF_SEED_CAP_PCT	45U
#define MT9M114_DEEP_LOWLIGHT_STRICT_GAIN_MAX_FALLBACK	256U
#define MT9M114_DEEP_LOWLIGHT_EXPOSURE_ENTER_PCT	95U
#define MT9M114_DEEP_LOWLIGHT_EXPOSURE_EXIT_PCT		60U
#define MT9M114_DEEP_LOWLIGHT_FPS_MIN			2U
#define MT9M114_DEEP_LOWLIGHT_FPS_MAX			10U
#define MT9M114_DEEP_LOWLIGHT_FPS_ENTER_SEED		5U
#define MT9M114_DEEP_LOWLIGHT_FPS_EXIT_RESERVE		10U
#define MT9M114_DEEP_LOWLIGHT_FPS_EXIT_SEED		10U
#define MT9M114_DEEP_LOWLIGHT_FPS_EXIT_SEED_DARK	5U
#define MT9M114_DEEP_LOWLIGHT_OFF_DARK_GAIN_PCT	35U
#define MT9M114_DEEP_LOWLIGHT_TRANSITION_SCALE		4U
#define MT9M114_DEEP_LOWLIGHT_TRANSITION_SCALE_OFF	2U
#define MT9M114_DEEP_LOWLIGHT_SWITCH_HYSTERESIS		(3 * HZ)
#define MT9M114_DEEP_LOWLIGHT_STRICT_LUMA_ENTER	24U
#define MT9M114_DEEP_LOWLIGHT_LUMA_EXIT		96U
#define MT9M114_DEEP_LOWLIGHT_STRICT_LUMA_ZERO_LOW_GAIN_SAMPLES_DEFAULT	4U
#define MT9M114_DEEP_LOWLIGHT_STALE_LIMIT		8U
#define MT9M114_DEEP_LOWLIGHT_STRICT_STALE_FORCE_OFF_DEFAULT	8U
#define MT9M114_DEEP_LOWLIGHT_REENTRY_COOLDOWN		(5 * HZ)
#define MT9M114_DEEP_LOWLIGHT_FORCE_OFF_REENTRY_COOLDOWN	(HZ)
#define MT9M114_DEEP_LOWLIGHT_INVALID_EXIT_GUARD	(5 * HZ)
#define MT9M114_DEEP_LOWLIGHT_INVALID_ENTER_SAMPLES	3U
#define MT9M114_DEEP_LOWLIGHT_STALE_DUMP_TRIGGER	3U
#define MT9M114_DEEP_LOWLIGHT_ON_SATURATION_SAMPLES	3U
#define MT9M114_DEEP_LOWLIGHT_PENDING_ZERO_STATS_DUMP_TRIGGER	3U
#define MT9M114_DEEP_LOWLIGHT_PENDING_ZERO_STATS_LOW_GAIN_PCT	55U
#define MT9M114_DEEP_LOWLIGHT_PENDING_ZERO_STATS_ENTER_SAMPLES	10U
#define MT9M114_DEEP_LOWLIGHT_ON_ZERO_STATS_TIMEOUT_SAMPLES	12U
#define MT9M114_DEEP_LOWLIGHT_STALE_HARD_FORCE_OFF_MULTIPLIER	2U

#define MT9M114_IFP_OUTPUT_FMT_311C			CCI_REG16(0x311c)
#define MT9M114_IFP_OUTPUT_FMT_311E			CCI_REG16(0x311e)

#define MT9M114_DEF_PIXCLOCK				48000000

#define MT9M114_PIXEL_ARRAY_WIDTH			1296U
#define MT9M114_PIXEL_ARRAY_HEIGHT			976U

/*
 * These values are not well documented and are semi-arbitrary. The pixel array
 * minimum output size is 8 pixels larger than the minimum scaler cropped input
 * width to account for the demosaicing.
 */
#define MT9M114_PIXEL_ARRAY_MIN_OUTPUT_WIDTH		(32U + 8U)
#define MT9M114_PIXEL_ARRAY_MIN_OUTPUT_HEIGHT		(32U + 8U)
#define MT9M114_SCALER_CROPPED_INPUT_WIDTH		32U
#define MT9M114_SCALER_CROPPED_INPUT_HEIGHT		32U

/* Indices into the mt9m114.ifp.tpg array. */
#define MT9M114_TPG_PATTERN				0
#define MT9M114_TPG_RED					1
#define MT9M114_TPG_GREEN				2
#define MT9M114_TPG_BLUE				3

/* -----------------------------------------------------------------------------
 * Data Structures
 */

struct mt9m114_model_info {
	bool state_standby_polling;
};

enum mt9m114_format_flag {
	MT9M114_FMT_FLAG_PARALLEL = BIT(0),
	MT9M114_FMT_FLAG_CSI2 = BIT(1),
};

/* Metering presets for common use cases */
enum mt9m114_metering_preset {
	MT9M114_METERING_PRESET_CENTER = 0,  /* Center-weighted (default) */
	MT9M114_METERING_PRESET_UNIFORM,     /* Uniform (landscape) */
	MT9M114_METERING_PRESET_BACKLIT,     /* Backlit portrait */
	MT9M114_METERING_PRESET_SPOT,        /* Spot metering (macro) */
};

/* 5x5 metering weight tables (registers 0x3190-0x31AB, 25 bytes total) */
static const u8 mt9m114_metering_patterns[][25] = {
	/* Center-Weighted: Emphasize center, gentle falloff */
	[MT9M114_METERING_PRESET_CENTER] = {
		2, 2, 4, 2, 2,
		2, 4, 8, 4, 2,
		4, 8, 8, 8, 4,
		2, 4, 8, 4, 2,
		2, 2, 4, 2, 2,
	},
	/* Uniform: Even weight across frame (landscape mode) */
	[MT9M114_METERING_PRESET_UNIFORM] = {
		4, 4, 4, 4, 4,
		4, 4, 4, 4, 4,
		4, 4, 4, 4, 4,
		4, 4, 4, 4, 4,
		4, 4, 4, 4, 4,
	},
	/* Backlit Portrait: Ignore background, focus on center subject */
	[MT9M114_METERING_PRESET_BACKLIT] = {
		0, 0, 0, 0, 0,
		0, 4, 8, 4, 0,
		0, 8, 8, 8, 0,
		0, 4, 8, 4, 0,
		0, 0, 0, 0, 0,
	},
	/* Spot: Only center 3x3 (macro photography) */
	[MT9M114_METERING_PRESET_SPOT] = {
		0, 0, 0, 0, 0,
		0, 0, 0, 0, 0,
		0, 0, 8, 0, 0,
		0, 0, 0, 0, 0,
		0, 0, 0, 0, 0,
	},
};

static const char * const mt9m114_metering_preset_names[] = {
	"Center-Weighted",
	"Uniform",
	"Backlit Portrait",
	"Spot Center",
	NULL,
};

static const char * const mt9m114_ae_rule_algo_names[] = {
	"Average Brightness",
	"Weighted Average",
	"Adaptive Weighted Highlights",
	"Adaptive Weighted Lowlights",
	NULL,
};

static bool mt9m114_smart_metering = true;
module_param_named(smart_metering, mt9m114_smart_metering, bool, 0644);
MODULE_PARM_DESC(smart_metering,
		 "Enable contrast-aware AE metering switching during streaming");

static bool mt9m114_smart_metering_lock_uniform_strict = true;
module_param_named(smart_metering_lock_uniform_strict,
		   mt9m114_smart_metering_lock_uniform_strict, bool, 0644);
MODULE_PARM_DESC(smart_metering_lock_uniform_strict,
		 "In strict-YUV mode, force smart metering candidate to Uniform for AE stability diagnostics");

static bool mt9m114_smart_metering_blc;
module_param_named(smart_metering_blc, mt9m114_smart_metering_blc, bool, 0644);
MODULE_PARM_DESC(smart_metering_blc,
		 "Enable backlight compensation bit toggling at register 0x3102");

static bool mt9m114_deep_lowlight = true;
module_param_named(deep_lowlight, mt9m114_deep_lowlight, bool, 0644);
MODULE_PARM_DESC(deep_lowlight,
		 "Enable transparent 2x2 summing deep low-light mode for 1280x720/960 and 1296x976 YUV output");

static bool mt9m114_deep_lowlight_runtime = true;
static bool mt9m114_deep_lowlight_test_once;
static bool mt9m114_deep_lowlight_force_ifp_yuv;
static bool mt9m114_deep_lowlight_strict_ifp_yuv = true;
static bool mt9m114_deep_lowlight_strict_runtime = true;
static bool mt9m114_deep_lowlight_strict_allow_regular_enter = true;
static unsigned int mt9m114_deep_lowlight_strict_stale_force_off =
	MT9M114_DEEP_LOWLIGHT_STRICT_STALE_FORCE_OFF_DEFAULT;
static bool mt9m114_deep_lowlight_strict_luma_zero_low_gain_exit = true;
static unsigned int mt9m114_deep_lowlight_strict_luma_zero_low_gain_samples =
	MT9M114_DEEP_LOWLIGHT_STRICT_LUMA_ZERO_LOW_GAIN_SAMPLES_DEFAULT;
static bool mt9m114_deep_lowlight_allow_raw_switch;
static bool mt9m114_deep_lowlight_dump_stats_on_stale;
module_param_named(deep_lowlight_dump_stats_on_stale,
		   mt9m114_deep_lowlight_dump_stats_on_stale, bool, 0644);
MODULE_PARM_DESC(deep_lowlight_dump_stats_on_stale,
		 "DIAGNOSTIC: dump read-only AE/BLC logical stats once per sustained stale/luma-zero episode");

static bool mt9m114_stop_fast_no_state;
module_param_named(stop_fast_no_state, mt9m114_stop_fast_no_state, bool, 0644);
MODULE_PARM_DESC(stop_fast_no_state,
		 "DIAGNOSTIC: skip explicit sensor stop state transition (STANDBY/SUSPEND) to isolate stop-stream timeout source");

struct mt9m114_format_info {
	u32 code;
	u32 output_format;
	u32 flags;
};

struct mt9m114 {
	struct i2c_client *client;
	struct regmap *regmap;

	struct clk *clk;
	struct gpio_desc *reset;
	struct regulator_bulk_data supplies[3];
	struct v4l2_fwnode_endpoint bus_cfg;
	bool bypass_pll;

	struct aptina_pll pll;

	unsigned int pixrate;
	bool streaming;
	bool deep_lowlight_summing;
	u32 pad_slew_rate;

	/* Pixel Array */
	struct {
		struct v4l2_subdev sd;
		struct media_pad pad;

		struct v4l2_ctrl_handler hdl;
		struct v4l2_ctrl *exposure;
		struct v4l2_ctrl *gain;
		struct v4l2_ctrl *hblank;
		struct v4l2_ctrl *vblank;
		struct v4l2_ctrl *ae_metering_preset;
		struct v4l2_ctrl *ae_track_speed;
		struct v4l2_ctrl *ae_rule_algo;
		u32 active_width;
		u32 active_height;
	} pa;

	/* Image Flow Processor */
	struct {
		struct v4l2_subdev sd;
		struct media_pad pads[2];

		struct v4l2_ctrl_handler hdl;
		unsigned int frame_rate;
		bool ae_auto;
		struct delayed_work smart_meter_work;
		struct delayed_work deep_lowlight_work;
		unsigned long deep_lowlight_last_switch;
		unsigned long deep_lowlight_reentry_block_until;
		unsigned long deep_lowlight_invalid_exit_guard_until;
		unsigned int deep_lowlight_gain_max;
		unsigned int deep_lowlight_last_exposure;
		unsigned int deep_lowlight_last_gain;
		unsigned int deep_lowlight_transition_exposure;
		unsigned int deep_lowlight_transition_gain;
		bool deep_lowlight_transition_valid;
		u8 deep_lowlight_last_luma;
		u8 deep_lowlight_stale_count;
		u8 deep_lowlight_stale_recover_count;
		u8 deep_lowlight_luma_zero_low_gain_count;
		u8 deep_lowlight_on_saturation_count;
		u8 deep_lowlight_on_zero_stats_count;
		u8 deep_lowlight_invalid_enter_count;
		u8 deep_lowlight_pending_zero_stats_count;
		bool deep_lowlight_stale_dumped;
		bool deep_lowlight_faulted;
		bool set_fmt_trace_sink_logged;
		bool set_fmt_trace_src_logged;
		unsigned int smart_metering_active_preset;
		unsigned long smart_metering_last_switch;
		u8 smart_last_scene_avg;
		u8 smart_last_center_avg;

		struct v4l2_ctrl *tpg[4];
		struct completion unregistered;
	} ifp;

	const struct mt9m114_model_info *info;
};

/* -----------------------------------------------------------------------------
 * Formats
 */

static const struct mt9m114_format_info mt9m114_format_infos[] = {
	{
		/*
		 * The first two entries are used as defaults, for parallel and
		 * CSI-2 buses respectively. Keep them in that order.
		 */
		.code = MEDIA_BUS_FMT_UYVY8_2X8,
		.flags = MT9M114_FMT_FLAG_PARALLEL,
		.output_format = MT9M114_CAM_OUTPUT_FORMAT_FORMAT_YUV,
	}, {
		.code = MEDIA_BUS_FMT_UYVY8_1X16,
		.flags = MT9M114_FMT_FLAG_CSI2,
		.output_format = MT9M114_CAM_OUTPUT_FORMAT_FORMAT_YUV,
	}, {
		.code = MEDIA_BUS_FMT_YUYV8_2X8,
		.flags = MT9M114_FMT_FLAG_PARALLEL,
		.output_format = MT9M114_CAM_OUTPUT_FORMAT_FORMAT_YUV
			       | MT9M114_CAM_OUTPUT_FORMAT_SWAP_BYTES,
	}, {
		.code = MEDIA_BUS_FMT_YUYV8_1X16,
		.flags = MT9M114_FMT_FLAG_CSI2,
		.output_format = MT9M114_CAM_OUTPUT_FORMAT_FORMAT_YUV
			       | MT9M114_CAM_OUTPUT_FORMAT_SWAP_BYTES,
	}, {
		.code = MEDIA_BUS_FMT_RGB565_2X8_LE,
		.flags = MT9M114_FMT_FLAG_PARALLEL,
		.output_format = MT9M114_CAM_OUTPUT_FORMAT_RGB_FORMAT_565RGB
			       | MT9M114_CAM_OUTPUT_FORMAT_FORMAT_RGB
			       | MT9M114_CAM_OUTPUT_FORMAT_SWAP_BYTES,
	}, {
		.code = MEDIA_BUS_FMT_RGB565_2X8_BE,
		.flags = MT9M114_FMT_FLAG_PARALLEL,
		.output_format = MT9M114_CAM_OUTPUT_FORMAT_RGB_FORMAT_565RGB
			       | MT9M114_CAM_OUTPUT_FORMAT_FORMAT_RGB,
	}, {
		.code = MEDIA_BUS_FMT_RGB565_1X16,
		.flags = MT9M114_FMT_FLAG_CSI2,
		.output_format = MT9M114_CAM_OUTPUT_FORMAT_RGB_FORMAT_565RGB
			       | MT9M114_CAM_OUTPUT_FORMAT_FORMAT_RGB,
	}, {
		.code = MEDIA_BUS_FMT_SGRBG8_1X8,
		.output_format = MT9M114_CAM_OUTPUT_FORMAT_BAYER_FORMAT_PROCESSED8
			       | MT9M114_CAM_OUTPUT_FORMAT_FORMAT_BAYER,
		.flags = MT9M114_FMT_FLAG_PARALLEL | MT9M114_FMT_FLAG_CSI2,
	}, {
		/* Keep the format compatible with the IFP sink pad last. */
		.code = MEDIA_BUS_FMT_SGRBG10_1X10,
		.output_format = MT9M114_CAM_OUTPUT_FORMAT_BAYER_FORMAT_RAWR10
			| MT9M114_CAM_OUTPUT_FORMAT_FORMAT_BAYER,
		.flags = MT9M114_FMT_FLAG_PARALLEL | MT9M114_FMT_FLAG_CSI2,
	}
};

static const struct mt9m114_format_info *
mt9m114_default_format_info(struct mt9m114 *sensor)
{
	if (sensor->bus_cfg.bus_type == V4L2_MBUS_CSI2_DPHY)
		return &mt9m114_format_infos[1];
	else
		return &mt9m114_format_infos[0];
}

static u32 mt9m114_default_ifp_src_code(struct mt9m114 *sensor)
{
	return mt9m114_default_format_info(sensor)->code;
}

static bool mt9m114_ifp_yuv_test_active(struct mt9m114 *sensor)
{
	if (mt9m114_deep_lowlight_strict_ifp_yuv)
		return true;

	if (!mt9m114_deep_lowlight_force_ifp_yuv)
		return false;

	/*
	 * On this AtomISP CSI-2 path, forcing sensor-side YUV output can stall
	 * streaming. Keep the knob for non-CSI2 users, but ignore on CSI-2.
	 */
	if (sensor->bus_cfg.bus_type == V4L2_MBUS_CSI2_DPHY)
		return false;

	return true;
}

static const struct mt9m114_format_info *
mt9m114_format_info(struct mt9m114 *sensor, unsigned int pad, u32 code)
{
	const unsigned int num_formats = ARRAY_SIZE(mt9m114_format_infos);
	unsigned int flag;
	unsigned int i;

	switch (pad) {
	case 0:
		return &mt9m114_format_infos[num_formats - 1];

	case 1:
		if (sensor->bus_cfg.bus_type == V4L2_MBUS_CSI2_DPHY)
			flag = MT9M114_FMT_FLAG_CSI2;
		else
			flag = MT9M114_FMT_FLAG_PARALLEL;

		for (i = 0; i < num_formats; ++i) {
			const struct mt9m114_format_info *info =
				&mt9m114_format_infos[i];

			if (info->code == code && info->flags & flag)
				return info;
		}

		return mt9m114_default_format_info(sensor);

	default:
		return NULL;
	}
}

/* -----------------------------------------------------------------------------
 * Initialization
 */

static const struct cci_reg_sequence mt9m114_init[] = {
	{ MT9M114_RESET_REGISTER, MT9M114_RESET_REGISTER_MASK_BAD |
				  MT9M114_RESET_REGISTER_LOCK_REG |
				  0x0010 },

	/* Sensor optimization */
	{ CCI_REG16(0x316a), 0x8270 },
	{ CCI_REG16(0x316c), 0x8270 },
	{ CCI_REG16(0x3ed0), 0x2305 },
	{ CCI_REG16(0x3ed2), 0x77cf },
	{ CCI_REG16(0x316e), 0x8202 },
	{ CCI_REG16(0x3180), 0x87ff },
	{ CCI_REG16(0x30d4), 0x6080 },
	{ CCI_REG16(0xa802), 0x0008 },

	{ CCI_REG16(0x3e14), 0xff39 },

	/* APGA */
	{ MT9M114_CAM_PGA_PGA_CONTROL,			0x0000 },

	/* Automatic White balance */
	{ MT9M114_CAM_AWB_CCM_L(0),			0x0267 },
	{ MT9M114_CAM_AWB_CCM_L(1),			0xff1a },
	{ MT9M114_CAM_AWB_CCM_L(2),			0xffb3 },
	{ MT9M114_CAM_AWB_CCM_L(3),			0xff80 },
	{ MT9M114_CAM_AWB_CCM_L(4),			0x0166 },
	{ MT9M114_CAM_AWB_CCM_L(5),			0x0003 },
	{ MT9M114_CAM_AWB_CCM_L(6),			0xff9a },
	{ MT9M114_CAM_AWB_CCM_L(7),			0xfeb4 },
	{ MT9M114_CAM_AWB_CCM_L(8),			0x024d },
	{ MT9M114_CAM_AWB_CCM_M(0),			0x01bf },
	{ MT9M114_CAM_AWB_CCM_M(1),			0xff01 },
	{ MT9M114_CAM_AWB_CCM_M(2),			0xfff3 },
	{ MT9M114_CAM_AWB_CCM_M(3),			0xff75 },
	{ MT9M114_CAM_AWB_CCM_M(4),			0x0198 },
	{ MT9M114_CAM_AWB_CCM_M(5),			0xfffd },
	{ MT9M114_CAM_AWB_CCM_M(6),			0xff9a },
	{ MT9M114_CAM_AWB_CCM_M(7),			0xfee7 },
	{ MT9M114_CAM_AWB_CCM_M(8),			0x02a8 },
	{ MT9M114_CAM_AWB_CCM_R(0),			0x01d9 },
	{ MT9M114_CAM_AWB_CCM_R(1),			0xff26 },
	{ MT9M114_CAM_AWB_CCM_R(2),			0xfff3 },
	{ MT9M114_CAM_AWB_CCM_R(3),			0xffb3 },
	{ MT9M114_CAM_AWB_CCM_R(4),			0x0132 },
	{ MT9M114_CAM_AWB_CCM_R(5),			0xffe8 },
	{ MT9M114_CAM_AWB_CCM_R(6),			0xffda },
	{ MT9M114_CAM_AWB_CCM_R(7),			0xfecd },
	{ MT9M114_CAM_AWB_CCM_R(8),			0x02c2 },
	{ MT9M114_CAM_AWB_CCM_L_RG_GAIN,		0x0075 },
	{ MT9M114_CAM_AWB_CCM_L_BG_GAIN,		0x011c },
	{ MT9M114_CAM_AWB_CCM_M_RG_GAIN,		0x009a },
	{ MT9M114_CAM_AWB_CCM_M_BG_GAIN,		0x0105 },
	{ MT9M114_CAM_AWB_CCM_R_RG_GAIN,		0x00a4 },
	{ MT9M114_CAM_AWB_CCM_R_BG_GAIN,		0x00ac },
	{ MT9M114_CAM_AWB_CCM_L_CTEMP,			0x0a8c },
	{ MT9M114_CAM_AWB_CCM_M_CTEMP,			0x0f0a },
	{ MT9M114_CAM_AWB_CCM_R_CTEMP,			0x1964 },
	{ MT9M114_CAM_AWB_AWB_XSHIFT_PRE_ADJ,		51 },
	{ MT9M114_CAM_AWB_AWB_YSHIFT_PRE_ADJ,		60 },
	{ MT9M114_CAM_AWB_AWB_XSCALE,			3 },
	{ MT9M114_CAM_AWB_AWB_YSCALE,			2 },
	{ MT9M114_CAM_AWB_AWB_WEIGHTS(0),		0x0000 },
	{ MT9M114_CAM_AWB_AWB_WEIGHTS(1),		0x0000 },
	{ MT9M114_CAM_AWB_AWB_WEIGHTS(2),		0x0000 },
	{ MT9M114_CAM_AWB_AWB_WEIGHTS(3),		0xe724 },
	{ MT9M114_CAM_AWB_AWB_WEIGHTS(4),		0x1583 },
	{ MT9M114_CAM_AWB_AWB_WEIGHTS(5),		0x2045 },
	{ MT9M114_CAM_AWB_AWB_WEIGHTS(6),		0x03ff },
	{ MT9M114_CAM_AWB_AWB_WEIGHTS(7),		0x007c },
	{ MT9M114_CAM_AWB_K_R_L,			0x80 },
	{ MT9M114_CAM_AWB_K_G_L,			0x80 },
	{ MT9M114_CAM_AWB_K_B_L,			0x80 },
	{ MT9M114_CAM_AWB_K_R_R,			0x88 },
	{ MT9M114_CAM_AWB_K_G_R,			0x80 },
	{ MT9M114_CAM_AWB_K_B_R,			0x80 },

	/* Low-Light Image Enhancements */
	{ MT9M114_CAM_LL_START_BRIGHTNESS,		0x0020 },
	{ MT9M114_CAM_LL_STOP_BRIGHTNESS,		0x009a },
	{ MT9M114_CAM_LL_START_GAIN_METRIC,		0x0070 },
	{ MT9M114_CAM_LL_STOP_GAIN_METRIC,		0x00f3 },
	{ MT9M114_CAM_LL_START_CONTRAST_LUMA_PERCENTAGE, 0x20 },
	{ MT9M114_CAM_LL_STOP_CONTRAST_LUMA_PERCENTAGE,	0x9a },
	{ MT9M114_CAM_LL_START_SATURATION,		0x80 },
	{ MT9M114_CAM_LL_END_SATURATION,		0x4b },
	{ MT9M114_CAM_LL_START_DESATURATION,		0x00 },
	{ MT9M114_CAM_LL_END_DESATURATION,		0xff },
	{ MT9M114_CAM_LL_START_DEMOSAICING,		0x3c },
	{ MT9M114_CAM_LL_START_AP_GAIN,			0x02 },
	{ MT9M114_CAM_LL_START_AP_THRESH,		0x06 },
	{ MT9M114_CAM_LL_STOP_DEMOSAICING,		0x64 },
	{ MT9M114_CAM_LL_STOP_AP_GAIN,			0x01 },
	{ MT9M114_CAM_LL_STOP_AP_THRESH,		0x0c },
	{ MT9M114_CAM_LL_START_NR_RED,			0x3c },
	{ MT9M114_CAM_LL_START_NR_GREEN,		0x3c },
	{ MT9M114_CAM_LL_START_NR_BLUE,			0x3c },
	{ MT9M114_CAM_LL_START_NR_THRESH,		0x0f },
	{ MT9M114_CAM_LL_STOP_NR_RED,			0x64 },
	{ MT9M114_CAM_LL_STOP_NR_GREEN,			0x64 },
	{ MT9M114_CAM_LL_STOP_NR_BLUE,			0x64 },
	{ MT9M114_CAM_LL_STOP_NR_THRESH,		0x32 },
	{ MT9M114_CAM_LL_START_CONTRAST_BM,		0x0020 },
	{ MT9M114_CAM_LL_STOP_CONTRAST_BM,		0x009a },
	{ MT9M114_CAM_LL_GAMMA,				0x00dc },
	{ MT9M114_CAM_LL_START_CONTRAST_GRADIENT,	0x38 },
	{ MT9M114_CAM_LL_STOP_CONTRAST_GRADIENT,	0x30 },
	{ MT9M114_CAM_LL_START_CONTRAST_LUMA_PERCENTAGE, 0x50 },
	{ MT9M114_CAM_LL_STOP_CONTRAST_LUMA_PERCENTAGE,	0x19 },
	{ MT9M114_CAM_LL_START_FADE_TO_BLACK_LUMA,	0x0230 },
	{ MT9M114_CAM_LL_STOP_FADE_TO_BLACK_LUMA,	0x0010 },
	{ MT9M114_CAM_LL_CLUSTER_DC_TH_BM,		0x01cd },
	{ MT9M114_CAM_LL_CLUSTER_DC_GATE_PERCENTAGE,	0x05 },
	{ MT9M114_CAM_LL_SUMMING_SENSITIVITY_FACTOR,	0x40 },

	/* Auto-Exposure */
	{ MT9M114_CAM_AET_TARGET_AVERAGE_LUMA_DARK,	0x1b },
	{ MT9M114_CAM_AET_AEMODE,			0x00 },
	{ MT9M114_CAM_AET_TARGET_GAIN,			0x0080 },
	{ MT9M114_CAM_AET_AE_MAX_VIRT_AGAIN,		0x0100 },
	{ MT9M114_CAM_AET_BLACK_CLIPPING_TARGET,	0x005a },

	{ MT9M114_CCM_DELTA_GAIN,			0x05 },
	{ MT9M114_AE_TRACK_AE_TRACKING_DAMPENING_SPEED,	0x20 },

	/* Pixel array timings and integration time */
	{ MT9M114_CAM_SENSOR_CFG_ROW_SPEED,		1 },
	{ MT9M114_CAM_SENSOR_CFG_FINE_INTEG_TIME_MIN,	219 },
	{ MT9M114_CAM_SENSOR_CFG_FINE_INTEG_TIME_MAX,	1459 },
	{ MT9M114_CAM_SENSOR_CFG_FINE_CORRECTION,	96 },
	{ MT9M114_CAM_SENSOR_CFG_REG_0_DATA,		32 },
};

/* -----------------------------------------------------------------------------
 * Hardware Configuration
 */

/* Wait for a command to complete. */
static int mt9m114_poll_command(struct mt9m114 *sensor, u32 command)
{
	unsigned int i;
	u64 value;
	int ret;

	for (i = 0; i < 100; ++i) {
		ret = cci_read(sensor->regmap, MT9M114_COMMAND_REGISTER, &value,
			       NULL);
		if (ret < 0)
			return ret;

		if (!(value & command))
			break;

		usleep_range(5000, 6000);
	}

	if (value & command) {
		dev_err(&sensor->client->dev, "Command %u completion timeout\n",
			command);
		return -ETIMEDOUT;
	}

	if (!(value & MT9M114_COMMAND_REGISTER_OK)) {
		dev_err(&sensor->client->dev, "Command %u failed\n", command);
		return -EIO;
	}

	return 0;
}

/* Wait for a state to be entered. */
static int mt9m114_poll_state(struct mt9m114 *sensor, u32 state)
{
	unsigned int i;
	u64 value;
	int ret;

	for (i = 0; i < 100; ++i) {
		ret = cci_read(sensor->regmap, MT9M114_SYSMGR_CURRENT_STATE,
			       &value, NULL);
		if (ret < 0)
			return ret;

		if (value == state)
			return 0;

		usleep_range(1000, 1500);
	}

	dev_err(&sensor->client->dev, "Timeout waiting for state 0x%02x\n",
		state);
	return -ETIMEDOUT;
}

static int mt9m114_set_state(struct mt9m114 *sensor, u8 next_state)
{
	int ret = 0;

	/* Set the next desired state and start the state transition. */
	cci_write(sensor->regmap, MT9M114_SYSMGR_NEXT_STATE, next_state, &ret);
	cci_write(sensor->regmap, MT9M114_COMMAND_REGISTER,
		  MT9M114_COMMAND_REGISTER_OK |
		  MT9M114_COMMAND_REGISTER_SET_STATE, &ret);
	if (ret < 0)
		return ret;

	/* Wait for the state transition to complete. */
	ret = mt9m114_poll_command(sensor, MT9M114_COMMAND_REGISTER_SET_STATE);
	if (ret < 0)
		return ret;

	return 0;
}

static int mt9m114_initialize(struct mt9m114 *sensor)
{
	u32 value;
	int ret;

	ret = cci_multi_reg_write(sensor->regmap, mt9m114_init,
				  ARRAY_SIZE(mt9m114_init), NULL);
	if (ret < 0) {
		dev_err(&sensor->client->dev,
			"Failed to initialize the sensor\n");
		return ret;
	}

	/* Configure the PLL. */
	if (sensor->bypass_pll) {
		cci_write(sensor->regmap, MT9M114_CAM_SYSCTL_PLL_ENABLE,
			  MT9M114_CAM_SYSCTL_PLL_DISABLE_VALUE, &ret);
	} else {
		cci_write(sensor->regmap, MT9M114_CAM_SYSCTL_PLL_ENABLE,
			  MT9M114_CAM_SYSCTL_PLL_ENABLE_VALUE, &ret);
		cci_write(sensor->regmap, MT9M114_CAM_SYSCTL_PLL_DIVIDER_M_N,
			  MT9M114_CAM_SYSCTL_PLL_DIVIDER_VALUE(sensor->pll.m,
							       sensor->pll.n),
			  &ret);
		cci_write(sensor->regmap, MT9M114_CAM_SYSCTL_PLL_DIVIDER_P,
			  MT9M114_CAM_SYSCTL_PLL_DIVIDER_P_VALUE(sensor->pll.p1),
			  &ret);
	}

	cci_write(sensor->regmap, MT9M114_CAM_SENSOR_CFG_PIXCLK,
		  sensor->pixrate, &ret);

	/* Configure the output mode. */
	if (sensor->bus_cfg.bus_type == V4L2_MBUS_CSI2_DPHY) {
		value = MT9M114_CAM_PORT_PORT_SELECT_MIPI
		      | MT9M114_CAM_PORT_CHAN_NUM(0)
		      | 0x8000;
		if (!(sensor->bus_cfg.bus.mipi_csi2.flags &
		      V4L2_MBUS_CSI2_NONCONTINUOUS_CLOCK))
			value |= MT9M114_CAM_PORT_CONT_MIPI_CLK;
	} else {
		value = MT9M114_CAM_PORT_PORT_SELECT_PARALLEL
		      | 0x8000;
	}
	cci_write(sensor->regmap, MT9M114_CAM_PORT_OUTPUT_CONTROL, value, &ret);
	if (ret < 0)
		return ret;

	value = sensor->pad_slew_rate
	      | sensor->pad_slew_rate << 4
	      |	sensor->pad_slew_rate << 8;
	cci_write(sensor->regmap, MT9M114_PAD_SLEW, value, &ret);
	if (ret < 0)
		return ret;

	return 0;
}

static int mt9m114_configure_pa(struct mt9m114 *sensor,
				struct v4l2_subdev_state *state)
{
	const struct v4l2_mbus_framefmt *format;
	const struct v4l2_rect *crop;
	unsigned int hratio, vratio;
	u64 read_mode;
	int ret;

	format = v4l2_subdev_state_get_format(state, 0);
	crop = v4l2_subdev_state_get_crop(state, 0);

	ret = cci_read(sensor->regmap, MT9M114_CAM_SENSOR_CONTROL_READ_MODE,
		       &read_mode, NULL);
	if (ret < 0)
		return ret;

	hratio = crop->width / format->width;
	vratio = crop->height / format->height;

	/*
	 * Pixel array crop and binning. The CAM_SENSOR_CFG_CPIPE_LAST_ROW
	 * register isn't clearly documented, but is always set to the number
	 * of active rows minus 4 divided by the vertical binning factor in all
	 * example sensor modes.
	 */
	cci_write(sensor->regmap, MT9M114_CAM_SENSOR_CFG_X_ADDR_START,
		  crop->left, &ret);
	cci_write(sensor->regmap, MT9M114_CAM_SENSOR_CFG_Y_ADDR_START,
		  crop->top, &ret);
	cci_write(sensor->regmap, MT9M114_CAM_SENSOR_CFG_X_ADDR_END,
		  crop->width + crop->left - 1, &ret);
	cci_write(sensor->regmap, MT9M114_CAM_SENSOR_CFG_Y_ADDR_END,
		  crop->height + crop->top - 1, &ret);
	cci_write(sensor->regmap, MT9M114_CAM_SENSOR_CFG_CPIPE_LAST_ROW,
		  (crop->height - 4) / vratio - 1, &ret);

	read_mode &= ~(MT9M114_CAM_SENSOR_CONTROL_X_READ_OUT_MASK |
		       MT9M114_CAM_SENSOR_CONTROL_Y_READ_OUT_MASK);

	if (hratio > 1)
		read_mode |= MT9M114_CAM_SENSOR_CONTROL_X_READ_OUT_SUMMING;
	if (vratio > 1)
		read_mode |= MT9M114_CAM_SENSOR_CONTROL_Y_READ_OUT_SUMMING;

	cci_write(sensor->regmap, MT9M114_CAM_SENSOR_CONTROL_READ_MODE,
		  read_mode, &ret);

	return ret;
}

/*
 * For source pad formats other then RAW10 the IFP removes a 4 pixel border from
 * its sink pad format size for demosaicing.
 */
static int mt9m114_ifp_get_border(struct v4l2_subdev_state *state)
{
	const struct v4l2_mbus_framefmt *format =
		v4l2_subdev_state_get_format(state, 1);

	return format->code == MEDIA_BUS_FMT_SGRBG10_1X10 ? 0 : 4;
}

static int mt9m114_configure_ifp(struct mt9m114 *sensor,
				 struct v4l2_subdev_state *state)
{
	const struct mt9m114_format_info *info;
	const struct v4l2_mbus_framefmt *format;
	const struct v4l2_rect *crop;
	const struct v4l2_rect *compose;
	unsigned int border;
	u64 output_format;
	int ret = 0;

	format = v4l2_subdev_state_get_format(state, 1);
	info = mt9m114_format_info(sensor, 1, format->code);
	crop = v4l2_subdev_state_get_crop(state, 0);
	compose = v4l2_subdev_state_get_compose(state, 0);

	ret = cci_read(sensor->regmap, MT9M114_CAM_OUTPUT_FORMAT,
		       &output_format, NULL);
	if (ret < 0)
		return ret;

	/*
	 * Color pipeline (IFP) cropping and scaling. The crop window registers
	 * apply cropping after demosaicing, which itself consumes 4 pixels on
	 * each side of the image. The crop rectangle exposed to userspace
	 * includes that demosaicing border, subtract it from the left and top
	 * coordinates to configure the crop window.
	 */
	border = mt9m114_ifp_get_border(state);

	cci_write(sensor->regmap, MT9M114_CAM_CROP_WINDOW_XOFFSET,
		  crop->left - border, &ret);
	cci_write(sensor->regmap, MT9M114_CAM_CROP_WINDOW_YOFFSET,
		  crop->top - border, &ret);
	cci_write(sensor->regmap, MT9M114_CAM_CROP_WINDOW_WIDTH,
		  crop->width, &ret);
	cci_write(sensor->regmap, MT9M114_CAM_CROP_WINDOW_HEIGHT,
		  crop->height, &ret);

	cci_write(sensor->regmap, MT9M114_CAM_OUTPUT_WIDTH,
		  compose->width, &ret);
	cci_write(sensor->regmap, MT9M114_CAM_OUTPUT_HEIGHT,
		  compose->height, &ret);

	/* AWB and AE windows, use the full frame. */
	cci_write(sensor->regmap, MT9M114_CAM_STAT_AWB_CLIP_WINDOW_XSTART,
		  0, &ret);
	cci_write(sensor->regmap, MT9M114_CAM_STAT_AWB_CLIP_WINDOW_YSTART,
		  0, &ret);
	cci_write(sensor->regmap, MT9M114_CAM_STAT_AWB_CLIP_WINDOW_XEND,
		  compose->width - 1, &ret);
	cci_write(sensor->regmap, MT9M114_CAM_STAT_AWB_CLIP_WINDOW_YEND,
		  compose->height - 1, &ret);

	cci_write(sensor->regmap, MT9M114_CAM_STAT_AE_INITIAL_WINDOW_XSTART,
		  0, &ret);
	cci_write(sensor->regmap, MT9M114_CAM_STAT_AE_INITIAL_WINDOW_YSTART,
		  0, &ret);
	cci_write(sensor->regmap, MT9M114_CAM_STAT_AE_INITIAL_WINDOW_XEND,
		  compose->width / 5 - 1, &ret);
	cci_write(sensor->regmap, MT9M114_CAM_STAT_AE_INITIAL_WINDOW_YEND,
		  compose->height / 5 - 1, &ret);

	cci_write(sensor->regmap, MT9M114_CAM_CROP_CROPMODE,
		  MT9M114_CAM_CROP_MODE_AWB_AUTO_CROP_EN |
		  MT9M114_CAM_CROP_MODE_AE_AUTO_CROP_EN, &ret);

	/* Set the media bus code. */
	output_format &= ~(MT9M114_CAM_OUTPUT_FORMAT_RGB_FORMAT_MASK |
			   MT9M114_CAM_OUTPUT_FORMAT_BAYER_FORMAT_MASK |
			   MT9M114_CAM_OUTPUT_FORMAT_FORMAT_MASK |
			   MT9M114_CAM_OUTPUT_FORMAT_SWAP_BYTES |
			   MT9M114_CAM_OUTPUT_FORMAT_SWAP_RED_BLUE);
	output_format |= info->output_format;

	cci_write(sensor->regmap, MT9M114_CAM_OUTPUT_FORMAT,
		  output_format, &ret);

	return ret;
}

static unsigned int mt9m114_get_min_fps(struct mt9m114 *sensor);
static void mt9m114_update_vblank_range_for_min_fps(struct mt9m114 *sensor,
						   struct v4l2_subdev_state *state,
						   unsigned int min_fps);

static int mt9m114_set_frame_rate_with_state(struct mt9m114 *sensor,
					     struct v4l2_subdev_state *pa_state)
{
	unsigned int max_fps = sensor->ifp.frame_rate;
	unsigned int min_fps = sensor->ifp.ae_auto ? MT9M114_MIN_FRAME_RATE_FLOOR
						 : max_fps;
	u16 min_rate;
	u16 max_rate;
	int ret = 0;

	if (min_fps > max_fps)
		min_fps = max_fps;

	min_rate = min_fps << 8;
	max_rate = max_fps << 8;

	cci_write(sensor->regmap, MT9M114_CAM_AET_MIN_FRAME_RATE,
		  min_rate, &ret);
	cci_write(sensor->regmap, MT9M114_CAM_AET_MAX_FRAME_RATE,
		  max_rate, &ret);

	mt9m114_update_vblank_range_for_min_fps(sensor, pa_state, min_fps);

	return ret;
}

static int mt9m114_set_frame_rate(struct mt9m114 *sensor)
{
	return mt9m114_set_frame_rate_with_state(sensor, NULL);
}

static unsigned int mt9m114_get_min_fps(struct mt9m114 *sensor)
{
	if (!sensor->ifp.ae_auto)
		return sensor->ifp.frame_rate;

	return min_t(unsigned int, MT9M114_MIN_FRAME_RATE_FLOOR,
		     sensor->ifp.frame_rate);
}

static void mt9m114_update_vblank_range_for_min_fps(struct mt9m114 *sensor,
						   struct v4l2_subdev_state *state,
						   unsigned int min_fps)
{
	struct v4l2_subdev_state *active_state = state;
	const struct v4l2_mbus_framefmt *format;
	unsigned int line_length;
	u32 max_vblank;
	u64 frame_length;
	u32 default_vblank;
	bool locked = false;

	if (!sensor->pa.vblank || !sensor->pa.hblank)
		return;

	if (!min_fps)
		return;

	if (!active_state)
		active_state = v4l2_subdev_get_locked_active_state(&sensor->pa.sd);

	if (!active_state) {
		active_state = v4l2_subdev_lock_and_get_active_state(&sensor->pa.sd);
		locked = true;
	}

	format = v4l2_subdev_state_get_format(active_state, 0);

	line_length = format->width + sensor->pa.hblank->val;
	if (!line_length) {
		if (locked)
			v4l2_subdev_unlock_state(active_state);
		return;
	}

	frame_length = div_u64((u64)sensor->pixrate,
			       (u64)line_length * min_fps);
	if (frame_length < format->height + MT9M114_MIN_VBLANK)
		frame_length = format->height + MT9M114_MIN_VBLANK;
	if (frame_length > MT9M114_CAM_SENSOR_CFG_FRAME_LENGTH_LINES_MAX)
		frame_length = MT9M114_CAM_SENSOR_CFG_FRAME_LENGTH_LINES_MAX;

	max_vblank = frame_length - format->height;
	default_vblank = clamp_t(u32, sensor->pa.vblank->val,
				 MT9M114_MIN_VBLANK, max_vblank);

	dev_dbg(&sensor->client->dev,
		"vblank range update: min_fps=%u line_len=%u frame_len=%llu max_vblank=%u default_vblank=%u\n",
		min_fps, line_length, frame_length, max_vblank, default_vblank);
	__v4l2_ctrl_modify_range(sensor->pa.vblank, MT9M114_MIN_VBLANK,
				 max_vblank, 1, default_vblank);

	if (locked)
		v4l2_subdev_unlock_state(active_state);
}

static int mt9m114_start_streaming(struct mt9m114 *sensor,
				   struct v4l2_subdev_state *pa_state,
				   struct v4l2_subdev_state *ifp_state)
{
	int ret;

	ret = pm_runtime_resume_and_get(&sensor->client->dev);
	if (ret) {
		dev_err(&sensor->client->dev,
			"start_stream: pm_runtime_resume_and_get failed: %d\n",
			ret);
		return ret;
	}

	ret = mt9m114_initialize(sensor);
	if (ret) {
		dev_err(&sensor->client->dev,
			"start_stream: initialize failed: %d\n", ret);
		goto error;
	}

	if (mt9m114_ifp_yuv_test_active(sensor))
		dev_info_once(&sensor->client->dev,
			      mt9m114_deep_lowlight_strict_ifp_yuv ?
			      "deep-lowlight: STRICT YUV bring-up mode active\n" :
			      "deep-lowlight: forcing sensor IFP UYVY output for test mode\n");
	else if (mt9m114_deep_lowlight_force_ifp_yuv &&
		 sensor->bus_cfg.bus_type == V4L2_MBUS_CSI2_DPHY)
		dev_warn_once(&sensor->client->dev,
		      "deep-lowlight: force_ifp_yuv ignored on CSI-2 (known stream timeout path); using negotiated format\n");

	ret = mt9m114_configure_ifp(sensor, ifp_state);
	if (ret) {
		dev_err(&sensor->client->dev,
			"start_stream: configure_ifp failed: %d\n", ret);
		goto error;
	}

	ret = mt9m114_configure_pa(sensor, pa_state);
	if (ret) {
		dev_err(&sensor->client->dev,
			"start_stream: configure_pa failed: %d\n", ret);
		goto error;
	}

	ret = mt9m114_set_frame_rate_with_state(sensor, pa_state);
	if (ret) {
		dev_err(&sensor->client->dev,
			"start_stream: set_frame_rate failed: %d\n", ret);
		goto error;
	}

	ret = __v4l2_ctrl_handler_setup(&sensor->pa.hdl);
	if (ret) {
		dev_err(&sensor->client->dev,
			"start_stream: pa ctrl setup failed: %d\n", ret);
		goto error;
	}

	ret = __v4l2_ctrl_handler_setup(&sensor->ifp.hdl);
	if (ret) {
		dev_err(&sensor->client->dev,
			"start_stream: ifp ctrl setup failed: %d\n", ret);
		goto error;
	}

	/*
	 * The Change-Config state is transient and moves to the streaming
	 * state automatically.
	 */
	ret = mt9m114_set_state(sensor, MT9M114_SYS_STATE_ENTER_CONFIG_CHANGE);
	if (ret) {
		dev_err(&sensor->client->dev,
			"start_stream: set_state ENTER_CONFIG_CHANGE failed: %d\n",
			ret);
		goto error;
	}
	/* Wait for the state transition to complete. */
	ret = mt9m114_poll_state(sensor, MT9M114_SYS_STATE_STREAMING);
	if (ret) {
		dev_err(&sensor->client->dev,
			"start_stream: wait for STREAMING state failed: %d\n", ret);
		goto error;
	}
	{
			const struct v4l2_mbus_framefmt *ifp_src_fmt;
			u64 out_fmt = 0, out_w = 0, out_h = 0, read_mode = 0;
			u64 cpipe_last = 0, line_len = 0, frame_len = 0;

			ifp_src_fmt = v4l2_subdev_state_get_format(ifp_state, 1);
			dev_info(&sensor->client->dev,
				 "mt9m114 FORMAT: ifp_src_code=0x%04x %ux%u force_ifp_yuv=%u\n",
				 ifp_src_fmt->code, ifp_src_fmt->width,
				 ifp_src_fmt->height,
				 mt9m114_ifp_yuv_test_active(sensor) ? 1 : 0);

			cci_read(sensor->regmap, MT9M114_CAM_OUTPUT_FORMAT, &out_fmt, NULL);
			cci_read(sensor->regmap, MT9M114_CAM_OUTPUT_WIDTH, &out_w, NULL);
			cci_read(sensor->regmap, MT9M114_CAM_OUTPUT_HEIGHT, &out_h, NULL);
			cci_read(sensor->regmap, MT9M114_CAM_SENSOR_CONTROL_READ_MODE,
							&read_mode, NULL);
			cci_read(sensor->regmap, MT9M114_CAM_SENSOR_CFG_CPIPE_LAST_ROW,
							&cpipe_last, NULL);
			cci_read(sensor->regmap, MT9M114_CAM_SENSOR_CFG_LINE_LENGTH_PCK,
							&line_len, NULL);
			cci_read(sensor->regmap, MT9M114_CAM_SENSOR_CFG_FRAME_LENGTH_LINES,
							&frame_len, NULL);

			dev_info(&sensor->client->dev,
					"mt9m114 POST-CONFIG: out_fmt=0x%04llx out=%llu x %llu "
					"read_mode=0x%04llx cpipe_last=%llu line_len=%llu frame_len=%llu\n",
					out_fmt, out_w, out_h, read_mode, cpipe_last, line_len,
					frame_len);

			if (mt9m114_deep_lowlight_strict_ifp_yuv &&
			    (out_fmt & MT9M114_CAM_OUTPUT_FORMAT_FORMAT_MASK) !=
			    MT9M114_CAM_OUTPUT_FORMAT_FORMAT_YUV) {
				dev_err(&sensor->client->dev,
					"strict-yuv: sensor output format mismatch (out_fmt=0x%04llx is not YUV)\n",
					out_fmt);
				ret = -EIO;
				goto error;
			}
	}

	sensor->streaming = true;
	sensor->deep_lowlight_summing = false;
	sensor->ifp.deep_lowlight_last_switch = 0;
	sensor->ifp.deep_lowlight_reentry_block_until = 0;
	sensor->ifp.deep_lowlight_invalid_exit_guard_until = 0;
	sensor->ifp.deep_lowlight_gain_max = 0;
	sensor->ifp.deep_lowlight_last_exposure = 0;
	sensor->ifp.deep_lowlight_last_gain = 0;
	sensor->ifp.deep_lowlight_transition_exposure = 0;
	sensor->ifp.deep_lowlight_transition_gain = 0;
	sensor->ifp.deep_lowlight_transition_valid = false;
	sensor->ifp.deep_lowlight_last_luma = 0;
	sensor->ifp.deep_lowlight_stale_count = 0;
	sensor->ifp.deep_lowlight_stale_recover_count = 0;
	sensor->ifp.deep_lowlight_luma_zero_low_gain_count = 0;
	sensor->ifp.deep_lowlight_on_saturation_count = 0;
	sensor->ifp.deep_lowlight_on_zero_stats_count = 0;
	sensor->ifp.deep_lowlight_invalid_enter_count = 0;
	sensor->ifp.deep_lowlight_pending_zero_stats_count = 0;
	sensor->ifp.deep_lowlight_stale_dumped = false;
	sensor->ifp.deep_lowlight_faulted = false;
	schedule_delayed_work(&sensor->ifp.deep_lowlight_work,
			      msecs_to_jiffies(MT9M114_DEEP_LOWLIGHT_INTERVAL_MS));

	
	if (mt9m114_smart_metering && sensor->ifp.ae_auto) {
		sensor->ifp.smart_metering_last_switch = 0;
		sensor->ifp.smart_metering_active_preset =
			sensor->pa.ae_metering_preset ?
			sensor->pa.ae_metering_preset->val :
			MT9M114_METERING_PRESET_CENTER;
		schedule_delayed_work(&sensor->ifp.smart_meter_work,
				      msecs_to_jiffies(MT9M114_SMART_METER_INTERVAL_MS));
	}

	return 0;

error:
	pm_runtime_put_autosuspend(&sensor->client->dev);

	return ret;
}

static int mt9m114_stop_streaming(struct mt9m114 *sensor)
{
	u64 read_mode_u64;
	u16 read_mode;
	int ret = 0;
	int lowlight_ret = 0;
	int suspend_ret;

	cancel_delayed_work_sync(&sensor->ifp.smart_meter_work);
	cancel_delayed_work_sync(&sensor->ifp.deep_lowlight_work);

	if (sensor->deep_lowlight_summing) {
		lowlight_ret = cci_read(sensor->regmap,
				       MT9M114_CAM_SENSOR_CONTROL_READ_MODE,
				       &read_mode_u64, NULL);
		if (!lowlight_ret) {
			read_mode = read_mode_u64;
			read_mode &= ~(MT9M114_CAM_SENSOR_CONTROL_X_READ_OUT_MASK |
				       MT9M114_CAM_SENSOR_CONTROL_Y_READ_OUT_MASK);

			lowlight_ret = 0;
			cci_write(sensor->regmap, MT9M114_CAM_SENSOR_CONTROL_READ_MODE,
				  read_mode, &lowlight_ret);
		}

		if (!lowlight_ret) {
			lowlight_ret = mt9m114_set_state(sensor,
						 MT9M114_SYS_STATE_ENTER_CONFIG_CHANGE);
			if (!lowlight_ret)
				lowlight_ret = mt9m114_poll_state(sensor,
							 MT9M114_SYS_STATE_STREAMING);
		}

		if (lowlight_ret)
			dev_warn(&sensor->client->dev,
				 "stop_stream: failed to restore non-summing mode before suspend: %d\n",
				 lowlight_ret);
	}

	sensor->streaming = false;
	sensor->deep_lowlight_summing = false;
	sensor->ifp.deep_lowlight_last_switch = 0;
	sensor->ifp.deep_lowlight_reentry_block_until = 0;
	sensor->ifp.deep_lowlight_invalid_exit_guard_until = 0;
	sensor->ifp.deep_lowlight_gain_max = 0;
	sensor->ifp.deep_lowlight_last_exposure = 0;
	sensor->ifp.deep_lowlight_last_gain = 0;
	sensor->ifp.deep_lowlight_transition_exposure = 0;
	sensor->ifp.deep_lowlight_transition_gain = 0;
	sensor->ifp.deep_lowlight_transition_valid = false;
	sensor->ifp.deep_lowlight_last_luma = 0;
	sensor->ifp.deep_lowlight_stale_count = 0;
	sensor->ifp.deep_lowlight_stale_recover_count = 0;
	sensor->ifp.deep_lowlight_luma_zero_low_gain_count = 0;
	sensor->ifp.deep_lowlight_on_saturation_count = 0;
	sensor->ifp.deep_lowlight_on_zero_stats_count = 0;
	sensor->ifp.deep_lowlight_invalid_enter_count = 0;
	sensor->ifp.deep_lowlight_pending_zero_stats_count = 0;
	sensor->ifp.deep_lowlight_stale_dumped = false;
	sensor->ifp.deep_lowlight_faulted = false;

	if (mt9m114_stop_fast_no_state) {
		dev_info_once(&sensor->client->dev,
			      "stop-fast-no-state: skipping explicit sensor stop state transition\n");
		pm_runtime_put_autosuspend(&sensor->client->dev);
		return 0;
	}

	suspend_ret = mt9m114_set_state(sensor, MT9M114_SYS_STATE_ENTER_STANDBY);
	if (suspend_ret) {
		dev_warn(&sensor->client->dev,
			 "stop_stream: ENTER_STANDBY failed (%d), trying ENTER_SUSPEND\n",
			 suspend_ret);
		suspend_ret = mt9m114_set_state(sensor,
					       MT9M114_SYS_STATE_ENTER_SUSPEND);
	}
	if (suspend_ret)
		ret = suspend_ret;

	pm_runtime_put_autosuspend(&sensor->client->dev);

	return ret;
}

/* -----------------------------------------------------------------------------
 * Common Subdev Operations
 */

static const struct media_entity_operations mt9m114_entity_ops = {
	.link_validate = v4l2_subdev_link_validate,
};

/* -----------------------------------------------------------------------------
 * Pixel Array Control Operations
 */

static inline struct mt9m114 *pa_ctrl_to_mt9m114(struct v4l2_ctrl *ctrl)
{
	return container_of(ctrl->handler, struct mt9m114, pa.hdl);
}

/**
 * mt9m114_group_hold - Enable/disable grouped parameter hold
 * @sensor: The MT9M114 sensor
 * @enable: true to enable GROUP_HOLD (buffer writes), false to apply atomically
 *
 * When enabled, all register writes are buffered in shadow registers.
 * When disabled, buffered writes are applied atomically at the next SOF.
 *
 * Critical for synchronizing VTS (Frame_length_lines) and Exposure
 * (Coarse_integration_time) to prevent rolling shutter artifacts.
 *
 * Register: 0x8404 (GROUPED_PARAMETER_HOLD)
 *   0x0100 = Enable (buffer writes)
 *   0x0000 = Disable (apply at SOF)
 *
 * Returns: 0 on success, negative errno on failure
 */
static int mt9m114_group_hold(struct mt9m114 *sensor, bool enable)
{
	u16 value = enable ? MT9M114_GROUPED_PARAMETER_HOLD_ENABLE
			   : MT9M114_GROUPED_PARAMETER_HOLD_DISABLE;
	int ret;

	ret = cci_write(sensor->regmap, MT9M114_GROUPED_PARAMETER_HOLD, value, NULL);
	if (ret) {
		dev_err(&sensor->client->dev, "Failed to %s group hold: %d\n",
			enable ? "enable" : "disable", ret);
		return ret;
	}

	dev_dbg(&sensor->client->dev, "GROUP_HOLD %s\n", enable ? "ENABLED" : "DISABLED");
	return 0;
}

static int mt9m114_ensure_manual_ae(struct mt9m114 *sensor)
{
	u64 ae_track_mode;
	int ret;

	ret = cci_read(sensor->regmap, MT9M114_AE_TRACK_MODE, &ae_track_mode, NULL);
	if (ret) {
		dev_err(&sensor->client->dev, "Failed to read AE_TRACK_MODE: %d\n", ret);
		return ret;
	}

	if (ae_track_mode & MT9M114_AE_TRACK_MODE_AUTO_ENABLE) {
		ret = cci_write(sensor->regmap, MT9M114_AE_TRACK_MODE, 0x00, NULL);
		if (ret) {
			dev_err(&sensor->client->dev,
				"Failed to disable internal AE: %d\n", ret);
			return ret;
		}
		dev_info(&sensor->client->dev,
			 "Disabled sensor internal auto-exposure for manual control\n");
	}

	return 0;
}

/**
 * mt9m114_update_vts_for_exposure - Adjust VTS (frame length) for long exposure
 * @sensor: The MT9M114 sensor
 * @exposure: Desired exposure time in lines
 *
 * When exposure time exceeds the standard frame length, we need to extend
 * the VTS (Vertical Total Size) to accommodate it. This drops the frame rate
 * to gain more light collection time.
 *
 * Example: 30 FPS = 997 lines (976 active + 21 blanking)
 *          200ms exposure = ~6000 lines → VTS must be 6002, FPS drops to ~5
 *
 * CRITICAL: Hardware requires Integration_time < Frame_length - 2
 *           If violated, sensor's timing generator freezes!
 *
 * Uses GROUP_HOLD (0x8404) to synchronize VTS and Exposure atomically.
 *
 * Returns: 0 on success, negative errno on failure
 */
static int mt9m114_update_vts_for_exposure(struct mt9m114 *sensor, u32 exposure)
{
	u32 min_frame_length = exposure + 2;  /* CRITICAL: +2 margin required */
	u32 old_vblank = sensor->pa.vblank->val;
	u32 new_vblank;

	/* If current VTS is sufficient, no adjustment needed */
	if (min_frame_length <= MT9M114_PIXEL_ARRAY_HEIGHT + old_vblank)
		return 0;

	/* Calculate new VBLANK to accommodate exposure + 2-line margin */
	new_vblank = min_frame_length - MT9M114_PIXEL_ARRAY_HEIGHT;

	/* Clamp to maximum allowed VBLANK */
	if (new_vblank > MT9M114_MAX_VBLANK_LOWLIGHT)
		new_vblank = MT9M114_MAX_VBLANK_LOWLIGHT;

	/* Warn if large VTS change during streaming (AtomISP CSS timeout risk) */
	if (sensor->streaming && new_vblank > old_vblank * 2) {
		dev_warn_once(&sensor->client->dev,
			      "Large VTS adjustment (%u -> %u) during streaming. "
			      "AtomISP CSS firmware may timeout. "
			      "Recommend stop/reconfigure/restart stream.\n",
			      old_vblank, new_vblank);
	}

	/* Update VBLANK control value (will be written by s_ctrl) */
	return __v4l2_ctrl_modify_range(sensor->pa.vblank, MT9M114_MIN_VBLANK,
					MT9M114_MAX_VBLANK_LOWLIGHT, 1, new_vblank);
}

static int mt9m114_apply_metering_preset(struct mt9m114 *sensor, unsigned int preset)
{
	const u8 *pattern;
	unsigned int i;
	int ret;

	if (preset >= ARRAY_SIZE(mt9m114_metering_patterns)) {
		dev_err(&sensor->client->dev, "Invalid metering preset %u\n", preset);
		return -EINVAL;
	}

	pattern = mt9m114_metering_patterns[preset];

	for (i = 0; i < 25; i++) {
		ret = cci_write(sensor->regmap,
				MT9M114_AE_WEIGHT_TABLE_BASE + i,
				pattern[i], NULL);
		if (ret) {
			dev_err(&sensor->client->dev,
				"Failed to write metering weight[%u]: %d\n", i, ret);
			return ret;
		}
	}

	dev_dbg(&sensor->client->dev, "Applied metering preset: %s\n",
		mt9m114_metering_preset_names[preset]);

	return 0;
}

static unsigned int mt9m114_smart_metering_candidate(u8 scene_avg, u8 center_avg)
{
	unsigned int delta = abs((int)scene_avg - (int)center_avg);

	if (scene_avg >= MT9M114_SMART_METER_BACKLIT_SCENE_MIN &&
	    center_avg + MT9M114_SMART_METER_BACKLIT_DELTA < scene_avg)
		return MT9M114_METERING_PRESET_BACKLIT;

	if (delta <= MT9M114_SMART_METER_UNIFORM_DELTA)
		return MT9M114_METERING_PRESET_UNIFORM;

	return MT9M114_METERING_PRESET_CENTER;
}

static void mt9m114_smart_metering_set_blc(struct mt9m114 *sensor, bool enable)
{
	int ret;

	if (!mt9m114_smart_metering_blc)
		return;

	ret = cci_update_bits(sensor->regmap, MT9M114_SMART_BLC_CTRL,
			      MT9M114_SMART_BLC_ENABLE,
			      enable ? MT9M114_SMART_BLC_ENABLE : 0, NULL);
	if (ret)
		dev_dbg(&sensor->client->dev,
			"smart-meter: BLC update failed (%d)\n", ret);
}

static void mt9m114_smart_metering_work(struct work_struct *work)
{
	struct mt9m114 *sensor = container_of(to_delayed_work(work),
					      struct mt9m114,
					      ifp.smart_meter_work);
	u64 scene_u64;
	u64 center_u64;
	unsigned int candidate;
	u8 scene_avg;
	u8 center_avg;
	int ret;

	if (!sensor->streaming || !sensor->ifp.ae_auto || !mt9m114_smart_metering)
		return;

	if (!pm_runtime_get_if_in_use(&sensor->client->dev))
		goto reschedule;

	ret = cci_read(sensor->regmap, MT9M114_SMART_AE_STATS_AVG_LUMA,
		       &scene_u64, NULL);
	if (ret)
		goto out_pm;

	ret = cci_read(sensor->regmap, MT9M114_SMART_AE_STATS_CENTER_LUMA,
		       &center_u64, NULL);
	if (ret)
		goto out_pm;

	scene_avg = scene_u64;
	center_avg = center_u64;
	sensor->ifp.smart_last_scene_avg = scene_avg;
	sensor->ifp.smart_last_center_avg = center_avg;

	candidate = mt9m114_smart_metering_candidate(scene_avg, center_avg);
	if (mt9m114_deep_lowlight_strict_ifp_yuv &&
	    mt9m114_smart_metering_lock_uniform_strict)
		candidate = MT9M114_METERING_PRESET_UNIFORM;
	if (candidate != sensor->ifp.smart_metering_active_preset &&
	    (sensor->ifp.smart_metering_last_switch == 0 ||
	     time_after_eq(jiffies,
			   sensor->ifp.smart_metering_last_switch +
			   MT9M114_SMART_METER_HYSTERESIS))) {
		ret = mt9m114_apply_metering_preset(sensor, candidate);
		if (!ret) {
			sensor->ifp.smart_metering_active_preset = candidate;
			sensor->ifp.smart_metering_last_switch = jiffies;

			if (sensor->pa.ae_metering_preset) {
				sensor->pa.ae_metering_preset->val = candidate;
				sensor->pa.ae_metering_preset->cur.val = candidate;
			}

			dev_dbg(&sensor->client->dev,
				"smart-meter: preset=%u scene=%u center=%u\n",
				candidate, scene_avg, center_avg);
		}
	}

	mt9m114_smart_metering_set_blc(sensor,
				       candidate == MT9M114_METERING_PRESET_BACKLIT);

out_pm:
	pm_runtime_put_autosuspend(&sensor->client->dev);

reschedule:
	if (sensor->streaming && sensor->ifp.ae_auto && mt9m114_smart_metering)
		schedule_delayed_work(&sensor->ifp.smart_meter_work,
				      msecs_to_jiffies(MT9M114_SMART_METER_INTERVAL_MS));
}

static unsigned int mt9m114_gain_threshold(unsigned int min,
					  unsigned int max,
					  unsigned int percent);
static int mt9m114_set_deep_lowlight_mode(struct mt9m114 *sensor, bool enable);
static int mt9m114_rekick_auto_exposure(struct mt9m114 *sensor);
static int mt9m114_calc_timing_for_fps(struct mt9m114 *sensor,
				      unsigned int fps,
				      u32 *frame_length,
				      u32 *exposure,
				      u32 *frame_height);
static int mt9m114_seed_exposure_for_fps(struct mt9m114 *sensor,
					 unsigned int fps);
static int mt9m114_seed_transition_controls(struct mt9m114 *sensor,
					 bool enable);
static int mt9m114_set_ae_fps_window(struct mt9m114 *sensor,
				    unsigned int min_fps,
				    unsigned int max_fps);
static int mt9m114_dump_stats(struct mt9m114 *sensor);

static void mt9m114_maybe_switch_deep_lowlight_values(struct mt9m114 *sensor,
					      unsigned int exposure,
					      unsigned int gain,
					      bool have_luma,
					      unsigned int luma,
					      bool have_center,
					      unsigned int center_luma)
{
	unsigned int enter_gain;
	unsigned int exit_gain;
	unsigned int gain_min;
	unsigned int gain_max;
	unsigned int exposure_max;
	unsigned int exposure_enter;
	unsigned int exposure_exit;
	unsigned int exit_gain_reserve;
	unsigned int on_saturation_gain;
	unsigned int exposure_exit_reserve = 0;
	u64 gain_max_hw;
	u64 frame_length_u64;
	bool use_reserve_exit;
	bool reserve_exit_valid;
	bool can_switch;
	bool want_enter;
	bool want_exit;
	bool want_exit_exposure_gain;
	bool want_exit_luma;
	bool strict_luma_zero_low_gain_exit;
	bool on_saturation_exit;
	bool on_zero_stats_timeout_exit;
	bool strict_low_luma_enter;
	bool reentry_blocked;
	bool exposure_invalid;
	bool strict_invalid_enter;
	bool strict_pending_zero_stats_enter;
	bool strict_regular_enter_suppressed;
	const char *pending_reason;
	const char *on_reason;
	int ret;

	if (!mt9m114_deep_lowlight || !sensor->streaming || !sensor->pa.gain ||
	    !sensor->pa.exposure)
		return;

	if (!mt9m114_deep_lowlight_runtime && !mt9m114_deep_lowlight_test_once)
		return;

	if (mt9m114_deep_lowlight_strict_ifp_yuv &&
	    !mt9m114_deep_lowlight_strict_runtime) {
		dev_info_once(&sensor->client->dev,
			      "deep-lowlight strict-yuv: runtime switching disabled (set deep_lowlight_strict_runtime=1 to enable)\n");
		return;
	}

	if (sensor->ifp.deep_lowlight_faulted)
		return;

	if (sensor->deep_lowlight_summing && mt9m114_deep_lowlight_test_once)
		return;

	exposure_max = sensor->pa.exposure->maximum;
	ret = cci_read(sensor->regmap, MT9M114_CAM_SENSOR_CFG_FRAME_LENGTH_LINES,
		       &frame_length_u64, NULL);
	if (!ret && frame_length_u64 > 2) {
		u64 hw_exposure_max = min_t(u64, frame_length_u64 - 2, UINT_MAX);

		exposure_max = max_t(unsigned int, exposure_max,
				     (unsigned int)hw_exposure_max);
	}
	gain_min = sensor->pa.gain->minimum;
	gain_max = sensor->ifp.deep_lowlight_gain_max ?
		sensor->ifp.deep_lowlight_gain_max : sensor->pa.gain->maximum;

	if (!sensor->ifp.deep_lowlight_gain_max) {
		ret = cci_read(sensor->regmap, MT9M114_CAM_AET_AE_MAX_VIRT_AGAIN,
			       &gain_max_hw, NULL);
		if (!ret && gain_max_hw > 0 && gain_max_hw < gain_max) {
			gain_max = (unsigned int)gain_max_hw;
			sensor->ifp.deep_lowlight_gain_max = gain_max;
			dev_info_once(&sensor->client->dev,
				      "deep-lowlight: using AE gain max %u (ctrl max %u)\n",
				      gain_max, (unsigned int)sensor->pa.gain->maximum);
		}
	}

	if (mt9m114_deep_lowlight_strict_ifp_yuv &&
	    gain_max > MT9M114_DEEP_LOWLIGHT_STRICT_GAIN_MAX_FALLBACK) {
		gain_max = MT9M114_DEEP_LOWLIGHT_STRICT_GAIN_MAX_FALLBACK;
		dev_info_once(&sensor->client->dev,
			      "deep-lowlight strict-yuv: capping gain max for thresholds to %u\n",
			      gain_max);
	}

	enter_gain = mt9m114_gain_threshold(gain_min, gain_max,
				    MT9M114_DEEP_LOWLIGHT_GAIN_ENTER_PCT);
	exit_gain = mt9m114_gain_threshold(gain_min, gain_max,
				   MT9M114_DEEP_LOWLIGHT_GAIN_EXIT_PCT);
	exit_gain_reserve = mt9m114_gain_threshold(gain_min, gain_max,
					   MT9M114_DEEP_LOWLIGHT_GAIN_EXIT_RESERVE_PCT);
	on_saturation_gain = mt9m114_gain_threshold(gain_min, gain_max,
					   MT9M114_DEEP_LOWLIGHT_GAIN_ON_SATURATION_PCT);
	exposure_exit = div_u64((u64)exposure_max *
			      MT9M114_DEEP_LOWLIGHT_EXPOSURE_EXIT_PCT, 100);
	exposure_enter = div_u64((u64)exposure_max *
			       MT9M114_DEEP_LOWLIGHT_EXPOSURE_ENTER_PCT, 100);
	use_reserve_exit = sensor->deep_lowlight_summing &&
		mt9m114_ifp_yuv_test_active(sensor);
	reserve_exit_valid = false;
	if (use_reserve_exit &&
	    !mt9m114_calc_timing_for_fps(sensor,
					 MT9M114_DEEP_LOWLIGHT_FPS_EXIT_RESERVE,
					 NULL,
					 &exposure_exit_reserve,
					 NULL)) {
		exposure_exit = exposure_exit_reserve;
		reserve_exit_valid = true;
	}

	if (exposure > exposure_max)
		exposure = exposure_max;
	if (gain > gain_max)
		gain = gain_max;

	exposure_invalid = exposure <= 2;

	can_switch = !sensor->ifp.deep_lowlight_last_switch ||
		time_after_eq(jiffies,
			      sensor->ifp.deep_lowlight_last_switch +
			      MT9M114_DEEP_LOWLIGHT_SWITCH_HYSTERESIS);

	strict_low_luma_enter = !sensor->deep_lowlight_summing &&
		mt9m114_deep_lowlight_strict_ifp_yuv &&
		have_luma && luma > 0 &&
		luma <= MT9M114_DEEP_LOWLIGHT_STRICT_LUMA_ENTER &&
		gain >= exit_gain_reserve &&
		exposure >= exposure_enter;

	want_enter = (gain >= enter_gain &&
		      (exposure >= exposure_enter ||
		       (mt9m114_deep_lowlight_strict_ifp_yuv && exposure_invalid))) ||
		strict_low_luma_enter;

	if (!sensor->deep_lowlight_summing &&
	    mt9m114_deep_lowlight_strict_ifp_yuv) {
		if (exposure_invalid && gain >= enter_gain) {
			if (sensor->ifp.deep_lowlight_invalid_enter_count < 255)
				sensor->ifp.deep_lowlight_invalid_enter_count++;
		} else {
			sensor->ifp.deep_lowlight_invalid_enter_count = 0;
		}
	}

	strict_invalid_enter = mt9m114_deep_lowlight_strict_ifp_yuv &&
		exposure_invalid && gain >= enter_gain && exposure < exposure_max &&
		sensor->ifp.deep_lowlight_invalid_enter_count >=
		MT9M114_DEEP_LOWLIGHT_INVALID_ENTER_SAMPLES;

	if (!sensor->deep_lowlight_summing &&
	    mt9m114_deep_lowlight_strict_ifp_yuv &&
	    have_luma && have_center &&
	    luma == 0 && center_luma == 0 &&
	    exposure >= exposure_enter) {
		if (sensor->ifp.deep_lowlight_pending_zero_stats_count < 255)
			sensor->ifp.deep_lowlight_pending_zero_stats_count++;
	} else {
		sensor->ifp.deep_lowlight_pending_zero_stats_count = 0;
		sensor->ifp.deep_lowlight_stale_dumped = false;
	}

	if (!sensor->deep_lowlight_summing &&
	    mt9m114_deep_lowlight_dump_stats_on_stale &&
	    !sensor->ifp.deep_lowlight_stale_dumped &&
	    have_luma && have_center && luma == 0 && center_luma == 0 &&
	    exposure >= exposure_enter &&
	    gain <= mt9m114_gain_threshold(gain_min, gain_max,
				   MT9M114_DEEP_LOWLIGHT_PENDING_ZERO_STATS_LOW_GAIN_PCT) &&
	    sensor->ifp.deep_lowlight_pending_zero_stats_count >=
	    MT9M114_DEEP_LOWLIGHT_PENDING_ZERO_STATS_DUMP_TRIGGER) {
		ret = mt9m114_dump_stats(sensor);
		if (ret)
			dev_warn_ratelimited(&sensor->client->dev,
				"deep-lowlight: pending zero-stats diagnostic dump failed: %d\n",
				ret);
		else
			dev_warn_ratelimited(&sensor->client->dev,
				"deep-lowlight: pending zero-stats with low gain, dumped AE histogram for diagnosis (reason=pending_zero_stats_low_gain)\n");
		sensor->ifp.deep_lowlight_stale_dumped = true;
	}

	strict_pending_zero_stats_enter =
		!sensor->deep_lowlight_summing &&
		mt9m114_deep_lowlight_strict_ifp_yuv &&
		sensor->ifp.deep_lowlight_pending_zero_stats_count >=
		MT9M114_DEEP_LOWLIGHT_PENDING_ZERO_STATS_ENTER_SAMPLES;

	if (strict_pending_zero_stats_enter)
		want_enter = true;

	strict_regular_enter_suppressed =
		mt9m114_deep_lowlight_strict_ifp_yuv &&
		!mt9m114_deep_lowlight_strict_allow_regular_enter &&
		want_enter && !strict_invalid_enter && !strict_pending_zero_stats_enter;
	if (strict_regular_enter_suppressed)
		want_enter = false;
	want_exit_exposure_gain = exposure <= exposure_exit && gain <= exit_gain;
	if (reserve_exit_valid)
		want_exit_exposure_gain = exposure <= exposure_exit_reserve &&
			gain <= exit_gain_reserve;
	want_exit_luma = have_luma && luma >= MT9M114_DEEP_LOWLIGHT_LUMA_EXIT;
	want_exit = want_exit_exposure_gain || want_exit_luma;
	strict_luma_zero_low_gain_exit = false;
	on_saturation_exit = false;
	on_zero_stats_timeout_exit = false;
	reentry_blocked = sensor->ifp.deep_lowlight_reentry_block_until &&
		!time_after_eq(jiffies, sensor->ifp.deep_lowlight_reentry_block_until);

	if (!sensor->deep_lowlight_summing && strict_invalid_enter)
		dev_info_ratelimited(&sensor->client->dev,
			"deep-lowlight strict-yuv: invalid exposure fallback active (exp=%u/%u gain=%u/%u enter=%u samples=%u)\n",
			exposure, exposure_max, gain, gain_max, enter_gain,
			sensor->ifp.deep_lowlight_invalid_enter_count);

	if (strict_pending_zero_stats_enter)
		dev_warn_ratelimited(&sensor->client->dev,
			"deep-lowlight strict-yuv: pending timeout with invalid luma/center stats (exp=%u/%u gain=%u/%u zeros=%u/%u), forcing ON entry\n",
			exposure, exposure_max, gain, gain_max,
			sensor->ifp.deep_lowlight_pending_zero_stats_count,
			MT9M114_DEEP_LOWLIGHT_PENDING_ZERO_STATS_ENTER_SAMPLES);

	if (!sensor->deep_lowlight_summing && strict_regular_enter_suppressed)
		dev_info_ratelimited(&sensor->client->dev,
			"deep-lowlight strict-yuv: suppressing regular auto-enter (exp=%u/%u gain=%u/%u), waiting for invalid-exposure fallback\n",
			exposure, exposure_max, gain, gain_max);

	if (strict_low_luma_enter)
		dev_info_ratelimited(&sensor->client->dev,
			"deep-lowlight strict-yuv: low-luma enter fallback active (luma=%u exp=%u/%u gain=%u/%u)\n",
			luma, exposure, exposure_max, gain, gain_max);

	if (sensor->deep_lowlight_summing &&
	    mt9m114_deep_lowlight_strict_ifp_yuv &&
	    exposure_invalid &&
	    want_exit_exposure_gain && !want_exit_luma)
		want_exit = false;

	if (sensor->deep_lowlight_summing &&
	    mt9m114_deep_lowlight_strict_ifp_yuv &&
	    sensor->ifp.deep_lowlight_invalid_exit_guard_until &&
	    !time_after_eq(jiffies,
			sensor->ifp.deep_lowlight_invalid_exit_guard_until) &&
	    want_exit_exposure_gain && !want_exit_luma)
		want_exit = false;

	if (sensor->deep_lowlight_summing &&
	    mt9m114_deep_lowlight_strict_ifp_yuv &&
	    mt9m114_deep_lowlight_strict_luma_zero_low_gain_exit &&
	    have_luma && luma == 0 &&
	    exposure <= exposure_exit &&
	    gain <= exit_gain_reserve) {
		if (sensor->ifp.deep_lowlight_luma_zero_low_gain_count < 255)
			sensor->ifp.deep_lowlight_luma_zero_low_gain_count++;
	} else {
		sensor->ifp.deep_lowlight_luma_zero_low_gain_count = 0;
	}

	if (sensor->ifp.deep_lowlight_luma_zero_low_gain_count >=
	    max_t(unsigned int, 1,
		  mt9m114_deep_lowlight_strict_luma_zero_low_gain_samples))
		strict_luma_zero_low_gain_exit = true;

	if (strict_luma_zero_low_gain_exit) {
		want_exit_luma = true;
		want_exit = true;
		if (mt9m114_deep_lowlight_dump_stats_on_stale &&
		    !sensor->ifp.deep_lowlight_stale_dumped &&
		    sensor->ifp.deep_lowlight_luma_zero_low_gain_count >=
		    MT9M114_DEEP_LOWLIGHT_STALE_DUMP_TRIGGER) {
			ret = mt9m114_dump_stats(sensor);
			if (ret)
				dev_warn_ratelimited(&sensor->client->dev,
					"deep-lowlight: stale/luma-zero diagnostic dump failed: %d\n",
					ret);
			sensor->ifp.deep_lowlight_stale_dumped = true;
		}
	}

	if (sensor->deep_lowlight_summing && have_luma && luma == 0 &&
	    gain >= on_saturation_gain) {
		if (sensor->ifp.deep_lowlight_on_saturation_count < 255)
			sensor->ifp.deep_lowlight_on_saturation_count++;
	} else {
		sensor->ifp.deep_lowlight_on_saturation_count = 0;
	}

	if (sensor->ifp.deep_lowlight_on_saturation_count >=
	    MT9M114_DEEP_LOWLIGHT_ON_SATURATION_SAMPLES) {
		on_saturation_exit = true;
		want_exit = true;
		want_exit_luma = true;
	}

	if (sensor->deep_lowlight_summing &&
	    mt9m114_deep_lowlight_strict_ifp_yuv &&
	    have_luma && have_center &&
	    luma == 0 && center_luma == 0 &&
	    gain <= on_saturation_gain) {
		if (sensor->ifp.deep_lowlight_on_zero_stats_count < 255)
			sensor->ifp.deep_lowlight_on_zero_stats_count++;
	} else {
		sensor->ifp.deep_lowlight_on_zero_stats_count = 0;
	}

	if (sensor->ifp.deep_lowlight_on_zero_stats_count >=
	    MT9M114_DEEP_LOWLIGHT_ON_ZERO_STATS_TIMEOUT_SAMPLES) {
		on_zero_stats_timeout_exit = true;
		want_exit = true;
		want_exit_luma = true;
	}

	pending_reason = strict_pending_zero_stats_enter ? "pending_zero_stats_timeout" :
		(strict_low_luma_enter ? "low_luma_enter" :
		 (strict_invalid_enter ? "invalid_enter" : "regular_enter_wait"));
	on_reason = on_saturation_exit ? "on_saturation" :
		(on_zero_stats_timeout_exit ? "on_zero_stats_timeout" :
		(strict_luma_zero_low_gain_exit ? "luma_zero" :
		 (want_exit ? "regular_exit" : "none")));

	if (!sensor->deep_lowlight_summing &&
	    (exposure >= (exposure_max * 9) / 10 || gain >= enter_gain))
		dev_info_ratelimited(&sensor->client->dev,
			"deep-lowlight pending: exp=%u/%u gain=%u/%u luma=%u center=%u enter=%u exit=(exp<=%u gain<=%u or luma>=%u)%s reason=%s\n",
			exposure, exposure_max, gain, gain_max,
			have_luma ? luma : 0,
			have_center ? center_luma : 0,
			enter_gain,
			exposure_exit,
			reserve_exit_valid ? exit_gain_reserve : exit_gain,
			MT9M114_DEEP_LOWLIGHT_LUMA_EXIT,
			reserve_exit_valid ? " [reserve:10fps+gain<50%]" : "",
			pending_reason);

	if (sensor->deep_lowlight_summing)
		dev_info_ratelimited(&sensor->client->dev,
			"deep-lowlight ON: exp=%u/%u gain=%u/%u luma=%u center=%u exit=(exp<=%u gain<=%u or luma>=%u)%s reason=%s\n",
			exposure, exposure_max, gain, gain_max,
			have_luma ? luma : 0,
			have_center ? center_luma : 0,
			exposure_exit,
			reserve_exit_valid ? exit_gain_reserve : exit_gain,
			MT9M114_DEEP_LOWLIGHT_LUMA_EXIT,
			reserve_exit_valid ? " [reserve:10fps+gain<50%]" : "",
			on_reason);

	if (strict_luma_zero_low_gain_exit)
		dev_warn_ratelimited(&sensor->client->dev,
			"deep-lowlight: strict-YUV luma stuck at 0 with low gain (%u/%u), forcing OFF path (reason=luma_zero)\n",
			sensor->ifp.deep_lowlight_luma_zero_low_gain_count,
			max_t(unsigned int, 1,
			      mt9m114_deep_lowlight_strict_luma_zero_low_gain_samples));

	if (on_saturation_exit)
		dev_warn_ratelimited(&sensor->client->dev,
			"deep-lowlight: ON saturation guard triggered (luma=0 gain=%u/%u count=%u/%u), forcing OFF path (reason=on_saturation)\n",
			gain, gain_max,
			sensor->ifp.deep_lowlight_on_saturation_count,
			MT9M114_DEEP_LOWLIGHT_ON_SATURATION_SAMPLES);

	if (on_zero_stats_timeout_exit)
		dev_warn_ratelimited(&sensor->client->dev,
			"deep-lowlight: ON zero-stats timeout (luma=0 center=0 gain=%u/%u count=%u/%u), forcing OFF path (reason=on_zero_stats_timeout)\n",
			gain, gain_max,
			sensor->ifp.deep_lowlight_on_zero_stats_count,
			MT9M114_DEEP_LOWLIGHT_ON_ZERO_STATS_TIMEOUT_SAMPLES);

	if (sensor->deep_lowlight_summing) {
		if (exposure == sensor->ifp.deep_lowlight_last_exposure &&
		    gain == sensor->ifp.deep_lowlight_last_gain &&
		    (!have_luma || luma == sensor->ifp.deep_lowlight_last_luma))
			sensor->ifp.deep_lowlight_stale_count++;
		else {
			sensor->ifp.deep_lowlight_stale_count = 0;
			sensor->ifp.deep_lowlight_stale_recover_count = 0;
			sensor->ifp.deep_lowlight_luma_zero_low_gain_count = 0;
			sensor->ifp.deep_lowlight_on_saturation_count = 0;
			sensor->ifp.deep_lowlight_stale_dumped = false;
		}

		sensor->ifp.deep_lowlight_last_exposure = exposure;
		sensor->ifp.deep_lowlight_last_gain = gain;
		sensor->ifp.deep_lowlight_last_luma = have_luma ? luma : 0;

		if (!want_exit &&
		    sensor->ifp.deep_lowlight_stale_count >=
		    MT9M114_DEEP_LOWLIGHT_STALE_LIMIT) {
			if (mt9m114_deep_lowlight_strict_ifp_yuv) {
				unsigned int stale_force_off_limit =
					max_t(unsigned int, 1,
					      mt9m114_deep_lowlight_strict_stale_force_off);
				bool strict_stale_force_off_allowed;

				if (sensor->ifp.deep_lowlight_stale_recover_count < 255)
					sensor->ifp.deep_lowlight_stale_recover_count++;

				strict_stale_force_off_allowed =
					gain >= on_saturation_gain ||
					sensor->ifp.deep_lowlight_stale_recover_count >=
					min_t(unsigned int, 255,
					      stale_force_off_limit *
					      MT9M114_DEEP_LOWLIGHT_STALE_HARD_FORCE_OFF_MULTIPLIER);

				if (mt9m114_deep_lowlight_dump_stats_on_stale &&
				    !sensor->ifp.deep_lowlight_stale_dumped &&
				    sensor->ifp.deep_lowlight_stale_recover_count >=
				    MT9M114_DEEP_LOWLIGHT_STALE_DUMP_TRIGGER) {
					ret = mt9m114_dump_stats(sensor);
					if (ret)
						dev_warn_ratelimited(&sensor->client->dev,
							"deep-lowlight: stale telemetry diagnostic dump failed: %d\n",
							ret);
					sensor->ifp.deep_lowlight_stale_dumped = true;
				}

				if (strict_stale_force_off_allowed &&
				    sensor->ifp.deep_lowlight_stale_recover_count >=
				    stale_force_off_limit) {
					ret = mt9m114_set_deep_lowlight_mode(sensor, false);
					if (!ret) {
						sensor->ifp.deep_lowlight_reentry_block_until =
							jiffies + MT9M114_DEEP_LOWLIGHT_FORCE_OFF_REENTRY_COOLDOWN;
						sensor->ifp.deep_lowlight_stale_count = 0;
						sensor->ifp.deep_lowlight_stale_recover_count = 0;
						sensor->ifp.deep_lowlight_invalid_enter_count = 0;
						sensor->ifp.deep_lowlight_stale_dumped = false;
						dev_warn_ratelimited(&sensor->client->dev,
							"deep-lowlight: strict-YUV stale telemetry persisted, forcing OFF for recovery (reason=stale)\n");
					} else {
						dev_warn_ratelimited(&sensor->client->dev,
							"deep-lowlight: strict-YUV stale force-OFF failed: %d\n",
							ret);
					}
					return;
				}

				if (!strict_stale_force_off_allowed &&
				    sensor->ifp.deep_lowlight_stale_recover_count >=
				    stale_force_off_limit)
					dev_warn_ratelimited(&sensor->client->dev,
						"deep-lowlight: stale telemetry in strict-YUV mode at low gain (%u/%u), skipping force-OFF to avoid flicker (reason=stale_low_gain_hold)\n",
						gain, gain_max);

				ret = mt9m114_rekick_auto_exposure(sensor);
				if (ret)
					dev_warn_ratelimited(&sensor->client->dev,
						"deep-lowlight: stale telemetry in strict-YUV mode, AE re-kick failed: %d (reason=stale)\n",
						ret);
				else
					dev_warn_ratelimited(&sensor->client->dev,
						"deep-lowlight: stale telemetry in strict-YUV mode, keeping ON and re-kicking AE (%u/%u) (reason=stale)\n",
						sensor->ifp.deep_lowlight_stale_recover_count,
						max_t(unsigned int, 1,
						      mt9m114_deep_lowlight_strict_stale_force_off));
				sensor->ifp.deep_lowlight_stale_count =
					MT9M114_DEEP_LOWLIGHT_STALE_LIMIT;
				return;
			}

			ret = mt9m114_set_deep_lowlight_mode(sensor, false);
			if (!ret) {
				sensor->ifp.deep_lowlight_reentry_block_until =
					jiffies + MT9M114_DEEP_LOWLIGHT_REENTRY_COOLDOWN;
				sensor->ifp.deep_lowlight_stale_count = 0;
				sensor->ifp.deep_lowlight_faulted =
					!mt9m114_deep_lowlight_strict_ifp_yuv;
				if (mt9m114_deep_lowlight_strict_ifp_yuv)
					dev_warn_ratelimited(&sensor->client->dev,
						"deep-lowlight: forced OFF due to stale telemetry, keeping runtime switching enabled in strict-YUV mode\n");
				else
					dev_warn_ratelimited(&sensor->client->dev,
						"deep-lowlight: forced OFF due to stale telemetry, disabling runtime switching until stream restart\n");
			} else {
				dev_dbg(&sensor->client->dev,
					"deep-lowlight forced exit failed: %d\n", ret);
			}
			return;
		}
	}

	if (!can_switch)
		return;

	if (!sensor->deep_lowlight_summing && want_enter && !reentry_blocked) {
		sensor->ifp.deep_lowlight_transition_exposure = exposure;
		sensor->ifp.deep_lowlight_transition_gain = gain;
		sensor->ifp.deep_lowlight_transition_valid = true;
		ret = mt9m114_set_deep_lowlight_mode(sensor, true);
		if (ret)
			dev_dbg(&sensor->client->dev,
				"deep-lowlight enter failed: %d\n", ret);
		else if (strict_invalid_enter)
			sensor->ifp.deep_lowlight_invalid_exit_guard_until =
				jiffies + MT9M114_DEEP_LOWLIGHT_INVALID_EXIT_GUARD;
		else
			sensor->ifp.deep_lowlight_invalid_exit_guard_until = 0;
		if (!ret)
			sensor->ifp.deep_lowlight_invalid_enter_count = 0;
		if (!ret)
			sensor->ifp.deep_lowlight_pending_zero_stats_count = 0;
		if (!ret)
			sensor->ifp.deep_lowlight_stale_recover_count = 0;
		if (!ret)
			sensor->ifp.deep_lowlight_luma_zero_low_gain_count = 0;
		if (!ret)
			sensor->ifp.deep_lowlight_on_saturation_count = 0;
		if (!ret)
			sensor->ifp.deep_lowlight_on_zero_stats_count = 0;
	} else if (sensor->deep_lowlight_summing && want_exit) {
		sensor->ifp.deep_lowlight_transition_exposure = exposure;
		sensor->ifp.deep_lowlight_transition_gain = gain;
		sensor->ifp.deep_lowlight_transition_valid = true;
		ret = mt9m114_set_deep_lowlight_mode(sensor, false);
		if (ret)
			dev_dbg(&sensor->client->dev,
				"deep-lowlight exit failed: %d\n", ret);
		if (!ret && strict_luma_zero_low_gain_exit)
			sensor->ifp.deep_lowlight_reentry_block_until =
				jiffies + MT9M114_DEEP_LOWLIGHT_FORCE_OFF_REENTRY_COOLDOWN;
		if (!ret)
			sensor->ifp.deep_lowlight_invalid_enter_count = 0;
		if (!ret)
			sensor->ifp.deep_lowlight_pending_zero_stats_count = 0;
		if (!ret)
			sensor->ifp.deep_lowlight_stale_recover_count = 0;
		if (!ret)
			sensor->ifp.deep_lowlight_luma_zero_low_gain_count = 0;
		if (!ret)
			sensor->ifp.deep_lowlight_on_saturation_count = 0;
		if (!ret)
			sensor->ifp.deep_lowlight_on_zero_stats_count = 0;
	}
}

static void mt9m114_deep_lowlight_work(struct work_struct *work)
{
	struct mt9m114 *sensor = container_of(to_delayed_work(work),
					      struct mt9m114,
					      ifp.deep_lowlight_work);
	u64 exposure_u64;
	u64 gain_u64;
	u64 luma_u64 = 0;
	u64 center_luma_u64 = 0;
	bool have_luma = false;
	bool have_center = false;
	int ret;

	if (!sensor->streaming)
		return;

	if (!pm_runtime_get_if_in_use(&sensor->client->dev))
		goto reschedule;

	ret = cci_read(sensor->regmap,
		       MT9M114_CAM_SENSOR_CONTROL_COARSE_INTEGRATION_TIME,
		       &exposure_u64, NULL);
	if (ret)
		goto out_pm;

	ret = cci_read(sensor->regmap, MT9M114_CAM_SENSOR_CONTROL_ANALOG_GAIN,
		       &gain_u64, NULL);
	if (ret)
		goto out_pm;

	ret = cci_read(sensor->regmap, MT9M114_SMART_AE_STATS_AVG_LUMA,
		       &luma_u64, NULL);
	if (!ret)
		have_luma = true;

	ret = cci_read(sensor->regmap, MT9M114_SMART_AE_STATS_CENTER_LUMA,
		       &center_luma_u64, NULL);
	if (!ret)
		have_center = true;

	mt9m114_maybe_switch_deep_lowlight_values(sensor, exposure_u64, gain_u64,
					    have_luma, luma_u64,
					    have_center, center_luma_u64);

out_pm:
	pm_runtime_put_autosuspend(&sensor->client->dev);

reschedule:
	if (sensor->streaming)
		schedule_delayed_work(&sensor->ifp.deep_lowlight_work,
				      msecs_to_jiffies(MT9M114_DEEP_LOWLIGHT_INTERVAL_MS));
}

static unsigned int mt9m114_gain_threshold(unsigned int min,
					  unsigned int max,
					  unsigned int percent)
{
	u32 range;

	if (max <= min)
		return 0;

	range = max - min;
	return min + div_u64((u64)range * percent, 100);
}

static int mt9m114_calc_timing_for_fps(struct mt9m114 *sensor,
				      unsigned int fps,
				      u32 *frame_length,
				      u32 *exposure,
				      u32 *frame_height)
{
	struct v4l2_subdev_state *pa_state;
	const struct v4l2_mbus_framefmt *format;
	u64 frame_length_u64;
	u32 height;
	u32 line_length;
	bool locked = false;

	if (!fps || !sensor->pa.hblank)
		return -EINVAL;

	pa_state = v4l2_subdev_get_locked_active_state(&sensor->pa.sd);
	if (!pa_state) {
		pa_state = v4l2_subdev_lock_and_get_active_state(&sensor->pa.sd);
		locked = true;
	}

	format = v4l2_subdev_state_get_format(pa_state, 0);
	if (!format) {
		if (locked)
			v4l2_subdev_unlock_state(pa_state);
		return -EINVAL;
	}

	height = max_t(u32, format->height, MT9M114_PIXEL_ARRAY_HEIGHT);
	line_length = format->width + sensor->pa.hblank->val;
	if (!line_length) {
		if (locked)
			v4l2_subdev_unlock_state(pa_state);
		return -EINVAL;
	}

	frame_length_u64 = div_u64((u64)sensor->pixrate,
				   (u64)line_length * fps);
	if (frame_length_u64 < (u64)height + MT9M114_MIN_VBLANK)
		frame_length_u64 = (u64)height + MT9M114_MIN_VBLANK;
	if (frame_length_u64 > MT9M114_CAM_SENSOR_CFG_FRAME_LENGTH_LINES_MAX)
		frame_length_u64 = MT9M114_CAM_SENSOR_CFG_FRAME_LENGTH_LINES_MAX;

	if (frame_length)
		*frame_length = (u32)frame_length_u64;
	if (exposure)
		*exposure = (u32)frame_length_u64 - 2;
	if (frame_height)
		*frame_height = height;

	if (locked)
		v4l2_subdev_unlock_state(pa_state);

	return 0;
}

static int mt9m114_set_ae_fps_window(struct mt9m114 *sensor,
				    unsigned int min_fps,
				    unsigned int max_fps)
{
	u16 min_rate;
	u16 max_rate;
	int ret = 0;

	if (!min_fps || !max_fps)
		return -EINVAL;

	if (min_fps > max_fps)
		swap(min_fps, max_fps);

	min_rate = min_fps << 8;
	max_rate = max_fps << 8;

	cci_write(sensor->regmap, MT9M114_CAM_AET_MIN_FRAME_RATE, min_rate, &ret);
	cci_write(sensor->regmap, MT9M114_CAM_AET_MAX_FRAME_RATE, max_rate, &ret);

	return ret;
}

static int mt9m114_seed_exposure_for_fps(struct mt9m114 *sensor,
					 unsigned int fps)
{
	u32 frame_length;
	u32 exposure;
	u32 frame_height;
	u32 vblank;
	int ret;

	ret = mt9m114_calc_timing_for_fps(sensor, fps, &frame_length,
					  &exposure, &frame_height);
	if (ret)
		return ret;

	ret = mt9m114_group_hold(sensor, true);
	if (ret)
		return ret;

	cci_write(sensor->regmap, MT9M114_FRAME_LENGTH_LINES, frame_length, &ret);
	cci_write(sensor->regmap, MT9M114_CAM_SENSOR_CFG_FRAME_LENGTH_LINES,
		  frame_length, &ret);
	cci_write(sensor->regmap, MT9M114_COARSE_INTEGRATION_TIME, exposure, &ret);
	cci_write(sensor->regmap, MT9M114_CAM_SENSOR_CONTROL_COARSE_INTEGRATION_TIME,
		  exposure, &ret);

	if (ret)
		mt9m114_group_hold(sensor, false);
	else
		ret = mt9m114_group_hold(sensor, false);

	if (ret)
		return ret;

	vblank = frame_length - frame_height;
	if (sensor->pa.vblank) {
		sensor->pa.vblank->val = vblank;
		sensor->pa.vblank->cur.val = vblank;
	}
	if (sensor->pa.exposure) {
		sensor->pa.exposure->val = exposure;
		sensor->pa.exposure->cur.val = exposure;
	}

	return 0;
}

static int mt9m114_seed_transition_controls(struct mt9m114 *sensor,
					 bool enable)
{
	unsigned int target_fps = enable ? MT9M114_DEEP_LOWLIGHT_FPS_ENTER_SEED
					 : MT9M114_DEEP_LOWLIGHT_FPS_EXIT_SEED;
	u32 frame_length;
	u32 exposure_limit;
	u32 frame_height;
	u32 vblank;
	u32 exposure;
	u32 gain;
	u32 gain_min;
	u32 gain_max;
	u32 off_gain_cap;
	u32 off_dark_gain;
	u32 src_exposure;
	u32 src_gain;
	u64 desired_ev;
	int ret;

	if (!sensor->pa.gain)
		return -EINVAL;

	gain_min = sensor->pa.gain->minimum;
	gain_max = sensor->pa.gain->maximum;
	off_gain_cap = mt9m114_gain_threshold(gain_min, gain_max,
					 MT9M114_DEEP_LOWLIGHT_GAIN_OFF_SEED_CAP_PCT);
	off_dark_gain = mt9m114_gain_threshold(gain_min, gain_max,
				      MT9M114_DEEP_LOWLIGHT_OFF_DARK_GAIN_PCT);

	if (sensor->ifp.deep_lowlight_transition_valid) {
		src_exposure = sensor->ifp.deep_lowlight_transition_exposure;
		src_gain = sensor->ifp.deep_lowlight_transition_gain;
	} else {
		src_exposure = sensor->pa.exposure ? sensor->pa.exposure->val : 0;
		src_gain = sensor->pa.gain->val;
	}

	if (!enable && mt9m114_deep_lowlight_strict_ifp_yuv &&
	    src_gain <= off_dark_gain)
		target_fps = MT9M114_DEEP_LOWLIGHT_FPS_EXIT_SEED_DARK;

	ret = mt9m114_calc_timing_for_fps(sensor, target_fps, &frame_length,
					  &exposure_limit, &frame_height);
	if (ret)
		return ret;

	if (!src_exposure)
		src_exposure = exposure_limit;
	if (src_gain < gain_min)
		src_gain = gain_min;

	desired_ev = (u64)src_exposure * src_gain;

	if (enable) {
		desired_ev = div_u64(desired_ev,
				    MT9M114_DEEP_LOWLIGHT_TRANSITION_SCALE);
		exposure = min_t(u32, src_exposure, exposure_limit);
		if (!exposure)
			exposure = 1;
		gain = clamp_t(u32, div_u64(desired_ev, exposure),
			       gain_min, gain_max);
		if (gain == gain_min) {
			u32 exp_from_ev = div_u64(desired_ev, gain_min);

			exposure = clamp_t(u32, exp_from_ev, 1, exposure_limit);
		}
	} else {
		desired_ev *= MT9M114_DEEP_LOWLIGHT_TRANSITION_SCALE_OFF;
		exposure = min_t(u32,
				 (u32)((u64)src_exposure *
				 MT9M114_DEEP_LOWLIGHT_TRANSITION_SCALE_OFF),
				 exposure_limit);
		if (!exposure)
			exposure = 1;
		gain = clamp_t(u32, div_u64(desired_ev, exposure),
			       gain_min, gain_max);
		if (gain == gain_max) {
			u32 exp_from_ev = div_u64(desired_ev, gain_max);

			exposure = clamp_t(u32, exp_from_ev, 1, exposure_limit);
		}
		if (gain > off_gain_cap)
			gain = off_gain_cap;
	}

	gain = clamp_t(u32, gain, gain_min, gain_max);

	ret = mt9m114_group_hold(sensor, true);
	if (ret)
		return ret;

	cci_write(sensor->regmap, MT9M114_FRAME_LENGTH_LINES, frame_length, &ret);
	cci_write(sensor->regmap, MT9M114_CAM_SENSOR_CFG_FRAME_LENGTH_LINES,
		  frame_length, &ret);
	cci_write(sensor->regmap, MT9M114_COARSE_INTEGRATION_TIME, exposure, &ret);
	cci_write(sensor->regmap, MT9M114_CAM_SENSOR_CONTROL_COARSE_INTEGRATION_TIME,
		  exposure, &ret);
	cci_write(sensor->regmap, MT9M114_CAM_SENSOR_CONTROL_ANALOG_GAIN,
		  gain, &ret);
	cci_write(sensor->regmap, MT9M114_GLOBAL_GAIN, gain, &ret);

	if (ret)
		mt9m114_group_hold(sensor, false);
	else
		ret = mt9m114_group_hold(sensor, false);

	if (ret)
		return ret;

	vblank = frame_length - frame_height;
	if (sensor->pa.vblank) {
		sensor->pa.vblank->val = vblank;
		sensor->pa.vblank->cur.val = vblank;
	}
	if (sensor->pa.exposure) {
		sensor->pa.exposure->val = exposure;
		sensor->pa.exposure->cur.val = exposure;
	}
	if (sensor->pa.gain) {
		sensor->pa.gain->val = gain;
		sensor->pa.gain->cur.val = gain;
	}

	sensor->ifp.deep_lowlight_transition_valid = false;

	dev_dbg(&sensor->client->dev,
		"deep-lowlight: transition seed %s fps=%u exp=%u gain=%u (src exp=%u gain=%u)\n",
		enable ? "ON" : "OFF", target_fps, exposure, gain,
		src_exposure, src_gain);

	return 0;
}

static int mt9m114_get_deep_lowlight_geometry(struct mt9m114 *sensor,
					     u16 *binned_height,
					     u16 *output_height)
{
	struct v4l2_subdev_state *ifp_state;
	const struct v4l2_mbus_framefmt *ifp_fmt;
	bool ifp_locked = false;
	u64 out_w;
	u64 out_h;
	u64 out_fmt;
	u64 out_mode;
	int ret;

	ret = cci_read(sensor->regmap, MT9M114_CAM_OUTPUT_WIDTH, &out_w, NULL);
	if (ret)
		return ret;

	ret = cci_read(sensor->regmap, MT9M114_CAM_OUTPUT_HEIGHT, &out_h, NULL);
	if (ret)
		return ret;

	if (!out_w || !out_h) {
		ifp_state = v4l2_subdev_get_locked_active_state(&sensor->ifp.sd);
		if (!ifp_state) {
			ifp_state = v4l2_subdev_lock_and_get_active_state(&sensor->ifp.sd);
			ifp_locked = true;
		}

		ifp_fmt = v4l2_subdev_state_get_format(ifp_state, 1);
		if (ifp_fmt) {
			if (!out_w && ifp_fmt->width)
				out_w = ifp_fmt->width;
			if (!out_h && ifp_fmt->height)
				out_h = ifp_fmt->height;
		}

		if (ifp_locked)
			v4l2_subdev_unlock_state(ifp_state);

		dev_dbg_ratelimited(&sensor->client->dev,
			"deep-lowlight: recovered transient output geometry via active fmt: %llux%llu\n",
			out_w, out_h);
	}

	ret = cci_read(sensor->regmap, MT9M114_CAM_OUTPUT_FORMAT, &out_fmt, NULL);
	if (ret)
		return ret;

	out_mode = out_fmt & MT9M114_CAM_OUTPUT_FORMAT_FORMAT_MASK;
	if (out_mode == MT9M114_CAM_OUTPUT_FORMAT_FORMAT_BAYER &&
	    !mt9m114_deep_lowlight_allow_raw_switch) {
		dev_warn_ratelimited(&sensor->client->dev,
			"deep-lowlight: RAW/BAYER runtime switch disabled for stability (set deep_lowlight_allow_raw_switch=1 to force test)\n");
		return -EINVAL;
	}

	if (out_mode != MT9M114_CAM_OUTPUT_FORMAT_FORMAT_YUV &&
	    out_mode != MT9M114_CAM_OUTPUT_FORMAT_FORMAT_BAYER) {
		dev_warn_ratelimited(&sensor->client->dev,
			"deep-lowlight: skip switch for unsupported output format 0x%04llx\n",
			out_fmt);
		return -EINVAL;
	}

	if (out_w != MT9M114_DEEP_LOWLIGHT_OUTPUT_WIDTH &&
	    out_w != MT9M114_DEEP_LOWLIGHT_OUTPUT_WIDTH_1288 &&
	    out_w != MT9M114_DEEP_LOWLIGHT_OUTPUT_WIDTH_1296)
		return -EINVAL;

	if (out_h == MT9M114_DEEP_LOWLIGHT_OUTPUT_HEIGHT_720) {
		*binned_height = MT9M114_DEEP_LOWLIGHT_BINNED_HEIGHT_720;
		*output_height = MT9M114_DEEP_LOWLIGHT_OUTPUT_HEIGHT_720;
		return 0;
	}

	if (out_h == MT9M114_DEEP_LOWLIGHT_OUTPUT_HEIGHT_960) {
		*binned_height = MT9M114_DEEP_LOWLIGHT_BINNED_HEIGHT_960;
		*output_height = MT9M114_DEEP_LOWLIGHT_OUTPUT_HEIGHT_960;
		return 0;
	}

	if (out_h == MT9M114_DEEP_LOWLIGHT_OUTPUT_HEIGHT_968) {
		*binned_height = MT9M114_DEEP_LOWLIGHT_BINNED_HEIGHT_976;
		*output_height = MT9M114_DEEP_LOWLIGHT_OUTPUT_HEIGHT_968;
		return 0;
	}

	if (out_h == MT9M114_DEEP_LOWLIGHT_OUTPUT_HEIGHT_976) {
		*binned_height = MT9M114_DEEP_LOWLIGHT_BINNED_HEIGHT_976;
		*output_height = MT9M114_DEEP_LOWLIGHT_OUTPUT_HEIGHT_976;
		return 0;
	}

	dev_dbg_ratelimited(&sensor->client->dev,
			    "deep-lowlight: skip switch for output %llux%llu (expected 1280x720/960, 1288x968, or 1296x976)\n",
			    out_w, out_h);

	return -EINVAL;
}

static int mt9m114_reapply_active_stream_config(struct mt9m114 *sensor)
{
	struct v4l2_subdev_state *pa_state;
	struct v4l2_subdev_state *ifp_state;
	bool pa_locked = false;
	bool ifp_locked = false;
	int ret;

	pa_state = v4l2_subdev_get_locked_active_state(&sensor->pa.sd);
	if (!pa_state) {
		pa_state = v4l2_subdev_lock_and_get_active_state(&sensor->pa.sd);
		pa_locked = true;
	}

	ifp_state = v4l2_subdev_get_locked_active_state(&sensor->ifp.sd);
	if (!ifp_state) {
		ifp_state = v4l2_subdev_lock_and_get_active_state(&sensor->ifp.sd);
		ifp_locked = true;
	}

	ret = mt9m114_configure_ifp(sensor, ifp_state);
	if (ret)
		goto out_unlock;

	ret = mt9m114_configure_pa(sensor, pa_state);
	if (ret)
		goto out_unlock;

	ret = mt9m114_set_frame_rate_with_state(sensor, pa_state);

out_unlock:
	if (ifp_locked)
		v4l2_subdev_unlock_state(ifp_state);
	if (pa_locked)
		v4l2_subdev_unlock_state(pa_state);

	return ret;
}

static int mt9m114_rekick_auto_exposure(struct mt9m114 *sensor)
{
	int ret;

	if (!sensor->ifp.ae_auto)
		return 0;

	ret = cci_write(sensor->regmap, MT9M114_AE_TRACK_ALGO, 0, NULL);
	if (ret)
		return ret;

	ret = cci_write(sensor->regmap, MT9M114_AE_TRACK_ALGO,
			MT9M114_AE_TRACK_EXEC_AUTOMATIC_EXPOSURE | 0x00fe,
			NULL);
	if (ret)
		return ret;

	return cci_write(sensor->regmap, MT9M114_AE_TRACK_AE_TRACKING_DAMPENING_SPEED,
			 MT9M114_AE_TRACK_SPEED_NORMAL, NULL);
}

static int mt9m114_set_deep_lowlight_mode(struct mt9m114 *sensor, bool enable)
{
	u64 read_mode_u64;
	u16 read_mode;
	u16 binned_height;
	u16 output_height;
	u16 output_width;
	int ret;

	if (!sensor->streaming)
		return 0;

	ret = mt9m114_get_deep_lowlight_geometry(sensor, &binned_height,
						 &output_height);
	if (ret)
		return 0;

	if (output_height == MT9M114_DEEP_LOWLIGHT_OUTPUT_HEIGHT_976)
		output_width = MT9M114_DEEP_LOWLIGHT_OUTPUT_WIDTH_1296;
	else if (output_height == MT9M114_DEEP_LOWLIGHT_OUTPUT_HEIGHT_968)
		output_width = MT9M114_DEEP_LOWLIGHT_OUTPUT_WIDTH_1288;
	else
		output_width = MT9M114_DEEP_LOWLIGHT_OUTPUT_WIDTH;

	ret = cci_read(sensor->regmap, MT9M114_CAM_SENSOR_CONTROL_READ_MODE,
		       &read_mode_u64, NULL);
	if (ret)
		return ret;

	read_mode = read_mode_u64;
	read_mode &= ~(MT9M114_CAM_SENSOR_CONTROL_X_READ_OUT_MASK |
		       MT9M114_CAM_SENSOR_CONTROL_Y_READ_OUT_MASK);
	if (enable)
		read_mode |= MT9M114_CAM_SENSOR_CONTROL_X_READ_OUT_SUMMING |
			     MT9M114_CAM_SENSOR_CONTROL_Y_READ_OUT_SUMMING;

	ret = 0;
	cci_write(sensor->regmap, MT9M114_CAM_SENSOR_CONTROL_READ_MODE,
		  read_mode, &ret);
	if (ret)
		return ret;

	if (enable) {
		ret = mt9m114_reapply_active_stream_config(sensor);
		if (ret)
			dev_warn(&sensor->client->dev,
				 "deep-lowlight: reapply active config failed: %d\n",
				 ret);

		ret = mt9m114_set_ae_fps_window(sensor,
						MT9M114_DEEP_LOWLIGHT_FPS_MIN,
						MT9M114_DEEP_LOWLIGHT_FPS_MAX);
		if (ret)
			dev_warn(&sensor->client->dev,
				 "deep-lowlight: failed to set %u-%u fps window: %d\n",
				 MT9M114_DEEP_LOWLIGHT_FPS_MIN,
				 MT9M114_DEEP_LOWLIGHT_FPS_MAX,
				 ret);

		ret = mt9m114_seed_transition_controls(sensor, true);
		if (ret)
			ret = mt9m114_seed_exposure_for_fps(sensor,
						  MT9M114_DEEP_LOWLIGHT_FPS_ENTER_SEED);
		if (ret)
			dev_warn(&sensor->client->dev,
				 "deep-lowlight: failed to seed %u fps on enter: %d\n",
				 MT9M114_DEEP_LOWLIGHT_FPS_ENTER_SEED, ret);
	}

	if (!enable) {
		ret = mt9m114_reapply_active_stream_config(sensor);
		if (ret)
			dev_warn(&sensor->client->dev,
				 "deep-lowlight: reapply active config failed: %d\n",
				 ret);

		ret = mt9m114_seed_transition_controls(sensor, false);
		if (ret)
			ret = mt9m114_seed_exposure_for_fps(sensor,
						  MT9M114_DEEP_LOWLIGHT_FPS_EXIT_SEED);
		if (ret)
			dev_warn(&sensor->client->dev,
				 "deep-lowlight: failed to seed %u fps on exit: %d\n",
				 MT9M114_DEEP_LOWLIGHT_FPS_EXIT_SEED, ret);

		ret = mt9m114_rekick_auto_exposure(sensor);
		if (ret)
			dev_warn(&sensor->client->dev,
				 "deep-lowlight: AE re-kick failed: %d\n", ret);
	}

	ret = mt9m114_set_state(sensor, MT9M114_SYS_STATE_ENTER_CONFIG_CHANGE);
	if (ret)
		return ret;

	ret = mt9m114_poll_state(sensor, MT9M114_SYS_STATE_STREAMING);
	if (ret)
		return ret;

	sensor->deep_lowlight_summing = enable;
	sensor->ifp.deep_lowlight_last_switch = jiffies;
	if (!enable)
		sensor->ifp.deep_lowlight_invalid_exit_guard_until = 0;

	dev_info(&sensor->client->dev,
		 "deep-lowlight: switched %s (2x2 summing, output %ux%u)\n",
		 enable ? "ON" : "OFF", output_width, output_height);

	return 0;
}

static void mt9m114_maybe_switch_deep_lowlight(struct mt9m114 *sensor)
{
	unsigned int gain;
	unsigned int exposure;

	gain = sensor->pa.gain->val;
	exposure = sensor->pa.exposure->val;

	mt9m114_maybe_switch_deep_lowlight_values(sensor, exposure, gain,
					    false, 0, false, 0);
}

static int mt9m114_write_exposure(struct mt9m114 *sensor, u32 exposure,
				  const struct v4l2_mbus_framefmt *format)
{
	u32 min_frame_length = exposure + 2;
	u32 frame_length = format->height + sensor->pa.vblank->val;
	u32 new_vblank;
	int ret;

	ret = mt9m114_update_vts_for_exposure(sensor, exposure);
	if (ret)
		return ret;

	if (min_frame_length > frame_length) {
		new_vblank = min_frame_length - format->height;
		if (new_vblank > MT9M114_MAX_VBLANK_LOWLIGHT)
			new_vblank = MT9M114_MAX_VBLANK_LOWLIGHT;

		frame_length = format->height + new_vblank;

		cci_write(sensor->regmap, MT9M114_FRAME_LENGTH_LINES,
			  frame_length, &ret);
		cci_write(sensor->regmap, MT9M114_CAM_SENSOR_CFG_FRAME_LENGTH_LINES,
			  frame_length, &ret);

		sensor->pa.vblank->val = new_vblank;
		sensor->pa.vblank->cur.val = new_vblank;
	}

	ret = cci_write(sensor->regmap, MT9M114_COARSE_INTEGRATION_TIME,
			exposure, NULL);
	if (ret)
		return ret;

	return cci_write(sensor->regmap,
			 MT9M114_CAM_SENSOR_CONTROL_COARSE_INTEGRATION_TIME,
			 exposure, NULL);
}

static int mt9m114_pa_g_ctrl(struct v4l2_ctrl *ctrl)
{
	struct mt9m114 *sensor = pa_ctrl_to_mt9m114(ctrl);
	u64 value;
	int ret;

	if (!pm_runtime_get_if_in_use(&sensor->client->dev))
		return 0;

	switch (ctrl->id) {
	case V4L2_CID_VBLANK:
		ret = cci_read(sensor->regmap, MT9M114_FRAME_LENGTH_LINES,
			       &value, NULL);
		if (ret)
			break;

		if (!sensor->pa.active_height)
			sensor->pa.active_height = MT9M114_PIXEL_ARRAY_HEIGHT;

		if (value <= sensor->pa.active_height)
			ctrl->val = MT9M114_MIN_VBLANK;
		else
			ctrl->val = clamp_t(u32, value - sensor->pa.active_height,
					 MT9M114_MIN_VBLANK,
					 MT9M114_MAX_VBLANK_LOWLIGHT);
		ret = 0;
		break;

	case V4L2_CID_EXPOSURE:
		ret = cci_read(sensor->regmap,
			       MT9M114_CAM_SENSOR_CONTROL_COARSE_INTEGRATION_TIME,
			       &value, NULL);
		if (ret)
			break;

		ctrl->val = value;
		break;

	case V4L2_CID_ANALOGUE_GAIN:
		ret = cci_read(sensor->regmap,
			       MT9M114_CAM_SENSOR_CONTROL_ANALOG_GAIN,
			       &value, NULL);
		if (ret)
			break;

		ctrl->val = value;
		break;

	default:
		ret = -EINVAL;
		break;
	}

	pm_runtime_put_autosuspend(&sensor->client->dev);

	return ret;
}

static int mt9m114_pa_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct mt9m114 *sensor = pa_ctrl_to_mt9m114(ctrl);
	const struct v4l2_mbus_framefmt *format;
	struct v4l2_subdev_state *state;
	unsigned int mask;
	u32 value;
	int ret = 0;

	if (!pm_runtime_get_if_in_use(&sensor->client->dev))
		return 0;

	state = v4l2_subdev_get_locked_active_state(&sensor->pa.sd);
	if (!state) {
		pm_runtime_put_autosuspend(&sensor->client->dev);
		return -EINVAL;
	}
	format = v4l2_subdev_state_get_format(state, 0);

	switch (ctrl->id) {
	case V4L2_CID_MT9M114_AE_METERING_PRESET:
		ret = mt9m114_apply_metering_preset(sensor, ctrl->val);
		break;

	case V4L2_CID_MT9M114_AE_TRACK_SPEED:
		cci_write(sensor->regmap, MT9M114_AE_TRACK_SPEED,
			  ctrl->val, &ret);
		break;

	case V4L2_CID_MT9M114_AE_RULE_ALGO:
		cci_write(sensor->regmap, MT9M114_AE_RULE_ALGO,
			  ctrl->val, &ret);
		break;

	case V4L2_CID_ANALOGUE_GAIN:
		cci_write(sensor->regmap, MT9M114_CAM_SENSOR_CONTROL_ANALOG_GAIN,
			  ctrl->val, &ret);
		cci_write(sensor->regmap, MT9M114_GLOBAL_GAIN,
			  ctrl->val, &ret);
		if (!ret)
			mt9m114_maybe_switch_deep_lowlight(sensor);
		break;

	case V4L2_CID_PIXEL_RATE:
		/* Read-only, nothing to apply. */
		break;

	case V4L2_CID_EXPOSURE:
		ret = mt9m114_ensure_manual_ae(sensor);
		if (ret)
			break;

		ret = mt9m114_group_hold(sensor, true);
		if (ret)
			break;

		ret = mt9m114_write_exposure(sensor, ctrl->val, format);
		if (ret)
			mt9m114_group_hold(sensor, false);
		else
			ret = mt9m114_group_hold(sensor, false);
		if (!ret)
			mt9m114_maybe_switch_deep_lowlight(sensor);
		break;

	case V4L2_CID_VBLANK:
		ret = mt9m114_ensure_manual_ae(sensor);
		if (ret)
			break;

		/*
		 * VBLANK (Vertical Blanking Lines)
		 *
		 * Frame_length_lines = PIXEL_ARRAY_HEIGHT (976) + VBLANK
		 *
		 * Uses GROUP_HOLD to synchronize with exposure if needed.
		 * Writes to CAM register:
		 *   0xC812 (CAM: CAM_SENSOR_CFG_FRAME_LENGTH_LINES)
		 */
		ret = mt9m114_group_hold(sensor, true);
		if (ret)
			break;

		value = ctrl->val + format->height;
		cci_write(sensor->regmap, MT9M114_FRAME_LENGTH_LINES, value, &ret);
		cci_write(sensor->regmap, MT9M114_CAM_SENSOR_CFG_FRAME_LENGTH_LINES,
			  value, &ret);

		/*
		 * Updating the frame length may extend the frame interval,
		 * shrinking the maximum exposure value. If it was at its
		 * maximum, it needs to be reduced to remain smaller than
		 * two lines less than the frame length.
		 */
		__v4l2_ctrl_modify_range(sensor->pa.exposure, 1,
				 value - 2, 1,
					 sensor->pa.exposure->default_value);
		if (ret)
			mt9m114_group_hold(sensor, false);
		else
			ret = mt9m114_group_hold(sensor, false);
		break;

	case V4L2_CID_HBLANK:
		cci_write(sensor->regmap, MT9M114_CAM_SENSOR_CFG_LINE_LENGTH_PCK,
			  ctrl->val + format->width, &ret);
		break;

	case V4L2_CID_HFLIP:
		mask = MT9M114_CAM_SENSOR_CONTROL_HORZ_MIRROR_EN;
		ret = cci_update_bits(sensor->regmap,
				      MT9M114_CAM_SENSOR_CONTROL_READ_MODE,
				      mask, ctrl->val ? mask : 0, NULL);
		break;

	case V4L2_CID_VFLIP:
		mask = MT9M114_CAM_SENSOR_CONTROL_VERT_FLIP_EN;
		ret = cci_update_bits(sensor->regmap,
				      MT9M114_CAM_SENSOR_CONTROL_READ_MODE,
				      mask, ctrl->val ? mask : 0, NULL);
		break;

	default:
		ret = -EINVAL;
		break;
	}

	pm_runtime_put_autosuspend(&sensor->client->dev);

	return ret;
}

static const struct v4l2_ctrl_ops mt9m114_pa_ctrl_ops = {
	.g_volatile_ctrl = mt9m114_pa_g_ctrl,
	.s_ctrl = mt9m114_pa_s_ctrl,
};

static void mt9m114_pa_ctrl_update_exposure(struct mt9m114 *sensor, bool manual)
{
	/*
	 * Update the volatile flag on the manual exposure and gain controls.
	 * If the controls have switched to manual, read their current value
	 * from the hardware to ensure that control read and write operations
	 * will behave correctly
	 */
	if (manual) {
		mt9m114_pa_g_ctrl(sensor->pa.exposure);
		sensor->pa.exposure->cur.val = sensor->pa.exposure->val;
		sensor->pa.exposure->flags &= ~V4L2_CTRL_FLAG_VOLATILE;

		mt9m114_pa_g_ctrl(sensor->pa.gain);
		sensor->pa.gain->cur.val = sensor->pa.gain->val;
		sensor->pa.gain->flags &= ~V4L2_CTRL_FLAG_VOLATILE;
	} else {
		sensor->pa.exposure->flags |= V4L2_CTRL_FLAG_VOLATILE;
		sensor->pa.gain->flags |= V4L2_CTRL_FLAG_VOLATILE;
	}
}

static void mt9m114_pa_ctrl_update_blanking(struct mt9m114 *sensor,
					    struct v4l2_subdev_state *state,
					    const struct v4l2_mbus_framefmt *format)
{
	unsigned int max_blank;

	/* Update the blanking controls ranges based on the output size. */
	max_blank = MT9M114_CAM_SENSOR_CFG_LINE_LENGTH_PCK_MAX
		  - format->width;
	__v4l2_ctrl_modify_range(sensor->pa.hblank, MT9M114_MIN_HBLANK,
				 max_blank, 1, MT9M114_DEF_HBLANK);

	sensor->pa.active_width = format->width;
	sensor->pa.active_height = format->height;

	max_blank = MT9M114_CAM_SENSOR_CFG_FRAME_LENGTH_LINES_MAX
		  - format->height;
	__v4l2_ctrl_modify_range(sensor->pa.vblank, MT9M114_MIN_VBLANK,
				 max_blank, 1, MT9M114_DEF_VBLANK);

	mt9m114_update_vblank_range_for_min_fps(sensor, state,
						     mt9m114_get_min_fps(sensor));
}

/* -----------------------------------------------------------------------------
 * Pixel Array Subdev Operations
 */

static inline struct mt9m114 *pa_to_mt9m114(struct v4l2_subdev *sd)
{
	return container_of(sd, struct mt9m114, pa.sd);
}

static int mt9m114_pa_init_state(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state)
{
	struct v4l2_mbus_framefmt *format;
	struct v4l2_rect *crop;

	crop = v4l2_subdev_state_get_crop(state, 0);

	crop->left = 0;
	crop->top = 0;
	crop->width = MT9M114_PIXEL_ARRAY_WIDTH;
	crop->height = MT9M114_PIXEL_ARRAY_HEIGHT;

	format = v4l2_subdev_state_get_format(state, 0);

	format->width = MT9M114_PIXEL_ARRAY_WIDTH;
	format->height = MT9M114_PIXEL_ARRAY_HEIGHT;
	format->code = MEDIA_BUS_FMT_SGRBG10_1X10;
	format->field = V4L2_FIELD_NONE;
	format->colorspace = V4L2_COLORSPACE_RAW;
	format->ycbcr_enc = V4L2_YCBCR_ENC_601;
	format->quantization = V4L2_QUANTIZATION_FULL_RANGE;
	format->xfer_func = V4L2_XFER_FUNC_NONE;

	return 0;
}

static int mt9m114_pa_enum_mbus_code(struct v4l2_subdev *sd,
				     struct v4l2_subdev_state *state,
				     struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->index > 0)
		return -EINVAL;

	code->code = MEDIA_BUS_FMT_SGRBG10_1X10;

	return 0;
}

static int mt9m114_pa_enum_framesizes(struct v4l2_subdev *sd,
				      struct v4l2_subdev_state *state,
				      struct v4l2_subdev_frame_size_enum *fse)
{
	if (fse->index > 1)
		return -EINVAL;

	if (fse->code != MEDIA_BUS_FMT_SGRBG10_1X10)
		return -EINVAL;

	/* Report binning capability through frame size enumeration. */
	fse->min_width = MT9M114_PIXEL_ARRAY_WIDTH / (fse->index + 1);
	fse->max_width = MT9M114_PIXEL_ARRAY_WIDTH / (fse->index + 1);
	fse->min_height = MT9M114_PIXEL_ARRAY_HEIGHT / (fse->index + 1);
	fse->max_height = MT9M114_PIXEL_ARRAY_HEIGHT / (fse->index + 1);

	return 0;
}

static int mt9m114_pa_set_fmt(struct v4l2_subdev *sd,
			      struct v4l2_subdev_state *state,
			      struct v4l2_subdev_format *fmt)
{
	struct mt9m114 *sensor = pa_to_mt9m114(sd);
	struct v4l2_mbus_framefmt *format;
	struct v4l2_rect *crop;
	unsigned int hscale;
	unsigned int vscale;

	crop = v4l2_subdev_state_get_crop(state, fmt->pad);
	format = v4l2_subdev_state_get_format(state, fmt->pad);

	/* The sensor can bin horizontally and vertically. */
	hscale = DIV_ROUND_CLOSEST(crop->width, fmt->format.width ? : 1);
	vscale = DIV_ROUND_CLOSEST(crop->height, fmt->format.height ? : 1);
	format->width = crop->width / clamp(hscale, 1U, 2U);
	format->height = crop->height / clamp(vscale, 1U, 2U);

	fmt->format = *format;

	if (fmt->which == V4L2_SUBDEV_FORMAT_ACTIVE)
		mt9m114_pa_ctrl_update_blanking(sensor, state, format);

	return 0;
}

static int mt9m114_pa_get_selection(struct v4l2_subdev *sd,
				    struct v4l2_subdev_state *state,
				    struct v4l2_subdev_selection *sel)
{
	switch (sel->target) {
	case V4L2_SEL_TGT_CROP:
		sel->r = *v4l2_subdev_state_get_crop(state, sel->pad);
		return 0;

	case V4L2_SEL_TGT_CROP_DEFAULT:
	case V4L2_SEL_TGT_CROP_BOUNDS:
	case V4L2_SEL_TGT_NATIVE_SIZE:
		sel->r.left = 0;
		sel->r.top = 0;
		sel->r.width = MT9M114_PIXEL_ARRAY_WIDTH;
		sel->r.height = MT9M114_PIXEL_ARRAY_HEIGHT;
		return 0;

	default:
		return -EINVAL;
	}
}

static int mt9m114_pa_set_selection(struct v4l2_subdev *sd,
				    struct v4l2_subdev_state *state,
				    struct v4l2_subdev_selection *sel)
{
	struct mt9m114 *sensor = pa_to_mt9m114(sd);
	struct v4l2_mbus_framefmt *format;
	struct v4l2_rect *crop;
	int ret = 0;

	if (sel->target != V4L2_SEL_TGT_CROP)
		return -EINVAL;

	crop = v4l2_subdev_state_get_crop(state, sel->pad);
	format = v4l2_subdev_state_get_format(state, sel->pad);

	/*
	 * Clamp the crop rectangle. The vertical coordinates must be even, and
	 * the horizontal coordinates must be a multiple of 4.
	 *
	 * FIXME: The horizontal coordinates must be a multiple of 8 when
	 * binning, but binning is configured after setting the selection, so
	 * we can't know tell here if it will be used.
	 */
	sel->r.left = ALIGN(sel->r.left, 4);
	sel->r.top = ALIGN(sel->r.top, 2);
	sel->r.width = clamp_t(unsigned int, ALIGN(sel->r.width, 4),
			       MT9M114_PIXEL_ARRAY_MIN_OUTPUT_WIDTH,
			       MT9M114_PIXEL_ARRAY_WIDTH - sel->r.left);
	sel->r.height = clamp_t(unsigned int, ALIGN(sel->r.height, 2),
				MT9M114_PIXEL_ARRAY_MIN_OUTPUT_HEIGHT,
				MT9M114_PIXEL_ARRAY_HEIGHT - sel->r.top);

	/* Changing the selection size is not allowed in streaming state. */
	if (sensor->streaming &&
	    (sel->r.height != crop->height || sel->r.width != crop->width))
		return -EBUSY;

	*crop = sel->r;

	/* Reset the format. */
	format->width = crop->width;
	format->height = crop->height;

	if (sel->which != V4L2_SUBDEV_FORMAT_ACTIVE)
		return ret;

	mt9m114_pa_ctrl_update_blanking(sensor, state, format);

	/* Apply values immediately if streaming. */
	if (sensor->streaming) {
		ret = mt9m114_configure_pa(sensor, state);
		if (ret)
			return ret;
		/* Changing the cropping config requires a CONFIG_CHANGE. */
		ret = mt9m114_set_state(sensor,
					MT9M114_SYS_STATE_ENTER_CONFIG_CHANGE);
	}
	return ret;
}

static const struct v4l2_subdev_pad_ops mt9m114_pa_pad_ops = {
	.enum_mbus_code = mt9m114_pa_enum_mbus_code,
	.enum_frame_size = mt9m114_pa_enum_framesizes,
	.get_fmt = v4l2_subdev_get_fmt,
	.set_fmt = mt9m114_pa_set_fmt,
	.get_selection = mt9m114_pa_get_selection,
	.set_selection = mt9m114_pa_set_selection,
};

static const struct v4l2_subdev_ops mt9m114_pa_ops = {
	.pad = &mt9m114_pa_pad_ops,
};

static const struct v4l2_subdev_internal_ops mt9m114_pa_internal_ops = {
	.init_state = mt9m114_pa_init_state,
};

static int mt9m114_pa_init(struct mt9m114 *sensor)
{
	struct v4l2_ctrl_handler *hdl = &sensor->pa.hdl;
	struct v4l2_subdev *sd = &sensor->pa.sd;
	struct media_pad *pads = &sensor->pa.pad;
	const struct v4l2_mbus_framefmt *format;
	struct v4l2_subdev_state *state;
	unsigned int max_exposure;
	int ret;

	/* Initialize the subdev. */
	v4l2_subdev_init(sd, &mt9m114_pa_ops);
	sd->internal_ops = &mt9m114_pa_internal_ops;
	v4l2_i2c_subdev_set_name(sd, sensor->client, NULL, " pixel array");

	sd->flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	sd->owner = THIS_MODULE;
	sd->dev = &sensor->client->dev;
	v4l2_set_subdevdata(sd, sensor->client);

	/* Initialize the media entity. */
	sd->entity.function = MEDIA_ENT_F_CAM_SENSOR;
	sd->entity.ops = &mt9m114_entity_ops;
	pads[0].flags = MEDIA_PAD_FL_SOURCE;
	ret = media_entity_pads_init(&sd->entity, 1, pads);
	if (ret < 0)
		return ret;

	/* Initialize the control handler. */
	v4l2_ctrl_handler_init(hdl, 10); /* Increased for 3 new custom controls */

	/* The range of the HBLANK and VBLANK controls will be updated below. */
	sensor->pa.hblank = v4l2_ctrl_new_std(hdl, &mt9m114_pa_ctrl_ops,
					      V4L2_CID_HBLANK,
					      MT9M114_DEF_HBLANK,
					      MT9M114_DEF_HBLANK, 1,
					      MT9M114_DEF_HBLANK);
	sensor->pa.vblank = v4l2_ctrl_new_std(hdl, &mt9m114_pa_ctrl_ops,
					      V4L2_CID_VBLANK,
					      MT9M114_DEF_VBLANK,
					      MT9M114_DEF_VBLANK, 1,
					      MT9M114_DEF_VBLANK);

	/*
	 * Custom control: AE Metering Preset
	 * User-friendly alternative to exposing 25 individual weight controls.
	 * Each preset applies a predefined pattern optimized for a use case.
	 */
	static const struct v4l2_ctrl_config ae_metering_preset_cfg = {
		.ops = &mt9m114_pa_ctrl_ops,
		.id = V4L2_CID_MT9M114_AE_METERING_PRESET,
		.type = V4L2_CTRL_TYPE_MENU,
		.name = "AE Metering Preset",
		.min = 0,
		.max = ARRAY_SIZE(mt9m114_metering_preset_names) - 2,
		.def = MT9M114_METERING_PRESET_CENTER,
		.qmenu = mt9m114_metering_preset_names,
	};
	sensor->pa.ae_metering_preset = v4l2_ctrl_new_custom(hdl, &ae_metering_preset_cfg, NULL);

	/* Custom control: AE Tracking Speed (0x31AC register) */
	static const struct v4l2_ctrl_config ae_track_speed_cfg = {
		.ops = &mt9m114_pa_ctrl_ops,
		.id = V4L2_CID_MT9M114_AE_TRACK_SPEED,
		.type = V4L2_CTRL_TYPE_INTEGER,
		.name = "AE Track Speed",
		.min = MT9M114_AE_TRACK_SPEED_NORMAL,
		.max = 0x07,
		.step = 1,
		.def = MT9M114_AE_TRACK_SPEED_NORMAL,
		.flags = V4L2_CTRL_FLAG_SLIDER,
	};
	sensor->pa.ae_track_speed = v4l2_ctrl_new_custom(hdl, &ae_track_speed_cfg, NULL);

	if (sensor->pa.ae_track_speed)
		sensor->pa.ae_track_speed->flags |= V4L2_CTRL_FLAG_SLIDER;

	/* Custom control: AE Rule Algorithm (0xA404 register) */
	static const struct v4l2_ctrl_config ae_rule_algo_cfg = {
		.ops = &mt9m114_pa_ctrl_ops,
		.id = V4L2_CID_MT9M114_AE_RULE_ALGO,
		.type = V4L2_CTRL_TYPE_MENU,
		.name = "AE Algorithm Mode",
		.min = 0,
		.max = ARRAY_SIZE(mt9m114_ae_rule_algo_names) - 2,
		.def = 0,
		.qmenu = mt9m114_ae_rule_algo_names,
	};
	sensor->pa.ae_rule_algo = v4l2_ctrl_new_custom(hdl, &ae_rule_algo_cfg, NULL);

	/*
	 * The maximum coarse integration time is the frame length in lines
	 * minus two (hardware requirement to prevent timing generator freeze).
	 * Extended to support low-light mode with VTS up to ~30000 lines.
	 */
	max_exposure = MT9M114_PIXEL_ARRAY_HEIGHT + MT9M114_MIN_VBLANK - 2;
	sensor->pa.exposure = v4l2_ctrl_new_std(hdl, &mt9m114_pa_ctrl_ops,
						V4L2_CID_EXPOSURE, 1,
						max_exposure, 1, 16);
	if (sensor->pa.exposure)
		sensor->pa.exposure->flags |= V4L2_CTRL_FLAG_VOLATILE;

	sensor->pa.gain = v4l2_ctrl_new_std(hdl, &mt9m114_pa_ctrl_ops,
					    V4L2_CID_ANALOGUE_GAIN, 1,
					    511, 1, 32);
	if (sensor->pa.gain)
		sensor->pa.gain->flags |= V4L2_CTRL_FLAG_VOLATILE;

	{
		struct v4l2_ctrl *pixel_rate;

		pixel_rate = v4l2_ctrl_new_std(hdl, &mt9m114_pa_ctrl_ops,
					 V4L2_CID_PIXEL_RATE,
					 sensor->pixrate, sensor->pixrate, 1,
					 sensor->pixrate);
		if (pixel_rate)
			pixel_rate->flags |= V4L2_CTRL_FLAG_READ_ONLY;
	}

	v4l2_ctrl_new_std(hdl, &mt9m114_pa_ctrl_ops,
			  V4L2_CID_HFLIP,
			  0, 1, 1, 0);
	v4l2_ctrl_new_std(hdl, &mt9m114_pa_ctrl_ops,
			  V4L2_CID_VFLIP,
			  0, 1, 1, 0);

	if (hdl->error) {
		ret = hdl->error;
		goto error;
	}

	sd->state_lock = hdl->lock;

	ret = v4l2_subdev_init_finalize(sd);
	if (ret)
		goto error;

	/* Update the range of the blanking controls based on the format. */
	state = v4l2_subdev_lock_and_get_active_state(sd);
	format = v4l2_subdev_state_get_format(state, 0);
	mt9m114_pa_ctrl_update_blanking(sensor, state, format);
	v4l2_subdev_unlock_state(state);

	sd->ctrl_handler = hdl;

	init_completion(&sensor->ifp.unregistered);

	return 0;

error:
	v4l2_ctrl_handler_free(&sensor->pa.hdl);
	media_entity_cleanup(&sensor->pa.sd.entity);
	return ret;
}

static void mt9m114_pa_cleanup(struct mt9m114 *sensor)
{
	v4l2_ctrl_handler_free(&sensor->pa.hdl);
	media_entity_cleanup(&sensor->pa.sd.entity);
}

/* -----------------------------------------------------------------------------
 * Image Flow Processor Control Operations
 */

static const char * const mt9m114_test_pattern_menu[] = {
	"Disabled",
	"Solid Color",
	"100% Color Bars",
	"Pseudo-Random",
	"Fade-to-Gray Color Bars",
	"Walking Ones 10-bit",
	"Walking Ones 8-bit",
};

/* Keep in sync with mt9m114_test_pattern_menu */
static const unsigned int mt9m114_test_pattern_value[] = {
	MT9M114_CAM_MODE_TEST_PATTERN_SELECT_SOLID,
	MT9M114_CAM_MODE_TEST_PATTERN_SELECT_SOLID_BARS,
	MT9M114_CAM_MODE_TEST_PATTERN_SELECT_RANDOM,
	MT9M114_CAM_MODE_TEST_PATTERN_SELECT_FADING_BARS,
	MT9M114_CAM_MODE_TEST_PATTERN_SELECT_WALKING_1S_10B,
	MT9M114_CAM_MODE_TEST_PATTERN_SELECT_WALKING_1S_8B,
};

static inline struct mt9m114 *ifp_ctrl_to_mt9m114(struct v4l2_ctrl *ctrl)
{
	return container_of(ctrl->handler, struct mt9m114, ifp.hdl);
}

static int mt9m114_read_logical_u16(struct mt9m114 *sensor, u16 address,
				    u16 *value)
{
	u64 raw = 0;
	int ret = 0;

	cci_write(sensor->regmap, MT9M114_LOGICAL_ADDRESS_ACCESS, address, &ret);
	cci_read(sensor->regmap, MT9M114_MCU_VARIABLE_DATA0, &raw, &ret);
	if (ret)
		return ret;

	*value = raw;

	return 0;
}

static int mt9m114_dump_stats(struct mt9m114 *sensor)
{
	static const u16 blc_addr = 0x0a06;
	static const u16 stats_start = 0x0a54;
	static const u16 stats_end = 0x0a86;
	static const unsigned int values_per_line = 5;
	u16 values[((stats_end - stats_start) / 2) + 1];
	u16 blc = 0;
	u32 sum = 0;
	u32 low_sum = 0;
	u32 mid_sum = 0;
	u32 high_sum = 0;
	u16 peak = 0;
	u16 peak_addr = stats_start;
	u16 non_zero = 0;
	const char *dominant_zone = "mixed";
	unsigned int i;
	int ret;

	ret = mt9m114_read_logical_u16(sensor, blc_addr, &blc);
	if (ret) {
		dev_err(&sensor->client->dev,
			"debug-dump: failed reading BLC 0x%04x (%d)\n",
			blc_addr, ret);
		return ret;
	}

	for (i = 0; i < ARRAY_SIZE(values); i++) {
		u16 addr = stats_start + i * 2;

		ret = mt9m114_read_logical_u16(sensor, addr, &values[i]);
		if (ret) {
			dev_err(&sensor->client->dev,
				"debug-dump: failed reading AE zone @0x%04x (%d)\n",
				addr, ret);
			return ret;
		}

		sum += values[i];
		if (i < ARRAY_SIZE(values) / 3)
			low_sum += values[i];
		else if (i < (2 * ARRAY_SIZE(values)) / 3)
			mid_sum += values[i];
		else
			high_sum += values[i];

		if (values[i])
			non_zero++;
		if (values[i] >= peak) {
			peak = values[i];
			peak_addr = addr;
		}
	}

	if (low_sum > mid_sum && low_sum > high_sum)
		dominant_zone = "low";
	else if (mid_sum > low_sum && mid_sum > high_sum)
		dominant_zone = "mid";
	else if (high_sum > low_sum && high_sum > mid_sum)
		dominant_zone = "high";

	dev_info(&sensor->client->dev,
		 "debug-dump: logical BLC[0x%04x]=0x%04x (%u)\n",
		 blc_addr, blc, blc);
	dev_info(&sensor->client->dev,
		 "debug-dump: AE summary: sum=%u nonzero=%u/%zu peak=%u @0x%04x\n",
		 sum, non_zero, ARRAY_SIZE(values), peak, peak_addr);
	dev_info(&sensor->client->dev,
		 "debug-dump: AE buckets: low=%u mid=%u high=%u dominant=%s\n",
		 low_sum, mid_sum, high_sum, dominant_zone);

	for (i = 0; i < ARRAY_SIZE(values); i += values_per_line) {
		unsigned int end = min(i + values_per_line, (unsigned int)ARRAY_SIZE(values));

		if (end - i == 5)
			dev_info(&sensor->client->dev,
				 "debug-dump: AE[0x%04x..0x%04x] = %u %u %u %u %u\n",
				 stats_start + i * 2, stats_start + (end - 1) * 2,
				 values[i], values[i + 1], values[i + 2], values[i + 3], values[i + 4]);
		else if (end - i == 4)
			dev_info(&sensor->client->dev,
				 "debug-dump: AE[0x%04x..0x%04x] = %u %u %u %u\n",
				 stats_start + i * 2, stats_start + (end - 1) * 2,
				 values[i], values[i + 1], values[i + 2], values[i + 3]);
		else if (end - i == 3)
			dev_info(&sensor->client->dev,
				 "debug-dump: AE[0x%04x..0x%04x] = %u %u %u\n",
				 stats_start + i * 2, stats_start + (end - 1) * 2,
				 values[i], values[i + 1], values[i + 2]);
		else if (end - i == 2)
			dev_info(&sensor->client->dev,
				 "debug-dump: AE[0x%04x..0x%04x] = %u %u\n",
				 stats_start + i * 2, stats_start + (end - 1) * 2,
				 values[i], values[i + 1]);
		else
			dev_info(&sensor->client->dev,
				 "debug-dump: AE[0x%04x..0x%04x] = %u\n",
				 stats_start + i * 2, stats_start + (end - 1) * 2,
				 values[i]);
	}

	return 0;
}

static int mt9m114_ifp_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct mt9m114 *sensor = ifp_ctrl_to_mt9m114(ctrl);
	u32 value;
	int ret = 0;

	if (ctrl->id == V4L2_CID_EXPOSURE_AUTO)
		mt9m114_pa_ctrl_update_exposure(sensor,
						ctrl->val != V4L2_EXPOSURE_AUTO);

	if (ctrl->id == V4L2_CID_EXPOSURE_AUTO) {
		sensor->ifp.ae_auto = ctrl->val == V4L2_EXPOSURE_AUTO;
		if (sensor->pa.vblank) {
			if (sensor->ifp.ae_auto)
				sensor->pa.vblank->flags |= V4L2_CTRL_FLAG_VOLATILE;
			else
				sensor->pa.vblank->flags &= ~V4L2_CTRL_FLAG_VOLATILE;
		}

		if (!sensor->ifp.ae_auto)
			cancel_delayed_work_sync(&sensor->ifp.smart_meter_work);
	}

	if (ctrl->id == V4L2_CID_ILLUMINATORS_1) {
		ret = pm_runtime_resume_and_get(&sensor->client->dev);
		if (ret < 0)
			return ret;

		ret = mt9m114_dump_stats(sensor);

		pm_runtime_put_autosuspend(&sensor->client->dev);

		return ret;
	}

	/* V4L2 controls values are applied only when power is up. */
	if (!pm_runtime_get_if_in_use(&sensor->client->dev))
		return 0;

	switch (ctrl->id) {
	case V4L2_CID_AUTO_WHITE_BALANCE:
		/* Control both the AWB mode and the CCM algorithm. */
		if (ctrl->val)
			value = MT9M114_CAM_AWB_MODE_AUTO
			      | MT9M114_CAM_AWB_MODE_EXCLUSIVE_AE;
		else
			value = 0;

		cci_write(sensor->regmap, MT9M114_CAM_AWB_AWBMODE, value, &ret);

		if (ctrl->val)
			value = MT9M114_CCM_EXEC_CALC_CCM_MATRIX | 0x22;
		else
			value = 0;

		cci_write(sensor->regmap, MT9M114_CCM_ALGO, value, &ret);
		break;

	case V4L2_CID_EXPOSURE_AUTO:
		if (ctrl->val == V4L2_EXPOSURE_AUTO)
			value = MT9M114_AE_TRACK_EXEC_AUTOMATIC_EXPOSURE
			      | 0x00fe;
		else
			value = 0;

		cci_write(sensor->regmap, MT9M114_AE_TRACK_ALGO, value, &ret);
		if (ret)
			break;

		ret = mt9m114_set_frame_rate(sensor);
		if (ret)
			break;

		if (sensor->ifp.ae_auto && sensor->streaming && mt9m114_smart_metering) {
			sensor->ifp.smart_metering_last_switch = 0;
			sensor->ifp.smart_metering_active_preset =
				sensor->pa.ae_metering_preset ?
				sensor->pa.ae_metering_preset->val :
				MT9M114_METERING_PRESET_CENTER;
			schedule_delayed_work(&sensor->ifp.smart_meter_work,
					      msecs_to_jiffies(MT9M114_SMART_METER_INTERVAL_MS));
		} else {
			mt9m114_smart_metering_set_blc(sensor, false);
		}

		break;

	case V4L2_CID_PIXEL_RATE:
	case V4L2_CID_LINK_FREQ:
		/* Read-only, nothing to apply. */
		break;

	case V4L2_CID_ILLUMINATORS_1:
		ret = 0;
		break;

	case V4L2_CID_TEST_PATTERN:
	case V4L2_CID_TEST_PATTERN_RED:
	case V4L2_CID_TEST_PATTERN_GREENR:
	case V4L2_CID_TEST_PATTERN_BLUE: {
		unsigned int pattern = sensor->ifp.tpg[MT9M114_TPG_PATTERN]->val;

		if (pattern) {
			cci_write(sensor->regmap, MT9M114_CAM_MODE_SELECT,
				  MT9M114_CAM_MODE_SELECT_TEST_PATTERN, &ret);
			cci_write(sensor->regmap,
				  MT9M114_CAM_MODE_TEST_PATTERN_SELECT,
				  mt9m114_test_pattern_value[pattern - 1], &ret);
			cci_write(sensor->regmap,
				  MT9M114_CAM_MODE_TEST_PATTERN_RED,
				  sensor->ifp.tpg[MT9M114_TPG_RED]->val, &ret);
			cci_write(sensor->regmap,
				  MT9M114_CAM_MODE_TEST_PATTERN_GREEN,
				  sensor->ifp.tpg[MT9M114_TPG_GREEN]->val, &ret);
			cci_write(sensor->regmap,
				  MT9M114_CAM_MODE_TEST_PATTERN_BLUE,
				  sensor->ifp.tpg[MT9M114_TPG_BLUE]->val, &ret);
		} else {
			cci_write(sensor->regmap, MT9M114_CAM_MODE_SELECT,
				  MT9M114_CAM_MODE_SELECT_NORMAL, &ret);
		}

		/*
		 * A Config-Change needs to be issued for the change to take
		 * effect. If we're not streaming ignore this, the change will
		 * be applied when the stream is started.
		 */
		if (ret || !sensor->streaming)
			break;

		ret = mt9m114_set_state(sensor,
					MT9M114_SYS_STATE_ENTER_CONFIG_CHANGE);
		break;
	}

	default:
		ret = -EINVAL;
		break;
	}

	pm_runtime_put_autosuspend(&sensor->client->dev);

	return ret;
}

static const struct v4l2_ctrl_ops mt9m114_ifp_ctrl_ops = {
	.s_ctrl = mt9m114_ifp_s_ctrl,
};

/* -----------------------------------------------------------------------------
 * Image Flow Processor Subdev Operations
 */

static inline struct mt9m114 *ifp_to_mt9m114(struct v4l2_subdev *sd)
{
	return container_of(sd, struct mt9m114, ifp.sd);
}

static int mt9m114_ifp_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct mt9m114 *sensor = ifp_to_mt9m114(sd);
	struct v4l2_subdev_state *pa_state;
	struct v4l2_subdev_state *ifp_state;
	int ret;

	if (!enable)
		return mt9m114_stop_streaming(sensor);

	ifp_state = v4l2_subdev_lock_and_get_active_state(&sensor->ifp.sd);
	pa_state = v4l2_subdev_lock_and_get_active_state(&sensor->pa.sd);

	ret = mt9m114_start_streaming(sensor, pa_state, ifp_state);

	v4l2_subdev_unlock_state(pa_state);
	v4l2_subdev_unlock_state(ifp_state);

	return ret;
}

static int mt9m114_ifp_get_frame_interval(struct v4l2_subdev *sd,
					  struct v4l2_subdev_state *sd_state,
					  struct v4l2_subdev_frame_interval *interval)
{
	struct v4l2_fract *ival = &interval->interval;
	struct mt9m114 *sensor = ifp_to_mt9m114(sd);

	/*
	 * FIXME: Implement support for V4L2_SUBDEV_FORMAT_TRY, using the V4L2
	 * subdev active state API.
	 */
	if (interval->which != V4L2_SUBDEV_FORMAT_ACTIVE)
		return -EINVAL;

	ival->numerator = 1;
	ival->denominator = sensor->ifp.frame_rate;

	return 0;
}

static int mt9m114_ifp_set_frame_interval(struct v4l2_subdev *sd,
					  struct v4l2_subdev_state *sd_state,
					  struct v4l2_subdev_frame_interval *interval)
{
	struct v4l2_fract *ival = &interval->interval;
	struct mt9m114 *sensor = ifp_to_mt9m114(sd);
	int ret = 0;

	/*
	 * FIXME: Implement support for V4L2_SUBDEV_FORMAT_TRY, using the V4L2
	 * subdev active state API.
	 */
	if (interval->which != V4L2_SUBDEV_FORMAT_ACTIVE)
		return -EINVAL;

	if (ival->numerator != 0 && ival->denominator != 0)
		sensor->ifp.frame_rate = min_t(unsigned int,
					       ival->denominator / ival->numerator,
					       MT9M114_MAX_FRAME_RATE);
	else
		sensor->ifp.frame_rate = MT9M114_MAX_FRAME_RATE;

	ival->numerator = 1;
	ival->denominator = sensor->ifp.frame_rate;

	if (sensor->streaming)
		ret = mt9m114_set_frame_rate(sensor);

	return ret;
}

static int mt9m114_ifp_init_state(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state)
{
	struct mt9m114 *sensor = ifp_to_mt9m114(sd);
	struct v4l2_mbus_framefmt *format;
	struct v4l2_rect *crop;
	struct v4l2_rect *compose;

	format = v4l2_subdev_state_get_format(state, 0);

	format->width = MT9M114_PIXEL_ARRAY_WIDTH;
	format->height = MT9M114_PIXEL_ARRAY_HEIGHT;
	format->code = MEDIA_BUS_FMT_SGRBG10_1X10;
	format->field = V4L2_FIELD_NONE;
	format->colorspace = V4L2_COLORSPACE_RAW;
	format->ycbcr_enc = V4L2_YCBCR_ENC_601;
	format->quantization = V4L2_QUANTIZATION_FULL_RANGE;
	format->xfer_func = V4L2_XFER_FUNC_NONE;

	crop = v4l2_subdev_state_get_crop(state, 0);

	crop->left = 4;
	crop->top = 4;
	crop->width = format->width - 8;
	crop->height = format->height - 8;

	compose = v4l2_subdev_state_get_compose(state, 0);

	compose->left = 0;
	compose->top = 0;
	compose->width = crop->width;
	compose->height = crop->height;

	format = v4l2_subdev_state_get_format(state, 1);

	format->width = compose->width;
	format->height = compose->height;
	format->code = mt9m114_default_ifp_src_code(sensor);
	format->field = V4L2_FIELD_NONE;
	format->colorspace = V4L2_COLORSPACE_SRGB;
	format->ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
	format->quantization = V4L2_QUANTIZATION_DEFAULT;
	format->xfer_func = V4L2_XFER_FUNC_DEFAULT;

	return 0;
}

static int mt9m114_ifp_enum_mbus_code(struct v4l2_subdev *sd,
				      struct v4l2_subdev_state *state,
				      struct v4l2_subdev_mbus_code_enum *code)
{
	const unsigned int num_formats = ARRAY_SIZE(mt9m114_format_infos);
	struct mt9m114 *sensor = ifp_to_mt9m114(sd);
	unsigned int index = 0;
	unsigned int flag;
	unsigned int i;

	switch (code->pad) {
	case 0:
		if (code->index != 0)
			return -EINVAL;

		code->code = mt9m114_format_infos[num_formats - 1].code;
		return 0;

	case 1:
		if (mt9m114_ifp_yuv_test_active(sensor)) {
			if (code->index != 0)
				return -EINVAL;

			code->code = mt9m114_default_ifp_src_code(sensor);
			return 0;
		}

		if (sensor->bus_cfg.bus_type == V4L2_MBUS_CSI2_DPHY)
			flag = MT9M114_FMT_FLAG_CSI2;
		else
			flag = MT9M114_FMT_FLAG_PARALLEL;

		for (i = 0; i < num_formats; ++i) {
			const struct mt9m114_format_info *info =
				&mt9m114_format_infos[i];

			if (info->flags & flag) {
				if (index == code->index) {
					code->code = info->code;
					return 0;
				}

				index++;
			}
		}

		return -EINVAL;

	default:
		return -EINVAL;
	}
}

static int mt9m114_ifp_enum_framesizes(struct v4l2_subdev *sd,
				       struct v4l2_subdev_state *state,
				       struct v4l2_subdev_frame_size_enum *fse)
{
	struct mt9m114 *sensor = ifp_to_mt9m114(sd);
	const struct mt9m114_format_info *info;

	if (fse->index > 0)
		return -EINVAL;

	info = mt9m114_format_info(sensor, fse->pad, fse->code);
	if (!info || info->code != fse->code)
		return -EINVAL;

	if (fse->pad == 0) {
		fse->min_width = MT9M114_PIXEL_ARRAY_MIN_OUTPUT_WIDTH;
		fse->max_width = MT9M114_PIXEL_ARRAY_WIDTH;
		fse->min_height = MT9M114_PIXEL_ARRAY_MIN_OUTPUT_HEIGHT;
		fse->max_height = MT9M114_PIXEL_ARRAY_HEIGHT;
	} else {
		const struct v4l2_rect *crop;

		crop = v4l2_subdev_state_get_crop(state, 0);

		fse->max_width = crop->width;
		fse->max_height = crop->height;

		fse->min_width = fse->max_width / 4;
		fse->min_height = fse->max_height / 4;
	}

	return 0;
}

static int mt9m114_ifp_enum_frameintervals(struct v4l2_subdev *sd,
					   struct v4l2_subdev_state *state,
					   struct v4l2_subdev_frame_interval_enum *fie)
{
	struct mt9m114 *sensor = ifp_to_mt9m114(sd);
	const struct mt9m114_format_info *info;

	if (fie->index > 0)
		return -EINVAL;

	info = mt9m114_format_info(sensor, fie->pad, fie->code);
	if (!info || info->code != fie->code)
		return -EINVAL;

	fie->interval.numerator = 1;
	fie->interval.denominator = MT9M114_MAX_FRAME_RATE;

	return 0;
}

/*
 * Helper function to update IFP crop, compose rectangles and source format
 * when the pixel border size changes, which requires resetting these.
 */
static void mt9m114_ifp_update_sel_and_src_fmt(struct v4l2_subdev_state *state)
{
	struct v4l2_mbus_framefmt *src_format, *sink_format;
	struct v4l2_rect *crop;
	unsigned int border;

	sink_format = v4l2_subdev_state_get_format(state, 0);
	src_format = v4l2_subdev_state_get_format(state, 1);
	crop = v4l2_subdev_state_get_crop(state, 0);
	border = mt9m114_ifp_get_border(state);

	crop->left = border;
	crop->top = border;
	crop->width = sink_format->width - 2 * border;
	crop->height = sink_format->height - 2 * border;
	*v4l2_subdev_state_get_compose(state, 0) = *crop;

	src_format->width = crop->width;
	src_format->height = crop->height;

	if (src_format->code == MEDIA_BUS_FMT_SGRBG10_1X10) {
		src_format->colorspace = V4L2_COLORSPACE_RAW;
		src_format->ycbcr_enc = V4L2_YCBCR_ENC_601;
		src_format->quantization = V4L2_QUANTIZATION_FULL_RANGE;
	} else {
		src_format->colorspace = V4L2_COLORSPACE_SRGB;
		src_format->ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
		src_format->quantization = V4L2_QUANTIZATION_DEFAULT;
	}
}

static int mt9m114_ifp_set_fmt(struct v4l2_subdev *sd,
			       struct v4l2_subdev_state *state,
			       struct v4l2_subdev_format *fmt)
{
	struct mt9m114 *sensor = ifp_to_mt9m114(sd);
	struct v4l2_mbus_framefmt *format;
	u32 requested_code = fmt->format.code;

	format = v4l2_subdev_state_get_format(state, fmt->pad);

	if (fmt->pad == 0) {
		/* Only the size can be changed on the sink pad. */
		format->width = clamp(ALIGN(fmt->format.width, 8),
				      MT9M114_PIXEL_ARRAY_MIN_OUTPUT_WIDTH,
				      MT9M114_PIXEL_ARRAY_WIDTH);
		format->height = clamp(ALIGN(fmt->format.height, 8),
				       MT9M114_PIXEL_ARRAY_MIN_OUTPUT_HEIGHT,
				       MT9M114_PIXEL_ARRAY_HEIGHT);

		/* Propagate changes downstream. */
		mt9m114_ifp_update_sel_and_src_fmt(state);

		if (!sensor->ifp.set_fmt_trace_sink_logged) {
			dev_info(&sensor->client->dev,
				 "set_fmt trace: which=%s pad=0 req=%ux%u code=0x%04x -> applied=%ux%u code=0x%04x\n",
				 fmt->which == V4L2_SUBDEV_FORMAT_ACTIVE ? "ACTIVE" : "TRY",
				 fmt->format.width, fmt->format.height, requested_code,
				 format->width, format->height, format->code);
			sensor->ifp.set_fmt_trace_sink_logged = true;
		}
	} else {
		const struct mt9m114_format_info *info;

		/* Only the media bus code can be changed on the source pad. */
		if (mt9m114_ifp_yuv_test_active(sensor))
			requested_code = mt9m114_default_ifp_src_code(sensor);

		info = mt9m114_format_info(sensor, 1, requested_code);

		/*
		 * If the output format changes from/to RAW10 then the crop
		 * rectangle needs to be adjusted to add / remove the 4 pixel
		 * border used for demosaicing. And these changes then need to
		 * be propagated to the compose rectangle and source format.
		 */
		if ((format->code == MEDIA_BUS_FMT_SGRBG10_1X10) !=
		    (info->code == MEDIA_BUS_FMT_SGRBG10_1X10)) {
			format->code = info->code;
			mt9m114_ifp_update_sel_and_src_fmt(state);
		} else {
			format->code = info->code;
		}

		if (mt9m114_ifp_yuv_test_active(sensor) &&
		    fmt->format.code != format->code)
			dev_info_once(&sensor->client->dev,
				      mt9m114_deep_lowlight_strict_ifp_yuv ?
				      "deep-lowlight: strict-yuv overriding requested IFP source code 0x%04x -> 0x%04x\n" :
				      "deep-lowlight: overriding requested IFP source code 0x%04x -> 0x%04x\n",
				      fmt->format.code, format->code);

		if (!sensor->ifp.set_fmt_trace_src_logged) {
			dev_info(&sensor->client->dev,
				 "set_fmt trace: which=%s pad=1 req_code=0x%04x -> applied_code=0x%04x size=%ux%u\n",
				 fmt->which == V4L2_SUBDEV_FORMAT_ACTIVE ? "ACTIVE" : "TRY",
				 fmt->format.code, format->code,
				 format->width, format->height);
			sensor->ifp.set_fmt_trace_src_logged = true;
		}
	}

	fmt->format = *format;

	return 0;
}

static int mt9m114_ifp_get_selection(struct v4l2_subdev *sd,
				     struct v4l2_subdev_state *state,
				     struct v4l2_subdev_selection *sel)
{
	const struct v4l2_mbus_framefmt *format;
	const struct v4l2_rect *crop;
	unsigned int border;
	int ret = 0;

	/* Crop and compose are only supported on the sink pad. */
	if (sel->pad != 0)
		return -EINVAL;

	switch (sel->target) {
	case V4L2_SEL_TGT_CROP:
		sel->r = *v4l2_subdev_state_get_crop(state, 0);
		break;

	case V4L2_SEL_TGT_CROP_DEFAULT:
	case V4L2_SEL_TGT_CROP_BOUNDS:
		/*
		 * Crop defaults and bounds are equal to the sink format size.
		 * For source pad formats other then RAW10 this gets reduced
		 * by 4 pixels on each side for demosaicing.
		 */
		format = v4l2_subdev_state_get_format(state, 0);
		border = mt9m114_ifp_get_border(state);

		sel->r.left = border;
		sel->r.top = border;
		sel->r.width = format->width - 2 * border;
		sel->r.height = format->height - 2 * border;
		break;

	case V4L2_SEL_TGT_COMPOSE:
		sel->r = *v4l2_subdev_state_get_compose(state, 0);
		break;

	case V4L2_SEL_TGT_COMPOSE_DEFAULT:
	case V4L2_SEL_TGT_COMPOSE_BOUNDS:
		/*
		 * The compose default and bounds sizes are equal to the sink
		 * crop rectangle size.
		 */
		crop = v4l2_subdev_state_get_crop(state, 0);
		sel->r.left = 0;
		sel->r.top = 0;
		sel->r.width = crop->width;
		sel->r.height = crop->height;
		break;

	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}

static int mt9m114_ifp_set_selection(struct v4l2_subdev *sd,
				     struct v4l2_subdev_state *state,
				     struct v4l2_subdev_selection *sel)
{
	struct v4l2_mbus_framefmt *format, *src_format;
	struct v4l2_rect *crop;
	struct v4l2_rect *compose;
	unsigned int border;

	if (sel->target != V4L2_SEL_TGT_CROP &&
	    sel->target != V4L2_SEL_TGT_COMPOSE)
		return -EINVAL;

	/* Crop and compose are only supported on the sink pad. */
	if (sel->pad != 0)
		return -EINVAL;

	crop = v4l2_subdev_state_get_crop(state, 0);

	/* Crop and compose cannot be changed when bypassing the scaler. */
	src_format = v4l2_subdev_state_get_format(state, 1);
	if (src_format->code == MEDIA_BUS_FMT_SGRBG10_1X10) {
		sel->r = *crop;
		return 0;
	}

	format = v4l2_subdev_state_get_format(state, 0);
	compose = v4l2_subdev_state_get_compose(state, 0);

	if (sel->target == V4L2_SEL_TGT_CROP) {
		/*
		 * Clamp the crop rectangle. For source pad formats other then
		 * RAW10 demosaicing removes 4 pixels on each side of the image.
		 */
		border = mt9m114_ifp_get_border(state);

		crop->left = clamp_t(unsigned int, ALIGN(sel->r.left, 2), border,
				     format->width - border -
				     MT9M114_SCALER_CROPPED_INPUT_WIDTH);
		crop->top = clamp_t(unsigned int, ALIGN(sel->r.top, 2), border,
				    format->height - border -
				    MT9M114_SCALER_CROPPED_INPUT_HEIGHT);
		crop->width = clamp_t(unsigned int, ALIGN(sel->r.width, 2),
				      MT9M114_SCALER_CROPPED_INPUT_WIDTH,
				      format->width - border - crop->left);
		crop->height = clamp_t(unsigned int, ALIGN(sel->r.height, 2),
				       MT9M114_SCALER_CROPPED_INPUT_HEIGHT,
				       format->height - border - crop->top);

		sel->r = *crop;

		/* Propagate to the compose rectangle. */
		compose->width = crop->width;
		compose->height = crop->height;
	} else {
		/*
		 * Clamp the compose rectangle. The scaler can only downscale.
		 */
		compose->left = 0;
		compose->top = 0;
		compose->width = clamp_t(unsigned int, ALIGN(sel->r.width, 2),
					 MT9M114_SCALER_CROPPED_INPUT_WIDTH,
					 crop->width);
		compose->height = clamp_t(unsigned int, ALIGN(sel->r.height, 2),
					  MT9M114_SCALER_CROPPED_INPUT_HEIGHT,
					  crop->height);

		sel->r = *compose;
	}

	/* Propagate the compose rectangle to the source format. */
	src_format->width = compose->width;
	src_format->height = compose->height;

	return 0;
}

static void mt9m114_ifp_unregistered(struct v4l2_subdev *sd)
{
	struct mt9m114 *sensor = ifp_to_mt9m114(sd);
	struct device *dev = &sensor->client->dev;

	dev_dbg(dev, "ifp unregistered callback (ifp.v4l2_dev=%p, pa.v4l2_dev=%p)\n",
		sensor->ifp.sd.v4l2_dev, sensor->pa.sd.v4l2_dev);

	v4l2_device_unregister_subdev(&sensor->pa.sd);
	complete(&sensor->ifp.unregistered);
}

static int mt9m114_ifp_registered(struct v4l2_subdev *sd)
{
	struct mt9m114 *sensor = ifp_to_mt9m114(sd);
	int ret;

	ret = v4l2_device_register_subdev(sd->v4l2_dev, &sensor->pa.sd);
	if (ret < 0) {
		dev_err(&sensor->client->dev,
			"Failed to register pixel array subdev\n");
		return ret;
	}

	ret = media_create_pad_link(&sensor->pa.sd.entity, 0,
				    &sensor->ifp.sd.entity, 0,
				    MEDIA_LNK_FL_ENABLED |
				    MEDIA_LNK_FL_IMMUTABLE);
	if (ret < 0) {
		dev_err(&sensor->client->dev,
			"Failed to link pixel array to ifp\n");
		v4l2_device_unregister_subdev(&sensor->pa.sd);
		return ret;
	}

	return 0;
}

static const struct v4l2_subdev_video_ops mt9m114_ifp_video_ops = {
	.s_stream = mt9m114_ifp_s_stream,
};

static const struct v4l2_subdev_pad_ops mt9m114_ifp_pad_ops = {
	.enum_mbus_code = mt9m114_ifp_enum_mbus_code,
	.enum_frame_size = mt9m114_ifp_enum_framesizes,
	.enum_frame_interval = mt9m114_ifp_enum_frameintervals,
	.get_fmt = v4l2_subdev_get_fmt,
	.set_fmt = mt9m114_ifp_set_fmt,
	.get_selection = mt9m114_ifp_get_selection,
	.set_selection = mt9m114_ifp_set_selection,
	.get_frame_interval = mt9m114_ifp_get_frame_interval,
	.set_frame_interval = mt9m114_ifp_set_frame_interval,
};

static const struct v4l2_subdev_ops mt9m114_ifp_ops = {
	.video = &mt9m114_ifp_video_ops,
	.pad = &mt9m114_ifp_pad_ops,
};

static const struct v4l2_subdev_internal_ops mt9m114_ifp_internal_ops = {
	.init_state = mt9m114_ifp_init_state,
	.registered = mt9m114_ifp_registered,
	.unregistered = mt9m114_ifp_unregistered,
};

static int mt9m114_ifp_init(struct mt9m114 *sensor)
{
	struct v4l2_subdev *sd = &sensor->ifp.sd;
	struct media_pad *pads = sensor->ifp.pads;
	struct v4l2_ctrl_handler *hdl = &sensor->ifp.hdl;
	struct v4l2_ctrl *link_freq;
	int ret;

	/* Initialize the subdev. */
	v4l2_i2c_subdev_init(sd, sensor->client, &mt9m114_ifp_ops);
	v4l2_i2c_subdev_set_name(sd, sensor->client, NULL, " ifp");

	sd->flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	sd->internal_ops = &mt9m114_ifp_internal_ops;

	/* Initialize the media entity. */
	sd->entity.function = MEDIA_ENT_F_PROC_VIDEO_ISP;
	sd->entity.ops = &mt9m114_entity_ops;
	pads[0].flags = MEDIA_PAD_FL_SINK;
	pads[1].flags = MEDIA_PAD_FL_SOURCE;
	ret = media_entity_pads_init(&sd->entity, 2, pads);
	if (ret < 0)
		return ret;

	sensor->ifp.frame_rate = MT9M114_DEF_FRAME_RATE;
	sensor->ifp.ae_auto = true;
	sensor->ifp.smart_metering_active_preset = MT9M114_METERING_PRESET_CENTER;
	INIT_DELAYED_WORK(&sensor->ifp.smart_meter_work,
			  mt9m114_smart_metering_work);
	INIT_DELAYED_WORK(&sensor->ifp.deep_lowlight_work,
			  mt9m114_deep_lowlight_work);

	/* Initialize the control handler. */
	v4l2_ctrl_handler_init(hdl, 9);
	v4l2_ctrl_new_std(hdl, &mt9m114_ifp_ctrl_ops,
			  V4L2_CID_AUTO_WHITE_BALANCE,
			  0, 1, 1, 1);
	v4l2_ctrl_new_std_menu(hdl, &mt9m114_ifp_ctrl_ops,
			       V4L2_CID_EXPOSURE_AUTO,
			       V4L2_EXPOSURE_MANUAL, 0,
			       V4L2_EXPOSURE_AUTO);
	v4l2_ctrl_new_std(hdl, &mt9m114_ifp_ctrl_ops,
			  V4L2_CID_ILLUMINATORS_1,
			  0, 1, 1, 0);

	if (sensor->bus_cfg.nr_of_link_frequencies) {
		link_freq = v4l2_ctrl_new_int_menu(hdl, &mt9m114_ifp_ctrl_ops,
						   V4L2_CID_LINK_FREQ,
						   sensor->bus_cfg.nr_of_link_frequencies - 1,
						   0,
						   sensor->bus_cfg.link_frequencies);
		if (link_freq)
			link_freq->flags |= V4L2_CTRL_FLAG_READ_ONLY;
	}

	if (sensor->pa.vblank)
		sensor->pa.vblank->flags |= V4L2_CTRL_FLAG_VOLATILE;

	{
		struct v4l2_ctrl *pixel_rate;

		pixel_rate = v4l2_ctrl_new_std(hdl, &mt9m114_ifp_ctrl_ops,
					 V4L2_CID_PIXEL_RATE,
					 sensor->pixrate, sensor->pixrate, 1,
					 sensor->pixrate);
		if (pixel_rate)
			pixel_rate->flags |= V4L2_CTRL_FLAG_READ_ONLY;
	}

	sensor->ifp.tpg[MT9M114_TPG_PATTERN] =
		v4l2_ctrl_new_std_menu_items(hdl, &mt9m114_ifp_ctrl_ops,
					     V4L2_CID_TEST_PATTERN,
					     ARRAY_SIZE(mt9m114_test_pattern_menu) - 1,
					     0, 0, mt9m114_test_pattern_menu);
	sensor->ifp.tpg[MT9M114_TPG_RED] =
		v4l2_ctrl_new_std(hdl, &mt9m114_ifp_ctrl_ops,
				  V4L2_CID_TEST_PATTERN_RED,
				  0, 1023, 1, 1023);
	sensor->ifp.tpg[MT9M114_TPG_GREEN] =
		v4l2_ctrl_new_std(hdl, &mt9m114_ifp_ctrl_ops,
				  V4L2_CID_TEST_PATTERN_GREENR,
				  0, 1023, 1, 1023);
	sensor->ifp.tpg[MT9M114_TPG_BLUE] =
		v4l2_ctrl_new_std(hdl, &mt9m114_ifp_ctrl_ops,
				  V4L2_CID_TEST_PATTERN_BLUE,
				  0, 1023, 1, 1023);

	v4l2_ctrl_cluster(ARRAY_SIZE(sensor->ifp.tpg), sensor->ifp.tpg);

	if (hdl->error) {
		ret = hdl->error;
		goto error;
	}

	sd->ctrl_handler = hdl;
	sd->state_lock = hdl->lock;

	ret = v4l2_subdev_init_finalize(sd);
	if (ret)
		goto error;

	return 0;

error:
	v4l2_ctrl_handler_free(&sensor->ifp.hdl);
	media_entity_cleanup(&sensor->ifp.sd.entity);
	return ret;
}

static void mt9m114_ifp_cleanup(struct mt9m114 *sensor)
{
	cancel_delayed_work_sync(&sensor->ifp.smart_meter_work);
	cancel_delayed_work_sync(&sensor->ifp.deep_lowlight_work);
	v4l2_ctrl_handler_free(&sensor->ifp.hdl);
	media_entity_cleanup(&sensor->ifp.sd.entity);
}

/* -----------------------------------------------------------------------------
 * Power Management
 */

static int mt9m114_power_on(struct mt9m114 *sensor)
{
	int ret;

	/* Enable power and clocks. */
	ret = regulator_bulk_enable(ARRAY_SIZE(sensor->supplies),
				    sensor->supplies);
	if (ret < 0)
		return ret;

	ret = clk_prepare_enable(sensor->clk);
	if (ret < 0)
		goto error_regulator;

	/* Perform a hard reset if available, or a soft reset otherwise. */
	if (sensor->reset) {
		long freq = clk_get_rate(sensor->clk);
		unsigned int duration;

		/*
		 * The minimum duration is 50 clock cycles, thus typically
		 * around 2µs. Double it to be safe.
		 */
		duration = DIV_ROUND_UP(2 * 50 * 1000000, freq);

		gpiod_set_value(sensor->reset, 1);
		fsleep(duration);
		gpiod_set_value(sensor->reset, 0);
	} else {
		/*
		 * The power may have just been turned on, we need to wait for
		 * the sensor to be ready to accept I2C commands.
		 */
		usleep_range(44500, 50000);

		cci_write(sensor->regmap, MT9M114_RESET_AND_MISC_CONTROL,
			  MT9M114_RESET_SOC, &ret);
		cci_write(sensor->regmap, MT9M114_RESET_AND_MISC_CONTROL, 0,
			  &ret);

		if (ret < 0) {
			dev_err(&sensor->client->dev, "Soft reset failed\n");
			goto error_clock;
		}
	}

	/*
	 * Wait for the sensor to be ready to accept I2C commands by polling the
	 * command register to wait for initialization to complete.
	 */
	usleep_range(44500, 50000);

	ret = mt9m114_poll_command(sensor, MT9M114_COMMAND_REGISTER_SET_STATE);
	if (ret < 0)
		goto error_clock;

	if (sensor->bus_cfg.bus_type == V4L2_MBUS_PARALLEL) {
		/*
		 * In parallel mode (OE set to low), the sensor will enter the
		 * streaming state after initialization. Enter the standby
		 * manually to stop streaming.
		 */
		ret = mt9m114_set_state(sensor,
					MT9M114_SYS_STATE_ENTER_STANDBY);
		if (ret < 0)
			goto error_clock;
	}

	/*
	 * Before issuing any Set-State command, we must ensure that the sensor
	 * reaches the standby mode (either initiated manually above in
	 * parallel mode, or automatically after reset in MIPI mode).
	 */
	if (sensor->info->state_standby_polling) {
		ret = mt9m114_poll_state(sensor, MT9M114_SYS_STATE_STANDBY);
		if (ret < 0)
			goto error_clock;
	}

	return 0;

error_clock:
	clk_disable_unprepare(sensor->clk);
error_regulator:
	regulator_bulk_disable(ARRAY_SIZE(sensor->supplies), sensor->supplies);
	return ret;
}

static void mt9m114_power_off(struct mt9m114 *sensor)
{
	unsigned int duration;

	gpiod_set_value(sensor->reset, 1);
	/* Power off takes 10 clock cycles. Double it to be safe. */
	duration = DIV_ROUND_UP(2 * 10 * 1000000, clk_get_rate(sensor->clk));
	fsleep(duration);

	clk_disable_unprepare(sensor->clk);
	regulator_bulk_disable(ARRAY_SIZE(sensor->supplies), sensor->supplies);
}

static int __maybe_unused mt9m114_runtime_resume(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct mt9m114 *sensor = ifp_to_mt9m114(sd);

	return mt9m114_power_on(sensor);
}

static int __maybe_unused mt9m114_runtime_suspend(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct mt9m114 *sensor = ifp_to_mt9m114(sd);

	mt9m114_power_off(sensor);

	return 0;
}

static const struct dev_pm_ops mt9m114_pm_ops = {
	SET_RUNTIME_PM_OPS(mt9m114_runtime_suspend, mt9m114_runtime_resume, NULL)
};

/* -----------------------------------------------------------------------------
 * Probe & Remove
 */

static int mt9m114_verify_link_frequency(struct mt9m114 *sensor,
					 unsigned int pixrate)
{
	u32 i;
	unsigned int link_freq = sensor->bus_cfg.bus_type == V4L2_MBUS_CSI2_DPHY
			       ? pixrate * 8 : pixrate * 2;

	if (!sensor->bus_cfg.nr_of_link_frequencies)
		return -EINVAL;

	for (i = 0; i < sensor->bus_cfg.nr_of_link_frequencies; i++) {
		if (sensor->bus_cfg.link_frequencies[i] == link_freq)
			return 0;
	}

	return -EINVAL;
}

/*
 * Based on the docs the PLL is believed to have the following setup:
 *
 *         +-----+     +-----+     +-----+     +-----+     +-----+
 * Fin --> | / N | --> | x M | --> | x 2 | --> | / P | --> | / 2 | -->
 *         +-----+     +-----+     +-----+     +-----+     +-----+
 *                                         fBit       fWord       fSensor
 * ext_clock    int_clock   out_clock                             pix_clock
 *
 * The MT9M114 docs give a max fBit rate of 768 MHz which translates to
 * an out_clock_max of 384 MHz.
 */
static int mt9m114_clk_init(struct mt9m114 *sensor)
{
	static const struct aptina_pll_limits limits = {
		.ext_clock_min = 6000000,
		.ext_clock_max = 54000000,
		/* int_clock_* limits are not documented taken from mt9p031.c */
		.int_clock_min = 2000000,
		.int_clock_max = 13500000,
		/* out_clock_min is not documented, taken from mt9p031.c */
		.out_clock_min = 180000000,
		.out_clock_max = 384000000,
		.pix_clock_max = 48000000,
		.n_min = 1,
		.n_max = 64,
		.m_min = 16,
		.m_max = 192,
		.p1_min = 8,
		.p1_max = 8,
	};
	unsigned int pixrate;
	int ret;

	if (!sensor->bus_cfg.nr_of_link_frequencies) {
		/*
		 * ACPI fallback path: no reliable endpoint link frequency available.
		 * Use the default PLL target instead of EXTCLK bypass to avoid
		 * under-clocking the sensor and getting blank/timeout streams.
		 */
		sensor->pll.ext_clock = clk_get_rate(sensor->clk);
		sensor->pll.pix_clock = MT9M114_DEF_PIXCLOCK;

		ret = aptina_pll_calculate(&sensor->client->dev, &limits,
					  &sensor->pll);
		if (ret)
			return ret;

		sensor->pixrate = sensor->pll.ext_clock * sensor->pll.m
			/ (sensor->pll.n * sensor->pll.p1);
		sensor->bypass_pll = false;

		dev_warn(&sensor->client->dev,
			 "no link-frequencies provided, using default PLL clocking\n");
		return 0;
	}

	/*
	 * Calculate the pixel rate and link frequency. The CSI-2 bus is clocked
	 * for 16-bit per pixel, transmitted in DDR over a single lane. For
	 * parallel mode, the sensor ouputs one pixel in two PIXCLK cycles.
	 */

	/*
	 * Check if EXTCLK fits the configured link frequency. Bypass the PLL
	 * in this case.
	 */
	pixrate = clk_get_rate(sensor->clk) / 2;
	if (mt9m114_verify_link_frequency(sensor, pixrate) == 0) {
		sensor->pixrate = pixrate;
		sensor->bypass_pll = true;
		return 0;
	}

	/* Check if the PLL configuration fits the configured link frequency. */
	sensor->pll.ext_clock = clk_get_rate(sensor->clk);
	sensor->pll.pix_clock = MT9M114_DEF_PIXCLOCK;

	ret = aptina_pll_calculate(&sensor->client->dev, &limits, &sensor->pll);
	if (ret)
		return ret;

	pixrate = sensor->pll.ext_clock * sensor->pll.m
		/ (sensor->pll.n * sensor->pll.p1);
	if (mt9m114_verify_link_frequency(sensor, pixrate) == 0) {
		sensor->pixrate = pixrate;
		sensor->bypass_pll = false;
		return 0;
	}

	dev_err(&sensor->client->dev, "Unsupported DT link-frequencies\n");
	return -EINVAL;
}

static int mt9m114_identify(struct mt9m114 *sensor)
{
	u64 major, minor, release, customer;
	u64 value;
	int ret;

	ret = cci_read(sensor->regmap, MT9M114_CHIP_ID, &value, NULL);
	if (ret) {
		dev_err(&sensor->client->dev, "Failed to read chip ID\n");
		return -ENXIO;
	}

	if (value != 0x2481) {
		dev_err(&sensor->client->dev, "Invalid chip ID 0x%04llx\n",
			value);
		return -ENXIO;
	}

	cci_read(sensor->regmap, MT9M114_MON_MAJOR_VERSION, &major, &ret);
	cci_read(sensor->regmap, MT9M114_MON_MINOR_VERSION, &minor, &ret);
	cci_read(sensor->regmap, MT9M114_MON_RELEASE_VERSION, &release, &ret);
	cci_read(sensor->regmap, MT9M114_CUSTOMER_REV, &customer, &ret);
	if (ret) {
		dev_err(&sensor->client->dev, "Failed to read version\n");
		return -ENXIO;
	}

	dev_dbg(&sensor->client->dev,
		"monitor v%llu.%llu.%04llx customer rev 0x%04llx\n",
		major, minor, release, customer);

	return 0;
}

static int mt9m114_parse_dt(struct mt9m114 *sensor)
{
	struct fwnode_handle *fwnode;
	struct fwnode_handle *ep;
	int ret;

#if IS_ENABLED(CONFIG_ACPI)
	if (has_acpi_companion(&sensor->client->dev)) {
		/*
		 * On some reload sequences a stale software-node graph can be
		 * observed for this ACPI-enumerated sensor. Use the known safe
		 * default bus configuration and skip endpoint graph parsing.
		 */
		memset(&sensor->bus_cfg, 0, sizeof(sensor->bus_cfg));
		sensor->bus_cfg.bus_type = V4L2_MBUS_CSI2_DPHY;
		sensor->bus_cfg.bus.mipi_csi2.num_data_lanes = 1;
		goto read_slew_rate;
	}
#endif
	fwnode = dev_fwnode(&sensor->client->dev);

	/*
	 * On ACPI systems the fwnode graph can be initialized by a bridge
	 * driver, which may not have probed yet. Wait for this.
	 *
	 * TODO: Return an error once bridge driver code will have moved
	 * to the ACPI core.
	 */
	ep = fwnode_graph_get_next_endpoint(fwnode, NULL);
	if (IS_ERR(ep))
		return dev_err_probe(&sensor->client->dev, PTR_ERR(ep),
				     "failed to get fwnode graph endpoint\n");
	if (!ep)
		return dev_err_probe(&sensor->client->dev, -EPROBE_DEFER,
				     "waiting for fwnode graph endpoint\n");

	sensor->bus_cfg.bus_type = V4L2_MBUS_UNKNOWN;
	ret = v4l2_fwnode_endpoint_alloc_parse(ep, &sensor->bus_cfg);
	fwnode_handle_put(ep);
	if (ret < 0) {
		dev_err(&sensor->client->dev, "Failed to parse endpoint\n");
		goto error;
	}

	switch (sensor->bus_cfg.bus_type) {
	case V4L2_MBUS_CSI2_DPHY:
	case V4L2_MBUS_PARALLEL:
		break;

	default:
		dev_err(&sensor->client->dev, "unsupported bus type %u\n",
			sensor->bus_cfg.bus_type);
		ret = -EINVAL;
		goto error;
	}

read_slew_rate:
	sensor->pad_slew_rate = MT9M114_PAD_SLEW_DEFAULT;
	device_property_read_u32(&sensor->client->dev, "slew-rate",
				 &sensor->pad_slew_rate);

	if (sensor->pad_slew_rate < MT9M114_PAD_SLEW_MIN ||
	    sensor->pad_slew_rate > MT9M114_PAD_SLEW_MAX) {
		dev_err(&sensor->client->dev, "Invalid slew-rate %u\n",
			sensor->pad_slew_rate);
		return -EINVAL;
	}

	return 0;

error:
	v4l2_fwnode_endpoint_free(&sensor->bus_cfg);
	return ret;
}

static int mt9m114_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct mt9m114 *sensor;
	int ret;

	sensor = devm_kzalloc(dev, sizeof(*sensor), GFP_KERNEL);
	if (!sensor)
		return -ENOMEM;

	sensor->client = client;

	sensor->regmap = devm_cci_regmap_init_i2c(client, 16);
	if (IS_ERR(sensor->regmap)) {
		dev_err(dev, "Unable to initialize I2C\n");
		return -ENODEV;
	}

	ret = mt9m114_parse_dt(sensor);
	if (ret < 0)
		return ret;

	sensor->info = device_get_match_data(dev);
	if (!sensor->info)
		return -ENODEV;

	/* Acquire clocks, GPIOs and regulators. */
	sensor->clk = devm_v4l2_sensor_clk_get(dev, NULL);
	if (IS_ERR(sensor->clk)) {
		ret = dev_err_probe(dev, PTR_ERR(sensor->clk),
				    "Failed to get clock\n");
		goto error_ep_free;
	}

	sensor->reset = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(sensor->reset)) {
		ret = PTR_ERR(sensor->reset);
		dev_err_probe(dev, ret, "Failed to get reset GPIO\n");
		goto error_ep_free;
	}

	sensor->supplies[0].supply = "vddio";
	sensor->supplies[1].supply = "vdd";
	sensor->supplies[2].supply = "vaa";

	ret = devm_regulator_bulk_get(dev, ARRAY_SIZE(sensor->supplies),
				      sensor->supplies);
	if (ret < 0) {
		dev_err_probe(dev, ret, "Failed to get regulators\n");
		goto error_ep_free;
	}

	ret = mt9m114_clk_init(sensor);
	if (ret)
		goto error_ep_free;

	/*
	 * Identify the sensor. The driver supports runtime PM, but needs to
	 * work when runtime PM is disabled in the kernel. To that end, power
	 * the sensor on manually here to reach the same state as if resumed
	 * through runtime PM.
	 */
	ret = mt9m114_power_on(sensor);
	if (ret < 0) {
		dev_err_probe(dev, ret, "Could not power on the device\n");
		goto error_ep_free;
	}

	ret = mt9m114_identify(sensor);
	if (ret < 0)
		goto error_power_off;

	/*
	 * Enable runtime PM with autosuspend. As the device has been powered
	 * manually, mark it as active, and increase the usage count without
	 * resuming the device.
	 */
	pm_runtime_set_active(dev);
	pm_runtime_get_noresume(dev);
	pm_runtime_enable(dev);
	pm_runtime_set_autosuspend_delay(dev, 1000);
	pm_runtime_use_autosuspend(dev);

	/* Initialize the subdevices. */
	ret = mt9m114_pa_init(sensor);
	if (ret < 0)
		goto error_pm_cleanup;

	ret = mt9m114_ifp_init(sensor);
	if (ret < 0)
		goto error_pa_cleanup;

	ret = v4l2_async_register_subdev(&sensor->ifp.sd);
	if (ret < 0)
		goto error_ifp_cleanup;

	/*
	 * Decrease the PM usage count. The device will get suspended after the
	 * autosuspend delay, turning the power off.
	 */
	pm_runtime_put_autosuspend(dev);

	return 0;

error_ifp_cleanup:
	mt9m114_ifp_cleanup(sensor);
error_pa_cleanup:
	mt9m114_pa_cleanup(sensor);
error_pm_cleanup:
	pm_runtime_disable(dev);
	pm_runtime_put_noidle(dev);
error_power_off:
	mt9m114_power_off(sensor);
error_ep_free:
	v4l2_fwnode_endpoint_free(&sensor->bus_cfg);
	return ret;
}

static void mt9m114_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct mt9m114 *sensor = ifp_to_mt9m114(sd);
	struct device *dev = &client->dev;
	bool ifp_async_registered = sensor->ifp.sd.async_list.next;
	bool ifp_bound = sensor->ifp.sd.v4l2_dev;

	dev_dbg(dev,
		"remove start (ifp_bound=%u ifp_async_registered=%u ifp.v4l2_dev=%p pa.v4l2_dev=%p)\n",
		ifp_bound, ifp_async_registered,
		sensor->ifp.sd.v4l2_dev, sensor->pa.sd.v4l2_dev);

	if (ifp_async_registered) {
		reinit_completion(&sensor->ifp.unregistered);
		v4l2_async_unregister_subdev(&sensor->ifp.sd);
		if (ifp_bound)
			wait_for_completion(&sensor->ifp.unregistered);
	} else {
		dev_warn(dev, "ifp async subdev already unregistered, skipping\n");
	}

	mt9m114_ifp_cleanup(sensor);
	mt9m114_pa_cleanup(sensor);
	v4l2_fwnode_endpoint_free(&sensor->bus_cfg);

	/*
	 * Disable runtime PM. In case runtime PM is disabled in the kernel,
	 * make sure to turn power off manually.
	 */
	pm_runtime_disable(dev);
	if (!pm_runtime_status_suspended(dev))
		mt9m114_power_off(sensor);
	pm_runtime_set_suspended(dev);
}

static const struct mt9m114_model_info mt9m114_models_default = {
	.state_standby_polling = true,
};

static const struct mt9m114_model_info mt9m114_models_aptina = {
	.state_standby_polling = false,
};

static void mt9m114_shutdown(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct mt9m114 *sensor;

	if (!sd)
		return;

	sensor = ifp_to_mt9m114(sd);

	if (sensor->streaming)
		mt9m114_stop_streaming(sensor);

	pm_runtime_disable(&client->dev);
	if (!pm_runtime_status_suspended(&client->dev))
		mt9m114_power_off(sensor);
	pm_runtime_set_suspended(&client->dev);
}

static const struct of_device_id mt9m114_of_ids[] = {
	{ .compatible = "onnn,mt9m114", .data = &mt9m114_models_default },
	{ .compatible = "aptina,mi1040", .data = &mt9m114_models_aptina },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, mt9m114_of_ids);

static const struct acpi_device_id mt9m114_acpi_ids[] = {
	{ "INT33F0", (kernel_ulong_t)&mt9m114_models_default },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(acpi, mt9m114_acpi_ids);

static struct i2c_driver mt9m114_driver = {
	.driver = {
		.name	= "mt9m114",
		.pm	= &mt9m114_pm_ops,
		.of_match_table = mt9m114_of_ids,
		.acpi_match_table = mt9m114_acpi_ids,
	},
	.probe		= mt9m114_probe,
	.remove		= mt9m114_remove,
	.shutdown	= mt9m114_shutdown,
};

module_i2c_driver(mt9m114_driver);

MODULE_DESCRIPTION("onsemi MT9M114 Sensor Driver");
MODULE_AUTHOR("Laurent Pinchart <laurent.pinchart@ideasonboard.com>");
MODULE_LICENSE("GPL");
