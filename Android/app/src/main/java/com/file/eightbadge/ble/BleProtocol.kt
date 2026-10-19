package com.file.eightbadge.ble

import com.file.eightbadge.model.DeviceInfo
import com.file.eightbadge.model.DeviceState
import com.file.eightbadge.model.ImageFormat
import com.file.eightbadge.model.ProtocolError
import com.file.eightbadge.model.RefreshBlockedEvent
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.UUID

object BleProtocol {
    // UUIDs (Updated for Protocol v3)
    val SERVICE_UUID: UUID = UUID.fromString("42414447-0001-4e5a-9c1a-6b7c8d9e0f11")
    val CONTROL_CHAR_UUID: UUID = UUID.fromString("42414447-0002-4e5a-9c1a-6b7c8d9e0f11")
    val DATA_CHAR_UUID: UUID = UUID.fromString("42414447-0003-4e5a-9c1a-6b7c8d9e0f11")
    val CCCD_DESCRIPTOR_UUID: UUID = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")

    const val DEVICE_NAME_PREFIX = "8adge-"
    const val PROTOCOL_VERSION = 3

    // Opcodes (Request)
    const val OP_GET_INFO: Byte = 0x01
    const val OP_BEGIN_IMAGE: Byte = 0x02
    const val OP_END_IMAGE: Byte = 0x03
    const val OP_ABORT: Byte = 0x04
    const val OP_SHOW_HINT: Byte = 0x05
    const val OP_PING: Byte = 0x06
    const val OP_SET_OPTIONS: Byte = 0x07
    const val OP_BLOCK_CRCS: Byte = 0x08     // v2
    const val OP_RETRY_BLOCK: Byte = 0x09    // v2
    const val OP_SYNC_TIME: Byte = 0x0A      // v3 UTC time sync
    const val OP_CONFIRM_REFRESH: Byte = 0x0B // v3 refresh confirmation

    // Opcodes (Response)
    const val OP_RESP_GET_INFO: Byte = 0x81.toByte()
    const val OP_RESP_BEGIN_IMAGE: Byte = 0x82.toByte()
    const val OP_RESP_END_IMAGE: Byte = 0x83.toByte()
    const val OP_RESP_ABORT: Byte = 0x84.toByte()
    const val OP_RESP_SHOW_HINT: Byte = 0x85.toByte()
    const val OP_RESP_PING: Byte = 0x86.toByte()
    const val OP_RESP_SET_OPTIONS: Byte = 0x87.toByte()
    const val OP_RESP_BLOCK_CRCS: Byte = 0x88.toByte()
    const val OP_RESP_RETRY_BLOCK: Byte = 0x89.toByte()
    const val OP_RESP_SYNC_TIME: Byte = 0x8A.toByte()
    const val OP_RESP_CONFIRM_REFRESH: Byte = 0x8B.toByte()

    // Opcodes (Events)
    const val EVT_STATE: Byte = 0xC0.toByte()
    const val EVT_PROGRESS: Byte = 0xC1.toByte()
    const val EVT_LOG: Byte = 0xC2.toByte()
    const val EVT_REFRESH_BLOCKED: Byte = 0xC3.toByte() // v3 cooldown notification
    const val RESP_GENERIC_ERROR: Byte = 0xE0.toByte()

    // Progress Flags
    const val PROGRESS_FLAG_BLOCK_DONE: Byte = 0x01

    // Options
    const val OPT_DISABLE_HINT: Byte = 0x01

    /**
     * Builds a 4-byte CONTROL request frame header + optional payload.
     */
    fun buildFrame(opcode: Byte, seq: Byte, payload: ByteArray = ByteArray(0)): ByteArray {
        val frame = ByteArray(4 + payload.size)
        frame[0] = opcode
        frame[1] = seq
        frame[2] = 0x00 // status = 0 in request
        frame[3] = payload.size.toByte()
        if (payload.isNotEmpty()) {
            System.arraycopy(payload, 0, frame, 4, payload.size)
        }
        return frame
    }

