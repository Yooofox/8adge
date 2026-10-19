// ============================================================================
//  BLE image transfer - protocol engine and NimBLE glue.
//
//  Design notes worth keeping:
//
//  * Only the two things that cannot run here live in main.cpp: painting the
//    panel (blocks for 15-25 s) and the 2-minute hint timer. A verified transfer
//    therefore only raises bleTakeRenderRequest(); the paint happens in loop().
//
//  * DATA writes are scattered straight into the 2bpp framebuffer, so there is
//    never a second 106 KB copy in flight. For 2bpp that is a rotation of each
//    nibble group; for 1bpp it is an expansion into the same rotated positions
//    (see ble_geometry.h). The mapping is per payload byte, so a chunk may begin
//    and end anywhere and the code needs no carry state.
//
//  * Integrity is per *block*, not per image. The app declares one CRC32 per
//    block up front; the device verifies each block as its bytes arrive and
//    END_IMAGE answers with a bitmap of the ones that failed. The app then
//    resends just those. See "why block CRCs" below.
//
//  * The whole-image CRC is recomputed from the framebuffer at END_IMAGE rather
//    than accumulated during reception. Reception-side verification is already
//    covered by the block CRCs, and recomputing is what makes the reported
//    whole-image digest honest *after* a repair - the bytes that were replaced
//    are the ones now in the buffer, so no stale running CRC can disagree with
//    the pixels on the glass. It costs one pass over 423936 pixels (~50 ms) out
//    of a 15-25 s refresh.
//
//  * CRC is the standard IEEE 802.3 / zlib CRC32 that java.util.zip.CRC32
//    produces, computed over the payload bytes exactly as they went over the air
//    (for 1bpp, over the packed 1bpp bytes - not the expanded 2bpp).
//
//  ------------------------------------------------------------------ why block CRCs
//
//  Per-packet retransmission is already handled below us: the BLE link layer
//  acknowledges every LL packet, retransmits until acknowledged, and carries its
//  own CRC, so a corrupted or lost packet never reaches the ATT layer. What is
//  NOT protected is anything above that - an app-side write that returned false
//  and was dropped, a phone that ran out of memory mid-transfer, a stack that
//  discarded a queued write on disconnect. Those produce a *gap*, and a gap is
//  exactly what the whole-image CRC detects but cannot localise.
//
//  v1 therefore had to resend the entire 106 KB on any mismatch. At the measured
//  9.5 kB/s that is 11 seconds to fix one lost 512-byte write. With 4 KB blocks
//  the same repair costs 0.4 seconds, and because a block carries its own index
//  the resend lands at the right offset no matter how the bytes around it moved.
// ============================================================================

#include "ble_service.h"

#include <NimBLEDevice.h>
#include <string.h>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "ble_geometry.h"
#include "ble_protocol.h"
#include "config.h"
#include "epd_398_4color.h"
#include "refresh_guard.h"

// ---------------------------------------------------------------- module state

static char s_name[40] = {0};
static uint8_t *s_frame = nullptr;

static NimBLEServer *s_server = nullptr;
static NimBLECharacteristic *s_ctrl = nullptr;
static NimBLECharacteristic *s_data = nullptr;

static volatile bool s_connected = false;
static volatile bool s_everConnected = false;
static volatile uint32_t s_connCount = 0;
static volatile uint16_t s_mtu = 23;
static volatile uint16_t s_connHandle = 0;

static volatile bool s_renderPending = false;
// Set while main.cpp is actually painting (15-25 s). s_renderPending alone is
// not enough: bleTakeRenderRequest() clears it before the paint starts, so
// without this a fast app could BEGIN the next image into the buffer that is
// being pushed to the panel right now.
static volatile bool s_rendering = false;
static volatile uint8_t s_renderFormat = 0;
static volatile uint32_t s_renderBytes = 0;
static volatile bool s_hintRequested = false;
static volatile uint8_t s_options = 0;

// Transfer state. Touched from the NimBLE host task (data/control callbacks)
// and from the Arduino loop task (timeout poll, render hand-off, hint paint).
// A single core is NOT a substitute for locking: FreeRTOS preempts at any
// instruction, and several of these fields form one logical unit. Every entry
// point that touches more than one of them takes s_stateMux.
static SemaphoreHandle_t s_stateMux = nullptr;

struct StateLock {
    StateLock() { if (s_stateMux) xSemaphoreTakeRecursive(s_stateMux, portMAX_DELAY); }
    ~StateLock() { if (s_stateMux) xSemaphoreGiveRecursive(s_stateMux); }
    StateLock(const StateLock &) = delete;
    StateLock &operator=(const StateLock &) = delete;
};

static volatile bool s_receiving = false;
// millis() of the last byte/control frame that belongs to the active session,
// used by blePollTransferTimeout() to abandon a stalled transfer.
static volatile uint32_t s_lastRxMs = 0;
// False between BEGIN_IMAGE and the first DATA byte, so the much longer
// BLE_BEGIN_TIMEOUT_MS applies while the app is still preparing its payload.
static bool s_firstByteSeen = false;
// Payload bytes past the length declared in BEGIN_IMAGE. They cannot be stored
// (every offset is bounded by s_total) and they are NOT treated as a fatal
// error any more: an app that repeats its last chunk used to lose a completely
// valid image to ERR_OVERFLOW. They are counted here and only reported - as
// ERR_OVERFLOW, the more precise diagnosis - if the image fails to verify
// afterwards.
static uint32_t s_strayBytes = 0;
// App-side request guidance from the last DATA write, so a repeated chunk can
// be answered with a diagnosis instead of silence.
static volatile uint8_t s_dataHint = BLE_HINT_NONE;
// BLE_FMT_* (1 = 2bpp, 2 = 1bpp). MUST NOT be bool: BLE_FMT_2BPP is 1, so a
// bool collapses BLE_FMT_1BPP (2) into "true" and every 1bpp transfer is
// decoded and re-CRCed as 2bpp - the transfer then always fails END_IMAGE.
static uint8_t s_format = 0;
static uint32_t s_total = 0;
static uint32_t s_received = 0;
static uint32_t s_crcDeclared = 0;      // whole-image CRC from BEGIN_IMAGE
static uint8_t s_session = 0;
static uint16_t s_chunkSize = 0;
static uint8_t s_chunksInWindow = 0;

// Per-block verification.
static uint8_t s_blockShift = 0;        // 0 = no block CRCs (v1 app)
static uint16_t s_blockCount = 0;
static uint32_t s_blockSize = 0;
static uint32_t s_blockCrcDeclared[BLE_MAX_BLOCKS];   // CRC the app promised
static uint8_t s_blockBad[(BLE_MAX_BLOCKS + 7) / 8];  // bit set = failed
static uint16_t s_blockBadCount = 0;
static uint16_t s_blockIndex = 0;       // block currently being accumulated
static uint32_t s_blockCrcRunning = 0xFFFFFFFFu;
static uint32_t s_blockBytes = 0;       // bytes folded into that accumulator

// Repair state (RETRY_BLOCK armed).
static volatile bool s_fixing = false;
static uint16_t s_fixBlock = 0;
static uint32_t s_fixOffset = 0;        // payload offset of the block start
static uint32_t s_fixLen = 0;           // total bytes this block needs
static uint32_t s_fixNeeded = 0;        // bytes still outstanding
static uint32_t s_fixCrcRunning = 0xFFFFFFFFu;

// ---------------------------------------------------------------- CRC32

// Standard CRC-32 (poly 0xEDB88320 reflected, init 0xFFFFFFFF, final xor
// 0xFFFFFFFF) - the same value java.util.zip.CRC32 produces. Bitwise rather
// than table-driven: the whole image is a few tens of milliseconds either way,
// and it keeps 1 KB of table out of RAM.
static uint32_t crc32Update(uint32_t crc, const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
        }
    }
    return crc;
}

static inline uint32_t crc32Finish(uint32_t crc) { return crc ^ 0xFFFFFFFFu; }

// ---------------------------------------------------------------- log throttle

// millis() of the last throttled line. See BLE_LOG_THROTTLE_MS: two calls
// inside the window return false, so a repeated chunk or a block that keeps
// failing CRC cannot flood (and, worse, stall) the host task's console.
static uint32_t s_lastThrottledLog = 0;

