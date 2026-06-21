/*
 * dht11_reader.c — standalone DHT11 temperature/humidity reader for QNX,
 * built on the hardware-component-samples librpi_gpio client API.
 *
 * Why a separate C binary instead of pure Python:
 *   - librpi_gpio is a C library (rpi_gpio.h) — not exposed to Python.
 *   - rpi_gpio_input()/output()/setup() each do a synchronous MsgSend to
 *     the GPIO resource manager (/dev/gpio/msg) — too slow/jittery to
 *     bit-bang DHT11's ~27us/~70us pulses by polling rpi_gpio_input() in
 *     a loop.
 *   - rpi_gpio_add_event_detect() registers a QNX pulse (SIGEV_PULSE)
 *     that the resource manager delivers asynchronously on a GPIO edge.
 *     We register RISING|FALLING together in a single call. (An earlier
 *     version registered them as two separate calls with different
 *     event_ids, hoping the pulse itself would tell us the direction —
 *     on real hardware this silently failed: the second add_event_detect
 *     call appears to replace the first rather than add to it, since
 *     every captured edge came back tagged as whichever edge type was
 *     registered last. Confirmed via a verbose per-edge timing dump that
 *     showed 30+ consecutive "FALLING" edges at a real, alternating
 *     DHT11 bit cadence (~78us/~124us) — i.e. real rising edges were
 *     happening on the wire but never reported.) Now we timestamp
 *     immediately on pulse receipt (still no polling in the timing-
 *     critical wait), then make one rpi_gpio_input() call afterward just
 *     to label which direction it was — that extra call doesn't affect
 *     the recorded timestamp, only how we annotate it.
 *
 * DHT11 frame / bit numbering used here:
 * edge[2k] (R) / edge[2k+1] (F) is bit k's data-high pulse, k=0..31 —
 * confirmed structurally correct by inspecting real captures: every
 * single (even,odd) gap is cleanly ~24us or ~70us (the '0'/'1' data
 * pulse widths) and every (odd,even) gap is cleanly ~53us (the 50us
 * separator), with no ambiguity anywhere in a real capture. So edge[0]
 * really is bit0's data-high start, not the ACK (the ACK is missed
 * entirely — see below).
 *
 * Per the standard DHT11 byte layout (humidity-int, humidity-dec,
 * temp-int, temp-dec, checksum), bits 16-23 are the temperature integer
 * and bits 24-31 are the temperature decimal (always 0 on real DHT11
 * hardware, but read anyway per request). We only use temperature —
 * humidity (bits 0-15) was checked against an independent thermometer/
 * hygrometer reading and never matched at any alignment we tried
 * within reach of the capture limit below, so it's dropped rather than
 * reported as a guess.
 *
 * Known hard limit: this Pi's capture reliably and reproducibly stalls
 * with a multi-hundred-ms gap right around bit 31 (~3ms of elapsed real
 * time after arming) — same exact edge count, multiple runs in a row,
 * regardless of re-arming the event detection after every edge (which
 * rules out a registration/pulse-capacity issue). Root cause not found
 * (would need slog2info or kernel-level tracing to dig further). Bit 31
 * is exactly the last bit we need, so this is right at the edge of
 * what's reachable — reads may fail more often than earlier, shorter
 * captures did.
 *
 * Build (run ON the Pi, after building librpi_gpio.a per
 * common/rpi_gpio/Makefile in the QNX hardware-component-samples repo —
 * paths below assume that repo was extracted to /tmp/hardware-component-
 * samples-main; adjust SAMPLES_DIR if yours lives elsewhere):
 *   SAMPLES_DIR=/tmp/hardware-component-samples-main
 *   qcc -Vgcc_ntoaarch64le dht11_reader.c \
 *     -I$SAMPLES_DIR/common/system/gpio \
 *     -I$SAMPLES_DIR/common/rpi_gpio/public \
 *     -L$SAMPLES_DIR/common/rpi_gpio/build/aarch64le-debug \
 *     -lm -lrpi_gpio -lpthread \
 *     -o dht11_reader
 * (the -Vgcc_ntoaarch64le variant is a best guess for QNX SDP 8.0.4 on
 * aarch64 — if qcc rejects it, check common/config.mk in that repo for
 * the CC variable's actual invocation and use that instead.)
 *
 * Run (needs root — same as the rest of the sensor/camera work):
 *   sudo ./dht11_reader [bcm_gpio_pin]      # defaults to GPIO 17
 *
 * Output: on a good read, exactly one line "temperature\n" (a single
 * integer, e.g. "23") on stdout, exit code 0. No checksum validation
 * (see frame note above) — value is taken on trust.
 * On a failed read (timeout, wrong edge order) prints a
 * diagnostic to stderr, exit code 1, nothing on stdout — caller should
 * wait >=1s and retry, per the DHT11 datasheet's minimum sample interval.
 *
 * UNTESTED on real hardware — written directly from the rpi_gpio.c/
 * rpi_gpio.h source pasted into the chat, since there's no QNX
 * environment available to build/run this in. Expect to need at least
 * one debugging pass on the actual Pi (wrong pin, timeout tuning,
 * EXPECTED_EDGES off-by-one from sensor noise, etc.).
 */

