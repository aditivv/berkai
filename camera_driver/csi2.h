/*
 * csi2.h - RP1 CSI-2 DMA controller registers (QNX port)
 *
 * Register layout confirmed from:
 *   linux/drivers/media/platform/raspberrypi/rp1_cfe/csi2.c (rpi-6.12.y)
 *
 * RP1 address map (from rp1.dtsi csi@110000):
 *   CSI2 DMA base = RP1_BAR0 + 0x00110000  (reg[0], size 0x100)
 *   DPHY/Host     = RP1_BAR0 + 0x00114000  (reg[1], size 0x100)
 *   MIPI CFG      = RP1_BAR0 + 0x00120000  (reg[2], size 0x100)
 *
 * RP1's CSI-2 DMA block has 4 independent channels (CH0–CH3).
 * Each channel captures one Virtual Channel / Data Type stream.
 * We use CH0 for VC0 / RAW10 (IMX708 default).
 *
 * IMPORTANT: There is NO global "enable" or "N_LANES" register in the
 * CSI2 DMA block.  Lanes are configured in the DPHY.  The DMA channel
 * is armed by writing DMA_EN in CH_CTRL and then writing CH_ADDR0 last.
 */
#ifndef CSI2_H
#define CSI2_H

#include <stdint.h>
#include "dphy.h"

/* -----------------------------------------------------------------------
 * Global CSI-2 DMA registers (offsets from CSI2 DMA base)
 * ----------------------------------------------------------------------- */

#define CSI2_STATUS             0x000   /* Interrupt status — W1C             */
#define CSI2_QOS                0x004   /* AXI QoS (leave at reset default)   */
#define CSI2_DISCARDS_OVERFLOW  0x008
#define CSI2_DISCARDS_INACTIVE  0x00c
#define CSI2_DISCARDS_UNMATCHED 0x010
#define CSI2_DISCARDS_LEN_LIMIT 0x014
#define CSI2_LLEV_PANICS        0x018
#define CSI2_ULEV_PANICS        0x01c
#define CSI2_IRQ_MASK           0x020   /* Error IRQ mask (0 = no HW IRQ)     */
#define CSI2_CTRL               0x024   /* Global control                     */

/* CSI2_CTRL bits */
#define EOP_IS_EOL              (1u << 0)   /* Treat EOP as EOL (set for RAW) */

/* CSI2_STATUS bit fields
 * Each channel x has 5 status bits; channel stride = 4 bits within group */
#define IRQ_FS(x)               (1u << (0  + (x)))  /* Frame Start, ch x */
#define IRQ_FE(x)               (1u << (4  + (x)))  /* Frame End,   ch x */
#define IRQ_FE_ACK(x)           (1u << (8  + (x)))  /* FE ACK,      ch x */
#define IRQ_LE(x)               (1u << (12 + (x)))  /* Line End,    ch x */
#define IRQ_LE_ACK(x)           (1u << (16 + (x)))  /* LE ACK,      ch x */
#define IRQ_CH_MASK(x)  (IRQ_FS(x)|IRQ_FE(x)|IRQ_FE_ACK(x)|IRQ_LE(x)|IRQ_LE_ACK(x))
#define IRQ_OVERFLOW            (1u << 20)
#define IRQ_DISCARD_OVERFLOW    (1u << 21)
#define IRQ_DISCARD_LEN_LIMIT   (1u << 22)
#define IRQ_DISCARD_UNMATCHED   (1u << 23)
#define IRQ_DISCARD_INACTIVE    (1u << 24)

/* -----------------------------------------------------------------------
 * Per-channel registers — 4 channels, stride 0x40 each.
 * Channel N starts at 0x28 + N*0x40.
 * ----------------------------------------------------------------------- */

#define CSI2_CH_CTRL(x)         ((x) * 0x40 + 0x28)
#define CSI2_CH_ADDR0(x)        ((x) * 0x40 + 0x2c)  /* *** Write LAST *** */
#define CSI2_CH_STRIDE(x)       ((x) * 0x40 + 0x30)
#define CSI2_CH_LENGTH(x)       ((x) * 0x40 + 0x34)
#define CSI2_CH_DEBUG(x)        ((x) * 0x40 + 0x38)
#define CSI2_CH_ADDR1(x)        ((x) * 0x40 + 0x3c)
#define CSI2_CH_FRAME_SIZE(x)   ((x) * 0x40 + 0x40)  /* (height<<16)|width_px */

