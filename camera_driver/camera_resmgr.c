/*
 * camera_resmgr.c - QNX Resource Manager for Raspberry Pi Camera Module 3
 *
 * Exposes /dev/video0 and writes raw RAW10 frames to it on each read().
 *
 * Hardware path:
 *   IMX708 sensor → CSI-2 (2-lane, 450 Mbps/lane) → RP1 DPHY → RP1 CSI2 DMA
 *
 * Implementation steps (all in this file):
 *   Step 1: Map RP1 PCIe BAR, bring up DPHY and CSI-2 RX.
 *   Step 2: Init IMX708 sensor via I2C, verify chip ID, stream-on.
 *   Step 3: Alloc DMA buffer, start CSI-2 channel, poll for frames.
 *   Step 4: Wire interrupt, expose /dev/video0 via QNX resmgr.
 *
 * Build:  make  (see Makefile)
 * Deploy: scp camera_resmgr qnxuser@<pi-ip>:/usr/sbin/
 * Run:    /usr/sbin/camera_resmgr &
 * Read:   dd if=/dev/video0 of=frame.raw bs=3732480 count=1
 *         # then on PC: ffmpeg -f rawvideo -pixel_format bayer_rggb10 \
 *         #   -video_size 2304x1296 -i frame.raw frame.png
 *
 * RP1 register offsets within BAR0 — camera connected to the CD0 connector:
 *   The sensor answers on /dev/i2c6 (gpio38/39 muxed to i2c, confirmed by
 *   GPIO funcsel dump).  Per the Pi5 device tree, the CD0 connector bundles
 *   i2c6 + csi0 + cam0_reg(gpio34) as ONE connector, so the sensor's MIPI
 *   lanes terminate at the CSI0 hardware block — NOT CSI1.
 *   (Earlier code used CSI1 based on an "inverted naming" assumption; the
 *    register/GPIO dumps disproved it — see change log below.)
 *   MIPI CFG  : RP1_BAR0 + 0x00120000  ← MUST write SEL_CSI=1 here first
 *   CSI0 DMA  : RP1_BAR0 + 0x00110000
 *   CSI0 DPHY : RP1_BAR0 + 0x00114000  (DW CSI-2 Host + D-PHY, not raw DPHY)
 *
 * All three blocks must be mapped; MIPI_CFG must be programmed first or
 * the CSI2/DPHY blocks are gated (all reads return 0xFFFFFFFF).
 */

/* =========================================================================
 * Includes
 * ========================================================================= */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>

/* QNX-specific */
#include <sys/mman.h>
#include <sys/neutrino.h>
#include <sys/iofunc.h>
#include <sys/dispatch.h>
#include <hw/i2c.h>

#include "dphy.h"
#include "csi2.h"
#include "imx708_regs.h"

/* =========================================================================
 * Configuration constants
 * ========================================================================= */

/*
 * RP1 physical base address as seen by the BCM2712 CPU.
 *
 * The BCM2712 firmware maps PCIe1 (internal link to RP1) via:
 *   ranges = <0x02000000 0x00 0xc0000000  0x1f 0x00000000  0x00 0x40000000>
 * meaning RP1 BAR0 (PCIe addr 0xc0000000) sits at CPU phys 0x1f00000000.
 * This is fixed on ALL RPi5 boards — no need for the PCI library.
 *
 * Verify by booting Linux on the same Pi and running:
 *   sudo cat /proc/iomem | grep -i rp1
 * You should see something like: 1f00000000-1f3fffffff : rp1
 */
#define RP1_BAR0_PHYS           0x1f00000000ULL

/*
 * Register offsets within RP1 BAR0.
 * Source: rp1.dtsi, Linux rpi-6.12.y (reg-names "csi2" and "dphy").
 *
 * If CSI2_STATUS reads 0xFFFFFFFF, the offset is wrong.
 * Cross-check with: sudo cat /sys/bus/platform/devices/ADDR:csi0/resource on Linux.
 */
/*
 * CD0 connector → CSI0 hardware block in RP1.
 *
 * Evidence (from rp1_clk_dump GPIO bank decode + Pi5 device tree):
 *   - i2c6 (gpio38/39) is muxed to the i2c function (funcsel 3) and the
 *     sensor ACKs there; the CD1 pins gpio40/41 are idle (funcsel 5/RIO).
 *     => the camera is physically on the CD0 connector.
 *   - Pi5 DT pairs CD0 = i2c6 = csi0 = cam0_reg(gpio34).  cam0_reg is already
 *     driven high (RIO bank2 OUT/OE bit0 = 1) so analog power is on.
 *
 * CSI0 offsets from rp1.dtsi csi@110000 reg[0..2]:
 *   reg[0] = <0xc0 0x40110000 ...>  → BAR0 + 0x110000  (DMA)
 *   reg[1] = <0xc0 0x40114000 ...>  → BAR0 + 0x114000  (DPHY / DW CSI-2 Host)
 *   reg[2] = <0xc0 0x40120000 ...>  → BAR0 + 0x120000  (MIPI CFG)
 *
 * NOTE: macro names keep the RP1_CSI0_ prefix; the values below are now the
 * real CSI0 offsets (previously they held CSI1 offsets by mistake).
 */
#define RP1_CSI0_MIPICFG_OFFSET 0x00120000ULL  /* MIPI CFG (CSI0 / CD0 connector)  */
#define RP1_CSI0_MIPICFG_SIZE   0x100u
#define RP1_CSI0_DMA_OFFSET     0x00110000ULL  /* CSI-2 DMA registers (CSI0)        */
#define RP1_CSI0_DMA_SIZE       0x200u
#define RP1_CSI0_DPHY_OFFSET    0x00114000ULL  /* D-PHY registers (CSI0)            */
#define RP1_CSI0_DPHY_SIZE      0x200u

