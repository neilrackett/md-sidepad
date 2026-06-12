# BREAKPAD — Breakout example for the Atari ST.
# Build inside atarist-toolkit-docker:  stcmd make

CC      = m68k-atari-mint-gcc
CFLAGS  = -O2 -fomit-frame-pointer -Wall
LDFLAGS = -s

TARGET = BREAKPAD.TOS

all: $(TARGET)

$(TARGET): breakpad.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

clean:
	rm -f $(TARGET)

.PHONY: all clean
