package com.file.eightbadge.image

import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.ColorMatrix
import android.graphics.ColorMatrixColorFilter
import android.graphics.Matrix
import android.graphics.Paint
import android.graphics.Rect
import android.graphics.RectF
import com.file.eightbadge.model.DitherMode
import com.file.eightbadge.model.ImageFormat
import com.file.eightbadge.model.ScaleMode
import com.file.eightbadge.model.ScreenOrientation
import java.util.Random
import java.util.zip.CRC32

object ImageProcessor {

    private fun makeRgb(r: Int, g: Int, b: Int): Int = (0xFF shl 24) or ((r and 0xFF) shl 16) or ((g and 0xFF) shl 8) or (b and 0xFF)
    private fun getR(c: Int): Int = (c shr 16) and 0xFF
    private fun getG(c: Int): Int = (c shr 8) and 0xFF
    private fun getB(c: Int): Int = c and 0xFF

    // 模拟真实墨水屏观感的墨水屏调色板 RGB 值（0=黑，1=白，2=黄，3=红）
    val COLOR_BLACK = makeRgb(0, 0, 0)         // 色码 0：纯墨水屏黑 #000000
    val COLOR_WHITE = makeRgb(255, 255, 255)   // 色码 1：纸白 #FFFFFF
    val COLOR_YELLOW = makeRgb(230, 184, 0)    // 色码 2：模拟真实墨水屏黄 #E6B800
    val COLOR_RED = makeRgb(200, 40, 40)       // 色码 3：模拟真实墨水屏红 #C82828

    data class ProcessResult(
        val rawPayload: ByteArray,
        val previewBitmap: Bitmap,
        val format: ImageFormat,
        val crc32: Long
    ) {
        override fun equals(other: Any?): Boolean {
            if (this === other) return true
            if (javaClass != other?.javaClass) return false

            other as ProcessResult

            if (!rawPayload.contentEquals(other.rawPayload)) return false
            if (previewBitmap != other.previewBitmap) return false
            if (format != other.format) return false
            if (crc32 != other.crc32) return false

            return true
        }

        override fun hashCode(): Int {
            var result = rawPayload.contentHashCode()
            result = 31 * result + previewBitmap.hashCode()
            result = 31 * result + format.hashCode()
            result = 31 * result + crc32.hashCode()
            return result
        }
    }

    /**
     * 按指定角度（0、90、180、270）旋转位图。
     */
    fun rotateBitmap(src: Bitmap, degrees: Float): Bitmap {
        if (degrees % 360f == 0f) return src
        val matrix = Matrix()
        matrix.postRotate(degrees)
        return Bitmap.createBitmap(src, 0, 0, src.width, src.height, matrix, true)
    }

    /**
     * 按归一化裁剪矩形（坐标范围 0.0..1.0）安全裁剪位图。
     */
    fun cropBitmap(src: Bitmap, normalizedCropRect: RectF): Bitmap {
        if (src.width <= 0 || src.height <= 0) return src
        val srcW = src.width
        val srcH = src.height

        val normL = if (normalizedCropRect.left.isNaN()) 0f else normalizedCropRect.left.coerceIn(0f, 1f)
        val normT = if (normalizedCropRect.top.isNaN()) 0f else normalizedCropRect.top.coerceIn(0f, 1f)
        val normR = if (normalizedCropRect.right.isNaN()) 1f else normalizedCropRect.right.coerceIn(normL, 1f)
        val normB = if (normalizedCropRect.bottom.isNaN()) 1f else normalizedCropRect.bottom.coerceIn(normT, 1f)

        val l = (normL * srcW).toInt().coerceIn(0, srcW - 1)
        val t = (normT * srcH).toInt().coerceIn(0, srcH - 1)
        val r = (normR * srcW).toInt().coerceIn(l + 1, srcW)
        val b = (normB * srcH).toInt().coerceIn(t + 1, srcH)

        val cropW = (r - l).coerceIn(1, srcW - l)
        val cropH = (b - t).coerceIn(1, srcH - t)

        return Bitmap.createBitmap(src, l, t, cropW, cropH)
    }

    /**
     * 主图像处理函数：支持动态分辨率、裁剪、旋转、色彩调整与 14 种抖动算法。
     */
    fun processImage(
        srcBitmap: Bitmap,
        format: ImageFormat,
        orientation: ScreenOrientation = ScreenOrientation.PORTRAIT,
        scaleMode: ScaleMode = ScaleMode.CROP_FILL,
        ditherMode: DitherMode = DitherMode.FLOYD_STEINBERG,
        rotationDegrees: Float = 0f,
        cropRect: RectF? = null,
        brightness: Float = 0f,      // -100 到 +100
        contrast: Float = 1.0f,      // 0.5 到 2.0
        saturation: Float = 1.0f,    // 0.0 到 2.5
        gamma: Float = 1.0f,         // 0.2 到 3.0
        sharpen: Float = 0f,         // 0 到 100
        invertColors: Boolean = false,
        ensureActive: (() -> Unit)? = null
    ): ProcessResult {
        val width = orientation.width
        val height = orientation.height

        ensureActive?.invoke()

        // 步骤 1：旋转
        val rotatedBitmap = rotateBitmap(srcBitmap, rotationDegrees)

        // 步骤 2：若指定了裁剪区域则裁剪
        val croppedBitmap = if (cropRect != null && (cropRect.left > 0f || cropRect.top > 0f || cropRect.right < 1f || cropRect.bottom < 1f)) {
            val cropped = cropBitmap(rotatedBitmap, cropRect)
            if (rotatedBitmap !== srcBitmap && rotatedBitmap !== cropped && !rotatedBitmap.isRecycled) {
                rotatedBitmap.recycle()
            }
            cropped
        } else {
            rotatedBitmap
        }

        try {
            ensureActive?.invoke()

            // 步骤 3：缩放与色彩调整（亮度、对比度、饱和度、Gamma、锐化、反色）
            val adjustedBitmap = scaleAndAdjust(
                croppedBitmap, width, height, scaleMode,
                brightness, contrast, saturation, gamma, sharpen, invertColors,
                ensureActive
            )

            try {
                ensureActive?.invoke()

                // 步骤 4：量化与抖动
                val result = if (format == ImageFormat.BPP2) {
                    process2bpp(adjustedBitmap, width, height, ditherMode, orientation, ensureActive)
                } else {
                    process1bpp(adjustedBitmap, width, height, ditherMode, orientation, ensureActive)
                }

                return result
            } finally {
                if (!adjustedBitmap.isRecycled) {
                    adjustedBitmap.recycle()
                }
            }
        } finally {
            if (croppedBitmap !== srcBitmap && !croppedBitmap.isRecycled) {
                croppedBitmap.recycle()
            }
        }
    }

    private fun scaleAndAdjust(
        src: Bitmap,
        targetW: Int,
        targetH: Int,
        scaleMode: ScaleMode,
        brightness: Float,
        contrast: Float,
        saturation: Float,
        gamma: Float,
        sharpen: Float,
        invertColors: Boolean,
        ensureActive: (() -> Unit)? = null
    ): Bitmap {
        val target = Bitmap.createBitmap(targetW, targetH, Bitmap.Config.ARGB_8888)
        val canvas = Canvas(target)
        canvas.drawColor(Color.WHITE)

        val srcW = src.width
        val srcH = src.height

        val dstRect = Rect()

        when (scaleMode) {
            ScaleMode.STRETCH -> {
                dstRect.set(0, 0, targetW, targetH)
            }
            ScaleMode.CROP_FILL -> {
                val scale = maxOf(targetW.toFloat() / srcW, targetH.toFloat() / srcH)
                val scaledW = (srcW * scale).toInt()
                val scaledH = (srcH * scale).toInt()
                val left = (targetW - scaledW) / 2
                val top = (targetH - scaledH) / 2
                dstRect.set(left, top, left + scaledW, top + scaledH)
            }
            ScaleMode.FIT_PAD -> {
                val scale = minOf(targetW.toFloat() / srcW, targetH.toFloat() / srcH)
                val scaledW = (srcW * scale).toInt()
                val scaledH = (srcH * scale).toInt()
                val left = (targetW - scaledW) / 2
                val top = (targetH - scaledH) / 2
                dstRect.set(left, top, left + scaledW, top + scaledH)
            }
        }

        val paint = Paint(Paint.ANTI_ALIAS_FLAG or Paint.FILTER_BITMAP_FLAG)

        val cm = ColorMatrix()
        cm.setSaturation(saturation)

        if (contrast != 1.0f || brightness != 0f) {
            val scale = contrast
            val translate = (-0.5f * scale + 0.5f) * 255f + brightness
            val contrastMatrix = ColorMatrix(
                floatArrayOf(
                    scale, 0f, 0f, 0f, translate,
                    0f, scale, 0f, 0f, translate,
                    0f, 0f, scale, 0f, translate,
                    0f, 0f, 0f, 1f, 0f
                )
            )
            cm.postConcat(contrastMatrix)
        }

        if (invertColors) {
            val invertMatrix = ColorMatrix(
                floatArrayOf(
                    -1f,  0f,  0f, 0f, 255f,
                     0f, -1f,  0f, 0f, 255f,
                     0f,  0f, -1f, 0f, 255f,
                     0f,  0f,  0f, 1f,   0f
                )
            )
            cm.postConcat(invertMatrix)
        }

        paint.colorFilter = ColorMatrixColorFilter(cm)
        canvas.drawBitmap(src, null, dstRect, paint)

        // 若需要，执行 Gamma 校正与锐化的逐像素处理
        if (gamma != 1.0f || sharpen > 0f) {
            applyGammaAndSharpen(target, gamma, sharpen, ensureActive)
        }

        return target
    }