/*
 * MIPI CFG register to select CSI vs DSI mode.
 * Must write MIPICFG_CFG_SEL_CSI=1 before any CSI2/DPHY access.
 * Source: cfe.c cfe_start_streaming() line 1158
 */
#define MIPICFG_CFG_REG         0x004u         /* offset within MIPI_CFG     */
#define MIPICFG_CFG_SEL_CSI     (1u << 0)      /* 1=CSI, 0=DSI               */

/* CSI-2 channel used for image capture (VC0) */
#define CAPTURE_CHANNEL         0
#define CAPTURE_VC              0
#define CAPTURE_DT              CSI2_DT_RAW10

/* Frame geometry: 2304x1296 2x2-binned mode (see imx708_regs.h) */
#define FRAME_WIDTH             IMX708_2X2_WIDTH
#define FRAME_HEIGHT            IMX708_2X2_HEIGHT
#define FRAME_BYTES             IMX708_2X2_FRAME_BYTES   /* 3,732,480 */

/* DMA stop-state timeout */
#define DPHY_TIMEOUT_MS         500

/* Number of frames to skip before exposing data (sensor warm-up) */
#define FRAME_SKIP_COUNT        3

/* /dev node name */
#define VIDEO_DEV_PATH          "/dev/video0"

/* =========================================================================
 * Step 1 helpers — direct physical memory mapping (no PCI library needed)
 * ========================================================================= */

/*
 * rp1_map - mmap a sub-region of RP1's address space.
 *
 * Uses mmap_device_memory() with the hardcoded RP1 physical base.
 * Requires the process to have the PROCMGR_AID_MEM_PHYS ability,
 * which is granted when running as root (default for QNX drivers).
 *
 * phys_offset : byte offset from RP1_BAR0_PHYS
 * size        : region size in bytes
 */
static volatile uint32_t *rp1_map(uint64_t phys_offset, uint32_t size)
{
    uint64_t phys = RP1_BAR0_PHYS + phys_offset;
    void *va = mmap_device_memory(NULL, size,
                                  PROT_READ | PROT_WRITE | PROT_NOCACHE,
                                  MAP_SHARED,
                                  phys);
    if (va == MAP_FAILED) {
        fprintf(stderr, "[mmap] mmap_device_memory phys=0x%016llx size=%u "
                "failed: %s\n",
                (unsigned long long)phys, size, strerror(errno));
        fprintf(stderr, "       Are you running as root?\n");
        return NULL;
    }
    fprintf(stderr, "[mmap] mapped phys=0x%016llx size=0x%x → virt=%p\n",
            (unsigned long long)phys, size, va);
    return (volatile uint32_t *)va;
}

/* =========================================================================
 * RP1 CLOCKS block — enable the CSI0 MIPI config clock (RP1_CLK_MIPI0_CFG)
 *
 * QNX boot leaves this clock disabled (confirmed by rp1_clk_dump:
 * CLK_MIPI0_CFG_CTRL @ 0x180c4 reads 0).  The DW CSI-2 Host APB registers are
 * still readable on the always-on system clock, but the D-PHY *functional*
 * logic — including LP-11 stop-state detection that drives PHY_STOPSTATE —
 * runs on this config clock.  With it off, STOPSTATE can never assert no
 * matter what the sensor transmits.  Linux rp1-cfe enables it during CSI
 * bring-up; we must do the same.
 *
 * Register map + sequence from Linux clk-rp1.c:
 *   CLOCKS block base   = RP1_BAR0 + 0x18000
 *   CLK_MIPI0_CFG_CTRL    = +0x0c4   (ENABLE = BIT(11), AUXSRC = bits[9:5])
 *   CLK_MIPI0_CFG_DIV_INT = +0x0c8   (integer divider)
 *   parent = xosc (50 MHz), target 25 MHz  → div_int = 2
 *   enable = set AUXSRC=0 (xosc) + CLK_CTRL_ENABLE
 * ========================================================================= */
#define RP1_CLOCKS_OFFSET       0x00018000ULL
#define RP1_CLOCKS_SIZE         0x1000u
#define CLK_MIPI0_CFG_CTRL      0x0C4u      /* offset within CLOCKS block */
#define CLK_MIPI0_CFG_DIV_INT   0x0C8u
#define CLK_CTRL_ENABLE         (1u << 11)
#define CLK_MIPI0_CFG_DIV       2u          /* 50 MHz xosc / 2 = 25 MHz   */

static int rp1_enable_mipi0_cfg_clock(void)
{
    volatile uint32_t *clk = rp1_map(RP1_CLOCKS_OFFSET, RP1_CLOCKS_SIZE);
    if (!clk)
        return -1;

    /* Integer divider: xosc(50 MHz) / 2 = 25 MHz (matches DT 25000000). */
    clk[CLK_MIPI0_CFG_DIV_INT >> 2] = CLK_MIPI0_CFG_DIV;

    /* CTRL: AUXSRC field = 0 selects xosc; set ENABLE (bit 11).
     * Matches rp1_clock_set_parent(xosc) + rp1_clock_on() in clk-rp1.c.
     * (The hardware sets the high status bits, e.g. 0x10000000, itself.) */
    clk[CLK_MIPI0_CFG_CTRL >> 2] = CLK_CTRL_ENABLE;

    uint32_t ctrl = clk[CLK_MIPI0_CFG_CTRL >> 2];
    uint32_t div  = clk[CLK_MIPI0_CFG_DIV_INT >> 2];
    fprintf(stderr, "[clk] MIPI0_CFG: CTRL=0x%08x DIV_INT=0x%08x "
            "(want bit11 set, div=2 → 25MHz)\n", ctrl, div);

    if (!(ctrl & CLK_CTRL_ENABLE)) {
        fprintf(stderr, "[clk] WARNING: ENABLE bit did not stick — "
                "MIPI0_CFG clock may still be off\n");
        return -1;
    }
    return 0;
}

