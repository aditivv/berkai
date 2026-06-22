/*
 * imx708_regs.h - Sony IMX708 sensor register tables for QNX driver
 *
 * Extracted from linux/drivers/media/i2c/imx708.c
 * rpi-6.12.y branch, Raspberry Pi Ltd / Sony.
 *
 * IMX708 I2C:
 *   Address    : 0x1A (7-bit)
 *   Bus        : camera I2C (RPi5 = /dev/i2c6 on Linux; adjust I2C_BUS below)
 *   Reg width  : 16-bit address, 8-bit data
 *
 * Modes provided:
 *   IMX708_MODE_FULL   : 4608 x 2592, RAW10, ~14 fps
 *   IMX708_MODE_2X2BIN : 2304 x 1296, RAW10, ~56 fps  ← default in our driver
 *   IMX708_MODE_720P   : 1536 x 864,  RAW10, ~120 fps
 *
 * CSI-2 link rate = 450 Mbps/lane, 2 lanes, Data Type = 0x2B (RAW10).
 */
#ifndef IMX708_REGS_H
#define IMX708_REGS_H

#include <stdint.h>

/* -----------------------------------------------------------------------
 * Device identity
 * ----------------------------------------------------------------------- */

#define IMX708_I2C_ADDR         0x1A        /* 7-bit I2C address          */

/* Adjust this to match your QNX i2c device node.
 * On RPi5, the camera I2C bus (GPIO22/23, CAM1) typically appears as
 * /dev/i2c6 on Linux.  Check with: i2cdetect -l  on Linux, or ls /dev/i2c*
 * on QNX after the i2c resource manager starts. */
#define IMX708_I2C_BUS          "/dev/i2c2"

#define IMX708_REG_CHIP_ID      0x0016
#define IMX708_CHIP_ID          0x0708      /* expected value (16-bit read) */

/* Stream control */
#define IMX708_REG_MODE_SELECT  0x0100
#define IMX708_STANDBY          0x00
#define IMX708_STREAMING        0x01

/* Test pattern (set to IMX708_TP_COLOR_BARS during bring-up) */
#define IMX708_REG_TEST_PATTERN 0x0600
#define IMX708_TP_DISABLED      0x0000
#define IMX708_TP_COLOR_BARS    0x0002      /* SMPTE color bars */

/* Exposure and gain (defaults that work for typical indoor lighting) */
#define IMX708_REG_EXPOSURE     0x0202
#define IMX708_REG_ANA_GAIN     0x0204
#define IMX708_DEFAULT_EXPOSURE 0x0640
#define IMX708_DEFAULT_GAIN     0x0070      /* minimum analogue gain */

/* -----------------------------------------------------------------------
 * Register table type (matches Linux struct imx708_reg)
 * ----------------------------------------------------------------------- */

typedef struct {
    uint16_t addr;
    uint8_t  val;
} imx708_reg_t;

/* -----------------------------------------------------------------------
 * Common initialisation — write first, before any mode-specific table.
 * Source: mode_common_regs[] in Linux imx708.c
 * ----------------------------------------------------------------------- */

