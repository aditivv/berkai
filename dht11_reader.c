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
 * DHT11 frame, after we release the start signal and arm detection
 * (line idles HIGH via pull-up until the sensor responds):
 *   edge[0]  F  - ACK low starts      edge[1]  R  - ACK low ends / ACK high starts
 *   edge[2]  F  - ACK high ends / bit0 lead-low starts
 *   edge[3]  R  - bit0 lead-low ends / bit0 DATA-HIGH starts
 *   edge[4]  F  - bit0 DATA-HIGH ends / bit1 lead-low starts
 *   ...
 *   edge[3+2k] R / edge[4+2k] F  -> width of bit k's data-high pulse, k=0..39
 *   -> 2 (ACK) + 40*2 (lead-low + data-high per bit) + ... = 83 edges total
 *      (edge[82] is the falling edge ending bit 39's data-high pulse)
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
 * Output: on a good read, exactly one line "humidity,temperature\n"
 * (two integers, e.g. "45,23") on stdout, exit code 0.
 * On a failed read (timeout, bad checksum, wrong edge order) prints a
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
#define EVENT_ID_EDGE     1   /* single id for both rising+falling, registered together */
#define EXPECTED_EDGES    83   /* 2 (ACK low+high) + 40 bits * 2 (lead-low + data-high) + 1 trailing falling */
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

/* Decode 83 captured edges into (humidity, temperature). Returns 0 on
 * success, -1 on bad framing/checksum. */
static int decode_edges(edge_t *edges, int n_edges, int *humidity, int *temperature)
{
    if (n_edges != EXPECTED_EDGES) return -1;

    uint64_t widths[40];
    for (int k = 0; k < 40; k++) {
        int r_idx = 3 + 2 * k;
        int f_idx = 4 + 2 * k;
        if (!edges[r_idx].is_rising || edges[f_idx].is_rising) {
            fprintf(stderr, "[dht11] unexpected edge order at bit %d — noisy read\n", k);
            return -1;
        }
        widths[k] = edges[f_idx].ts_ns - edges[r_idx].ts_ns;
    }

    uint64_t shortest = widths[0], longest = widths[0];
    for (int k = 1; k < 40; k++) {
        if (widths[k] < shortest) shortest = widths[k];
        if (widths[k] > longest) longest = widths[k];
    }
    uint64_t halfway = (shortest + longest) / 2;

    uint8_t bytes[5] = {0, 0, 0, 0, 0};
    for (int k = 0; k < 40; k++) {
        int bit = widths[k] > halfway ? 1 : 0;
        bytes[k / 8] = (uint8_t)((bytes[k / 8] << 1) | bit);
    }

    uint8_t checksum = (uint8_t)(bytes[0] + bytes[1] + bytes[2] + bytes[3]);
    if (checksum != bytes[4]) {
        fprintf(stderr, "[dht11] checksum mismatch (got 0x%02x, expected 0x%02x)\n",
                checksum, bytes[4]);
        return -1;
    }

    *humidity = bytes[0];
    *temperature = bytes[2];
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

    int humidity = 0, temperature = 0;
    if (decode_edges(edges, EXPECTED_EDGES, &humidity, &temperature) != 0) {
        rpi_gpio_cleanup();
        return 1;
    }

    printf("%d,%d\n", humidity, temperature);
    rpi_gpio_cleanup();
    return 0;
}
