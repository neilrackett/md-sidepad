/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * File: xpadstate_test.c
 * Description: Host test for the Xpad block writer.
 *
 * The RP is little endian and the m68k is big endian, and the window is
 * served as 16 bit words, so every field has to be placed by hand. That
 * is the part worth checking without hardware, and it is checked the
 * only way that means anything: build the block, then read it back the
 * way the ST would, bytes within each word the other way round.
 *
 * Stub headers in stub/ stand in for the Pico SDK, so this builds with
 * a plain host compiler. Run it with `make -C rp/test`.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "include/xpad.h"
#include "include/xpadstate.h"
#include "include/controller.h"

#define RELEASE_VERSION "9.9.9"

unsigned char xpadtest_rom[0x10000];

/* xpadstate.c pulls its input from here; stand in for controller.c. */
static controller_state_t gState;
void controller_getState(controller_state_t *out) { *out = gState; }

#include "../src/xpadstate.c"

/* The ST's view: bytes within each 16-bit word are the other way round. */
static uint8_t st8(uint32_t off) { return xpadtest_rom[off ^ 1u]; }
static uint16_t st16(uint32_t off) { return (uint16_t)((st8(off) << 8) | st8(off + 1)); }
static uint32_t st32(uint32_t off) { return ((uint32_t)st16(off) << 16) | st16(off + 2); }

static int failures;
static void check(int ok, const char *what)
{
    printf("%-52s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) failures++;
}

#define B XPADSTATE_BLOCK_OFFSET

int main(void)
{
    uint32_t padOff;
    uint16_t seq0;
    uint8_t act0;
    int i, ok;

    printf("xpadstate byte order and layout\n\n");

    check(CHANDLER_APP_FREE_OFFSET == 0x2300, "APP_FREE is where chandler.h says");

    memset(xpadtest_rom, 0xa5, sizeof(xpadtest_rom));
    xpadstate_init();

    check(st32(B + 0) == XPAD_MAGIC, "magic reads as 'XPAD' on the ST");
    check(st16(B + 4) == XPAD_VERSION, "version");
    check(st16(B + 6) == HDR_SIZE, "hdr_size");
    check(st16(B + 8) == 26, "pads_offset");
    check(st16(B + 10) == XPAD_PAD_SIZE_V1, "pad_size");
    check(st16(B + 12) == (XPAD_CAP_ANALOG | XPAD_CAP_HOTPLUG), "caps");
    check(st8(B + 16) == XPADSTATE_PAD_COUNT, "pad_count is a byte at offset 16");
    check(st8(B + 17) == 0, "active is a byte at offset 17");
    check(st32(B + 18) == XPADSTATE_ST_PROVIDER, "provider points into the window");
    check(st32(B + 22) == 0, "req is NULL");

    /* Everything xpad_valid() insists on. */
    check(st16(B + 10) >= XPAD_PAD_SIZE_V1, "pad_size clears the v1 floor");
    check(st16(B + 8) >= XPAD_HDR_FIXED, "pads start past the fixed header");
    check(st16(B + 8) <= st16(B + 6), "pads start inside the block");
    check((uint32_t)st8(B + 16) * 2u * st16(B + 10) <=
              (uint32_t)(st16(B + 6) - st16(B + 8)),
          "both pad buffers fit inside hdr_size");

    ok = 1;
    for (i = 0; i < 11; i++)
    {
        char want = "MD/Sidepad "[i];
        if ((char)st8(XPADSTATE_PROVIDER_OFFSET + i) != want) ok = 0;
    }
    check(ok, "provider string reads the right way round");

    /* A pad with something in every field. */
    memset(&gState, 0, sizeof(gState));
    gState.connected = true;
    gState.btnA = true;    /* south */
    gState.btnY = true;    /* top -> north, not XPAD_Y */
    gState.btnRS = true;   /* right thumb */
    gState.padLeft = true;
    gState.lx = 1.0f;      /* full right */
    gState.ly = 0.0f;      /* full up */
    gState.rx = 0.5f;      /* centred */
    gState.lt = 1.0f;
    gState.rt = 0.0f;

    seq0 = st16(B + 14);
    act0 = st8(B + 17);
    xpadstate_publish();

    check(st16(B + 14) == (uint16_t)(seq0 + 1), "seq advances on commit");
    check(st8(B + 17) == (act0 ^ 1), "active flips to the other buffer");

    padOff = B + 26 + (uint32_t)st8(B + 17) * XPADSTATE_PAD_COUNT * XPAD_PAD_SIZE_V1;

    check(st32(padOff + 0) == (XPAD_SOUTH | XPAD_NORTH | XPAD_THUMBR | XPAD_LEFT |
                               XPAD_TL2),
          "buttons map by position, with the trigger digitised");
    check((int8_t)st8(padOff + 4) == 127, "lx full right is +127");
    check((int8_t)st8(padOff + 5) == -127, "ly full up is -127");
    check((int8_t)st8(padOff + 6) == 0, "rx centred is 0");
    check(st8(padOff + 8) == 255, "lt fully pulled is 255");
    check(st8(padOff + 9) == 0, "rt released is 0");
    check(st8(padOff + 10) == XPAD_TYPE_GAMEPAD, "type says gamepad");
    check(st8(padOff + 11) == (XPAD_PAD_ANALOG | XPAD_PAD_WIRELESS), "flags");

    /* Disconnect: an empty slot, not a pad frozen at its last state. */
    gState.connected = false;
    xpadstate_publish();
    padOff = B + 26 + (uint32_t)st8(B + 17) * XPADSTATE_PAD_COUNT * XPAD_PAD_SIZE_V1;
    check(st8(padOff + 10) == XPAD_TYPE_NONE, "disconnecting empties the slot");
    check(st32(padOff + 0) == 0, "and clears the buttons");

    /* Buffers must alternate, and never be the one being read. */
    ok = 1;
    for (i = 0; i < 4; i++)
    {
        uint8_t before = st8(B + 17);
        xpadstate_publish();
        if (st8(B + 17) == before) ok = 0;
    }
    check(ok, "every commit swaps buffers");

    check(HDR_SIZE <= XPADSTATE_BLOCK_SIZE, "the block fits its reservation");

    printf("\n%s\n", failures ? "FAILED" : "all checks passed");
    return failures ? 1 : 0;
}
