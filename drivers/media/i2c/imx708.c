// SPDX-License-Identifier: GPL-2.0
/*
 * A V4L2 driver for Sony IMX708 cameras.
 * Copyright (C) 2022, Raspberry Pi Ltd
 *
 * Based on Sony imx708 camera driver
 * Copyright (C) 2020 Raspberry Pi Ltd
 */
#include <asm/unaligned.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-event.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-mediabus.h>
#include <linux/rk-camera-module.h>
#include <linux/version.h>

#define DRIVER_VERSION			KERNEL_VERSION(0, 0x01, 0x03)
#define IMX708_NAME			"imx708"
#define OF_CAMERA_HDR_MODE		"rockchip,camera-hdr-mode"
#define IMX708_LANES  2

static int debug = 0;
module_param(debug, int, 0644);

#define IMX708_REG_VALUE_08BIT		1
#define IMX708_REG_VALUE_16BIT		2

/* Chip ID */
#define IMX708_REG_CHIP_ID		0x0016
#define IMX708_CHIP_ID			0x0708

#define IMX708_REG_MODE_SELECT		0x0100
#define IMX708_MODE_STANDBY		0x00
#define IMX708_MODE_STREAMING		0x01

#define IMX708_REG_ORIENTATION		0x101

#define IMX708_INCLK_FREQ		24000000

/* Default initial pixel rate, will get updated for each mode. */
#define IMX708_INITIAL_PIXEL_RATE	590000000

/* V_TIMING internal */
#define IMX708_REG_FRAME_LENGTH		0x0340
#define IMX708_FRAME_LENGTH_MAX		0xffff

/* Long exposure multiplier */
#define IMX708_LONG_EXP_SHIFT_MAX	7
#define IMX708_LONG_EXP_SHIFT_REG	0x3100

/* Exposure control */
#define IMX708_REG_EXPOSURE		0x0202
#define IMX708_EXPOSURE_OFFSET		48
#define IMX708_EXPOSURE_DEFAULT		0x640
#define IMX708_EXPOSURE_STEP		1
#define IMX708_EXPOSURE_MIN		1
#define IMX708_EXPOSURE_MAX		(IMX708_FRAME_LENGTH_MAX - \
					 IMX708_EXPOSURE_OFFSET)

/* Analog gain control */
#define IMX708_REG_ANALOG_GAIN		0x0204
#define IMX708_ANA_GAIN_MIN		112
#define IMX708_ANA_GAIN_MAX		960
#define IMX708_ANA_GAIN_STEP		1
#define IMX708_ANA_GAIN_DEFAULT	   IMX708_ANA_GAIN_MIN

/* Digital gain control */
#define IMX708_REG_DIGITAL_GAIN		0x020e
#define IMX708_DGTL_GAIN_MIN		0x0100
#define IMX708_DGTL_GAIN_MAX		0xffff
#define IMX708_DGTL_GAIN_DEFAULT	0x0100
#define IMX708_DGTL_GAIN_STEP		1

/* Colour balance controls */
#define IMX708_REG_COLOUR_BALANCE_RED   0x0b90
#define IMX708_REG_COLOUR_BALANCE_BLUE	0x0b92
#define IMX708_COLOUR_BALANCE_MIN	0x01
#define IMX708_COLOUR_BALANCE_MAX	0xffff
#define IMX708_COLOUR_BALANCE_STEP	0x01
#define IMX708_COLOUR_BALANCE_DEFAULT	0x100

/* Test Pattern Control */
#define IMX708_REG_TEST_PATTERN		0x0600
#define IMX708_TEST_PATTERN_DISABLE	0
#define IMX708_TEST_PATTERN_SOLID_COLOR	1
#define IMX708_TEST_PATTERN_COLOR_BARS	2
#define IMX708_TEST_PATTERN_GREY_COLOR	3
#define IMX708_TEST_PATTERN_PN9		4

/* Test pattern colour components */
#define IMX708_REG_TEST_PATTERN_R	0x0602
#define IMX708_REG_TEST_PATTERN_GR	0x0604
#define IMX708_REG_TEST_PATTERN_B	0x0606
#define IMX708_REG_TEST_PATTERN_GB	0x0608
#define IMX708_TEST_PATTERN_COLOUR_MIN	0
#define IMX708_TEST_PATTERN_COLOUR_MAX	0x0fff
#define IMX708_TEST_PATTERN_COLOUR_STEP	1

#define IMX708_REG_BASE_SPC_GAINS_L	0x7b10
#define IMX708_REG_BASE_SPC_GAINS_R	0x7c00

/* HDR exposure ratio (long:med == med:short) */
#define IMX708_HDR_EXPOSURE_RATIO       4
#define IMX708_REG_MID_EXPOSURE	0x3116
#define IMX708_REG_SHT_EXPOSURE	0x0224
#define IMX708_REG_MID_ANALOG_GAIN	0x3118
#define IMX708_REG_SHT_ANALOG_GAIN	0x0216

/*
 * Metadata buffer holds a variety of data, all sent with the same VC/DT (0x12).
 * It comprises two scanlines (of up to 5760 bytes each, for 4608 pixels)
 * of embedded data, one line of PDAF data, and two lines of AE-HIST data
 * (AE histograms are valid for HDR mode and empty in non-HDR modes).
 */
#define IMX708_EMBEDDED_LINE_WIDTH (5 * 5760)
#define IMX708_NUM_EMBEDDED_LINES 1

/* IMX708 native and active pixel array size. */
#define IMX708_NATIVE_WIDTH			4640U
#define IMX708_NATIVE_HEIGHT		2658U
#define IMX708_PIXEL_ARRAY_LEFT		16U
#define IMX708_PIXEL_ARRAY_TOP		24U
#define IMX708_PIXEL_ARRAY_WIDTH	4608U
#define IMX708_PIXEL_ARRAY_HEIGHT	2592U

struct imx708_reg {
	u16 address;
	u8 val;
};

struct imx708_reg_list {
	unsigned int num_of_regs;
	const struct imx708_reg *regs;
};

/* Mode : resolution and related config&values */
struct imx708_mode {
	u32 bus_fmt;

	/* Frame width */
	unsigned int width;

	/* Frame height */
	unsigned int height;

	struct v4l2_fract max_fps;

	/* H-timing in pixels */
	unsigned int line_length_pix;

	/* Analog crop rectangle. */
	struct v4l2_rect crop;

	/* Highest possible framerate. */
	unsigned int vblank_min;

	/* Default framerate. */
	unsigned int vblank_default;

	/* Default register values */
	struct imx708_reg_list reg_list;

	/* Not all modes have the same pixel rate. */
	u64 pixel_rate;

	/* Not all modes have the same minimum exposure. */
	u32 exposure_lines_min;

	/* Not all modes have the same exposure lines step. */
	u32 exposure_lines_step;

	u32 hdr_mode;
};

/* Default PDAF pixel correction gains */
static const u8 pdaf_gains[2][9] = {
	{ 0x4c, 0x4c, 0x4c, 0x46, 0x3e, 0x38, 0x35, 0x35, 0x35 },
	{ 0x35, 0x35, 0x35, 0x38, 0x3e, 0x46, 0x4c, 0x4c, 0x4c }
};

/* Link frequency setup */
enum {
	IMX708_LINK_FREQ_450MHZ,
	IMX708_LINK_FREQ_447MHZ,
	IMX708_LINK_FREQ_453MHZ,
};

static const s64 link_freqs[] = {
	[IMX708_LINK_FREQ_450MHZ] = 450000000,
	[IMX708_LINK_FREQ_447MHZ] = 447000000,
	[IMX708_LINK_FREQ_453MHZ] = 453000000,
};

/* 450MHz is the nominal "default" link frequency */
static const struct imx708_reg link_450Mhz_regs[] = {
	{0x030E, 0x01},
	{0x030F, 0x2c},
};

static const struct imx708_reg link_447Mhz_regs[] = {
	{0x030E, 0x01},
	{0x030F, 0x2a},
};

static const struct imx708_reg link_453Mhz_regs[] = {
	{0x030E, 0x01},
	{0x030F, 0x2e},
};

static const struct imx708_reg_list link_freq_regs[] = {
	[IMX708_LINK_FREQ_450MHZ] = {
		.regs = link_450Mhz_regs,
		.num_of_regs = ARRAY_SIZE(link_450Mhz_regs)
	},
	[IMX708_LINK_FREQ_447MHZ] = {
		.regs = link_447Mhz_regs,
		.num_of_regs = ARRAY_SIZE(link_447Mhz_regs)
	},
	[IMX708_LINK_FREQ_453MHZ] = {
		.regs = link_453Mhz_regs,
		.num_of_regs = ARRAY_SIZE(link_453Mhz_regs)
	},
};

