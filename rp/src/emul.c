/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * File: emul.c
 * Author: Diego Parrilla Santamaría
 * Date: February 2025, February 2026
 * Copyright: 2025-2026 - GOODDATA LABS
 * Description: Template code for the core emulation
 */

#include "emul.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// inclusw in the C file to avoid multiple definitions
#include "aconfig.h"
#include "chandler.h"
#include "commemul.h"
#include "constants.h"
#include "controller.h"
#include "debug.h"
#include "display.h"
#include "ff.h"
#include "gconfig.h"
#include "memfunc.h"
#include "network.h"
#include "pico/stdlib.h"
#include "reset.h"
#include "romemul.h"
#include "sdcard.h"
#include "select.h"
#include "target_firmware.h"  // Include the target firmware binary
#include "term.h"

#define SLEEP_LOOP_MS 50
#define UI_REFRESH_MS 100

// Shared-variable slot (0-59, all app-free) carrying the digital BT joystick
// state to the m68k userfw hooks. The LSB lands at m68k $FA201F.
#define SIDEPAD_BT_JOY_SLOT 3

// Shared-variable slot feeding the m68k boot_gem exit banner. The banner text
// and the VT52 clear live in the cartridge ROM (main.s); the RP only supplies
// the connected flag here (LSB at m68k $FA2023), which boot_gem uses to pick
// between the connected / not-connected banner. The joystick slot (3) never
// overlaps this.
#define SIDEPAD_EXIT_FLAG_SLOT 4

// Shared-variable slot carrying the BT mouse packet to the m68k userfw hook
// when mouse mode is on (right stick -> GEM cursor). Packed big-endian, so the
// m68k reads (at $FA2024): byte0 = enabled, byte1 = buttons (bit1 = left / R3),
// byte2 = signed dx, byte3 = signed dy (per-frame deltas). Step 2 only
// publishes it; the m68k does not read it yet (that is step 3).
#define SIDEPAD_BT_MOUSE_SLOT 5

// Shared-variable slot carrying the VBL/ETV hook-mode flag to the m68k userfw
// installer, read once when it installs the hook (LSB at m68k $FA202B): 0 = VBL
// ($70), 1 = ETV ($400). Published before the CMD_START burst in
// exitToGemDesktop so it is latched when the installer runs.
#define SIDEPAD_HOOK_MODE_SLOT 6

// Right-stick-as-mouse tuning (analog, proportional). Deadzone around centre
// (0.5) below which the stick is idle, and the max per-frame cursor delta at
// full deflection. Both are feel parameters to tune on hardware.
#define SIDEPAD_MOUSE_DEADZONE 0.15f
#define SIDEPAD_MOUSE_MAX_SPEED 10

enum {
  APP_MODE_SETUP = 255  // Setup
};

// Number of terminal rows used by the controller UI (last row is the terminal
// status line, owned by term.c).
#define UI_ROW_COUNT (TERM_SCREEN_SIZE_Y - 1)

// Throttles redraws of the live controller UI to UI_REFRESH_MS.
static absolute_time_t uiRefreshTime;

static void appendFmt(char *buffer, size_t bufferSize, size_t *offset,
                      const char *fmt, ...) {
  if ((buffer == NULL) || (offset == NULL) || (*offset >= bufferSize) ||
      (fmt == NULL)) {
    return;
  }

  va_list args;
  va_start(args, fmt);
  size_t remaining = bufferSize - *offset;
  int written = vsnprintf(buffer + *offset, remaining, fmt, args);
  va_end(args);

  if (written < 0) {
    return;
  }

  size_t writeSize = (size_t)written;
  if (writeSize >= remaining) {
    *offset = bufferSize - 1;
    return;
  }

  *offset += writeSize;
}

static void appendMoveAndClearLine(char *buffer, size_t bufferSize,
                                   size_t *offset, uint8_t row) {
  appendFmt(buffer, bufferSize, offset,
            "\x1B"
            "Y%c%c\x1B"
            "K",
            (char)(TERM_POS_Y + row), (char)(TERM_POS_X));
}

static void appendLineAt(char *buffer, size_t bufferSize, size_t *offset,
                         uint8_t row, const char *text) {
  char line[TERM_SCREEN_SIZE_X + 1] = {0};
  if (text != NULL) {
    snprintf(line, sizeof(line), "%-39.39s", text);
  } else {
    snprintf(line, sizeof(line), "%-39s", "");
  }
  appendMoveAndClearLine(buffer, bufferSize, offset, row);
  appendFmt(buffer, bufferSize, offset, "%s", line);
}

// Layout of the digital joystick indicator drawn with text characters.
// A direction box on the left holds a solid block that moves to one of nine
// positions (centre + 8 compass directions); a fire box on the right fills
// solid when any button is pressed.
#define DIR_BOX_INNER_W 9  // inner playfield width in chars
#define DIR_BOX_INNER_H 9  // inner playfield height in rows
#define DIR_BLOCK_SIZE 3   // direction blob is DIR_BLOCK_SIZE x DIR_BLOCK_SIZE
#define DIR_BOX_ROW 6      // first screen row of the direction box
#define DIR_BOX_COL 0      // left screen column of the direction box
#define FIRE_BOX_INNER_W 3
#define FIRE_BOX_INNER_H 3
// Direction box spans cols 0..(DIR_BOX_COL + DIR_BOX_INNER_W + 1) = 0..10;
// place the fire box at col 12 to leave exactly one empty column (11) between
// them.
#define FIRE_BOX_COL 12  // left screen column of the fire box

// Mouse mode draws a second pair: an exact clone of the joystick pair shifted
// to the right half (direction box left, button box on its right). Direction
// box cols 22..32, button box cols 34..38 (the rightmost rendered cell). Titles
// sit one blank row below both boxes (box bottom is DIR_BOX_ROW + INNER_H + 1).
#define MOUSE_DIR_BOX_COL 22
#define MOUSE_FIRE_BOX_COL 34
#define VIS_TITLE_ROW (DIR_BOX_ROW + DIR_BOX_INNER_H + 3)