/* =========================================================================
 * Camera power-on reset (cam0_reg regulator via RP1 GPIO34)
 *
 * On Pi5 the IMX708's 2.7V analog supply is gated by cam0_reg, enabled by
 * driving RP1 GPIO34 high (DT: cam0_reg gpio = <&rp1_gpio 34>).  The boot
 * firmware leaves it high, so QNX never resets the sensor — it gets
 * configured in whatever state it booted into.  Linux power-cycles the
 * regulator (imx708_power_on/off) on every bring-up.  Replicate that:
 * drive GPIO34 low, wait, drive high, wait the regulator startup delay,
 * giving the sensor a clean power-on reset before I2C config.
 *
 * GPIO34 = RIO bank2 (0xe8000), bit 0 (confirmed by rp1_clk_dump).
 * RIO register layout: OUT=+0x00, OE=+0x04.
 * Delays from the imx708 overlay: off-on = 30ms, startup = 70ms.
 * ========================================================================= */
#define RP1_RIO2_OFFSET         0x000E8000ULL
#define RP1_RIO2_SIZE           0x1000u
#define RIO_OUT                 0x00u
#define RIO_OE                  0x04u
#define CAM0_REG_GPIO34_BIT     (1u << 0)

static int rp1_camera_power_cycle(void)
{
    volatile uint32_t *rio = rp1_map(RP1_RIO2_OFFSET, RP1_RIO2_SIZE);
    if (!rio)
        return -1;

    /* gpio34 must be an output (it already is; ensure it). */
    rio[RIO_OE >> 2] |= CAM0_REG_GPIO34_BIT;

    /* Power OFF: drive cam0_reg low. */
    rio[RIO_OUT >> 2] &= ~CAM0_REG_GPIO34_BIT;
    fprintf(stderr, "[pwr] cam0_reg OFF (gpio34 low), OUT=0x%08x\n",
            rio[RIO_OUT >> 2]);
    usleep(30000);   /* off-on-delay 30 ms */

    /* Power ON: drive cam0_reg high. */
    rio[RIO_OUT >> 2] |= CAM0_REG_GPIO34_BIT;
    fprintf(stderr, "[pwr] cam0_reg ON  (gpio34 high), OUT=0x%08x\n",
            rio[RIO_OUT >> 2]);
    usleep(70000);   /* startup-delay 70 ms (regulator ramp + sensor POR) */

    return 0;
}

/* =========================================================================
 * Step 2 helpers — IMX708 I2C sensor init
 * ========================================================================= */

static int i2c_fd = -1;

/*
 * imx708_write - write one 8-bit register (16-bit address).
 */
static int imx708_write(uint16_t reg, uint8_t val)
{
    uint8_t buf[3] = { (reg >> 8) & 0xFF, reg & 0xFF, val };

    /* QNX DCMD_I2C_SEND: header + data bytes */
    struct {
        i2c_send_t hdr;
        uint8_t    data[3];
    } msg;

    memset(&msg, 0, sizeof(msg));
    msg.hdr.slave.addr = IMX708_I2C_ADDR;
    msg.hdr.slave.fmt  = I2C_ADDRFMT_7BIT;
    msg.hdr.len        = 3;
    msg.hdr.stop       = 1;
    memcpy(msg.data, buf, 3);

    int r = devctl(i2c_fd, DCMD_I2C_SEND, &msg, sizeof(msg), NULL);
    if (r != EOK) {
        fprintf(stderr, "[i2c] write reg=0x%04x val=0x%02x failed: %s\n",
                reg, val, strerror(r));
        return -1;
    }
    return 0;
}

/*
 * imx708_read16 - read a 16-bit value (two consecutive 8-bit registers).
 * Used for chip ID verification: reg 0x0016 (high byte) + 0x0017 (low byte).
 */
static int imx708_read16(uint16_t reg, uint16_t *out)
{
    /* Write: 2-byte register address */
    /* Read:  2-byte value            */
    struct {
        i2c_sendrecv_t hdr;
        uint8_t        data[4];  /* 2 sent + 2 received */
    } msg;

    memset(&msg, 0, sizeof(msg));
    msg.hdr.slave.addr = IMX708_I2C_ADDR;
    msg.hdr.slave.fmt  = I2C_ADDRFMT_7BIT;
    msg.hdr.send_len   = 2;
    msg.hdr.recv_len   = 2;
    msg.hdr.stop       = 1;
    msg.data[0] = (reg >> 8) & 0xFF;
    msg.data[1] = reg & 0xFF;

    int r = devctl(i2c_fd, DCMD_I2C_SENDRECV, &msg, sizeof(msg), NULL);
    if (r != EOK) {
        fprintf(stderr, "[i2c] read16 reg=0x%04x failed: %s\n",
                reg, strerror(r));
        return -1;
    }

    /* Dump raw buffer to diagnose buffer-layout vs. power issues.
     * If sensor is powered, expect 0x07 and 0x08 somewhere in [0..3].
     * If all zeros, sensor is likely unpowered (needs camera power GPIO). */
    fprintf(stderr, "[i2c] read16 reg=0x%04x raw[0..3]: "
            "%02x %02x %02x %02x\n",
            reg,
            msg.data[0], msg.data[1], msg.data[2], msg.data[3]);

    /* QNX DCMD_I2C_SENDRECV places received bytes at offset 0 (not send_len).
     * raw[0]=chip_id_hi, raw[1]=chip_id_lo, raw[2..3]=sent addr bytes (stale). */
    *out = ((uint16_t)msg.data[0] << 8) | (uint16_t)msg.data[1];
    return 0;
}

/*
 * imx708_read8 - read a single 8-bit register (16-bit address).
 */
