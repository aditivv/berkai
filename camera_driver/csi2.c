/*
 * csi2.c - RP1 CSI-2 DMA controller driver (QNX port)
 *
 * Ported from linux/drivers/media/platform/raspberrypi/rp1_cfe/csi2.c
 * rpi-6.12.y branch, Raspberry Pi Ltd.
 *
 * Register offsets verified against the Linux source.  Previous version
 * had completely wrong offsets (channel stride 0x10 vs real 0x40, wrong
 * STATUS/CTRL locations, non-existent N_LANES register, etc.).
 */

#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include "csi2.h"

/* -----------------------------------------------------------------------
 * Register helpers
 * ----------------------------------------------------------------------- */

static inline uint32_t reg_rd(volatile uint32_t *base, uint32_t off)
{
    return base[off >> 2];
}

static inline void reg_wr(volatile uint32_t *base, uint32_t off, uint32_t val)
{
    base[off >> 2] = val;
}

/* -----------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------- */

void csi2_init(csi2_t *c, volatile uint32_t *base, dphy_t *dphy, int nlanes)
{
    c->base   = base;
    c->dphy   = dphy;
    c->nlanes = nlanes;
}

void csi2_open_rx(csi2_t *c)
{
    fprintf(stderr, "[csi2] opening RX (%d lanes)\n", c->nlanes);

    /*
     * CSI2_IRQ_MASK gates the hardware interrupt line for error/overflow
     * conditions only; it does not affect STATUS updates or CH_DEBUG counts.
     * Set to 0 — we are polling, not using HW interrupts.
     */
    reg_wr(c->base, CSI2_IRQ_MASK, 0);

    /*
     * Start the DPHY.  Must happen after sensor is streaming so the DPHY
     * can lock to the LP-11 idle state the sensor drives between bursts.
     */
    dphy_start(c->dphy);

    /*
     * EOP_IS_EOL: treat each packet end as a line end.
     * Required for correct DMA line-packing of RAW Bayer data.
     */
    reg_wr(c->base, CSI2_CTRL, EOP_IS_EOL);

    fprintf(stderr, "[csi2] RX enabled, STATUS=0x%08x CTRL=0x%08x\n",
            reg_rd(c->base, CSI2_STATUS),
            reg_rd(c->base, CSI2_CTRL));
}

void csi2_start_channel(csi2_t *c, int ch,
                         uint64_t buf_phys,
                         uint32_t stride, uint32_t height, uint32_t width_px,
                         int vc, int dt)
{
    uint32_t size = stride * height;   /* total bytes in one frame */
    uint64_t addr = buf_phys >> 4;     /* addresses are in units of 16 bytes */

    fprintf(stderr, "[csi2] ch%d start: phys=0x%016llx size=%u "
            "stride=%u h=%u w_px=%u vc=%d dt=0x%02x\n",
            ch, (unsigned long long)buf_phys, size,
            stride, height, width_px, vc, dt);

    /* 1. Disable channel and clear debug counter */
    reg_wr(c->base, CSI2_CH_CTRL(ch),  0);
    reg_wr(c->base, CSI2_CH_DEBUG(ch), 0);

    /* 2. Clear this channel's interrupt flags in STATUS (W1C) */
    reg_wr(c->base, CSI2_STATUS, IRQ_CH_MASK(ch));

    /* 3. Build CH_CTRL:
     *      DMA_EN    = enable DMA
     *      PACK_LINE = one AXI burst per line (required for correct packing)
     *      VC field  = bits [6:5]
     *      DT field  = bits [12:7]
     *
     * Note: we do NOT set IRQ_EN_FE_ACK here because we poll CH_DEBUG.
     * Set it if/when HW interrupts are wired up.
     */
    uint32_t ctrl = DMA_EN | PACK_LINE;
    ctrl |= ((uint32_t)vc << VC_SHIFT) & VC_MASK;
    ctrl |= ((uint32_t)dt << DT_SHIFT) & DT_MASK;

    /* 4. Set frame geometry (height in pixels [31:16], width in pixels [15:0]).
     *    The DMA uses this to detect frame boundaries independent of packet
     *    count, providing robustness against dropped lines. */
    reg_wr(c->base, CSI2_CH_FRAME_SIZE(ch), (height << 16) | width_px);

    /* 5. Write buffer registers.
     *    ADDR0 MUST be written LAST — it latches all other channel registers
     *    into the hardware double-buffer, arming the DMA for the next frame. */
    reg_wr(c->base, CSI2_CH_LENGTH(ch), size   >> 4);
    reg_wr(c->base, CSI2_CH_STRIDE(ch), stride >> 4);
    reg_wr(c->base, CSI2_CH_CTRL(ch),   ctrl);
    reg_wr(c->base, CSI2_CH_ADDR1(ch),  (uint32_t)(addr >> 32));
    reg_wr(c->base, CSI2_CH_ADDR0(ch),  (uint32_t)(addr & 0xFFFFFFFFu)); /* ARM */

    fprintf(stderr, "[csi2] ch%d armed: CTRL=0x%08x ADDR0=0x%08x "
            "ADDR1=0x%08x LENGTH=0x%08x STRIDE=0x%08x\n",
            ch, ctrl,
            (uint32_t)(addr & 0xFFFFFFFFu),
            (uint32_t)(addr >> 32),
            size   >> 4,
            stride >> 4);
}

void csi2_stop_channel(csi2_t *c, int ch)
{
    /* FORCE stops capture immediately, even mid-frame */
    reg_wr(c->base, CSI2_CH_CTRL(ch), FORCE);
    /* Writing ADDR0=0 latches the FORCE bit into HW */
    reg_wr(c->base, CSI2_CH_ADDR0(ch), 0);
    reg_wr(c->base, CSI2_CH_ADDR0(ch), 0);  /* Linux does this twice */
    fprintf(stderr, "[csi2] ch%d stopped\n", ch);
}

uint32_t csi2_read_status(csi2_t *c)
{
    return reg_rd(c->base, CSI2_STATUS);
}

uint32_t csi2_read_frame_count(csi2_t *c, int ch)
{
    /* Frame counter lives in CH_DEBUG bits [31:16] */
    return reg_rd(c->base, CSI2_CH_DEBUG(ch)) >> CH_DEBUG_FRAME_SHIFT;
}

void csi2_clear_status(csi2_t *c)
{
    reg_wr(c->base, CSI2_STATUS, 0xFFFFFFFFu);
}

void csi2_close(csi2_t *c)
{
    for (int ch = 0; ch < 4; ch++)
        csi2_stop_channel(c, ch);
    dphy_stop(c->dphy);
    fprintf(stderr, "[csi2] closed\n");
}
