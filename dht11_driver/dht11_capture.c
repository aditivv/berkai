/*
 * dht11_capture.c — see dht11_capture.h.
 */

#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/neutrino.h>
#include <sys/syspage.h>

#include "dht11_capture.h"
#include "dht11_decode.h"
#include "rp1_gpio.h"

#define START_LOW_US        20000   /* host start signal: >=18 ms LOW */
#define QUIET_US            1000    /* no edge for 1 ms => frame over */
#define GUARD_US            20000   /* absolute burst cap */

int dht11_capture_init(int pin)
{
    if (rp1_gpio_map() != 0)
        return -1;
    rp1_gpio_claim(pin, RP1_PULL_UP);
    return 0;
}

void dht11_boost_realtime(void)
{
    struct sched_param sp;
    int max = sched_get_priority_max(SCHED_FIFO);

    sp.sched_priority = max;
    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) != 0)
        fprintf(stderr, "[dht11] warning: could not set SCHED_FIFO %d "
                "(not root?)\n", max);

    /* Pin to the last CPU so the busy-wait can't migrate mid-burst. */
    if (ThreadCtl(_NTO_TCTL_RUNMASK, (void *)(uintptr_t)(1u << 3)) == -1)
        fprintf(stderr, "[dht11] warning: RUNMASK pin-to-cpu failed\n");

    /* I/O privileges — required later for InterruptDisable(). */
    if (ThreadCtl(_NTO_TCTL_IO, 0) == -1)
        fprintf(stderr, "[dht11] warning: _NTO_TCTL_IO failed "
                "(interrupt-lock mode unavailable)\n");
}

double dht11_cycles_to_us(const dht11_capture_t *cap, uint64_t dt)
{
    return (double)dt * 1e6 / (double)cap->cps;
}

int dht11_capture_frame(int pin, dht11_capture_t *cap, int use_intr_lock)
{
    uint64_t quiet_cyc, guard_cyc, now, t_last_edge;
    int last;

    cap->cps = SYSPAGE_ENTRY(qtime)->cycles_per_sec;
    cap->n_edges = 0;
    quiet_cyc = cap->cps / 1000000u * QUIET_US;
    guard_cyc = cap->cps / 1000000u * GUARD_US;

    /* Host start signal: hold LOW >=18 ms, then release — the pull-up
     * raises the line and the sensor answers within ~20-40 us. Not
     * timing-critical, so usleep is fine here. */
    rp1_gpio_set_output(pin, 0);
    usleep(START_LOW_US);

    if (use_intr_lock)
        InterruptDisable();

    rp1_gpio_set_input(pin);

    /* Busy-wait capture. Each iteration: one ClockCycles() read (ARM
     * generic timer, ~ns) + one RP1 register read (~sub-us over PCIe).
     * That samples the line every well under 5 us — >5 samples even for
     * the shortest 26 us pulse. */
    cap->t_start = ClockCycles();
    cap->start_level = rp1_gpio_read(pin);
    last = cap->start_level;
    t_last_edge = cap->t_start;

    while (cap->n_edges < DHT11_MAX_EDGES) {
        int lvl;

        now = ClockCycles();
        lvl = rp1_gpio_read(pin);
        if (lvl != last) {
            cap->edges[cap->n_edges].t = now;
            cap->edges[cap->n_edges].level = lvl;
            cap->n_edges++;
            last = lvl;
            t_last_edge = now;
        }
        if (now - t_last_edge > quiet_cyc)
            break;                       /* frame finished */
        if (now - cap->t_start > guard_cyc)
            break;                       /* absolute guard */
    }

    if (use_intr_lock)
        InterruptEnable();

    return cap->n_edges;
}

int dht11_read_once(int pin, struct dht11_reading *out)
{
    dht11_capture_t cap;
    double highs[DHT11_MAX_EDGES];
    int n = 0, i;

    dht11_capture_frame(pin, &cap, 0);
    /* edges[i] is the transition TO edges[i].level; the time spent HIGH is
     * t[i+1] - t[i] for edges where level==1. */
    for (i = 0; i + 1 < cap.n_edges; i++)
        if (cap.edges[i].level == 1)
            highs[n++] = dht11_cycles_to_us(&cap,
                                            cap.edges[i + 1].t - cap.edges[i].t);
    return dht11_decode_highs(highs, n, out);
}
