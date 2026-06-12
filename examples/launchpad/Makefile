# LAUNCHPAD — Missile Command example for the Atari ST.
# Build inside atarist-toolkit-docker:  stcmd make

CC      = m68k-atari-mint-gcc
CFLAGS  = -O2 -fomit-frame-pointer -Wall
LDFLAGS = -s

TARGET = LAUNCHPD.TOS

all: $(TARGET)

$(TARGET): launchpad.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

clean:
	rm -f $(TARGET)

.PHONY: all clean