// Right-stick-as-mouse toggle ([M] in the controller UI). Persisted to per-app
// config (ACONFIG_PARAM_MOUSE): loaded at startup and saved on each toggle, so
// the choice survives reboots.
static bool mouseMode = false;

// VBL/ETV hook-mode toggle ([H] in the controller UI). false = VBL ($70), true
// = ETV ($400). Defaults to ETV (ACONFIG_PARAM_HOOK default is "true"); this
// initial value is overwritten by loadHookMode() from per-app config at startup.
// Published to shared-var slot 6 (inline in exitToGemDesktop) on the way out to
// the desktop so the m68k userfw installer hooks the matching vector. ETV
// survives programs that replace the VBL vector; the m68k rate-limits it to
// ~50 Hz to match the VBL route.
static bool etvMode = true;

// Help-screen toggle (dedicated ST Help key, scan code SCAN_HELP). When true
// the controller visualiser is replaced by a static help page; the title and
// footer rows stay so Help/Esc still work. Debounced so Help-key auto-repeat
// does not flicker the screen.
#define SCAN_HELP 0x62
static bool helpVisible = false;
static absolute_time_t helpLastToggle;
static bool helpToggleValid = false;

// Auto-exit countdown: once a controller connects, the terminal counts down for
// SIDEPAD_AUTOEXIT_SECONDS then exits to the desktop as if ESC was pressed. Any
// key cancels the running countdown for the rest of the session
// (autoExitCancelled is sticky, so a later disconnect/reconnect does not
// restart it). A disconnect alone just stops the current run; reconnecting
// restarts it unless it was key-cancelled.
#define SIDEPAD_AUTOEXIT_SECONDS 10
static bool autoExitActive = false;
static bool autoExitCancelled = false;
static absolute_time_t autoExitDeadline;

// Box-drawing and block glyphs from u8g2_font_amstrad_cpc_extended_8f. Codes
// confirmed by decoding the font bitmaps (see GLYPH debug screen):
//   128 vertical, 129 horizontal, 131/132/133/130 = TL/TR/BR/BL corners,
//   157 = solid filled square.
#define GLYPH_V ((char)128)          // vertical line
#define GLYPH_H ((char)129)          // horizontal line
#define GLYPH_CORNER_TL ((char)131)  // top-left
#define GLYPH_CORNER_TR ((char)132)  // top-right
#define GLYPH_CORNER_BR ((char)133)  // bottom-right
#define GLYPH_CORNER_BL ((char)130)  // bottom-left
#define BLOCK_CHAR ((char)157)       // solid filled square

// Draws a bordered box into lines at the given top-left cell using the
// line-drawing glyphs. The inner area is filled by fillChar; pass '\0' to leave
// it blank. The block, when blockRow/blockCol are >= 0, overwrites a
// blockSize x blockSize square of inner cells with the solid glyph.
static void drawBox(char lines[][TERM_SCREEN_SIZE_X + 1], uint8_t topRow,
                    uint8_t leftCol, uint8_t innerW, uint8_t innerH,
                    char fillChar, int blockRow, int blockCol,
                    uint8_t blockSize) {
  uint8_t bottomRow = topRow + innerH + 1;
  uint8_t rightCol = leftCol + innerW + 1;

  // Corners.
  lines[topRow][leftCol] = GLYPH_CORNER_TL;
  lines[topRow][rightCol] = GLYPH_CORNER_TR;
  lines[bottomRow][leftCol] = GLYPH_CORNER_BL;
  lines[bottomRow][rightCol] = GLYPH_CORNER_BR;

  // Top and bottom edges.
  for (uint8_t x = 0; x < innerW; x++) {
    lines[topRow][leftCol + 1 + x] = GLYPH_H;
    lines[bottomRow][leftCol + 1 + x] = GLYPH_H;
  }

  // Sides and interior.
  for (uint8_t y = 0; y < innerH; y++) {
    uint8_t row = topRow + 1 + y;
    lines[row][leftCol] = GLYPH_V;
    lines[row][rightCol] = GLYPH_V;
    for (uint8_t x = 0; x < innerW; x++) {
      lines[row][leftCol + 1 + x] = (fillChar != '\0') ? fillChar : ' ';
    }
  }

  if ((blockRow >= 0) && (blockCol >= 0)) {
    for (uint8_t by = 0; by < blockSize; by++) {
      for (uint8_t bx = 0; bx < blockSize; bx++) {
        lines[topRow + 1 + blockRow + by][leftCol + 1 + blockCol + bx] =
            BLOCK_CHAR;
      }
    }
  }
}

// Draws one direction+button visualiser pair: a direction box (with the blob
// pushed to the active 8-way position) at dirCol and a button box at fireCol.
static void drawJoyPair(char lines[][TERM_SCREEN_SIZE_X + 1], uint8_t dirCol,
                        uint8_t fireCol, bool up, bool down, bool left,
                        bool right, bool fire) {
  int blockRow = (DIR_BOX_INNER_H - DIR_BLOCK_SIZE) / 2;
  int blockCol = (DIR_BOX_INNER_W - DIR_BLOCK_SIZE) / 2;
  if (up) blockRow = 0;
  if (down) blockRow = DIR_BOX_INNER_H - DIR_BLOCK_SIZE;
  if (left) blockCol = 0;
  if (right) blockCol = DIR_BOX_INNER_W - DIR_BLOCK_SIZE;

  drawBox(lines, DIR_BOX_ROW, dirCol, DIR_BOX_INNER_W, DIR_BOX_INNER_H, '\0',
          blockRow, blockCol, DIR_BLOCK_SIZE);
  drawBox(lines, DIR_BOX_ROW, fireCol, FIRE_BOX_INNER_W, FIRE_BOX_INNER_H,
          fire ? BLOCK_CHAR : '\0', -1, -1, 1);
}