static bool throttleLogDue() {
    const uint32_t now = millis();
    if ((uint32_t)(now - s_lastThrottledLog) < BLE_LOG_THROTTLE_MS) return false;
    s_lastThrottledLog = now;
    return true;
}

// ---------------------------------------------------------------- framing

static void pushFrame(uint8_t opcode, uint8_t seq, uint8_t status,
                      const uint8_t *payload, size_t len) {
    if (!s_ctrl) return;
    if (len > BLE_FRAME_MAX_PAYLOAD) len = BLE_FRAME_MAX_PAYLOAD;

    uint8_t frame[BLE_FRAME_MAX_LEN];
    frame[0] = opcode;
    frame[1] = seq;
    frame[2] = status;
    frame[3] = (uint8_t)len;
    if (len) memcpy(frame + BLE_FRAME_HEADER_LEN, payload, len);

    // notify(value, len) copies the frame into its own mbuf. Do NOT use
    // setValue() + notify(): those are two separate operations on shared
    // characteristic state, and a preemption between them (the host task is
    // also sending here) can publish one frame's bytes under the other's
    // notification - losing or duplicating a response/event. There is no READ
    // property on CONTROL, so setValue() serves no purpose anyway.
    s_ctrl->notify(frame, len + BLE_FRAME_HEADER_LEN);
}

static void sendResponse(uint8_t opcode, uint8_t seq, uint8_t status,
                         const uint8_t *payload, size_t len) {
    pushFrame(BLE_OP_RESPONSE(opcode), seq, status, payload, len);
}

static void sendEvent(uint8_t opcode, const uint8_t *payload, size_t len) {
    pushFrame(opcode, 0, BLE_OK, payload, len);
}

static void sendStateEvent(uint8_t state, uint8_t error) {
    uint8_t p[2] = { state, error };
    sendEvent(BLE_EVT_STATE, p, sizeof(p));
}

static void sendProgress(bool blockDone) {
    uint8_t p[10];
    bleWr32(p, s_received);
    bleWr32(p + 4, s_total);
    p[8] = s_session;
    p[9] = blockDone ? BLE_PROGRESS_FLAG_BLOCK_DONE : 0;
    sendEvent(BLE_EVT_PROGRESS, p, sizeof(p));
}

// ---------------------------------------------------------------- refresh guard

// Refuse a refresh that would land inside the cooldown, and tell the app why.
//
// The app is expected to warn the user about permanent panel damage, and - only
// if the user still says yes - hand the token back in CONFIRM_REFRESH and then
// repeat the very command that was refused. Nothing is refreshed in the
// meantime: silence from the app means "do not refresh", which is the safe
// default for a mistake that cannot be undone.
static void sendRefreshBlocked(uint8_t reqOpcode, uint8_t seq) {
    const uint8_t token = refreshGuardIssueToken(reqOpcode);

    uint8_t p[12];
    p[0] = token;
    p[1] = refreshGuardReason();
    p[2] = reqOpcode;          // what to retry once the user agrees
    p[3] = 0;                  // reserved
    bleWr16(p + 4, (uint16_t)refreshGuardWaitSeconds());
    bleWr16(p + 6, refreshGuardMinGapSeconds());
    bleWr32(p + 8, refreshGuardElapsedSeconds());

    sendResponse(reqOpcode, seq, BLE_ERR_COOLDOWN, nullptr, 0);
    sendEvent(BLE_EVT_REFRESH_BLOCKED, p, sizeof(p));

    BLE_LOG("[BLE] %s refused: refresh cooldown, %lu s left (reason %u, elapsed %lu s)\n",
            (reqOpcode == BLE_OP_BEGIN_IMAGE) ? "BEGIN_IMAGE" : "SHOW_HINT",
            (unsigned long)refreshGuardWaitSeconds(), (unsigned)refreshGuardReason(),
            (unsigned long)refreshGuardElapsedSeconds());
}

// ---------------------------------------------------------------- transfer

static uint32_t expectedLength(uint8_t format) {
    if (format == BLE_FMT_2BPP) return EPD398_COLOR_BYTES;
    if (format == BLE_FMT_1BPP) return EPD398_COLOR_BYTES / 2;
    return 0;
}

static uint16_t dataChunkSize() {
    // ATT payload is MTU-3; the app is told not to exceed this.
    int mtu = (int)s_mtu;
    if (mtu < 23) mtu = 23;
    int chunk = mtu - 3;
    if (chunk > BLE_DATA_CHUNK_MAX) chunk = BLE_DATA_CHUNK_MAX;
    if (chunk < 1) chunk = 1;
    return (uint16_t)chunk;
}

// Largest CONTROL frame value (payload + 4-byte header) this link can carry.
// ATT notifications are silently truncated by the stack at MTU-3, which would
// hand the app a frame whose length byte promises more bytes than arrived.
static size_t attFrameLimit() {
    int mtu = (int)s_mtu;
    if (mtu < 23) mtu = 23;
    return (size_t)(mtu - 3);
}

static bool frameFits(size_t payloadLen) {
    return payloadLen + BLE_FRAME_HEADER_LEN <= attFrameLimit();
}

static void resetTransfer() {
    s_receiving = false;
    s_strayBytes = 0;
    s_format = 0;
    s_lastRxMs = 0;
    s_firstByteSeen = false;
    s_total = 0;
    s_received = 0;
    s_crcDeclared = 0;
    s_chunksInWindow = 0;
    s_blockShift = 0;
    s_blockCount = 0;
    s_blockSize = 0;
    s_blockBadCount = 0;
    s_blockIndex = 0;
    s_blockCrcRunning = 0xFFFFFFFFu;
    s_blockBytes = 0;
    s_fixing = false;
    s_fixBlock = 0;
    s_fixOffset = 0;
    s_fixLen = 0;
    s_fixNeeded = 0;
    s_fixCrcRunning = 0xFFFFFFFFu;
    memset(s_blockBad, 0, sizeof(s_blockBad));
}

static inline bool blockIsBad(uint16_t index) {
    if (index >= s_blockCount) return false;
    return (s_blockBad[index >> 3] & (1u << (index & 7))) != 0;
}

static inline void markBlock(uint16_t index, bool bad) {
    if (index >= s_blockCount) return;
    const uint8_t mask = (uint8_t)(1u << (index & 7));
    const bool wasBad = (s_blockBad[index >> 3] & mask) != 0;
    if (bad && !wasBad) {
        s_blockBad[index >> 3] |= mask;
        s_blockBadCount++;
    } else if (!bad && wasBad) {
        s_blockBad[index >> 3] &= (uint8_t)~mask;
        if (s_blockBadCount) s_blockBadCount--;
    }
}

// Write one payload byte at an arbitrary payload offset, rotating it into the
// panel framebuffer. Shared by the normal path and the repair path.
static inline void storePayloadByte(uint32_t offset, uint8_t b) {
    if (s_format == BLE_FMT_2BPP) {
        const int ys = (int)(offset / BLE_IMAGE_ROW_BYTES_2BPP);
        const int xs = (int)(offset % BLE_IMAGE_ROW_BYTES_2BPP) * 4;
        bleSetCanvasPixel(s_frame, xs + 0, ys, (uint8_t)((b >> 6) & 0x03));
        bleSetCanvasPixel(s_frame, xs + 1, ys, (uint8_t)((b >> 4) & 0x03));
        bleSetCanvasPixel(s_frame, xs + 2, ys, (uint8_t)((b >> 2) & 0x03));
        bleSetCanvasPixel(s_frame, xs + 3, ys, (uint8_t)(b & 0x03));
    } else {
        // 1bpp: one bit per pixel, 1 = white, expanded into four-byte groups.
        const int ys = (int)(offset / BLE_IMAGE_ROW_BYTES_1BPP);
        const int xs = (int)(offset % BLE_IMAGE_ROW_BYTES_1BPP) * 8;
        for (int k = 0; k < 8; k++) {
            const bool white = ((b >> (7 - k)) & 0x01) != 0;
            bleSetCanvasPixel(s_frame, xs + k, ys, white ? EPD398_WHITE : EPD398_BLACK);
        }
    }
}

