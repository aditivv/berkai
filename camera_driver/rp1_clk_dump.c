/*
 * rp1_clk_dump.c - QNX diagnostic: dump RP1 register regions (clocks, GPIO/pinmux)
 *
 * Standalone tool, independent of camera_resmgr.  It maps a region of the RP1
 * PCIe BAR0 and prints every 32-bit register as "offset: value", so we can see
 * what QNX has (and has not) set up for the camera.
 *
 * Why this exists:
 *   The IMX708 answers on I2C but never streams.  Strongest hypothesis: QNX
 *   never enables the 24 MHz camera master clock (MCLK / XCLK) that the RP1
 *   clock generator must produce and route to the CAM0 FPC pin.  On Linux the
 *   clk-rp1 + pinctrl-rp1 drivers do this from device tree; on QNX nothing does.
 *
 *   This tool lets us inspect the RP1 CLOCKS block (and the GPIO/pinmux bank)
 *   *from inside QNX*, with no Linux boot required, to find:
 *     1. which clock generator output is (or should be) producing 24 MHz, and
 *     2. which GPIO function-select routes it onto the camera clock pin.
 *
 * Regions of interest (offsets within RP1 BAR0):
 *   0x018000  CLOCKS block (clock generators / PL* dividers)  <-- default
 *   The GPIO / pinctrl bank offset is board/SoC specific; pass it explicitly
 *   once identified, e.g.:  rp1_clk_dump 0x0d0000 0x4000
 *
 * IMPORTANT: This is a raw dumper on purpose.  Rather than trust hard-coded
 * register names (which are easy to get wrong), it prints raw offset:value so
 * we can correlate against the RP1 peripheral datasheet / clk-rp1.c.  A few
 * light, clearly-marked heuristics flag likely-enabled clocks to speed triage.
 *
 * Build:  make rp1_clk_dump
 * Run:    ./rp1_clk_dump | tee clk_dump.txt           # default CLOCKS block
 *         ./rp1_clk_dump 0x18000 0x11000              # explicit base + size
 *         ./rp1_clk_dump -a 0x18000 0x2000            # -a = print ALL regs
 *
 * Requires root (PROCMGR_AID_MEM_PHYS) like the main driver.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/mman.h>

/*
 * RP1 physical base as seen by the BCM2712 CPU (fixed on all RPi5 boards).
 * Same constant the camera_resmgr uses; see that file for the derivation.
 */
#define RP1_BAR0_PHYS       0x1f00000000ULL

/* Defaults: the RP1 CLOCKS block. */
#define DEFAULT_OFFSET      0x00018000ULL
#define DEFAULT_SIZE        0x00011000u   /* ~ matches the documented block size */

/*
 * RP1 clock-generator CTRL register heuristics.
 *
 * In the RP1 CLOCKS block each clock output has a CTRL register.  From
 * clk-rp1.c the ENABLE bit is BIT(11) and the auxiliary source select sits in
 * a small field below it.  We cannot know which offsets are CTRL vs DIV vs SEL
 * without the per-clock table, so we only use this to ANNOTATE candidates:
 * any word with bit 11 set is flagged as a possible enabled-clock CTRL.
 * Treat these flags as hints, not truth.
 */
#define RP1_CLK_CTRL_ENABLE_BIT   (1u << 11)

static volatile uint32_t *rp1_map(uint64_t phys_offset, uint32_t size)
{
    uint64_t phys = RP1_BAR0_PHYS + phys_offset;
    void *va = mmap_device_memory(NULL, size,
                                  PROT_READ | PROT_WRITE | PROT_NOCACHE,
                                  MAP_SHARED,
                                  phys);
    if (va == MAP_FAILED) {
        fprintf(stderr,
                "[map] mmap_device_memory phys=0x%016llx size=0x%x failed: %s\n",
                (unsigned long long)phys, size, strerror(errno));
        fprintf(stderr, "      Are you running as root?\n");
        return NULL;
    }
    return (volatile uint32_t *)va;
}

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [-a] [offset_hex [size_hex]]\n"
        "  -a          print ALL registers (default: non-zero only)\n"
        "  offset_hex  byte offset within RP1 BAR0 (default 0x%08llx = CLOCKS)\n"
        "  size_hex    region size in bytes        (default 0x%x)\n\n"
        "Examples:\n"
        "  %s                       # CLOCKS block, non-zero regs\n"
        "  %s -a 0x18000 0x2000     # all regs in first 8 KiB of CLOCKS\n"
        "  %s 0x18000 0x11000       # full CLOCKS block, non-zero regs\n",
        prog,
        (unsigned long long)DEFAULT_OFFSET, DEFAULT_SIZE,
        prog, prog, prog);
}

