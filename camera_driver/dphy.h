/*
 * dphy.h - DesignWare MIPI D-PHY register definitions for RP1 (QNX port)
 *
 * Ported from linux/drivers/media/platform/raspberrypi/rp1_cfe/dphy.h
 * rpi-6.12.y branch, Raspberry Pi Ltd.
 *
 * RP1 address map (from rp1.dtsi):
 *   CSI0 DPHY base = RP1_BAR0 + 0x00C0B700, size 0x200
 *   CSI1 DPHY base = RP1_BAR0 + 0x00C0D700, size 0x200
 */
#ifndef DPHY_H
#define DPHY_H

#include <stdint.h>

/* -----------------------------------------------------------------------
 * Register offsets (byte offsets from DPHY base)
 * ----------------------------------------------------------------------- */

/* Number of active data lanes (write N_LANES-1) */
#define DPHY_CTRL0              0x00
#define   DPHY_N_LANES(n)       ((n) & 0x3)        /* bits [1:0] */

/* Enable / soft-reset */
#define DPHY_CTRL1              0x04
#define   DPHY_SHUTDOWNZ        (1u << 0)
#define   DPHY_RSTZ             (1u << 1)

/* Receive status - read to confirm lanes are in stop state */
#define DPHY_RX                 0x08
#define   DPHY_RX_ULPSACTIVENOT_CLK    (1u << 8)

/* Stop state - all data lanes must show stopstate before streaming */
#define DPHY_STOPSTATE          0x0C

/* DesignWare test interface */
#define DPHY_TST_CTRL0          0x10
#define   DPHY_TESTCLK          (1u << 1)
#define   DPHY_TESTCLR          (1u << 0)

#define DPHY_TST_CTRL1          0x14
#define   DPHY_TESTEN           (1u << 16)
#define   DPHY_TESTDIN_MASK     0xFF
#define   DPHY_TESTDOUT_SHIFT   8

/*
 * DesignWare test codes (internal PLL/PHY configuration):
 *   0x44 = HSFREQRANGE register address
 *
 * HSFREQRANGE values (bits [6:1] of the test data byte):
 *   Range 430-450 Mbps/lane  → code 0x06  → write (0x06 << 1) = 0x0C
 *   Range 450-470 Mbps/lane  → code 0x07  → write (0x07 << 1) = 0x0E
 *
 * IMX708 nominal link rate = 450 Mbps/lane → use 0x06 → data byte = 0x0C
 */
#define DPHY_TESTCODE_HSFREQRANGE   0x44
#define DPHY_HSFREQ_450MHZ          0x0C    /* (0x06 << 1) */

/* -----------------------------------------------------------------------
 * Context structure
 * ----------------------------------------------------------------------- */

typedef struct {
    volatile uint32_t *base;   /* mmap'd DPHY register base */
    int                nlanes; /* number of data lanes (1 or 2) */
} dphy_t;

/* -----------------------------------------------------------------------
 * API
 * ----------------------------------------------------------------------- */

/*
 * dphy_init - store base address and lane count
 *   base   : mmap_device_memory result for DPHY registers
 *   nlanes : 1 or 2 data lanes
 */
void dphy_init(dphy_t *d, volatile uint32_t *base, int nlanes);

/*
 * dphy_start - run the full DesignWare D-PHY bring-up sequence.
 *   After return, call dphy_wait_stop() to confirm stop-state.
 */
void dphy_start(dphy_t *d);

/*
 * dphy_wait_stop - poll DPHY_STOPSTATE until all lanes reach stop state.
 *   Returns 0 on success, -1 on timeout (ms).
 */
int dphy_wait_stop(dphy_t *d, int timeout_ms);

/*
 * dphy_stop - assert resets (call when tearing down)
 */
void dphy_stop(dphy_t *d);

#endif /* DPHY_H */