static const struct imx708_reg mode_common_regs[] = {
	{0x0100, 0x00},
	{0x0136, 0x18},
	{0x0137, 0x00},
	{0x33F0, 0x02},
	{0x33F1, 0x05},
	{0x3062, 0x00},
	{0x3063, 0x12},
	{0x3068, 0x00},
	{0x3069, 0x12},
	{0x306A, 0x00},
	{0x306B, 0x30},
	{0x3076, 0x00},
	{0x3077, 0x30},
	{0x3078, 0x00},
	{0x3079, 0x30},
	{0x5E54, 0x0C},
	{0x6E44, 0x00},
	{0xB0B6, 0x01},
	{0xE829, 0x00},
	{0xF001, 0x08},
	{0xF003, 0x08},
	{0xF00D, 0x10},
	{0xF00F, 0x10},
	{0xF031, 0x08},
	{0xF033, 0x08},
	{0xF03D, 0x10},
	{0xF03F, 0x10},
	{0x0112, 0x0A},
	{0x0113, 0x0A},
	{0x0114, 0x01},
	{0x0B8E, 0x01},
	{0x0B8F, 0x00},
	{0x0B94, 0x01},
	{0x0B95, 0x00},
	{0x3400, 0x01},
	{0x3478, 0x01},
	{0x3479, 0x1c},
	{0x3091, 0x01},
	{0x3092, 0x00},
	{0x3419, 0x00},
	{0xBCF1, 0x02},
	{0x3094, 0x01},
	{0x3095, 0x01},
	{0x3362, 0x00},
	{0x3363, 0x00},
	{0x3364, 0x00},
	{0x3365, 0x00},
	{0x0138, 0x01},
};

/* 10-bit. */
static const struct imx708_reg mode_4608x2592_regs[] = {
	{0x0342, 0x3D},
	{0x0343, 0x20},
	{0x0340, 0x0A},
	{0x0341, 0x59},
	{0x0344, 0x00},
	{0x0345, 0x00},
	{0x0346, 0x00},
	{0x0347, 0x00},
	{0x0348, 0x11},
	{0x0349, 0xFF},
	{0x034A, 0X0A},
	{0x034B, 0x1F},
	{0x0220, 0x62},
	{0x0222, 0x01},
	{0x0900, 0x00},
	{0x0901, 0x11},
	{0x0902, 0x0A},
	{0x3200, 0x01},
	{0x3201, 0x01},
	{0x32D5, 0x01},
	{0x32D6, 0x00},
	{0x32DB, 0x01},
	{0x32DF, 0x00},
	{0x350C, 0x00},
	{0x350D, 0x00},
	{0x0408, 0x00},
	{0x0409, 0x00},
	{0x040A, 0x00},
	{0x040B, 0x00},
	{0x040C, 0x12},
	{0x040D, 0x00},
	{0x040E, 0x0A},
	{0x040F, 0x20},
	{0x034C, 0x12},
	{0x034D, 0x00},
	{0x034E, 0x0A},
	{0x034F, 0x20},
	{0x0301, 0x05},
	{0x0303, 0x02},
	{0x0305, 0x02},
	{0x0306, 0x00},
	{0x0307, 0x7C},
	{0x030B, 0x02},
	{0x030D, 0x04},
	{0x0310, 0x01},
	{0x3CA0, 0x00},
	{0x3CA1, 0x64},
	{0x3CA4, 0x00},
	{0x3CA5, 0x00},
	{0x3CA6, 0x00},
	{0x3CA7, 0x00},
	{0x3CAA, 0x00},
	{0x3CAB, 0x00},
	{0x3CB8, 0x00},
	{0x3CB9, 0x08},
	{0x3CBA, 0x00},
	{0x3CBB, 0x00},
	{0x3CBC, 0x00},
	{0x3CBD, 0x3C},
	{0x3CBE, 0x00},
	{0x3CBF, 0x00},
	{0x0202, 0x0A},
	{0x0203, 0x29},
	{0x0224, 0x01},
	{0x0225, 0xF4},
	{0x3116, 0x01},
	{0x3117, 0xF4},
	{0x0204, 0x00},
	{0x0205, 0x00},
	{0x0216, 0x00},
	{0x0217, 0x00},
	{0x0218, 0x01},
	{0x0219, 0x00},
	{0x020E, 0x01},
	{0x020F, 0x00},
	{0x3118, 0x00},
	{0x3119, 0x00},
	{0x311A, 0x01},
	{0x311B, 0x00},
	{0x341a, 0x00},
	{0x341b, 0x00},
	{0x341c, 0x00},
	{0x341d, 0x00},
	{0x341e, 0x01},
	{0x341f, 0x20},
	{0x3420, 0x00},
	{0x3421, 0xd8},
	{0xC428, 0x00},
	{0xC429, 0x04},
	{0x3366, 0x00},
	{0x3367, 0x00},
	{0x3368, 0x00},
	{0x3369, 0x00},
};

static const struct imx708_reg mode_2x2binned_regs[] = {
	{0x0342, 0x1E},
	{0x0343, 0x90},
	{0x0340, 0x05},
	{0x0341, 0x38},
	{0x0344, 0x00},
	{0x0345, 0x00},
	{0x0346, 0x00},
	{0x0347, 0x00},
	{0x0348, 0x11},
	{0x0349, 0xFF},
	{0x034A, 0X0A},
	{0x034B, 0x1F},
	{0x0220, 0x62},
	{0x0222, 0x01},
	{0x0900, 0x01},
	{0x0901, 0x22},
	{0x0902, 0x08},
	{0x3200, 0x41},
	{0x3201, 0x41},
	{0x32D5, 0x00},
	{0x32D6, 0x00},
	{0x32DB, 0x01},
	{0x32DF, 0x00},
	{0x350C, 0x00},
	{0x350D, 0x00},
	{0x0408, 0x00},
	{0x0409, 0x00},
	{0x040A, 0x00},
	{0x040B, 0x00},
	{0x040C, 0x09},
	{0x040D, 0x00},
	{0x040E, 0x05},
	{0x040F, 0x10},
	{0x034C, 0x09},
	{0x034D, 0x00},
	{0x034E, 0x05},
	{0x034F, 0x10},
	{0x0301, 0x05},
	{0x0303, 0x02},
	{0x0305, 0x02},
	{0x0306, 0x00},
	{0x0307, 0x7A},
	{0x030B, 0x02},
	{0x030D, 0x04},
	{0x0310, 0x01},
	{0x3CA0, 0x00},
	{0x3CA1, 0x3C},
	{0x3CA4, 0x00},
	{0x3CA5, 0x3C},
	{0x3CA6, 0x00},
	{0x3CA7, 0x00},
	{0x3CAA, 0x00},
	{0x3CAB, 0x00},
	{0x3CB8, 0x00},
	{0x3CB9, 0x1C},
	{0x3CBA, 0x00},
	{0x3CBB, 0x08},
	{0x3CBC, 0x00},
	{0x3CBD, 0x1E},
	{0x3CBE, 0x00},
	{0x3CBF, 0x0A},
	{0x0202, 0x05},
	{0x0203, 0x08},
	{0x0224, 0x01},
	{0x0225, 0xF4},
	{0x3116, 0x01},
	{0x3117, 0xF4},
	{0x0204, 0x00},
	{0x0205, 0x70},
	{0x0216, 0x00},
	{0x0217, 0x70},
	{0x0218, 0x01},
	{0x0219, 0x00},
	{0x020E, 0x01},
	{0x020F, 0x00},
	{0x3118, 0x00},
	{0x3119, 0x70},
	{0x311A, 0x01},
	{0x311B, 0x00},
	{0x341a, 0x00},
	{0x341b, 0x00},
	{0x341c, 0x00},
	{0x341d, 0x00},
	{0x341e, 0x00},
	{0x341f, 0x90},
	{0x3420, 0x00},
	{0x3421, 0x6c},
	{0x3366, 0x07},
	{0x3367, 0x80},
	{0x3368, 0x04},
	{0x3369, 0x38},
};

static const struct imx708_reg mode_2x2binned_720p_regs[] = {
	{0x0342, 0x14},
	{0x0343, 0x60},
	{0x0340, 0x04},
	{0x0341, 0xB6},
	{0x0344, 0x03},
	{0x0345, 0x00},
	{0x0346, 0x01},
	{0x0347, 0xB0},
	{0x0348, 0x0E},
	{0x0349, 0xFF},
	{0x034A, 0x08},
	{0x034B, 0x6F},
	{0x0220, 0x62},
	{0x0222, 0x01},
	{0x0900, 0x01},
	{0x0901, 0x22},
	{0x0902, 0x08},
	{0x3200, 0x41},
	{0x3201, 0x41},
	{0x32D5, 0x00},
	{0x32D6, 0x00},
	{0x32DB, 0x01},
	{0x32DF, 0x01},
	{0x350C, 0x00},
	{0x350D, 0x00},
	{0x0408, 0x00},
	{0x0409, 0x00},
	{0x040A, 0x00},
	{0x040B, 0x00},
	{0x040C, 0x06},
	{0x040D, 0x00},
	{0x040E, 0x03},
	{0x040F, 0x60},
	{0x034C, 0x06},
	{0x034D, 0x00},
	{0x034E, 0x03},
	{0x034F, 0x60},
	{0x0301, 0x05},
	{0x0303, 0x02},
	{0x0305, 0x02},
	{0x0306, 0x00},
	{0x0307, 0x76},
	{0x030B, 0x02},
	{0x030D, 0x04},
	{0x0310, 0x01},
	{0x3CA0, 0x00},
	{0x3CA1, 0x3C},
	{0x3CA4, 0x01},
	{0x3CA5, 0x5E},
	{0x3CA6, 0x00},
	{0x3CA7, 0x00},
	{0x3CAA, 0x00},
	{0x3CAB, 0x00},
	{0x3CB8, 0x00},
	{0x3CB9, 0x0C},
	{0x3CBA, 0x00},
	{0x3CBB, 0x04},
	{0x3CBC, 0x00},
	{0x3CBD, 0x1E},
	{0x3CBE, 0x00},
	{0x3CBF, 0x05},
	{0x0202, 0x04},
	{0x0203, 0x86},
	{0x0224, 0x01},
	{0x0225, 0xF4},
	{0x3116, 0x01},
	{0x3117, 0xF4},
	{0x0204, 0x00},
	{0x0205, 0x70},
	{0x0216, 0x00},
	{0x0217, 0x70},
	{0x0218, 0x01},
	{0x0219, 0x00},
	{0x020E, 0x01},
	{0x020F, 0x00},
	{0x3118, 0x00},
	{0x3119, 0x70},
	{0x311A, 0x01},
	{0x311B, 0x00},
	{0x341a, 0x00},
	{0x341b, 0x00},
	{0x341c, 0x00},
	{0x341d, 0x00},
	{0x341e, 0x00},
	{0x341f, 0x60},
	{0x3420, 0x00},
	{0x3421, 0x48},
	{0x3366, 0x00},
	{0x3367, 0x00},
	{0x3368, 0x00},
	{0x3369, 0x00},
};