// Consume one DATA write. Runs in the NimBLE host task.
//
// Two modes: normal reception (bytes go to s_received and roll into the current
// block) and block repair (bytes go to the armed block's fixed offset). Both go
// through storePayloadByte(), so the rotation and the format handling exist once.
static void handleData(const uint8_t *d, size_t n) {
    if (n == 0) return;

    StateLock lock;

    if (!s_receiving) {
        // A stray write outside a transfer. Ignore it rather than tear the link
        // down - the app may simply have raced an ABORT, and a chunk that was
        // already queued when END_IMAGE landed must not cost that app its image.
        // No unconditional log either: an app that keeps pipelining after
        // END_IMAGE would flood the console. s_dataHint is what the next
        // END_IMAGE uses to explain itself, and the single throttled line below
        // covers the case that really happens - data arriving while the previous
        // image is being painted. (A repeated chunk can keep the
        // BLE_DATA_CHUNK_MAX window open past the throttle, so a progress report
        // is no guarantee that the app has heard anything.)
        if (s_renderPending || s_rendering) {
            s_dataHint = BLE_HINT_DATA_AFTER_END;
            if (throttleLogDue()) {
                BLE_LOG("[BLE] DATA during the render of the previous image ignored "
                        "(the app is pipelining past its %u-chunk window)\n",
                        (unsigned)BLE_WINDOW_CHUNKS);
            }
        }
        return;
    }
    if (!s_frame) {
        // Defensive: BEGIN_IMAGE refuses without a framebuffer, so a session
        // cannot exist here. Count the bytes instead of tearing the session
        // down; the verification below then fails on its own.
        s_strayBytes += (uint32_t)n;
        return;
    }

    // Any byte accepted for the active session proves the app is still there.
    s_lastRxMs = millis();
    s_firstByteSeen = true;

    if (s_fixing) {
        // Repair: fill the armed block, ignoring anything past its length so a
        // stale in-flight chunk from before cannot corrupt the next block.
        //
        // NOTE: s_received deliberately does NOT move here. It counts payload
        // bytes received, and a repair replaces bytes that were already counted;
        // incrementing it would push the total past s_total and make the next
        // END_IMAGE look like an overflow.
        // NOTE: the destination offset advances with the bytes already folded
        // into this block, not with the local index of the chunk. Using `i`
        // directly pinned every chunk to the start of the block, so a block
        // larger than one chunk ended up holding its first chunk repeated.
        size_t take = n;
        if (take > s_fixNeeded) take = (size_t)s_fixNeeded;
        const uint32_t consumed = s_fixLen - s_fixNeeded;
        for (size_t i = 0; i < take; i++) {
            storePayloadByte(s_fixOffset + consumed + (uint32_t)i, d[i]);
        }
        s_fixCrcRunning = crc32Update(s_fixCrcRunning, d, take);
        s_fixNeeded -= (uint32_t)take;

        if (s_fixNeeded == 0) {
            const bool bad = crc32Finish(s_fixCrcRunning) != s_blockCrcDeclared[s_fixBlock];
            markBlock(s_fixBlock, bad);
            BLE_LOG("[BLE] block %u repaired -> %s\n", (unsigned)s_fixBlock,
                    bad ? "STILL BAD" : "ok");
            s_fixing = false;
            sendStateEvent(BLE_STATE_RECEIVING, BLE_OK);
            sendProgress(true);
        }
        return;
    }

    // Bytes past the declared length are ignored, not fatal: they cannot be
    // stored anywhere (every payload offset is bounded by s_total), and an app
    // that repeats a chunk used to lose an image that was already complete and
    // correct. They are counted so END_IMAGE can name the real problem when the
    // image does not verify either.
    uint32_t take = (s_received < s_total) ? (s_total - s_received) : 0;
    if (take > (uint32_t)n) take = (uint32_t)n;
    if (take < (uint32_t)n) s_strayBytes += (uint32_t)n - take;

    // ONE pass: store the byte, roll it into the current block, and close the
    // block the moment its last byte lands.
    //
    // This has to be a single loop. Doing the block bookkeeping in a second pass
    // (as an earlier version did) means the CRC accumulator consumed every byte
    // of the chunk before any boundary was noticed, so whenever a block boundary
    // fell inside a chunk the bytes *after* the boundary were folded into the
    // previous block's CRC and every block from there on reported a mismatch.
    // With the default 512-byte chunks and 4096-byte blocks the boundary always
    // lands on a chunk edge, which is exactly why it stayed hidden until a
    // non-dividing chunk size was tested.
    for (size_t i = 0; i < take; i++) {
        const uint32_t offset = s_received + (uint32_t)i;
        storePayloadByte(offset, d[i]);
        if (s_blockShift == 0) continue;

        // A previous boundary can be sitting at a chunk edge. Reset the
        // accumulator here, on the first byte of the new chunk, rather than at
        // the boundary itself: if that whole write was lost, the full
        // s_blockBytes is what lets closeMissingBlocks() name the lost block.
        if (s_blockBytes == s_blockSize) {
            s_blockCrcRunning = 0xFFFFFFFFu;
            s_blockBytes = 0;
        }

        s_blockCrcRunning = crc32Update(s_blockCrcRunning, &d[i], 1);
        s_blockBytes++;
        // The final block of an image is usually short: close it on the last
        // declared byte rather than waiting for a full block that never comes.
        if (s_blockBytes == s_blockSize || (offset + 1) == s_total) {
            const bool bad =
                crc32Finish(s_blockCrcRunning) != s_blockCrcDeclared[s_blockIndex];
            markBlock(s_blockIndex, bad);
            // Throttled: the bitmap and s_blockBadCount stay exact either way,
            // only the console line is skipped.
            if (bad && throttleLogDue()) {
                BLE_LOG("[BLE] block %u FAILED crc %08lX != %08lX\n",
                        (unsigned)s_blockIndex,
                        (unsigned long)crc32Finish(s_blockCrcRunning),
                        (unsigned long)s_blockCrcDeclared[s_blockIndex]);
            }
            s_blockIndex++;
            // Deliberately NOT resetting s_blockCrcRunning/s_blockBytes here if
            // this is the end of the chunk; see the reset above.
        }
    }
    s_received += take;

    // Whatever the app still holds is in flight: a chunk that arrived past the
    // declared length means its END_IMAGE may already be on the way.
    s_dataHint = s_strayBytes ? BLE_HINT_DATA_IGNORED
                              : ((s_received >= s_total) ? BLE_HINT_DATA_AFTER_END
                                                         : BLE_HINT_NONE);

    s_chunksInWindow++;
    if (s_received >= s_total || s_chunksInWindow >= BLE_WINDOW_CHUNKS) {
        s_chunksInWindow = 0;
        sendProgress(false);
    }
}

// Rebuild the payload digest from the framebuffer.
//
// Reading the canvas back through bleGetCanvasPixel() is what makes this honest
// after a repair, and re-packing is exact in both directions: 2bpp stores the
// four colour codes verbatim, and a 1bpp payload only ever contained black and
// white, so the threshold back to one bit loses nothing.
static uint32_t recomputeWholeCrc() {
    if (!s_frame) return 0;
    uint32_t crc = 0xFFFFFFFFu;
    uint8_t out[BLE_IMAGE_ROW_BYTES_2BPP];
    int used = 0;

    if (s_format == BLE_FMT_2BPP) {
        for (int y = 0; y < BLE_IMAGE_HEIGHT; y++) {
            used = 0;
            for (int x = 0; x < BLE_IMAGE_WIDTH; x += 4) {
                const uint8_t packed =
                    (uint8_t)((bleGetCanvasPixel(s_frame, x + 0, y) << 6) |
                              (bleGetCanvasPixel(s_frame, x + 1, y) << 4) |
                              (bleGetCanvasPixel(s_frame, x + 2, y) << 2) |
                              (bleGetCanvasPixel(s_frame, x + 3, y)));
                out[used++] = packed;
            }
            crc = crc32Update(crc, out, (size_t)used);
        }
    } else {
        for (int y = 0; y < BLE_IMAGE_HEIGHT; y++) {
            used = 0;
            for (int x = 0; x < BLE_IMAGE_WIDTH; x += 8) {
                uint8_t b = 0;
                for (int k = 0; k < 8; k++) {
                    if (bleGetCanvasPixel(s_frame, x + k, y) == EPD398_WHITE) {
                        b = (uint8_t)(b | (0x80 >> k));
                    }
                }
                out[used++] = b;
            }
            crc = crc32Update(crc, out, (size_t)used);
        }
    }
    return crc32Finish(crc);
}

