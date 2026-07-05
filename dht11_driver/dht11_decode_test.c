/*
 * dht11_decode_test.c — offline unit test for dht11_decode (Phase 3 gate).
 *
 * No GPIO, no root: synthesizes in-frame HIGH pulse lists shaped exactly
 * like the Phase 2 captures (pre-response ~12 us, response ~87 us, then 40
 * bits at ~24 us / ~72 us with deterministic jitter) and checks the decode
 * recovers the encoded bytes, the checksum verdicts, and the failure paths.
 *
 * Build & run (on the Pi, as any user):
 *   cd ~/berkai/dht11_driver && make test
 */

#include <stdio.h>
#include <string.h>

#include "dht11_decode.h"

static int failures;

static void check(const char *name, int cond)
{
    printf("[%s] %s\n", cond ? "PASS" : "FAIL", name);
    failures += !cond;
}

/* Deterministic jitter in [-2.0, +2.0] us, same flavour as real captures. */
static double jitter(int i)
{
    return (double)((i * 7) % 5) - 2.0;
}

/*
 * Build a full frame pulse list for the given 4 data bytes: pre-response,
 * response, then 40 bits MSB-first. Returns pulse count. checksum_delta
 * lets a test corrupt the (correctly computed) checksum byte.
 */
static int synth_frame(const unsigned char data[4], int checksum_delta,
                       double *out)
{
    unsigned char bytes[5];
    int n = 0, i;

    memcpy(bytes, data, 4);
    bytes[4] = (unsigned char)((data[0] + data[1] + data[2] + data[3]
                                + checksum_delta) & 0xFF);

    out[n++] = 12.0;                       /* pre-response blip  */
    out[n++] = 87.0;                       /* response HIGH      */
    for (i = 0; i < 40; i++) {
        int bit = (bytes[i / 8] >> (7 - i % 8)) & 1;
        out[n++] = (bit ? 72.0 : 24.0) + jitter(i);
    }
    return n;
}

int main(void)
{
    dht11_reading_t r;
    double pulses[64];
    int n, rc;

    /* 1. Nominal frame: RH=62.0 T=23.4 */
    {
        const unsigned char d[4] = { 62, 0, 23, 4 };
        n = synth_frame(d, 0, pulses);
        rc = dht11_decode_highs(pulses, n, &r);
        check("nominal decode returns OK", rc == DHT11_DECODE_OK);
        check("nominal bytes exact",
              r.bytes[0] == 62 && r.bytes[1] == 0 &&
              r.bytes[2] == 23 && r.bytes[3] == 4);
        check("nominal values", r.humidity == 62.0 &&
              r.temperature > 23.39 && r.temperature < 23.41);
        check("adaptive threshold inside the gap",
              r.threshold_us > 30.0 && r.threshold_us < 68.0);
    }

    /* 2. Ground-truth pattern from the qnx2 sessions: 92% / 12 C */
    {
        const unsigned char d[4] = { 92, 0, 12, 0 };
        n = synth_frame(d, 0, pulses);
        rc = dht11_decode_highs(pulses, n, &r);
        check("92%/12C decodes OK", rc == DHT11_DECODE_OK &&
              r.humidity == 92.0 && r.temperature == 12.0);
    }

    /* 3. Trailing idle HIGH (>150us) must be ignored, not treated as a bit */
    {
        const unsigned char d[4] = { 55, 0, 21, 7 };
        n = synth_frame(d, 0, pulses);
        pulses[n++] = 100000.0;            /* idle line after the frame */
        rc = dht11_decode_highs(pulses, n, &r);
        check("idle tail ignored", rc == DHT11_DECODE_OK &&
              r.bytes[0] == 55 && r.bytes[2] == 21);
    }

    /* 4. Corrupted checksum must be rejected but bytes still inspectable */
    {
        const unsigned char d[4] = { 62, 0, 23, 4 };
        n = synth_frame(d, 3, pulses);
        rc = dht11_decode_highs(pulses, n, &r);
        check("bad checksum rejected", rc == DHT11_DECODE_BAD_SUM);
        check("bytes still filled on BAD_SUM", r.bytes[0] == 62);
    }

    /* 5. Truncated capture (dropped edges) must be SHORT_FRAME */
    {
        const unsigned char d[4] = { 62, 0, 23, 4 };
        n = synth_frame(d, 0, pulses);
        rc = dht11_decode_highs(pulses, 30, &r);
        check("short frame rejected", rc == DHT11_DECODE_SHORT_FRAME);
    }

    /* 6. All-zero data bits (no long pulses at all): fixed-threshold
     *    fallback must classify them as zeros and the checksum (0) pass. */
    {
        const unsigned char d[4] = { 0, 0, 0, 0 };
        n = synth_frame(d, 0, pulses);
        rc = dht11_decode_highs(pulses, n, &r);
        check("all-zero frame decodes via fallback threshold",
              rc == DHT11_DECODE_OK && r.humidity == 0.0);
    }

    /* 7. Different cluster centres (sensor variance): 30us vs 60us */
    {
        const unsigned char d[4] = { 45, 0, 19, 2 };
        int i;
        n = synth_frame(d, 0, pulses);
        for (i = 2; i < n; i++)            /* squeeze both clusters */
            pulses[i] = pulses[i] < 45.0 ? pulses[i] + 6.0 : pulses[i] - 12.0;
        rc = dht11_decode_highs(pulses, n, &r);
        check("shifted clusters still decode (adaptive threshold)",
              rc == DHT11_DECODE_OK && r.bytes[0] == 45 && r.bytes[2] == 19);
    }

    printf("\n%s (%d failure%s)\n", failures ? "TEST FAILED" : "ALL TESTS PASSED",
           failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
