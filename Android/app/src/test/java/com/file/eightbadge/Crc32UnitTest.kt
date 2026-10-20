package com.file.eightbadge

import org.junit.Assert.assertEquals
import org.junit.Test
import java.util.zip.CRC32

class Crc32UnitTest {

    private fun calculateCrc(bytes: ByteArray): Long {
        val crc = CRC32()
        crc.update(bytes)
        return crc.value and 0xFFFFFFFFL
    }

    @Test
    fun testCrcVectorsFromProtocolDoc() {
        // 测试向量 1："" -> 0x00000000
        assertEquals(0x00000000L, calculateCrc("".toByteArray()))

        // 测试向量 2："123456789" -> 0xCBF43926
        assertEquals(0xCBF43926L, calculateCrc("123456789".toByteArray(Charsets.US_ASCII)))

        // 测试向量 3：105984 字节的 0x55（2bpp 全白）-> 0xC6FD3AF0
        val allWhite2bpp = ByteArray(105984) { 0x55.toByte() }
        assertEquals(0xC6FD3AF0L, calculateCrc(allWhite2bpp))

        // 测试向量 4：105984 字节的 0x00（2bpp 全黑）-> 0x2D69E75C
        val allBlack2bpp = ByteArray(105984) { 0x00.toByte() }
        assertEquals(0x2D69E75CL, calculateCrc(allBlack2bpp))

        // 测试向量 5：52992 字节的 0xFF（1bpp 全白）-> 0xFC2E8495
        val allWhite1bpp = ByteArray(52992) { 0xFF.toByte() }
        assertEquals(0xFC2E8495L, calculateCrc(allWhite1bpp))

        // 测试向量 6：52992 字节的 0x00（1bpp 全黑）-> 0x2F809144
        val allBlack1bpp = ByteArray(52992) { 0x00.toByte() }
        assertEquals(0x2F809144L, calculateCrc(allBlack1bpp))
    }

    @Test
    fun testInvestigate1bppPaddedCrc() {
        val allWhite1bpp = ByteArray(52992) { 0xFF.toByte() }
        val paddedSectorFF = allWhite1bpp + ByteArray(53248 - 52992) { 0xFF.toByte() }
        val paddedSector00 = allWhite1bpp + ByteArray(53248 - 52992) { 0x00.toByte() }
        val padded105k00 = allWhite1bpp + ByteArray(105984 - 52992) { 0x00.toByte() }
        val padded105kFF = allWhite1bpp + ByteArray(105984 - 52992) { 0xFF.toByte() }

        println("1bpp All-White 52992B CRC = 0x%08X".format(calculateCrc(allWhite1bpp)))
        println("1bpp All-White padded 53248B (13x4KB) 0xFF CRC = 0x%08X".format(calculateCrc(paddedSectorFF)))
        println("1bpp All-White padded 53248B (13x4KB) 0x00 CRC = 0x%08X".format(calculateCrc(paddedSector00)))
        println("1bpp All-White padded 105984B 0x00 CRC = 0x%08X".format(calculateCrc(padded105k00)))
        println("1bpp All-White padded 105984B 0xFF CRC = 0x%08X".format(calculateCrc(padded105kFF)))
    }
}
