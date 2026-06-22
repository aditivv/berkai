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
#   -lpci    : QNX PCI server API (pci_attach, pci_device_find, etc.)
#   -lpthread: POSIX threads (dispatch loop uses it internally)
LDFLAGS = -lpci -lpthread

SRCS    = camera_resmgr.c csi2.c dphy.c
OBJS    = $(SRCS:.c=.o)
TARGET  = camera_resmgr

.PHONY: all clean install

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
	@echo "Built: $(TARGET)"

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

install: $(TARGET)
	cp $(TARGET) /usr/sbin/$(TARGET)
	@echo "Installed: /usr/sbin/$(TARGET)"

clean:
	rm -f $(OBJS) $(TARGET)