static const struct imx708_reg mode_hdr_regs[] = {
	{0x0342, 0x14},
	{0x0343, 0x60},
	{0x0340, 0x0A},
	{0x0341, 0x5B},
	{0x0344, 0x00},
	{0x0345, 0x00},
	{0x0346, 0x00},
	{0x0347, 0x00},
	{0x0348, 0x11},
	{0x0349, 0xFF},
	{0x034A, 0X0A},
	{0x034B, 0x1F},
	{0x0220, 0x01},
	{0x0222, IMX708_HDR_EXPOSURE_RATIO},
	{0x0900, 0x00},
	{0x0901, 0x11},
	{0x0902, 0x0A},
	{0x3200, 0x01},
	{0x3201, 0x01},
	{0x32D5, 0x00},
	{0x32D6, 0x00},
	{0x32DB, 0x01},
	{0x32DF, 0x00},
	{0x350C, 0x00},
	{0x350D, 0x00},
	{0x0408, 0x00},
	{0x0409, 0x00},
	{0x040A, 0x00},
	{0x040B, 0x00},
	{0x040C, 0x09},
	{0x040D, 0x00},
	{0x040E, 0x05},
	{0x040F, 0x10},
	{0x034C, 0x09},
	{0x034D, 0x00},
	{0x034E, 0x05},
	{0x034F, 0x10},
	{0x0301, 0x05},
	{0x0303, 0x02},
	{0x0305, 0x02},
	{0x0306, 0x00},
	{0x0307, 0xA2},
	{0x030B, 0x02},
	{0x030D, 0x04},
	{0x0310, 0x01},
	{0x3CA0, 0x00},
	{0x3CA1, 0x00},
	{0x3CA4, 0x00},
	{0x3CA5, 0x00},
	{0x3CA6, 0x00},
	{0x3CA7, 0x28},
	{0x3CAA, 0x00},
	{0x3CAB, 0x00},
	{0x3CB8, 0x00},
	{0x3CB9, 0x30},
	{0x3CBA, 0x00},
	{0x3CBB, 0x00},
	{0x3CBC, 0x00},
	{0x3CBD, 0x32},
	{0x3CBE, 0x00},
	{0x3CBF, 0x00},
	{0x0202, 0x0A},
	{0x0203, 0x2B},
	{0x0224, 0x0A},
	{0x0225, 0x2B},
	{0x3116, 0x0A},
	{0x3117, 0x2B},
	{0x0204, 0x00},
	{0x0205, 0x00},
	{0x0216, 0x00},
	{0x0217, 0x00},
	{0x0218, 0x01},
	{0x0219, 0x00},
	{0x020E, 0x01},
	{0x020F, 0x00},
	{0x3118, 0x00},
	{0x3119, 0x00},
	{0x311A, 0x01},
	{0x311B, 0x00},
	{0x341a, 0x00},
	{0x341b, 0x00},
	{0x341c, 0x00},
	{0x341d, 0x00},
	{0x341e, 0x00},
	{0x341f, 0x90},
	{0x3420, 0x00},
	{0x3421, 0x6c},
	{0x3360, 0x01},
	{0x3361, 0x01},
	{0x3366, 0x07},
	{0x3367, 0x80},
	{0x3368, 0x04},
	{0x3369, 0x38},
};

/* Mode configs. Keep separate lists for when HDR is enabled or not. */
static const struct imx708_mode supported_modes[] = {
	{
		.bus_fmt = MEDIA_BUS_FMT_SRGGB10_1X10,
		/* Full resolution. */
		.width = 4608,
		.height = 2592,
		.max_fps = {
			.numerator = 10000,
			.denominator = 140000,
		},
		.line_length_pix = 0x3d20,
		.crop = {
			.left = IMX708_PIXEL_ARRAY_LEFT,
			.top = IMX708_PIXEL_ARRAY_TOP,
			.width = 4608,
			.height = 2592,
		},
		.vblank_min = 58,
		.vblank_default = 58,
		.reg_list = {
			.num_of_regs = ARRAY_SIZE(mode_4608x2592_regs),
			.regs = mode_4608x2592_regs,
		},
		.pixel_rate = 595200000,
		.exposure_lines_min = 8,
		.exposure_lines_step = 1,
		.hdr_mode = NO_HDR,
	},
	{
		.bus_fmt = MEDIA_BUS_FMT_SRGGB10_1X10,
		/* regular 2x2 binned. */
		.width = 1920,
		.height = 1080,
		.max_fps = {
			.numerator = 10000,
			.denominator = 660000,
		},
		.line_length_pix = 0x1e90,
		.crop = {
			.left = IMX708_PIXEL_ARRAY_LEFT,
			.top = IMX708_PIXEL_ARRAY_TOP,
			.width = 4608,
			.height = 2592,
		},
		.vblank_min = 40,
		.vblank_default = 1198,
		.reg_list = {
			.num_of_regs = ARRAY_SIZE(mode_2x2binned_regs),
			.regs = mode_2x2binned_regs,
		},
		.pixel_rate = 585600000,
		.exposure_lines_min = 4,
		.exposure_lines_step = 2,
		.hdr_mode = NO_HDR,
	},
	{
		.bus_fmt = MEDIA_BUS_FMT_SRGGB10_1X10,
		/* There's only one HDR mode, which is 2x2 downscaled */
		.width = 1920,
		.height = 1080,
		.max_fps = {
			.numerator = 10000,
			.denominator = 310000,
		},
		.line_length_pix = 0x1460,
		.crop = {
			.left = IMX708_PIXEL_ARRAY_LEFT,
			.top = IMX708_PIXEL_ARRAY_TOP,
			.width = 4608,
			.height = 2592,
		},
		.vblank_min = 3673,
		.vblank_default = 3673,
		.reg_list = {
			.num_of_regs = ARRAY_SIZE(mode_hdr_regs),
			.regs = mode_hdr_regs,
		},
		.pixel_rate = 777600000,
		.exposure_lines_min = 8 * IMX708_HDR_EXPOSURE_RATIO * IMX708_HDR_EXPOSURE_RATIO,
		.exposure_lines_step = 2 * IMX708_HDR_EXPOSURE_RATIO * IMX708_HDR_EXPOSURE_RATIO,
		.hdr_mode = HDR_X3,
	},
	{
		.bus_fmt = MEDIA_BUS_FMT_SRGGB10_1X10,
		/* 2x2 binned and cropped for 720p. */
		.width = 1536,
		.height = 864,
		.max_fps = {
			.numerator = 10000,
			.denominator = 1200000,
		},
		.line_length_pix = 0x1460,
		.crop = {
			.left = IMX708_PIXEL_ARRAY_LEFT + 768,
			.top = IMX708_PIXEL_ARRAY_TOP + 432,
			.width = 3072,
			.height = 1728,
		},
		.vblank_min = 40,
		.vblank_default = 2755,
		.reg_list = {
			.num_of_regs = ARRAY_SIZE(mode_2x2binned_720p_regs),
			.regs = mode_2x2binned_720p_regs,
		},
		.pixel_rate = 566400000,
		.exposure_lines_min = 4,
		.exposure_lines_step = 2,
		.hdr_mode = NO_HDR,
	},
};


static const char * const imx708_test_pattern_menu[] = {
	"Disabled",
	"Color Bars",
	"Solid Color",
	"Grey Color Bars",
	"PN9"
};

static const int imx708_test_pattern_val[] = {
	IMX708_TEST_PATTERN_DISABLE,
	IMX708_TEST_PATTERN_COLOR_BARS,
	IMX708_TEST_PATTERN_SOLID_COLOR,
	IMX708_TEST_PATTERN_GREY_COLOR,
	IMX708_TEST_PATTERN_PN9,
};

/* regulator supplies */
static const char * const imx708_supply_name[] = {
	/* Supplies can be enabled in any order */
	"vana1",  /* Analog1 (2.8V) supply */
	"vana2",  /* Analog2 (1.8V) supply */
	"vdig",  /* Digital Core (1.1V) supply */
	"vddl",  /* IF (1.8V) supply */
};