    private fun applyGammaAndSharpen(bitmap: Bitmap, gamma: Float, sharpen: Float, ensureActive: (() -> Unit)? = null) {
        val w = bitmap.width
        val h = bitmap.height
        val pixels = IntArray(w * h)
        bitmap.getPixels(pixels, 0, w, 0, 0, w, h)

        val gammaTable = IntArray(256) { i ->
            val normalized = i / 255.0
            val corrected = Math.pow(normalized, 1.0 / gamma).coerceIn(0.0, 1.0)
            (corrected * 255.0).toInt()
        }

        // 1. Gamma 校正
        if (gamma != 1.0f) {
            for (i in pixels.indices) {
                val c = pixels[i]
                val r = gammaTable[getR(c)]
                val g = gammaTable[getG(c)]
                val b = gammaTable[getB(c)]
                pixels[i] = makeRgb(r, g, b)
            }
        }

        // 2. Laplacian 锐化
        if (sharpen > 0f) {
            val factor = (sharpen / 100f) * 1.5f
            val tempPixels = pixels.clone()
            for (y in 0 until h) {
                ensureActive?.invoke()
                val yUp = (y - 1).coerceIn(0, h - 1)
                val yDown = (y + 1).coerceIn(0, h - 1)
                for (x in 0 until w) {
                    val idx = y * w + x
                    val xLeft = (x - 1).coerceIn(0, w - 1)
                    val xRight = (x + 1).coerceIn(0, w - 1)

                    val c = tempPixels[idx]
                    val cUp = tempPixels[yUp * w + x]
                    val cDown = tempPixels[yDown * w + x]
                    val cLeft = tempPixels[y * w + xLeft]
                    val cRight = tempPixels[y * w + xRight]

                    val r = (getR(c) + factor * (5 * getR(c) - getR(cUp) - getR(cDown) - getR(cLeft) - getR(cRight))).toInt().coerceIn(0, 255)
                    val g = (getG(c) + factor * (5 * getG(c) - getG(cUp) - getG(cDown) - getG(cLeft) - getG(cRight))).toInt().coerceIn(0, 255)
                    val b = (getB(c) + factor * (5 * getB(c) - getB(cUp) - getB(cDown) - getB(cLeft) - getB(cRight))).toInt().coerceIn(0, 255)

                    pixels[idx] = makeRgb(r, g, b)
                }
            }
        }

        bitmap.setPixels(pixels, 0, w, 0, 0, w, h)
    }

    private fun process2bpp(
        bitmap: Bitmap,
        w: Int,
        h: Int,
        ditherMode: DitherMode,
        orientation: ScreenOrientation,
        ensureActive: (() -> Unit)? = null
    ): ProcessResult {
        val pixels = IntArray(w * h)
        bitmap.getPixels(pixels, 0, w, 0, 0, w, h)

        val paletteRGB = intArrayOf(COLOR_BLACK, COLOR_WHITE, COLOR_YELLOW, COLOR_RED)

        val paletteIndices = IntArray(w * h)
        val previewPixels = IntArray(w * h)

        when (ditherMode) {
            DitherMode.FLOYD_STEINBERG -> floydSteinbergDither2bpp(pixels, w, h, paletteRGB, paletteIndices, previewPixels, ensureActive)
            DitherMode.ATKINSON -> atkinsonDither2bpp(pixels, w, h, paletteRGB, paletteIndices, previewPixels, ensureActive)
            DitherMode.JJN -> jjnDither2bpp(pixels, w, h, paletteRGB, paletteIndices, previewPixels, ensureActive)
            DitherMode.STUCKI -> stuckiDither2bpp(pixels, w, h, paletteRGB, paletteIndices, previewPixels, ensureActive)
            DitherMode.BURKES -> burkesDither2bpp(pixels, w, h, paletteRGB, paletteIndices, previewPixels, ensureActive)
            DitherMode.SIERRA_3 -> sierra3Dither2bpp(pixels, w, h, paletteRGB, paletteIndices, previewPixels, ensureActive)
            DitherMode.SIERRA_2 -> sierra2Dither2bpp(pixels, w, h, paletteRGB, paletteIndices, previewPixels, ensureActive)
            DitherMode.SIERRA_LITE -> sierraLiteDither2bpp(pixels, w, h, paletteRGB, paletteIndices, previewPixels, ensureActive)
            DitherMode.BAYER_2X2 -> bayer2x2Dither2bpp(pixels, w, h, paletteRGB, paletteIndices, previewPixels, ensureActive)
            DitherMode.BAYER_4X4 -> bayer4x4Dither2bpp(pixels, w, h, paletteRGB, paletteIndices, previewPixels, ensureActive)
            DitherMode.BAYER_8X8 -> bayer8x8Dither2bpp(pixels, w, h, paletteRGB, paletteIndices, previewPixels, ensureActive)
            DitherMode.CLUSTER_DOT_4X4 -> clusterDotDither2bpp(pixels, w, h, paletteRGB, paletteIndices, previewPixels, ensureActive)
            DitherMode.RANDOM -> randomNoiseDither2bpp(pixels, w, h, paletteRGB, paletteIndices, previewPixels, ensureActive)
            DitherMode.NEAREST -> nearestColor2bpp(pixels, w, h, paletteRGB, paletteIndices, previewPixels, ensureActive)
        }

        val preview = Bitmap.createBitmap(previewPixels, w, h, Bitmap.Config.ARGB_8888)

        // 确保 BLE 传输负载映射为设备要求的 552 × 768 画布布局
        val panelIndices = rotatePaletteToPhysicalPanel(paletteIndices, w, h, orientation)
        val panelW = if (orientation == ScreenOrientation.PORTRAIT) w else 552
        val panelH = if (orientation == ScreenOrientation.PORTRAIT) h else 768

        val packedData = pack2bpp(panelIndices, panelW, panelH)
        val crc = calculateCrc32(packedData)

        return ProcessResult(packedData, preview, ImageFormat.BPP2, crc)
    }

    private fun process1bpp(
        bitmap: Bitmap,
        w: Int,
        h: Int,
        ditherMode: DitherMode,
        orientation: ScreenOrientation,
        ensureActive: (() -> Unit)? = null
    ): ProcessResult {
        val pixels = IntArray(w * h)
        bitmap.getPixels(pixels, 0, w, 0, 0, w, h)

        val paletteRGB = intArrayOf(COLOR_BLACK, COLOR_WHITE)
        val paletteIndices = IntArray(w * h)
        val previewPixels = IntArray(w * h)

        when (ditherMode) {
            DitherMode.FLOYD_STEINBERG -> floydSteinbergDither1bpp(pixels, w, h, paletteRGB, paletteIndices, previewPixels, ensureActive)
            DitherMode.ATKINSON -> atkinsonDither1bpp(pixels, w, h, paletteRGB, paletteIndices, previewPixels, ensureActive)
            DitherMode.JJN -> jjnDither1bpp(pixels, w, h, paletteRGB, paletteIndices, previewPixels, ensureActive)
            DitherMode.STUCKI -> stuckiDither1bpp(pixels, w, h, paletteRGB, paletteIndices, previewPixels, ensureActive)
            DitherMode.BURKES -> burkesDither1bpp(pixels, w, h, paletteRGB, paletteIndices, previewPixels, ensureActive)
            DitherMode.SIERRA_3 -> sierra3Dither1bpp(pixels, w, h, paletteRGB, paletteIndices, previewPixels, ensureActive)
            DitherMode.SIERRA_2 -> sierra2Dither1bpp(pixels, w, h, paletteRGB, paletteIndices, previewPixels, ensureActive)
            DitherMode.SIERRA_LITE -> sierraLiteDither1bpp(pixels, w, h, paletteRGB, paletteIndices, previewPixels, ensureActive)
            DitherMode.BAYER_2X2 -> bayer2x2Dither1bpp(pixels, w, h, paletteRGB, paletteIndices, previewPixels, ensureActive)
            DitherMode.BAYER_4X4 -> bayer4x4Dither1bpp(pixels, w, h, paletteRGB, paletteIndices, previewPixels, ensureActive)
            DitherMode.BAYER_8X8 -> bayer8x8Dither1bpp(pixels, w, h, paletteRGB, paletteIndices, previewPixels, ensureActive)
            DitherMode.CLUSTER_DOT_4X4 -> clusterDotDither1bpp(pixels, w, h, paletteRGB, paletteIndices, previewPixels, ensureActive)
            DitherMode.RANDOM -> randomNoiseDither1bpp(pixels, w, h, paletteRGB, paletteIndices, previewPixels, ensureActive)
            DitherMode.NEAREST -> nearestColor1bpp(pixels, w, h, paletteRGB, paletteIndices, previewPixels, ensureActive)
        }

        val preview = Bitmap.createBitmap(previewPixels, w, h, Bitmap.Config.ARGB_8888)

        // 确保 BLE 传输负载映射为设备要求的 552 × 768 画布布局
        val panelIndices = rotatePaletteToPhysicalPanel(paletteIndices, w, h, orientation)
        val panelW = if (orientation == ScreenOrientation.PORTRAIT) w else 552
        val panelH = if (orientation == ScreenOrientation.PORTRAIT) h else 768

        val packedData = pack1bpp(panelIndices, panelW, panelH)
        val crc = calculateCrc32(packedData)

        return ProcessResult(packedData, preview, ImageFormat.BPP1, crc)
    }

