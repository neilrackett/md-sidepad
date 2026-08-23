/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * File: xpadstate.h
 * Description: Publish the connected controller as an Xpad block.
 *
 * Xpad (https://github.com/neilrackett/atarist-xpad) is a transport
 * neutral shared state block, found through the cookie jar, that carries
 * every button, stick and trigger of a modern pad. Publishing one costs
 * the ST nothing: the block lives in the cartridge window, so a consumer
 * reads it directly and no hook is involved at all.
 *
 * This runs alongside the joyvec and mouse injection rather than
 * replacing them. Those are unaffected by anything here, and the block
 * always reports the whole pad regardless of how they are configured:
 * mouse mode changes what gets injected, never what Xpad reports.
 */

#ifndef XPADSTATE_H
#define XPADSTATE_H

#include <stdint.h>

/*
 * The block sits at the start of APP_FREE, which nothing else uses.
 * userfw.s installs a cookie pointing at it, so XPAD_BLOCK there must
 * match XPADSTATE_ST_BLOCK below. All three sides spell the address out
 * rather than share a definition, because they are built by different
 * toolchains; xpadstate.c asserts this one against the framework's
 * CHANDLER_APP_FREE_OFFSET so they cannot drift apart quietly.
 */
#define XPADSTATE_ROM4_BASE 0x00FA0000UL /* window base on the m68k */

#define XPADSTATE_BLOCK_OFFSET 0x2300u
#define XPADSTATE_BLOCK_SIZE 128
#define XPADSTATE_PROVIDER_OFFSET \
  (XPADSTATE_BLOCK_OFFSET + XPADSTATE_BLOCK_SIZE)
#define XPADSTATE_PROVIDER_SIZE 32

#define XPADSTATE_ST_BLOCK (XPADSTATE_ROM4_BASE + XPADSTATE_BLOCK_OFFSET)
#define XPADSTATE_ST_PROVIDER (XPADSTATE_ROM4_BASE + XPADSTATE_PROVIDER_OFFSET)

/* One pad for now: Sidepad pairs a single controller. The block reserves
 * room for XPAD_MAX_PADS so raising this needs no address to move. */
#define XPADSTATE_PAD_COUNT 1

/* Write the parts of the block that never change, and an empty pad.
 * Call once, before the first xpadstate_publish(). */
void xpadstate_init(void);

/* Sample the controller and publish it. Cheap enough to call at the
 * joystick poll rate. */
void xpadstate_publish(void);

#endif  // XPADSTATE_H
