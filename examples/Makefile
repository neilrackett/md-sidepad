# Example games for the Atari ST.
# Build inside atarist-toolkit-docker:  stcmd make           (builds all)
#                                       stcmd make invaders  (builds one)

CC      = m68k-atari-mint-gcc
CFLAGS  = -O2 -fomit-frame-pointer -Wall
LDFLAGS = -s

GAMES = invaders sideroids breakpad landingpad sidepong launchpad sidewinder

all: $(GAMES)

invaders:   INVADERS.TOS
sideroids:  SIDEROID.TOS
breakpad:   BREAKPAD.TOS
landingpad: LANDPAD.TOS
sidepong:   SIDEPONG.TOS
launchpad:  LAUNCHPD.TOS
sidewinder: SIDEWIND.TOS

INVADERS.TOS: invaders.c
SIDEROID.TOS: sideroids.c
BREAKPAD.TOS: breakpad.c
LANDPAD.TOS:  landingpad.c
SIDEPONG.TOS: sidepong.c
LAUNCHPD.TOS: launchpad.c
SIDEWIND.TOS: sidewinder.c

%.TOS:
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

clean:
	rm -f *.TOS

.PHONY: all clean $(GAMES)
