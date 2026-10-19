#ifndef BADGE8_BLE_SERVICE_H
#define BADGE8_BLE_SERVICE_H

// ============================================================================
//  The BLE side of the firmware: NimBLE GATT server + the protocol engine.
//
//  Everything the phone can do to this device is implemented here; main.cpp
//  only owns the things that must not run in a BLE callback (painting the panel,
//  driving the LED, the 2-minute hint timer).
//
//  Call flow:
//      bleInit(name, frame)        once, from setup()
//      ... BLE callbacks fire on the NimBLE task while the app transfers ...
//      bleTakeRenderRequest(...)   polled from loop(); true once per transfer
//      bleNotifyRendering()/bleNotifyDisplayed()   from loop(), around the paint
// ============================================================================

#include <Arduino.h>
#include <stdint.h>

// Bring up the stack, register the GATT service and start advertising.
//
// `deviceName` is advertised verbatim and echoed in the GET_INFO reply.
// `frameBuffer` must point at EPD398_COLOR_BYTES (105984) bytes and stay alive
// for the lifetime of the firmware: DATA writes land in it directly, which is
// what keeps the transfer free of a second 106 KB copy.
//
// Returns false if the buffer is null (the stack still comes up, but every
// BEGIN_IMAGE is answered with ERR_MEM).
bool bleInit(const char *deviceName, uint8_t *frameBuffer);

// The name given to bleInit().
const char *bleDeviceName();

// ---------------------------------------------------------------- state
bool bleIsConnected();
// True once any central has connected since power-on. Informational only: the
// idle hint timer uses bleIsConnected(), not this.
bool bleHasEverConnected();
uint32_t bleConnectionCount();

// ---------------------------------------------------------------- transfer
// True while a BEGIN_IMAGE transfer is in progress (the framebuffer is in use).
bool bleIsTransferring();

// True while the app is resending a single block (RETRY_BLOCK is armed).
bool bleIsFixingBlock();

// True once a transfer that passed verification, exactly once per image.
// Consumes the flag and marks the framebuffer as in use, so the caller
// MUST pair it with bleRenderFinished() once the panel has been painted.
bool bleTakeRenderRequest(uint8_t *outFormat, uint32_t *outBytes);

// Release the framebuffer after painting. Until this is called, BEGIN_IMAGE
// answers ERR_BUSY - the refresh takes 15-25 s and a second transfer started
// during it would overwrite the bytes being sent to the panel.
void bleRenderFinished();

// Claim the framebuffer for a device-drawn screen (the hint). Returns false -
// and changes nothing - when a transfer is receiving, when a verified image is
// still waiting to be painted, or when another paint already holds the buffer;
// the caller must then not touch the framebuffer. On success the caller MUST
// pair it with bleRenderFinished().
//
// Without this the hint path could paint while the phone was streaming an
// image into the same 105984-byte buffer (and BEGIN_IMAGE would not even see
// ERR_BUSY).
bool bleBeginHintPaint();

// Abandon a transfer whose app has gone silent for BLE_RX_TIMEOUT_MS after the
// first DATA byte (or BLE_BEGIN_TIMEOUT_MS after BEGIN_IMAGE, while the app is
// still preparing the payload) while the link stays up. Poll this from loop();
// it resets RECEIVING/FIXING back to CONNECTED and emits EVT_STATE(CONNECTED),
// so the next BEGIN_IMAGE is not answered with ERR_STATE forever.
void blePollTransferTimeout();

// True exactly once after an app asked for the hint screen (SHOW_HINT).
bool bleTakeHintRequest();

// Option bits set by the last SET_OPTIONS (0 after a new connection).
uint8_t bleOptions();

// ---------------------------------------------------------------- events
// Pushes an EVT_STATE frame to the connected app. Safe from the main loop.
void bleNotifyState(uint8_t state, uint8_t error);
void bleNotifyRendering();
void bleNotifyDisplayed(uint8_t error);
// Pushes an EVT_LOG frame (UTF-8, no NUL). Convenience for debugging the app.
void bleNotifyLog(const char *text);

#endif // BADGE8_BLE_SERVICE_H