/*
 * Initialisation delay between XCLR low->high and the moment when the sensor
 * can start capture (i.e. can leave software standby), given by T7 in the
 * datasheet is 8ms.  This does include I2C setup time as well.
 *
 * Note, that delay between XCLR low->high and reading the CCI ID register (T6
 * in the datasheet) is much smaller - 600us.
 */
#define IMX708_XCLR_MIN_DELAY_US	8000
#define IMX708_XCLR_DELAY_RANGE_US	1000

struct imx708 {
	struct i2c_client	*client;
	struct clk *inclk;
	struct gpio_desc *reset_gpio;

	struct regulator_bulk_data supplies[ARRAY_SIZE(imx708_supply_name)];

	struct v4l2_mbus_framefmt fmt;

	u32 inclk_freq;

	struct v4l2_subdev subdev;
	struct media_pad pad;
	struct v4l2_ctrl_handler ctrl_handler;
	/* V4L2 Controls */
	struct v4l2_ctrl *pixel_rate;
	struct v4l2_ctrl *exposure;
	struct v4l2_ctrl *vblank;
	struct v4l2_ctrl *hblank;
	struct v4l2_ctrl *link_freq;
	struct {
		struct v4l2_ctrl *hflip;
		struct v4l2_ctrl *vflip;
	};

	/*
	 * Mutex for serialized access:
	 * Protect sensor module set pad format and start/stop streaming safely.
	 */
	struct mutex mutex;

	/* Streaming on/off */
	bool streaming;
	bool power_on;

	/* Current mode */
	const struct imx708_mode *cur_mode;
	/*module*/
	u32 		module_index;
	u32			cfg_num;
	const char *module_facing;
	const char *module_name;
	const char *len_name;

	/* Rewrite common registers on stream on? */
	bool common_regs_written;

	/* Current long exposure factor in use. Set through V4L2_CID_VBLANK */
	unsigned int long_exp_shift;

	unsigned int link_freq_idx;
};

static inline struct imx708 *to_imx708(struct v4l2_subdev *_sd)
{
	return container_of(_sd, struct imx708, subdev);
}

/* Read registers up to 2 at a time */
static int imx708_read_reg(struct imx708 *imx708, u16 reg, u32 len, u32 *val)
{
	struct i2c_client *client = v4l2_get_subdevdata(&imx708->subdev);
	struct i2c_msg msgs[2];
	u8 addr_buf[2] = { reg >> 8, reg & 0xff };
	u8 data_buf[4] = { 0, };
	int ret;

	if (len > 4)
		return -EINVAL;

	/* Write register address */
	msgs[0].addr = client->addr;
	msgs[0].flags = 0;
	msgs[0].len = ARRAY_SIZE(addr_buf);
	msgs[0].buf = addr_buf;

	/* Read data from register */
	msgs[1].addr = client->addr;
	msgs[1].flags = I2C_M_RD;
	msgs[1].len = len;
	msgs[1].buf = &data_buf[4 - len];

	ret = i2c_transfer(client->adapter, msgs, ARRAY_SIZE(msgs));
	if (ret != ARRAY_SIZE(msgs))
		return -EIO;

	*val = get_unaligned_be32(data_buf);

	return 0;
}

/* Write registers up to 2 at a time */
static int imx708_write_reg(struct imx708 *imx708, u16 reg, u32 len, u32 val)
{
	struct i2c_client *client = v4l2_get_subdevdata(&imx708->subdev);
	u8 buf[6];

	if (len > 4)
		return -EINVAL;

	put_unaligned_be16(reg, buf);
	put_unaligned_be32(val << (8 * (4 - len)), buf + 2);
	if (i2c_master_send(client, buf, len + 2) != len + 2)
		return -EIO;

	return 0;
}

/* Write a list of registers */
static int imx708_write_regs(struct imx708 *imx708,
			     const struct imx708_reg *regs, u32 len)
{
	struct i2c_client *client = v4l2_get_subdevdata(&imx708->subdev);
	unsigned int i;

	for (i = 0; i < len; i++) {
		int ret;

		ret = imx708_write_reg(imx708, regs[i].address, 1, regs[i].val);
		if (ret) {
			dev_err_ratelimited(&client->dev,
					    "Failed to write reg 0x%4.4x. error = %d\n",
					    regs[i].address, ret);

			return ret;
		}
	}

	return 0;
}

#ifdef CONFIG_VIDEO_V4L2_SUBDEV_API
static int imx708_open(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh)
{
	struct imx708 *imx708 = to_imx708(sd);
	struct v4l2_mbus_framefmt *try_fmt_img =
		v4l2_subdev_get_try_format(sd, fh->state, 0);

	mutex_lock(&imx708->mutex);

	try_fmt_img->width = supported_modes[0].width;
	try_fmt_img->height = supported_modes[0].height;
	try_fmt_img->code =  supported_modes[0].bus_fmt;
	try_fmt_img->field = V4L2_FIELD_NONE;

	mutex_unlock(&imx708->mutex);

	return 0;
}
#endif

static int imx708_set_exposure(struct imx708 *imx708, unsigned int val)
{
	val = max(val, imx708->cur_mode->exposure_lines_min);
	val -= val % imx708->cur_mode->exposure_lines_step;

	/*
	 * In HDR mode this will set the longest exposure. The sensor
	 * will automatically divide the medium and short ones by 4,16.
	 */
	return imx708_write_reg(imx708, IMX708_REG_EXPOSURE,
				IMX708_REG_VALUE_16BIT,
				val >> imx708->long_exp_shift);
}

static void imx708_adjust_exposure_range(struct imx708 *imx708,
					 struct v4l2_ctrl *ctrl)
{
	int exposure_max, exposure_def;

	/* Honour the VBLANK limits when setting exposure. */
	exposure_max = imx708->cur_mode->height + imx708->vblank->val -
		IMX708_EXPOSURE_OFFSET;
	exposure_def = min(exposure_max, imx708->exposure->val);
	__v4l2_ctrl_modify_range(imx708->exposure, imx708->exposure->minimum,
				 exposure_max, imx708->exposure->step,
				 exposure_def);
}

static int imx708_set_analogue_gain(struct imx708 *imx708, unsigned int val)
{
	int ret;

	/*
	 * In HDR mode this will set the gain for the longest exposure,
	 * and by default the sensor uses the same gain for all of them.
	 */
	ret = imx708_write_reg(imx708, IMX708_REG_ANALOG_GAIN,
			       IMX708_REG_VALUE_16BIT, val);

	return ret;
}

static int imx708_set_frame_length(struct imx708 *imx708, unsigned int val)
{
	int ret;

	imx708->long_exp_shift = 0;

	while (val > IMX708_FRAME_LENGTH_MAX) {
		imx708->long_exp_shift++;
		val >>= 1;
	}

	ret = imx708_write_reg(imx708, IMX708_REG_FRAME_LENGTH,
			       IMX708_REG_VALUE_16BIT, val);
	if (ret)
		return ret;

	return imx708_write_reg(imx708, IMX708_LONG_EXP_SHIFT_REG,
				IMX708_REG_VALUE_08BIT, imx708->long_exp_shift);
}

static void imx708_set_framing_limits(struct imx708 *imx708)
{
	const struct imx708_mode *mode = imx708->cur_mode;
	unsigned int hblank;

	__v4l2_ctrl_modify_range(imx708->pixel_rate,
				 mode->pixel_rate, mode->pixel_rate,
				 1, mode->pixel_rate);

	/* Update limits and set FPS to default */
	__v4l2_ctrl_modify_range(imx708->vblank, mode->vblank_min,
				 ((1 << IMX708_LONG_EXP_SHIFT_MAX) *
					IMX708_FRAME_LENGTH_MAX) - mode->height,
				 1, mode->vblank_default);

	/*
	 * Currently PPL is fixed to the mode specified value, so hblank
	 * depends on mode->width only, and is not changeable in any
	 * way other than changing the mode.
	 */
	hblank = mode->line_length_pix - mode->width;
	__v4l2_ctrl_modify_range(imx708->hblank, hblank, hblank, 1, hblank);
}

