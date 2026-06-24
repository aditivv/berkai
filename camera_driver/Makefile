# Makefile for camera_resmgr — QNX SDP 8.0, native build on Raspberry Pi 5
#
# Build:
#   cd ~/berkai/camera_driver && make
#
# Install:
#   make install   (copies to /usr/sbin/camera_resmgr)
#
# Run:
#   /usr/sbin/camera_resmgr &           # normal
#   /usr/sbin/camera_resmgr --test &    # with sensor color bars

CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -g \
           -D_QNX_SOURCE \
           -I.

# Libraries:
#   -lpthread: POSIX threads (dispatch loop uses it internally)
#   Note: no -lpci needed — RP1 registers accessed via mmap_device_memory()
#   directly at the hardcoded physical base 0x1f00000000.
LDFLAGS = -lpthread

SRCS    = camera_resmgr.c csi2.c dphy.c
OBJS    = $(SRCS:.c=.o)
TARGET  = camera_resmgr

# Standalone diagnostic: dumps RP1 clock/GPIO registers from QNX (no Linux boot).
DUMP_TARGET = rp1_clk_dump

.PHONY: all clean install

all: $(TARGET) $(DUMP_TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
	@echo "Built: $(TARGET)"

# Single-file tool, built independently of the resmgr.
$(DUMP_TARGET): rp1_clk_dump.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)
	@echo "Built: $(DUMP_TARGET)  (run: ./$(DUMP_TARGET) | tee clk_dump.txt)"

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

install: $(TARGET)
	cp $(TARGET) /usr/sbin/$(TARGET)
	@echo "Installed: /usr/sbin/$(TARGET)"

clean:
	rm -f $(OBJS) $(TARGET) $(DUMP_TARGET)
