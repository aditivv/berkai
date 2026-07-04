/*
 * gpio_peek.c — Phase 0 sanity tool for the DHT11 driver (QNX 8 / Pi 5 / RP1).
 *
 * Purpose: prove we can READ a GPIO pin level through RP1 registers before
 * writing any DHT11 protocol/timing code. The camera driver (camera_resmgr.c)
 * only ever WRITES RIO OUT/OE — the input path (RIO_IN, per-pin FUNCSEL to
 * SYS_RIO, pad input-enable + pull-up) is unverified on this board until this
 * tool passes its jumper test.
 *
 * What it does:
 *   1. mmaps IO_BANK0 (pinmux), SYS_RIO0 (register GPIO) and PADS_BANK0.
 *   2. Dumps the pin's CTRL / STATUS / PAD registers (before + after config).
 *   3. Configures the pin: FUNCSEL=SYS_RIO(5), input (RIO OE bit cleared),
 *      pad IE=1 + pull-up on, output-disable off.
 *   4. Polls ~10x/s, printing the level from BOTH candidate input paths —
 *      SYS_RIO IN (+0x08) and IO_BANK0 STATUS — so we can see which tracks
 *      the pin if the documented offsets are wrong on this silicon.
 *   5. Restores the pin's original CTRL/PAD registers on Ctrl-C.
 *
 * PASS CRITERION (Phase 0 gate): touch a jumper from the pin to 3.3V then GND
 * ~20 times; the printed level must track every touch. With the DHT11 wired
 * and powered (pull-up present), the idle level must read HIGH (1).
 *
 * Build (on the Pi):   cd ~/berkai/dht11_driver && make
 * Run (as root):       su   (password: root)
 *                      ./gpio_peek --selftest # automated loopback pass/fail
 *                      ./gpio_peek            # manual watch, GPIO17 default
 *                      ./gpio_peek --pin 23   # any bank-0 pin (GPIO0..27)
 *
 * Register map (RP1 datasheet + Linux pinctrl-rp1.c; offsets from RP1 BAR0,
 * same base the camera driver uses):
 *   IO_BANK0    0x0D0000   GPIOx: STATUS @ 8*x, CTRL @ 8*x+4
 *   SYS_RIO0    0x0E0000   OUT @ 0x00, OE @ 0x04, IN @ 0x08   (bit = GPIO#)
 *   PADS_BANK0  0x0F0000   VOLTAGE_SELECT @ 0x00, GPIOx pad @ 0x04 + 4*x
 */

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>

#define RP1_BAR0_PHYS        0x1f00000000ULL   /* same as camera_resmgr.c */

#define RP1_IO_BANK0_OFFSET  0x000D0000ULL
#define RP1_SYS_RIO0_OFFSET  0x000E0000ULL
#define RP1_PADS_BANK0_OFFSET 0x000F0000ULL
#define RP1_BLOCK_SIZE       0x1000u

/* IO_BANK0 per-pin registers */
#define GPIO_STATUS(pin)     (((pin) * 8u) >> 2)        /* word index */
#define GPIO_CTRL(pin)       (((pin) * 8u + 4u) >> 2)
#define CTRL_FUNCSEL_MASK    0x1fu
#define FUNCSEL_SYS_RIO      5u                          /* "funcsel 5/RIO" —
                                                            camera_resmgr.c */

/* SYS_RIO registers (word indices) */
#define RIO_OUT              (0x00u >> 2)
#define RIO_OE               (0x04u >> 2)
#define RIO_IN               (0x08u >> 2)

/* PADS_BANK0: pad register word index for a pin */
#define PAD_REG(pin)         ((0x04u + 4u * (pin)) >> 2)
#define PAD_SLEWFAST         (1u << 0)
#define PAD_SCHMITT          (1u << 1)
#define PAD_PDE              (1u << 2)   /* pull-down enable */
#define PAD_PUE              (1u << 3)   /* pull-up enable   */
#define PAD_IE               (1u << 6)   /* input enable     */
#define PAD_OD               (1u << 7)   /* output disable   */