static int imx708_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct imx708 *imx708 =
		container_of(ctrl->handler, struct imx708, ctrl_handler);
	struct i2c_client *client = imx708->client;
	int ret = 0;

	switch (ctrl->id) {
	case V4L2_CID_VBLANK:
		/*
		 * The VBLANK control may change the limits of usable exposure,
		 * so check and adjust if necessary.
		 */
		imx708_adjust_exposure_range(imx708, ctrl);
		break;
	}

	/*
	 * Applying V4L2 control value only happens
	 * when power is up for streaming
	 */
	if (pm_runtime_get_if_in_use(&client->dev) == 0)
		return 0;

	switch (ctrl->id) {
	case V4L2_CID_ANALOGUE_GAIN:
		imx708_set_analogue_gain(imx708, ctrl->val);
		break;
	case V4L2_CID_EXPOSURE:
		ret = imx708_set_exposure(imx708, ctrl->val);
		break;
	case V4L2_CID_DIGITAL_GAIN:
		ret = imx708_write_reg(imx708, IMX708_REG_DIGITAL_GAIN,
				       IMX708_REG_VALUE_16BIT, ctrl->val);
		break;
	case V4L2_CID_TEST_PATTERN:
		ret = imx708_write_reg(imx708, IMX708_REG_TEST_PATTERN,
				       IMX708_REG_VALUE_16BIT,
				       imx708_test_pattern_val[ctrl->val]);
		break;
	case V4L2_CID_TEST_PATTERN_RED:
		ret = imx708_write_reg(imx708, IMX708_REG_TEST_PATTERN_R,
				       IMX708_REG_VALUE_16BIT, ctrl->val);
		break;
	case V4L2_CID_TEST_PATTERN_GREENR:
		ret = imx708_write_reg(imx708, IMX708_REG_TEST_PATTERN_GR,
				       IMX708_REG_VALUE_16BIT, ctrl->val);
		break;
	case V4L2_CID_TEST_PATTERN_BLUE:
		ret = imx708_write_reg(imx708, IMX708_REG_TEST_PATTERN_B,
				       IMX708_REG_VALUE_16BIT, ctrl->val);
		break;
	case V4L2_CID_TEST_PATTERN_GREENB:
		ret = imx708_write_reg(imx708, IMX708_REG_TEST_PATTERN_GB,
				       IMX708_REG_VALUE_16BIT, ctrl->val);
		break;
	case V4L2_CID_HFLIP:
	case V4L2_CID_VFLIP:
		ret = imx708_write_reg(imx708, IMX708_REG_ORIENTATION, 1,
				       imx708->hflip->val |
				       imx708->vflip->val << 1);
		break;
	case V4L2_CID_VBLANK:
		ret = imx708_set_frame_length(imx708,
					      imx708->cur_mode->height + ctrl->val);
		break;
	default:
		dev_info(&client->dev,
			 "ctrl(id:0x%x,val:0x%x) is not handled\n",
			 ctrl->id, ctrl->val);
		ret = -EINVAL;
		break;
	}

	pm_runtime_put(&client->dev);

	return ret;
}

static const struct v4l2_ctrl_ops imx708_ctrl_ops = {
	.s_ctrl = imx708_set_ctrl,
};

static int imx708_g_frame_interval(struct v4l2_subdev *sd,
				   struct v4l2_subdev_frame_interval *fi)
{
	struct imx708 *imx708 = to_imx708(sd);
	const struct imx708_mode *mode = imx708->cur_mode;

	mutex_lock(&imx708->mutex);
	fi->interval = mode->max_fps;
	mutex_unlock(&imx708->mutex);

	return 0;
}

static int imx708_enum_mbus_code(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *sd_state,
				 struct v4l2_subdev_mbus_code_enum *code)
{
	struct imx708 *imx708 = to_imx708(sd);
		if (code->index >= imx708->cfg_num)
			return -EINVAL;

	code->code = supported_modes[code->index].bus_fmt;

	return 0;
}

static int imx708_enum_frame_size(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *sd_state,
				  struct v4l2_subdev_frame_size_enum *fse)
{
	struct imx708 *imx708 = to_imx708(sd);


		if (fse->index >= imx708->cfg_num)
			return -EINVAL;

		if (fse->code != supported_modes[fse->index].bus_fmt)
			return -EINVAL;

		fse->min_width = supported_modes[fse->index].width;
		fse->max_width = fse->min_width;;
		fse->min_height = supported_modes[fse->index].height;
		fse->max_height = fse->min_height;;

	return 0;
}

static int imx708_get_pad_format(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *sd_state,
				 struct v4l2_subdev_format *fmt)
{
	struct imx708 *imx708 = to_imx708(sd);
	const struct imx708_mode *mode = imx708->cur_mode;

	mutex_lock(&imx708->mutex);

	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY) {
#ifdef CONFIG_VIDEO_V4L2_SUBDEV_API
		fmt->format = *v4l2_subdev_get_try_format(sd, sd_state,
						   fmt->pad);
#else
		mutex_unlock(&imx477->mutex);
		return -ENOTTY;
#endif
	} else {
		fmt->format.width = mode->width;
		fmt->format.height = mode->height;
		fmt->format.code = mode->bus_fmt;
		fmt->format.field = V4L2_FIELD_NONE;
	}

	mutex_unlock(&imx708->mutex);
	return 0;
}

static int imx708_get_reso_dist(const struct imx708_mode *mode,
				struct v4l2_mbus_framefmt *framefmt)
{
	return abs(mode->width - framefmt->width) +
	       abs(mode->height - framefmt->height);
}

static const struct imx708_mode *
imx708_find_best_fit(struct imx708 *imx708, struct v4l2_subdev_format *fmt)
{
	struct v4l2_mbus_framefmt *framefmt = &fmt->format;
	int dist;
	int cur_best_fit = 0;
	int cur_best_fit_dist = -1;
	unsigned int i;

	for (i = 0; i < imx708->cfg_num; i++) {
		dist = imx708_get_reso_dist(&supported_modes[i], framefmt);
		if ((cur_best_fit_dist == -1 || dist < cur_best_fit_dist) &&
			supported_modes[i].bus_fmt == framefmt->code) {
			cur_best_fit_dist = dist;
			cur_best_fit = i;
		}
	}
	dev_info(&imx708->client->dev, "%s: cur_best_fit(%d)",
		 __func__, cur_best_fit);

	return &supported_modes[cur_best_fit];
}

static int imx708_set_pad_format(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *sd_state,
				 struct v4l2_subdev_format *fmt)
{
	const struct imx708_mode *mode;
	struct imx708 *imx708 = to_imx708(sd);

	mutex_lock(&imx708->mutex);
	mode = imx708_find_best_fit(imx708,fmt);
	fmt->format.code = mode->bus_fmt;
	fmt->format.width = mode->width;
	fmt->format.height = mode->height;
	fmt->format.field = V4L2_FIELD_NONE;
	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY) {
#ifdef CONFIG_VIDEO_V4L2_SUBDEV_API
		*v4l2_subdev_get_try_format(sd, sd_state, fmt->pad) = fmt->format;
#else
		mutex_unlock(&imx708->mutex);
		return -ENOTTY;
#endif
	} else {
		imx708->cur_mode = mode;
		imx708_set_framing_limits(imx708);
	}

	mutex_unlock(&imx708->mutex);

	return 0;
}

/* Start streaming */
static int imx708_start_streaming(struct imx708 *imx708)
{
	struct i2c_client *client = v4l2_get_subdevdata(&imx708->subdev);
	const struct imx708_reg_list *reg_list, *freq_regs;
	int i, ret;
	u32 val;

	if (!imx708->common_regs_written) {
		ret = imx708_write_regs(imx708, mode_common_regs,
					ARRAY_SIZE(mode_common_regs));
		if (ret) {
			dev_err(&client->dev, "%s failed to set common settings\n",
				__func__);
			return ret;
		}

		ret = imx708_read_reg(imx708, IMX708_REG_BASE_SPC_GAINS_L,
				      IMX708_REG_VALUE_08BIT, &val);
		if (ret == 0 && val == 0x40) {
			for (i = 0; i < 54 && ret == 0; i++) {
				ret = imx708_write_reg(imx708,
						       IMX708_REG_BASE_SPC_GAINS_L + i,
						       IMX708_REG_VALUE_08BIT,
						       pdaf_gains[0][i % 9]);
			}
			for (i = 0; i < 54 && ret == 0; i++) {
				ret = imx708_write_reg(imx708,
						       IMX708_REG_BASE_SPC_GAINS_R + i,
						       IMX708_REG_VALUE_08BIT,
						       pdaf_gains[1][i % 9]);
			}
		}
		if (ret) {
			dev_err(&client->dev, "%s failed to set PDAF gains\n",
				__func__);
			return ret;
		}

		imx708->common_regs_written = true;
	}

	/* Apply default values of current mode */
	reg_list = &imx708->cur_mode->reg_list;
	ret = imx708_write_regs(imx708, reg_list->regs, reg_list->num_of_regs);
	if (ret) {
		dev_err(&client->dev, "%s failed to set mode\n", __func__);
		return ret;
	}

	/* Update the link frequency registers */
	freq_regs = &link_freq_regs[imx708->link_freq_idx];
	ret = imx708_write_regs(imx708, freq_regs->regs,
				freq_regs->num_of_regs);
	if (ret) {
		dev_err(&client->dev, "%s failed to set link frequency registers\n",
			__func__);
		return ret;
	}

	/* Apply customized values from user */
	ret =  __v4l2_ctrl_handler_setup(imx708->subdev.ctrl_handler);
	if (ret)
		return ret;

	/* set stream on register */
	return imx708_write_reg(imx708, IMX708_REG_MODE_SELECT,
				IMX708_REG_VALUE_08BIT, IMX708_MODE_STREAMING);
}

/* Stop streaming */
static void imx708_stop_streaming(struct imx708 *imx708)
{
	struct i2c_client *client = v4l2_get_subdevdata(&imx708->subdev);
	int ret;

	/* set stream off register */
	ret = imx708_write_reg(imx708, IMX708_REG_MODE_SELECT,
			       IMX708_REG_VALUE_08BIT, IMX708_MODE_STANDBY);
	if (ret)
		dev_err(&client->dev, "%s failed to set stream\n", __func__);
}