static int imx708_read8(uint16_t reg, uint8_t *out)
{
    struct {
        i2c_sendrecv_t hdr;
        uint8_t        data[2];   /* 2 sent, 1 received */
    } msg;

    memset(&msg, 0, sizeof(msg));
    msg.hdr.slave.addr = IMX708_I2C_ADDR;
    msg.hdr.slave.fmt  = I2C_ADDRFMT_7BIT;
    msg.hdr.send_len   = 2;
    msg.hdr.recv_len   = 1;
    msg.hdr.stop       = 1;
    msg.data[0] = (reg >> 8) & 0xFF;
    msg.data[1] = reg & 0xFF;

    if (devctl(i2c_fd, DCMD_I2C_SENDRECV, &msg, sizeof(msg), NULL) != EOK)
        return -1;
    *out = msg.data[0];   /* received byte lands at offset 0 */
    return 0;
}

/*
 * imx708_dump_state - read back a few sensor registers to see what the sensor
 * actually thinks it's doing.  Key register: FRM_CNT (0x0005) — a CCS-standard
 * live frame counter that increments every frame while streaming and reads
 * 0xFF in standby.  If it changes between two calls, the sensor IS producing
 * frames (problem is downstream); if static, the sensor isn't streaming.
 * Also reads MODE_SELECT (0x0100): should be 0x01 if our stream-on stuck.
 */
static void imx708_dump_state(const char *when)
{
    uint8_t frm = 0, mode = 0, frl_hi = 0, frl_lo = 0;
    imx708_read8(0x0005, &frm);                 /* FRM_CNT  */
    imx708_read8(IMX708_REG_MODE_SELECT, &mode);/* 0x0100   */
    imx708_read8(0x0340, &frl_hi);              /* frame_length_lines hi */
    imx708_read8(0x0341, &frl_lo);              /* frame_length_lines lo */
    fprintf(stderr, "[imx708:%s] FRM_CNT(0x0005)=0x%02x  MODE(0x0100)=0x%02x  "
            "frame_len(0x0340:41)=0x%02x%02x\n",
            when, frm, mode, frl_hi, frl_lo);
}

/*
 * imx708_write_regs - write a table of (addr, val) pairs.
 */
static int imx708_write_regs(const imx708_reg_t *regs, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (imx708_write(regs[i].addr, regs[i].val) != 0)
            return -1;
        /* IMX708 datasheet: allow 1 µs between consecutive writes on I2C */
        usleep(1);
    }
    return 0;
}

/*
 * imx708_probe_bus - send a 1-byte I2C write to IMX708_I2C_ADDR and check
 * for ACK.  Returns EOK if the device responded, non-zero (EIO/ENXIO) if NAK.
 * Used to identify which /dev/i2cN has the sensor before the full init.
 */
static int imx708_probe_bus(const char *bus_path)
{
    int fd = open(bus_path, O_RDWR);
    if (fd < 0) return errno;

    struct {
        i2c_send_t hdr;
        uint8_t    data[1];
    } probe;
    memset(&probe, 0, sizeof(probe));
    probe.hdr.slave.addr = IMX708_I2C_ADDR;
    probe.hdr.slave.fmt  = I2C_ADDRFMT_7BIT;
    probe.hdr.len        = 1;   /* 1 dummy byte — enough to get an ACK/NAK */
    probe.hdr.stop       = 1;
    probe.data[0]        = 0;

    int r = devctl(fd, DCMD_I2C_SEND, &probe, sizeof(probe), NULL);
    close(fd);
    return r;
}

/*
 * imx708_init - open I2C, verify chip ID, write init tables, stream on.
 * Pass test_mode=1 to enable sensor color bars (useful during bring-up).
 */
static int imx708_init(int test_mode)
{
    /* Scan the two known I2C buses so we can report which has the sensor */
    static const char *buses[] = { "/dev/i2c1", "/dev/i2c6", NULL };
    for (int b = 0; buses[b]; b++) {
        int r = imx708_probe_bus(buses[b]);
        fprintf(stderr, "[i2c] probe 0x%02x on %-12s → %s\n",
                IMX708_I2C_ADDR, buses[b],
                r == EOK ? "ACK  ← sensor here"
                         : "NAK (not present or unpowered)");
    }

    i2c_fd = open(IMX708_I2C_BUS, O_RDWR);
    if (i2c_fd < 0) {
        fprintf(stderr, "[i2c] open %s failed: %s\n",
                IMX708_I2C_BUS, strerror(errno));
        return -1;
    }

    /* Verify chip ID */
    uint16_t chip_id = 0;
    if (imx708_read16(IMX708_REG_CHIP_ID, &chip_id) != 0) {
        fprintf(stderr, "[imx708] chip ID read failed — "
                "check I2C bus (%s) and address (0x%02x)\n",
                IMX708_I2C_BUS, IMX708_I2C_ADDR);
        return -1;
    }
    if (chip_id != IMX708_CHIP_ID) {
        fprintf(stderr, "[imx708] unexpected chip ID: got 0x%04x, want 0x%04x\n",
                chip_id, IMX708_CHIP_ID);
        return -1;
    }
    fprintf(stderr, "[imx708] chip ID OK: 0x%04x\n", chip_id);

    /* Common init */
    fprintf(stderr, "[imx708] writing common registers (%zu regs)...\n",
            IMX708_COMMON_REGS_N);
    if (imx708_write_regs(imx708_common_regs, IMX708_COMMON_REGS_N) != 0)
        return -1;

    /* Link frequency: 450 MHz */
    if (imx708_write_regs(imx708_link_450mhz, IMX708_LINK_REGS_N) != 0)
        return -1;

    /* Mode: 2x2 binned (2304x1296) */
    fprintf(stderr, "[imx708] writing 2x2bin mode registers (%zu regs)...\n",
            IMX708_MODE_2X2BIN_N);
    if (imx708_write_regs(imx708_mode_2x2bin, IMX708_MODE_2X2BIN_N) != 0)
        return -1;

    /* Optionally enable color bars for pipeline verification */
    if (test_mode) {
        fprintf(stderr, "[imx708] enabling color bar test pattern\n");
        imx708_write(IMX708_REG_TEST_PATTERN,     (IMX708_TP_COLOR_BARS >> 8) & 0xFF);
        imx708_write(IMX708_REG_TEST_PATTERN + 1, IMX708_TP_COLOR_BARS & 0xFF);
    }

    /* Stream on */
    fprintf(stderr, "[imx708] streaming on\n");
    imx708_write(IMX708_REG_MODE_SELECT, IMX708_STREAMING);
    usleep(100000);   /* 100 ms: let the sensor stabilise before first frame */

    return 0;
}

