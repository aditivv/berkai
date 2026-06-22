/*
 * dphy.c - DesignWare MIPI D-PHY driver for RP1 (QNX port)
 *
 * Ported from linux/drivers/media/platform/raspberrypi/rp1_cfe/dphy.c
 * rpi-6.12.y branch.
 *
 * Key changes vs. Linux:
 *   readl/writel  →  direct volatile pointer access (reg_rd / reg_wr)
 *   usleep_range  →  usleep()
 *   dev_dbg/err   →  fprintf(stderr, ...)
 *   module glue   →  removed
 */

#include <stdio.h>
#include <stdint.h>
#include <unistd.h>       /* usleep() */
#include "dphy.h"

/* -----------------------------------------------------------------------
 * Internal register helpers
 * DPHY registers are 32-bit, byte-addressed.
 * base is volatile uint32_t* (word-addressed), so offset /4.
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
 * DesignWare test interface transaction
 *
 * Writing an internal register via the test bus:
 *   1. Present test code on TESTDIN with TESTEN=1, pulse TESTCLK high→low
 *      (this latches the address).
 *   2. Present data byte on TESTDIN with TESTEN=0, pulse TESTCLK high→low
 *      (this writes the value).
 * ----------------------------------------------------------------------- */

static void dphy_transaction(dphy_t *d, uint8_t testcode, uint8_t data)
{
    /* Step 1: address phase - assert TESTEN, put testcode on TESTDIN */
    reg_wr(d->base, DPHY_TST_CTRL1, DPHY_TESTEN | (uint32_t)testcode);
    /* Pulse TESTCLK */
    reg_wr(d->base, DPHY_TST_CTRL0, DPHY_TESTCLK);
    reg_wr(d->base, DPHY_TST_CTRL0, 0);

    /* Step 2: data phase - deassert TESTEN, put data on TESTDIN */
    reg_wr(d->base, DPHY_TST_CTRL1, (uint32_t)data);
    /* Pulse TESTCLK */
    reg_wr(d->base, DPHY_TST_CTRL0, DPHY_TESTCLK);
    reg_wr(d->base, DPHY_TST_CTRL0, 0);
}

/* -----------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------- */

void dphy_init(dphy_t *d, volatile uint32_t *base, int nlanes)
{
    d->base   = base;
    d->nlanes = nlanes;
}

void dphy_start(dphy_t *d)
{
    fprintf(stderr, "[dphy] starting D-PHY, %d lane(s)\n", d->nlanes);

    /* 1. Assert all resets */
    reg_wr(d->base, DPHY_CTRL1, 0);   /* SHUTDOWNZ=0, RSTZ=0  */

    /* 2. Set number of active data lanes (write lanes-1) */
    reg_wr(d->base, DPHY_CTRL0, DPHY_N_LANES(d->nlanes - 1));

    /* 3. Reset the test interface (TESTCLR pulse) */
    reg_wr(d->base, DPHY_TST_CTRL0, DPHY_TESTCLR);
    usleep(15);
    reg_wr(d->base, DPHY_TST_CTRL0, 0);
    usleep(15);

    /*
     * 4. Program HSFREQRANGE via the test interface.
     *    Test code 0x44 selects the HSFREQRANGE register.
     *    For 450 Mbps/lane: code 0x06, data byte = (0x06 << 1) = 0x0C.
     *    See DesignWare MIPI D-PHY Databook, "PLL Test Registers" table.
     */
    dphy_transaction(d, DPHY_TESTCODE_HSFREQRANGE, DPHY_HSFREQ_450MHZ);

    /* 5. Power up */
    reg_wr(d->base, DPHY_CTRL1, DPHY_SHUTDOWNZ);     /* shutdown released */
    usleep(15);
    reg_wr(d->base, DPHY_CTRL1, DPHY_SHUTDOWNZ | DPHY_RSTZ); /* reset released */

    fprintf(stderr, "[dphy] reset released, waiting for stop-state...\n");
}

int dphy_wait_stop(dphy_t *d, int timeout_ms)
{
    /*
     * We need to see stopstate on all active lanes.
     * DPHY_STOPSTATE bits [nlanes-1:0] should all be 1.
     */
    uint32_t lane_mask = (1u << d->nlanes) - 1u;   /* e.g. 0x3 for 2 lanes */
    int elapsed = 0;

    while (elapsed < timeout_ms) {
        uint32_t st = reg_rd(d->base, DPHY_STOPSTATE);
        if ((st & lane_mask) == lane_mask) {
            fprintf(stderr, "[dphy] stop-state confirmed (STOPSTATE=0x%08x)\n", st);
            return 0;
        }
        usleep(1000);   /* 1 ms */
        elapsed++;
    }

    fprintf(stderr, "[dphy] ERROR: stop-state timeout after %d ms "
            "(STOPSTATE=0x%08x, want bits 0x%x)\n",
            timeout_ms,
            reg_rd(d->base, DPHY_STOPSTATE),
            lane_mask);
    return -1;
}

void dphy_stop(dphy_t *d)
{
    reg_wr(d->base, DPHY_CTRL1, 0);   /* assert SHUTDOWNZ=0, RSTZ=0 */
    fprintf(stderr, "[dphy] stopped\n");
}
