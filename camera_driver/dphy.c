/*
 * dphy.c - RP1 DW CSI-2 Host + D-PHY driver (QNX port)
 *
 * Faithfully ported from:
 *   linux/drivers/media/platform/raspberrypi/rp1_cfe/dphy.c  (rpi-6.12.y)
 *   Raspberry Pi Ltd.
 *
 * Translation notes (Linux → QNX):
 *   readl/writel            → volatile uint32_t* word access (byte offset >> 2)
 *   usleep_range(a, b)      → usleep(b)
 *   dev_dbg/info/err        → fprintf(stderr, ...)
 *   struct dphy_data        → dphy_t
 *   All Linux-specific glue (module, clk, reset, devm_*) removed.
 *
 * Previous version of this file used completely wrong register offsets —
 * see dphy.h for a detailed change log.  Specifically, "CTRL0" (0x000) was
 * the VERSION register (DW DPHY Host HW v1.20 = 0x3132302a), not a lane-
 * enable register.  All lane and PHY control has been corrected below.
 */

#include <stdio.h>
#include <stdint.h>
#include <unistd.h>       /* usleep() */
#include "dphy.h"

/* -----------------------------------------------------------------------
 * Internal register helpers
 * Registers are 32-bit, byte-addressed.  base is word-addressed (uint32_t*),
 * so divide byte offset by 4.
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
 * Test interface helpers — all use read-modify-write to toggle individual
 * bits.  Linux never writes whole-register values to the test registers
 * because other bits may be live (e.g. TESTCLK while toggling TESTEN).
 * ----------------------------------------------------------------------- */

static void set_tstclr(dphy_t *d, uint32_t val)
{
    uint32_t ctrl0 = reg_rd(d->base, DPHY_TST_CTRL0);
    reg_wr(d->base, DPHY_TST_CTRL0, (ctrl0 & ~TST_CTRL0_TESTCLR) |
           (val ? TST_CTRL0_TESTCLR : 0));
}

static void set_tstclk(dphy_t *d, uint32_t val)
{
    uint32_t ctrl0 = reg_rd(d->base, DPHY_TST_CTRL0);
    reg_wr(d->base, DPHY_TST_CTRL0, (ctrl0 & ~TST_CTRL0_TESTCLK) |
           (val ? TST_CTRL0_TESTCLK : 0));
}

static uint8_t get_tstdout(dphy_t *d)
{
    uint32_t ctrl1 = reg_rd(d->base, DPHY_TST_CTRL1);
    return (uint8_t)((ctrl1 >> 8) & 0xFF);
}

static void set_testen(dphy_t *d, uint32_t val)
{
    uint32_t ctrl1 = reg_rd(d->base, DPHY_TST_CTRL1);
    reg_wr(d->base, DPHY_TST_CTRL1, (ctrl1 & ~TST_CTRL1_TESTEN) |
           (val ? TST_CTRL1_TESTEN : 0));
}

static void set_testdin(dphy_t *d, uint32_t val)
{
    uint32_t ctrl1 = reg_rd(d->base, DPHY_TST_CTRL1);
    reg_wr(d->base, DPHY_TST_CTRL1,
           (ctrl1 & ~TST_CTRL1_TESTDIN_MASK) | (val & TST_CTRL1_TESTDIN_MASK));
}

/* -----------------------------------------------------------------------
 * dphy_transaction — write one internal DPHY register via the test bus.
 *
 * Protocol (DW DPHY databook §"Test Interface"):
 *   Address phase (TESTEN=1 + TESTCLK falling edge):
 *     1. TESTCLK high
 *     2. TESTEN low
 *     3. TESTDIN ← test_code
 *     4. TESTEN high
 *     5. TESTCLK low   ← address latched on falling edge
 *   Data phase (TESTEN=0 + TESTCLK rising edge):
 *     6. TESTEN low
 *     7. TESTDIN ← test_data
 *     8. TESTCLK high  ← data written on rising edge
 * Returns TESTDOUT (useful for read-back, ignored here).
 * ----------------------------------------------------------------------- */

static uint8_t dphy_transaction(dphy_t *d, uint8_t test_code, uint8_t test_data)
{
    set_tstclk(d, 1);
    set_testen(d, 0);
    set_testdin(d, test_code);
    set_testen(d, 1);
    set_tstclk(d, 0);
    set_testen(d, 0);
    set_testdin(d, test_data);
    set_tstclk(d, 1);
    return get_tstdout(d);
}