// Left-aligns a title under a direction box, starting one char in from the
// box's left edge (dirCol + 1, the box's inner-left column) at the given row.
static void drawTitle(char lines[][TERM_SCREEN_SIZE_X + 1], uint8_t row,
                      uint8_t dirCol, const char *text) {
  size_t len = strlen(text);
  size_t start = dirCol + 1;
  if (start + len <= TERM_SCREEN_SIZE_X) {
    memcpy(lines[row] + start, text, len);
  }
}

static void renderControllerScreen(const controller_state_t *state,
                                   bool forceFullRefresh) {
  static char previousLines[UI_ROW_COUNT][TERM_SCREEN_SIZE_X + 1] = {{0}};
  char currentLines[UI_ROW_COUNT][TERM_SCREEN_SIZE_X + 1] = {{0}};

  if (state == NULL) {
    return;
  }

  // Pad every row to full width so stale characters are always overwritten.
  for (uint8_t row = 0; row < UI_ROW_COUNT; row++) {
    memset(currentLines[row], ' ', TERM_SCREEN_SIZE_X);
    currentLines[row][TERM_SCREEN_SIZE_X] = '\0';
  }

  // Title row: two leading rule cells, "MD/Sidepad", then a rule to the edge
  // (--MD/Sidepad------).
  const char *title = "MD/Sidepad";
  size_t titleLen = strlen(title);
  const size_t titleLead = 2;
  memset(currentLines[0], GLYPH_H, titleLead);
  memcpy(currentLines[0] + titleLead, title, titleLen);
  memset(currentLines[0] + titleLead + titleLen, GLYPH_H,
         TERM_SCREEN_SIZE_X - titleLead - titleLen);

  // Prefer the connected controller's name; fall back to the status message.
  const char *status;
  if (state->connected && state->deviceName[0] != '\0') {
    status = state->deviceName;
  } else if (state->status[0] != '\0') {
    status = state->status;
  } else {
    status = "N/A";
  }
  size_t statusLen = strlen(status);
  if (statusLen > TERM_SCREEN_SIZE_X) statusLen = TERM_SCREEN_SIZE_X;
  memcpy(currentLines[2], status, statusLen);

  // Auto-exit countdown message, one line below the status/name line. Shown
  // only while the countdown is running; when it is not, row 3 stays blank and
  // the line diff clears any previous message.
  if (autoExitActive) {
    int64_t remUs =
        absolute_time_diff_us(get_absolute_time(), autoExitDeadline);
    int secs = (int)((remUs + 999999) / 1000000);  // ceil to whole seconds
    if (secs < 1) secs = 1;  // never display 0; loop exits
    char countdown[TERM_SCREEN_SIZE_X + 1];
    int n = snprintf(countdown, sizeof(countdown),
                     "Exit in %ds (any key to cancel)", secs);
    size_t countdownLen = (n < 0) ? 0 : (size_t)n;
    if (countdownLen > TERM_SCREEN_SIZE_X) countdownLen = TERM_SCREEN_SIZE_X;
    memcpy(currentLines[3], countdown, countdownLen);
  }

  // Visualiser(s). Mouse mode off: a single pair shows the combined joystick
  // (both sticks + D-pad), exactly as before. Mouse mode on: the left pair
  // shows the joystick (left stick + D-pad, fire excluding the R3 mouse click)
  // and a cloned right pair shows the mouse (right stick + R3), each titled
  // below. Step 1 is visual only: the actual joystick injection still uses any*
  // either way, so the pad reacts identically with mouse mode on or off.
  if (mouseMode) {
    bool joyFire = state->btnA || state->btnB || state->btnX || state->btnY ||
                   state->btnLB || state->btnRB || state->btnLS ||
                   state->lt > CONTROLLER_TRIGGER_THRESHOLD ||
                   state->rt > CONTROLLER_TRIGGER_THRESHOLD;
    drawJoyPair(currentLines, DIR_BOX_COL, FIRE_BOX_COL, state->padUp,
                state->padDown, state->padLeft, state->padRight, joyFire);
    drawJoyPair(currentLines, MOUSE_DIR_BOX_COL, MOUSE_FIRE_BOX_COL,
                state->rstickUp, state->rstickDown, state->rstickLeft,
                state->rstickRight, state->btnRS);
    drawTitle(currentLines, VIS_TITLE_ROW, DIR_BOX_COL, "Joystick");
    drawTitle(currentLines, VIS_TITLE_ROW, MOUSE_DIR_BOX_COL, "Mouse (RS)");
  } else {
    drawJoyPair(currentLines, DIR_BOX_COL, FIRE_BOX_COL, state->anyUp,
                state->anyDown, state->anyLeft, state->anyRight,
                state->anyButton);
  }

  // Help page: when the Help key is toggled on, replace the controller content
  // area (everything between the title row and the bottom rule) with the static
  // help text. Drawn last so it overwrites the status/visualiser written above;
  // the shared title and footer rows are untouched. For now the page is just
  // the word HELP.
  if (helpVisible) {
    for (uint8_t row = 1; row <= UI_ROW_COUNT - 3; row++) {
      memset(currentLines[row], ' ', TERM_SCREEN_SIZE_X);
    }
    memcpy(currentLines[2], "HELP", 4);
  }

  // Bottom two rows: a horizontal rule then the controls. Each
  // currentLines[row] is one independent screen line, so they go in separate
  // rows (not one string with \n). Use the box-drawing horizontal glyph, which
  // sits mid-cell, rather than '_' (cell bottom) so the rule clears the
  // controls text below it.
  memset(currentLines[UI_ROW_COUNT - 2], GLYPH_H, TERM_SCREEN_SIZE_X);
  // Overlay the build version near the right end of the rule (----vX.Y.Z--).
  // appendLineAt renders TERM_SCREEN_SIZE_X - 1 cells, so anchor to that and
  // leave two trailing rule cells after the version.
  const char *version = RELEASE_VERSION;
  size_t versionLen = strlen(version);
  if (versionLen + 2 < (size_t)(TERM_SCREEN_SIZE_X - 1)) {
    size_t versionCol = (size_t)(TERM_SCREEN_SIZE_X - 1) - 2 - versionLen;
    memcpy(currentLines[UI_ROW_COUNT - 2] + versionCol, version, versionLen);
  }
  // The controls footer (bottom row) is emitted separately below with the key
  // names in reverse video, so it is not part of this plain-char grid (the
  // ESC p/q codes are not fixed-width cells).

  char updates[2048] = {0};
  size_t offset = 0;

  if (forceFullRefresh) {
    appendFmt(updates, sizeof(updates), &offset,
              "\x1B"
              "E");
  }

  for (uint8_t row = 0; row < UI_ROW_COUNT; row++) {
    if (forceFullRefresh ||
        (strcmp(currentLines[row], previousLines[row]) != 0)) {
      appendLineAt(updates, sizeof(updates), &offset, row, currentLines[row]);
      snprintf(previousLines[row], sizeof(previousLines[row]), "%s",
               currentLines[row]);
    }
  }

  // Controls footer with reverse-video key names (VT52 ESC p = on, ESC q =
  // off). Emitted outside the plain-char grid because the escape codes are not
  // fixed-width cells. Only on a full refresh: the row is static, so it
  // persists across partial refreshes (the grid never repaints this row).
  if (forceFullRefresh) {
    appendMoveAndClearLine(updates, sizeof(updates), &offset, UI_ROW_COUNT - 1);
    appendFmt(updates, sizeof(updates), &offset,
              "\x1B"
              "p"
              "Help"
              "\x1B"
              "q"
              " "
              "\x1B"
              "p"
              "Esc"
              "\x1B"
              "q"
              " "
              "\x1B"
              "p"
              "B"
              "\x1B"
              "q"
              "ooster "
              "\x1B"
              "p"
              "P"
              "\x1B"
              "q"
              "air "
              "\x1B"
              "p"
              "M"
              "\x1B"
              "q"
              "ouse "
              "\x1B"
              "p"
              "H"
              "\x1B"
              "q"
              "ook:%s ",
              etvMode ? "ETV" : "VBL");
  }

  // Park the cursor block in the empty bottom-right cell so it doesn't sit on
  // top of the footer text. (The terminal always draws a block cursor; this
  // keeps it out of the way without changing the shared terminal code.)
  appendFmt(updates, sizeof(updates), &offset,
            "\x1B"
            "Y%c%c",
            (char)(TERM_POS_Y + (UI_ROW_COUNT - 1)),
            (char)(TERM_POS_X + (TERM_SCREEN_SIZE_X - 1)));
  term_printString(updates);
}

