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

TOOLS   = gpio_peek dht11_cli dht11_decode_test dht11_resmgr

.PHONY: all clean test

all: $(TOOLS)

dht11_resmgr: dht11_resmgr.c dht11_capture.c dht11_decode.c rp1_gpio.c dht11_capture.h dht11_decode.h rp1_gpio.h
	$(CC) $(CFLAGS) -o $@ dht11_resmgr.c dht11_capture.c dht11_decode.c rp1_gpio.c $(LDFLAGS)
	@echo "Built: dht11_resmgr  (run as root: ./dht11_resmgr >/tmp/dht11.log 2>&1 &)"

gpio_peek: gpio_peek.c rp1_gpio.c rp1_gpio.h
	$(CC) $(CFLAGS) -o $@ gpio_peek.c rp1_gpio.c $(LDFLAGS)
	@echo "Built: gpio_peek  (run as root: ./gpio_peek --selftest)"

dht11_cli: dht11_cli.c dht11_capture.c dht11_decode.c dht11_capture.h dht11_decode.h rp1_gpio.c rp1_gpio.h
	$(CC) $(CFLAGS) -o $@ dht11_cli.c dht11_capture.c dht11_decode.c rp1_gpio.c $(LDFLAGS)
	@echo "Built: dht11_cli  (run as root: ./dht11_cli --reads 20)"

# Pure-arithmetic decode test — no GPIO, no root, runs anywhere.
dht11_decode_test: dht11_decode_test.c dht11_decode.c dht11_decode.h
	$(CC) $(CFLAGS) -o $@ dht11_decode_test.c dht11_decode.c $(LDFLAGS)

test: dht11_decode_test
	./dht11_decode_test

clean:
	rm -f *.o $(TOOLS)