static const imx708_reg_t imx708_common_regs[] = {
    {0x0100, 0x00},   /* standby */
    {0x0136, 0x18},   /* EXTCLK 24 MHz integer part  */
    {0x0137, 0x00},   /* EXTCLK fractional part       */
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
    /* Output format: RAW10, 2-lane CSI-2 */
    {0x0112, 0x0A},   /* CSI output 10-bit (high byte)  */
    {0x0113, 0x0A},   /* CSI output 10-bit (low byte)   */
    {0x0114, 0x01},   /* 2 data lanes                   */
    {0x0B8E, 0x01},
    {0x0B8F, 0x00},
    {0x0B94, 0x01},
    {0x0B95, 0x00},
    {0x3400, 0x01},
    {0x3478, 0x01},
    {0x3479, 0x1C},
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
#define IMX708_COMMON_REGS_N  (sizeof(imx708_common_regs) / sizeof(imx708_common_regs[0]))

/* Link frequency: 450 MHz (nominal) */
static const imx708_reg_t imx708_link_450mhz[] = {
    {0x030E, 0x01},
    {0x030F, 0x2C},
};
#define IMX708_LINK_REGS_N  (sizeof(imx708_link_450mhz) / sizeof(imx708_link_450mhz[0]))

/* -----------------------------------------------------------------------
 * Mode: 2x2 binned — 2304 x 1296, ~56 fps
 * This is the best balance of resolution and frame rate for pipe monitoring.
 * RAW10, 2-lane CSI-2 @ 450 Mbps/lane.
 * Line size = 2304 pixels * 10/8 = 2880 bytes.
 * Frame size = 2880 * 1296 = 3,732,480 bytes (~3.6 MB)
 * ----------------------------------------------------------------------- */

#define IMX708_2X2_WIDTH        2304
#define IMX708_2X2_HEIGHT       1296
#define IMX708_2X2_LINE_BYTES   (IMX708_2X2_WIDTH * 10 / 8)   /* 2880 */
#define IMX708_2X2_FRAME_BYTES  (IMX708_2X2_LINE_BYTES * IMX708_2X2_HEIGHT)

static const imx708_reg_t imx708_mode_2x2bin[] = {
    {0x0342, 0x1E},   /* line_length_pck high */
    {0x0343, 0x90},   /* line_length_pck low  */
    {0x0340, 0x05},   /* frame_length_lines high */
    {0x0341, 0x38},   /* frame_length_lines low  */
    {0x0344, 0x00},   /* x_addr_start high */
    {0x0345, 0x00},
    {0x0346, 0x00},   /* y_addr_start high */
    {0x0347, 0x00},
    {0x0348, 0x11},   /* x_addr_end high */
    {0x0349, 0xFF},
    {0x034A, 0x0A},   /* y_addr_end high */
    {0x034B, 0x1F},
    {0x0220, 0x62},
    {0x0222, 0x01},
    {0x0900, 0x01},   /* binning enable */
    {0x0901, 0x22},   /* 2x2 binning    */
    {0x0902, 0x08},
    {0x3200, 0x41},
    {0x3201, 0x41},
    {0x32D5, 0x00},
    {0x32D6, 0x00},
    {0x32DB, 0x01},
    {0x32DF, 0x00},
    {0x350C, 0x00},
    {0x350D, 0x00},
    {0x0408, 0x00},   /* digital crop x offset */
    {0x0409, 0x00},
    {0x040A, 0x00},   /* digital crop y offset */
    {0x040B, 0x00},
    {0x040C, 0x09},   /* digital crop width high  → 0x0900 = 2304 */
    {0x040D, 0x00},
    {0x040E, 0x05},   /* digital crop height high → 0x0510 = 1296 */
    {0x040F, 0x10},
    {0x034C, 0x09},   /* output width high  */
    {0x034D, 0x00},
    {0x034E, 0x05},   /* output height high */
    {0x034F, 0x10},
    {0x0301, 0x05},   /* VT pixel clock divider */
    {0x0303, 0x02},
    {0x0305, 0x02},
    {0x0306, 0x00},
    {0x0307, 0x7A},   /* PLL multiplier */
    {0x030B, 0x02},
    {0x030D, 0x04},
    {0x0310, 0x01},
    {0x3CA0, 0x00}, {0x3CA1, 0x3C},
    {0x3CA4, 0x00}, {0x3CA5, 0x3C},
    {0x3CA6, 0x00}, {0x3CA7, 0x00},
    {0x3CAA, 0x00}, {0x3CAB, 0x00},
    {0x3CB8, 0x00}, {0x3CB9, 0x1C},
    {0x3CBA, 0x00}, {0x3CBB, 0x08},
    {0x3CBC, 0x00}, {0x3CBD, 0x1E},
    {0x3CBE, 0x00}, {0x3CBF, 0x0A},
    {0x0202, 0x05},   /* coarse integration time high */
    {0x0203, 0x08},
    {0x0224, 0x01},
    {0x0225, 0xF4},
    {0x3116, 0x01},
    {0x3117, 0xF4},
    {0x0204, 0x00},   /* analogue gain high */
    {0x0205, 0x70},   /* analogue gain low  */
    {0x0216, 0x00},
    {0x0217, 0x70},
    {0x0218, 0x01},
    {0x0219, 0x00},
    {0x020E, 0x01},   /* digital gain high (unity = 0x0100) */
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
    {0x3421, 0x6C},
    {0x3366, 0x00},
    {0x3367, 0x00},
    {0x3368, 0x00},
    {0x3369, 0x00},
};
#define IMX708_MODE_2X2BIN_N  (sizeof(imx708_mode_2x2bin) / sizeof(imx708_mode_2x2bin[0]))

#endif /* IMX708_REGS_H */