    /**
     * 将调色板索引数组映射为设备要求的 552 × 768 画布布局。
     * 若方向为 PORTRAIT（552x768），调色板本身已是 552x768，直接返回。
     * 若方向为 LANDSCAPE（768x552），则把 768x552 的调色板索引旋转进 552x768 的画布帧缓冲，
     * 使行宽与设备端画布坐标布局对齐（每行 138 字节 / 共 768 行）。
     */
    fun rotatePaletteToPhysicalPanel(
        paletteIndices: IntArray,
        srcW: Int,
        srcH: Int,
        orientation: ScreenOrientation
    ): IntArray {
        if (orientation == ScreenOrientation.PORTRAIT) {
            return paletteIndices
        }

        // srcW = 768，srcH = 552
        // 目标画布帧缓冲：panelW = 552，panelH = 768
        val panelW = 552
        val panelH = 768
        val panelIndices = IntArray(panelW * panelH)

        for (y in 0 until srcH) {
            for (x in 0 until srcW) {
                val srcIdx = y * srcW + x

                // 旋转 -90°（相对之前的 +90° 翻转 180°），使横屏方向正确对齐：
                // px = y              (0..551)
                // py = 768 - 1 - x    (0..767)
                val px = y
                val py = panelH - 1 - x
                val panelIdx = py * panelW + px

                panelIndices[panelIdx] = paletteIndices[srcIdx]
            }
        }
        return panelIndices
    }

    // --- 14 种抖动算法实现 ---

    private fun floydSteinbergDither2bpp(srcPixels: IntArray, w: Int, h: Int, paletteRGB: IntArray, outIndices: IntArray, outPreviewPixels: IntArray, ensureActive: (() -> Unit)? = null) {
        val rErr = FloatArray(w * h)
        val gErr = FloatArray(w * h)
        val bErr = FloatArray(w * h)

        for (i in 0 until w * h) {
            val c = srcPixels[i]
            rErr[i] = getR(c).toFloat()
            gErr[i] = getG(c).toFloat()
            bErr[i] = getB(c).toFloat()
        }

        for (y in 0 until h) {
            ensureActive?.invoke()
            for (x in 0 until w) {
                val idx = y * w + x
                val currR = rErr[idx].coerceIn(0f, 255f)
                val currG = gErr[idx].coerceIn(0f, 255f)
                val currB = bErr[idx].coerceIn(0f, 255f)

                val bestIndex = findNearestColorIndex(currR, currG, currB, paletteRGB)
                outIndices[idx] = bestIndex

                val matchedColor = paletteRGB[bestIndex]
                outPreviewPixels[idx] = matchedColor

                val errR = currR - getR(matchedColor)
                val errG = currG - getG(matchedColor)
                val errB = currB - getB(matchedColor)

                distributeError(x + 1, y, errR, errG, errB, 7f / 16f, rErr, gErr, bErr, w, h)
                distributeError(x - 1, y + 1, errR, errG, errB, 3f / 16f, rErr, gErr, bErr, w, h)
                distributeError(x, y + 1, errR, errG, errB, 5f / 16f, rErr, gErr, bErr, w, h)
                distributeError(x + 1, y + 1, errR, errG, errB, 1f / 16f, rErr, gErr, bErr, w, h)
            }
        }
    }

    private fun atkinsonDither2bpp(srcPixels: IntArray, w: Int, h: Int, paletteRGB: IntArray, outIndices: IntArray, outPreviewPixels: IntArray, ensureActive: (() -> Unit)? = null) {
        val rErr = FloatArray(w * h)
        val gErr = FloatArray(w * h)
        val bErr = FloatArray(w * h)

        for (i in 0 until w * h) {
            val c = srcPixels[i]
            rErr[i] = getR(c).toFloat()
            gErr[i] = getG(c).toFloat()
            bErr[i] = getB(c).toFloat()
        }

        for (y in 0 until h) {
            ensureActive?.invoke()
            for (x in 0 until w) {
                val idx = y * w + x
                val currR = rErr[idx].coerceIn(0f, 255f)
                val currG = gErr[idx].coerceIn(0f, 255f)
                val currB = bErr[idx].coerceIn(0f, 255f)

                val bestIndex = findNearestColorIndex(currR, currG, currB, paletteRGB)
                outIndices[idx] = bestIndex

                val matchedColor = paletteRGB[bestIndex]
                outPreviewPixels[idx] = matchedColor

                val errR = currR - getR(matchedColor)
                val errG = currG - getG(matchedColor)
                val errB = currB - getB(matchedColor)

                val factor = 1f / 8f
                distributeError(x + 1, y, errR, errG, errB, factor, rErr, gErr, bErr, w, h)
                distributeError(x + 2, y, errR, errG, errB, factor, rErr, gErr, bErr, w, h)
                distributeError(x - 1, y + 1, errR, errG, errB, factor, rErr, gErr, bErr, w, h)
                distributeError(x, y + 1, errR, errG, errB, factor, rErr, gErr, bErr, w, h)
                distributeError(x + 1, y + 1, errR, errG, errB, factor, rErr, gErr, bErr, w, h)
                distributeError(x, y + 2, errR, errG, errB, factor, rErr, gErr, bErr, w, h)
            }
        }
    }

    private fun jjnDither2bpp(srcPixels: IntArray, w: Int, h: Int, paletteRGB: IntArray, outIndices: IntArray, outPreviewPixels: IntArray, ensureActive: (() -> Unit)? = null) {
        val rErr = FloatArray(w * h)
        val gErr = FloatArray(w * h)
        val bErr = FloatArray(w * h)

        for (i in 0 until w * h) {
            val c = srcPixels[i]
            rErr[i] = getR(c).toFloat()
            gErr[i] = getG(c).toFloat()
            bErr[i] = getB(c).toFloat()
        }

        for (y in 0 until h) {
            ensureActive?.invoke()
            for (x in 0 until w) {
                val idx = y * w + x
                val currR = rErr[idx].coerceIn(0f, 255f)
                val currG = gErr[idx].coerceIn(0f, 255f)
                val currB = bErr[idx].coerceIn(0f, 255f)

                val bestIndex = findNearestColorIndex(currR, currG, currB, paletteRGB)
                outIndices[idx] = bestIndex

                val matchedColor = paletteRGB[bestIndex]
                outPreviewPixels[idx] = matchedColor

                val errR = currR - getR(matchedColor)
                val errG = currG - getG(matchedColor)
                val errB = currB - getB(matchedColor)

                val div = 48f
                distributeError(x + 1, y, errR, errG, errB, 7f / div, rErr, gErr, bErr, w, h)
                distributeError(x + 2, y, errR, errG, errB, 5f / div, rErr, gErr, bErr, w, h)
                distributeError(x - 2, y + 1, errR, errG, errB, 3f / div, rErr, gErr, bErr, w, h)
                distributeError(x - 1, y + 1, errR, errG, errB, 5f / div, rErr, gErr, bErr, w, h)
                distributeError(x, y + 1, errR, errG, errB, 7f / div, rErr, gErr, bErr, w, h)
                distributeError(x + 1, y + 1, errR, errG, errB, 5f / div, rErr, gErr, bErr, w, h)
                distributeError(x + 2, y + 1, errR, errG, errB, 3f / div, rErr, gErr, bErr, w, h)
                distributeError(x - 2, y + 2, errR, errG, errB, 1f / div, rErr, gErr, bErr, w, h)
                distributeError(x - 1, y + 2, errR, errG, errB, 3f / div, rErr, gErr, bErr, w, h)
                distributeError(x, y + 2, errR, errG, errB, 5f / div, rErr, gErr, bErr, w, h)
                distributeError(x + 1, y + 2, errR, errG, errB, 3f / div, rErr, gErr, bErr, w, h)
                distributeError(x + 2, y + 2, errR, errG, errB, 1f / div, rErr, gErr, bErr, w, h)
            }
        }
    }

