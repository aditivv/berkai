/*
 * csi2.h - RP1 CSI-2 DMA controller registers and API (QNX port)
 *
 * Ported from linux/drivers/media/platform/raspberrypi/rp1_cfe/csi2.h
 * rpi-6.12.y branch, Raspberry Pi Ltd.
 *
 * RP1 address map (confirmed from rp1.dtsi, Linux rpi-6.12.y):
 *   MIPI CFG base = RP1_BAR0 + 0x00120000  ← write SEL_CSI=1 before using CSI2
 *   CSI0 DMA base = RP1_BAR0 + 0x00110000, size 0x100
 *   CSI0 DPHY     = RP1_BAR0 + 0x00114000, size 0x100
 *
 * RP1's CSI-2 block has 4 independent DMA channels (VC0..VC3).
 * Each channel captures one Virtual Channel / Data Type pair.
 * We use channel 0 for VC0 / RAW10 (IMX708 default output).
 */
#ifndef CSI2_H
#define CSI2_H

#include <stdint.h>
#include "dphy.h"

/* -----------------------------------------------------------------------
 * Global CSI-2 registers (offset from CSI2 base)
 * ----------------------------------------------------------------------- */

/* Global enable / soft-reset */
#define CSI2_CTRL               0x00
#define   CSI2_CTRL_EN          (1u << 0)
#define   CSI2_CTRL_SRST        (1u << 1)     /* self-clearing soft reset */

/* CSI-2 number of active lanes */
#define CSI2_N_LANES            0x04
#define   CSI2_N_LANES_VAL(n)   ((n) & 0x7)  /* write n-1 */

/* Interrupt status (write 1 to clear) */
#define CSI2_STATUS             0x08
#define   CSI2_STATUS_PHY_ERRESC        (1u << 0)
#define   CSI2_STATUS_PHY_ERRSYNCESC    (1u << 1)
#define   CSI2_STATUS_PHY_ERRCONTROL    (1u << 2)
#define   CSI2_STATUS_PHY_ERRSOTHS      (1u << 3)
#define   CSI2_STATUS_PHY_ERRSOTHS_SYNC (1u << 4)
#define   CSI2_STATUS_PHY_ERRORS        0x1F  /* all D-PHY error bits */

/* -----------------------------------------------------------------------
 * Per-channel registers — 4 channels, stride 0x10 each
 *
 *  CH_CTRL(n)   : channel n control
 *  CH_ADDR0(n)  : DMA buffer physical address bits [35:4]  (WRITE LAST)
 *  CH_ADDR1(n)  : DMA buffer physical address bits [63:36]
 *  CH_LENGTH(n) : buffer length in bytes, right-shifted >>4
 *  CH_STRIDE(n) : line stride in bytes, right-shifted >>4
 *  CH_DEBUG(n)  : frame count (bits [15:0]), line count (bits [31:16])
 * ----------------------------------------------------------------------- */

#define CSI2_CH_BASE            0x10          /* first channel starts here  */
#define CSI2_CH_STRIDE          0x10          /* bytes between channels     */

#define CSI2_CH_CTRL(n)     (CSI2_CH_BASE + CSI2_CH_STRIDE*(n) + 0x00)
#define CSI2_CH_ADDR0(n)    (CSI2_CH_BASE + CSI2_CH_STRIDE*(n) + 0x04)
#define CSI2_CH_ADDR1(n)    (CSI2_CH_BASE + CSI2_CH_STRIDE*(n) + 0x08)
#define CSI2_CH_DEBUG(n)    (CSI2_CH_BASE + CSI2_CH_STRIDE*(n) + 0x0C)

/* These live in the second register block (offset 0x50 from CSI2 base) */
#define CSI2_CH2_BASE           0x50
#define CSI2_CH_LENGTH(n)   (CSI2_CH2_BASE + 0x08*(n) + 0x00)
#define CSI2_CH_STRIDE_REG(n) (CSI2_CH2_BASE + 0x08*(n) + 0x04)

/* CH_CTRL bit fields */
#define   CH_CTRL_EN            (1u << 0)    /* enable DMA channel         */
#define   CH_CTRL_IRQ_FS        (1u << 1)    /* IRQ on frame start         */
#define   CH_CTRL_IRQ_FE        (1u << 2)    /* IRQ on frame end           */
#define   CH_CTRL_IRQ_FE_ACK    (1u << 3)    /* IRQ on frame end (ack)     */
#define   CH_CTRL_IRQ_OF        (1u << 4)    /* IRQ on overflow            */
#define   CH_CTRL_PACK_LINE     (1u << 7)    /* pack pixels, one burst/line */
#define   CH_CTRL_VC_SHIFT      5            /* Virtual Channel [6:5]      */
#define   CH_CTRL_DT_SHIFT      8            /* Data Type    [13:8]        */

/* MIPI CSI-2 Data Types used by IMX708 */
#define CSI2_DT_RAW10           0x2B
#define CSI2_DT_EMBEDDED        0x12

/* -----------------------------------------------------------------------
 * Context structure
 * ----------------------------------------------------------------------- */

typedef struct {
    volatile uint32_t *base;   /* mmap'd CSI2 DMA register base          */
    dphy_t            *dphy;   /* associated D-PHY (shared context)       */
    int                nlanes; /* active CSI-2 data lanes                 */
} csi2_t;

/* -----------------------------------------------------------------------
 * API
 * ----------------------------------------------------------------------- */

void csi2_init(csi2_t *c, volatile uint32_t *base, dphy_t *dphy, int nlanes);

/*
 * csi2_open_rx - soft-reset and enable the CSI-2 RX, configure lanes.
 *   Call after dphy_start() + dphy_wait_stop().
 */
void csi2_open_rx(csi2_t *c);

/*
 * csi2_start_channel - program DMA channel n to capture into buf_phys.
 *   ch       : channel index (0-3, use 0 for VC0 image)
 *   buf_phys : physical address of pre-allocated contiguous buffer
 *   width    : image width in bytes (for RAW10: width_pixels * 10/8)
 *   height   : image height in lines
 *   vc       : Virtual Channel (0 for IMX708 default)
 *   dt       : Data Type (use CSI2_DT_RAW10 for IMX708)
 *
 * IMPORTANT: CH_ADDR0 must be written LAST — writing it triggers the
 * hardware to arm the double-buffer, so all other registers must be set first.
 */
void csi2_start_channel(csi2_t *c, int ch,
                         uint64_t buf_phys,
                         uint32_t width, uint32_t height,
                         int vc, int dt);

/*
 * csi2_stop_channel - disable a DMA channel.
 */
void csi2_stop_channel(csi2_t *c, int ch);

/*
 * csi2_read_status - return the global CSI2_STATUS register.
 *   Callers should check for CSI2_STATUS_PHY_ERRORS being zero.
 */
uint32_t csi2_read_status(csi2_t *c);

/*
 * csi2_read_debug - return CH_DEBUG for channel n.
 *   Bits [15:0] = frame counter, increments on each frame end.
 *   Bits [31:16] = current line count within the ongoing frame.
 */
uint32_t csi2_read_debug(csi2_t *c, int ch);

/*
 * csi2_clear_status - write 1s to clear all status bits.
 */
void csi2_clear_status(csi2_t *c);

/*
 * csi2_close - disable the RX, assert soft-reset.
 */
void csi2_close(csi2_t *c);

#endif /* CSI2_H */