/* =========================================================================
 * Step 3 — DMA buffer allocation + capture
 * ========================================================================= */

static void    *dma_virt = NULL;   /* virtual address of DMA buffer  */
static off64_t  dma_phys = 0;     /* physical address (off64_t for mem_offset64) */

/*
 * dma_alloc_contig - allocate a physically contiguous buffer.
 * QNX SDP 8: use posix_typed_mem_open("/memory/below4G", ...) so the
 * buffer lives in the 32-bit physical address range, which RP1 DMA can reach.
 */
static int dma_alloc_contig(size_t size)
{
    /* Open typed memory region for contiguous allocation below 4 GB */
    int tmfd = posix_typed_mem_open("/memory/below4G", O_RDWR,
                                    POSIX_TYPED_MEM_ALLOCATE_CONTIG);
    if (tmfd < 0) {
        fprintf(stderr, "[dma] posix_typed_mem_open failed: %s\n",
                strerror(errno));
        return -1;
    }

    dma_virt = mmap(0, size,
                    PROT_READ | PROT_WRITE | PROT_NOCACHE,
                    MAP_SHARED, tmfd, 0);
    close(tmfd);

    if (dma_virt == MAP_FAILED) {
        fprintf(stderr, "[dma] mmap contig %zu bytes failed: %s\n",
                size, strerror(errno));
        dma_virt = NULL;
        return -1;
    }

    /* Get physical address */
    if (mem_offset64(dma_virt, NOFD, 1, &dma_phys, NULL) != 0) {
        fprintf(stderr, "[dma] mem_offset64 failed: %s\n", strerror(errno));
        munmap(dma_virt, size);
        dma_virt = NULL;
        return -1;
    }

    fprintf(stderr, "[dma] allocated %zu bytes: virt=%p phys=0x%016llx\n",
            size, dma_virt, (unsigned long long)dma_phys);
    return 0;
}

/* =========================================================================
 * Step 4 — QNX Resource Manager (exposes /dev/video0)
 * ========================================================================= */

/* Channel ID for the interrupt pulse */
static int    intr_chid = -1;
static int    intr_coid = -1;
static int    intr_id   = -1;

/* Pulse code sent by ISR on frame-done */
#define PULSE_FRAME_DONE    1

/* Frame-done counter — incremented by the interrupt handler thread */
static volatile uint32_t frame_count = 0;

/*
 * The resmgr boilerplate: connect funcs, I/O funcs, iofunc attribute.
 */
static resmgr_connect_funcs_t  connect_funcs;
static resmgr_io_funcs_t       io_funcs;
static iofunc_attr_t           dev_attr;
static dispatch_t             *dpp = NULL;
static int                     resmgr_id = -1;

/* -----------------------------------------------------------------------
 * Interrupt service routine (runs in a dedicated thread started below).
 * RP1 triggers a PCI MSI/INTx when CH_CTRL_IRQ_FE fires.
 * We clear the status and send a pulse.
 * ----------------------------------------------------------------------- */
static csi2_t *g_csi2 = NULL;   /* set before interrupt is attached */

static const struct sigevent *csi2_isr(void *arg, int id)
{
    (void)arg; (void)id;
    /*
     * Acknowledge the interrupt at the CSI-2 level by clearing STATUS.
     * In a real driver we'd also re-arm ADDR0 here for the next buffer.
     * For now we just count frames and let user-space poll.
     */
    if (g_csi2)
        csi2_clear_status(g_csi2);

    frame_count++;

    /* Send pulse to the dispatch thread */
    static struct sigevent ev;
    SIGEV_PULSE_INIT(&ev, intr_coid, SIGEV_PULSE_PRIO_INHERIT,
                     PULSE_FRAME_DONE, 0);
    return &ev;
}

/* -----------------------------------------------------------------------
 * io_read — called when user does read() on /dev/video0
 *
 * Blocks until a new frame arrives (via interrupt pulse), then copies
 * FRAME_BYTES from the DMA buffer to the user's read buffer.
 * If the user buffer is smaller than a full frame, we return however
 * many bytes fit and advance an internal offset.
 * ----------------------------------------------------------------------- */

/* Per-open context: tracks read offset within current frame */
typedef struct {
    iofunc_ocb_t ocb;          /* must be first */
    uint32_t     last_frame;   /* frame_count when this frame started    */
    size_t       offset;       /* bytes consumed so far in current frame */
} video_ocb_t;

static int io_open(resmgr_context_t *ctp, io_open_t *msg,
                   iofunc_attr_t *attr, void *extra)
{
    (void)extra;
    video_ocb_t *ocb = calloc(1, sizeof(*ocb));
    if (!ocb)
        return ENOMEM;

    /* iofunc_ocb_attach: last arg is io_funcs (not mount), NULL = use defaults */
    iofunc_ocb_attach(ctp, msg, &ocb->ocb, attr, NULL);
    ocb->last_frame = frame_count;
    ocb->offset     = 0;
    return EOK;
}