static volatile uint32_t *io_bank0;
static volatile uint32_t *sys_rio0;
static volatile uint32_t *pads_bank0;

static int      g_pin = 17;              /* DHT11 DATA — header pin 11 */
static uint32_t g_orig_ctrl, g_orig_pad;
static volatile sig_atomic_t g_stop = 0;

static void on_sigint(int sig)
{
    (void)sig;
    g_stop = 1;
}

/* Same technique as camera_resmgr.c rp1_map(): PROT_NOCACHE is essential —
 * a cached mapping would return stale pin levels. */
static volatile uint32_t *rp1_map(uint64_t phys_offset, uint32_t size)
{
    uint64_t phys = RP1_BAR0_PHYS + phys_offset;
    void *va = mmap_device_memory(NULL, size,
                                  PROT_READ | PROT_WRITE | PROT_NOCACHE,
                                  MAP_SHARED, phys);
    if (va == MAP_FAILED) {
        fprintf(stderr,
                "[mmap] failed at phys=0x%016llx size=%u: %s\n"
                "       (are you root? mmap_device_memory needs "
                "PROCMGR_AID_MEM_PHYS — run under 'su')\n",
                (unsigned long long)phys, size, strerror(errno));
        return NULL;
    }
    return (volatile uint32_t *)va;
}

static void dump_pin_regs(const char *label)
{
    printf("[%s] GPIO%d: CTRL=0x%08x (funcsel=%u)  STATUS=0x%08x  "
           "PAD=0x%08x (IE=%u PUE=%u PDE=%u OD=%u)  RIO: OUT=%u OE=%u IN=%u\n",
           label, g_pin,
           io_bank0[GPIO_CTRL(g_pin)],
           io_bank0[GPIO_CTRL(g_pin)] & CTRL_FUNCSEL_MASK,
           io_bank0[GPIO_STATUS(g_pin)],
           pads_bank0[PAD_REG(g_pin)],
           !!(pads_bank0[PAD_REG(g_pin)] & PAD_IE),
           !!(pads_bank0[PAD_REG(g_pin)] & PAD_PUE),
           !!(pads_bank0[PAD_REG(g_pin)] & PAD_PDE),
           !!(pads_bank0[PAD_REG(g_pin)] & PAD_OD),
           !!(sys_rio0[RIO_OUT] & (1u << g_pin)),
           !!(sys_rio0[RIO_OE]  & (1u << g_pin)),
           !!(sys_rio0[RIO_IN]  & (1u << g_pin)));
}

static void configure_pin_as_input(void)
{
    uint32_t pad;

    g_orig_ctrl = io_bank0[GPIO_CTRL(g_pin)];
    g_orig_pad  = pads_bank0[PAD_REG(g_pin)];

    /* RIO input: make sure we're not driving the line. */
    sys_rio0[RIO_OE] &= ~(1u << g_pin);

    /* Pad: input-enable + pull-up, no pull-down, output driver allowed
     * (OD=0 is irrelevant while OE is clear), keep schmitt on for a clean
     * digital read of a slow-edged line. */
    pad = g_orig_pad;
    pad |=  PAD_IE | PAD_PUE | PAD_SCHMITT;
    pad &= ~(PAD_PDE | PAD_OD);
    pads_bank0[PAD_REG(g_pin)] = pad;

    /* Mux the pin to SYS_RIO so the RIO block owns it. */
    io_bank0[GPIO_CTRL(g_pin)] =
        (g_orig_ctrl & ~CTRL_FUNCSEL_MASK) | FUNCSEL_SYS_RIO;
}

static void restore_pin(void)
{
    io_bank0[GPIO_CTRL(g_pin)]  = g_orig_ctrl;
    pads_bank0[PAD_REG(g_pin)]  = g_orig_pad;
    printf("\n[restore] GPIO%d CTRL/PAD restored to original values "
           "(0x%08x / 0x%08x)\n", g_pin, g_orig_ctrl, g_orig_pad);
}