    private fun stuckiDither2bpp(srcPixels: IntArray, w: Int, h: Int, paletteRGB: IntArray, outIndices: IntArray, outPreviewPixels: IntArray, ensureActive: (() -> Unit)? = null) {
        val rErr = FloatArray(w * h)
        val gErr = FloatArray(w * h)
        val bErr = FloatArray(w * h)

        for (i in 0 until w * h) {
            val c = srcPixels[i]
            rErr[i] = getR(c).toFloat()
            gErr[i] = getG(c).toFloat()
            bErr[i] = getB(c).toFloat()
        }

        for (y in 0 until h) {
            ensureActive?.invoke()
            for (x in 0 until w) {
                val idx = y * w + x
                val currR = rErr[idx].coerceIn(0f, 255f)
                val currG = gErr[idx].coerceIn(0f, 255f)
                val currB = bErr[idx].coerceIn(0f, 255f)

                val bestIndex = findNearestColorIndex(currR, currG, currB, paletteRGB)
                outIndices[idx] = bestIndex

                val matchedColor = paletteRGB[bestIndex]
                outPreviewPixels[idx] = matchedColor

                val errR = currR - getR(matchedColor)
                val errG = currG - getG(matchedColor)
                val errB = currB - getB(matchedColor)

                val div = 42f
                distributeError(x + 1, y, errR, errG, errB, 8f / div, rErr, gErr, bErr, w, h)
                distributeError(x + 2, y, errR, errG, errB, 4f / div, rErr, gErr, bErr, w, h)
                distributeError(x - 2, y + 1, errR, errG, errB, 2f / div, rErr, gErr, bErr, w, h)
                distributeError(x - 1, y + 1, errR, errG, errB, 4f / div, rErr, gErr, bErr, w, h)
                distributeError(x, y + 1, errR, errG, errB, 8f / div, rErr, gErr, bErr, w, h)
                distributeError(x + 1, y + 1, errR, errG, errB, 4f / div, rErr, gErr, bErr, w, h)
                distributeError(x + 2, y + 1, errR, errG, errB, 2f / div, rErr, gErr, bErr, w, h)
                distributeError(x - 2, y + 2, errR, errG, errB, 1f / div, rErr, gErr, bErr, w, h)
                distributeError(x - 1, y + 2, errR, errG, errB, 2f / div, rErr, gErr, bErr, w, h)
                distributeError(x, y + 2, errR, errG, errB, 4f / div, rErr, gErr, bErr, w, h)
                distributeError(x + 1, y + 2, errR, errG, errB, 2f / div, rErr, gErr, bErr, w, h)
                distributeError(x + 2, y + 2, errR, errG, errB, 1f / div, rErr, gErr, bErr, w, h)
            }
        }
    }

    private fun burkesDither2bpp(srcPixels: IntArray, w: Int, h: Int, paletteRGB: IntArray, outIndices: IntArray, outPreviewPixels: IntArray, ensureActive: (() -> Unit)? = null) {
        val rErr = FloatArray(w * h)
        val gErr = FloatArray(w * h)
        val bErr = FloatArray(w * h)

        for (i in 0 until w * h) {
            val c = srcPixels[i]
            rErr[i] = getR(c).toFloat()
            gErr[i] = getG(c).toFloat()
            bErr[i] = getB(c).toFloat()
        }

        for (y in 0 until h) {
            ensureActive?.invoke()
            for (x in 0 until w) {
                val idx = y * w + x
                val currR = rErr[idx].coerceIn(0f, 255f)
                val currG = gErr[idx].coerceIn(0f, 255f)
                val currB = bErr[idx].coerceIn(0f, 255f)

                val bestIndex = findNearestColorIndex(currR, currG, currB, paletteRGB)
                outIndices[idx] = bestIndex

                val matchedColor = paletteRGB[bestIndex]
                outPreviewPixels[idx] = matchedColor

                val errR = currR - getR(matchedColor)
                val errG = currG - getG(matchedColor)
                val errB = currB - getB(matchedColor)

                val div = 32f
                distributeError(x + 1, y, errR, errG, errB, 8f / div, rErr, gErr, bErr, w, h)
                distributeError(x + 2, y, errR, errG, errB, 4f / div, rErr, gErr, bErr, w, h)
                distributeError(x - 2, y + 1, errR, errG, errB, 2f / div, rErr, gErr, bErr, w, h)
                distributeError(x - 1, y + 1, errR, errG, errB, 4f / div, rErr, gErr, bErr, w, h)
                distributeError(x, y + 1, errR, errG, errB, 8f / div, rErr, gErr, bErr, w, h)
                distributeError(x + 1, y + 1, errR, errG, errB, 4f / div, rErr, gErr, bErr, w, h)
                distributeError(x + 2, y + 1, errR, errG, errB, 2f / div, rErr, gErr, bErr, w, h)
            }
        }
    }

    private fun sierra3Dither2bpp(srcPixels: IntArray, w: Int, h: Int, paletteRGB: IntArray, outIndices: IntArray, outPreviewPixels: IntArray, ensureActive: (() -> Unit)? = null) {
        val rErr = FloatArray(w * h)
        val gErr = FloatArray(w * h)
        val bErr = FloatArray(w * h)

        for (i in 0 until w * h) {
            val c = srcPixels[i]
            rErr[i] = getR(c).toFloat()
            gErr[i] = getG(c).toFloat()
            bErr[i] = getB(c).toFloat()
        }

        for (y in 0 until h) {
            ensureActive?.invoke()
            for (x in 0 until w) {
                val idx = y * w + x
                val currR = rErr[idx].coerceIn(0f, 255f)
                val currG = gErr[idx].coerceIn(0f, 255f)
                val currB = bErr[idx].coerceIn(0f, 255f)

                val bestIndex = findNearestColorIndex(currR, currG, currB, paletteRGB)
                outIndices[idx] = bestIndex

                val matchedColor = paletteRGB[bestIndex]
                outPreviewPixels[idx] = matchedColor

                val errR = currR - getR(matchedColor)
                val errG = currG - getG(matchedColor)
                val errB = currB - getB(matchedColor)

                val div = 32f
                distributeError(x + 1, y, errR, errG, errB, 5f / div, rErr, gErr, bErr, w, h)
                distributeError(x + 2, y, errR, errG, errB, 3f / div, rErr, gErr, bErr, w, h)
                distributeError(x - 2, y + 1, errR, errG, errB, 2f / div, rErr, gErr, bErr, w, h)
                distributeError(x - 1, y + 1, errR, errG, errB, 4f / div, rErr, gErr, bErr, w, h)
                distributeError(x, y + 1, errR, errG, errB, 5f / div, rErr, gErr, bErr, w, h)
                distributeError(x + 1, y + 1, errR, errG, errB, 4f / div, rErr, gErr, bErr, w, h)
                distributeError(x + 2, y + 1, errR, errG, errB, 2f / div, rErr, gErr, bErr, w, h)
                distributeError(x - 1, y + 2, errR, errG, errB, 2f / div, rErr, gErr, bErr, w, h)
                distributeError(x, y + 2, errR, errG, errB, 3f / div, rErr, gErr, bErr, w, h)
                distributeError(x + 1, y + 2, errR, errG, errB, 2f / div, rErr, gErr, bErr, w, h)
            }
        }
    }

    private fun sierra2Dither2bpp(srcPixels: IntArray, w: Int, h: Int, paletteRGB: IntArray, outIndices: IntArray, outPreviewPixels: IntArray, ensureActive: (() -> Unit)? = null) {
        val rErr = FloatArray(w * h)
        val gErr = FloatArray(w * h)
        val bErr = FloatArray(w * h)

        for (i in 0 until w * h) {
            val c = srcPixels[i]
            rErr[i] = getR(c).toFloat()
            gErr[i] = getG(c).toFloat()
            bErr[i] = getB(c).toFloat()
        }

        for (y in 0 until h) {
            ensureActive?.invoke()
            for (x in 0 until w) {
                val idx = y * w + x
                val currR = rErr[idx].coerceIn(0f, 255f)
                val currG = gErr[idx].coerceIn(0f, 255f)
                val currB = bErr[idx].coerceIn(0f, 255f)

                val bestIndex = findNearestColorIndex(currR, currG, currB, paletteRGB)
                outIndices[idx] = bestIndex

                val matchedColor = paletteRGB[bestIndex]
                outPreviewPixels[idx] = matchedColor

                val errR = currR - getR(matchedColor)
                val errG = currG - getG(matchedColor)
                val errB = currB - getB(matchedColor)

                val div = 16f
                distributeError(x + 1, y, errR, errG, errB, 4f / div, rErr, gErr, bErr, w, h)
                distributeError(x + 2, y, errR, errG, errB, 3f / div, rErr, gErr, bErr, w, h)
                distributeError(x - 2, y + 1, errR, errG, errB, 1f / div, rErr, gErr, bErr, w, h)
                distributeError(x - 1, y + 1, errR, errG, errB, 2f / div, rErr, gErr, bErr, w, h)
                distributeError(x, y + 1, errR, errG, errB, 3f / div, rErr, gErr, bErr, w, h)
                distributeError(x + 1, y + 1, errR, errG, errB, 2f / div, rErr, gErr, bErr, w, h)
                distributeError(x + 2, y + 1, errR, errG, errB, 1f / div, rErr, gErr, bErr, w, h)
            }
        }
    }