#include <errno.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/neutrino.h>
#include "rpi_gpio.h"

#define DEFAULT_DHT_PIN   17
#define EVENT_ID_EDGE     1    /* single id for both rising+falling, registered together */
#define TEMP_INT_BIT      16   /* bit 16 = first bit of the temperature-integer byte */
#define TEMP_DEC_BIT      24   /* bit 24 = first bit of the temperature-decimal byte */
#define EXPECTED_EDGES    64   /* need through bit 31 (temp-decimal's last bit) = edges 0..63.
                                 * This Pi's capture has reproducibly stalled right around this point
                                 * (see file header) — bit 31 is at the edge of what's reachable. */
#define READ_TIMEOUT_MS   200  /* safety net per-edge wait, in case the sensor stalls */

typedef struct {
    uint64_t ts_ns;
    int      is_rising;   /* 1 = rising, 0 = falling */
} edge_t;

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* Run this reader at elevated real-time priority so scheduling jitter
 * between a pulse arriving and us timestamping it stays small relative
 * to DHT11's tens-of-microseconds pulse widths. Non-fatal if it fails
 * (e.g. not root) — just noisier timing. */
static void boost_priority(void)
{
    struct sched_param sp;
    sp.sched_priority = sched_get_priority_max(SCHED_FIFO);
    if (sched_setscheduler(0, SCHED_FIFO, &sp) != 0) {
        fprintf(stderr, "[dht11] warning: couldn't raise scheduling priority (%s) — "
                         "timing may be jittery. Run as root to fix.\n", strerror(errno));
    }
}

/* Drive the DHT11 start sequence: pull low 18ms, release high ~30us,
 * then switch to input (with pull-up) so the sensor's response is
 * visible on the line. */
static int send_start_signal(int dht_pin)
{
    if (rpi_gpio_setup(dht_pin, GPIO_OUT) != GPIO_SUCCESS) return -1;
    if (rpi_gpio_output(dht_pin, GPIO_LOW) != GPIO_SUCCESS) return -1;

    struct timespec low_delay = { .tv_sec = 0, .tv_nsec = 18000000 }; /* 18ms */
    nanosleep(&low_delay, NULL);

    if (rpi_gpio_output(dht_pin, GPIO_HIGH) != GPIO_SUCCESS) return -1;
    struct timespec release_delay = { .tv_sec = 0, .tv_nsec = 30000 }; /* 30us */
    nanosleep(&release_delay, NULL);

    /* Pull-up in case the breakout board doesn't already have one. */
    if (rpi_gpio_setup_pull(dht_pin, GPIO_IN, GPIO_PUD_UP) != GPIO_SUCCESS) return -1;
    return 0;
}

