/*
 * dphy.h - RP1 DW CSI-2 Host + D-PHY register definitions (QNX port)
 *
 * Ported from linux/drivers/media/platform/raspberrypi/rp1_cfe/dphy.c
 * rpi-6.12.y branch, Raspberry Pi Ltd.
 *
 * The RP1 exposes a DW CSI-2 Host controller at the DPHY base address.
 * This block contains both the CSI-2 protocol layer and the DW D-PHY
 * control/test interface — all at offsets from the same base.
 *
 * Previous version of this file had completely wrong offsets because the
 * register layout was guessed rather than read from the Linux source.
 * Specifically:
 *   - Offset 0x000 is VERSION (read-only), NOT a lane-enable CTRL0.
 *     The constant readback of 0x3132302a = DW DPHY Host HW v1.20.
 *   - Offset 0x004 is N_LANES, NOT CTRL1 (SHUTDOWNZ/RSTZ/BASEDIR).
 *   - PHY_SHUTDOWNZ and PHY_RSTZ are separate 1-bit registers at 0x040/0x044.
 *   - PHY_STOPSTATE is at 0x04C, not 0x00C.
 *   - Test interface registers are at 0x050/0x054, not 0x010/0x014.
 *   - There is no BASEDIR_PERIPHERAL — the RP1 DW DPHY is always in RX mode.
 *   - The test interface uses read-modify-write bit manipulation, not
 *     whole-register writes.
 *   - HSFREQRANGE code for 450 Mbps is 0b010110 → data byte 0x2C, not 0x0C.
 *
 * RP1 address map (from rp1.dtsi):
 *   CAM/DISP 1 (22-pin) → CSI0: DPHY base = RP1_BAR0 + 0x00114000
 *   CAM/DISP 0 (22-pin) → CSI1: DPHY base = RP1_BAR0 + 0x0012C000
 */
#ifndef DPHY_H
#define DPHY_H

#include <stdint.h>

/* -----------------------------------------------------------------------
 * DW CSI-2 Host register offsets (byte offsets from DPHY/host base)
 * ----------------------------------------------------------------------- */

#define DPHY_VERSION        0x000   /* HW IP version (read-only)              */
#define DPHY_N_LANES        0x004   /* Active data lanes: write (nlanes - 1)  */
#define DPHY_RESETN         0x008   /* Host soft reset: 0=reset, ~0=run       */
/* 0x00C–0x03F: reserved in DW CSI-2 Host */
#define DPHY_PHY_SHUTDOWNZ  0x040   /* D-PHY shutdown: 0=off, 1=active        */
#define DPHY_PHY_RSTZ       0x044   /* D-PHY reset:    0=reset, 1=active      */
#define DPHY_PHY_RX         0x048   /* PHY receive status (HS active, ULPS)   */
#define DPHY_PHY_STOPSTATE  0x04C   /* Data lane stop state: bits[nlanes-1:0] */
#define DPHY_TST_CTRL0      0x050   /* Test interface control 0               */
#define DPHY_TST_CTRL1      0x054   /* Test interface control 1               */
#define DPHY_PHY2_TST_CTRL0 0x058   /* PHY2 test interface control 0          */
#define DPHY_PHY2_TST_CTRL1 0x05C   /* PHY2 test interface control 1          */

/* DPHY_TST_CTRL0 bit fields (read-modify-write — do not write whole register) */
#define TST_CTRL0_TESTCLR   (1u << 0)   /* Pulse high to reset test interface */
#define TST_CTRL0_TESTCLK   (1u << 1)   /* Test clock: rising edge latches addr/data */

/* DPHY_TST_CTRL1 bit fields (read-modify-write) */
#define TST_CTRL1_TESTDIN_MASK  0x000000FFu   /* Test data in  [7:0]  */
#define TST_CTRL1_TESTDOUT_MASK 0x0000FF00u   /* Test data out [15:8] */
#define TST_CTRL1_TESTEN        (1u << 16)    /* 1 = address phase, 0 = data phase */

/* -----------------------------------------------------------------------
 * HSFREQRANGE lookup — test interface address 0x44 (DPHY_HS_RX_CTRL_LANE0)
 *
 * Source: DW DPHY databook Table 5-1 and rp1_cfe/dphy.c dphy_set_hsfreqrange()
 *
 * For 450 Mbps/lane (IMX708 default):
 *   450 Mbps falls in the range (449, 499] → code = 0b010110 = 0x16
 *   Data byte written = code << 1 = 0x2C
 *
 * Incorrect previous value was 0x0C (code 0x06, valid only for ≤449 Mbps).
 * ----------------------------------------------------------------------- */
#define DPHY_TESTCODE_HSFREQRANGE   0x44u
#define DPHY_HSFREQ_450MHZ          0x2Cu   /* (0b010110) << 1, for 450 Mbps/lane */

/* -----------------------------------------------------------------------
 * Context structure
 * ----------------------------------------------------------------------- */

typedef struct {
    volatile uint32_t *base;   /* mmap'd DW CSI-2 Host / DPHY base */
    int                nlanes; /* number of active data lanes (1..4)  */
} dphy_t;

/* -----------------------------------------------------------------------
 * API
 * ----------------------------------------------------------------------- */

/*
 * dphy_init - store base address and lane count (no hardware access).
 */
void dphy_init(dphy_t *d, volatile uint32_t *base, int nlanes);

/*
 * dphy_start - full DW CSI-2 Host + D-PHY bring-up sequence.
 *   Matches Linux rp1_cfe dphy_start() exactly.
 *   After return, call dphy_wait_stop() to confirm LP-11 stop state.
 */
void dphy_start(dphy_t *d);

/*
 * dphy_wait_stop - poll PHY_STOPSTATE until all lanes reach LP-11.
 *   Returns 0 on success, -1 on timeout.
 *   timeout_ms: maximum wait in milliseconds.
 */
int dphy_wait_stop(dphy_t *d, int timeout_ms);

/*
 * dphy_stop - no-op (matches Linux: resetting mid-stream corrupts IDI bus).
 */
void dphy_stop(dphy_t *d);

#endif /* DPHY_H */