    private fun sierraLiteDither2bpp(srcPixels: IntArray, w: Int, h: Int, paletteRGB: IntArray, outIndices: IntArray, outPreviewPixels: IntArray, ensureActive: (() -> Unit)? = null) {
        val rErr = FloatArray(w * h)
        val gErr = FloatArray(w * h)
        val bErr = FloatArray(w * h)

        for (i in 0 until w * h) {
            val c = srcPixels[i]
            rErr[i] = getR(c).toFloat()
            gErr[i] = getG(c).toFloat()
            bErr[i] = getB(c).toFloat()
        }

        for (y in 0 until h) {
            ensureActive?.invoke()
            for (x in 0 until w) {
                val idx = y * w + x
                val currR = rErr[idx].coerceIn(0f, 255f)
                val currG = gErr[idx].coerceIn(0f, 255f)
                val currB = bErr[idx].coerceIn(0f, 255f)

                val bestIndex = findNearestColorIndex(currR, currG, currB, paletteRGB)
                outIndices[idx] = bestIndex

                val matchedColor = paletteRGB[bestIndex]
                outPreviewPixels[idx] = matchedColor

                val errR = currR - getR(matchedColor)
                val errG = currG - getG(matchedColor)
                val errB = currB - getB(matchedColor)

                val div = 4f
                distributeError(x + 1, y, errR, errG, errB, 2f / div, rErr, gErr, bErr, w, h)
                distributeError(x - 1, y + 1, errR, errG, errB, 1f / div, rErr, gErr, bErr, w, h)
                distributeError(x, y + 1, errR, errG, errB, 1f / div, rErr, gErr, bErr, w, h)
            }
        }
    }

    private fun bayer2x2Dither2bpp(srcPixels: IntArray, w: Int, h: Int, paletteRGB: IntArray, outIndices: IntArray, outPreviewPixels: IntArray, ensureActive: (() -> Unit)? = null) {
        val bayerMatrix = arrayOf(
            intArrayOf(0, 2),
            intArrayOf(3, 1)
        )
        for (y in 0 until h) {
            ensureActive?.invoke()
            for (x in 0 until w) {
                val idx = y * w + x
                val c = srcPixels[idx]
                val threshold = (bayerMatrix[y % 2][x % 2] / 4.0f - 0.5f) * 64f

                val r = (getR(c) + threshold).coerceIn(0f, 255f)
                val g = (getG(c) + threshold).coerceIn(0f, 255f)
                val b = (getB(c) + threshold).coerceIn(0f, 255f)

                val bestIndex = findNearestColorIndex(r, g, b, paletteRGB)
                outIndices[idx] = bestIndex
                outPreviewPixels[idx] = paletteRGB[bestIndex]
            }
        }
    }

    private fun bayer4x4Dither2bpp(srcPixels: IntArray, w: Int, h: Int, paletteRGB: IntArray, outIndices: IntArray, outPreviewPixels: IntArray, ensureActive: (() -> Unit)? = null) {
        val bayerMatrix = arrayOf(
            intArrayOf(0, 8, 2, 10),
            intArrayOf(12, 4, 14, 6),
            intArrayOf(3, 11, 1, 9),
            intArrayOf(15, 7, 13, 5)
        )
        for (y in 0 until h) {
            ensureActive?.invoke()
            for (x in 0 until w) {
                val idx = y * w + x
                val c = srcPixels[idx]
                val threshold = (bayerMatrix[y % 4][x % 4] / 16.0f - 0.5f) * 64f

                val r = (getR(c) + threshold).coerceIn(0f, 255f)
                val g = (getG(c) + threshold).coerceIn(0f, 255f)
                val b = (getB(c) + threshold).coerceIn(0f, 255f)

                val bestIndex = findNearestColorIndex(r, g, b, paletteRGB)
                outIndices[idx] = bestIndex
                outPreviewPixels[idx] = paletteRGB[bestIndex]
            }
        }
    }

    private fun bayer8x8Dither2bpp(srcPixels: IntArray, w: Int, h: Int, paletteRGB: IntArray, outIndices: IntArray, outPreviewPixels: IntArray, ensureActive: (() -> Unit)? = null) {
        val bayer8 = arrayOf(
            intArrayOf( 0, 32,  8, 40,  2, 34, 10, 42),
            intArrayOf(48, 16, 56, 24, 50, 18, 58, 26),
            intArrayOf(12, 44,  4, 36, 14, 46,  6, 38),
            intArrayOf(60, 28, 52, 20, 62, 30, 54, 22),
            intArrayOf( 3, 35, 11, 43,  1, 33,  9, 41),
            intArrayOf(51, 19, 59, 27, 49, 17, 57, 25),
            intArrayOf(15, 47,  7, 39, 13, 45,  5, 37),
            intArrayOf(63, 31, 55, 23, 61, 29, 53, 21)
        )
        for (y in 0 until h) {
            ensureActive?.invoke()
            for (x in 0 until w) {
                val idx = y * w + x
                val c = srcPixels[idx]
                val threshold = (bayer8[y % 8][x % 8] / 64.0f - 0.5f) * 64f

                val r = (getR(c) + threshold).coerceIn(0f, 255f)
                val g = (getG(c) + threshold).coerceIn(0f, 255f)
                val b = (getB(c) + threshold).coerceIn(0f, 255f)

                val bestIndex = findNearestColorIndex(r, g, b, paletteRGB)
                outIndices[idx] = bestIndex
                outPreviewPixels[idx] = paletteRGB[bestIndex]
            }
        }
    }

    private fun clusterDotDither2bpp(srcPixels: IntArray, w: Int, h: Int, paletteRGB: IntArray, outIndices: IntArray, outPreviewPixels: IntArray, ensureActive: (() -> Unit)? = null) {
        val clusterMatrix = arrayOf(
            intArrayOf(12,  5,  6, 13),
            intArrayOf( 4,  0,  1,  7),
            intArrayOf(11,  3,  2,  8),
            intArrayOf(15, 10,  9, 14)
        )
        for (y in 0 until h) {
            ensureActive?.invoke()
            for (x in 0 until w) {
                val idx = y * w + x
                val c = srcPixels[idx]
                val threshold = (clusterMatrix[y % 4][x % 4] / 16.0f - 0.5f) * 64f

                val r = (getR(c) + threshold).coerceIn(0f, 255f)
                val g = (getG(c) + threshold).coerceIn(0f, 255f)
                val b = (getB(c) + threshold).coerceIn(0f, 255f)

                val bestIndex = findNearestColorIndex(r, g, b, paletteRGB)
                outIndices[idx] = bestIndex
                outPreviewPixels[idx] = paletteRGB[bestIndex]
            }
        }
    }

    private fun randomNoiseDither2bpp(srcPixels: IntArray, w: Int, h: Int, paletteRGB: IntArray, outIndices: IntArray, outPreviewPixels: IntArray, ensureActive: (() -> Unit)? = null) {
        val rand = Random(42L)
        for (y in 0 until h) {
            ensureActive?.invoke()
            for (x in 0 until w) {
                val i = y * w + x
                val c = srcPixels[i]
                val noise = (rand.nextFloat() - 0.5f) * 48f

                val r = (getR(c) + noise).coerceIn(0f, 255f)
                val g = (getG(c) + noise).coerceIn(0f, 255f)
                val b = (getB(c) + noise).coerceIn(0f, 255f)

                val bestIndex = findNearestColorIndex(r, g, b, paletteRGB)
                outIndices[i] = bestIndex
                outPreviewPixels[i] = paletteRGB[bestIndex]
            }
        }
    }

    private fun nearestColor2bpp(srcPixels: IntArray, w: Int, h: Int, paletteRGB: IntArray, outIndices: IntArray, outPreviewPixels: IntArray, ensureActive: (() -> Unit)? = null) {
        for (y in 0 until h) {
            ensureActive?.invoke()
            for (x in 0 until w) {
                val i = y * w + x
                val c = srcPixels[i]
                val r = getR(c).toFloat()
                val g = getG(c).toFloat()
                val b = getB(c).toFloat()

                val bestIndex = findNearestColorIndex(r, g, b, paletteRGB)
                outIndices[i] = bestIndex
                outPreviewPixels[i] = paletteRGB[bestIndex]
            }
        }
    }

    // --- 1bpp 抖动算法实现变体 ---

    private fun floydSteinbergDither1bpp(srcPixels: IntArray, w: Int, h: Int, paletteRGB: IntArray, outIndices: IntArray, outPreviewPixels: IntArray, ensureActive: (() -> Unit)? = null) {
        val grayErr = FloatArray(w * h)
        for (i in 0 until w * h) {
            val c = srcPixels[i]
            grayErr[i] = (0.299f * getR(c) + 0.587f * getG(c) + 0.114f * getB(c))
        }

        for (y in 0 until h) {
            ensureActive?.invoke()
            for (x in 0 until w) {
                val idx = y * w + x
                val currGray = grayErr[idx].coerceIn(0f, 255f)

                val bestIndex = if (currGray >= 128f) 1 else 0
                outIndices[idx] = bestIndex
                outPreviewPixels[idx] = paletteRGB[bestIndex]

                val errGray = currGray - (if (bestIndex == 1) 255f else 0f)

                distributeGrayError(x + 1, y, errGray, 7f / 16f, grayErr, w, h)
                distributeGrayError(x - 1, y + 1, errGray, 3f / 16f, grayErr, w, h)
                distributeGrayError(x, y + 1, errGray, 5f / 16f, grayErr, w, h)
                distributeGrayError(x + 1, y + 1, errGray, 1f / 16f, grayErr, w, h)
            }
        }
    }