// ---------------------------------------------------------------- requests

// Device capabilities. The layout is described in docs/BLE_PROTOCOL.md §5.2;
// image_width/height are the *canvas* the app must produce, panel_width/height
// are what the glass actually is, and rotation is what the device applies
// between the two (informational - the app never pre-rotates).
//
// v3 appends the refresh cooldown *after* the variable-length device name, so
// every offset a v2 parser already knows stays where it was: read the name via
// device_name_len and then the two trailing bytes.
static void handleGetInfo(uint8_t seq) {
    // Sized for the fixed block plus the longest name this firmware can build
    // (s_name is 40 bytes), plus the trailing v3 field. The reply is far below
    // BLE_FRAME_MAX_PAYLOAD either way; this just removes a silent overflow if
    // the name is ever made longer.
    uint8_t p[96];
    size_t o = 0;
    const size_t nameLen = strlen(s_name);

    // 39 fixed bytes + the name. If the negotiated MTU cannot carry the reply,
    // say so instead of letting the stack truncate it into a malformed frame.
    if (!frameFits(39 + nameLen)) {
        BLE_LOG("[GET_INFO] MTU %u too small for a %u-byte reply\n",
                (unsigned)s_mtu, (unsigned)(39 + nameLen));
        sendResponse(BLE_OP_GET_INFO, seq, BLE_ERR_MTU, nullptr, 0);
        return;
    }

    p[o++] = BLE_PROTOCOL_VERSION;
    p[o++] = FW_VERSION_MAJOR;
    p[o++] = FW_VERSION_MINOR;
    p[o++] = FW_VERSION_PATCH;
    bleWr16(p + o, (uint16_t)BLE_IMAGE_WIDTH);   o += 2;
    bleWr16(p + o, (uint16_t)BLE_IMAGE_HEIGHT);  o += 2;
    bleWr16(p + o, (uint16_t)EPD398_WIDTH);      o += 2;
    bleWr16(p + o, (uint16_t)EPD398_HEIGHT);     o += 2;
    p[o++] = EPD398_BPP;
    p[o++] = 4;
    p[o++] = EPD398_BLACK;
    p[o++] = EPD398_WHITE;
    p[o++] = EPD398_YELLOW;
    p[o++] = EPD398_RED;
    bleWr32(p + o, (uint32_t)EPD398_COLOR_BYTES);       o += 4;  // BLE_FMT_2BPP
    bleWr32(p + o, (uint32_t)(EPD398_COLOR_BYTES / 2)); o += 4;  // BLE_FMT_1BPP
    bleWr16(p + o, s_mtu);              o += 2;
    bleWr16(p + o, dataChunkSize());    o += 2;
    bleWr16(p + o, BLE_WINDOW_CHUNKS);  o += 2;
    p[o++] = (uint8_t)BLE_ROTATION;
    // v2 capability block: which block shifts are accepted and how many blocks
    // the failure bitmap can hold. A v1 app simply ignores the extra bytes.
    p[o++] = BLE_BLOCK_SHIFT_MIN;
    p[o++] = BLE_BLOCK_SHIFT_MAX;
    p[o++] = (uint8_t)BLE_MAX_BLOCKS;
    p[o++] = (uint8_t)nameLen;
    memcpy(p + o, s_name, nameLen);
    o += nameLen;

    // v3: the minimum interval between full refreshes. The app needs it before
    // it sends anything, to tell the user "the panel was refreshed N seconds
    // ago" up front instead of after a refused request.
    bleWr16(p + o, refreshGuardMinGapSeconds());
    o += 2;

    sendResponse(BLE_OP_GET_INFO, seq, BLE_OK, p, o);
}

static void handleBeginImage(uint8_t seq, const uint8_t *p, size_t len) {
    if (len < 12) { sendResponse(BLE_OP_BEGIN_IMAGE, seq, BLE_ERR_LENGTH, nullptr, 0); return; }
    if (s_renderPending || s_rendering) {
        sendResponse(BLE_OP_BEGIN_IMAGE, seq, BLE_ERR_BUSY, nullptr, 0);
        return;
    }
    if (s_receiving) {
        // A valid new BEGIN supersedes an unfinished or failed session: the app
        // is explicitly starting over (docs: after ERR_CRC, retry from BEGIN).
        // Log it here, but keep the old session alive until the new request has
        // passed validation and the cooldown gate.
        BLE_LOG("[BLE] BEGIN supersedes unfinished session (%lu/%lu bytes)\n",
                (unsigned long)s_received, (unsigned long)s_total);
    }

    const uint8_t format = p[0];
    const uint8_t blockShift = p[2];
    const uint32_t total = bleRd32(p + 4);
    const uint32_t crc   = bleRd32(p + 8);

    // flags and reserved are documented as must-be-0. Accepting garbage here
    // would silently reinterpret a future frame version as this one.
    if (p[1] != 0 || p[3] != 0) {
        sendResponse(BLE_OP_BEGIN_IMAGE, seq, BLE_ERR_SIZE, nullptr, 0);
        return;
    }

    const uint32_t want = expectedLength(format);
    if (want == 0) { sendResponse(BLE_OP_BEGIN_IMAGE, seq, BLE_ERR_FORMAT, nullptr, 0); return; }
    if (total != want) { sendResponse(BLE_OP_BEGIN_IMAGE, seq, BLE_ERR_SIZE, nullptr, 0); return; }
    if (!s_frame) { sendResponse(BLE_OP_BEGIN_IMAGE, seq, BLE_ERR_MEM, nullptr, 0); return; }

    if (blockShift != 0 &&
        (blockShift < BLE_BLOCK_SHIFT_MIN || blockShift > BLE_BLOCK_SHIFT_MAX)) {
        sendResponse(BLE_OP_BEGIN_IMAGE, seq, BLE_ERR_SIZE, nullptr, 0);
        return;
    }

    // The refresh cooldown. Checked here rather than at END_IMAGE so a refusal
    // costs nothing - the alternative is transferring 106 KB and only then
    // being told no. A grant the user confirmed earlier is accepted here and
    // stays armed until the panel is actually painted, so a transfer that fails
    // on CRC does not make the user confirm twice.
    if (!refreshGuardMayRefreshFor(BLE_OP_BEGIN_IMAGE)) {
        sendRefreshBlocked(BLE_OP_BEGIN_IMAGE, seq);
        return;
    }

    resetTransfer();
    s_receiving = true;
    s_lastRxMs = millis();
    s_firstByteSeen = false;
    s_format = format;
    s_total = total;
    s_crcDeclared = crc;
    s_chunkSize = dataChunkSize();
    s_session++;
    // A new session starts with a clean slate, including the diagnosis the next
    // ERR_STATE END_IMAGE would carry.
    s_dataHint = BLE_HINT_NONE;

    s_blockShift = blockShift;
    if (blockShift != 0) {
        s_blockSize = 1u << blockShift;
        s_blockCount = (uint16_t)((total + s_blockSize - 1) / s_blockSize);
        if (s_blockCount > BLE_MAX_BLOCKS) {
            resetTransfer();
            sendResponse(BLE_OP_BEGIN_IMAGE, seq, BLE_ERR_SIZE, nullptr, 0);
            return;
        }
        // The END_IMAGE failure reply carries a bitmap of ceil(count/8) bytes.
        // If it cannot fit the negotiated MTU the app could never learn which
        // blocks to retransmit, so refuse before any data is transferred.
        if (!frameFits(10 + (size_t)((s_blockCount + 7) / 8))) {
            resetTransfer();
            BLE_LOG("[BLE] MTU %u too small for a %u-block failure bitmap\n",
                    (unsigned)s_mtu, (unsigned)s_blockCount);
            sendResponse(BLE_OP_BEGIN_IMAGE, seq, BLE_ERR_MTU, nullptr, 0);
            return;
        }
        // Until BLOCK_CRCS arrives, every block counts as unverified. A block
        // whose declared CRC is still the initial 0 is also failed by the
        // comparison, so an app that forgets BLOCK_CRCS gets ERR_BLOCKS rather
        // than a silently accepted image.
        memset(s_blockCrcDeclared, 0, sizeof(s_blockCrcDeclared));
    }

    uint8_t r[12];
    bleWr16(r, s_chunkSize);
    bleWr16(r + 2, BLE_WINDOW_CHUNKS);
    bleWr32(r + 4, s_total);
    r[8] = s_session;
    r[9] = s_format;
    r[10] = s_blockShift;
    r[11] = (uint8_t)s_blockCount;
    sendResponse(BLE_OP_BEGIN_IMAGE, seq, BLE_OK, r, sizeof(r));

    BLE_LOG("[BLE] BEGIN format=%u total=%lu chunk=%u session=%u blockShift=%u blocks=%u\n",
            (unsigned)format, (unsigned long)total, (unsigned)s_chunkSize,
            (unsigned)s_session, (unsigned)s_blockShift, (unsigned)s_blockCount);
    sendStateEvent(BLE_STATE_RECEIVING, BLE_OK);
}

