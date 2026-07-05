/*
 * dht11_capture.h — DHT11 frame capture by busy-wait register polling.
 *
 * The qnx2-branch attempt used QNX's gpio_event queue and failed: dequeue
 * latency jitter made the ~26us (bit=0) vs ~70us (bit=1) HIGH pulses
 * indistinguishable. This module never touches an event queue: it polls the
 * pin level via rp1_gpio_read() (a single volatile load) in a tight loop and
 * timestamps each transition with ClockCycles(), at SCHED_FIFO max priority
 * pinned to one CPU. The whole burst is ~5 ms per read, at a >=2 s cadence.
 *
 * DHT11 frame (after the host start signal):
 *   host: LOW >=18 ms, release (pull-up raises the line)
 *   sensor: LOW ~80 us, HIGH ~80 us            (response)
 *   40 bits: LOW ~50 us + HIGH ~26 us (0) / ~70 us (1)
 *   final LOW, then line returns to idle HIGH
 *   => ~84 transitions end to end.
 */

#ifndef DHT11_CAPTURE_H
#define DHT11_CAPTURE_H

#include <stdint.h>

#define DHT11_MAX_EDGES 128

typedef struct {
    uint64_t t;       /* ClockCycles() at the transition */
    int      level;   /* pin level AFTER the transition */
} dht11_edge_t;

typedef struct {
    uint64_t     t_start;      /* ClockCycles() at capture start */
    int          start_level;  /* level at capture start */
    int          n_edges;
    dht11_edge_t edges[DHT11_MAX_EDGES];
    uint64_t     cps;          /* ClockCycles() ticks per second */
} dht11_capture_t;

/* Map RP1 + claim the pin (input, pull-up). Root only. 0 on success. */
int dht11_capture_init(int pin);

/* SCHED_FIFO max priority + pin the calling thread to one CPU. Call once
 * before the first capture. Failures are reported but non-fatal. */
void dht11_boost_realtime(void);

/*
 * One full read: drive the start signal, then busy-wait-capture every
 * transition until the line has been quiet for ~1 ms (frame over) or a
 * ~20 ms absolute guard expires. Returns the number of edges captured.
 *
 * use_intr_lock: if non-zero, disables interrupts on this CPU for the
 * ~5 ms burst (diagnostic escalation only — try without first).
 */
int dht11_capture_frame(int pin, dht11_capture_t *cap, int use_intr_lock);

/* Convert a ClockCycles() delta from `cap` to microseconds. */
double dht11_cycles_to_us(const dht11_capture_t *cap, uint64_t dt);

/*
 * Capture one frame and decode it (bridge to dht11_decode). Returns a
 * DHT11_DECODE_* code and fills *out. The caller owns the >=2s cadence
 * between calls.
 */
struct dht11_reading;   /* dht11_decode.h */
int dht11_read_once(int pin, struct dht11_reading *out);

#endif /* DHT11_CAPTURE_H */