    private fun atkinsonDither1bpp(srcPixels: IntArray, w: Int, h: Int, paletteRGB: IntArray, outIndices: IntArray, outPreviewPixels: IntArray, ensureActive: (() -> Unit)? = null) {
        val grayErr = FloatArray(w * h)
        for (i in 0 until w * h) {
            val c = srcPixels[i]
            grayErr[i] = (0.299f * getR(c) + 0.587f * getG(c) + 0.114f * getB(c))
        }

        for (y in 0 until h) {
            ensureActive?.invoke()
            for (x in 0 until w) {
                val idx = y * w + x
                val currGray = grayErr[idx].coerceIn(0f, 255f)

                val bestIndex = if (currGray >= 128f) 1 else 0
                outIndices[idx] = bestIndex
                outPreviewPixels[idx] = paletteRGB[bestIndex]

                val errGray = currGray - (if (bestIndex == 1) 255f else 0f)
                val factor = 1f / 8f

                distributeGrayError(x + 1, y, errGray, factor, grayErr, w, h)
                distributeGrayError(x + 2, y, errGray, factor, grayErr, w, h)
                distributeGrayError(x - 1, y + 1, errGray, factor, grayErr, w, h)
                distributeGrayError(x, y + 1, errGray, factor, grayErr, w, h)
                distributeGrayError(x + 1, y + 1, errGray, factor, grayErr, w, h)
                distributeGrayError(x, y + 2, errGray, factor, grayErr, w, h)
            }
        }
    }

    private fun jjnDither1bpp(srcPixels: IntArray, w: Int, h: Int, paletteRGB: IntArray, outIndices: IntArray, outPreviewPixels: IntArray, ensureActive: (() -> Unit)? = null) {
        val grayErr = FloatArray(w * h)
        for (i in 0 until w * h) {
            val c = srcPixels[i]
            grayErr[i] = (0.299f * getR(c) + 0.587f * getG(c) + 0.114f * getB(c))
        }

        for (y in 0 until h) {
            ensureActive?.invoke()
            for (x in 0 until w) {
                val idx = y * w + x
                val currGray = grayErr[idx].coerceIn(0f, 255f)

                val bestIndex = if (currGray >= 128f) 1 else 0
                outIndices[idx] = bestIndex
                outPreviewPixels[idx] = paletteRGB[bestIndex]

                val errGray = currGray - (if (bestIndex == 1) 255f else 0f)
                val div = 48f

                distributeGrayError(x + 1, y, errGray, 7f / div, grayErr, w, h)
                distributeGrayError(x + 2, y, errGray, 5f / div, grayErr, w, h)
                distributeGrayError(x - 2, y + 1, errGray, 3f / div, grayErr, w, h)
                distributeGrayError(x - 1, y + 1, errGray, 5f / div, grayErr, w, h)
                distributeGrayError(x, y + 1, errGray, 7f / div, grayErr, w, h)
                distributeGrayError(x + 1, y + 1, errGray, 5f / div, grayErr, w, h)
                distributeGrayError(x + 2, y + 1, errGray, 3f / div, grayErr, w, h)
                distributeGrayError(x - 2, y + 2, errGray, 1f / div, grayErr, w, h)
                distributeGrayError(x - 1, y + 2, errGray, 3f / div, grayErr, w, h)
                distributeGrayError(x, y + 2, errGray, 5f / div, grayErr, w, h)
                distributeGrayError(x + 1, y + 2, errGray, 3f / div, grayErr, w, h)
                distributeGrayError(x + 2, y + 2, errGray, 1f / div, grayErr, w, h)
            }
        }
    }

    private fun stuckiDither1bpp(srcPixels: IntArray, w: Int, h: Int, paletteRGB: IntArray, outIndices: IntArray, outPreviewPixels: IntArray, ensureActive: (() -> Unit)? = null) {
        val grayErr = FloatArray(w * h)
        for (i in 0 until w * h) {
            val c = srcPixels[i]
            grayErr[i] = (0.299f * getR(c) + 0.587f * getG(c) + 0.114f * getB(c))
        }

        for (y in 0 until h) {
            ensureActive?.invoke()
            for (x in 0 until w) {
                val idx = y * w + x
                val currGray = grayErr[idx].coerceIn(0f, 255f)

                val bestIndex = if (currGray >= 128f) 1 else 0
                outIndices[idx] = bestIndex
                outPreviewPixels[idx] = paletteRGB[bestIndex]

                val errGray = currGray - (if (bestIndex == 1) 255f else 0f)
                val div = 42f

                distributeGrayError(x + 1, y, errGray, 8f / div, grayErr, w, h)
                distributeGrayError(x + 2, y, errGray, 4f / div, grayErr, w, h)
                distributeGrayError(x - 2, y + 1, errGray, 2f / div, grayErr, w, h)
                distributeGrayError(x - 1, y + 1, errGray, 4f / div, grayErr, w, h)
                distributeGrayError(x, y + 1, errGray, 8f / div, grayErr, w, h)
                distributeGrayError(x + 1, y + 1, errGray, 4f / div, grayErr, w, h)
                distributeGrayError(x + 2, y + 1, errGray, 2f / div, grayErr, w, h)
                distributeGrayError(x - 2, y + 2, errGray, 1f / div, grayErr, w, h)
                distributeGrayError(x - 1, y + 2, errGray, 2f / div, grayErr, w, h)
                distributeGrayError(x, y + 2, errGray, 4f / div, grayErr, w, h)
                distributeGrayError(x + 1, y + 2, errGray, 2f / div, grayErr, w, h)
                distributeGrayError(x + 2, y + 2, errGray, 1f / div, grayErr, w, h)
            }
        }
    }

    private fun burkesDither1bpp(srcPixels: IntArray, w: Int, h: Int, paletteRGB: IntArray, outIndices: IntArray, outPreviewPixels: IntArray, ensureActive: (() -> Unit)? = null) {
        val grayErr = FloatArray(w * h)
        for (i in 0 until w * h) {
            val c = srcPixels[i]
            grayErr[i] = (0.299f * getR(c) + 0.587f * getG(c) + 0.114f * getB(c))
        }

        for (y in 0 until h) {
            ensureActive?.invoke()
            for (x in 0 until w) {
                val idx = y * w + x
                val currGray = grayErr[idx].coerceIn(0f, 255f)

                val bestIndex = if (currGray >= 128f) 1 else 0
                outIndices[idx] = bestIndex
                outPreviewPixels[idx] = paletteRGB[bestIndex]

                val errGray = currGray - (if (bestIndex == 1) 255f else 0f)

                distributeGrayError(x + 1, y, errGray, 8f / 32f, grayErr, w, h)
                distributeGrayError(x + 2, y, errGray, 4f / 32f, grayErr, w, h)
                distributeGrayError(x - 2, y + 1, errGray, 2f / 32f, grayErr, w, h)
                distributeGrayError(x - 1, y + 1, errGray, 4f / 32f, grayErr, w, h)
                distributeGrayError(x, y + 1, errGray, 8f / 32f, grayErr, w, h)
                distributeGrayError(x + 1, y + 1, errGray, 4f / 32f, grayErr, w, h)
                distributeGrayError(x + 2, y + 1, errGray, 2f / 32f, grayErr, w, h)
            }
        }
    }

    private fun sierra3Dither1bpp(srcPixels: IntArray, w: Int, h: Int, paletteRGB: IntArray, outIndices: IntArray, outPreviewPixels: IntArray, ensureActive: (() -> Unit)? = null) {
        val grayErr = FloatArray(w * h)
        for (i in 0 until w * h) {
            val c = srcPixels[i]
            grayErr[i] = (0.299f * getR(c) + 0.587f * getG(c) + 0.114f * getB(c))
        }

        for (y in 0 until h) {
            ensureActive?.invoke()
            for (x in 0 until w) {
                val idx = y * w + x
                val currGray = grayErr[idx].coerceIn(0f, 255f)

                val bestIndex = if (currGray >= 128f) 1 else 0
                outIndices[idx] = bestIndex
                outPreviewPixels[idx] = paletteRGB[bestIndex]

                val errGray = currGray - (if (bestIndex == 1) 255f else 0f)
                val div = 32f

                distributeGrayError(x + 1, y, errGray, 5f / div, grayErr, w, h)
                distributeGrayError(x + 2, y, errGray, 3f / div, grayErr, w, h)
                distributeGrayError(x - 2, y + 1, errGray, 2f / div, grayErr, w, h)
                distributeGrayError(x - 1, y + 1, errGray, 4f / div, grayErr, w, h)
                distributeGrayError(x, y + 1, errGray, 5f / div, grayErr, w, h)
                distributeGrayError(x + 1, y + 1, errGray, 4f / div, grayErr, w, h)
                distributeGrayError(x + 2, y + 1, errGray, 2f / div, grayErr, w, h)
                distributeGrayError(x - 1, y + 2, errGray, 2f / div, grayErr, w, h)
                distributeGrayError(x, y + 2, errGray, 3f / div, grayErr, w, h)
                distributeGrayError(x + 1, y + 2, errGray, 2f / div, grayErr, w, h)
            }
        }
    }