// Render the controller UI from the live controller.c snapshot.
static void refreshControllerUI(bool forceFullRefresh) {
  controller_state_t state = {0};
  controller_getState(&state);
  renderControllerScreen(&state, forceFullRefresh);
}

// Keep active loop or exit
static bool keepActive = true;

// Should we reset the device, or jump to the booster app?
// By default, we reset the device.
static bool resetDeviceAtBoot = true;

// Set when the user chooses [ESC] Desktop: leave the terminal loop, install the
// m68k joystick hook, and continue booting to the GEM desktop instead of
// resetting / returning to Booster.
static bool exitToDesktop = false;

// Number of CMD_START frames to hold while the m68k installs the joystick hook
// on the way out to the desktop (the installer is idempotent).
#define SIDEPAD_HOOK_START_TICKS 10
// Number of CMD_BOOT_GEM frames to hold so the m68k reliably latches the boot
// handoff. A single sentinel write can race the m68k's vsync-paced
// check_commands poll and be dropped, leaving it stuck redrawing the terminal;
// repeating it guarantees boot_gem runs. Once boot_gem rts's, the m68k leaves
// the print loop and stops reading the sentinel, so the extra writes are inert.
#define SIDEPAD_BOOT_GEM_TICKS 10
// Joystick state refresh interval once a game is running (~50 Hz).
#define SIDEPAD_JOY_POLL_MS 20

// Pack the controller's 8-way + fire signals into the IKBD joystick byte the
// m68k VBL hook consumes and publish it to the shared-variable slot the hook
// polls. Direction bits use the standard IKBD encoding, confirmed on hardware
// via JOYMOUT: bit0=Up, bit1=Down, bit2=Left, bit3=Right. Fire is carried in
// bit6 here and moved to the IKBD fire bit (bit7) by userfw.s.
//
// In mouse mode the right stick drives the cursor, so the joystick uses left
// stick + D-pad only (pad*) and fire excludes the right-stick click (R3, now
// the mouse button). Mouse mode off = the union of both sticks + D-pad, as
// before.
static void writeBtJoyState(void) {
  controller_state_t btState = {0};
  controller_getState(&btState);
  uint8_t bt = 0;
  if (mouseMode) {
    if (btState.padUp) bt |= 0x01;
    if (btState.padDown) bt |= 0x02;
    if (btState.padLeft) bt |= 0x04;
    if (btState.padRight) bt |= 0x08;
    // Fire = any face/shoulder/left-thumb button or pulled trigger, but NOT R3
    // (the mouse click).
    if (btState.btnA || btState.btnB || btState.btnX || btState.btnY ||
        btState.btnLB || btState.btnRB || btState.btnLS ||
        btState.lt > CONTROLLER_TRIGGER_THRESHOLD ||
        btState.rt > CONTROLLER_TRIGGER_THRESHOLD) {
      bt |= 0x40;
    }
  } else {
    if (btState.anyUp) bt |= 0x01;
    if (btState.anyDown) bt |= 0x02;
    if (btState.anyLeft) bt |= 0x04;
    if (btState.anyRight) bt |= 0x08;
    if (btState.anyButton) bt |= 0x40;
  }
  uint32_t romBase = (uint32_t)&__rom_in_ram_start__;
  SET_SHARED_VAR(SIDEPAD_BT_JOY_SLOT, (uint32_t)bt, romBase,
                 CHANDLER_SHARED_VARIABLES_OFFSET);
}

