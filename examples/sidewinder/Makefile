# SIDEWINDER — Snake example for the Atari ST.
# Build inside atarist-toolkit-docker:  stcmd make

CC      = m68k-atari-mint-gcc
CFLAGS  = -O2 -fomit-frame-pointer -Wall
LDFLAGS = -s

TARGET = SIDEWIND.TOS

all: $(TARGET)

$(TARGET): sidewinder.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

clean:
	rm -f $(TARGET)

.PHONY: all clean