static int io_read(resmgr_context_t *ctp, io_read_t *msg, iofunc_ocb_t *ocb_base)
{
    video_ocb_t *ocb = (video_ocb_t *)ocb_base;

    /* Wait for a new frame if we've consumed the current one.
     * Poll the CH_DEBUG frame counter (which AUTO_ARM keeps advancing), not
     * the interrupt-only global frame_count — interrupts aren't wired up, so
     * that counter never moves and the old code would block forever. */
    if (ocb->offset == 0 && g_csi2) {
        uint32_t start = csi2_read_frame_count(g_csi2, CAPTURE_CHANNEL);
        uint32_t cur   = start;
        int waited     = 0;
        while (cur == start && waited++ < 3000) {  /* wait up to ~3 s */
            usleep(1000);
            cur = csi2_read_frame_count(g_csi2, CAPTURE_CHANNEL);
        }
        ocb->last_frame = cur;
    }

    /* How many bytes can we serve? */
    size_t remaining = FRAME_BYTES - ocb->offset;
    int nbytes = msg->i.nbytes;
    if ((size_t)nbytes > remaining)
        nbytes = (int)remaining;

    /* Reply directly with the frame data */
    _IO_SET_READ_NBYTES(ctp, nbytes);
    SETIOV(ctp->iov, (uint8_t *)dma_virt + ocb->offset, nbytes);

    ocb->offset += nbytes;
    if (ocb->offset >= FRAME_BYTES)
        ocb->offset = 0;  /* fully consumed; next read waits for next frame */

    /* _RESMGR_NPARTS(n): tell dispatcher we filled n iov entries */
    return _RESMGR_NPARTS(1);
}

/* =========================================================================
 * Main — ties all steps together
 * ========================================================================= */