// Map a centred analog axis (0..1, 0.5 = rest) to a signed per-frame cursor
// delta: zero inside the deadzone, then a linear+quadratic blend up to
// SIDEPAD_MOUSE_MAX_SPEED at full deflection. The blend (0.5*t + 0.5*t^2)
// halves the slope near the deadzone edge -> half the minimum speed for finer
// slow control, while full deflection still reaches the same max.
static int8_t mouseAxisDelta(float axis) {
  float off = axis - 0.5f;  // -0.5 .. +0.5
  float mag = off < 0.0f ? -off : off;
  if (mag <= SIDEPAD_MOUSE_DEADZONE) {
    return 0;
  }
  float t = (mag - SIDEPAD_MOUSE_DEADZONE) / (0.5f - SIDEPAD_MOUSE_DEADZONE);
  if (t > 1.0f) t = 1.0f;  // clamp to full deflection
  float scaled = (0.5f * t + 0.5f * t * t) * (float)SIDEPAD_MOUSE_MAX_SPEED;
  int delta = (int)(scaled + 0.5f);
  if (delta > SIDEPAD_MOUSE_MAX_SPEED) delta = SIDEPAD_MOUSE_MAX_SPEED;
  return (int8_t)(off < 0.0f ? -delta : delta);
}

// Publish the right-stick-as-mouse packet to the shared region for the m68k VBL
// hook: enabled flag, left-button (R3) state, and signed per-frame dx/dy. When
// mouse mode is off, publish a disabled/idle packet so the hook does nothing.
// Step 2: the m68k does not consume slot 5 yet, so this is verifiable only via
// the serial log below.
static void writeBtMouseState(void) {
  controller_state_t st = {0};
  controller_getState(&st);
  uint8_t enabled = 0;
  uint8_t buttons = 0;
  int8_t dx = 0;
  int8_t dy = 0;
  if (mouseMode) {
    enabled = 1;
    dx = mouseAxisDelta(st.rx);
    dy = mouseAxisDelta(st.ry);
    if (st.btnRS) buttons |= 0x02;  // IKBD left button = bit 1 ($FA header)
  }
  uint32_t v = ((uint32_t)enabled << 24) | ((uint32_t)buttons << 16) |
               ((uint32_t)(uint8_t)dx << 8) | (uint32_t)(uint8_t)dy;
  uint32_t romBase = (uint32_t)&__rom_in_ram_start__;
  SET_SHARED_VAR(SIDEPAD_BT_MOUSE_SLOT, v, romBase,
                 CHANDLER_SHARED_VARIABLES_OFFSET);

  // Step 2 verification: log only when the packed packet changes, so the serial
  // console shows the analog mapping without flooding at the poll rate.
  static uint32_t prevV = 0;
  if (v != prevV) {
    prevV = v;
    DPRINTF("Mouse slot: en=%u btn=0x%02X dx=%d dy=%d\n", (unsigned)enabled,
            (unsigned)buttons, (int)dx, (int)dy);
  }
}

// Publish the connection state to the shared region so the m68k boot_gem
// handler can pick which fixed banner ("Sidepad connected" / "Sidepad not
// connected") to print from ROM on its way out to the desktop. Must run before
// CMD_BOOT_GEM so the flag is in place when the m68k reads it.
static void writeExitMessage(void) {
  controller_state_t st = {0};
  controller_getState(&st);
  uint32_t romBase = (uint32_t)&__rom_in_ram_start__;

  // Connection state -> slot 4 (m68k boot_gem picks the banner from ROM).
  SET_SHARED_VAR(SIDEPAD_EXIT_FLAG_SLOT, st.connected ? 1u : 0u, romBase,
                 CHANDLER_SHARED_VARIABLES_OFFSET);
}

// Load the persisted mouse-mode flag from per-app config into mouseMode.
static void loadMouseMode(void) {
  SettingsConfigEntry *entry =
      settings_find_entry(aconfig_getContext(), ACONFIG_PARAM_MOUSE);
  mouseMode = (entry != NULL) && (strcmp(entry->value, "true") == 0);
  DPRINTF("Mouse mode loaded from config: %s\n", mouseMode ? "on" : "off");
}

// Persist the current mouse-mode flag to per-app config flash.
static void saveMouseMode(void) {
  settings_put_bool(aconfig_getContext(), ACONFIG_PARAM_MOUSE, mouseMode);
  settings_save(aconfig_getContext(), true);
}

// Load the persisted hook-mode flag from per-app config into etvMode.
static void loadHookMode(void) {
  SettingsConfigEntry *entry =
      settings_find_entry(aconfig_getContext(), ACONFIG_PARAM_HOOK);
  etvMode = (entry != NULL) && (strcmp(entry->value, "true") == 0);
  DPRINTF("Hook mode loaded from config: %s\n", etvMode ? "ETV" : "VBL");
}

// Persist the current hook-mode flag to per-app config flash.
static void saveHookMode(void) {
  settings_put_bool(aconfig_getContext(), ACONFIG_PARAM_HOOK, etvMode);
  settings_save(aconfig_getContext(), true);
}