/* Collect EXPECTED_EDGES timestamped rising/falling events on dht_pin via
 * QNX pulses (no rpi_gpio_input() polling). chid/coid are created by the
 * caller *before* driving the start signal, so the only thing happening
 * between "switch pin to input" and "armed" is this one registration
 * call — minimizing the chance of missing the sensor's first transition,
 * which is what a ChannelCreate/ConnectAttach done at this point (i.e.
 * after the start signal) was costing us. Returns 0 on success, -1 on
 * timeout/error. */
static int capture_edges(int dht_pin, int chid, int coid, edge_t *edges, int n_edges)
{
    int rc = 0;
    if (rpi_gpio_add_event_detect(dht_pin, coid, GPIO_RISING | GPIO_FALLING, EVENT_ID_EDGE) != GPIO_SUCCESS) {
        fprintf(stderr, "[dht11] add_event_detect(RISING|FALLING) failed\n");
        rc = -1;
    }

    uint64_t armed_ns = now_ns();
    int got = 0;
    while (rc == 0 && got < n_edges) {
        struct _pulse pulse;
        uint64_t timeout_ns = (uint64_t)READ_TIMEOUT_MS * 1000000ULL;
        TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_RECEIVE, NULL, &timeout_ns, NULL);

        int rcvid = MsgReceivePulse(chid, &pulse, sizeof(pulse), NULL);
        if (rcvid != 0) {
            if (errno == ETIMEDOUT) {
                fprintf(stderr, "[dht11] timed out waiting for edge %d/%d — "
                                 "check wiring/pull-up on GPIO %d\n", got, n_edges, dht_pin);
            } else {
                perror("MsgReceivePulse");
            }
            rc = -1;
            break;
        }

        if (pulse.value.sival_int != EVENT_ID_EDGE) {
            continue; /* unrelated pulse, ignore without consuming a slot */
        }

        uint64_t ts = now_ns();  /* timestamp first — input() call below is not timing-critical */
        unsigned level = GPIO_LOW;
        rpi_gpio_input(dht_pin, &level);  /* current level right after the edge tells us its direction */

        edges[got].ts_ns = ts;
        edges[got].is_rising = (level == GPIO_HIGH) ? 1 : 0;
        got++;
    }

    /* Always dump what we actually saw to stderr — doesn't touch stdout,
     * so it's safe even when called from sensor_reader.py's subprocess
     * wrapper. This is the actual diagnostic: distinguishes real noise
     * (many sub-microsecond-spaced edges) from a genuinely stuck line
     * (one huge gap) from a clean signal that just stopped partway. */
    fprintf(stderr, "[dht11] captured %d edge(s):\n", got);
    uint64_t prev_ns = armed_ns;
    for (int i = 0; i < got; i++) {
        double delta_us = (double)(edges[i].ts_ns - prev_ns) / 1000.0;
        fprintf(stderr, "  edge[%2d] %s  +%.1fus since previous\n",
                i, edges[i].is_rising ? "RISING " : "FALLING", delta_us);
        prev_ns = edges[i].ts_ns;
    }

    return (rc == 0 && got == n_edges) ? 0 : -1;
}

/* Decode one 8-bit byte starting at bit number first_bit (NOT an edge
 * index — edges[2*first_bit] is that bit's R, edges[2*first_bit+1] its
 * F), using a threshold computed globally across the whole buffer for
 * stability. Returns -1 if the run isn't a clean R,F,R,F,... pattern. */
static int decode_byte_at_bit(edge_t *edges, int first_bit, uint64_t halfway, uint8_t *byte_out)
{
    uint8_t byte = 0;
    for (int k = 0; k < 8; k++) {
        int r_idx = 2 * (first_bit + k);
        int f_idx = r_idx + 1;
        if (!edges[r_idx].is_rising || edges[f_idx].is_rising) {
            return -1;
        }
        uint64_t width = edges[f_idx].ts_ns - edges[r_idx].ts_ns;
        int bit = width > halfway ? 1 : 0;
        byte = (uint8_t)((byte << 1) | bit);
    }
    *byte_out = byte;
    return 0;
}