static void handleBlockCrcs(uint8_t seq, const uint8_t *p, size_t len) {
    if (len < 4) { sendResponse(BLE_OP_BLOCK_CRCS, seq, BLE_ERR_LENGTH, nullptr, 0); return; }
    if (!s_receiving) { sendResponse(BLE_OP_BLOCK_CRCS, seq, BLE_ERR_STATE, nullptr, 0); return; }
    if (s_blockShift == 0) {
        // The app asked for block CRCs in BEGIN? No - it did not, so this frame
        // has nothing to attach to.
        sendResponse(BLE_OP_BLOCK_CRCS, seq, BLE_ERR_STATE, nullptr, 0);
        return;
    }
    if (s_received != 0) {
        // Must arrive before the first DATA byte, otherwise the device has
        // already closed blocks it cannot retroactively verify.
        sendResponse(BLE_OP_BLOCK_CRCS, seq, BLE_ERR_STATE, nullptr, 0);
        return;
    }

    const uint16_t blockSize = bleRd16(p);
    const uint16_t count = bleRd16(p + 2);
    if (blockSize != s_blockSize || count != s_blockCount ||
        len < (size_t)(4 + 4 * count)) {
        sendResponse(BLE_OP_BLOCK_CRCS, seq, BLE_ERR_SIZE, nullptr, 0);
        return;
    }

    for (uint16_t i = 0; i < count; i++) {
        s_blockCrcDeclared[i] = bleRd32(p + 4 + 4 * i);
    }
    s_lastRxMs = millis();
    const uint8_t echo = (uint8_t)count;
    sendResponse(BLE_OP_BLOCK_CRCS, seq, BLE_OK, &echo, 1);
    BLE_LOG("[BLE] %u block CRCs accepted (block size %lu)\n",
            (unsigned)count, (unsigned long)s_blockSize);
}

static void handleRetryBlock(uint8_t seq, const uint8_t *p, size_t len) {
    if (len < 2) { sendResponse(BLE_OP_RETRY_BLOCK, seq, BLE_ERR_LENGTH, nullptr, 0); return; }
    if (!s_receiving || s_blockShift == 0) {
        sendResponse(BLE_OP_RETRY_BLOCK, seq, BLE_ERR_STATE, nullptr, 0);
        return;
    }
    const uint16_t index = bleRd16(p);
    // reserved must be 0, exactly as BEGIN_IMAGE's flags/reserved are checked:
    // accepting garbage there would silently reinterpret a future frame version
    // as this one. Only checked when the field is present, so a 2-byte frame
    // from an app that omits it keeps working.
    if (len >= 4 && bleRd16(p + 2) != 0) {
        sendResponse(BLE_OP_RETRY_BLOCK, seq, BLE_ERR_SIZE, nullptr, 0);
        return;
    }
    if (index >= s_blockCount) {
        sendResponse(BLE_OP_RETRY_BLOCK, seq, BLE_ERR_SIZE, nullptr, 0);
        return;
    }
    if (!blockIsBad(index)) {
        // Already good - nothing to do, but do not make the app treat that as a
        // failure. The response carries a zero length so it knows to skip it.
        const uint8_t r[4] = {0, 0, 0, 0};
        sendResponse(BLE_OP_RETRY_BLOCK, seq, BLE_OK, r, sizeof(r));
        return;
    }

    // Re-arming while a repair is still outstanding (docs: one block at a time)
    // silently drops the bytes already received for the previous block; that
    // block stays marked bad, so the app can still recover, but it also loses
    // what it already sent. Logged before the state is overwritten, so the
    // numbers mean something.
    if (s_fixing && throttleLogDue()) {
        BLE_LOG("[BLE] RETRY_BLOCK %u supersedes block %u with %lu of %lu bytes outstanding\n",
                (unsigned)index, (unsigned)s_fixBlock,
                (unsigned long)s_fixNeeded, (unsigned long)s_fixLen);
    }
    s_fixBlock = index;
    s_fixOffset = (uint32_t)index * s_blockSize;
    uint32_t remain = s_total - s_fixOffset;
    s_fixLen = (remain < s_blockSize) ? remain : s_blockSize;
    s_fixNeeded = s_fixLen;
    s_fixCrcRunning = 0xFFFFFFFFu;
    s_fixing = true;
    s_lastRxMs = millis();

    // The byte count the app must send next, so it does not have to derive the
    // short final block itself.
    uint8_t r[4];
    bleWr16(r, index);
    bleWr16(r + 2, (uint16_t)s_fixNeeded);
    sendResponse(BLE_OP_RETRY_BLOCK, seq, BLE_OK, r, sizeof(r));
    sendStateEvent(BLE_STATE_FIXING, BLE_OK);
    BLE_LOG("[BLE] RETRY block %u (%lu bytes)\n",
            (unsigned)index, (unsigned long)s_fixNeeded);
}

// Account for blocks whose bytes never arrived.
//
// The per-chunk reset in handleData() marks the blocks whose bytes are missing
// or shifted; if the loss includes the final block, nothing ever closes it and
// END_IMAGE cannot name it. Closing the tail here (partial current block first,
// then every remaining one) marks those blocks bad so ERR_BLOCKS carries a
// bitmap the app can repair. It does NOT change s_received: that is a wire-byte
// counter, and a repaired block replaces bytes that were already counted.
static void closeMissingBlocks() {
    if (s_blockShift == 0 || s_blockCount == 0) return;
    if (s_blockIndex >= s_blockCount) return;

    if (s_blockBytes != 0) {
        // A partial block cannot match a CRC declared over the full block.
        markBlock(s_blockIndex, true);
        s_blockIndex++;
    }
    while (s_blockIndex < s_blockCount) {
        markBlock(s_blockIndex, true);
        s_blockIndex++;
    }
    s_blockCrcRunning = 0xFFFFFFFFu;
    s_blockBytes = 0;
}