/* -----------------------------------------------------------------------
 * dphy_set_hsfreqrange — program internal HS receive frequency range.
 *
 * Test code 0x44 selects HS_RX_CTRL_LANE0 (HSFREQRANGE).
 * Data byte = hsfreqrange_code << 1.
 *
 * Table from Linux rp1_cfe/dphy.c (which in turn references DW DPHY
 * databook Table 5-1).  All lane Mbps thresholds are upper bounds.
 * We hard-code 450 Mbps for the IMX708 in 2x2bin mode:
 *   450 Mbps falls in (449, 499] → code = 0b010110 → data = 0x2C.
 *
 * (Previous code used 0x0C = code 0b000110, valid only for ≤449 Mbps.)
 * ----------------------------------------------------------------------- */

static void dphy_set_hsfreqrange(dphy_t *d, uint32_t mbps)
{
    /* DW DPHY databook Table 5-1 — (max_mbps, code) pairs */
    static const struct { uint16_t max_mbps; uint8_t code; } table[] = {
        {  89, 0b000000 }, {  99, 0b010000 }, { 109, 0b100000 },
        { 129, 0b000001 }, { 139, 0b010001 }, { 149, 0b100001 },
        { 169, 0b000010 }, { 179, 0b010010 }, { 199, 0b100010 },
        { 219, 0b000011 }, { 239, 0b010011 }, { 249, 0b100011 },
        { 269, 0b000100 }, { 299, 0b010100 }, { 329, 0b000101 },
        { 359, 0b010101 }, { 399, 0b100101 }, { 449, 0b000110 },
        { 499, 0b010110 }, { 549, 0b000111 }, { 599, 0b010111 },
        { 649, 0b001000 }, { 699, 0b011000 }, { 749, 0b001001 },
        { 799, 0b011001 }, { 849, 0b101001 }, { 899, 0b111001 },
        { 949, 0b001010 }, { 999, 0b011010 }, {1049, 0b101010 },
        {1099, 0b111010 }, {1149, 0b001011 }, {1199, 0b011011 },
        {1249, 0b101011 }, {1299, 0b111011 }, {1349, 0b001100 },
        {1399, 0b011100 }, {1449, 0b101100 }, {1500, 0b111100 },
    };
    unsigned int n = sizeof(table) / sizeof(table[0]);
    unsigned int i;

    if (mbps < 80 || mbps > 1500)
        fprintf(stderr, "[dphy] WARNING: link rate %u Mbps out of table range\n",
                mbps);

    for (i = 0; i < n - 1; i++) {
        if (mbps <= table[i].max_mbps)
            break;
    }

    uint8_t data = (uint8_t)(table[i].code << 1);
    fprintf(stderr, "[dphy] HSFREQRANGE: %u Mbps → code 0x%02x → data byte 0x%02x\n",
            mbps, table[i].code, data);
    dphy_transaction(d, DPHY_TESTCODE_HSFREQRANGE, data);
}

/* -----------------------------------------------------------------------
 * dphy_hw_init — power-cycle the PHY and program frequency range.
 *   Matches Linux dphy_init() exactly.
 * ----------------------------------------------------------------------- */

static void dphy_hw_init(dphy_t *d)
{
    /* Assert PHY resets */
    reg_wr(d->base, DPHY_PHY_RSTZ,      0);
    reg_wr(d->base, DPHY_PHY_SHUTDOWNZ, 0);

    /* Initialise test interface: TESTCLK high, TESTEN low, then TESTCLR pulse */
    set_tstclk(d, 1);
    set_testen(d, 0);
    set_tstclr(d, 1);
    usleep(15);
    set_tstclr(d, 0);
    usleep(15);

    /* Program HS receive frequency range.
     * IMX708 link_freq = 450 MHz; D-PHY data rate = 2 × link_freq (DDR) =
     * 900 Mbps/lane (confirmed from Linux rp1_cfe cfe.c: link_freq *= 2).
     * 900 Mbps → table code 0b001010 → data byte 0x14.
     * (Was 450 here, which gave 0x2C and mis-sampled the HS data: lanes
     *  reached LP-11 but RAW10 packets were corrupted and never captured.) */
    dphy_set_hsfreqrange(d, 900);

    /* Power up the PHY */
    usleep(5);
    reg_wr(d->base, DPHY_PHY_SHUTDOWNZ, 1);
    usleep(5);
    reg_wr(d->base, DPHY_PHY_RSTZ,      1);
}

/* -----------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------- */

