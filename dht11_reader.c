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
 * DHT11 frame, and why we no longer assume a fixed bit-offset:
 * In theory edge[0] is the ACK pulse's falling edge, but on this Pi it
 * never arrives — by the time the synchronous rpi_gpio_setup_pull()/
 * add_event_detect() resmgr calls finish, the ACK low pulse has usually
 * already started. We tried correcting for this with a single shared
 * offset shifting the whole frame (assuming only the ACK was missed),
 * but real measurements disproved that: with an independent thermometer
 * reading ~12C/~92% RH for comparison, no single offset produced a
 * plausible humidity AND temperature together — sliding the window
 * within one byte's width just bit-rotates the same data, and the
 * rotations never landed on 12. That means humidity and temperature
 * need independent, non-uniform corrections (consistent with whatever
 * is happening right after the ACK — edge[1] is suspiciously short,
 * ~21us, matching neither the expected ~50us bit-separator nor the
 * expected ~80us ACK-high — possibly corrupting just the first
 * transmitted byte while leaving everything after it clean).
 *
 * So instead of assuming any fixed frame structure, decode_edges() now
 * slides an 8-bit window across the *entire* captured buffer and
 * reports every possible byte value, flagging whichever ones land near
 * HUMIDITY_HINT/TEMPERATURE_HINT (independently measured reference
 * values, not from this sensor — supplied at the top of this file).
 * This is a debugging tool, not a real deployment strategy: it only
 * works because we have ground truth to compare against right now.
 * Once we know which raw start-index reliably corresponds to which
 * value, that should get hardcoded back into a real frame model.
 *
 * We also can't capture the full 40-bit frame at all: this Pi's
 * capture reliably and reproducibly stalls with a multi-hundred-ms gap
 * right around bit 31 (~3ms of elapsed real time after arming) — same
 * exact edge count, three runs in a row, regardless of re-arming the
 * event detection after every edge, which rules out a registration/
 * pulse-capacity issue. Root cause not found (would need slog2info or
 * kernel-level tracing to dig further).
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
 * (two integers, e.g. "45,23") on stdout, exit code 0. No checksum
 * validation (see frame note above) — values are taken on trust.
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
#define EXPECTED_EDGES    60   /* as many raw edges as we can grab — this Pi's capture has reproducibly
                                 * stalled past edge ~65 (see file header), so this stays under that ceiling */
#define READ_TIMEOUT_MS   200  /* safety net per-edge wait, in case the sensor stalls */
/* Known-good reference values for THIS test, from an independent thermometer/hygrometer reading
 * (~12C, ~92% RH) — used only to find which sliding 8-bit window in the captured buffer is real
 * data, by checking which window's value lands close to one of these, not as a permanent feature. */
#define HUMIDITY_HINT     92
#define TEMPERATURE_HINT  12

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

/* Decode one 8-bit byte from edges[start..start+15] (8 bit-pairs, R then
 * F each), using a caller-supplied threshold (computed globally across
 * the whole buffer, not just this window, for stability). Also fills
 * widths_out[0..7] with the raw pulse widths (ns) behind each bit, so a
 * match can be visually double-checked rather than trusted blindly.
 * Returns -1 if start..start+15 doesn't land on a clean R,F,R,F,... run. */
static int decode_byte_at(edge_t *edges, int start, uint64_t halfway, uint8_t *byte_out, uint64_t widths_out[8])
{
    uint8_t byte = 0;
    for (int k = 0; k < 8; k++) {
        int r_idx = start + 2 * k;
        int f_idx = start + 2 * k + 1;
        if (!edges[r_idx].is_rising || edges[f_idx].is_rising) {
            return -1;
        }
        uint64_t width = edges[f_idx].ts_ns - edges[r_idx].ts_ns;
        widths_out[k] = width;
        int bit = width > halfway ? 1 : 0;
        byte = (uint8_t)((byte << 1) | bit);
    }
    *byte_out = byte;
    return 0;
}

static void print_widths_us(uint64_t widths[8])
{
    for (int k = 0; k < 8; k++) {
        fprintf(stderr, "%s%.1f", k == 0 ? "" : ",", (double)widths[k] / 1000.0);
    }
}

/* No fixed byte structure assumed — slides an 8-bit window across every
 * valid starting position in the whole captured buffer. Rather than
 * checking if a window's *value* is merely close to HUMIDITY_HINT/
 * TEMPERATURE_HINT, this checks for an EXACT bit-pattern match against
 * those known values (92 = 01011100, 12 = 00001100) — a much stronger
 * signal, since matching the precise pattern of short/long pulses is
 * far less likely to happen by chance than landing within +/-2 of a
 * number. Prints the raw widths behind every exact match so it can be
 * eyeballed, not just trusted. Returns 0 if at least one exact match
 * was found for either value. */
static int decode_edges(edge_t *edges, int n_edges, int *humidity, int *temperature)
{
    int n_widths = (n_edges - 1) / 2;
    uint64_t shortest = UINT64_MAX, longest = 0;
    for (int i = 0; i < n_widths; i++) {
        int r_idx = 2 * i, f_idx = 2 * i + 1;
        if (!edges[r_idx].is_rising || edges[f_idx].is_rising) continue;
        uint64_t w = edges[f_idx].ts_ns - edges[r_idx].ts_ns;
        if (w < shortest) shortest = w;
        if (w > longest) longest = w;
    }
    uint64_t halfway = (shortest + longest) / 2;

    int humidity_match = -1, temperature_match = -1;
    fprintf(stderr, "[dht11] scanning for EXACT matches to humidity=92 (0x5c) / temp=12 (0x0c), threshold %lluus:\n",
            (unsigned long long)(halfway / 1000));
    for (int start = 0; start + 15 < n_edges; start += 2) {
        uint8_t byte;
        uint64_t widths[8];
        if (decode_byte_at(edges, start, halfway, &byte, widths) != 0) {
            continue;
        }
        if (byte == HUMIDITY_HINT) {
            fprintf(stderr, "  start %2d: byte=0x%02x == HUMIDITY_HINT exactly. Widths(us): ", start, byte);
            print_widths_us(widths);
            fprintf(stderr, "\n");
            if (humidity_match == -1) humidity_match = byte;
        } else if (byte == TEMPERATURE_HINT) {
            fprintf(stderr, "  start %2d: byte=0x%02x == TEMPERATURE_HINT exactly. Widths(us): ", start, byte);
            print_widths_us(widths);
            fprintf(stderr, "\n");
            if (temperature_match == -1) temperature_match = byte;
        }
    }

    if (humidity_match == -1 && temperature_match == -1) {
        fprintf(stderr, "[dht11] no window exactly matched either reference value\n");
        return -1;
    }
    *humidity = (humidity_match != -1) ? humidity_match : 0;
    *temperature = (temperature_match != -1) ? temperature_match : 0;
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