static void handleEndImage(uint8_t seq, const uint8_t *p, size_t len) {
    if (len < 4) { sendResponse(BLE_OP_END_IMAGE, seq, BLE_ERR_LENGTH, nullptr, 0); return; }
    if (!s_receiving) {
        // There is no session to verify. Two apps get here: one that raced an
        // ABORT, and one that pipelined its DATA straight into the paint of the
        // image it just finished - the log throttle is per BLE_DATA_CHUNK_MAX
        // window, so a fast link keeps that window open well past it, and this is
        // the only answer such an app will ever get. The hint byte says which.
        const uint8_t hint = s_dataHint;   // volatile -> a plain copy to pass on
        sendResponse(BLE_OP_END_IMAGE, seq, BLE_ERR_STATE, &hint, 1);
        return;
    }

    // Ending a transfer is activity: the app may legitimately need a while to
    // inspect ERR_BLOCKS and issue RETRY_BLOCK, so the stall clock restarts
    // here rather than expiring while it decides.
    s_lastRxMs = millis();

    const uint32_t declared = bleRd32(p);
    // Captured before any resetTransfer(), so both the success log and the
    // failure branch report the same number.
    const uint32_t stray = s_strayBytes;
    // Sized for the largest possible failure reply: 8 bytes of CRC/byte count,
    // the two ERR_BLOCKS counters and the 16-byte bitmap of BLE_MAX_BLOCKS.
    uint8_t r[4 + 4 + 2 + (BLE_MAX_BLOCKS + 7) / 8];
    // What a successful reply carries, and nothing more: the documented
    // success payload is fixed at 8 bytes, so sizeof(r) (26) must NOT be sent.
    const size_t OK_PAYLOAD_LEN = 8;

    sendStateEvent(BLE_STATE_VERIFYING, BLE_OK);

    // Close any block the link dropped, so the failure bitmap can name it.
    closeMissingBlocks();

    // Whole-image digest, recomputed from what actually ended up in the buffer.
    // Doing it here (rather than accumulating during reception) is what keeps
    // the reported value correct after a repair.
    const uint32_t actual = recomputeWholeCrc();

    const bool complete = (s_received == s_total);
    const bool blocksEnabled = (s_blockShift != 0);
    // The CRC in BEGIN and in END describe the same payload; if they disagree
    // the app is confused (or the frame is corrupt), so fail before touching
    // the panel. The recomputed value is reported either way.
    const bool declaredOk = (s_crcDeclared == declared);
    // Every block present and verified. s_blockIndex reaching s_blockCount means
    // all blocks were closed - normally by received bytes, otherwise by
    // closeMissingBlocks() + RETRY_BLOCK. A repaired short block fills the
    // framebuffer, so the wire byte count no longer has to equal s_total.
    const bool blocksOk = blocksEnabled &&
                          (s_blockBadCount == 0) &&
                          (s_blockIndex == s_blockCount);

    bool ok;
    if (blocksEnabled) {
        // Per-block CRCs are strictly stronger than one digest over everything,
        // so they decide. The whole-image CRC is still reported for the app to
        // cross-check, but a mismatch there would mean a bug, not a bad link.
        // Bytes past total_len are ignored above and cannot make a verified
        // image fail, so a fully verified payload is accepted even then.
        ok = blocksOk && declaredOk && (actual == declared);
    } else {
        ok = complete && declaredOk && (actual == declared);
    }

    if (ok) {
        const uint8_t doneFormat = s_format;
        const uint32_t doneBytes = s_total;
        resetTransfer();
        s_renderFormat = doneFormat;
        s_renderBytes = doneBytes;
        s_renderPending = true;

        bleWr32(r, doneBytes);
        bleWr32(r + 4, actual);
        sendResponse(BLE_OP_END_IMAGE, seq, BLE_OK, r, OK_PAYLOAD_LEN);
        BLE_LOG("[BLE] transfer OK: %lu bytes, crc %08lX\n",
                (unsigned long)doneBytes, (unsigned long)actual);
                if (stray) {
                    // Tolerated, but the app's chunk arithmetic is worth fixing: the
                    // next image may not be so lucky.
                    BLE_LOG("[BLE]   note: %lu byte(s) past total_len were ignored\n",
                            (unsigned long)stray);
                }
                return;
    }

    // Failure. Report enough for the app to decide between "resend some blocks"
    // and "start over", without a second round trip.
    bleWr32(r, actual);
    bleWr32(r + 4, s_received);

    uint8_t status;
    size_t plen;
    if (stray != 0) {
        // Bytes arrived past the declared length. They were ignored (nothing
        // could have been stored) and the payload did NOT verify either, so the
        // app's framing is the suspect - a more useful answer than a bitmap of
        // a mis-framed stream, and the session stays retryable.
        plen = 8;
        status = BLE_ERR_OVERFLOW;
        BLE_LOG("[BLE] END: %lu bytes over total_len (%lu/%lu), crc %08lX (want %08lX)\n",
                    (unsigned long)stray, (unsigned long)s_received,
                (unsigned long)s_total, (unsigned long)actual,
                (unsigned long)declared);
    } else if (blocksEnabled && s_blockBadCount > 0) {
        const size_t bitmapLen = (s_blockCount + 7) / 8;
        r[8] = (uint8_t)s_blockCount;
        r[9] = (uint8_t)s_blockBadCount;
        memcpy(r + 10, s_blockBad, bitmapLen);
        plen = 10 + bitmapLen;
        status = BLE_ERR_BLOCKS;
        BLE_LOG("[BLE] END: %u/%u blocks bad, %lu/%lu bytes, crc %08lX (want %08lX)\n",
                (unsigned)s_blockBadCount, (unsigned)s_blockCount,
                (unsigned long)s_received, (unsigned long)s_total,
                (unsigned long)actual, (unsigned long)declared);
    } else {
        plen = 8;
        status = BLE_ERR_CRC;
        BLE_LOG("[BLE] END: %lu/%lu bytes, crc %08lX (want %08lX)\n",
                (unsigned long)s_received, (unsigned long)s_total,
                (unsigned long)actual, (unsigned long)declared);
    }
    sendResponse(BLE_OP_END_IMAGE, seq, status, r, plen);

    // Stay in the receiving state so the app can RETRY_BLOCK without a fresh
    // BEGIN_IMAGE. An ABORT or a new BEGIN_IMAGE clears it. The event is what
    // stops an app that tracks EVT_STATE from sitting at VERIFYING forever.
    sendStateEvent(BLE_STATE_RECEIVING, BLE_OK);
}

// The app's clock, adopted once per connection (or whenever it wants to correct
// it). This is the only time source the device has: it never connects to
// anything, and its RTC does not survive a power cut. The value is anchored to
// the device's own monotonic uptime, so time keeps advancing on its own from
// then on and the cooldown becomes an exact comparison rather than a bound.
static void handleSyncTime(uint8_t seq, const uint8_t *p, size_t len) {
    if (len < 8) { sendResponse(BLE_OP_SYNC_TIME, seq, BLE_ERR_LENGTH, nullptr, 0); return; }

    // UTC seconds since the epoch, little-endian, 64-bit. 64 bits rather than
    // 32 so that the classic mistake - sending milliseconds from
    // System.currentTimeMillis() - cannot be silently truncated into something
    // that happens to look like a plausible date.
    uint64_t utc = 0;
    for (int i = 7; i >= 0; i--) utc = (utc << 8) | p[i];

    if (!refreshGuardSyncClock(utc)) {
        sendResponse(BLE_OP_SYNC_TIME, seq, BLE_ERR_SIZE, nullptr, 0);
        return;
    }

    uint8_t r[8];
    bleWr32(r, refreshGuardLastRefreshUtc());   // 0 = none, or it could not be dated
    bleWr16(r + 4, refreshGuardMinGapSeconds());
    r[6] = 1;   // clock known from here on
    r[7] = 0;   // reserved
    sendResponse(BLE_OP_SYNC_TIME, seq, BLE_OK, r, sizeof(r));

    BLE_LOG("[BLE] clock synced: last refresh %lu, min gap %u s\n",
            (unsigned long)refreshGuardLastRefreshUtc(),
            (unsigned)refreshGuardMinGapSeconds());
}

// The user accepted the warning. Arms a one-shot authorisation covering the
// command the app is about to repeat; it is consumed when the panel is actually
// painted, so it cannot be replayed for a second refresh.
static void handleConfirmRefresh(uint8_t seq, const uint8_t *p, size_t len) {
    if (len < 1) { sendResponse(BLE_OP_CONFIRM_REFRESH, seq, BLE_ERR_LENGTH, nullptr, 0); return; }

    const uint8_t token = p[0];
    if (!refreshGuardArmGrant(token)) {
        // Unknown, stale, already-used, or too soon after a forced refresh.
        // The app should treat this as "ask the user again" rather than
        // retrying blindly.
        sendResponse(BLE_OP_CONFIRM_REFRESH, seq, BLE_ERR_STATE, nullptr, 0);
        return;
    }
    sendResponse(BLE_OP_CONFIRM_REFRESH, seq, BLE_OK, &token, 1);
}

