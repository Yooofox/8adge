package com.file.eightbadge

import com.file.eightbadge.image.ImageProcessor
import org.junit.Assert.assertEquals
import org.junit.Test

class ImageProcessorUnitTest {

    @Test
    fun testPack2bppAllWhiteAndBlackLandscape() {
        val w = 768
        val h = 552

        // All white 2bpp: palette indices all = 1 (code 1 for White)
        // Each byte has 4 pixels: (1<<6) | (1<<4) | (1<<2) | 1 = 0x55
        val whitePalette = IntArray(w * h) { 1 }
        val packedWhite = ImageProcessor.pack2bpp(whitePalette, w, h)

        assertEquals(105984, packedWhite.size)
        assertEquals(0x55.toByte(), packedWhite[0])
        assertEquals(0xC6FD3AF0L, ImageProcessor.calculateCrc32(packedWhite))

        // All black 2bpp: palette indices all = 0 (code 0 for Black)
        // Each byte = 0x00
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

        // All white 1bpp: bit = 1 for white -> 0xFF per byte
        val whitePalette = IntArray(w * h) { 1 }
        val packedWhite = ImageProcessor.pack1bpp(whitePalette, w, h)

        assertEquals(52992, packedWhite.size)
        assertEquals(0xFF.toByte(), packedWhite[0])
        assertEquals(0xFC2E8495L, ImageProcessor.calculateCrc32(packedWhite))

        // All black 1bpp: bit = 0 for black -> 0x00 per byte
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
        // Test non-multiple-of-4 width (e.g. 551, 553, 555) for 2bpp
        val w2 = 551
        val h2 = 100
        val palette2 = IntArray(w2 * h2) { 1 }
        val packed2 = ImageProcessor.pack2bpp(palette2, w2, h2)
        val expectedBytesPerRow2 = (w2 + 3) / 4 // (551 + 3) / 4 = 138
        assertEquals(expectedBytesPerRow2 * h2, packed2.size)

        // Test non-multiple-of-8 width (e.g. 553, 555, 557) for 1bpp
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
        val palette = IntArray(w * h) { 3 } // All Red (code 3)
        val packed = ImageProcessor.pack2bpp(palette, w, h)
        assertEquals(105984, packed.size)
        // 3 in 2-bit binary is 11. 4 pixels = 0xFF
        assertEquals(0xFF.toByte(), packed[0])
    }
}