/* CH_CTRL bit fields */
#define DMA_EN                  (1u << 0)   /* Enable DMA for this channel    */
#define FORCE                   (1u << 3)   /* Force-stop (use to disable)    */
#define AUTO_ARM                (1u << 4)   /* Re-arm automatically (dbl-buf) */
#define IRQ_EN_FS               (1u << 13)  /* HW IRQ on Frame Start          */
#define IRQ_EN_FE               (1u << 14)  /* HW IRQ on Frame End            */
#define IRQ_EN_FE_ACK           (1u << 15)  /* HW IRQ on FE ACK               */
#define IRQ_EN_LE               (1u << 16)  /* HW IRQ on Line End             */
#define IRQ_EN_LE_ACK           (1u << 17)  /* HW IRQ on LE ACK               */
#define PACK_LINE               (1u << 29)  /* One AXI burst per line         */
#define PACK_BYTES              (1u << 30)  /* Pack bytes (use for embedded)   */

/* CH_CTRL field masks / shifts (use set_field() pattern) */
#define CH_MODE_SHIFT           1
#define CH_MODE_MASK            (0x3u << 1)  /* 0=normal, 1=remap, 3=FE      */
#define VC_SHIFT                5
#define VC_MASK                 (0x3u << 5)  /* Virtual Channel [6:5]         */
#define DT_SHIFT                7
#define DT_MASK                 (0x3fu << 7) /* Data Type      [12:7]         */

/* CH_DEBUG register layout */
#define CH_DEBUG_FRAME_SHIFT    16           /* Frame counter in bits [31:16] */
#define CH_DEBUG_LINE_MASK      0xFFFFu      /* Current line in bits [15:0]   */

/* MIPI CSI-2 Data Types */
#define CSI2_DT_RAW10           0x2Bu
#define CSI2_DT_EMBEDDED        0x12u

/* -----------------------------------------------------------------------
 * Context structure
 * ----------------------------------------------------------------------- */

typedef struct {
    volatile uint32_t *base;   /* mmap'd CSI2 DMA register base */
    dphy_t            *dphy;   /* associated D-PHY context       */
    int                nlanes; /* active CSI-2 data lanes        */
} csi2_t;

/* -----------------------------------------------------------------------
 * API
 * ----------------------------------------------------------------------- */

void csi2_init(csi2_t *c, volatile uint32_t *base, dphy_t *dphy, int nlanes);

/*
 * csi2_open_rx - enable the CSI-2 receiver.
 *   Writes IRQ_MASK=0, starts the DPHY, sets EOP_IS_EOL in CTRL.
 *   Call AFTER the sensor is streaming (so DPHY can lock to LP-11).
 */
void csi2_open_rx(csi2_t *c);

/*
 * csi2_start_channel - arm DMA channel n to capture into buf_phys.
 *   ch         : channel index (0–3)
 *   buf_phys   : physical address of pre-allocated contiguous buffer
 *   stride     : bytes per line (RAW10: width_pixels * 10/8)
 *   height     : image height in lines
 *   width_px   : image width in pixels (for FRAME_SIZE register)
 *   vc         : Virtual Channel (0 for IMX708 default)
 *   dt         : Data Type (CSI2_DT_RAW10)
 *
 * CH_ADDR0 is written LAST — this latches all other registers and arms
 * the double-buffer, starting DMA capture on the next frame.
 */
void csi2_start_channel(csi2_t *c, int ch,
                         uint64_t buf_phys,
                         uint32_t stride, uint32_t height, uint32_t width_px,
                         int vc, int dt);

void csi2_stop_channel(csi2_t *c, int ch);
uint32_t csi2_read_status(csi2_t *c);

/*
 * csi2_read_frame_count - return the frame counter for channel ch.
 *   Increments each time a frame end is captured.
 *   Lives in CH_DEBUG bits [31:16].
 */
uint32_t csi2_read_frame_count(csi2_t *c, int ch);

void csi2_clear_status(csi2_t *c);
void csi2_close(csi2_t *c);

#endif /* CSI2_H */
