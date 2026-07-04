/*
 * rp1_gpio.h — register-level GPIO on the Pi 5's RP1, bank 0 (GPIO0..27).
 *
 * QNX has no sysfs/libgpiod; this module mmaps the RP1 blocks directly
 * (camera_resmgr.c pattern) and drives pins through SYS_RIO. All offsets
 * and bit meanings were hardware-verified by dht11_driver/gpio_peek
 * --selftest (Phase 0, 20/20 loopback cycles on GPIO17).
 *
 * Needs root (mmap_device_memory / PROCMGR_AID_MEM_PHYS).
 *
 * The read path is exposed as a static inline over the mapped pointer:
 * the DHT11 capture loop polls it in a microsecond-scale busy-wait and
 * must not pay a function call per sample.
 */

#ifndef RP1_GPIO_H
#define RP1_GPIO_H

#include <stdint.h>

#define RP1_GPIO_BANK0_NPINS  28

typedef enum { RP1_PULL_NONE = 0, RP1_PULL_UP, RP1_PULL_DOWN } rp1_pull_t;

/* Mapped register blocks — set by rp1_gpio_map(). Treat as read-only
 * outside this module except via the inline helpers below. */
extern volatile uint32_t *rp1_io_bank0;   /* STATUS @ 8*pin, CTRL @ 8*pin+4 */
extern volatile uint32_t *rp1_sys_rio0;   /* OUT @ 0x00, OE @ 0x04, IN @ 0x08 */
extern volatile uint32_t *rp1_pads_bank0; /* pad @ 0x04 + 4*pin */

/* SYS_RIO word indices (verified Phase 0) */
#define RP1_RIO_OUT  (0x00u >> 2)
#define RP1_RIO_OE   (0x04u >> 2)
#define RP1_RIO_IN   (0x08u >> 2)

/*
 * Map IO_BANK0 / SYS_RIO0 / PADS_BANK0. Returns 0 on success, -1 on error
 * (message on stderr — typically "not root"). Idempotent.
 */
int rp1_gpio_map(void);

/*
 * Snapshot a pin's CTRL + PAD registers / restore them. claim() calls
 * save() automatically the first time it touches a pin.
 */
void rp1_gpio_save(int pin);
void rp1_gpio_restore(int pin);

/*
 * Claim a pin for register GPIO: funcsel -> SYS_RIO(5), input buffer on,
 * schmitt on, requested pull, output driver permitted, direction = input
 * (RIO OE cleared). Call before any read/write/set_output on the pin.
 */
void rp1_gpio_claim(int pin, rp1_pull_t pull);

/*
 * Direction control. set_output(pin, level) presets the level THEN enables
 * the driver (no glitch); set_input(pin) releases the driver so the pad
 * floats back to its pull.
 */
void rp1_gpio_set_output(int pin, int level);
void rp1_gpio_set_input(int pin);

/* Change the driven level while already an output. */
void rp1_gpio_write(int pin, int level);

/* Raw register accessors for diagnostics/dumps. */
uint32_t rp1_gpio_ctrl_reg(int pin);
uint32_t rp1_gpio_status_reg(int pin);
uint32_t rp1_gpio_pad_reg(int pin);

/*
 * Fast input read — single volatile load + mask, safe to call in a
 * busy-wait. Requires rp1_gpio_map() + rp1_gpio_claim() done.
 */
static inline int rp1_gpio_read(int pin)
{
    return (int)((rp1_sys_rio0[RP1_RIO_IN] >> pin) & 1u);
}

#endif /* RP1_GPIO_H */