static void handleControlFrame(const uint8_t *f, size_t n) {
    if (n < BLE_FRAME_HEADER_LEN) return;   // too short to even read a header

    // One lock for the whole request: a control frame mutates several fields
    // that the loop task also reads (session, render flags, hint flag), and the
    // handlers below may nest into the refresh guard. Lock order is always
    // state -> guard; nothing takes them in the other order.
    StateLock lock;

    const uint8_t opcode = f[0];
    const uint8_t seq = f[1];
    const uint8_t plen = f[3];

    // Range-check the opcode first, so every sendResponse() below is guaranteed
    // to build an opcode inside the response range (0x81..0xBF) rather than
    // something that looks like an event.
    if (opcode == 0 || opcode > BLE_OP_MAX_REQUEST) {
        pushFrame(BLE_OP_ERROR, seq, BLE_ERR_OPCODE, &opcode, 1);
        return;
    }

    if ((size_t)plen + BLE_FRAME_HEADER_LEN > n) {
        sendResponse(opcode, seq, BLE_ERR_LENGTH, nullptr, 0);
        return;
    }
    const uint8_t *p = f + BLE_FRAME_HEADER_LEN;

    switch (opcode) {
        case BLE_OP_GET_INFO:
            handleGetInfo(seq);
            break;

        case BLE_OP_BEGIN_IMAGE:
            handleBeginImage(seq, p, plen);
            break;

        case BLE_OP_BLOCK_CRCS:
            handleBlockCrcs(seq, p, plen);
            break;

        case BLE_OP_RETRY_BLOCK:
            handleRetryBlock(seq, p, plen);
            break;

        case BLE_OP_SYNC_TIME:
            handleSyncTime(seq, p, plen);
            break;

        case BLE_OP_CONFIRM_REFRESH:
            handleConfirmRefresh(seq, p, plen);
            break;

        case BLE_OP_END_IMAGE:
            handleEndImage(seq, p, plen);
            break;

        case BLE_OP_ABORT:
            resetTransfer();
            s_renderPending = false;   // the app is abandoning this image too
            s_hintRequested = false;
            sendResponse(BLE_OP_ABORT, seq, BLE_OK, nullptr, 0);
            // A paint that is already running cannot be cancelled: the panel is
            // mid-waveform and stopping there would leave it in an undefined
            // state, so the DISPLAYED event still follows (disconnect-proof
            // rendering is deliberate). Saying so is what keeps an app from
            // believing the screen will not change.
            if (s_rendering) {
                static const char kCannotAbort[] = "render in progress, cannot be aborted";
                // Same discipline as PING: a frame the link cannot carry would be
                // truncated by the stack, so it is not sent at all.
                if (frameFits(sizeof(kCannotAbort) - 1)) {
                    sendEvent(BLE_EVT_LOG, (const uint8_t *)kCannotAbort,
                              sizeof(kCannotAbort) - 1);
                }
                BLE_LOG("[BLE] ABORT while rendering: the refresh runs to completion\n");
            }
            sendStateEvent(BLE_STATE_CONNECTED, BLE_OK);
            BLE_LOG("[BLE] ABORT\n");
            break;

        case BLE_OP_SHOW_HINT:
            // A hint is a full refresh of the same framebuffer; it cannot be
            // painted while a transfer is writing into it or an image is being
            // pushed to the panel. Returning OK here used to mean "accepted,
            // then silently dropped" - now the app learns why.
            if (s_receiving || s_renderPending || s_rendering) {
                sendResponse(BLE_OP_SHOW_HINT, seq, BLE_ERR_BUSY, nullptr, 0);
                break;
            }
            // Painting the hint screen is a full refresh like any other, so it
            // owes the panel the same cooldown. A confirmation grant only
            // authorises the command it was issued for.
            if (!refreshGuardMayRefreshFor(BLE_OP_SHOW_HINT)) {
                sendRefreshBlocked(BLE_OP_SHOW_HINT, seq);
                break;
            }
            s_hintRequested = true;
            sendResponse(BLE_OP_SHOW_HINT, seq, BLE_OK, nullptr, 0);
            break;

        case BLE_OP_PING:
            // Echo, but never a frame the link cannot carry: an ATT notification
            // longer than MTU-3 is silently TRUNCATED by the stack, which hands
            // the app a frame whose length byte promises more bytes than arrived.
            // That is the failure ERR_MTU exists for, and the doc caps PING at
            // 250 bytes of payload anyway.
            if (!frameFits(plen)) {
                BLE_LOG("[PING] MTU %u cannot echo a %u-byte payload\n",
                        (unsigned)s_mtu, (unsigned)plen);
                sendResponse(BLE_OP_PING, seq, BLE_ERR_MTU, nullptr, 0);
                break;
            }
            sendResponse(BLE_OP_PING, seq, BLE_OK, p, plen);
            break;

        case BLE_OP_SET_OPTIONS: {
            if (plen < 1) { sendResponse(BLE_OP_SET_OPTIONS, seq, BLE_ERR_LENGTH, nullptr, 0); break; }
            const uint8_t opts = p[0];
            if (opts & (uint8_t)~BLE_OPT_VALID_MASK) {
                sendResponse(BLE_OP_SET_OPTIONS, seq, BLE_ERR_OPTIONS, nullptr, 0);
                break;
            }
            s_options = opts;
            const uint8_t echo = opts;
            sendResponse(BLE_OP_SET_OPTIONS, seq, BLE_OK, &echo, 1);
            BLE_LOG("[BLE] options=0x%02X\n", (unsigned)opts);
            break;
        }

        default:
            sendResponse(opcode, seq, BLE_ERR_OPCODE, nullptr, 0);
            break;
    }
}

// ---------------------------------------------------------------- callbacks

class ControlCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic *c, ble_gap_conn_desc *desc) override {
        (void)desc;
        const NimBLEAttValue v = c->getValue();
        handleControlFrame(v.data(), v.length());
    }

    void onSubscribe(NimBLECharacteristic *c, ble_gap_conn_desc *desc, uint16_t subValue) override {
        (void)c;
        (void)desc;
        if (subValue == 0) return;   // unsubscribed
        BLE_LOG("[BLE] CONTROL notifications enabled\n");
        sendStateEvent(s_receiving ? BLE_STATE_RECEIVING : BLE_STATE_CONNECTED, BLE_OK);
    }
};

class DataCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic *c, ble_gap_conn_desc *desc) override {
        (void)desc;
        const NimBLEAttValue v = c->getValue();
        handleData(v.data(), v.length());
    }
};

// Ask for the link parameters that decide the transfer speed.
//
// Nothing here is optional if the transfer is to run at more than ~10 kB/s:
// Android's default connection interval for an unprioritised peripheral is
// ~48 ms and it sends one write per event, which is exactly the 9.5 kB/s that
// was measured. Both calls are requests - the phone may clamp them - so the
// results are logged and the app is told to do its half as well.
static void applyFastLinkParams(uint16_t connHandle) {
    // Both calls are NimBLE requests, not commands: updateConnParams() and
    // setDataLen() return void in NimBLE-Arduino 1.4.3 (the phone may clamp or
    // refuse them silently), so the only thing that can be checked here is that
    // the request was made - the real outcome shows up in the connection
    // interval and MTU this same log line reports.
    if (s_server) {
        s_server->updateConnParams(connHandle,
                                   BLE_CONN_MIN_INTERVAL, BLE_CONN_MAX_INTERVAL,
                                   BLE_CONN_LATENCY, BLE_CONN_TIMEOUT);
        BLE_LOG("[BLE] conn params requested: %.2f-%.2f ms, latency %u, timeout %u ms\n",
                BLE_CONN_MIN_INTERVAL * 1.25, BLE_CONN_MAX_INTERVAL * 1.25,
                (unsigned)BLE_CONN_LATENCY, (unsigned)(BLE_CONN_TIMEOUT * 10));
    }

    // Data length extension. Without it the controller may keep the 27-byte
    // default LL payload and chop every 512-byte ATT write into ~20 PDUs.
    if (s_server) {
        s_server->setDataLen(connHandle, BLE_LL_TX_OCTETS);
        BLE_LOG("[BLE] data length requested: %u octets\n",
                (unsigned)BLE_LL_TX_OCTETS);
    }
}

class ServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer *server, ble_gap_conn_desc *desc) override {
        // One central only. The protocol has a single session and a single
        // framebuffer; a second peer could inject DATA into an active transfer
        // or read/steal the state events. Advertising is stopped on connect,
        // but a correctness rule must not depend on that default.
        if (s_connected) {
            BLE_LOG("[BLE] rejecting second central (handle %u)\n",
                    (unsigned)desc->conn_handle);
            server->disconnect(desc->conn_handle);
            return;
        }

        s_connected = true;
        s_everConnected = true;
        s_connCount++;
        s_options = 0;
        s_connHandle = desc->conn_handle;
        s_mtu = server->getPeerMTU(desc->conn_handle);

        BLE_LOG("[BLE] connected (#%lu, mtu=%u, interval %.2f ms)\n",
                (unsigned long)s_connCount, (unsigned)s_mtu,
                desc->conn_itvl * 1.25);

        applyFastLinkParams(desc->conn_handle);