    /**
     * Builds BEGIN_IMAGE payload (12 bytes)
     * Format (1 byte), Flags (1 byte = 0), blockShift (1 byte), reserved (1 byte = 0), totalLen (4 bytes LE), crc32 (4 bytes LE)
     */
    fun buildBeginImagePayload(format: ImageFormat, blockShift: Byte, totalLen: Int, crc32: Long): ByteArray {
        val buffer = ByteBuffer.allocate(12).order(ByteOrder.LITTLE_ENDIAN)
        buffer.put(format.code)
        buffer.put(0x00.toByte()) // flags
        buffer.put(blockShift)    // blockShift (e.g. 12 = 4096B block size)
        buffer.put(0x00.toByte()) // reserved
        buffer.putInt(totalLen)
        buffer.putInt(crc32.toInt())
        return buffer.array()
    }

    /**
     * Builds BLOCK_CRCS payload (4 + 4*N bytes)
     */
    fun buildBlockCrcsPayload(blockSize: Int, count: Int, blockCrcs: LongArray): ByteArray {
        val buffer = ByteBuffer.allocate(4 + 4 * count).order(ByteOrder.LITTLE_ENDIAN)
        buffer.putShort(blockSize.toShort())
        buffer.putShort(count.toShort())
        for (i in 0 until count) {
            buffer.putInt(blockCrcs[i].toInt())
        }
        return buffer.array()
    }

    /**
     * Builds RETRY_BLOCK payload (2 bytes: blockIndex u16 LE)
     */
    fun buildRetryBlockPayload(blockIndex: Int): ByteArray {
        val buffer = ByteBuffer.allocate(2).order(ByteOrder.LITTLE_ENDIAN)
        buffer.putShort(blockIndex.toShort())
        return buffer.array()
    }

    /**
     * Builds SYNC_TIME payload (8 bytes: unixUtcSeconds u64 LE)
     */
    fun buildSyncTimePayload(unixUtcSeconds: Long): ByteArray {
        val buffer = ByteBuffer.allocate(8).order(ByteOrder.LITTLE_ENDIAN)
        buffer.putLong(unixUtcSeconds)
        return buffer.array()
    }

    /**
     * Builds CONFIRM_REFRESH payload (1 byte: token)
     */
    fun buildConfirmRefreshPayload(token: Byte): ByteArray {
        return byteArrayOf(token)
    }

    /**
     * Builds END_IMAGE payload (4 bytes CRC32 LE)
     */
    fun buildEndImagePayload(crc32: Long): ByteArray {
        val buffer = ByteBuffer.allocate(4).order(ByteOrder.LITTLE_ENDIAN)
        buffer.putInt(crc32.toInt())
        return buffer.array()
    }

    /**
     * Builds SET_OPTIONS payload (1 byte)
     */
    fun buildSetOptionsPayload(disableHint: Boolean): ByteArray {
        val options = if (disableHint) OPT_DISABLE_HINT else 0x00.toByte()
        return byteArrayOf(options)
    }

    /**
     * Parsed Control Message
     */
    sealed class ControlFrame(val opcode: Byte, val seq: Byte, val status: ProtocolError) {
        class GetInfoResponse(
            seq: Byte,
            status: ProtocolError,
            val deviceInfo: DeviceInfo
        ) : ControlFrame(OP_RESP_GET_INFO, seq, status)

        class BeginImageResponse(
            seq: Byte,
            status: ProtocolError,
            val chunkSize: Int,
            val windowChunks: Int,
            val expectedLen: Int,
            val session: Int,
            val format: Byte,
            val blockShift: Int,
            val blockCount: Int
        ) : ControlFrame(OP_RESP_BEGIN_IMAGE, seq, status)

        class BlockCrcsResponse(
            seq: Byte,
            status: ProtocolError,
            val count: Int
        ) : ControlFrame(OP_RESP_BLOCK_CRCS, seq, status)

        class RetryBlockResponse(
            seq: Byte,
            status: ProtocolError,
            val blockIndex: Int,
            val fixNeeded: Int
        ) : ControlFrame(OP_RESP_RETRY_BLOCK, seq, status)

        class SyncTimeResponse(
            seq: Byte,
            status: ProtocolError,
            val lastRefreshUtc: Long,
            val minGapS: Int,
            val clockKnown: Boolean
        ) : ControlFrame(OP_RESP_SYNC_TIME, seq, status)

        class ConfirmRefreshResponse(
            seq: Byte,
            status: ProtocolError,
            val token: Byte
        ) : ControlFrame(OP_RESP_CONFIRM_REFRESH, seq, status)