// Leave the terminal for the GEM desktop and never return: install the m68k
// joystick hook, publish the exit banner, hand off to boot_gem, then loop
// forever republishing the joystick byte the resident VBL hook reads. Invoked
// once the main loop exits with exitToDesktop set.
static void exitToGemDesktop(void) {
  // Persist the mouse-mode and hook-mode choices now, on the way out to the
  // desktop: the main terminal loop has just exited and the m68k handoff bursts
  // have not started, so this is the one quiescent point for the flash write
  // (the Atari reads ROM from RAM, so the bus is unaffected by the brief
  // write).
  saveMouseMode();
  saveHookMode();

  // The joystick injection hook only matters once a game is running, so install
  // it now, as we leave the terminal for the desktop. Installing it earlier
  // would race the VBL handler's cartridge-bus reads against the terminal's
  // send_sync protocol and corrupt keystroke delivery. The m68k terminal loop
  // is still alive here to handle CMD_START. Publish the hook-mode flag first so
  // it is latched when the installer reads it (the installer runs once, on the
  // first CMD_START, and picks the VBL $70 or ETV $400 vector from it). Written
  // inline as a direct shared-var store (same byte layout as SET_SHARED_VAR:
  // low word at slot+2, high word at slot+0), avoiding a single-use helper.
  {
    uintptr_t hookSlot = (uintptr_t)&__rom_in_ram_start__ +
                         CHANDLER_SHARED_VARIABLES_OFFSET +
                         (SIDEPAD_HOOK_MODE_SLOT * 4);
    *((volatile uint16_t *)(hookSlot + 2)) = etvMode ? 1u : 0u;
    *((volatile uint16_t *)(hookSlot)) = 0u;
  }
  for (int i = 0; i < SIDEPAD_HOOK_START_TICKS; i++) {
    SEND_COMMAND_TO_DISPLAY(DISPLAY_COMMAND_START);
    writeBtJoyState();
    writeBtMouseState();  // valid before the hook (step 3) reads slot 5
    controller_poll();
    sleep_ms(SLEEP_LOOP_MS);
  }
  // Publish the exit banner before handing off so it is in place when the m68k
  // boot_gem handler prints it. boot_gem prints it as the last thing before
  // rts'ing out of the print loop, so it persists on screen (the print loop
  // stops repainting the framebuffer over it).
  writeExitMessage();
  // Let the m68k continue booting to the GEM desktop. Hold CMD_BOOT_GEM for a
  // few frames rather than a single write: one write can race the m68k's
  // vsync-paced check_commands poll and be dropped, leaving it stuck redrawing
  // the terminal (the symptom: the firmware-load message flashes, then the
  // terminal returns). Repeating it guarantees boot_gem latches.
  for (int i = 0; i < SIDEPAD_BOOT_GEM_TICKS; i++) {
    SEND_COMMAND_TO_DISPLAY(DISPLAY_COMMAND_CONTINUE);
    writeBtJoyState();
    writeBtMouseState();
    controller_poll();
    sleep_ms(SLEEP_LOOP_MS);
  }
  // Now keep pumping the BLE host and republishing the joystick byte (and, when
  // mouse mode is on, the mouse packet) the resident VBL hook reads.
  while (true) {
    chandler_loop();
    controller_poll();
    writeBtJoyState();
    writeBtMouseState();
    sleep_ms(SIDEPAD_JOY_POLL_MS);
  }
}

static void showTitle() {
  term_printString(
      "\x1B"
      "E"
      "MD/Sidepad - " RELEASE_VERSION "\n");
}

// Handle a single key shortcut from the controller screen. Sidepad uses the
// terminal transport purely as a key-input path, so shortcuts act on the first
// keypress (no Enter, unlike the template's line-based command REPL).
static void handleUiKeystroke(char keystroke) {
  DPRINTF("UI keystroke: '%c' (0x%02X)\n", keystroke, (uint8_t)keystroke);
  // Any actionable key leaves the help screen and returns to the controller UI.
  if (helpVisible) {
    helpVisible = false;
    refreshControllerUI(true);
  }
  switch (tolower((unsigned char)keystroke)) {
    case 'p':
      DPRINTF("Pair requested\n");
      controller_requestPairing();
      break;
    case 'm':
      // Toggle right-stick-as-mouse. Persisted on exit to desktop (see
      // exitToGemDesktop), not here, to avoid a flash write on every keypress.
      // Force a full redraw so the second pair / titles appear or disappear.
      mouseMode = !mouseMode;
      DPRINTF("Mouse mode: %s\n", mouseMode ? "on" : "off");
      refreshControllerUI(true);
      break;
    case 'h':
      // Toggle the VBL/ETV hook mode. Persisted on exit to desktop (like mouse
      // mode); UI-only for now (the m68k still installs the VBL hook). Force a
      // full redraw so the footer's Hook:VBL/ETV label updates.
      etvMode = !etvMode;
      DPRINTF("Hook mode: %s\n", etvMode ? "ETV" : "VBL");
      refreshControllerUI(true);
      break;
    case 'b':
      // Return to Booster via the clean chip-reset path (loop tail).
      resetDeviceAtBoot = false;
      keepActive = false;
      exitToDesktop = false;
      break;
    default:
      break;
  }
}

// Toggle the help screen on the dedicated ST Help key. Debounced so the key's
// auto-repeat does not flicker the screen. Called from the keystroke callback,
// the same context handleUiKeystroke runs in, so rendering here is safe.
static void toggleHelp(void) {
  absolute_time_t now = get_absolute_time();
  if (helpToggleValid && absolute_time_diff_us(helpLastToggle, now) < 350000) {
    return;
  }
  helpLastToggle = now;
  helpToggleValid = true;
  helpVisible = !helpVisible;
  DPRINTF("Help screen: %s\n", helpVisible ? "on" : "off");
  refreshControllerUI(true);
}

// chandler callback: route the m68k's terminal keystrokes straight to the
// Sidepad single-key handler instead of the line-based command REPL.
static void sidepadInputCb(TransmissionProtocol *protocol,
                           uint16_t *payloadPtr) {
  if (protocol == NULL) {
    return;
  }
  // Any key cancels a running auto-exit countdown, and keeps it cancelled for
  // the rest of the session. Only mark cancelled when one was actually running,
  // so keys pressed before the first connect (e.g. [P] to pair) don't suppress
  // the first countdown. Keys with actions (p/m/x/esc) still perform them
  // below.
  if (autoExitActive) {
    autoExitActive = false;
    autoExitCancelled = true;
  }
  switch (protocol->command_id) {
    case APP_TERMINAL_START:
      // ESC ([ESC] Desktop): leave the terminal loop and continue booting to
      // the GEM desktop, installing the joystick hook on the way out.
      keepActive = false;
      exitToDesktop = true;
      break;
    case APP_TERMINAL_KEYSTROKE: {
      if (payloadPtr == NULL) {
        return;
      }
      uint32_t payload32 = TPROTO_GET_PAYLOAD_PARAM32(payloadPtr);
      // The dedicated ST Help key produces no useful ASCII, so dispatch it by
      // scan code (carried in bits 16-23 of the payload) before the ASCII path.
      uint8_t scanCode = (uint8_t)((payload32 & TERM_KEYBOARD_SCAN_MASK) >>
                                   TERM_KEYBOARD_SCAN_SHIFT);
      if (scanCode == SCAN_HELP) {
        toggleHelp();
        break;
      }
      handleUiKeystroke((char)(payload32 & TERM_KEYBOARD_KEY_MASK));
      break;
    }
    default:
      break;
  }
}

