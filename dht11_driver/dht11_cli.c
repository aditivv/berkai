/*
 * dht11_cli.c — Phase 2: capture DHT11 frames and dump raw pulse timings.
 *
 * THE go/no-go gate for the whole busy-wait approach: across repeated reads,
 * the HIGH-pulse-width distribution must be cleanly BIMODAL — one cluster
 * near ~26 us (bit=0), one near ~70 us (bit=1), with a visible gap. The
 * qnx2 event-queue approach never produced that gap; if busy-wait polling
 * does, decode (Phase 3) is straightforward.
 *
 * Build (on the Pi):  cd ~/berkai/dht11_driver && make
 * Run (as root):      su
 *                     ./dht11_cli --reads 20        # Phase 2 gate run
 *                     ./dht11_cli --raw             # every edge, one read
 *                     ./dht11_cli --intlock ...     # only if smeared
 *
 * Per read it prints edge count, HIGH/LOW pulse lists (us), a HIGH histogram,
 * and a bimodality verdict; at the end, an aggregate verdict over all reads.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "dht11_capture.h"
#include "dht11_decode.h"
#include "rp1_gpio.h"

#define MAX_PULSES DHT11_MAX_EDGES

/* HIGH pulses shorter than this are frame content; longer = idle/response
 * tail, excluded from the bit statistics. */
#define FRAME_HIGH_MAX_US  150.0

typedef struct {
    double v[MAX_PULSES];
    int    n;
} pulse_list_t;

static int cmp_double(const void *a, const void *b)
{
    double d = *(const double *)a - *(const double *)b;
    return (d > 0) - (d < 0);
}

/*
 * Extract the duration of every complete pulse. edges[i] is the transition
 * TO edges[i].level, so the time spent at that level is t[i+1] - t[i].
 */
static void extract_pulses(const dht11_capture_t *cap,
                           pulse_list_t *highs, pulse_list_t *lows)
{
    int i;

    highs->n = lows->n = 0;
    for (i = 0; i + 1 < cap->n_edges; i++) {
        double us = dht11_cycles_to_us(cap,
                                       cap->edges[i + 1].t - cap->edges[i].t);
        if (cap->edges[i].level == 1 && highs->n < MAX_PULSES)
            highs->v[highs->n++] = us;
        else if (cap->edges[i].level == 0 && lows->n < MAX_PULSES)
            lows->v[lows->n++] = us;
    }
}

static void print_pulse_line(const char *label, const pulse_list_t *p)
{
    int i;

    printf("  %s (%d):", label, p->n);
    for (i = 0; i < p->n; i++)
        printf(" %.1f", p->v[i]);
    printf("\n");
}

static void print_histogram(const pulse_list_t *p)
{
    int bins[30] = {0}, over = 0, i, b;

    for (i = 0; i < p->n; i++) {
        if (p->v[i] >= FRAME_HIGH_MAX_US) { over++; continue; }
        b = (int)(p->v[i] / 5.0);
        if (b > 29) b = 29;
        bins[b]++;
    }
    printf("  HIGH histogram (5us bins): ");
    for (b = 0; b < 30; b++)
        if (bins[b])
            printf("[%d-%dus]:%d ", b * 5, b * 5 + 5, bins[b]);
    if (over)
        printf("[>=150us]:%d", over);
    printf("\n");
}

/*
 * Bimodality check on the in-frame HIGH pulses: sort, find the widest gap
 * between consecutive widths in the 20..100 us region, and demand a real
 * gap with enough samples on both sides.
 *
 * Expected population: 40 data bits (mix of ~26 and ~70 us) plus the ~80 us
 * response pulse, which lands with/above the "long" cluster — so we ask for
 * shorts + longs ≈ 41 in total but don't over-constrain the split (it
 * depends on the sensor's current reading).
 */
static int verdict_bimodal(const pulse_list_t *highs_in,
                           double *gap_out, int *n_short, int *n_long)
{
    double v[MAX_PULSES];
    int n = 0, i, gap_idx = -1;
    double gap = 0.0;

    for (i = 0; i < highs_in->n; i++)
        if (highs_in->v[i] < FRAME_HIGH_MAX_US)
            v[n++] = highs_in->v[i];
    if (n < 30)
        return 0;   /* lost too many edges to be a full frame */

    qsort(v, n, sizeof(double), cmp_double);
    for (i = 0; i + 1 < n; i++) {
        double lo = v[i], hi = v[i + 1];
        if (lo < 15.0 || hi > 110.0)
            continue;   /* only look for the gap inside the bit region */
        if (hi - lo > gap) {
            gap = hi - lo;
            gap_idx = i;
        }
    }
    if (gap_idx < 0)
        return 0;
    *gap_out = gap;
    *n_short = gap_idx + 1;
    *n_long  = n - (gap_idx + 1);
    return (gap >= 12.0 && *n_short >= 5 && *n_long >= 5 &&
            *n_short + *n_long >= 40);
}

