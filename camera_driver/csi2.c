/*
 * csi2.c - RP1 CSI-2 DMA controller driver (QNX port)
 *
 * Ported from linux/drivers/media/platform/raspberrypi/rp1_cfe/csi2.c
 * rpi-6.12.y branch, Raspberry Pi Ltd.
 *
 * Key changes vs. Linux:
 *   readl/writel    → reg_rd / reg_wr (volatile pointer, byte-offset / 4)
 *   dma_addr_t      → uint64_t physical address
 *   dev_dbg/warn    → fprintf(stderr, ...)
 *   V4L2 / DMA API  → removed; caller supplies physical buf address
 *   usleep_range    → usleep()
 */

#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include "csi2.h"

/* -----------------------------------------------------------------------
 * Register helpers (same pattern as dphy.c)
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
    fprintf(stderr, "[csi2] opening RX, %d lane(s)\n", c->nlanes);

    /* Soft-reset (self-clearing) */
    reg_wr(c->base, CSI2_CTRL, CSI2_CTRL_SRST);
    usleep(100);

    /* Clear any stale status bits */
    csi2_clear_status(c);

    /* Set number of active lanes (write N-1) */
    reg_wr(c->base, CSI2_N_LANES, CSI2_N_LANES_VAL(c->nlanes - 1));

    /* Enable CSI-2 receiver */
    reg_wr(c->base, CSI2_CTRL, CSI2_CTRL_EN);

    fprintf(stderr, "[csi2] RX enabled, STATUS=0x%08x\n",
            reg_rd(c->base, CSI2_STATUS));
}

void csi2_start_channel(csi2_t *c, int ch,
                         uint64_t buf_phys,
                         uint32_t width, uint32_t height,
                         int vc, int dt)
{
    /*
     * All size/address values are right-shifted >>4 before writing:
     * the hardware stores them in units of 16 bytes.
     *
     * buf_phys must be 16-byte aligned (guaranteed by mmap contiguous alloc).
     */
    uint32_t buf_size  = width * height;
    uint32_t stride    = width;

    /* Address split: RP1 DMA uses 40-bit physical addresses.
     * CH_ADDR1 holds bits [63:36] of (phys >> 4).
     * CH_ADDR0 holds bits [31:0]  of (phys >> 4). */
    uint64_t addr_shifted = buf_phys >> 4;
    uint32_t addr0 = (uint32_t)(addr_shifted & 0xFFFFFFFFu);
    uint32_t addr1 = (uint32_t)(addr_shifted >> 32);

    fprintf(stderr, "[csi2] ch%d start: buf_phys=0x%016llx size=%u "
            "stride=%u vc=%d dt=0x%02x\n",
            ch, (unsigned long long)buf_phys, buf_size, stride, vc, dt);

    /*
     * Build CH_CTRL word.
     * Bits [6:5]  = VC
     * Bits [13:8] = DT
     * Other flags: CH_CTRL_EN | CH_CTRL_IRQ_FE_ACK | CH_CTRL_PACK_LINE
     */
    uint32_t ctrl = CH_CTRL_EN
                  | CH_CTRL_IRQ_FE         /* interrupt on frame end        */
                  | CH_CTRL_IRQ_FE_ACK     /* ack-cleared variant           */
                  | CH_CTRL_PACK_LINE      /* pack: one AXI burst per line  */
                  | ((uint32_t)vc << CH_CTRL_VC_SHIFT)
                  | ((uint32_t)dt << CH_CTRL_DT_SHIFT);

    /*
     * Write order is important:
     *   1. LENGTH and STRIDE first (don't trigger anything)
     *   2. CTRL (arms IRQs, sets VC/DT filter)
     *   3. ADDR1 (high bits)
     *   4. ADDR0 LAST — writing ADDR0 causes HW to latch the full
     *      address into the double-buffer, arming the next capture.
     */
    reg_wr(c->base, CSI2_CH_LENGTH(ch),     buf_size >> 4);
    reg_wr(c->base, CSI2_CH_STRIDE_REG(ch), stride >> 4);
    reg_wr(c->base, CSI2_CH_CTRL(ch),       ctrl);
    reg_wr(c->base, CSI2_CH_ADDR1(ch),      addr1);
    reg_wr(c->base, CSI2_CH_ADDR0(ch),      addr0); /* MUST be last */

    fprintf(stderr, "[csi2] ch%d armed: CTRL=0x%08x ADDR0=0x%08x "
            "ADDR1=0x%08x\n", ch, ctrl, addr0, addr1);
}

void csi2_stop_channel(csi2_t *c, int ch)
{
    /* Clear EN bit in CH_CTRL */
    uint32_t ctrl = reg_rd(c->base, CSI2_CH_CTRL(ch));
    ctrl &= ~CH_CTRL_EN;
    reg_wr(c->base, CSI2_CH_CTRL(ch), ctrl);
    fprintf(stderr, "[csi2] ch%d stopped\n", ch);
}

uint32_t csi2_read_status(csi2_t *c)
{
    return reg_rd(c->base, CSI2_STATUS);
}

uint32_t csi2_read_debug(csi2_t *c, int ch)
{
    return reg_rd(c->base, CSI2_CH_DEBUG(ch));
}

void csi2_clear_status(csi2_t *c)
{
    reg_wr(c->base, CSI2_STATUS, 0xFFFFFFFFu);
}

void csi2_close(csi2_t *c)
{
    /* Disable all channels */
    for (int ch = 0; ch < 4; ch++)
        csi2_stop_channel(c, ch);

    /* Soft-reset */
    reg_wr(c->base, CSI2_CTRL, CSI2_CTRL_SRST);
    usleep(100);
    reg_wr(c->base, CSI2_CTRL, 0);
    fprintf(stderr, "[csi2] closed\n");
}