    private fun sierra2Dither1bpp(srcPixels: IntArray, w: Int, h: Int, paletteRGB: IntArray, outIndices: IntArray, outPreviewPixels: IntArray, ensureActive: (() -> Unit)? = null) {
        val grayErr = FloatArray(w * h)
        for (i in 0 until w * h) {
            val c = srcPixels[i]
            grayErr[i] = (0.299f * getR(c) + 0.587f * getG(c) + 0.114f * getB(c))
        }

        for (y in 0 until h) {
            ensureActive?.invoke()
            for (x in 0 until w) {
                val idx = y * w + x
                val currGray = grayErr[idx].coerceIn(0f, 255f)

                val bestIndex = if (currGray >= 128f) 1 else 0
                outIndices[idx] = bestIndex
                outPreviewPixels[idx] = paletteRGB[bestIndex]

                val errGray = currGray - (if (bestIndex == 1) 255f else 0f)
                val div = 16f

                distributeGrayError(x + 1, y, errGray, 4f / div, grayErr, w, h)
                distributeGrayError(x + 2, y, errGray, 3f / div, grayErr, w, h)
                distributeGrayError(x - 2, y + 1, errGray, 1f / div, grayErr, w, h)
                distributeGrayError(x - 1, y + 1, errGray, 2f / div, grayErr, w, h)
                distributeGrayError(x, y + 1, errGray, 3f / div, grayErr, w, h)
                distributeGrayError(x + 1, y + 1, errGray, 2f / div, grayErr, w, h)
                distributeGrayError(x + 2, y + 1, errGray, 1f / div, grayErr, w, h)
            }
        }
    }

    private fun sierraLiteDither1bpp(srcPixels: IntArray, w: Int, h: Int, paletteRGB: IntArray, outIndices: IntArray, outPreviewPixels: IntArray, ensureActive: (() -> Unit)? = null) {
        val grayErr = FloatArray(w * h)
        for (i in 0 until w * h) {
            val c = srcPixels[i]
            grayErr[i] = (0.299f * getR(c) + 0.587f * getG(c) + 0.114f * getB(c))
        }

        for (y in 0 until h) {
            ensureActive?.invoke()
            for (x in 0 until w) {
                val idx = y * w + x
                val currGray = grayErr[idx].coerceIn(0f, 255f)

                val bestIndex = if (currGray >= 128f) 1 else 0
                outIndices[idx] = bestIndex
                outPreviewPixels[idx] = paletteRGB[bestIndex]

                val errGray = currGray - (if (bestIndex == 1) 255f else 0f)
                val div = 4f

                distributeGrayError(x + 1, y, errGray, 2f / div, grayErr, w, h)
                distributeGrayError(x - 1, y + 1, errGray, 1f / div, grayErr, w, h)
                distributeGrayError(x, y + 1, errGray, 1f / div, grayErr, w, h)
            }
        }
    }

    private fun bayer2x2Dither1bpp(srcPixels: IntArray, w: Int, h: Int, paletteRGB: IntArray, outIndices: IntArray, outPreviewPixels: IntArray, ensureActive: (() -> Unit)? = null) {
        val bayerMatrix = arrayOf(
            intArrayOf(0, 2),
            intArrayOf(3, 1)
        )
        for (y in 0 until h) {
            ensureActive?.invoke()
            for (x in 0 until w) {
                val idx = y * w + x
                val c = srcPixels[idx]
                val threshold = (bayerMatrix[y % 2][x % 2] / 4.0f - 0.5f) * 64f
                val gray = (0.299f * getR(c) + 0.587f * getG(c) + 0.114f * getB(c) + threshold).coerceIn(0f, 255f)

                val bestIndex = if (gray >= 128f) 1 else 0
                outIndices[idx] = bestIndex
                outPreviewPixels[idx] = paletteRGB[bestIndex]
            }
        }
    }

    private fun bayer4x4Dither1bpp(srcPixels: IntArray, w: Int, h: Int, paletteRGB: IntArray, outIndices: IntArray, outPreviewPixels: IntArray, ensureActive: (() -> Unit)? = null) {
        val bayerMatrix = arrayOf(
            intArrayOf(0, 8, 2, 10),
            intArrayOf(12, 4, 14, 6),
            intArrayOf(3, 11, 1, 9),
            intArrayOf(15, 7, 13, 5)
        )

        for (y in 0 until h) {
            ensureActive?.invoke()
            for (x in 0 until w) {
                val idx = y * w + x
                val c = srcPixels[idx]
                val threshold = (bayerMatrix[y % 4][x % 4] / 16.0f - 0.5f) * 64f
                val gray = (0.299f * getR(c) + 0.587f * getG(c) + 0.114f * getB(c) + threshold).coerceIn(0f, 255f)

                val bestIndex = if (gray >= 128f) 1 else 0
                outIndices[idx] = bestIndex
                outPreviewPixels[idx] = paletteRGB[bestIndex]
            }
        }
    }

    private fun bayer8x8Dither1bpp(srcPixels: IntArray, w: Int, h: Int, paletteRGB: IntArray, outIndices: IntArray, outPreviewPixels: IntArray, ensureActive: (() -> Unit)? = null) {
        val bayer8 = arrayOf(
            intArrayOf( 0, 32,  8, 40,  2, 34, 10, 42),
            intArrayOf(48, 16, 56, 24, 50, 18, 58, 26),
            intArrayOf(12, 44,  4, 36, 14, 46,  6, 38),
            intArrayOf(60, 28, 52, 20, 62, 30, 54, 22),
            intArrayOf( 3, 35, 11, 43,  1, 33,  9, 41),
            intArrayOf(51, 19, 59, 27, 49, 17, 57, 25),
            intArrayOf(15, 47,  7, 39, 13, 45,  5, 37),
            intArrayOf(63, 31, 55, 23, 61, 29, 53, 21)
        )
        for (y in 0 until h) {
            ensureActive?.invoke()
            for (x in 0 until w) {
                val idx = y * w + x
                val c = srcPixels[idx]
                val threshold = (bayer8[y % 8][x % 8] / 64.0f - 0.5f) * 64f
                val gray = (0.299f * getR(c) + 0.587f * getG(c) + 0.114f * getB(c) + threshold).coerceIn(0f, 255f)

                val bestIndex = if (gray >= 128f) 1 else 0
                outIndices[idx] = bestIndex
                outPreviewPixels[idx] = paletteRGB[bestIndex]
            }
        }
    }

    private fun clusterDotDither1bpp(srcPixels: IntArray, w: Int, h: Int, paletteRGB: IntArray, outIndices: IntArray, outPreviewPixels: IntArray, ensureActive: (() -> Unit)? = null) {
        val clusterMatrix = arrayOf(
            intArrayOf(12,  5,  6, 13),
            intArrayOf( 4,  0,  1,  7),
            intArrayOf(11,  3,  2,  8),
            intArrayOf(15, 10,  9, 14)
        )
        for (y in 0 until h) {
            ensureActive?.invoke()
            for (x in 0 until w) {
                val idx = y * w + x
                val c = srcPixels[idx]
                val threshold = (clusterMatrix[y % 4][x % 4] / 16.0f - 0.5f) * 64f
                val gray = (0.299f * getR(c) + 0.587f * getG(c) + 0.114f * getB(c) + threshold).coerceIn(0f, 255f)

                val bestIndex = if (gray >= 128f) 1 else 0
                outIndices[idx] = bestIndex
                outPreviewPixels[idx] = paletteRGB[bestIndex]
            }
        }
    }

    private fun randomNoiseDither1bpp(srcPixels: IntArray, w: Int, h: Int, paletteRGB: IntArray, outIndices: IntArray, outPreviewPixels: IntArray, ensureActive: (() -> Unit)? = null) {
        val rand = Random(42L)
        for (y in 0 until h) {
            ensureActive?.invoke()
            for (x in 0 until w) {
                val i = y * w + x
                val c = srcPixels[i]
                val noise = (rand.nextFloat() - 0.5f) * 48f
                val gray = (0.299f * getR(c) + 0.587f * getG(c) + 0.114f * getB(c) + noise).coerceIn(0f, 255f)

                val bestIndex = if (gray >= 128f) 1 else 0
                outIndices[i] = bestIndex
                outPreviewPixels[i] = paletteRGB[bestIndex]
            }
        }
    }

    private fun nearestColor1bpp(srcPixels: IntArray, w: Int, h: Int, paletteRGB: IntArray, outIndices: IntArray, outPreviewPixels: IntArray, ensureActive: (() -> Unit)? = null) {
        for (y in 0 until h) {
            ensureActive?.invoke()
            for (x in 0 until w) {
                val i = y * w + x
                val c = srcPixels[i]
                val gray = 0.299f * getR(c) + 0.587f * getG(c) + 0.114f * getB(c)
                val bestIndex = if (gray >= 128f) 1 else 0
                outIndices[i] = bestIndex
                outPreviewPixels[i] = paletteRGB[bestIndex]
            }
        }
    }

