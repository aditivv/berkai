/*
 * dht11_decode.h — pure 40-bit DHT11 frame decode (no GPIO/QNX deps).
 *
 * Input model (validated by the Phase 2 capture runs — 20/20 bimodal,
 * ~24 us zeros vs ~72 us ones, ~44 us gap, 85 edges/frame):
 *   the in-frame HIGH pulses are [pre-response ~12 us], [response ~87 us],
 *   then exactly 40 bit pulses. Decode takes the LAST 40 in-frame highs,
 *   classifies each against a threshold placed adaptively at the midpoint
 *   of the widest gap in the pulse-width distribution, assembles 5 bytes
 *   MSB-first, and verifies the checksum.
 *
 * Pure arithmetic — unit-tested off-hardware by dht11_decode_test.c.
 */

#ifndef DHT11_DECODE_H
#define DHT11_DECODE_H

/* HIGH pulses at/above this are idle line, not frame content. */
#define DHT11_FRAME_HIGH_MAX_US 150.0

typedef struct {
    unsigned char bytes[5];   /* rh_int rh_dec t_int t_dec checksum */
    double humidity;          /* %RH  = bytes[0] + bytes[1]/10      */
    double temperature;       /* degC = bytes[2] + bytes[3]/10      */
    double threshold_us;      /* 0/1 threshold actually used        */
} dht11_reading_t;

#define DHT11_DECODE_OK          0
#define DHT11_DECODE_SHORT_FRAME (-1)   /* fewer than 40 in-frame highs */
#define DHT11_DECODE_BAD_SUM     (-2)   /* checksum mismatch */

/*
 * Decode from in-frame HIGH pulse widths (us), in frame order. Pulses
 * >= DHT11_FRAME_HIGH_MAX_US are ignored, so passing the raw list
 * including idle highs is fine. Returns one of the codes above; fills
 * *out (bytes/threshold always filled when >= 40 pulses were present,
 * so a BAD_SUM result can still be inspected).
 */
int dht11_decode_highs(const double *high_us, int n, dht11_reading_t *out);

#endif /* DHT11_DECODE_H */
