#ifndef BADGE8_BLE_PROTOCOL_H
#define BADGE8_BLE_PROTOCOL_H

// ============================================================================
//  8adge BLE image-transfer protocol - on-wire constants.
//
//  Protocol v3. The full specification - frame layouts, the state machine, the
//  flow-control rules, the retransmission handshake, the refresh cooldown and
//  the Android-side checklist - is in docs/BLE_PROTOCOL.md. This header is the
//  single source of truth for the numbers; the document is the prose.
//
//  Keep the two in sync: if a constant here changes, the document changes too.
//
//  What changed in v2 (relative to v1):
//    * per-block CRC32 with selective retransmission (BLOCK_CRCS + RETRY_BLOCK),
//      so a damaged region costs one block instead of the whole image;
//    * the window ACK became a plain progress report - the app no longer has to
//      stop at window boundaries (the link layer already flow-controls);
//    * fast connection parameters and data length extension on connect.
//
//  What changed in v3 (relative to v2):
//    * a full-refresh cooldown, enforced by the firmware because violating it
//      damages the panel irreversibly. The app supplies the clock
//      (SYNC_TIME), and a refresh requested inside the cooldown is refused with
//      ERR_COOLDOWN until the user confirms it (CONFIRM_REFRESH);
//    * GET_INFO reports the cooldown so the app can warn before it even sends
//      an image.
// ============================================================================

#include <stdint.h>

// ---------------------------------------------------------------- GATT
// Two characteristics, both under one 128-bit vendor service. The first four
// bytes of every UUID spell "BADG" in ASCII, which makes them easy to spot in a
// BLE scanner.
//
//   42414447-0001-4e5a-9c1a-6b7c8d9e0f11   service
//   42414447-0002-4e5a-9c1a-6b7c8d9e0f11   CONTROL  write, write-nr, notify
//   42414447-0003-4e5a-9c1a-6b7c8d9e0f11   DATA     write, write-nr
//
// CONTROL carries *all* structured traffic in both directions: requests from the
// app, and the responses plus unsolicited events back to it. DATA carries
// nothing but image bytes, so the bulk path never shares a packet queue with
// control frames - and image bytes carry no per-chunk header at all, which is
// the point: at 512 bytes per write a 2-byte header would be 0.4% of the link
// budget spent on something the block CRCs already detect.
#define BLE_SERVICE_UUID "42414447-0001-4e5a-9c1a-6b7c8d9e0f11"
#define BLE_CTRL_UUID    "42414447-0002-4e5a-9c1a-6b7c8d9e0f11"
#define BLE_DATA_UUID    "42414447-0003-4e5a-9c1a-6b7c8d9e0f11"

// ---------------------------------------------------------------- canvas
// The phone and the firmware both work in this portrait canvas. It is NOT the
// panel's native size: the panel is a 768x552 landscape part and the device
// rotates the canvas onto it itself (see ble_geometry.h), so an app never has to
// know which way round the glass is mounted.
//
// 552 x 768 = 768 x 552 pixels, so the rotation is exact - same pixel count,
// no scaling, no cropping - and the two formats below have the same length they
// would have had in landscape.
#define BLE_IMAGE_WIDTH        552
#define BLE_IMAGE_HEIGHT       768
#define BLE_IMAGE_ROW_BYTES_2BPP (BLE_IMAGE_WIDTH / 4)   // 138 bytes per row
#define BLE_IMAGE_ROW_BYTES_1BPP (BLE_IMAGE_WIDTH / 8)   //  69 bytes per row

// ---------------------------------------------------------------- framing
// Every frame on CONTROL starts with this 4-byte header. The payload length is a
// single byte, so a frame is at most 259 bytes; control traffic never comes
// close to that.
//
//   [0] opcode      bit7 set = device -> app (response or event)
//   [1] seq         echoed from the request; 0 for unsolicited events
//   [2] status      result code, responses only (0 in requests and events)
//   [3] length      payload bytes that follow (0..255)
//   [4..] payload
#define BLE_FRAME_HEADER_LEN  4
#define BLE_FRAME_MAX_PAYLOAD 255
#define BLE_FRAME_MAX_LEN     (BLE_FRAME_HEADER_LEN + BLE_FRAME_MAX_PAYLOAD)

// All multi-byte integers are little-endian.
#define BLE_PROTOCOL_VERSION 3

// ---------------------------------------------------------------- opcodes
// App -> device (requests).
enum : uint8_t {
    BLE_OP_GET_INFO    = 0x01,
    BLE_OP_BEGIN_IMAGE = 0x02,
    BLE_OP_END_IMAGE   = 0x03,
    BLE_OP_ABORT       = 0x04,
    BLE_OP_SHOW_HINT   = 0x05,
    BLE_OP_PING        = 0x06,
    BLE_OP_SET_OPTIONS = 0x07,
    // v2
    BLE_OP_BLOCK_CRCS  = 0x08,  // declare the per-block CRCs (after BEGIN)
    BLE_OP_RETRY_BLOCK = 0x09,  // prime the device to receive one block again
    // v3
    BLE_OP_SYNC_TIME       = 0x0A,  // u64 UTC seconds: adopt the app's clock
    BLE_OP_CONFIRM_REFRESH = 0x0B,  // u8 token: user approved a blocked refresh
};