// This section contains the functions that are called from the main loop

static bool getKeepActive() { return keepActive; }

static bool getResetDevice() { return resetDeviceAtBoot; }

static void preinit() {
  // Initialize the terminal
  term_init();

  // Clear the screen
  term_clearScreen();

  // Show the title
  showTitle();
  term_printString("\n\n");
  term_printString("Loading, please wait...\n");

  display_refresh();
}

static void init(void) {
  // Clear the screen
  term_clearScreen();

  // Draw the Sidepad controller screen from the live controller state.
  refreshControllerUI(true);
  uiRefreshTime = make_timeout_time_ms(UI_REFRESH_MS);

  display_refresh();
}

void emul_start() {
  // The anatomy of an app or microfirmware is as follows:
  // - The driver code running in the remote device (the computer)
  // - the driver code running in the host device (the rp2040/rp2350)
  //
  // The driver code running in the remote device is responsible for:
  // 1. Perform the emulation of the device (ex: a ROM cartridge)
  // 2. Handle the communication with the host device
  // 3. Handle the configuration of the driver (ex: the ROM file to load)
  // 4. Handle the communication with the user (ex: the terminal)
  //
  // The driver code running in the host device is responsible for:
  // 1. Handle the communication with the remote device
  // 2. Handle the configuration of the driver (ex: the ROM file to load)
  // 3. Handle the communication with the user (ex: the terminal)
  //
  // Hence, we effectively have two drivers running in two different devices
  // with different architectures and capabilities.
  //
  // Please read the documentation to learn to use the communication protocol
  // between the two devices in the tprotocol.h file.
  //

  // 1. Check if the host device must be initialized to perform the emulation
  //    of the device, or start in setup/configuration mode
  SettingsConfigEntry *appMode =
      settings_find_entry(aconfig_getContext(), ACONFIG_PARAM_MODE);
  int appModeValue = APP_MODE_SETUP;  // Setup menu
  if (appMode == NULL) {
    DPRINTF(
        "APP_MODE_SETUP not found in the configuration. Using default value\n");
  } else {
    appModeValue = atoi(appMode->value);
    DPRINTF("Start emulation in mode: %i\n", appModeValue);
  }

  // 2. Initialiaze the normal operation of the app, unless the configuration
  // option says to start the config app Or a SELECT button is (or was) pressed
  // to start the configuration section of the app

  // In this example, the flow will always start the configuration app first
  // The ROM Emulator app for example will check here if the start directly
  // in emulation mode is needed or not

  // 3. If we are here, it means the app is not in emulation mode, but in
  // setup/configuration mode

  // As a rule of thumb, the remote device (the computer) driver code must
  // be copied to the RAM of the host device where the emulation will take
  // place.
  // The code is stored as an array in the target_firmware.h file
  //
  // Copy the terminal firmware to RAM
  COPY_FIRMWARE_TO_RAM((uint16_t *)target_firmware, target_firmware_length);

  // Clear the command sentinel in the freshly-copied mirror before the bus comes
  // up. COPY_FIRMWARE only writes the zero-trimmed cartridge (~2.4 KB), and
  // chandler_init zeroes only the reserved slot, not the sentinel, so $FA2000
  // would otherwise hold stale SRAM the m68k could read as a spurious command.
  // Matches md-snap.
  *((volatile uint32_t *)((uintptr_t)&__rom_in_ram_start__ +
                          CHANDLER_CMD_SENTINEL_OFFSET)) = 0;

  // Initialize the cartridge ROM4 read engine. ROM4 reads are served entirely
  // by chained DMAs feeding the PIO TX FIFO — no CPU/IRQ involvement.
  // Without this engine the cartridge image is unreadable from the m68k,
  // so a failure here is fatal: panic instead of stumbling on with a half-
  // configured PIO/DMA setup.
  if (init_romemul(false) < 0) {
    panic("init_romemul failed: PIO/DMA claim or program load returned <0");
  }

  // Bring up the ROM3 command capture (PIO + DMA ring on GPIO 26) and the
  // command handler that polls the ring, parses the protocol, and dispatches
  // each command to the registered callbacks. commemul is similarly load-
  // bearing — without it the m68k can issue commands but the RP never sees
  // them, so any non-OK return is fatal.
  if (commemul_init() < 0) {
    panic("commemul_init failed: PIO/DMA claim or program load returned <0");
  }
  chandler_init();
  chandler_addCB(sidepadInputCb);

  // After this point, the remote computer can execute the code

  // 4. During the setup/configuration mode, the driver code must interact
  // with the user to configure the device. To simplify the process, the
  // terminal emulator is used to interact with the user.
  // The terminal emulator is a simple text-based interface that allows the
  // user to configure the device using text commands.
  // If you want to use a custom app in the remote computer, you can do it.
  // But it's easier to debug and code in the rp2040

  // Initialize the display
  display_setupU8g2();

  // 5. Init the sd card
  // Most of the apps or microfirmwares will need to read and write files
  // to the SD card. The SD card is used to store the ROM, floppies, even
  // full hard disk files, configuration files, and other data.
  // The SD card is initialized here. If the SD card is not present, the
  // app continues and reports SD status in the terminal menu.
  // Each app or microfirmware must have a folder in the SD card where the
  // files are stored. The folder name is defined in the configuration.
  // If there is no folder in the micro SD card, the app will create it.

  FATFS fsys;
  SettingsConfigEntry *folder =
      settings_find_entry(aconfig_getContext(), ACONFIG_PARAM_FOLDER);
  char *folderName = "/test";  // MODIFY THIS TO YOUR FOLDER NAME
  if (folder == NULL) {
    DPRINTF("FOLDER not found in the configuration. Using default value\n");
  } else {
    DPRINTF("FOLDER: %s\n", folder->value);
    folderName = folder->value;
  }
  int sdcardErr = sdcard_initFilesystem(&fsys, folderName);
  if (sdcardErr != SDCARD_INIT_OK) {
    DPRINTF("SD card unavailable (error %i). Continuing without SD.\n",
            sdcardErr);
  } else {
    DPRINTF("SD card found & initialized\n");
  }

  // Initialize the display again (in case the terminal emulator changed it)
  display_setupU8g2();

  // Pre-init the stuff
  // In this example it only prints the please wait message, but can be used as
  // a place to put other code that needs to be run before the network is
  // initialized
  preinit();

  // Restore the persisted mouse-mode and hook-mode choices so the first UI
  // render reflects them without needing the user to toggle.
  loadMouseMode();
  loadHookMode();

  // 6. Bring up the BLE controller host.
  // Sidepad does not use WiFi: the CYW43 chip is shared between WiFi and
  // Bluetooth, and we bring it up in BT-only mode via controller_init()
  // (which calls network_initChipOnly() then starts Bluepad32). The terminal
  // already shows the controller screen; surface init progress/errors there.
  controller_setStatus("Starting BLE controller host...");
  refreshControllerUI(true);
  int controllerErr = controller_init();
  if (controllerErr != 0) {
    DPRINTF("controller_init failed: %i\n", controllerErr);
    controller_setStatus("BLE init failed - check Pico W / CYW43");
  }
  refreshControllerUI(true);

  // 7. Configure the SELECT button so menu status can show it immediately.
  select_configure();

  // 8. Now complete the terminal emulator initialization
  // The terminal emulator is used to interact with the user to configure the
  // device.
  init();

  // Blink on
#ifdef BLINK_H
  blink_on();
#endif

  // 9. Start the main loop
  // The main loop is the core of the app. It is responsible for running the
  // app, handling the user input, and performing the tasks of the app.
  // The main loop runs until the user decides to exit.
  // For testing purposes, this app only shows commands to manage the settings
  DPRINTF("Start the app loop here\n");
  while (getKeepActive()) {
    // Drain the ROM3 command ring → dispatch to registered callbacks
    // (sidepadInputCb handles single-key shortcuts).
    chandler_loop();

    // Drive the BLE controller host (CYW43 + BTstack poll, status update).
    controller_poll();

    // Auto-exit countdown. Start it when a controller connects (rising edge);
    // cancel it if the controller drops. When it elapses, exit to the desktop
    // exactly as ESC would (a keypress cancels it via sidepadInputCb).
    static bool prevConnected = false;
    controller_state_t connState = {0};
    controller_getState(&connState);
    if (connState.connected && !prevConnected && !autoExitCancelled) {
      autoExitActive = true;
      autoExitDeadline = make_timeout_time_ms(SIDEPAD_AUTOEXIT_SECONDS * 1000);
    } else if (!connState.connected) {
      autoExitActive = false;
    }
    prevConnected = connState.connected;
    // Any controller input also cancels the running countdown (sticky, like a
    // keypress). Use the deadzoned directions + buttons; exclude the Guide
    // button (the wake/connect press) so connecting doesn't self-cancel.
    if (autoExitActive &&
        (connState.anyUp || connState.anyDown || connState.anyLeft ||
         connState.anyRight || connState.anyButton || connState.btnView ||
         connState.btnMenu)) {
      DPRINTF("Auto-exit cancelled by controller input\n");
      autoExitActive = false;
      autoExitCancelled = true;
    }
    if (autoExitActive &&
        absolute_time_diff_us(get_absolute_time(), autoExitDeadline) <= 0) {
      DPRINTF("Auto-exit countdown elapsed; exiting to desktop\n");
      autoExitActive = false;
      keepActive = false;
      exitToDesktop = true;
    }

    // Redraw the live controller UI on a fixed cadence.
    if (absolute_time_diff_us(get_absolute_time(), uiRefreshTime) <= 0) {
      refreshControllerUI(false);
      uiRefreshTime = make_timeout_time_ms(UI_REFRESH_MS);
    }

    sleep_ms(SLEEP_LOOP_MS);
  }

  if (exitToDesktop) {
    exitToGemDesktop();  // never returns
  }

  // 10. Send RESET computer command
  // Ok, so we are done with the setup but we want to reset the computer to
  // reboot in the same microfirmware app or start the booster app

  sleep_ms(SLEEP_LOOP_MS);
  // We must reset the computer
  SEND_COMMAND_TO_DISPLAY(DISPLAY_COMMAND_RESET);
  sleep_ms(SLEEP_LOOP_MS);
  if (getResetDevice()) {
    // Reset the device
    reset_device();
  } else {
    // Before jumping to the booster app, let's clean the settings
    // Set emulation mode to 255 (setup menu)
    settings_put_integer(aconfig_getContext(), ACONFIG_PARAM_MODE,
                         APP_MODE_SETUP);
    settings_save(aconfig_getContext(), true);

    // Return to Booster via a full chip reset rather than an in-place jump: the
    // reset clears the pio0/DMA state this app brought up (romemul + commemul),
    // so Booster starts on pristine hardware. An in-place jump leaves those
    // running and corrupts Booster (garbled screen). main() routes back to
    // Booster on the clean restart.
    DPRINTF("Rebooting to the booster app...\n");
    reset_reboot_to_booster();
  }
}
