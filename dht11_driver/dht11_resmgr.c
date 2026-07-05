/*
 * dht11_resmgr.c — QNX resource manager publishing DHT11 readings at
 * /dev/dht11 (mode 0666), so the Flask app (qnxuser) can read the sensor
 * that only root can capture (RP1 mmap needs PROCMGR_AID_MEM_PHYS).
 * Same ownership model as camera_resmgr / /dev/video0.
 *
 *   $ cat /dev/dht11
 *   {"temperature": 24.1, "humidity": 56.0, "age_s": 0.8, "ok": true}
 *
 * Behaviour:
 *   - Hardware is sampled at most once per MIN_INTERVAL_S (DHT11 datasheet
 *     minimum is 2 s) no matter how often clients read; between samples,
 *     readers get the cached value with its age.
 *   - A failed capture (bad checksum / short frame) keeps the previous
 *     good value; "ok" reports whether the LAST attempt succeeded and
 *     age_s how old the good value is. Consumers can threshold on age.
 *   - Each open() snapshots the JSON into its own OCB, so a client reading
 *     in two chunks can never see a torn line.
 *
 * Run (as root):   ./dht11_resmgr >/tmp/dht11.log 2>&1 &
 * Verify:          ls -l /dev/dht11   (crw-rw-rw-)
 *                  cat /dev/dht11     (as qnxuser)
 */

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
/* iofunc.h MUST come before dispatch.h so RESMGR_OCB_T defaults to
 * iofunc_ocb_t (not void) — otherwise io_funcs.read rejects our handler. */
#include <sys/iofunc.h>
#include <sys/dispatch.h>
#include <sys/neutrino.h>
#include <sys/syspage.h>

#include "dht11_capture.h"
#include "dht11_decode.h"
#include "rp1_gpio.h"

#define DEV_PATH        "/dev/dht11"
#define MIN_INTERVAL_S  2.0
#define JSON_MAX        160

static int g_pin = 17;

/* ── cached reading (single-threaded dispatch loop — no locking needed) ── */
static dht11_reading_t g_last;          /* last checksum-valid reading   */
static int      g_have_reading = 0;
static int      g_last_attempt_ok = 0;
static uint64_t g_t_good, g_t_attempt;  /* ClockCycles() stamps          */
static uint64_t g_cps;

static double cycles_to_s(uint64_t dt)
{
    return (double)dt / (double)g_cps;
}

/* Sample the hardware if the min interval has passed; else keep cache. */
static void refresh_if_due(void)
{
    uint64_t now = ClockCycles();
    dht11_reading_t rd;

    if (g_t_attempt != 0 && cycles_to_s(now - g_t_attempt) < MIN_INTERVAL_S)
        return;

    g_t_attempt = now;
    if (dht11_read_once(g_pin, &rd) == DHT11_DECODE_OK) {
        g_last = rd;
        g_have_reading = 1;
        g_last_attempt_ok = 1;
        g_t_good = now;
    } else {
        g_last_attempt_ok = 0;
    }
}

static int build_json(char *buf, int cap)
{
    if (!g_have_reading)
        return snprintf(buf, (size_t)cap,
                        "{\"temperature\": null, \"humidity\": null, "
                        "\"age_s\": null, \"ok\": false}\n");
    return snprintf(buf, (size_t)cap,
                    "{\"temperature\": %.1f, \"humidity\": %.1f, "
                    "\"age_s\": %.1f, \"ok\": %s}\n",
                    g_last.temperature, g_last.humidity,
                    cycles_to_s(ClockCycles() - g_t_good),
                    g_last_attempt_ok ? "true" : "false");
}

/* ── OCB with a per-open JSON snapshot (no torn reads across chunks) ──── */
typedef struct {
    iofunc_ocb_t base;
    char json[JSON_MAX];
    int  len;
    int  filled;
} dht11_ocb_t;

static iofunc_ocb_t *ocb_calloc(resmgr_context_t *ctp, iofunc_attr_t *attr)
{
    (void)ctp; (void)attr;
    return calloc(1, sizeof(dht11_ocb_t));
}

static void ocb_free(iofunc_ocb_t *ocb)
{
    free(ocb);
}

/* Filled in main() — QNX 8's structs have extra fields, so runtime
 * assignment beats brace initializers (statics start zeroed anyway). */
static iofunc_funcs_t ocb_funcs;
static iofunc_mount_t mountpoint;