        // The app is expected to subscribe; onSubscribe() sends the first state
        // event. This one covers centrals that write before subscribing.
        sendStateEvent(BLE_STATE_CONNECTED, BLE_OK);
    }

    void onDisconnect(NimBLEServer *server, ble_gap_conn_desc *desc) override {
        // A rejected second central must never tear down the active session.
        if (desc && s_connHandle != desc->conn_handle) {
            BLE_LOG("[BLE] rejected second central disconnected\n");
            return;
        }

        {
            StateLock lock;
            s_connected = false;
            resetTransfer();
            s_hintRequested = false;

            // s_renderPending is deliberately NOT cleared: an image that passed
            // every CRC check is complete in the framebuffer and is safe to
            // paint even if the phone walked away. ABORT is the explicit way to
            // cancel it.
        }

        BLE_LOG("[BLE] disconnected, advertising again\n");
        // advertiseOnDisconnect is disabled in bleInit, so this is the only
        // place advertising is restarted (the stack used to do it as well).
        if (server) server->startAdvertising();
    }

    void onMTUChange(uint16_t mtu, ble_gap_conn_desc *desc) override {
        (void)desc;
        s_mtu = mtu;
        BLE_LOG("[BLE] MTU now %u -> chunk %u\n", (unsigned)mtu,
                (unsigned)dataChunkSize());
    }
};

// ---------------------------------------------------------------- public API

bool bleInit(const char *deviceName, uint8_t *frameBuffer) {
    s_frame = frameBuffer;

    // Created before any callback can fire. Recursive because handlers nest.
    if (!s_stateMux) s_stateMux = xSemaphoreCreateRecursiveMutex();

    strncpy(s_name, deviceName ? deviceName : "8adge", sizeof(s_name) - 1);
    s_name[sizeof(s_name) - 1] = '\0';

    NimBLEDevice::init(s_name);
    NimBLEDevice::setMTU(BLE_MTU_PREFERRED);
    NimBLEDevice::setPower(ESP_PWR_LVL_P9);

    s_server = NimBLEDevice::createServer();
    s_server->setCallbacks(new ServerCallbacks());
    // Advertising is restarted by onDisconnect(); disabling the stack's own
    // automatic restart avoids doing it twice.
    s_server->advertiseOnDisconnect(false);

    NimBLEService *svc = s_server->createService(BLE_SERVICE_UUID);

    s_ctrl = svc->createCharacteristic(
        BLE_CTRL_UUID,
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::NOTIFY);
    s_ctrl->setCallbacks(new ControlCallbacks());

    // No explicit max_len: NimBLECharacteristic defaults it to
    // BLE_ATT_ATTR_MAX_LEN (512), and the host rejects any write whose length is
    // *greater* than that. That ceiling is exactly why BLE_DATA_CHUNK_MAX is 512
    // rather than "whatever the MTU allows" - with an MTU of 517 the ATT payload
    // would be 514 bytes and the write would come back as an ATT error.
    s_data = svc->createCharacteristic(
        BLE_DATA_UUID,
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
    s_data->setCallbacks(new DataCallbacks());

    svc->start();

    NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();

    // The advertisement data is built by hand rather than with
    // addServiceUUID(), because NimBLE packs the service UUID first and the
    // name only afterwards: 3 (flags) + 18 (128-bit UUID) + 16 (name) is 37
    // bytes, so the name would be pushed into the scan response and a passive
    // scan would see an anonymous device. Name-first is what the app filters on.
    //
    //   advertising packet : flags + complete local name   (19 bytes)
    //   scan response      : the 128-bit service UUID      (18 bytes)
    //
    // Android merges both into one ScanRecord, so a ScanFilter on the service
    // UUID matches even though the UUID is not in the advertising packet.
    NimBLEAdvertisementData advData;
    advData.setFlags(0x02 /* LE General Discoverable */);
    advData.setName(s_name);

    NimBLEAdvertisementData scanData;
    scanData.setCompleteServices(NimBLEUUID(BLE_SERVICE_UUID));

    adv->setAdvertisementData(advData);
    adv->setScanResponseData(scanData);
    adv->setScanResponse(true);
    adv->setMinInterval(0x20);   // 20 ms
    adv->setMaxInterval(0x40);   // 40 ms
    adv->start();

    BLE_LOG("[BLE] advertising as \"%s\"\n", s_name);
    return s_frame != nullptr;
}

const char *bleDeviceName() { return s_name; }

bool bleIsConnected() { return s_connected; }
bool bleHasEverConnected() { return s_everConnected; }
uint32_t bleConnectionCount() { return s_connCount; }
bool bleIsTransferring() { return s_receiving; }
bool bleIsFixingBlock() { return s_fixing; }

bool bleTakeRenderRequest(uint8_t *outFormat, uint32_t *outBytes) {
    StateLock lock;

    if (!s_renderPending) return false;
    if (!s_frame) {
        // Nothing can be painted; dropping the request is better than setting
        // s_rendering and leaving it set forever (which would answer every
        // later BEGIN_IMAGE with ERR_BUSY).
        s_renderPending = false;
        return false;
    }
    if (outFormat) *outFormat = s_renderFormat;
    if (outBytes) *outBytes = s_renderBytes;
    s_renderPending = false;
    // Claim the framebuffer until the caller says the paint is done. Without this
    // a BEGIN_IMAGE arriving during the 15-25 s refresh would rewrite the very
    // bytes being pushed to the panel.
    s_rendering = true;
    return true;
}

void bleRenderFinished() {
    StateLock lock;
    // One line, once, when an app managed to have its data dropped during the
    // paint: without it the only trace is a BEGIN_IMAGE that is sometimes
    // answered ERR_BUSY and sometimes ERR_COOLDOWN, which no phone-side bug can
    // be diagnosed from. s_dataHint itself is deliberately NOT cleared here -
    // the next BEGIN_IMAGE clears it, and until then it is what tells an app
    // whose END_IMAGE arrives after the paint why nothing was received.
    if (s_dataHint == BLE_HINT_DATA_AFTER_END) {
        BLE_LOG("[BLE] DATA arrived while the panel was being painted and was dropped\n");
    }
    s_rendering = false;
}

bool bleBeginHintPaint() {
    StateLock lock;

    if (!s_frame) return false;
    if (s_receiving || s_renderPending || s_rendering) return false;
    s_rendering = true;
    return true;
}

void blePollTransferTimeout() {
    bool timedOut = false;
    uint32_t silenceMs = 0;
    uint32_t received = 0;
    uint32_t total = 0;

    {
        StateLock lock;

        if (!s_receiving) return;
        // BEGIN -> first DATA may legitimately take a while (quantisation,
        // compression); every later gap is a stall.
        const uint32_t limit = s_firstByteSeen ? BLE_RX_TIMEOUT_MS : BLE_BEGIN_TIMEOUT_MS;
        silenceMs = (uint32_t)(millis() - s_lastRxMs);
        if (silenceMs < limit) return;

        // A phone that kept the link up but stopped sending (killed app, a write
        // that never retried) would otherwise leave the session stuck in
        // RECEIVING/FIXING and answer every future BEGIN_IMAGE with ERR_STATE.
        // The buffer already holds whatever arrived; a new BEGIN overwrites it.
        received = s_received;
        total = s_total;
        resetTransfer();
        timedOut = true;
    }

    if (!timedOut) return;

    BLE_LOG("[BLE] transfer stalled for %lu ms (%lu/%lu bytes), resetting session\n",
            (unsigned long)silenceMs, (unsigned long)received, (unsigned long)total);
    // Sent outside the state lock: notify() takes the NimBLE host lock, and
    // keeping the two nested would invert the lock order for no benefit.
    sendStateEvent(BLE_STATE_CONNECTED, BLE_OK);
}

bool bleTakeHintRequest() {
    if (!s_hintRequested) return false;
    s_hintRequested = false;
    return true;
}

uint8_t bleOptions() { return s_options; }

void bleNotifyState(uint8_t state, uint8_t error) { sendStateEvent(state, error); }
void bleNotifyRendering() { sendStateEvent(BLE_STATE_RENDERING, BLE_OK); }
void bleNotifyDisplayed(uint8_t error) {
    sendStateEvent(error == BLE_OK ? BLE_STATE_DISPLAYED : BLE_STATE_ERROR, error);
}

void bleNotifyLog(const char *text) {
    if (!text) return;
    sendEvent(BLE_EVT_LOG, (const uint8_t *)text, strlen(text));
}