// Device -> app responses: request opcode | 0x80, i.e. 0x81..0x8B for the
// request range 0x01..0x0B.
#define BLE_OP_RESPONSE(op) ((uint8_t)((op) | 0x80))

// Highest opcode that is still a request. Anything above it is reserved, and a
// frame that arrives with one is answered with BLE_OP_ERROR.
#define BLE_OP_MAX_REQUEST 0x3F

// The one opcode used when a request cannot be answered with `opcode | 0x80`,
// because the request opcode itself was out of range (0 or > 0x3F). Echoing it
// would land in the event range, so the offending opcode travels in the payload
// instead: `u8 offending_opcode`.
#define BLE_OP_ERROR 0xE0

// Device -> app unsolicited events.
//
// These start at 0xC0 deliberately: responses cover 0x81..0xBF, so an event can
// never be mistaken for a response.
enum : uint8_t {
    BLE_EVT_STATE      = 0xC0,  // u8 state, u8 error
    BLE_EVT_PROGRESS   = 0xC1,  // u32 received, u32 total, u8 session, u8 flags
    BLE_EVT_LOG        = 0xC2,  // UTF-8 text, no NUL terminator
    // v3: the refresh was refused because of the cooldown. The app must ask the
    // user and, if they agree, hand back the token in BLE_OP_CONFIRM_REFRESH
    // followed by the same command it originally sent.
    //   u8  token            correlates the confirmation
    //   u8  reason           BLE_GUARD_REASON_*
    //   u8  for_opcode       the request that was refused, so the app can retry
    //   u8  reserved         0
    //   u16 wait_seconds     still to go before the cooldown elapses by itself
    //   u16 min_gap_seconds  the device's configured minimum
    //   u32 elapsed_seconds  how long ago the last refresh was (a lower bound
    //                        when reason == BLE_GUARD_REASON_CLOCK_UNKNOWN)
    BLE_EVT_REFRESH_BLOCKED = 0xC3,
};

// Opcode 0xC1 was called EVT_WINDOW_ACK in v1, when it was a stop-and-wait
// credit. It is now a plain progress report and the app is free to ignore it
// for pacing; the name changed so nobody re-implements the old barrier.

// EVT_PROGRESS flags
#define BLE_PROGRESS_FLAG_BLOCK_DONE 0x01  // a whole block has just been verified

// ---------------------------------------------------------------- data hints
// The single byte an ERR_STATE END_IMAGE reply carries (docs §5.4): DATA
// arrived while there was no session to receive it. The device keeps accepting
// and discarding those writes, and a progress report is only sent every
// BLE_DATA_CHUNK_MAX window, so an app that pipelines straight into the paint
// of the image it just finished can wait for a DISPLAYED that will never come.
// This byte says why. Unknown values must be ignored by the app.
#define BLE_HINT_NONE              0x00  // nothing to report
#define BLE_HINT_DATA_AFTER_END    0x01  // the transfer is over, that data went nowhere
#define BLE_HINT_DATA_IGNORED      0x02  // bytes past total_len were discarded

// ---------------------------------------------------------------- status
// Result code in the STATUS byte of a response. BLE_OK is the only success.
enum : uint8_t {
    BLE_OK           = 0x00,
    BLE_ERR_OPCODE   = 0x01,  // unknown opcode
    BLE_ERR_LENGTH   = 0x02,  // frame too short, or payload length mismatch
    BLE_ERR_STATE    = 0x03,  // command not allowed in the current state
    BLE_ERR_FORMAT   = 0x04,  // unsupported image format
    BLE_ERR_SIZE     = 0x05,  // total_len / block_size / count does not match
    BLE_ERR_CRC      = 0x06,  // whole-image CRC mismatch, restart from BEGIN
    BLE_ERR_MEM      = 0x07,  // device could not allocate the frame buffer
    BLE_ERR_PANEL    = 0x08,  // panel refresh failed (BUSY timeout)
    BLE_ERR_BUSY     = 0x09,  // device is still painting the previous image
    BLE_ERR_OVERFLOW = 0x0A,  // more data than declared in BEGIN_IMAGE
    BLE_ERR_OPTIONS  = 0x0B,  // unsupported option bits
    BLE_ERR_BLOCKS   = 0x0C,  // v2: some blocks failed - resend them, see bitmap
    BLE_ERR_COOLDOWN = 0x0D,  // v3: refused, too soon after the last full refresh
    BLE_ERR_MTU      = 0x0E,  // v3.1: the reply would not fit the negotiated MTU
};

