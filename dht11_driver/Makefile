# Makefile for the DHT11 driver tools — QNX SDP 8.0, native build on Pi 5.
# Modeled on camera_driver/Makefile.
#
# Build:
#   cd ~/berkai/dht11_driver && make
#
# Phase 0 tool:
#   su                 # password: root (mmap_device_memory needs it)
#   ./gpio_peek        # watch GPIO17; see gpio_peek.c header for the test

CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -g \
           -D_QNX_SOURCE \
           -I.
LDFLAGS = -lpthread

TOOLS   = gpio_peek

.PHONY: all clean

all: $(TOOLS)

gpio_peek: gpio_peek.c rp1_gpio.c rp1_gpio.h
	$(CC) $(CFLAGS) -o $@ gpio_peek.c rp1_gpio.c $(LDFLAGS)
	@echo "Built: gpio_peek  (run as root: ./gpio_peek --selftest)"

clean:
	rm -f *.o $(TOOLS)
