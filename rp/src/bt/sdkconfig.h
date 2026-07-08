/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// Bluepad32 build-time configuration for the Pico W target.
//
// Bluepad32 normally gets these from ESP-IDF's "menuconfig" (sdkconfig.h). On
// the Pico W they are provided here and must be on the include path of both
// libbluepad32 and the app (see rp/src/CMakeLists.txt).

#ifndef SIDEPAD_SDKCONFIG_H
#define SIDEPAD_SDKCONFIG_H

// Sidepad injects a single joystick, so only one controller is ever paired at a
// time. Capping Bluepad32 at one device enforces that: a new controller can
// only pair after the current one is disconnected (see controller_requestPairing
// in controller.c), and a stray second controller can never co-drive joystick 1.
#define CONFIG_BLUEPAD32_MAX_DEVICES 1
#define CONFIG_BLUEPAD32_MAX_ALLOWLIST 1
#define CONFIG_BLUEPAD32_GAP_SECURITY 1
#define CONFIG_BLUEPAD32_ENABLE_BLE_BY_DEFAULT 1

// Sidepad provides its own uni_platform (see controller.c), so use the custom
// platform rather than one of the built-in ESP32 Unijoysticle platforms.
#define CONFIG_BLUEPAD32_PLATFORM_CUSTOM
#define CONFIG_TARGET_PICO_W

// 2 == Info
#define CONFIG_BLUEPAD32_LOG_LEVEL 2

#endif  // SIDEPAD_SDKCONFIG_H