int main(int argc, char *argv[])
{
    int      print_all = 0;
    uint64_t offset    = DEFAULT_OFFSET;
    uint32_t size      = DEFAULT_SIZE;

    /* ---- argument parsing -------------------------------------------- */
    int argi = 1;
    if (argi < argc && strcmp(argv[argi], "-a") == 0) {
        print_all = 1;
        argi++;
    }
    if (argi < argc && (strcmp(argv[argi], "-h") == 0 ||
                        strcmp(argv[argi], "--help") == 0)) {
        usage(argv[0]);
        return 0;
    }
    if (argi < argc) {
        offset = strtoull(argv[argi], NULL, 0);
        argi++;
    }
    if (argi < argc) {
        size = (uint32_t)strtoul(argv[argi], NULL, 0);
        argi++;
    }

    if (size == 0 || (size & 0x3u)) {
        fprintf(stderr, "size must be a non-zero multiple of 4 (got 0x%x)\n", size);
        usage(argv[0]);
        return 1;
    }

    /* ---- map and dump ------------------------------------------------ */
    printf("=== RP1 register dump ===\n");
    printf("  BAR0 phys base : 0x%016llx\n", (unsigned long long)RP1_BAR0_PHYS);
    printf("  region offset  : 0x%08llx\n", (unsigned long long)offset);
    printf("  region size    : 0x%08x (%u words)\n", size, size / 4);
    printf("  mode           : %s\n",
           print_all ? "ALL registers" : "non-zero registers only");
    printf("  absolute phys  : 0x%016llx .. 0x%016llx\n",
           (unsigned long long)(RP1_BAR0_PHYS + offset),
           (unsigned long long)(RP1_BAR0_PHYS + offset + size - 1));
    printf("\n");

    volatile uint32_t *base = rp1_map(offset, size);
    if (!base)
        return 1;

    uint32_t nwords   = size / 4;
    uint32_t nonzero  = 0;
    uint32_t enflags  = 0;

    printf("  %-12s %-12s  %s\n", "offset", "value", "notes");
    printf("  %-12s %-12s  %s\n", "------", "-----", "-----");

    for (uint32_t i = 0; i < nwords; i++) {
        uint32_t off = (uint32_t)(offset + i * 4);
        uint32_t val = base[i];

        if (val == 0xFFFFFFFFu) {
            /* All-ones usually means the block is gated/unmapped at this word. */
            if (print_all)
                printf("  0x%08x   0x%08x   (all-ones: block gated/unmapped?)\n",
                       off, val);
            continue;
        }

        if (val != 0)
            nonzero++;

        if (!print_all && val == 0)
            continue;

        /* Light heuristic annotation. */
        char note[64];
        note[0] = '\0';
        if (val & RP1_CLK_CTRL_ENABLE_BIT) {
            snprintf(note, sizeof(note), "bit11 set -> possible ENABLED clk CTRL");
            enflags++;
        }

        printf("  0x%08x   0x%08x   %s\n", off, val, note);
    }

    printf("\n");
    printf("  summary: %u/%u registers non-zero", nonzero, nwords);
    printf(", %u with bit11(ENABLE?) set\n", enflags);
    printf("\nNext: share this dump so we can map the 24 MHz cam clock output\n"
           "and its GPIO funcsel against the RP1 datasheet, then program them\n"
           "in camera_resmgr before imx708_init().\n");

    munmap((void *)base, size);
    return 0;
}