/* Fixed frame position, per the file header: bits 16-23 = temp integer,
 * bits 24-31 = temp decimal (edges[0..1] is bit0's R/F, so this needs
 * edges[32..63] to be present). Returns 0 and fills *temperature on
 * success (as a float — temp_int + temp_dec/10.0), -1 if the edge
 * buffer doesn't decode cleanly at these fixed positions. */
static int decode_temperature(edge_t *edges, int n_edges, double *temperature)
{
    if (n_edges < 2 * (TEMP_DEC_BIT + 8)) {
        fprintf(stderr, "[dht11] only %d edges captured, need %d to reach bit %d\n",
                n_edges, 2 * (TEMP_DEC_BIT + 8), TEMP_DEC_BIT + 7);
        return -1;
    }

    uint64_t shortest = UINT64_MAX, longest = 0;
    int n_widths = n_edges / 2;
    for (int i = 0; i < n_widths; i++) {
        int r_idx = 2 * i, f_idx = 2 * i + 1;
        if (!edges[r_idx].is_rising || edges[f_idx].is_rising) continue;
        uint64_t w = edges[f_idx].ts_ns - edges[r_idx].ts_ns;
        if (w < shortest) shortest = w;
        if (w > longest) longest = w;
    }
    uint64_t halfway = (shortest + longest) / 2;

    uint8_t temp_int, temp_dec;
    if (decode_byte_at_bit(edges, TEMP_INT_BIT, halfway, &temp_int) != 0) {
        fprintf(stderr, "[dht11] bits %d-%d (temp integer) didn't decode cleanly\n",
                TEMP_INT_BIT, TEMP_INT_BIT + 7);
        return -1;
    }
    if (decode_byte_at_bit(edges, TEMP_DEC_BIT, halfway, &temp_dec) != 0) {
        fprintf(stderr, "[dht11] bits %d-%d (temp decimal) didn't decode cleanly\n",
                TEMP_DEC_BIT, TEMP_DEC_BIT + 7);
        return -1;
    }

    fprintf(stderr, "[dht11] temp_int=%u temp_dec=%u (threshold %lluus)\n",
            temp_int, temp_dec, (unsigned long long)(halfway / 1000));
    *temperature = (double)temp_int + (double)temp_dec / 10.0;
    return 0;
}

int main(int argc, char **argv)
{
    int dht_pin = DEFAULT_DHT_PIN;
    if (argc > 1) {
        dht_pin = atoi(argv[1]);
    }

    boost_priority();

    /* Create the channel/connection *before* touching the pin at all, so
     * the only thing standing between "switch to input" and "armed" is
     * the single add_event_detect call inside capture_edges(). */
    int chid = ChannelCreate(0);
    if (chid == -1) {
        perror("ChannelCreate");
        return 1;
    }
    int coid = ConnectAttach(0, 0, chid, _NTO_SIDE_CHANNEL, 0);
    if (coid == -1) {
        perror("ConnectAttach");
        ChannelDestroy(chid);
        return 1;
    }

    if (send_start_signal(dht_pin) != 0) {
        fprintf(stderr, "[dht11] failed to drive start signal on GPIO %d\n", dht_pin);
        ConnectDetach(coid);
        ChannelDestroy(chid);
        rpi_gpio_cleanup();
        return 1;
    }

    edge_t edges[EXPECTED_EDGES];
    int capture_rc = capture_edges(dht_pin, chid, coid, edges, EXPECTED_EDGES);
    ConnectDetach(coid);
    ChannelDestroy(chid);
    if (capture_rc != 0) {
        fprintf(stderr, "[dht11] failed to capture a full edge sequence\n");
        rpi_gpio_cleanup();
        return 1;
    }

    double temperature = 0.0;
    if (decode_temperature(edges, EXPECTED_EDGES, &temperature) != 0) {
        rpi_gpio_cleanup();
        return 1;
    }

    printf("%.1f\n", temperature);
    rpi_gpio_cleanup();
    return 0;
}