// Status codes above are shared by responses and EVT_STATE.error.
//
// ERR_MTU means the request could not be answered without truncating the
// reply at the negotiated ATT MTU. The app must request a larger MTU (517 is
// the preferred value) and retry. It is only ever returned for GET_INFO and
// for a BEGIN_IMAGE whose failure bitmap could not fit; no state was changed.

// ---------------------------------------------------------------- states
// Value of EVT_STATE's first payload byte.
enum : uint8_t {
    BLE_STATE_IDLE        = 0,
    BLE_STATE_ADVERTISING = 1,
    BLE_STATE_CONNECTED   = 2,
    BLE_STATE_RECEIVING   = 3,
    BLE_STATE_VERIFYING   = 4,
    BLE_STATE_RENDERING   = 5,
    BLE_STATE_DISPLAYED   = 6,
    BLE_STATE_HINT_SHOWN  = 7,
    BLE_STATE_ERROR       = 8,
    BLE_STATE_FIXING      = 9,  // v2: receiving one retried block
};

// ---------------------------------------------------------------- formats
// Image payload formats accepted by BEGIN_IMAGE. Rows are BLE_IMAGE_WIDTH = 552
// pixels wide, packed 4 (2bpp) or 8 (1bpp) pixels per byte, MSB first, top to
// bottom, no padding between rows.
//
// The length is the same as a landscape 768x552 payload would be, because the
// rotation is a pure pixel permutation - see ble_geometry.h.
enum : uint8_t {
    // 2bpp, the panel's native packing: 4 pixels per byte, most significant pair
    // leftmost, row-major, 138 bytes per row, 768 rows. 105984 bytes total.
    // Pixel value: 0 = black, 1 = white, 2 = yellow, 3 = red.
    BLE_FMT_2BPP = 1,

    // 1bpp monochrome: 1 = white, 0 = black, most significant bit leftmost,
    // row-major, 69 bytes per row, 768 rows. 52992 bytes total. The device turns
    // it into 2bpp as it receives it, so a black-and-white-only picture ships in
    // half the time and the app does not need a 4-colour quantiser.
    BLE_FMT_1BPP = 2,
};

// ---------------------------------------------------------------- options
// Bitmap for SET_OPTIONS. Back to 0 on every new connection.
#define BLE_OPT_DISABLE_HINT 0x01  // never paint the 2-minute hint screen
#define BLE_OPT_VALID_MASK   0x01

// ---------------------------------------------------------------- refresh guard
// Minimum seconds between two full panel refreshes. The firmware enforces this
// because a 4-colour e-paper refresh - a long bipolar AC waveform - damages the
// panel irreversibly if it is started again before the pigment particles have
// settled. See src/refresh_guard.{h,cpp} for the mechanism and the document for
// the app-side handshake.
//
// The app must not hardcode this: it is reported in GET_INFO and in every
// EVT_REFRESH_BLOCKED. It is also the one number the app needs to decide whether
// it can skip asking the user at all.
#define BLE_REFRESH_MIN_GAP_S 180

// Why a refresh was refused, in EVT_REFRESH_BLOCKED's reason byte. It tells the
// app how much to trust `elapsed_seconds` in the same event.
//
// The device has no clock of its own - it never connects to anything, and its
// RTC cannot survive a power cut - so it takes the time from the app
// (BLE_OP_SYNC_TIME) and otherwise falls back to a conservative bound derived
// from its own uptime.
//
//   CLOCK_UNKNOWN  no clock, or the newest refresh could not be dated. The
//                  reported elapsed time is a *lower bound*: the real interval
//                  is at least that, possibly much more. Nothing is wrong; the
//                  device just cannot be more precise. Word the dialog
//                  accordingly ("至少 ...").
//   CLOCK_KNOWN    the app's clock is in use and the interval is exact.
//   CLOCK_BEHIND   the app's clock appears to predate the recorded refresh -
//                  someone sent milliseconds instead of seconds, or the phone's
//                  clock moved backwards. Never treat this as time having
//                  passed; the safest reading is "no interval has elapsed".
//
// Codes are in the 0x00-0x3F range on purpose: they travel in the protocol's
// single-byte fields, and the top bits are reserved for the frame direction.
enum : uint8_t {
    BLE_GUARD_REASON_CLOCK_UNKNOWN = 0,
    BLE_GUARD_REASON_CLOCK_KNOWN   = 1,
    BLE_GUARD_REASON_CLOCK_BEHIND  = 2,
};

// ---------------------------------------------------------------- helpers
static inline uint16_t bleRd16(const uint8_t *p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static inline uint32_t bleRd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline void bleWr16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)(v >> 8);
}

static inline void bleWr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

#endif // BADGE8_BLE_PROTOCOL_H