static int io_read(resmgr_context_t *ctp, io_read_t *msg,
                   iofunc_ocb_t *ocb_base)
{
    dht11_ocb_t *ocb = (dht11_ocb_t *)ocb_base;
    int nbytes, remaining, status;

    if ((status = iofunc_read_verify(ctp, msg, ocb_base, NULL)) != EOK)
        return status;

    if (!ocb->filled) {                 /* first read on this open */
        refresh_if_due();
        ocb->len = build_json(ocb->json, JSON_MAX);
        ocb->filled = 1;
        ocb->base.offset = 0;
    }

    remaining = ocb->len - (int)ocb->base.offset;
    if (remaining <= 0) {               /* EOF — lets `cat` terminate */
        _IO_SET_READ_NBYTES(ctp, 0);
        return _RESMGR_NPARTS(0);
    }
    nbytes = (int)msg->i.nbytes;
    if (nbytes > remaining)
        nbytes = remaining;

    _IO_SET_READ_NBYTES(ctp, nbytes);
    SETIOV(ctp->iov, ocb->json + ocb->base.offset, nbytes);
    ocb->base.offset += (unsigned)nbytes;
    return _RESMGR_NPARTS(1);
}

static void on_term(int sig)
{
    (void)sig;
    rp1_gpio_restore(g_pin);
    _exit(0);
}

int main(int argc, char **argv)
{
    static resmgr_connect_funcs_t connect_funcs;
    static resmgr_io_funcs_t io_funcs;
    static iofunc_attr_t dev_attr;
    dispatch_t *dpp;
    dispatch_context_t *ctx;
    resmgr_attr_t rm_attr;
    int i;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--pin") && i + 1 < argc) {
            g_pin = atoi(argv[++i]);
        } else {
            fprintf(stderr, "usage: %s [--pin N]\n", argv[0]);
            return 2;
        }
    }

    g_cps = SYSPAGE_ENTRY(qtime)->cycles_per_sec;

    if (dht11_capture_init(g_pin) != 0)
        return 1;
    dht11_boost_realtime();
    signal(SIGINT, on_term);
    signal(SIGTERM, on_term);

    /* Warm the cache so the first client read has data. */
    refresh_if_due();
    printf("[dht11_resmgr] initial reading: %s",
           g_last_attempt_ok ? "" : "(failed — will retry on demand) ");
    if (g_have_reading)
        printf("%.1f C  %.1f %%RH\n", g_last.temperature, g_last.humidity);
    else
        printf("none yet\n");

    dpp = dispatch_create();
    if (!dpp) {
        fprintf(stderr, "[dht11_resmgr] dispatch_create failed\n");
        return 1;
    }

    iofunc_func_init(_RESMGR_CONNECT_NFUNCS, &connect_funcs,
                     _RESMGR_IO_NFUNCS, &io_funcs);
    io_funcs.read = io_read;

    ocb_funcs.nfuncs     = _IOFUNC_NFUNCS;
    ocb_funcs.ocb_calloc = ocb_calloc;
    ocb_funcs.ocb_free   = ocb_free;
    mountpoint.funcs     = &ocb_funcs;

    iofunc_attr_init(&dev_attr, S_IFCHR | 0666, NULL, NULL);
    dev_attr.mount = &mountpoint;

    memset(&rm_attr, 0, sizeof(rm_attr));
    rm_attr.nparts_max = 1;
    rm_attr.msg_max_size = 2048;

    if (resmgr_attach(dpp, &rm_attr, DEV_PATH, _FTYPE_ANY, 0,
                      &connect_funcs, &io_funcs, &dev_attr) < 0) {
        fprintf(stderr, "[dht11_resmgr] resmgr_attach %s failed: %s\n",
                DEV_PATH, strerror(errno));
        return 1;
    }

    printf("=== dht11_resmgr ready ===  (%s, pin GPIO%d, min interval %.0fs)\n",
           DEV_PATH, g_pin, MIN_INTERVAL_S);
    fflush(stdout);

    ctx = dispatch_context_alloc(dpp);
    for (;;) {
        if ((ctx = dispatch_block(ctx)) == NULL) {
            fprintf(stderr, "[dht11_resmgr] dispatch_block error: %s\n",
                    strerror(errno));
            break;
        }
        dispatch_handler(ctx);
    }
    rp1_gpio_restore(g_pin);
    return 1;
}