static int imx708_set_stream(struct v4l2_subdev *sd, int enable)
{
	struct imx708 *imx708 = to_imx708(sd);
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	int ret = 0;

	mutex_lock(&imx708->mutex);
	if (imx708->streaming == enable) {
		mutex_unlock(&imx708->mutex);
		return 0;
	}

	if (enable) {
		ret = pm_runtime_get_sync(&client->dev);
		if (ret < 0) {
			pm_runtime_put_noidle(&client->dev);
			goto err_unlock;
		}

		/*
		 * Apply default & customized values
		 * and then start streaming.
		 */
		ret = imx708_start_streaming(imx708);
		if (ret)
			goto err_rpm_put;
	} else {
		imx708_stop_streaming(imx708);
		pm_runtime_put(&client->dev);
	}

	imx708->streaming = enable;

	/* vflip/hflip and hdr mode cannot change during streaming */
	__v4l2_ctrl_grab(imx708->vflip, enable);
	__v4l2_ctrl_grab(imx708->hflip, enable);

	mutex_unlock(&imx708->mutex);

	return ret;

err_rpm_put:
	pm_runtime_put(&client->dev);
err_unlock:
	mutex_unlock(&imx708->mutex);

	return ret;
}

/* Power/clock management functions */
static int imx708_power_on(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct imx708 *imx708 = to_imx708(sd);
	int ret;

	ret = regulator_bulk_enable(ARRAY_SIZE(imx708_supply_name),
				    imx708->supplies);
	if (ret) {
		dev_err(&client->dev, "%s: failed to enable regulators\n",
			__func__);
		return ret;
	}

	ret = clk_prepare_enable(imx708->inclk);
	if (ret) {
		dev_err(&client->dev, "%s: failed to enable clock\n",
			__func__);
		goto reg_off;
	}

	gpiod_direction_output(imx708->reset_gpio, 1);
	usleep_range(IMX708_XCLR_MIN_DELAY_US,
		     IMX708_XCLR_MIN_DELAY_US + IMX708_XCLR_DELAY_RANGE_US);

	v4l2_dbg(1, debug, &imx708->subdev,"%s.\n", __func__);


	return 0;

reg_off:
	regulator_bulk_disable(ARRAY_SIZE(imx708_supply_name),
			       imx708->supplies);
	return ret;
}

static int imx708_power_off(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct imx708 *imx708 = to_imx708(sd);

	gpiod_direction_output(imx708->reset_gpio, 0);
	regulator_bulk_disable(ARRAY_SIZE(imx708_supply_name),
			       imx708->supplies);
	clk_disable_unprepare(imx708->inclk);

	/* Force reprogramming of the common registers when powered up again. */
	imx708->common_regs_written = false;

	v4l2_dbg(1, debug, &imx708->subdev,"%s.\n", __func__);

	return 0;
}

static int __maybe_unused imx708_suspend(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct imx708 *imx708 = to_imx708(sd);

	if (imx708->streaming)
		imx708_stop_streaming(imx708);

	return 0;
}

static int __maybe_unused imx708_resume(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct imx708 *imx708 = to_imx708(sd);
	int ret;

	if (imx708->streaming) {
		ret = imx708_start_streaming(imx708);
		if (ret)
			goto error;
	}

	return 0;

error:
	imx708_stop_streaming(imx708);
	imx708->streaming = 0;
	return ret;
}

static int imx708_get_regulators(struct imx708 *imx708)
{
	struct i2c_client *client = v4l2_get_subdevdata(&imx708->subdev);
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(imx708_supply_name); i++)
		imx708->supplies[i].supply = imx708_supply_name[i];

	return devm_regulator_bulk_get(&client->dev,
				       ARRAY_SIZE(imx708_supply_name),
				       imx708->supplies);
}

/* Verify chip ID */
static int imx708_identify_module(struct imx708 *imx708)
{
	struct i2c_client *client = v4l2_get_subdevdata(&imx708->subdev);
	int ret;
	u32 val;

	ret = imx708_read_reg(imx708, IMX708_REG_CHIP_ID,
			      IMX708_REG_VALUE_16BIT, &val);
	if (ret) {
		dev_err(&client->dev, "failed to read chip id %x, with error %d\n",
			IMX708_CHIP_ID, ret);
		return ret;
	}

	if (val != IMX708_CHIP_ID) {
		dev_err(&client->dev, "chip id mismatch: %x!=%x\n",
			IMX708_CHIP_ID, val);
		return -EIO;
	}

	ret = imx708_read_reg(imx708, 0x0000, IMX708_REG_VALUE_16BIT, &val);
	if (!ret) {
		dev_info(&client->dev, "camera module ID 0x%04x\n", val);
		snprintf(imx708->subdev.name, sizeof(imx708->subdev.name), "imx708%s%s",
			 val & 0x02 ? "_wide" : "",
			 val & 0x80 ? "_noir" : "");
	}

	return 0;
}

static int imx708_s_power(struct v4l2_subdev *sd, int on)
{
	struct imx708 *imx708 = to_imx708(sd);
	struct i2c_client *client = imx708->client;
	int ret = 0;

	mutex_lock(&imx708->mutex);

	if (imx708->power_on == !!on)
		goto unlock_and_return;

	if (on) {
		ret = pm_runtime_get_sync(&client->dev);
		if (ret < 0) {
			pm_runtime_put_noidle(&client->dev);
			goto unlock_and_return;
		}
		imx708->power_on = true;
	} else {
		pm_runtime_put(&client->dev);
		imx708->power_on = false;
	}
	v4l2_dbg(1, debug, &imx708->subdev,"%s: %d.\n", __func__, on);
unlock_and_return:
	mutex_unlock(&imx708->mutex);

	return ret;
}


static int imx708_g_mbus_config(struct v4l2_subdev *sd, unsigned int pad_id,
				struct v4l2_mbus_config *config)
{

	config->type = V4L2_MBUS_CSI2_DPHY;
	config->bus.mipi_csi2.num_data_lanes = IMX708_LANES;

	return 0;
}

static void imx708_get_module_inf(struct imx708 *imx708,
				  struct rkmodule_inf *inf)
{
	memset(inf, 0, sizeof(*inf));
	strlcpy(inf->base.sensor, IMX708_NAME, sizeof(inf->base.sensor));
	strlcpy(inf->base.module, imx708->module_name,
		sizeof(inf->base.module));
	strlcpy(inf->base.lens, imx708->len_name, sizeof(inf->base.lens));

	v4l2_dbg(1, debug, &imx708->subdev,"%s: get_module_inf:%s, %s, %s.\n", __func__,
		inf->base.sensor, inf->base.module, inf->base.lens);
}

static long imx708_ioctl(struct v4l2_subdev *sd, unsigned int cmd, void *arg)
{
	struct imx708 *imx708 = to_imx708(sd);
	struct rkmodule_hdr_cfg *hdr;
	u32 i, h, w;
	long ret = 0;
	// struct rkmodule_csi_dphy_param *dphy_param;

	switch (cmd)
	{
	case RKMODULE_GET_MODULE_INFO:
		imx708_get_module_inf(imx708, (struct rkmodule_inf *)arg);
		break;
	// case RKMODULE_GET_HDR_CFG:
	// 		hdr = (struct rkmodule_hdr_cfg *)arg;
	// 		if (imx708->cur_mode->hdr_mode == NO_HDR)
	// 			hdr->esp.mode = HDR_NORMAL_VC;
	// 		else
	// 			hdr->esp.mode = HDR_ID_CODE;
	// 		hdr->hdr_mode = imx708->cur_mode->hdr_mode;
	// 		break;
	case RKMODULE_SET_HDR_CFG:
		hdr = (struct rkmodule_hdr_cfg *)arg;
		w = imx708->cur_mode->width;
		h = imx708->cur_mode->height;
		for (i = 0; i < imx708->cfg_num; i++) {
			if (w == supported_modes[i].width &&
			    h == supported_modes[i].height &&
			    supported_modes[i].hdr_mode == hdr->hdr_mode) {
				imx708->cur_mode = &supported_modes[i];
				break;
			}
		}
		if (i == imx708->cfg_num) {
			dev_err(&imx708->client->dev,
				"not find hdr mode:%d %dx%d config\n",
				hdr->hdr_mode, w, h);
			ret = -EINVAL;
		} else {
			imx708_set_framing_limits(imx708);
		}
		break;
	// case RKMODULE_GET_CSI_DPHY_PARAM:
	// 	if (imx708->cur_mode->hdr_mode == HDR_X2) {
	// 		dphy_param = (struct rkmodule_csi_dphy_param *)arg;
	// 		if (dphy_param->vendor == dcphy_param.vendor)
	// 			*dphy_param = dcphy_param;
	// 		dev_info(&imx708->client->dev,
	// 			 "get sensor dphy param\n");
	// 	} else
	// 		ret = -EINVAL;
	// 	break;
	default:
		ret = -ENOIOCTLCMD;
		break;
	}
	return ret;
}