int main(int argc, char **argv)
{
    int pin = 17, reads = 1, raw = 0, intlock = 0, i;
    double gap_s = 2.5;
    int ok_reads = 0;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--pin") && i + 1 < argc)        pin = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--reads") && i + 1 < argc) reads = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--gap") && i + 1 < argc)   gap_s = atof(argv[++i]);
        else if (!strcmp(argv[i], "--raw"))                   raw = 1;
        else if (!strcmp(argv[i], "--intlock"))               intlock = 1;
        else {
            fprintf(stderr, "usage: %s [--pin N] [--reads N] [--gap SECS] "
                    "[--raw] [--intlock]\n", argv[0]);
            return 2;
        }
    }
    if (gap_s < 2.0) {
        fprintf(stderr, "[dht11] --gap raised to 2.0 (DHT11 datasheet "
                "minimum sample interval)\n");
        gap_s = 2.0;
    }

    if (dht11_capture_init(pin) != 0)
        return 1;
    dht11_boost_realtime();

    printf("dht11_cli: pin GPIO%d, %d read(s), %.1fs apart%s\n",
           pin, reads, gap_s, intlock ? ", INTERRUPT-LOCKED bursts" : "");

    for (i = 0; i < reads; i++) {
        dht11_capture_t cap;
        pulse_list_t highs, lows;
        double gap = 0.0;
        int n_short = 0, n_long = 0, ok;

        if (i > 0) {   /* usleep >1s is not portable — split the wait */
            sleep((unsigned)gap_s);
            usleep((useconds_t)((gap_s - (unsigned)gap_s) * 1e6));
        }

        dht11_capture_frame(pin, &cap, intlock);
        extract_pulses(&cap, &highs, &lows);
        ok = verdict_bimodal(&highs, &gap, &n_short, &n_long);
        ok_reads += ok;

        printf("\n[read %2d] %d edges, frame span %.0f us\n", i, cap.n_edges,
               cap.n_edges ? dht11_cycles_to_us(&cap,
                   cap.edges[cap.n_edges - 1].t - cap.t_start) : 0.0);
        if (raw) {
            print_pulse_line("HIGH us", &highs);
            print_pulse_line("LOW  us", &lows);
        }
        print_histogram(&highs);
        if (ok)
            printf("  verdict: BIMODAL OK — %d short / %d long, "
                   "gap %.1f us\n", n_short, n_long, gap);
        else
            printf("  verdict: NOT CLEAN (short=%d long=%d gap=%.1f) — "
                   "rerun with --raw for full timings\n",
                   n_short, n_long, gap);

        /* Phase 3: decode the frame (checksum-verified). */
        {
            dht11_reading_t rd;
            int drc = dht11_decode_highs(highs.v, highs.n, &rd);

            if (drc == DHT11_DECODE_OK)
                printf("  decode : %.1f %%RH  %.1f C   "
                       "[%u %u %u %u sum %u]  thr %.1f us\n",
                       rd.humidity, rd.temperature,
                       rd.bytes[0], rd.bytes[1], rd.bytes[2], rd.bytes[3],
                       rd.bytes[4], rd.threshold_us);
            else
                printf("  decode : FAILED (%s)  bytes [%u %u %u %u sum %u]\n",
                       drc == DHT11_DECODE_BAD_SUM ? "checksum" : "short frame",
                       rd.bytes[0], rd.bytes[1], rd.bytes[2], rd.bytes[3],
                       rd.bytes[4]);
        }
    }

    printf("\n=== aggregate: %d/%d reads cleanly bimodal ===\n",
           ok_reads, reads);
    printf("Phase 2 gate: expect >= 90%% clean. If smeared, retry with "
           "--intlock; if still smeared, the I2C contingency applies.\n");

    rp1_gpio_restore(pin);
    return ok_reads == reads ? 0 : 1;
}
