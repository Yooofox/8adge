package com.file.eightbadge

import com.file.eightbadge.ble.BleProtocol
import com.file.eightbadge.model.ProtocolError
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class BleProtocolUnitTest {

    @Test
    fun testBuildFrame() {
        val frame = BleProtocol.buildFrame(BleProtocol.OP_GET_INFO, 0x2A.toByte())
        assertEquals(4, frame.size)
        assertEquals(0x01.toByte(), frame[0])
        assertEquals(0x2A.toByte(), frame[1])
        assertEquals(0x00.toByte(), frame[2])
        assertEquals(0x00.toByte(), frame[3])
    }

    @Test
    fun testSequenceIncrementWrap() {
        var nextSeq: Byte = 1
        fun getNextSeq(): Byte {
            val seq = nextSeq
            val currentInt = nextSeq.toInt() and 0xFF
            nextSeq = if (currentInt >= 255) 1 else (currentInt + 1).toByte()
            return seq
        }

        val seqs = mutableListOf<Int>()
        repeat(300) {
            seqs.add(getNextSeq().toInt() and 0xFF)
        }

        assertEquals(1, seqs[0])
        assertEquals(2, seqs[1])
        assertEquals(255, seqs[254])
        assertEquals(1, seqs[255])
        assertEquals(2, seqs[256])
        assertEquals(45, seqs[299])
    }

    @Test
    fun testBuildSyncTimeAndConfirmRefreshPayload() {
        val payload = BleProtocol.buildSyncTimePayload(1760900000L)
        assertEquals(8, payload.size)

        val confirmPayload = BleProtocol.buildConfirmRefreshPayload(0x07.toByte())
        assertEquals(1, confirmPayload.size)
        assertEquals(0x07.toByte(), confirmPayload[0])
    }

    @Test
    fun testBuildBlockCrcsAndRetryPayload() {
        val crcs = longArrayOf(0xC6FD3AF0L, 0x2D69E75CL)
        val payload = BleProtocol.buildBlockCrcsPayload(4096, 2, crcs)
        assertEquals(4 + 8, payload.size)

        val retryPayload = BleProtocol.buildRetryBlockPayload(3)
        assertEquals(2, retryPayload.size)
        assertEquals(3.toByte(), retryPayload[0])
        assertEquals(0.toByte(), retryPayload[1])
    }

    @Test
    fun testParseGetInfoResponseV3() {
        val nameBytes = "8adge-8D134".toByteArray()
        val payloadLen = 37 + nameBytes.size + 2

        val raw = byteArrayOf(
            0x81.toByte(), 0x2A.toByte(), 0x00.toByte(), payloadLen.toByte(),
            0x03, 0x01, 0x02, 0x00, // 协议版本 proto=3，固件版本 fw=1.2.0
            0x28, 0x02, 0x00, 0x03, // 画布宽 canvasW=552，画布高 canvasH=768
            0x00, 0x03, 0x28, 0x02, // 面板宽 panelW=768，面板高 panelH=552
            0x02, 0x04, 0x00, 0x01, 0x02, 0x03, // bpp=2，颜色数 colors=4，{0,1,2,3}
            0x00, 0x9E.toByte(), 0x01, 0x00,    // 2bpp 数据长度 len2bpp = 105984
            0x00, 0xCF.toByte(), 0x00, 0x00,    // 1bpp 数据长度 len1bpp = 52992
            0x05, 0x02, // MTU 大小 mtu=517
            0x00, 0x02, // 分片大小 chunkSize=512
            0x20, 0x00, // 窗口分片数 windowChunks=32
            0x5A,       // 旋转 rotation=90
            0x0B,       // 块位移最小值 blockShiftMin=11
            0x0E,       // 块位移最大值 blockShiftMax=14
            0x80.toByte(), // 最大块数 maxBlocks=128
            nameBytes.size.toByte()
        ) + nameBytes + byteArrayOf(0xB4.toByte(), 0x00) // 刷屏最小间隔秒数 refresh_min_gap_s = 180

        val parsed = BleProtocol.parseControlFrame(raw)
        assertTrue(parsed is BleProtocol.ControlFrame.GetInfoResponse)
        val resp = parsed as BleProtocol.ControlFrame.GetInfoResponse
        assertEquals(0x2A.toByte(), resp.seq)
        assertEquals(ProtocolError.OK, resp.status)
        assertEquals(3, resp.deviceInfo.protocolVersion)
        assertEquals("1.2.0", resp.deviceInfo.fwVersionString)
        assertEquals(552, resp.deviceInfo.canvasWidth)
        assertEquals(768, resp.deviceInfo.canvasHeight)
        assertEquals(768, resp.deviceInfo.panelWidth)
        assertEquals(552, resp.deviceInfo.panelHeight)
        assertEquals(11, resp.deviceInfo.blockShiftMin)
        assertEquals(14, resp.deviceInfo.blockShiftMax)
        assertEquals(128, resp.deviceInfo.maxBlocks)
        assertEquals("8adge-8D134", resp.deviceInfo.deviceName)
        assertEquals(180, resp.deviceInfo.refreshMinGapS)
    }

    @Test
    fun testParseRefreshBlockedEvent() {
        val raw = byteArrayOf(
            0xC3.toByte(), 0x00.toByte(), 0x00.toByte(), 0x0C.toByte(), // EVT_REFRESH_BLOCKED，长度 len=12
            0x07,                            // token = 7，令牌
            0x01,                            // reason = 1 (有时钟)
            0x02,                            // for_opcode = 2，对应 BEGIN_IMAGE
            0x00,                            // reserved，保留
            0x85.toByte(), 0x00,             // wait_seconds = 133，还需等待秒数
            0xB4.toByte(), 0x00,             // min_gap_seconds = 180，最小间隔秒数
            0x2F, 0x00, 0x00, 0x00           // elapsed_seconds = 47，距上次刷屏秒数
        )

        val parsed = BleProtocol.parseControlFrame(raw)
        assertTrue(parsed is BleProtocol.ControlFrame.RefreshBlockedEventFrame)
        val evt = (parsed as BleProtocol.ControlFrame.RefreshBlockedEventFrame).blockedEvent
        assertEquals(7.toByte(), evt.token)
        assertEquals(1, evt.reason)
        assertEquals(0x02.toByte(), evt.forOpcode)
        assertEquals(133, evt.waitSeconds)
        assertEquals(180, evt.minGapSeconds)
        assertEquals(47L, evt.elapsedSeconds)
    }

    @Test
    fun testProtocol1bppAnd2bppCrc32Vectors() {
        // 1bpp 全白 52,992 字节的 0xFF -> 0xFC2E8495
        val allWhite1bpp = ByteArray(52992) { 0xFF.toByte() }
        val crcWhite1bpp = com.file.eightbadge.image.ImageProcessor.calculateCrc32(allWhite1bpp)
        assertEquals(0xFC2E8495L, crcWhite1bpp)

        // 1bpp 全黑 52,992 字节的 0x00 -> 0x2F809144
        val allBlack1bpp = ByteArray(52992) { 0x00.toByte() }
        val crcBlack1bpp = com.file.eightbadge.image.ImageProcessor.calculateCrc32(allBlack1bpp)
        assertEquals(0x2F809144L, crcBlack1bpp)

        // 2bpp 全白 105,984 字节的 0x55 -> 0xC6FD3AF0
        val allWhite2bpp = ByteArray(105984) { 0x55.toByte() }
        val crcWhite2bpp = com.file.eightbadge.image.ImageProcessor.calculateCrc32(allWhite2bpp)
        assertEquals(0xC6FD3AF0L, crcWhite2bpp)

        // 2bpp 全黑 105,984 字节的 0x00 -> 0x2D69E75C
        val allBlack2bpp = ByteArray(105984) { 0x00.toByte() }
        val crcBlack2bpp = com.file.eightbadge.image.ImageProcessor.calculateCrc32(allBlack2bpp)
        assertEquals(0x2D69E75CL, crcBlack2bpp)
    }

    @Test
    fun testParseErrBlocksEndImageResponse() {
        val raw = byteArrayOf(
            0x83.toByte(), 0x2C.toByte(), 0x0C.toByte(), 0x0E.toByte(), // opcode=83，seq=2C，status=0C，长度 len=14
            0x5C, 0xE7.toByte(), 0x69, 0x2D, // actualCrc，实际 CRC = 0x2D69E75C
            0x00, 0x9E.toByte(), 0x01, 0x00, // received，已接收字节数 = 105984
            0x1A,                            // blockCount，块数 = 26
            0x02,                            // badCount，错误块数 = 2
            0x0A.toByte(), 0x00, 0x00, 0x00  // bitmap，错误块位图
        )

        val parsed = BleProtocol.parseControlFrame(raw)
        assertTrue(parsed is BleProtocol.ControlFrame.EndImageResponse)
        val resp = parsed as BleProtocol.ControlFrame.EndImageResponse
        assertEquals(ProtocolError.ERR_BLOCKS, resp.status)
        assertEquals(26, resp.blockCount)
        assertEquals(2, resp.blockBadCount)
        assertEquals(listOf(1, 3), resp.badBlockIndices)
    }
}
