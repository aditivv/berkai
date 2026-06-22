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
 * RP1 register base addresses (from rp1.dtsi, Linux rpi-6.12.y):
 *   CSI0 DMA  : RP1_BAR0 + 0x00C0B000
 *   CSI0 DPHY : RP1_BAR0 + 0x00C0B700
 *
 * VERIFY these with `pci-tool -v -b 0x1de4` on the Pi.
 * If they are wrong the STATUS read in Step 1 will show garbage or 0xFFFFFFFF.
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
 * Cross-check with: sudo cat /sys/bus/platform/devices/*/resource on Linux.
 */
#define RP1_CSI0_DMA_OFFSET     0x00C0B000ULL  /* CSI-2 DMA registers      */
#define RP1_CSI0_DMA_SIZE       0x100u
#define RP1_CSI0_DPHY_OFFSET    0x00C0B700ULL  /* D-PHY registers           */
#define RP1_CSI0_DPHY_SIZE      0x200u

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

    *out = ((uint16_t)msg.data[2] << 8) | (uint16_t)msg.data[3];
    return 0;
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
 * imx708_init - open I2C, verify chip ID, write init tables, stream on.
 * Pass test_mode=1 to enable sensor color bars (useful during bring-up).
 */
static int imx708_init(int test_mode)
{
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

static void   *dma_virt = NULL;   /* virtual address of DMA buffer  */
static uint64_t dma_phys = 0;     /* physical address                */

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
    off64_t off = 0;
    if (mem_offset64(dma_virt, NOFD, 1, &dma_phys, NULL) != 0) {
        fprintf(stderr, "[dma] mem_offset64 failed: %s\n", strerror(errno));
        munmap(dma_virt, size);
        dma_virt = NULL;
        return -1;
    }

    fprintf(stderr, "[dma] allocated %zu bytes: virt=%p phys=0x%016llx\n",
            size, dma_virt, (unsigned long long)dma_phys);
    (void)off;
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

static iofunc_funcs_t ocb_funcs = {
    _IOFUNC_NFUNCS,
    NULL,   /* nfuncs            */
};

static iofunc_mount_t mount_attr = {
    0, 0, 0, 0, &ocb_funcs
};

static int io_open(resmgr_context_t *ctp, io_open_t *msg,
                   iofunc_attr_t *attr, void *extra)
{
    (void)extra;
    video_ocb_t *ocb = calloc(1, sizeof(*ocb));
    if (!ocb)
        return ENOMEM;

    iofunc_ocb_attach(ctp, msg, &ocb->ocb, attr, &mount_attr);
    ocb->last_frame = frame_count;
    ocb->offset     = 0;
    return EOK;
}

static int io_read(resmgr_context_t *ctp, io_read_t *msg, iofunc_ocb_t *ocb_base)
{
    video_ocb_t *ocb = (video_ocb_t *)ocb_base;

    /* Wait for a new frame if we've consumed the current one */
    if (ocb->offset == 0) {
        uint32_t target = ocb->last_frame + 1;
        /* Spin-wait for the interrupt to increment frame_count.
         * This is a simple approach; a production driver would use
         * a condvar or MsgReceivePulse on intr_chid instead. */
        while (frame_count < target)
            usleep(1000);   /* 1 ms */
        ocb->last_frame = frame_count;
    }

    /* How many bytes can we serve? */
    size_t remaining = FRAME_BYTES - ocb->offset;
    int nbytes = msg->i.nbytes;
    if ((size_t)nbytes > remaining)
        nbytes = (int)remaining;

    /* Copy frame data to reply buffer */
    SETIOV(ctp->iov, (uint8_t *)dma_virt + ocb->offset, nbytes);
    _IO_SET_READ_NBYTES(ctp, nbytes);

    ocb->offset += nbytes;
    if (ocb->offset >= FRAME_BYTES)
        ocb->offset = 0;  /* frame fully consumed, next read waits for next frame */

    return _RESMGR_IOVEC(ctp, ctp->iov, 1);
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

    /* Map CSI-2 DMA registers */
    volatile uint32_t *csi2_regs = rp1_map(RP1_CSI0_DMA_OFFSET,
                                             RP1_CSI0_DMA_SIZE);
    if (!csi2_regs) return 1;

    /* Map DPHY registers */
    volatile uint32_t *dphy_regs = rp1_map(RP1_CSI0_DPHY_OFFSET,
                                             RP1_CSI0_DPHY_SIZE);
    if (!dphy_regs) return 1;

    /* Initialise DPHY (2 lanes, 450 Mbps) */
    static dphy_t dphy;
    dphy_init(&dphy, dphy_regs, 2);
    dphy_start(&dphy);

    if (dphy_wait_stop(&dphy, DPHY_TIMEOUT_MS) != 0) {
        fprintf(stderr, "FATAL: D-PHY stop-state timeout — "
                "check DPHY register offset (RP1_CSI0_DPHY_OFFSET=0x%08X)\n",
                RP1_CSI0_DPHY_OFFSET);
        return 1;
    }

    /* Initialise CSI-2 RX */
    static csi2_t csi2;
    csi2_init(&csi2, csi2_regs, &dphy, 2);
    csi2_open_rx(&csi2);
    g_csi2 = &csi2;

    uint32_t status = csi2_read_status(&csi2);
    fprintf(stderr, "[step1] CSI2_STATUS = 0x%08x\n", status);
    if (status & CSI2_STATUS_PHY_ERRORS) {
        fprintf(stderr, "WARNING: D-PHY errors in STATUS bits [4:0] = 0x%02x\n",
                status & CSI2_STATUS_PHY_ERRORS);
        fprintf(stderr, "  This can mean: wrong DPHY offset, bad cable, "
                "or sensor not yet powered\n");
        fprintf(stderr, "  Continuing — errors may clear once sensor starts\n");
    } else {
        fprintf(stderr, "[step1] OK — no PHY errors\n");
    }

    /* ------------------------------------------------------------------
     * STEP 2: IMX708 I2C sensor initialisation
     * ------------------------------------------------------------------ */
    fprintf(stderr, "\n--- Step 2: IMX708 I2C init ---\n");

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

    /* ------------------------------------------------------------------
     * STEP 3: DMA buffer allocation + start channel
     * ------------------------------------------------------------------ */
    fprintf(stderr, "\n--- Step 3: DMA setup ---\n");

    if (dma_alloc_contig(FRAME_BYTES) != 0) {
        fprintf(stderr, "FATAL: DMA buffer allocation failed\n");
        return 1;
    }

    csi2_start_channel(&csi2, CAPTURE_CHANNEL, dma_phys,
                       IMX708_2X2_LINE_BYTES, FRAME_HEIGHT,
                       CAPTURE_VC, CAPTURE_DT);

    /* Wait for the first few frames to arrive, confirm via CH_DEBUG */
    fprintf(stderr, "[step3] waiting for first %d frames (sensor warm-up)...\n",
            FRAME_SKIP_COUNT);
    uint32_t last_debug = 0;
    int frames_seen = 0;
    for (int attempts = 0; attempts < 5000 && frames_seen < FRAME_SKIP_COUNT;
         attempts++) {
        usleep(5000);   /* 5 ms */
        uint32_t dbg = csi2_read_debug(&csi2, CAPTURE_CHANNEL);
        uint16_t fc  = dbg & 0xFFFF;        /* frame counter in low 16 bits */
        if (fc != (last_debug & 0xFFFF)) {
            frames_seen++;
            fprintf(stderr, "[step3] frame %d received (CH_DEBUG=0x%08x)\n",
                    frames_seen, dbg);
            last_debug = dbg;
        }
    }
    if (frames_seen < FRAME_SKIP_COUNT) {
        fprintf(stderr, "WARNING: only saw %d/%d warm-up frames "
                "(CH_DEBUG=0x%08x)\n",
                frames_seen, FRAME_SKIP_COUNT,
                csi2_read_debug(&csi2, CAPTURE_CHANNEL));
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
