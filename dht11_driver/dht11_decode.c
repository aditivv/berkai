/*
 * dht11_decode.c — see dht11_decode.h.
 */

#include <stdlib.h>
#include <string.h>

#include "dht11_decode.h"

#define DEFAULT_THRESHOLD_US 45.0   /* midpoint of the measured 25/70 clusters */

static int cmp_double(const void *a, const void *b)
{
    double d = *(const double *)a - *(const double *)b;
    return (d > 0) - (d < 0);
}

/*
 * Threshold = midpoint of the widest gap between consecutive sorted widths
 * inside the plausible bit region. Falls back to the fixed midpoint of the
 * measured clusters if no clear gap exists (e.g. all 40 bits identical —
 * then any threshold on one side classifies them all the same way, and the
 * checksum arbitrates).
 */
static double pick_threshold(const double *v, int n)
{
    double *s = malloc((size_t)n * sizeof(double));
    double gap = 0.0, thr = DEFAULT_THRESHOLD_US;
    int i;

    if (!s)
        return DEFAULT_THRESHOLD_US;
    memcpy(s, v, (size_t)n * sizeof(double));
    qsort(s, (size_t)n, sizeof(double), cmp_double);
    for (i = 0; i + 1 < n; i++) {
        if (s[i] < 15.0 || s[i + 1] > 110.0)
            continue;
        if (s[i + 1] - s[i] > gap) {
            gap = s[i + 1] - s[i];
            thr = (s[i] + s[i + 1]) / 2.0;
        }
    }
    free(s);
    return (gap >= 8.0) ? thr : DEFAULT_THRESHOLD_US;
}

int dht11_decode_highs(const double *high_us, int n, dht11_reading_t *out)
{
    double frame[128];
    int nf = 0, i, first_bit;

    memset(out, 0, sizeof(*out));

    for (i = 0; i < n && nf < (int)(sizeof(frame) / sizeof(frame[0])); i++)
        if (high_us[i] < DHT11_FRAME_HIGH_MAX_US)
            frame[nf++] = high_us[i];

    if (nf < 40)
        return DHT11_DECODE_SHORT_FRAME;

    /* The 40 bit pulses are the LAST 40 in-frame highs (anything before
     * them is the pre-response blip and the ~80 us response pulse). */
    first_bit = nf - 40;
    out->threshold_us = pick_threshold(frame + first_bit, 40);

    for (i = 0; i < 40; i++) {
        int bit = frame[first_bit + i] > out->threshold_us;
        out->bytes[i / 8] = (unsigned char)((out->bytes[i / 8] << 1) | bit);
    }

    out->humidity    = out->bytes[0] + out->bytes[1] / 10.0;
    out->temperature = out->bytes[2] + out->bytes[3] / 10.0;

    if (((out->bytes[0] + out->bytes[1] + out->bytes[2] + out->bytes[3])
         & 0xFF) != out->bytes[4])
        return DHT11_DECODE_BAD_SUM;
    return DHT11_DECODE_OK;
}