void dphy_init(dphy_t *d, volatile uint32_t *base, int nlanes)
{
    d->base   = base;
    d->nlanes = nlanes;

    /* Log the hardware version so we know we're talking to the right block */
    uint32_t ver = reg_rd(d->base, DPHY_VERSION);
    uint8_t vmaj = (uint8_t)((ver >> 24) - '0');
    uint8_t vmin = (uint8_t)(((ver >> 16) - '0') * 10 + ((ver >> 8) - '0'));
    fprintf(stderr, "[dphy] DW CSI-2 Host HW v%u.%u (VERSION=0x%08x)\n",
            vmaj, vmin, ver);
}

void dphy_start(dphy_t *d)
{
    fprintf(stderr, "[dphy] starting, %d lane(s)\n", d->nlanes);

    /*
     * 1. Assert host soft reset.
     *    RESETN=0 holds the DW CSI-2 host state machine in reset.
     */
    reg_wr(d->base, DPHY_RESETN, 0);

    /*
     * 2. Set number of active data lanes.
     *    N_LANES register takes (nlanes - 1): 0=1 lane, 1=2 lanes, 3=4 lanes.
     */
    reg_wr(d->base, DPHY_N_LANES, (uint32_t)(d->nlanes - 1));
    fprintf(stderr, "[dphy] N_LANES=0x%08x (wrote %d = %d lane(s))\n",
            reg_rd(d->base, DPHY_N_LANES), d->nlanes - 1, d->nlanes);

    /*
     * 3. Initialise the D-PHY (power cycle + HSFREQRANGE).
     */
    dphy_hw_init(d);

    /*
     * 4. Release host soft reset.
     */
    reg_wr(d->base, DPHY_RESETN, 0xFFFFFFFFu);
    usleep(20);

    fprintf(stderr, "[dphy] started: RESETN=0x%08x N_LANES=0x%08x "
            "PHY_SHUTDOWNZ=0x%08x PHY_RSTZ=0x%08x\n",
            reg_rd(d->base, DPHY_RESETN),
            reg_rd(d->base, DPHY_N_LANES),
            reg_rd(d->base, DPHY_PHY_SHUTDOWNZ),
            reg_rd(d->base, DPHY_PHY_RSTZ));
    fprintf(stderr, "[dphy] PHY_RX=0x%08x PHY_STOPSTATE=0x%08x\n",
            reg_rd(d->base, DPHY_PHY_RX),
            reg_rd(d->base, DPHY_PHY_STOPSTATE));
}

int dphy_wait_stop(dphy_t *d, int timeout_ms)
{
    /*
     * PHY_STOPSTATE bits [nlanes-1:0] must all be 1 to confirm LP-11.
     * The sensor drives LP-11 between bursts/frames once it is streaming.
     */
    uint32_t lane_mask = (1u << d->nlanes) - 1u;   /* 0x3 for 2 lanes */
    int elapsed = 0;

    while (elapsed < timeout_ms) {
        uint32_t st = reg_rd(d->base, DPHY_PHY_STOPSTATE);
        if ((st & lane_mask) == lane_mask) {
            fprintf(stderr, "[dphy] LP-11 stop-state confirmed after %d ms "
                    "(PHY_STOPSTATE=0x%08x)\n", elapsed, st);
            return 0;
        }
        usleep(1000);
        elapsed++;
    }

    fprintf(stderr, "[dphy] ERROR: stop-state timeout after %d ms — "
            "PHY_STOPSTATE=0x%08x (want bits 0x%x set)\n",
            timeout_ms,
            reg_rd(d->base, DPHY_PHY_STOPSTATE),
            lane_mask);
    fprintf(stderr, "[dphy] PHY_RX=0x%08x  PHY_SHUTDOWNZ=0x%08x  PHY_RSTZ=0x%08x\n",
            reg_rd(d->base, DPHY_PHY_RX),
            reg_rd(d->base, DPHY_PHY_SHUTDOWNZ),
            reg_rd(d->base, DPHY_PHY_RSTZ));
    return -1;
}

void dphy_stop(dphy_t *d)
{
    /*
     * Intentional no-op — matches Linux rp1_cfe dphy_stop().
     *
     * Comment from Linux source:
     *   "We no longer go into reset here, because the camera might still be
     *    streaming. If we kill the CSI-2 Host in mid-packet, it can leave the
     *    IDI interface in a bad state, causing the next packet to be lost."
     */
    (void)d;
    fprintf(stderr, "[dphy] stop (no-op — host left running per Linux policy)\n");
}