int main(int argc, char *argv[])
{
    int test_mode = 0;

    /* Parse --test flag to enable color bars */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--test") == 0)
            test_mode = 1;
    }

    fprintf(stderr, "=== camera_resmgr starting ===\n");
    if (test_mode)
        fprintf(stderr, "  [MODE] color bar test pattern enabled\n");

    /* ------------------------------------------------------------------
     * STEP 1: Direct physical memory mapping + DPHY + CSI-2 RX init
     * ------------------------------------------------------------------ */
    fprintf(stderr, "\n--- Step 1: RP1 register mapping ---\n");
    fprintf(stderr, "[step1] RP1 BAR0 phys base = 0x%016llx\n",
            (unsigned long long)RP1_BAR0_PHYS);

    /*
     * CRITICAL (added): enable the CSI0 MIPI config clock BEFORE any DPHY/CSI
     * access.  Without it the D-PHY functional logic is unclocked and
     * PHY_STOPSTATE stays 0 forever (the symptom we chased for a long time).
     */
    if (rp1_enable_mipi0_cfg_clock() != 0) {
        fprintf(stderr, "FATAL: could not enable RP1_CLK_MIPI0_CFG\n");
        return 1;
    }

    /*
     * CRITICAL: Map MIPI_CFG first and set SEL_CSI=1.
     * Until this write the CSI2/DPHY blocks are in DSI mode and every
     * register read returns 0xFFFFFFFF.
     * (Linux: cfe_start_streaming() → cfg_reg_write(MIPICFG_CFG, SEL_CSI))
     */
    volatile uint32_t *mipi_cfg_regs = rp1_map(RP1_CSI0_MIPICFG_OFFSET,
                                                RP1_CSI0_MIPICFG_SIZE);
    if (!mipi_cfg_regs) return 1;
    mipi_cfg_regs[MIPICFG_CFG_REG >> 2] = MIPICFG_CFG_SEL_CSI;
    fprintf(stderr, "[mipicfg] SEL_CSI written (readback=0x%08x)\n",
            mipi_cfg_regs[MIPICFG_CFG_REG >> 2]);

    /* Map CSI-2 DMA registers */
    volatile uint32_t *csi2_regs = rp1_map(RP1_CSI0_DMA_OFFSET,
                                             RP1_CSI0_DMA_SIZE);
    if (!csi2_regs) return 1;

    /* Map DPHY registers */
    volatile uint32_t *dphy_regs = rp1_map(RP1_CSI0_DPHY_OFFSET,
                                             RP1_CSI0_DPHY_SIZE);
    if (!dphy_regs) return 1;

    /*
     * Initialise DPHY context but do NOT wait for stop-state yet.
     * The D-PHY stop-state (LP-11) is driven by the sensor; the sensor must
     * be powered and idle before LP-11 appears on the lanes.  We bring the
     * sensor up in Step 2 below, then wait for stop-state after that.
     */
    static dphy_t dphy;
    dphy_init(&dphy, dphy_regs, 2);
    dphy_start(&dphy);
    fprintf(stderr, "[dphy] reset released — will verify stop-state after sensor init\n");

    /* Pre-init CSI-2 context (open_rx called after sensor is up) */
    static csi2_t csi2;
    csi2_init(&csi2, csi2_regs, &dphy, 2);
    g_csi2 = &csi2;

    /* ------------------------------------------------------------------
     * STEP 2: IMX708 I2C sensor initialisation
     *
     * Must happen before dphy_wait_stop(): the sensor drives LP-11 on the
     * CSI-2 lanes once powered.  Waiting before this will always timeout.
     * ------------------------------------------------------------------ */
    fprintf(stderr, "\n--- Step 2: IMX708 I2C init ---\n");

    /* Clean power-on reset of the sensor before configuring it (Linux does
     * this on every bring-up; QNX inherited a firmware-enabled regulator and
     * never reset the sensor — likely why it accepts config but never streams
     * frames). */
    if (rp1_camera_power_cycle() != 0)
        fprintf(stderr, "WARNING: camera power-cycle failed; continuing\n");

    if (imx708_init(test_mode) != 0) {
        fprintf(stderr, "FATAL: IMX708 init failed\n");
        fprintf(stderr, "  Hints:\n");
        fprintf(stderr, "  - Check I2C bus: is /dev/i2c* running? "
                "(`ls /dev/i2c*`)\n");
        fprintf(stderr, "  - IMX708_I2C_BUS = %s (edit imx708_regs.h)\n",
                IMX708_I2C_BUS);
        fprintf(stderr, "  - I2C address = 0x%02x (fixed by hardware)\n",
                IMX708_I2C_ADDR);
        return 1;
    }
    fprintf(stderr, "[step2] IMX708 sensor initialised and streaming\n");
    imx708_dump_state("post-stream-on");   /* baseline FRM_CNT */

    /* ------------------------------------------------------------------
     * Back to Step 1: now that the sensor is up, verify D-PHY stop-state
     * and open the CSI-2 RX.
     * ------------------------------------------------------------------ */
    fprintf(stderr, "\n--- Step 1 (cont): DPHY stop-state + CSI-2 RX ---\n");

    if (dphy_wait_stop(&dphy, DPHY_TIMEOUT_MS) != 0) {
        fprintf(stderr, "WARNING: D-PHY stop-state timeout (STOPSTATE=0) on CSI0.\n");
        fprintf(stderr, "  Power is on (cam0_reg/gpio34 driven high). If STOPSTATE\n");
        fprintf(stderr, "  stays 0 here, suspect: missing 24MHz INCK, or sensor not\n");
        fprintf(stderr, "  streaming. (CSI block confirmed: CD0 connector -> CSI0.)\n");
        fprintf(stderr, "  Continuing anyway; DMA may still work if sensor streams.\n");
    }

    csi2_open_rx(&csi2);

    uint32_t status = csi2_read_status(&csi2);
    fprintf(stderr, "[step1] CSI2_STATUS = 0x%08x\n", status);

    /* ------------------------------------------------------------------
     * STEP 3: DMA buffer allocation + start channel
     * ------------------------------------------------------------------ */
    fprintf(stderr, "\n--- Step 3: DMA setup ---\n");

    if (dma_alloc_contig(FRAME_BYTES) != 0) {
        fprintf(stderr, "FATAL: DMA buffer allocation failed\n");
        return 1;
    }

    csi2_start_channel(&csi2, CAPTURE_CHANNEL, (uint64_t)dma_phys,
                       IMX708_2X2_LINE_BYTES, FRAME_HEIGHT, FRAME_WIDTH,
                       CAPTURE_VC, CAPTURE_DT);

    /* Snapshot discards + STOPSTATE before polling — tells us which failure mode */
    fprintf(stderr, "[diag] DPHY  PHY_RX           = 0x%08x\n",
            dphy_regs [DPHY_PHY_RX              >> 2]);
    fprintf(stderr, "[diag] DPHY  PHY_STOPSTATE    = 0x%08x (want 0x3)\n",
            dphy_regs [DPHY_PHY_STOPSTATE       >> 2]);
    fprintf(stderr, "[diag] CSI2  DISCARDS_OVERFLOW  = 0x%08x\n",
            csi2_regs[CSI2_DISCARDS_OVERFLOW    >> 2]);
    fprintf(stderr, "[diag] CSI2  DISCARDS_INACTIVE  = 0x%08x\n",
            csi2_regs[CSI2_DISCARDS_INACTIVE    >> 2]);
    fprintf(stderr, "[diag] CSI2  DISCARDS_UNMATCHED = 0x%08x\n",
            csi2_regs[CSI2_DISCARDS_UNMATCHED   >> 2]);
    fprintf(stderr, "[diag] CSI2  DISCARDS_LEN_LIMIT = 0x%08x\n",
            csi2_regs[CSI2_DISCARDS_LEN_LIMIT   >> 2]);

    /* Wait for the first few frames to arrive, confirm via CH_DEBUG */
    fprintf(stderr, "[step3] waiting for first %d frames (sensor warm-up)...\n",
            FRAME_SKIP_COUNT);
    uint32_t last_fc = 0;
    int frames_seen = 0;
    for (int attempts = 0; attempts < 5000 && frames_seen < FRAME_SKIP_COUNT;
         attempts++) {
        usleep(5000);   /* 5 ms */
        uint32_t fc = csi2_read_frame_count(&csi2, CAPTURE_CHANNEL);
        if (fc != last_fc) {
            frames_seen++;
            fprintf(stderr, "[step3] frame %d received (frame_count=%u)\n",
                    frames_seen, fc);
            last_fc = fc;
        }
    }
    /* Re-read the sensor's own frame counter. If FRM_CNT advanced since
     * post-stream-on, the sensor IS producing frames and the problem is in
     * the RP1 capture path; if it's unchanged (or 0xFF), the sensor itself
     * is not streaming and the issue is sensor-side config. */
    imx708_dump_state("post-wait");

    if (frames_seen < FRAME_SKIP_COUNT) {
        fprintf(stderr, "WARNING: only saw %d/%d warm-up frames "
                "(frame_count=%u)\n",
                frames_seen, FRAME_SKIP_COUNT,
                csi2_read_frame_count(&csi2, CAPTURE_CHANNEL));
        /* Second snapshot — shows if discards accumulated during the wait */
        fprintf(stderr, "[diag] DPHY  PHY_STOPSTATE    = 0x%08x\n",
                dphy_regs [DPHY_PHY_STOPSTATE       >> 2]);
        fprintf(stderr, "[diag] CSI2  DISCARDS_OVERFLOW  = 0x%08x\n",
                csi2_regs[CSI2_DISCARDS_OVERFLOW    >> 2]);
        fprintf(stderr, "[diag] CSI2  DISCARDS_INACTIVE  = 0x%08x\n",
                csi2_regs[CSI2_DISCARDS_INACTIVE    >> 2]);
        fprintf(stderr, "[diag] CSI2  DISCARDS_UNMATCHED = 0x%08x\n",
                csi2_regs[CSI2_DISCARDS_UNMATCHED   >> 2]);
        fprintf(stderr, "  If CH_DEBUG stays 0, the DMA isn't receiving data.\n");
        fprintf(stderr, "  Check: sensor streaming (Step 2 OK?), "
                "CSI2 channel offsets, cable\n");
    } else {
        fprintf(stderr, "[step3] OK — sensor is streaming frames\n");
    }

    /* ------------------------------------------------------------------
     * STEP 4: Interrupt setup + /dev/video0 resource manager
     * ------------------------------------------------------------------ */
    fprintf(stderr, "\n--- Step 4: interrupt + /dev/video0 ---\n");

    /* Create a channel for interrupt pulses */
    intr_chid = ChannelCreate(0);
    if (intr_chid < 0) {
        fprintf(stderr, "[intr] ChannelCreate failed: %s\n", strerror(errno));
        return 1;
    }
    intr_coid = ConnectAttach(0, 0, intr_chid, _NTO_SIDE_CHANNEL, 0);
    if (intr_coid < 0) {
        fprintf(stderr, "[intr] ConnectAttach failed: %s\n", strerror(errno));
        return 1;
    }

    /*
     * IRQ attachment.
     *
     * The QNX PCI server (pci-server) handles RP1's MSI-X setup.
     * To find the CSI-2 IRQ number, run on the Pi:
     *   pidin -P pci-server ir
     * or: cat /proc/interrupts  (if procfs is mounted)
     *
     * For now we skip interrupt attachment and rely on the CH_DEBUG
     * frame-counter polling already done in Step 3.  The read() path
     * also polls frame_count (incremented here by interrupt, or by a
     * future upgrade).  Polling works reliably for the pipe-monitoring
     * use case at 56 fps.
     *
     * To enable interrupts later, uncomment the block below and fill in
     * the correct IRQ number from pidin output.
     */
    int irq = -1;  /* set to actual IRQ to enable interrupt mode */
    /* Example (fill in correct IRQ):
     * irq = 189;
     */

    if (irq >= 0) {
        /* Need PROCMGR_AID_INTERRUPT ability — run as root or with procmgr ability */
        intr_id = InterruptAttach(irq, csi2_isr, NULL, 0, _NTO_INTR_FLAGS_TRK_MSK);
        if (intr_id < 0) {
            fprintf(stderr, "[intr] InterruptAttach irq=%d failed: %s\n",
                    irq, strerror(errno));
            fprintf(stderr, "  Continuing in polling mode\n");
        } else {
            fprintf(stderr, "[intr] interrupt attached (id=%d)\n", intr_id);
        }
    }

    /* Set up the resource manager for /dev/video0 */
    dpp = dispatch_create();
    if (!dpp) {
        fprintf(stderr, "[resmgr] dispatch_create failed\n");
        return 1;
    }

    /* Initialise connect and I/O function tables */
    iofunc_func_init(_RESMGR_CONNECT_NFUNCS, &connect_funcs,
                     _RESMGR_IO_NFUNCS, &io_funcs);
    connect_funcs.open = io_open;
    io_funcs.read      = io_read;

    /* Set up the iofunc attribute (device metadata) */
    iofunc_attr_init(&dev_attr, S_IFCHR | 0666, NULL, NULL);
    dev_attr.nbytes = FRAME_BYTES;

    /* Register /dev/video0 */
    resmgr_attr_t rm_attr;
    memset(&rm_attr, 0, sizeof(rm_attr));
    rm_attr.nparts_max   = 1;
    rm_attr.msg_max_size = 2048;

    resmgr_id = resmgr_attach(dpp, &rm_attr, VIDEO_DEV_PATH, _FTYPE_ANY, 0,
                               &connect_funcs, &io_funcs, &dev_attr);
    if (resmgr_id < 0) {
        fprintf(stderr, "[resmgr] resmgr_attach %s failed: %s\n",
                VIDEO_DEV_PATH, strerror(errno));
        return 1;
    }

    fprintf(stderr, "\n=== camera_resmgr ready ===\n");
    fprintf(stderr, "  Device : %s\n", VIDEO_DEV_PATH);
    fprintf(stderr, "  Frame  : %dx%d RAW10 (%u bytes/frame)\n",
            FRAME_WIDTH, FRAME_HEIGHT, (unsigned)FRAME_BYTES);
    fprintf(stderr, "\nTo capture one frame:\n");
    fprintf(stderr, "  dd if=/dev/video0 of=/tmp/frame.raw bs=%u count=1\n",
            (unsigned)FRAME_BYTES);
    fprintf(stderr, "\nTo decode on your PC (install ffmpeg):\n");
    fprintf(stderr, "  ffmpeg -f rawvideo -pixel_format bayer_rggb10 "
            "-video_size %dx%d -i frame.raw frame.png\n",
            FRAME_WIDTH, FRAME_HEIGHT);

    /* Enter the resource manager dispatch loop */
    dispatch_context_t *ctx = dispatch_context_alloc(dpp);
    for (;;) {
        if ((ctx = dispatch_block(ctx)) == NULL) {
            fprintf(stderr, "[resmgr] dispatch_block error: %s\n",
                    strerror(errno));
            break;
        }
        dispatch_handler(ctx);
    }

    /* Cleanup (not normally reached) */
    if (intr_id >= 0)    InterruptDetach(intr_id);
    if (intr_coid >= 0)  ConnectDetach(intr_coid);
    if (intr_chid >= 0)  ChannelDestroy(intr_chid);
    if (i2c_fd >= 0)     close(i2c_fd);
    dphy_stop(&dphy);
    csi2_close(&csi2);
    return 0;
}
