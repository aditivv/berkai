/*
 * rp1_gpio.c — see rp1_gpio.h. Offsets/bits hardware-verified by
 * gpio_peek --selftest (Phase 0).
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

#include "rp1_gpio.h"

#define RP1_BAR0_PHYS          0x1f00000000ULL   /* same as camera_resmgr.c */
#define RP1_IO_BANK0_OFFSET    0x000D0000ULL
#define RP1_SYS_RIO0_OFFSET    0x000E0000ULL
#define RP1_PADS_BANK0_OFFSET  0x000F0000ULL
#define RP1_BLOCK_SIZE         0x1000u

/* IO_BANK0 per-pin word indices */
#define GPIO_STATUS(pin)       ((unsigned)(pin) * 2u)
#define GPIO_CTRL(pin)         ((unsigned)(pin) * 2u + 1u)
#define CTRL_FUNCSEL_MASK      0x1fu
#define FUNCSEL_SYS_RIO        5u

/* PADS_BANK0 word index + bits */
#define PAD_REG(pin)           (1u + (unsigned)(pin))
#define PAD_SLEWFAST           (1u << 0)
#define PAD_SCHMITT            (1u << 1)
#define PAD_PDE                (1u << 2)
#define PAD_PUE                (1u << 3)
#define PAD_IE                 (1u << 6)
#define PAD_OD                 (1u << 7)

volatile uint32_t *rp1_io_bank0;
volatile uint32_t *rp1_sys_rio0;
volatile uint32_t *rp1_pads_bank0;

static uint32_t saved_ctrl[RP1_GPIO_BANK0_NPINS];
static uint32_t saved_pad[RP1_GPIO_BANK0_NPINS];
static uint8_t  saved[RP1_GPIO_BANK0_NPINS];

/* PROT_NOCACHE is essential: a cached mapping returns stale pin levels. */
static volatile uint32_t *rp1_map_block(uint64_t phys_offset, uint32_t size)
{
    uint64_t phys = RP1_BAR0_PHYS + phys_offset;
    void *va = mmap_device_memory(NULL, size,
                                  PROT_READ | PROT_WRITE | PROT_NOCACHE,
                                  MAP_SHARED, phys);
    if (va == MAP_FAILED) {
        fprintf(stderr,
                "[rp1_gpio] mmap failed at phys=0x%016llx size=%u: %s\n"
                "           (are you root? mmap_device_memory needs "
                "PROCMGR_AID_MEM_PHYS)\n",
                (unsigned long long)phys, size, strerror(errno));
        return NULL;
    }
    return (volatile uint32_t *)va;
}

int rp1_gpio_map(void)
{
    if (rp1_io_bank0 && rp1_sys_rio0 && rp1_pads_bank0)
        return 0;   /* already mapped */
    rp1_io_bank0   = rp1_map_block(RP1_IO_BANK0_OFFSET,   RP1_BLOCK_SIZE);
    rp1_sys_rio0   = rp1_map_block(RP1_SYS_RIO0_OFFSET,   RP1_BLOCK_SIZE);
    rp1_pads_bank0 = rp1_map_block(RP1_PADS_BANK0_OFFSET, RP1_BLOCK_SIZE);
    return (rp1_io_bank0 && rp1_sys_rio0 && rp1_pads_bank0) ? 0 : -1;
}

void rp1_gpio_save(int pin)
{
    if (!saved[pin]) {
        saved_ctrl[pin] = rp1_io_bank0[GPIO_CTRL(pin)];
        saved_pad[pin]  = rp1_pads_bank0[PAD_REG(pin)];
        saved[pin] = 1;
    }
}

void rp1_gpio_restore(int pin)
{
    if (saved[pin]) {
        rp1_sys_rio0[RP1_RIO_OE]        &= ~(1u << pin); /* stop driving */
        rp1_io_bank0[GPIO_CTRL(pin)]     = saved_ctrl[pin];
        rp1_pads_bank0[PAD_REG(pin)]     = saved_pad[pin];
        saved[pin] = 0;
    }
}

void rp1_gpio_claim(int pin, rp1_pull_t pull)
{
    uint32_t pad;

    rp1_gpio_save(pin);

    /* direction: input until asked otherwise */
    rp1_sys_rio0[RP1_RIO_OE] &= ~(1u << pin);

    pad = rp1_pads_bank0[PAD_REG(pin)];
    pad |=  PAD_IE | PAD_SCHMITT;
    pad &= ~(PAD_OD | PAD_PUE | PAD_PDE);
    if (pull == RP1_PULL_UP)   pad |= PAD_PUE;
    if (pull == RP1_PULL_DOWN) pad |= PAD_PDE;
    rp1_pads_bank0[PAD_REG(pin)] = pad;

    rp1_io_bank0[GPIO_CTRL(pin)] =
        (rp1_io_bank0[GPIO_CTRL(pin)] & ~CTRL_FUNCSEL_MASK) | FUNCSEL_SYS_RIO;
}

void rp1_gpio_set_output(int pin, int level)
{
    rp1_gpio_write(pin, level);              /* preset level first: no glitch */
    rp1_sys_rio0[RP1_RIO_OE] |= (1u << pin);
}

void rp1_gpio_set_input(int pin)
{
    rp1_sys_rio0[RP1_RIO_OE] &= ~(1u << pin);
}

void rp1_gpio_write(int pin, int level)
{
    if (level)
        rp1_sys_rio0[RP1_RIO_OUT] |=  (1u << pin);
    else
        rp1_sys_rio0[RP1_RIO_OUT] &= ~(1u << pin);
}

uint32_t rp1_gpio_ctrl_reg(int pin)   { return rp1_io_bank0[GPIO_CTRL(pin)]; }
uint32_t rp1_gpio_status_reg(int pin) { return rp1_io_bank0[GPIO_STATUS(pin)]; }
uint32_t rp1_gpio_pad_reg(int pin)    { return rp1_pads_bank0[PAD_REG(pin)]; }