    private fun distributeError(
        x: Int, y: Int,
        errR: Float, errG: Float, errB: Float,
        factor: Float,
        rErr: FloatArray, gErr: FloatArray, bErr: FloatArray,
        w: Int, h: Int
    ) {
        if (x in 0 until w && y in 0 until h) {
            val idx = y * w + x
            rErr[idx] += errR * factor
            gErr[idx] += errG * factor
            bErr[idx] += errB * factor
        }
    }

    private fun distributeGrayError(
        x: Int, y: Int,
        errGray: Float,
        factor: Float,
        grayErr: FloatArray,
        w: Int, h: Int
    ) {
        if (x in 0 until w && y in 0 until h) {
            val idx = y * w + x
            grayErr[idx] += errGray * factor
        }
    }

    private fun findNearestColorIndex(rIn: Float, gIn: Float, bIn: Float, paletteRGB: IntArray): Int {
        val r = rIn.coerceIn(0f, 255f)
        val g = gIn.coerceIn(0f, 255f)
        val b = bIn.coerceIn(0f, 255f)

        // 1bpp 模式（黑白）
        if (paletteRGB.size == 2) {
            val gray = 0.299f * r + 0.587f * g + 0.114f * b
            return if (gray > 127.5f) 1 else 0
        }

        // 2bpp 四色模式（色码 0=黑，1=白，2=黄，3=红）
        val maxC = maxOf(r, maxOf(g, b))
        val minC = minOf(r, minOf(g, b))
        val delta = maxC - minC

        val v = maxC / 255f
        val s = if (maxC <= 0f) 0f else delta / maxC

        // 1. 极端亮度阈值 -> 直接返回纯黑或纯白
        if (v < 0.15f) return 0 // 黑
        if (v > 0.90f && s < 0.12f) return 1 // 白

        // 2. 四色墨水屏上非原生色（蓝、绿、紫）的感知色相覆盖
        if (s > 0.20f) {
            val h = if (delta <= 0f) {
                0f
            } else {
                val hCalc = when {
                    maxC == r -> 60f * ((g - b) / delta)
                    maxC == g -> 60f * ((b - r) / delta) + 120f
                    else -> 60f * ((r - g) / delta) + 240f
                }
                if (hCalc < 0f) hCalc + 360f else hCalc
            }

            if (h in 70f..165f) {
                // 绿色区间 -> 墨水屏渲染为黄色（偏亮时）或黑色（偏暗时）
                return if (v > 0.40f) 2 else 0
            } else if (h in 165f..255f) {
                // 蓝/青区间 -> 墨水屏无法呈现蓝色，严格映射为纸白或黑色
                return if (v > 0.55f) 1 else 0
            } else if (h in 255f..360f) {
                // 紫/品红/粉区间 -> 墨水屏渲染为红色（偏亮时）或黑色（偏暗时）
                return if (v > 0.35f) 3 else 0
            }
        }

        // 3. 兜底：到黑、白、黄、红的感知加权距离
        var minDistance = Float.MAX_VALUE
        var bestIndex = 0

        for (i in paletteRGB.indices) {
            val pr = getR(paletteRGB[i]).toFloat()
            val pg = getG(paletteRGB[i]).toFloat()
            val pb = getB(paletteRGB[i]).toFloat()

            val dr = r - pr
            val dg = g - pg
            val db = b - pb
            val dist = 0.30f * (dr * dr) + 0.59f * (dg * dg) + 0.11f * (db * db)

            if (dist < minDistance) {
                minDistance = dist
                bestIndex = i
            }
        }
        return bestIndex
    }

    /**
     * 将调色板索引（0..3）打包为 2bpp 字节数组。
     * 高位在前：x=4k -> 位 7-6，x=4k+1 -> 位 5-4，x=4k+2 -> 位 3-2，x=4k+3 -> 位 1-0。
     */
    fun pack2bpp(palette: IntArray, w: Int = 552, h: Int = 768): ByteArray {
        val bytesPerRow = (w + 3) / 4
        val out = ByteArray(bytesPerRow * h)
        var o = 0
        for (y in 0 until h) {
            var x = 0
            val rowStart = y * w
            while (x < w) {
                val idx = rowStart + x
                val p0 = if (idx < palette.size) palette[idx] and 0x03 else 1
                val p1 = if (x + 1 < w && idx + 1 < palette.size) palette[idx + 1] and 0x03 else 1
                val p2 = if (x + 2 < w && idx + 2 < palette.size) palette[idx + 2] and 0x03 else 1
                val p3 = if (x + 3 < w && idx + 3 < palette.size) palette[idx + 3] and 0x03 else 1

                val b = (p0 shl 6) or (p1 shl 4) or (p2 shl 2) or p3
                out[o++] = b.toByte()
                x += 4
            }
        }
        return out
    }

    /**
     * 将调色板索引（0=黑，1=白）打包为 1bpp 字节数组。
     * 位 = 1 表示白，位 = 0 表示黑。位 7 为最左侧。
     */
    fun pack1bpp(palette: IntArray, w: Int = 552, h: Int = 768): ByteArray {
        val bytesPerRow = (w + 7) / 8
        val out = ByteArray(bytesPerRow * h)
        var o = 0
        for (y in 0 until h) {
            var x = 0
            val rowStart = y * w
            while (x < w) {
                val idx = rowStart + x
                var b = 0
                for (bit in 0 until 8) {
                    val px = x + bit
                    val pIdx = idx + bit
                    val isWhite = if (px < w && pIdx < palette.size) (palette[pIdx] and 0x01) == 1 else true
                    if (isWhite) {
                        b = b or (1 shl (7 - bit))
                    }
                }
                out[o++] = b.toByte()
                x += 8
            }
        }
        return out
    }

    fun calculateCrc32(data: ByteArray): Long {
        val crc = CRC32()
        crc.update(data)
        return crc.value and 0xFFFFFFFFL
    }

    /**
     * 生成内置测试图案，用于竖屏（552x768）或横屏（768x552）。
     */
    fun generateTestPattern(patternType: String, orientation: ScreenOrientation = ScreenOrientation.PORTRAIT): Bitmap {
        val width = orientation.width
        val height = orientation.height

        val bitmap = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888)
        val canvas = Canvas(bitmap)

        when (patternType) {
            "YELLOW_RED_TEST" -> {
                val paint = Paint()

                // 左侧黄色
                paint.color = COLOR_YELLOW
                canvas.drawRect(Rect(0, 0, width / 2, height), paint)

                // 右侧红色
                paint.color = COLOR_RED
                canvas.drawRect(Rect(width / 2, 0, width, height), paint)

                // 顶部黑色条
                paint.color = COLOR_BLACK
                canvas.drawRect(Rect(0, 0, width, 60), paint)

                // 底部白色条
                paint.color = COLOR_WHITE
                canvas.drawRect(Rect(0, height - 60, width, height), paint)

                // 文字
                paint.color = COLOR_WHITE
                paint.textSize = 32f
                paint.isFakeBoldText = true
                canvas.drawText("8adge 4-Color (${width}x${height})", 30f, 42f, paint)
            }
            "ALL_WHITE" -> canvas.drawColor(COLOR_WHITE)
            "ALL_BLACK" -> canvas.drawColor(COLOR_BLACK)
            "GRID_CHESS" -> {
                val numCols = if (orientation == ScreenOrientation.PORTRAIT) 12 else 16
                val numRows = if (orientation == ScreenOrientation.PORTRAIT) 16 else 12

                val tileWidth = width.toFloat() / numCols
                val tileHeight = height.toFloat() / numRows

                val paint = Paint()
                val colors = intArrayOf(COLOR_BLACK, COLOR_WHITE, COLOR_YELLOW, COLOR_RED)

                for (row in 0 until numRows) {
                    for (col in 0 until numCols) {
                        val cIdx = (col + row) % 4
                        paint.color = colors[cIdx]

                        val left = (col * tileWidth).toInt()
                        val top = (row * tileHeight).toInt()
                        val right = if (col == numCols - 1) width else ((col + 1) * tileWidth).toInt()
                        val bottom = if (row == numRows - 1) height else ((row + 1) * tileHeight).toInt()

                        canvas.drawRect(Rect(left, top, right, bottom), paint)
                    }
                }
            }
            "RAINBOW_GRADIENT" -> {
                val paint = Paint()
                val hsv = floatArrayOf(0f, 1.0f, 1.0f)
                for (y in 0 until height) {
                    // 色相从顶部 0°（红）平滑过渡到底部 280°（紫）
                    hsv[0] = (y.toFloat() / (height - 1)) * 280f
                    paint.color = Color.HSVToColor(hsv)
                    canvas.drawLine(0f, y.toFloat(), width.toFloat(), y.toFloat(), paint)
                }
            }
            else -> canvas.drawColor(COLOR_WHITE)
        }
        return bitmap
    }
}
