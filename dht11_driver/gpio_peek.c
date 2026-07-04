/*
 * gpio_peek.c — GPIO sanity/diagnostic tool for the DHT11 driver
 * (QNX 8 / Pi 5 / RP1). Phase 0 gate of the DHT11 plan; since Phase 1 it is
 * a thin client of rp1_gpio.c, so its --selftest also regression-tests that
 * module.
 *
 * Modes:
 *   --selftest   Loopback: the pin drives itself LOW and watches its own
 *                input, 20 cycles (driven-low must read 0, released must
 *                read 1 through the pull-up). Proves the input path tracks
 *                the physical pad and the output drive works — no jumper
 *                wire needed. Only drives low-and-release (the DHT11
 *                start-signal pattern), so no contention risk. Exit 0=pass.
 *   (default)    Watch: poll ~10x/s printing the level; jumper the pin to
 *                GND to see it track. Ctrl-C restores the pin and exits.
 *
 * PASS RECORD: Phase 0 ran 20/20 selftest cycles on GPIO17 (2026-07-04) —
 * funcsel 31->5, pad IE/PUE writes read back, idle HIGH via sensor pull-up.
 *
 * Build (on the Pi):   cd ~/berkai/dht11_driver && make
 * Run (as root):       su   (password: root)
 *                      ./gpio_peek --selftest # automated pass/fail
 *                      ./gpio_peek            # manual watch, GPIO17 default
 *                      ./gpio_peek --pin 23   # any bank-0 pin (GPIO0..27)
 */

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "rp1_gpio.h"

static int g_pin = 17;               /* DHT11 DATA — header pin 11 */
static volatile sig_atomic_t g_stop = 0;

static void on_sigint(int sig)
{
    (void)sig;
    g_stop = 1;
}

static void dump_pin_regs(const char *label)
{
    uint32_t pad = rp1_gpio_pad_reg(g_pin);
    printf("[%s] GPIO%d: CTRL=0x%08x (funcsel=%u)  STATUS=0x%08x  "
           "PAD=0x%08x (IE=%u PUE=%u PDE=%u OD=%u)  RIO IN=%d\n",
           label, g_pin,
           rp1_gpio_ctrl_reg(g_pin), rp1_gpio_ctrl_reg(g_pin) & 0x1fu,
           rp1_gpio_status_reg(g_pin),
           pad, !!(pad & (1u << 6)), !!(pad & (1u << 3)),
           !!(pad & (1u << 2)), !!(pad & (1u << 7)),
           rp1_gpio_read(g_pin));
}

static int run_selftest(void)
{
    int fails = 0, cyc;

    printf("\n[selftest] 20x: drive LOW 50ms -> expect IN=0; "
           "release -> pull-up must restore IN=1\n");
    for (cyc = 0; cyc < 20; cyc++) {
        int in_low, in_rel;

        rp1_gpio_set_output(g_pin, 0);
        usleep(50 * 1000);
        in_low = rp1_gpio_read(g_pin);

        rp1_gpio_set_input(g_pin);
        usleep(50 * 1000);
        in_rel = rp1_gpio_read(g_pin);

        if (in_low != 0 || in_rel != 1) {
            fails++;
            printf("[selftest] cycle %2d: driven-low reads %d (want 0), "
                   "released reads %d (want 1)  <-- FAIL\n",
                   cyc, in_low, in_rel);
        }
    }
    if (fails == 0) {
        printf("[selftest] PASS: 20/20 cycles — input path tracks the "
               "physical pin, output drive works, pull-up present.\n");
    } else {
        printf("[selftest] FAIL: %d/20 cycles wrong — RIO_IN offset, pad "
               "config, or wiring/pull-up is suspect.\n", fails);
    }
    return fails ? 1 : 0;
}

static void run_watch(void)
{
    int last = -1;
    unsigned poll = 0, changes = 0;

    signal(SIGINT, on_sigint);
    printf("\nPolling ~10x/s. Touch a jumper from the pin to GND;\n"
           "the level must track every touch. Ctrl-C to stop.\n\n");

    while (!g_stop) {
        int level = rp1_gpio_read(g_pin);
        if (level != last) {
            changes += (last >= 0);
            printf("[%6u] IN=%d   STATUS=0x%08x   (changes so far: %u)\n",
                   poll, level, rp1_gpio_status_reg(g_pin), changes);
            last = level;
        } else if (poll % 50 == 0) {   /* 5 s heartbeat while idle */
            printf("[%6u] IN=%d   (idle)\n", poll, level);
        }
        poll++;
        usleep(100 * 1000);
    }
}

int main(int argc, char **argv)
{
    int i, selftest = 0, rc = 0;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--pin") == 0 && i + 1 < argc) {
            g_pin = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--selftest") == 0) {
            selftest = 1;
        } else {
            fprintf(stderr, "usage: %s [--pin N] [--selftest]   (bank-0 pin, "
                    "GPIO0..27; default 17)\n", argv[0]);
            return 2;
        }
    }
    if (g_pin < 0 || g_pin >= RP1_GPIO_BANK0_NPINS) {
        fprintf(stderr, "pin %d out of range: bank 0 is GPIO0..27\n", g_pin);
        return 2;
    }

    if (rp1_gpio_map() != 0)
        return 1;

    printf("gpio_peek: watching GPIO%d "
           "(DHT11 DATA expected on GPIO17 / header pin 11)\n", g_pin);

    dump_pin_regs("before");
    rp1_gpio_claim(g_pin, RP1_PULL_UP);
    dump_pin_regs("after ");

    if (selftest)
        rc = run_selftest();
    else
        run_watch();

    rp1_gpio_restore(g_pin);
    printf("[restore] GPIO%d CTRL/PAD restored\n", g_pin);
    return rc;
}