        class EndImageResponse(
            seq: Byte,
            status: ProtocolError,
            val actualCrc: Long,
            val receivedBytes: Int,
            val blockCount: Int,
            val blockBadCount: Int,
            val badBlockIndices: List<Int>
        ) : ControlFrame(OP_RESP_END_IMAGE, seq, status)

        class AbortResponse(seq: Byte, status: ProtocolError) : ControlFrame(OP_RESP_ABORT, seq, status)
        class ShowHintResponse(seq: Byte, status: ProtocolError) : ControlFrame(OP_RESP_SHOW_HINT, seq, status)
        class PingResponse(seq: Byte, status: ProtocolError, val echoedData: ByteArray) : ControlFrame(OP_RESP_PING, seq, status)
        class SetOptionsResponse(seq: Byte, status: ProtocolError, val options: Byte) : ControlFrame(OP_RESP_SET_OPTIONS, seq, status)

        // Events
        class StateEvent(val state: DeviceState, val error: ProtocolError) :
            ControlFrame(EVT_STATE, 0, ProtocolError.OK)

        class ProgressEvent(val received: Int, val total: Int, val session: Int, val blockDone: Boolean) :
            ControlFrame(EVT_PROGRESS, 0, ProtocolError.OK)

        class LogEvent(val logText: String) : ControlFrame(EVT_LOG, 0, ProtocolError.OK)

        class RefreshBlockedEventFrame(val blockedEvent: RefreshBlockedEvent) :
            ControlFrame(EVT_REFRESH_BLOCKED, 0, ProtocolError.OK)

        class GenericErrorResponse(val offendingOpcode: Byte) :
            ControlFrame(RESP_GENERIC_ERROR, 0, ProtocolError.ERR_OPCODE)

        class UnknownFrame(opcode: Byte, seq: Byte, status: ProtocolError, val rawData: ByteArray) :
            ControlFrame(opcode, seq, status)
    }

