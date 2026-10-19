#ifndef BADGE8_BLE_SCREEN_H
#define BADGE8_BLE_SCREEN_H

#include <stdint.h>

// ============================================================================
//  The one screen this firmware has to draw by itself.
//
//  Everything else on the panel comes from the phone as a finished image. This
//  screen is the fallback: when nobody has connected over Bluetooth, the panel
//  has to tell the user how to reach the device.
//
//  Layout (portrait canvas, see ble_geometry.h - the device rotates it onto the
//  landscape panel itself):
//
//      +----------------------------------------+
//      | [] [] [] [] [] [] [] [] [] [] [] [] [] |  <- 4-colour ring, cycling
//      | []                                    [] |     black/white/red/yellow
//      | []       请使用蓝牙连接：              [] |
//      | []      8adge-XXXXX                    [] |  <- all text black
//      | []      打开手机App搜索并连接          [] |
//      | []                                    [] |
//      | [] [] [] [] [] [] [] [] [] [] [] [] [] |
//      +----------------------------------------+
//
//  It is the only 2bpp *colour* UI in the firmware - the ring is the one place
//  the panel's red and yellow pixels are exercised, so it doubles as a colour
//  self-test. Chinese comes from the generated subset font in ble_font.h (see
//  tools/gen_ble_font.py); the 5x7 ASCII font the older firmware used cannot
//  render it at all.
// ============================================================================

// Paint the "connect over Bluetooth" screen into `buf` and push it to the panel.
//
// `buf` is the caller's framebuffer (EPD398_COLOR_BYTES bytes, panel-native
// layout). It is passed in rather than allocated here because the firmware owns
// exactly one 105984-byte buffer for its whole lifetime: a second one would not
// fit next to the BLE stack, and freeing/reallocating around every paint only
// invites fragmentation.
//
// Blocks for 15-25 s while the panel refreshes, so call it from the main loop
// (or from setup()), never from a BLE callback. Returns false if `buf` is null
// or the panel did not answer.
bool bleScreenShowHint(uint8_t *buf, const char *deviceName);

#endif // BADGE8_BLE_SCREEN_H