#ifdef CONFIG_COMPAT
static long imx708_compat_ioctl32(struct v4l2_subdev *sd,
				  unsigned int cmd, unsigned long arg)
{
	void __user *up = compat_ptr(arg);
	struct rkmodule_inf *inf;
	struct rkmodule_awb_cfg *cfg;
	struct rkmodule_hdr_cfg *hdr;
	long ret;
	// struct rkmodule_csi_dphy_param *dphy_param;

	switch (cmd) {
	case RKMODULE_GET_MODULE_INFO:
		inf = kzalloc(sizeof(*inf), GFP_KERNEL);
		if (!inf) {
			ret = -ENOMEM;
			return ret;
		}

		ret = imx708_ioctl(sd, cmd, inf);
		if (!ret) {
			if (copy_to_user(up, inf, sizeof(*inf))) {
				kfree(inf);
				return -EFAULT;
			}
		}
		kfree(inf);
		break;
	case RKMODULE_AWB_CFG:
		cfg = kzalloc(sizeof(*cfg), GFP_KERNEL);
		if (!cfg) {
			ret = -ENOMEM;
			return ret;
		}

		if (copy_from_user(cfg, up, sizeof(*cfg))) {
			kfree(cfg);
			return -EFAULT;
		}
		ret = imx708_ioctl(sd, cmd, cfg);
		kfree(cfg);
		break;
	case RKMODULE_GET_HDR_CFG:
		hdr = kzalloc(sizeof(*hdr), GFP_KERNEL);
		if (!hdr) {
			ret = -ENOMEM;
			return ret;
		}

		ret = imx708_ioctl(sd, cmd, hdr);
		if (!ret) {
			if (copy_to_user(up, hdr, sizeof(*hdr))) {
				kfree(hdr);
				return -EFAULT;
			}
		}
		kfree(hdr);
		break;
	case RKMODULE_SET_HDR_CFG:
		hdr = kzalloc(sizeof(*hdr), GFP_KERNEL);
		if (!hdr) {
			ret = -ENOMEM;
			return ret;
		}

		if (copy_from_user(hdr, up, sizeof(*hdr))) {
			kfree(hdr);
			return -EFAULT;
		}
		ret = imx708_ioctl(sd, cmd, hdr);
		kfree(hdr);
		break;
	// case RKMODULE_GET_CSI_DPHY_PARAM:
	// 	dphy_param = kzalloc(sizeof(*dphy_param), GFP_KERNEL);
	// 	if (!dphy_param) {
	// 		ret = -ENOMEM;
	// 		return ret;
	// 	}

	// 	ret = imx708_ioctl(sd, cmd, dphy_param);
	// 	if (!ret) {
	// 		ret = copy_to_user(up, dphy_param, sizeof(*dphy_param));
	// 		if (ret)
	// 			ret = -EFAULT;
	// 	}
	// 	kfree(dphy_param);
	// 	break;

	default:
		ret = -ENOIOCTLCMD;
		break;
	}
	return ret;

}
#endif

static int imx708_enum_frame_interval(struct v4l2_subdev *sd,
	struct v4l2_subdev_state *sd_state,
	struct v4l2_subdev_frame_interval_enum *fie)
{
	struct imx708 *imx708 = to_imx708(sd);

	if (fie->index >= imx708->cfg_num)
		return -EINVAL;

	fie->code = supported_modes[fie->index].bus_fmt;
	fie->width = supported_modes[fie->index].width;
	fie->height = supported_modes[fie->index].height;
	fie->interval = supported_modes[fie->index].max_fps;

	return 0;
}

static const struct v4l2_subdev_core_ops imx708_core_ops = {
	.s_power = imx708_s_power,
	.ioctl = imx708_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl32 = imx708_compat_ioctl32,
#endif
	.subscribe_event = v4l2_ctrl_subdev_subscribe_event,
	.unsubscribe_event = v4l2_event_subdev_unsubscribe,
};

static const struct v4l2_subdev_video_ops imx708_video_ops = {
	.s_stream = imx708_set_stream,
	.g_frame_interval = imx708_g_frame_interval,
};

static const struct v4l2_subdev_pad_ops imx708_pad_ops = {
	.enum_mbus_code = imx708_enum_mbus_code,
	.enum_frame_size = imx708_enum_frame_size,
	.enum_frame_interval = imx708_enum_frame_interval,
	.get_fmt = imx708_get_pad_format,
	.set_fmt = imx708_set_pad_format,
	// .get_selection = imx708_get_selection,
	.get_mbus_config = imx708_g_mbus_config,

};

static const struct v4l2_subdev_ops imx708_subdev_ops = {
	.core = &imx708_core_ops,
	.video = &imx708_video_ops,
	.pad = &imx708_pad_ops,
};

static const struct v4l2_subdev_internal_ops imx708_internal_ops = {
	.open = imx708_open,
};

/* Initialize control handlers */
static int imx708_init_controls(struct imx708 *imx708)
{
	struct v4l2_ctrl_handler *ctrl_hdlr;
	struct i2c_client *client = v4l2_get_subdevdata(&imx708->subdev);
	struct v4l2_fwnode_device_properties props;
	struct v4l2_ctrl *ctrl;
	unsigned int i;
	int ret;

	ctrl_hdlr = &imx708->ctrl_handler;
	ret = v4l2_ctrl_handler_init(ctrl_hdlr, 16);
	if (ret)
		return ret;

	mutex_init(&imx708->mutex);
	ctrl_hdlr->lock = &imx708->mutex;

	/* By default, PIXEL_RATE is read only */
	imx708->pixel_rate = v4l2_ctrl_new_std(ctrl_hdlr, &imx708_ctrl_ops,
					       V4L2_CID_PIXEL_RATE,
					       IMX708_INITIAL_PIXEL_RATE,
					       IMX708_INITIAL_PIXEL_RATE, 1,
					       IMX708_INITIAL_PIXEL_RATE);

	ctrl = v4l2_ctrl_new_int_menu(ctrl_hdlr, &imx708_ctrl_ops,
				      V4L2_CID_LINK_FREQ, 0, 0,
				      &link_freqs[imx708->link_freq_idx]);
	if (ctrl)
		ctrl->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	/*
	 * Create the controls here, but mode specific limits are setup
	 * in the imx708_set_framing_limits() call below.
	 */
	imx708->vblank = v4l2_ctrl_new_std(ctrl_hdlr, &imx708_ctrl_ops,
					   V4L2_CID_VBLANK, 0, 0xffff, 1, 0);
	imx708->hblank = v4l2_ctrl_new_std(ctrl_hdlr, &imx708_ctrl_ops,
					   V4L2_CID_HBLANK, 0, 0xffff, 1, 0);

	imx708->exposure = v4l2_ctrl_new_std(ctrl_hdlr, &imx708_ctrl_ops,
					     V4L2_CID_EXPOSURE,
					     IMX708_EXPOSURE_MIN,
					     IMX708_EXPOSURE_MAX,
					     IMX708_EXPOSURE_STEP,
					     IMX708_EXPOSURE_DEFAULT);

	v4l2_ctrl_new_std(ctrl_hdlr, &imx708_ctrl_ops, V4L2_CID_ANALOGUE_GAIN,
			  IMX708_ANA_GAIN_MIN, IMX708_ANA_GAIN_MAX,
			  IMX708_ANA_GAIN_STEP, IMX708_ANA_GAIN_DEFAULT);

	v4l2_ctrl_new_std(ctrl_hdlr, &imx708_ctrl_ops, V4L2_CID_DIGITAL_GAIN,
			  IMX708_DGTL_GAIN_MIN, IMX708_DGTL_GAIN_MAX,
			  IMX708_DGTL_GAIN_STEP, IMX708_DGTL_GAIN_DEFAULT);

	imx708->hflip = v4l2_ctrl_new_std(ctrl_hdlr, &imx708_ctrl_ops,
					  V4L2_CID_HFLIP, 0, 1, 1, 0);

	imx708->vflip = v4l2_ctrl_new_std(ctrl_hdlr, &imx708_ctrl_ops,
					  V4L2_CID_VFLIP, 0, 1, 1, 0);
	v4l2_ctrl_cluster(2, &imx708->hflip);

	v4l2_ctrl_new_std_menu_items(ctrl_hdlr, &imx708_ctrl_ops,
				     V4L2_CID_TEST_PATTERN,
				     ARRAY_SIZE(imx708_test_pattern_menu) - 1,
				     0, 0, imx708_test_pattern_menu);
	for (i = 0; i < 4; i++) {
		/*
		 * The assumption is that
		 * V4L2_CID_TEST_PATTERN_GREENR == V4L2_CID_TEST_PATTERN_RED + 1
		 * V4L2_CID_TEST_PATTERN_BLUE   == V4L2_CID_TEST_PATTERN_RED + 2
		 * V4L2_CID_TEST_PATTERN_GREENB == V4L2_CID_TEST_PATTERN_RED + 3
		 */
		v4l2_ctrl_new_std(ctrl_hdlr, &imx708_ctrl_ops,
				  V4L2_CID_TEST_PATTERN_RED + i,
				  IMX708_TEST_PATTERN_COLOUR_MIN,
				  IMX708_TEST_PATTERN_COLOUR_MAX,
				  IMX708_TEST_PATTERN_COLOUR_STEP,
				  IMX708_TEST_PATTERN_COLOUR_MAX);
		/* The "Solid color" pattern is white by default */
	}

	ret = v4l2_fwnode_device_parse(&client->dev, &props);
	if (ret)
		goto error;

	v4l2_ctrl_new_fwnode_properties(ctrl_hdlr, &imx708_ctrl_ops, &props);

	if (ctrl_hdlr->error) {
		ret = ctrl_hdlr->error;
		dev_err(&client->dev, "%s control init failed (%d)\n",
			__func__, ret);
		goto error;
	}

	imx708->hblank->flags |= V4L2_CTRL_FLAG_READ_ONLY;
	imx708->hflip->flags |= V4L2_CTRL_FLAG_MODIFY_LAYOUT;
	imx708->vflip->flags |= V4L2_CTRL_FLAG_MODIFY_LAYOUT;

	imx708->subdev.ctrl_handler = ctrl_hdlr;

	/* Setup exposure and frame/line length limits. */
	imx708_set_framing_limits(imx708);

	return 0;

error:
	v4l2_ctrl_handler_free(ctrl_hdlr);
	mutex_destroy(&imx708->mutex);

	return ret;
}