/*
 * Loopback self-test: the pin drives itself LOW and watches its own input.
 * With IE set, the input buffer samples the physical pad, so this proves the
 * full read path tracks the real pin — no jumper wire needed. Only ever
 * drives LOW then releases (the pull-up restores HIGH): that is exactly the
 * DHT11 start-signal pattern, so there is no drive-high contention risk with
 * the sensor.
 *
 * Returns 0 on pass (all 20 drive/release cycles read back correctly).
 */
static int run_selftest(void)
{
    int fails = 0, cyc;

    printf("\n[selftest] 20x: drive LOW 50ms -> expect IN=0; "
           "release -> pull-up must restore IN=1\n");
    for (cyc = 0; cyc < 20; cyc++) {
        int in_low, in_rel;

        sys_rio0[RIO_OUT] &= ~(1u << g_pin);   /* prepare LOW */
        sys_rio0[RIO_OE]  |=  (1u << g_pin);   /* drive */
        usleep(50 * 1000);
        in_low = !!(sys_rio0[RIO_IN] & (1u << g_pin));

        sys_rio0[RIO_OE]  &= ~(1u << g_pin);   /* release: input again */
        usleep(50 * 1000);
        in_rel = !!(sys_rio0[RIO_IN] & (1u << g_pin));

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

int main(int argc, char **argv)
{
    int i;

    int selftest = 0;

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
    if (g_pin < 0 || g_pin > 27) {
        fprintf(stderr, "pin %d out of range: this tool only maps bank 0 "
                "(GPIO0..27)\n", g_pin);
        return 2;
    }

    io_bank0   = rp1_map(RP1_IO_BANK0_OFFSET,   RP1_BLOCK_SIZE);
    sys_rio0   = rp1_map(RP1_SYS_RIO0_OFFSET,   RP1_BLOCK_SIZE);
    pads_bank0 = rp1_map(RP1_PADS_BANK0_OFFSET, RP1_BLOCK_SIZE);
    if (!io_bank0 || !sys_rio0 || !pads_bank0)
        return 1;

    printf("gpio_peek: RP1 @ 0x%016llx, watching GPIO%d "
           "(DHT11 DATA expected on GPIO17 / header pin 11)\n",
           (unsigned long long)RP1_BAR0_PHYS, g_pin);

    dump_pin_regs("before");
    configure_pin_as_input();
    dump_pin_regs("after ");

    if (selftest) {
        int rc = run_selftest();
        restore_pin();
        return rc;
    }

    signal(SIGINT, on_sigint);
    printf("\nPolling ~10x/s. Touch a jumper from the pin to 3.3V and GND;\n"
           "both level columns must track every touch. Ctrl-C to stop.\n\n");

    {
        int last_rio = -1, last_status = -1;
        unsigned poll = 0, changes = 0;

        while (!g_stop) {
            /* Two independent input paths: if they disagree, or neither
             * tracks the jumper, an offset/funcsel assumption is wrong. */
            int rio_in  = !!(sys_rio0[RIO_IN] & (1u << g_pin));
            uint32_t st = io_bank0[GPIO_STATUS(g_pin)];

            if (rio_in != last_rio || (int)st != last_status) {
                changes += (last_rio >= 0);
                printf("[%6u] RIO_IN=%d   STATUS=0x%08x   (changes so far: %u)\n",
                       poll, rio_in, st, changes);
                last_rio = rio_in;
                last_status = (int)st;
            } else if (poll % 50 == 0) {   /* 5 s heartbeat while idle */
                printf("[%6u] RIO_IN=%d   STATUS=0x%08x   (idle)\n",
                       poll, rio_in, st);
            }
            poll++;
            usleep(100 * 1000);
        }
    }

    restore_pin();
    return 0;
}
