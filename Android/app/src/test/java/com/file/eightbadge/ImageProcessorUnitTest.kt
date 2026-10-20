package com.file.eightbadge

import com.file.eightbadge.image.ImageProcessor
import org.junit.Assert.assertEquals
import org.junit.Test

class ImageProcessorUnitTest {

    @Test
    fun testPack2bppAllWhiteAndBlackLandscape() {
        val w = 768
        val h = 552

        // 2bpp 全白：调色板索引全为 1（白色对应编码 1）
        // 每个字节含 4 个像素：(1<<6) | (1<<4) | (1<<2) | 1 = 0x55
        val whitePalette = IntArray(w * h) { 1 }
        val packedWhite = ImageProcessor.pack2bpp(whitePalette, w, h)

        assertEquals(105984, packedWhite.size)
        assertEquals(0x55.toByte(), packedWhite[0])
        assertEquals(0xC6FD3AF0L, ImageProcessor.calculateCrc32(packedWhite))

        // 2bpp 全黑：调色板索引全为 0（黑色对应编码 0）
        // 每个字节 = 0x00
        val blackPalette = IntArray(w * h) { 0 }
        val packedBlack = ImageProcessor.pack2bpp(blackPalette, w, h)

        assertEquals(105984, packedBlack.size)
        assertEquals(0x00.toByte(), packedBlack[0])
        assertEquals(0x2D69E75CL, ImageProcessor.calculateCrc32(packedBlack))
    }

    @Test
    fun testPack1bppAllWhiteAndBlackLandscape() {
        val w = 768
        val h = 552

        // 1bpp 全白：白色的位 = 1 -> 每个字节 0xFF
        val whitePalette = IntArray(w * h) { 1 }
        val packedWhite = ImageProcessor.pack1bpp(whitePalette, w, h)

        assertEquals(52992, packedWhite.size)
        assertEquals(0xFF.toByte(), packedWhite[0])
        assertEquals(0xFC2E8495L, ImageProcessor.calculateCrc32(packedWhite))

        // 1bpp 全黑：黑色的位 = 0 -> 每个字节 0x00
        val blackPalette = IntArray(w * h) { 0 }
        val packedBlack = ImageProcessor.pack1bpp(blackPalette, w, h)

        assertEquals(52992, packedBlack.size)
        assertEquals(0x00.toByte(), packedBlack[0])
        assertEquals(0x2F809144L, ImageProcessor.calculateCrc32(packedBlack))
    }

    @Test
    fun testPack2bppPortraitDimensions() {
        val w = 552
        val h = 768

        val whitePalette = IntArray(w * h) { 1 }
        val packedWhite = ImageProcessor.pack2bpp(whitePalette, w, h)

        assertEquals(105984, packedWhite.size)
        assertEquals(0x55.toByte(), packedWhite[0])
        assertEquals(0xC6FD3AF0L, ImageProcessor.calculateCrc32(packedWhite))
    }

    @Test
    fun testNonAlignedDimensions() {
        // 测试 2bpp 下宽度不是 4 的倍数的情况（例如 551、553、555）
        val w2 = 551
        val h2 = 100
        val palette2 = IntArray(w2 * h2) { 1 }
        val packed2 = ImageProcessor.pack2bpp(palette2, w2, h2)
        val expectedBytesPerRow2 = (w2 + 3) / 4 // (551 + 3) / 4 = 138
        assertEquals(expectedBytesPerRow2 * h2, packed2.size)

        // 测试 1bpp 下宽度不是 8 的倍数的情况（例如 553、555、557）
        val w1 = 555
        val h1 = 100
        val palette1 = IntArray(w1 * h1) { 1 }
        val packed1 = ImageProcessor.pack1bpp(palette1, w1, h1)
        val expectedBytesPerRow1 = (w1 + 7) / 8 // (555 + 7) / 8 = 71
        assertEquals(expectedBytesPerRow1 * h1, packed1.size)
    }

    @Test
    fun testPack2bppBufferSizes() {
        val w = 552
        val h = 768
        val palette = IntArray(w * h) { 3 } // 全红（编码 3）
        val packed = ImageProcessor.pack2bpp(palette, w, h)
        assertEquals(105984, packed.size)
        // 3 的 2 位二进制为 11。4 个像素 = 0xFF
        assertEquals(0xFF.toByte(), packed[0])
    }
}