static int imx708_check_hwcfg(struct device *dev, struct imx708 *imx708)
{
	struct fwnode_handle *endpoint;
	struct v4l2_fwnode_endpoint ep_cfg = {
		.bus_type = V4L2_MBUS_CSI2_DPHY
	};
	int ret = -EINVAL;
	int i;

	endpoint = fwnode_graph_get_next_endpoint(dev_fwnode(dev), NULL);
	if (!endpoint) {
		dev_err(dev, "endpoint node not found\n");
		return -EINVAL;
	}

	if (v4l2_fwnode_endpoint_alloc_parse(endpoint, &ep_cfg)) {
		dev_err(dev, "could not parse endpoint\n");
		goto error_out;
	}

	/* Check the number of MIPI CSI2 data lanes */
	if (ep_cfg.bus.mipi_csi2.num_data_lanes != 2) {
		dev_err(dev, "only 2 data lanes are currently supported\n");
		goto error_out;
	}

	/* Check the link frequency set in device tree */
	if (!ep_cfg.nr_of_link_frequencies) {
		dev_err(dev, "link-frequency property not found in DT\n");
		goto error_out;
	}

	for (i = 0; i < ARRAY_SIZE(link_freqs); i++) {
		if (link_freqs[i] == ep_cfg.link_frequencies[0]) {
			imx708->link_freq_idx = i;
			break;
		}
	}

	if (i == ARRAY_SIZE(link_freqs)) {
		dev_err(dev, "Link frequency not supported: %lld\n",
			ep_cfg.link_frequencies[0]);
			ret = -EINVAL;
			goto error_out;
	}

	ret = 0;

error_out:
	v4l2_fwnode_endpoint_free(&ep_cfg);
	fwnode_handle_put(endpoint);

	return ret;
}

static int imx708_probe(struct i2c_client *client,
			const struct i2c_device_id *id)
{
	struct device *dev = &client->dev;
	struct device_node *node = dev->of_node;
	struct imx708 *imx708;
    struct v4l2_subdev *sd;
	char facing[2];
	int ret;
	u32 i, hdr_mode = 0;

	dev_info(dev, "driver version: %02x.%02x.%02x",
		DRIVER_VERSION >> 16,
		(DRIVER_VERSION & 0xff00) >> 8,
		DRIVER_VERSION & 0x00ff);

	imx708 = devm_kzalloc(&client->dev, sizeof(*imx708), GFP_KERNEL);
	if (!imx708)
		return -ENOMEM;

	ret = of_property_read_u32(node, RKMODULE_CAMERA_MODULE_INDEX,
				   &imx708->module_index);
	ret |= of_property_read_string(node, RKMODULE_CAMERA_MODULE_FACING,
				       &imx708->module_facing);
	ret |= of_property_read_string(node, RKMODULE_CAMERA_MODULE_NAME,
				       &imx708->module_name);
	ret |= of_property_read_string(node, RKMODULE_CAMERA_LENS_NAME,
				       &imx708->len_name);
	if (ret) {
		dev_err(dev, "could not get module information!\n");
		return -EINVAL;
	}

	ret = of_property_read_u32(node, OF_CAMERA_HDR_MODE, &hdr_mode);
	if (ret) {
		hdr_mode = NO_HDR;
		dev_warn(dev, " Get hdr mode failed! no hdr default\n");
	}

	imx708->client = client;
	imx708->cfg_num = ARRAY_SIZE(supported_modes);
	for (i = 0; i < imx708->cfg_num; i++) {
		if (hdr_mode == supported_modes[i].hdr_mode) {
			imx708->cur_mode = &supported_modes[i];
			break;
		}
	}

	if (i >= imx708->cfg_num) {
		dev_warn(dev, " Get hdr mode failed! no hdr config\n");
		imx708->cur_mode = &supported_modes[0];
	}

	sd = &imx708->subdev;
	v4l2_i2c_subdev_init(sd, client, &imx708_subdev_ops);

	/* Check the hardware configuration in device tree */
	if (imx708_check_hwcfg(dev, imx708))
		return -EINVAL;

	/* Get system clock (inclk) */
	imx708->inclk = devm_clk_get(dev, "xclk");
	if (IS_ERR(imx708->inclk))
		return dev_err_probe(dev, PTR_ERR(imx708->inclk),
				     "failed to get xclk\n");

	imx708->inclk_freq = clk_get_rate(imx708->inclk);
	if (imx708->inclk_freq != IMX708_INCLK_FREQ)
		return dev_err_probe(dev, -EINVAL,
				     "inclk frequency not supported: %d Hz\n",
				     imx708->inclk_freq);

	ret = imx708_get_regulators(imx708);
	if (ret)
		return dev_err_probe(dev, ret, "failed to get regulators\n");

	/* Request optional enable pin */
	imx708->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_ASIS);
	if (IS_ERR(imx708->reset_gpio))
		dev_warn(dev, "Failed to get reset-gpios\n");
	/*
	 * The sensor must be powered for imx708_identify_module()
	 * to be able to read the CHIP_ID register
	 */
	ret = imx708_power_on(dev);
	if (ret)
		return ret;

	ret = imx708_identify_module(imx708);
	if (ret)
		goto error_power_off;


	/* Enable runtime PM and turn off the device */
	pm_runtime_set_active(dev);
	pm_runtime_enable(dev);
	pm_runtime_idle(dev);

	/* This needs the pm runtime to be registered. */
	ret = imx708_init_controls(imx708);
	if (ret)
		goto error_pm_runtime;
#ifdef CONFIG_VIDEO_V4L2_SUBDEV_API
	/* Initialize subdev */
	sd->internal_ops = &imx708_internal_ops;
	sd->flags |= V4L2_SUBDEV_FL_HAS_DEVNODE |
			    V4L2_SUBDEV_FL_HAS_EVENTS;
#endif
#if defined(CONFIG_MEDIA_CONTROLLER)
	/* Initialize source pads */
	imx708->pad.flags = MEDIA_PAD_FL_SOURCE;
	sd->entity.function = MEDIA_ENT_F_CAM_SENSOR;

	ret = media_entity_pads_init(&sd->entity, 1, &imx708->pad);
	if (ret) {
		dev_err(dev, "failed to init entity pads: %d\n", ret);
		goto error_handler_free;
	}
#endif

	memset(facing, 0, sizeof(facing));
	if (strcmp(imx708->module_facing, "back") == 0)
		facing[0] = 'b';
	else
		facing[0] = 'f';

	snprintf(sd->name, sizeof(sd->name), "m%02d_%s_%s %s",
		 imx708->module_index, facing,
		 IMX708_NAME, dev_name(sd->dev));

	ret = v4l2_async_register_subdev_sensor(&imx708->subdev);
	if (ret < 0) {
		dev_err(dev, "failed to register sensor sub-device: %d\n", ret);
		goto error_media_entity;
	}

	return 0;

error_media_entity:
#if defined(CONFIG_MEDIA_CONTROLLER)
	media_entity_cleanup(&sd->entity);
#endif

error_handler_free:
	v4l2_ctrl_handler_free(&imx708->ctrl_handler);
	mutex_destroy(&imx708->mutex);
error_pm_runtime:
	pm_runtime_disable(&client->dev);
	pm_runtime_set_suspended(&client->dev);

error_power_off:
	imx708_power_off(&client->dev);

	return ret;
}

static void imx708_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct imx708 *imx708 = to_imx708(sd);

	v4l2_async_unregister_subdev(sd);
#if defined(CONFIG_MEDIA_CONTROLLER)
	media_entity_cleanup(&sd->entity);
#endif
	v4l2_ctrl_handler_free(&imx708->ctrl_handler);
	mutex_destroy(&imx708->mutex);

	pm_runtime_disable(&client->dev);
	if (!pm_runtime_status_suspended(&client->dev))
		imx708_power_off(&client->dev);
	pm_runtime_set_suspended(&client->dev);
}

#if IS_ENABLED(CONFIG_OF)
static const struct of_device_id imx708_of_match[] = {
	{ .compatible = "sony,imx708" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, imx708_of_match);
#endif

static const struct i2c_device_id imx708_match_id[] = {
	{ "sony,imx708", 0 },
	{ },
};

static const struct dev_pm_ops imx708_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(imx708_suspend, imx708_resume)
	SET_RUNTIME_PM_OPS(imx708_power_off, imx708_power_on, NULL)
};

static struct i2c_driver imx708_i2c_driver = {
	.driver = {
		.name = "imx708",
		.of_match_table	= of_match_ptr(imx708_of_match),
		.pm = &imx708_pm_ops,
	},
	.probe = &imx708_probe,
	.remove = &imx708_remove,
    .id_table	= imx708_match_id,
};

static int __init sensor_mod_init(void)
{
	return i2c_add_driver(&imx708_i2c_driver);
}

static void __exit sensor_mod_exit(void)
{
	i2c_del_driver(&imx708_i2c_driver);
}

device_initcall_sync(sensor_mod_init);
module_exit(sensor_mod_exit);

MODULE_DESCRIPTION("Sony IMX708 sensor driver");
MODULE_LICENSE("GPL v2");
