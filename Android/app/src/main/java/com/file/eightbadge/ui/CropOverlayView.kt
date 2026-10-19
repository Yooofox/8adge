package com.file.eightbadge.ui

import android.content.Context
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.PointF
import android.graphics.Rect
import android.graphics.RectF
import android.util.AttributeSet
import android.view.MotionEvent
import android.view.View
import kotlin.math.abs

class CropOverlayView @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null,
    defStyleAttr: Int = 0
) : View(context, attrs, defStyleAttr) {

    private var srcBitmap: Bitmap? = null

    // Exact destination bounds where the bitmap is drawn in fitCenter
    val imageDstRect = RectF()

    // Crop box in screen pixel coordinates
    val cropRect = RectF()

    var targetAspectRatio: Float = 552f / 768f // Default portrait ratio
    var isRatioLocked: Boolean = true
        set(value) {
            field = value
            if (value) {
                resetCropFrame()
            }
        }

    private val bitmapPaint = Paint(Paint.ANTI_ALIAS_FLAG or Paint.FILTER_BITMAP_FLAG)

    private val dimPaint = Paint().apply {
        color = Color.parseColor("#A6000000") // Semi-transparent overlay
        style = Paint.Style.FILL
    }

    private val framePaint = Paint().apply {
        color = Color.parseColor("#00E676") // Bright green border
        style = Paint.Style.STROKE
        strokeWidth = 4f
        isAntiAlias = true
    }

    private val handlePaint = Paint().apply {
        color = Color.parseColor("#00E676")
        style = Paint.Style.FILL
        isAntiAlias = true
    }

    private val gridPaint = Paint().apply {
        color = Color.parseColor("#80FFFFFF")
        style = Paint.Style.STROKE
        strokeWidth = 2f
        isAntiAlias = true
    }

    private enum class TouchHandle {
        NONE, TOP_LEFT, TOP_RIGHT, BOTTOM_LEFT, BOTTOM_RIGHT, CENTER
    }

    private var currentHandle = TouchHandle.NONE
    private val lastTouchPoint = PointF()
    private val handleRadius = 24f

    var onCropChangeListener: ((RectF) -> Unit)? = null

    private fun Float.coerceInSafe(minVal: Float, maxVal: Float): Float {
        if (this.isNaN()) return minOf(minVal, maxVal)
        val actualMin = minOf(minVal, maxVal)
        val actualMax = maxOf(minVal, maxVal)
        return this.coerceIn(actualMin, actualMax)
    }

    fun setImageBitmap(bitmap: Bitmap) {
        this.srcBitmap = bitmap
        updateImageBounds()
        resetCropFrame()
    }

    override fun onLayout(changed: Boolean, left: Int, top: Int, right: Int, bottom: Int) {
        super.onLayout(changed, left, top, right, bottom)
        if (changed) {
            updateImageBounds()
            resetCropFrame()
        }
    }

    private fun updateImageBounds() {
        val bitmap = srcBitmap ?: return
        val vW = width.toFloat()
        val vH = height.toFloat()
        if (vW <= 0f || vH <= 0f) return

        val bW = bitmap.width.toFloat()
        val bH = bitmap.height.toFloat()
        if (bW <= 0f || bH <= 0f) return

        val scale = minOf(vW / bW, vH / bH)
        val dstW = bW * scale
        val dstH = bH * scale

        val left = (vW - dstW) / 2f
        val top = (vH - dstH) / 2f
        imageDstRect.set(left, top, left + dstW, top + dstH)
    }

    fun resetCropFrame() {
        if (imageDstRect.isEmpty) updateImageBounds()
        if (imageDstRect.isEmpty) return

        val imgW = imageDstRect.width()
        val imgH = imageDstRect.height()
        val ratio = if (targetAspectRatio > 0f) targetAspectRatio else 1f

        var cropW = imgW
        var cropH = cropW / ratio

        if (cropH > imgH) {
            cropH = imgH
            cropW = cropH * ratio
        }

        val left = imageDstRect.left + (imgW - cropW) / 2f
        val top = imageDstRect.top + (imgH - cropH) / 2f
        cropRect.set(left, top, left + cropW, top + cropH)
        invalidate()
        notifyCropChange()
    }

    override fun onDraw(canvas: Canvas) {
        super.onDraw(canvas)

        // 1. Draw source bitmap exactly inside imageDstRect
        srcBitmap?.let { bitmap ->
            val srcRect = Rect(0, 0, bitmap.width, bitmap.height)
            canvas.drawBitmap(bitmap, srcRect, imageDstRect, bitmapPaint)
        }

        if (cropRect.isEmpty || imageDstRect.isEmpty) return

        val vW = width.toFloat()
        val vH = height.toFloat()

        // 2. Draw dim background around crop box
        canvas.drawRect(0f, 0f, vW, cropRect.top, dimPaint)
        canvas.drawRect(0f, cropRect.top, cropRect.left, cropRect.bottom, dimPaint)
        canvas.drawRect(cropRect.right, cropRect.top, vW, cropRect.bottom, dimPaint)
        canvas.drawRect(0f, cropRect.bottom, vW, vH, dimPaint)

        // 3. Draw Crop Frame & Rule of Thirds Grid
        canvas.drawRect(cropRect, framePaint)

        val thirdW = cropRect.width() / 3f
        val thirdH = cropRect.height() / 3f
        canvas.drawLine(cropRect.left + thirdW, cropRect.top, cropRect.left + thirdW, cropRect.bottom, gridPaint)
        canvas.drawLine(cropRect.left + thirdW * 2, cropRect.top, cropRect.left + thirdW * 2, cropRect.bottom, gridPaint)
        canvas.drawLine(cropRect.left, cropRect.top + thirdH, cropRect.right, cropRect.top + thirdH, gridPaint)
        canvas.drawLine(cropRect.left, cropRect.top + thirdH * 2, cropRect.right, cropRect.top + thirdH * 2, gridPaint)

        // 4. Draw Corner Handles
        canvas.drawCircle(cropRect.left, cropRect.top, handleRadius, handlePaint)
        canvas.drawCircle(cropRect.right, cropRect.top, handleRadius, handlePaint)
        canvas.drawCircle(cropRect.left, cropRect.bottom, handleRadius, handlePaint)
        canvas.drawCircle(cropRect.right, cropRect.bottom, handleRadius, handlePaint)
    }

    override fun performClick(): Boolean {
        super.performClick()
        return true
    }

    override fun onTouchEvent(event: MotionEvent): Boolean {
        val x = event.x
        val y = event.y

        when (event.action) {
            MotionEvent.ACTION_DOWN -> {
                currentHandle = detectTouchHandle(x, y)
                lastTouchPoint.set(x, y)
                if (currentHandle != TouchHandle.NONE) {
                    performClick()
                    return true
                }
                return false
            }
            MotionEvent.ACTION_MOVE -> {
                val dx = x - lastTouchPoint.x
                val dy = y - lastTouchPoint.y
                moveCropFrame(dx, dy)
                lastTouchPoint.set(x, y)
                invalidate()
                notifyCropChange()
                return true
            }
            MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                currentHandle = TouchHandle.NONE
                return true
            }
        }
        return super.onTouchEvent(event)
    }

    private fun detectTouchHandle(x: Float, y: Float): TouchHandle {
        val touchMargin = handleRadius * 2.5f
        return when {
            isNearPoint(x, y, cropRect.left, cropRect.top, touchMargin) -> TouchHandle.TOP_LEFT
            isNearPoint(x, y, cropRect.right, cropRect.top, touchMargin) -> TouchHandle.TOP_RIGHT
            isNearPoint(x, y, cropRect.left, cropRect.bottom, touchMargin) -> TouchHandle.BOTTOM_LEFT
            isNearPoint(x, y, cropRect.right, cropRect.bottom, touchMargin) -> TouchHandle.BOTTOM_RIGHT
            cropRect.contains(x, y) -> TouchHandle.CENTER
            else -> TouchHandle.NONE
        }
    }

    private fun isNearPoint(x: Float, y: Float, px: Float, py: Float, margin: Float): Boolean {
        return abs(x - px) <= margin && abs(y - py) <= margin
    }

    private fun moveCropFrame(dx: Float, dy: Float) {
        val minSize = 60f
        val boundL = imageDstRect.left
        val boundT = imageDstRect.top
        val boundR = imageDstRect.right
        val boundB = imageDstRect.bottom

        if (boundR <= boundL || boundB <= boundT) return
        val ratio = if (targetAspectRatio > 0f) targetAspectRatio else 1f

        when (currentHandle) {
            TouchHandle.CENTER -> {
                val maxL = (boundR - cropRect.width()).coerceInSafe(boundL, boundR)
                val maxT = (boundB - cropRect.height()).coerceInSafe(boundT, boundB)
                val newLeft = (cropRect.left + dx).coerceInSafe(boundL, maxL)
                val newTop = (cropRect.top + dy).coerceInSafe(boundT, maxT)
                cropRect.offsetTo(newLeft, newTop)
            }
            TouchHandle.BOTTOM_RIGHT -> {
                var newRight = (cropRect.right + dx).coerceInSafe(cropRect.left + minSize, boundR)
                var newBottom = if (isRatioLocked) {
                    cropRect.top + (newRight - cropRect.left) / ratio
                } else {
                    (cropRect.bottom + dy).coerceInSafe(cropRect.top + minSize, boundB)
                }

                if (newBottom > boundB) {
                    newBottom = boundB
                    if (isRatioLocked) {
                        newRight = (cropRect.left + (newBottom - cropRect.top) * ratio).coerceInSafe(cropRect.left + minSize, boundR)
                    }
                }
                cropRect.set(cropRect.left, cropRect.top, newRight, newBottom)
            }
            TouchHandle.BOTTOM_LEFT -> {
                var newLeft = (cropRect.left + dx).coerceInSafe(boundL, cropRect.right - minSize)
                var newBottom = if (isRatioLocked) {
                    cropRect.top + (cropRect.right - newLeft) / ratio
                } else {
                    (cropRect.bottom + dy).coerceInSafe(cropRect.top + minSize, boundB)
                }

                if (newBottom > boundB) {
                    newBottom = boundB
                    if (isRatioLocked) {
                        newLeft = (cropRect.right - (newBottom - cropRect.top) * ratio).coerceInSafe(boundL, cropRect.right - minSize)
                    }
                }
                cropRect.set(newLeft, cropRect.top, cropRect.right, newBottom)
            }
            TouchHandle.TOP_RIGHT -> {
                var newRight = (cropRect.right + dx).coerceInSafe(cropRect.left + minSize, boundR)
                var newTop = if (isRatioLocked) {
                    cropRect.bottom - (newRight - cropRect.left) / ratio
                } else {
                    (cropRect.top + dy).coerceInSafe(boundT, cropRect.bottom - minSize)
                }

                if (newTop < boundT) {
                    newTop = boundT
                    if (isRatioLocked) {
                        newRight = (cropRect.left + (cropRect.bottom - newTop) * ratio).coerceInSafe(cropRect.left + minSize, boundR)
                    }
                }
                cropRect.set(cropRect.left, newTop, newRight, cropRect.bottom)
            }
            TouchHandle.TOP_LEFT -> {
                var newLeft = (cropRect.left + dx).coerceInSafe(boundL, cropRect.right - minSize)
                var newTop = if (isRatioLocked) {
                    cropRect.bottom - (cropRect.right - newLeft) / ratio
                } else {
                    (cropRect.top + dy).coerceInSafe(boundT, cropRect.bottom - minSize)
                }

                if (newTop < boundT) {
                    newTop = boundT
                    if (isRatioLocked) {
                        newLeft = (cropRect.right - (cropRect.bottom - newTop) * ratio).coerceInSafe(boundL, cropRect.right - minSize)
                    }
                }
                cropRect.set(newLeft, newTop, cropRect.right, cropRect.bottom)
            }
            else -> {}
        }
    }

    /**
     * Returns normalized crop bounds STRICTLY relative to the rendered bitmap inside imageDstRect!
     */
    fun getNormalizedCropRect(): RectF {
        if (imageDstRect.width() <= 0f || imageDstRect.height() <= 0f) {
            return RectF(0f, 0f, 1f, 1f)
        }

        val normLeft = ((cropRect.left - imageDstRect.left) / imageDstRect.width()).coerceInSafe(0f, 1f)
        val normTop = ((cropRect.top - imageDstRect.top) / imageDstRect.height()).coerceInSafe(0f, 1f)
        val normRight = ((cropRect.right - imageDstRect.left) / imageDstRect.width()).coerceInSafe(0f, 1f)
        val normBottom = ((cropRect.bottom - imageDstRect.top) / imageDstRect.height()).coerceInSafe(0f, 1f)

        return RectF(normLeft, normTop, normRight, normBottom)
    }

    private fun notifyCropChange() {
        onCropChangeListener?.invoke(getNormalizedCropRect())
    }
}