    /**
     * Parses incoming notify frame from CONTROL characteristic.
     */
    fun parseControlFrame(data: ByteArray): ControlFrame? {
        if (data.size < 4) return null

        val opcode = data[0]
        val seq = data[1]
        val statusCode = data[2].toInt() and 0xFF
        val status = ProtocolError.fromCode(statusCode)
        val payloadLen = data[3].toInt() and 0xFF

        if (data.size < 4 + payloadLen) {
            return null
        }

        val payload = ByteArray(payloadLen)
        if (payloadLen > 0) {
            System.arraycopy(data, 4, payload, 0, payloadLen)
        }

        val buffer = ByteBuffer.wrap(payload).order(ByteOrder.LITTLE_ENDIAN)

        return when (opcode) {
            OP_RESP_GET_INFO -> {
                if (status != ProtocolError.OK || payloadLen < 32) {
                    ControlFrame.GetInfoResponse(seq, status, DeviceInfo())
                } else {
                    val protoVer = buffer.get().toInt() and 0xFF
                    val fwMajor = buffer.get().toInt() and 0xFF
                    val fwMinor = buffer.get().toInt() and 0xFF
                    val fwPatch = buffer.get().toInt() and 0xFF

                    val canvasW = buffer.short.toInt() and 0xFFFF
                    val canvasH = buffer.short.toInt() and 0xFFFF
                    val panelW = buffer.short.toInt() and 0xFFFF
                    val panelH = buffer.short.toInt() and 0xFFFF

                    val bpp = buffer.get().toInt() and 0xFF
                    val colors = buffer.get().toInt() and 0xFF
                    val colorCodes = ByteArray(4)
                    buffer.get(colorCodes)

                    val len2bpp = buffer.int
                    val len1bpp = buffer.int
                    val mtu = buffer.short.toInt() and 0xFFFF
                    val chunkSize = buffer.short.toInt() and 0xFFFF
                    val windowChunks = buffer.short.toInt() and 0xFFFF

                    val rotation = if (buffer.remaining() >= 1) buffer.get().toInt() and 0xFF else 90
                    val blockMin = if (buffer.remaining() >= 1) buffer.get().toInt() and 0xFF else 11
                    val blockMax = if (buffer.remaining() >= 1) buffer.get().toInt() and 0xFF else 14
                    val maxBlocks = if (buffer.remaining() >= 1) buffer.get().toInt() and 0xFF else 128

                    val nameLen = if (buffer.remaining() >= 1) buffer.get().toInt() and 0xFF else 0
                    val nameBytes = ByteArray(nameLen.coerceAtMost(buffer.remaining()))
                    if (nameBytes.isNotEmpty()) {
                        buffer.get(nameBytes)
                    }
                    val name = String(nameBytes, Charsets.UTF_8)

                    val refreshMinGap = if (buffer.remaining() >= 2) buffer.short.toInt() and 0xFFFF else 180

                    val info = DeviceInfo(
                        protocolVersion = protoVer,
                        fwMajor = fwMajor,
                        fwMinor = fwMinor,
                        fwPatch = fwPatch,
                        canvasWidth = canvasW,
                        canvasHeight = canvasH,
                        panelWidth = panelW,
                        panelHeight = panelH,
                        bpp = bpp,
                        colorCount = colors,
                        len2bpp = len2bpp,
                        len1bpp = len1bpp,
                        mtu = mtu,
                        chunkSize = chunkSize,
                        windowChunks = windowChunks,
                        rotation = rotation,
                        blockShiftMin = blockMin,
                        blockShiftMax = blockMax,
                        maxBlocks = maxBlocks,
                        deviceName = name,
                        refreshMinGapS = refreshMinGap
                    )
                    ControlFrame.GetInfoResponse(seq, status, info)
                }
            }

            OP_RESP_BEGIN_IMAGE -> {
                if (status != ProtocolError.OK || payloadLen < 10) {
                    ControlFrame.BeginImageResponse(seq, status, 0, 0, 0, 0, 0, 0, 0)
                } else {
                    val chunkSize = buffer.short.toInt() and 0xFFFF
                    val windowChunks = buffer.short.toInt() and 0xFFFF
                    val expectedLen = buffer.int
                    val session = buffer.get().toInt() and 0xFF
                    val fmt = buffer.get()

                    val blockShift = if (buffer.remaining() >= 1) buffer.get().toInt() and 0xFF else 0
                    val blockCount = if (buffer.remaining() >= 1) buffer.get().toInt() and 0xFF else 0

                    ControlFrame.BeginImageResponse(
                        seq, status, chunkSize, windowChunks, expectedLen, session, fmt, blockShift, blockCount
                    )
                }
            }

            OP_RESP_BLOCK_CRCS -> {
                val count = if (payloadLen >= 1) buffer.get().toInt() and 0xFF else 0
                ControlFrame.BlockCrcsResponse(seq, status, count)
            }

            OP_RESP_RETRY_BLOCK -> {
                val idx = if (payloadLen >= 2) buffer.short.toInt() and 0xFFFF else 0
                val fixNeeded = if (payloadLen >= 4) buffer.short.toInt() and 0xFFFF else 0
                ControlFrame.RetryBlockResponse(seq, status, idx, fixNeeded)
            }

            OP_RESP_SYNC_TIME -> {
                if (payloadLen >= 11) {
                    val lastRefreshUtc = buffer.long
                    val minGap = buffer.short.toInt() and 0xFFFF
                    val clockKnown = (buffer.get().toInt() and 0xFF) == 1
                    ControlFrame.SyncTimeResponse(seq, status, lastRefreshUtc, minGap, clockKnown)
                } else if (payloadLen >= 7) {
                    val lastRefreshUtc = buffer.int.toLong() and 0xFFFFFFFFL
                    val minGap = buffer.short.toInt() and 0xFFFF
                    val clockKnown = (buffer.get().toInt() and 0xFF) == 1
                    ControlFrame.SyncTimeResponse(seq, status, lastRefreshUtc, minGap, clockKnown)
                } else {
                    ControlFrame.SyncTimeResponse(seq, status, 0L, 180, false)
                }
            }

            OP_RESP_CONFIRM_REFRESH -> {
                val token = if (payloadLen >= 1) buffer.get() else 0.toByte()
                ControlFrame.ConfirmRefreshResponse(seq, status, token)
            }

            OP_RESP_END_IMAGE -> {
                if (status == ProtocolError.OK) {
                    val received = if (payloadLen >= 4) buffer.int else 0
                    val crc = if (payloadLen >= 8) buffer.int.toLong() and 0xFFFFFFFFL else 0L
                    ControlFrame.EndImageResponse(seq, status, crc, received, 0, 0, emptyList())
                } else if (status == ProtocolError.ERR_BLOCKS) {
                    val actualCrc = if (payloadLen >= 4) buffer.int.toLong() and 0xFFFFFFFFL else 0L
                    val received = if (payloadLen >= 8) buffer.int else 0
                    val count = if (payloadLen >= 9) buffer.get().toInt() and 0xFF else 0
                    val badCount = if (payloadLen >= 10) buffer.get().toInt() and 0xFF else 0

                    val badIndices = mutableListOf<Int>()
                    val bitmapLen = (count + 7) / 8
                    if (buffer.remaining() >= bitmapLen) {
                        val bitmap = ByteArray(bitmapLen)
                        buffer.get(bitmap)
                        for (i in 0 until count) {
                            val byteIdx = i / 8
                            val bitIdx = i % 8
                            if ((bitmap[byteIdx].toInt() and (1 shl bitIdx)) != 0) {
                                badIndices.add(i)
                            }
                        }
                    }
                    ControlFrame.EndImageResponse(seq, status, actualCrc, received, count, badCount, badIndices)
                } else {
                    val actualCrc = if (payloadLen >= 4) buffer.int.toLong() and 0xFFFFFFFFL else 0L
                    val received = if (payloadLen >= 8) buffer.int else 0
                    ControlFrame.EndImageResponse(seq, status, actualCrc, received, 0, 0, emptyList())
                }
            }

            OP_RESP_ABORT -> ControlFrame.AbortResponse(seq, status)
            OP_RESP_SHOW_HINT -> ControlFrame.ShowHintResponse(seq, status)
            OP_RESP_PING -> ControlFrame.PingResponse(seq, status, payload)
            OP_RESP_SET_OPTIONS -> ControlFrame.SetOptionsResponse(seq, status, if (payload.isNotEmpty()) payload[0] else 0)

            EVT_STATE -> {
                if (payloadLen >= 2) {
                    val stateCode = buffer.get().toInt() and 0xFF
                    val errCode = buffer.get().toInt() and 0xFF
                    ControlFrame.StateEvent(
                        DeviceState.fromCode(stateCode),
                        ProtocolError.fromCode(errCode)
                    )
                } else {
                    ControlFrame.StateEvent(DeviceState.ERROR, ProtocolError.ERR_LENGTH)
                }
            }

            EVT_PROGRESS -> {
                if (payloadLen >= 9) {
                    val received = buffer.int
                    val total = buffer.int
                    val session = buffer.get().toInt() and 0xFF
                    val flags = if (buffer.remaining() >= 1) buffer.get() else 0
                    val blockDone = (flags.toInt() and PROGRESS_FLAG_BLOCK_DONE.toInt()) != 0
                    ControlFrame.ProgressEvent(received, total, session, blockDone)
                } else {
                    ControlFrame.ProgressEvent(0, 0, 0, false)
                }
            }

            EVT_LOG -> {
                val text = String(payload, Charsets.UTF_8)
                ControlFrame.LogEvent(text)
            }

            EVT_REFRESH_BLOCKED -> {
                if (payloadLen >= 12) {
                    val token = buffer.get()
                    val reason = buffer.get().toInt() and 0xFF
                    val forOpcode = buffer.get()
                    val reserved = buffer.get()
                    val waitSeconds = buffer.short.toInt() and 0xFFFF
                    val minGapSeconds = buffer.short.toInt() and 0xFFFF
                    val elapsedSeconds = buffer.int.toLong() and 0xFFFFFFFFL

                    val event = RefreshBlockedEvent(
                        token = token,
                        reason = reason,
                        forOpcode = forOpcode,
                        waitSeconds = waitSeconds,
                        minGapSeconds = minGapSeconds,
                        elapsedSeconds = elapsedSeconds
                    )
                    ControlFrame.RefreshBlockedEventFrame(event)
                } else {
                    ControlFrame.RefreshBlockedEventFrame(
                        RefreshBlockedEvent(0, 0, 0, 0, 180, 0)
                    )
                }
            }

            RESP_GENERIC_ERROR -> {
                val offending = if (payload.isNotEmpty()) payload[0] else 0
                ControlFrame.GenericErrorResponse(offending)
            }

            else -> ControlFrame.UnknownFrame(opcode, seq, status, payload)
        }
    }
}
