/**
 * File: controller.h
 * Description: BLE controller host integration and UI-facing state snapshot.
 */

#ifndef CONTROLLER_H
#define CONTROLLER_H

#include <stdbool.h>
#include <stdint.h>

// Analog triggers (lt/rt, 0..1) count as fire once pulled past this point.
#define CONTROLLER_TRIGGER_THRESHOLD 0.5f

typedef struct {
  bool initialized;
  bool paired;
  bool connected;
  bool pairingRequested;
  char pairedAddress[24];
  char pairedAddressType[12];
  char deviceName[32];
  char status[64];

  float lx;
  float ly;
  float rx;
  float ry;
  float lt;
  float rt;

  uint8_t hat;

  bool btnA;
  bool btnB;
  bool btnX;
  bool btnY;
  bool btnLB;
  bool btnRB;
  bool btnView;
  bool btnMenu;
  bool btnLS;
  bool btnRS;
  bool btnGuide;
  bool dpadUp;
  bool dpadDown;
  bool dpadLeft;
  bool dpadRight;

  // Digital, Atari ST style joystick: any stick or D-pad combined into 8
  // directions, plus a single fire button (any face/shoulder button).
  bool anyUp;
  bool anyDown;
  bool anyLeft;
  bool anyRight;
  bool anyButton;

  // Digital directions split by source, for the dual joystick/mouse UI:
  //   pad*    = left stick + D-pad  (shown as the "Joystick" pair)
  //   rstick* = right stick only    (shown as the "Mouse" pair)
  // any* above remains the union of both, so joystick injection is unchanged.
  bool padUp;
  bool padDown;
  bool padLeft;
  bool padRight;
  bool rstickUp;
  bool rstickDown;
  bool rstickLeft;
  bool rstickRight;
} controller_state_t;

int controller_init(void);
void controller_poll(void);
void controller_requestPairing(void);
// Set the status-line text shown in the controller UI. Used to surface
// boot/init progress and errors in the same slot as live controller status.
void controller_setStatus(const char *message);
void controller_getState(controller_state_t *outState);
bool controller_getAndClearRestartRequested(void);

#endif  // CONTROLLER_H
