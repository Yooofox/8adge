package com.file.eightbadge.ui

import android.app.Application
import android.graphics.Bitmap
import android.graphics.RectF
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import com.file.eightbadge.ble.BleManager
import com.file.eightbadge.ble.BleProtocol
import com.file.eightbadge.image.ImageProcessor
import com.file.eightbadge.model.BleDeviceItem
import com.file.eightbadge.model.DeviceInfo
import com.file.eightbadge.model.DeviceState
import com.file.eightbadge.model.DitherMode
import com.file.eightbadge.model.ImageFormat
import com.file.eightbadge.model.LogMessage
import com.file.eightbadge.model.LogType
import com.file.eightbadge.model.RefreshBlockedEvent
import com.file.eightbadge.model.ScaleMode
import com.file.eightbadge.model.ScreenOrientation
import com.file.eightbadge.model.TransferProgress
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharedFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asSharedFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

class MainViewModel(application: Application) : AndroidViewModel(application) {

    val bleManager = BleManager(application)

    // Ble State delegate
    val scannedDevices: StateFlow<List<BleDeviceItem>> = bleManager.scannedDevices
    val isScanning: StateFlow<Boolean> = bleManager.isScanning
    val connectionState: StateFlow<DeviceState> = bleManager.connectionState
    val connectedDeviceName: StateFlow<String?> = bleManager.connectedDeviceName
    val deviceInfo: StateFlow<DeviceInfo> = bleManager.deviceInfo
    val transferProgress: StateFlow<TransferProgress> = bleManager.transferProgress
    val refreshBlockedEvent: SharedFlow<RefreshBlockedEvent> = bleManager.refreshBlockedFlow

    // Logs
    val logs: StateFlow<List<LogMessage>> = bleManager.logsFlow

    // Image Studio State
    private val _sourceBitmap = MutableStateFlow<Bitmap?>(null)
    val sourceBitmap: StateFlow<Bitmap?> = _sourceBitmap.asStateFlow()

    private val _processedResult = MutableStateFlow<ImageProcessor.ProcessResult?>(null)
    val processedResult: StateFlow<ImageProcessor.ProcessResult?> = _processedResult.asStateFlow()

    private val _isProcessing = MutableStateFlow(false)
    val isProcessing: StateFlow<Boolean> = _isProcessing.asStateFlow()

    // Processing Options
    val screenOrientation = MutableStateFlow(ScreenOrientation.PORTRAIT) // Default Portrait (552x768)
    val imageFormat = MutableStateFlow(ImageFormat.BPP2)
    val scaleMode = MutableStateFlow(ScaleMode.CROP_FILL)
    val ditherMode = MutableStateFlow(DitherMode.FLOYD_STEINBERG)
    val rotationDegrees = MutableStateFlow(0f)
    val cropRect = MutableStateFlow<RectF?>(null)

    // Color & Detail Adjustments
    val brightness = MutableStateFlow(0f)
    val contrast = MutableStateFlow(1.0f)
    val saturation = MutableStateFlow(1.0f)
    val gamma = MutableStateFlow(1.0f)
    val sharpen = MutableStateFlow(0f)
    val invertColors = MutableStateFlow(false)

    init {
        // Load default test pattern on startup
        loadTestPattern("YELLOW_RED_TEST")
    }

    fun startScan() {
        bleManager.startScan()
    }

    fun stopScan() {
        bleManager.stopScan()
    }

    fun connectDevice(item: BleDeviceItem) {
        bleManager.connect(item)
    }

    fun disconnectDevice() {
        bleManager.disconnect()
    }

    private var imageProcessJob: Job? = null
    private var lastPendingPayload: ByteArray? = null
    private var lastPendingFormat: ImageFormat? = null
    private var lastPendingCrc32: Long? = null

    fun confirmAndRetryRefresh(token: Byte, forOpcode: Byte) {
        viewModelScope.launch {
            val success = bleManager.confirmRefresh(token)
            if (success) {
                when (forOpcode) {
                    BleProtocol.OP_BEGIN_IMAGE -> {
                        val payload = lastPendingPayload
                        val format = lastPendingFormat
                        val crc = lastPendingCrc32
                        if (payload != null && format != null && crc != null) {
                            bleManager.sendImage(payload, format, crc, skipInitialAbort = true)
                        } else {
                            sendCurrentImageToEpaper(skipInitialAbort = true)
                        }
                    }
                    BleProtocol.OP_SHOW_HINT -> sendShowHint()
                }
            }
        }
    }

    fun setSourceBitmap(bitmap: Bitmap) {
        _sourceBitmap.value = bitmap
        rotationDegrees.value = 0f
        cropRect.value = null

        // Auto detect orientation based on image aspect ratio
        if (bitmap.width > bitmap.height) {
            screenOrientation.value = ScreenOrientation.LANDSCAPE
        } else {
            screenOrientation.value = ScreenOrientation.PORTRAIT
        }

        processCurrentImage()
    }

    fun loadTestPattern(patternType: String) {
        val testBitmap = ImageProcessor.generateTestPattern(patternType, screenOrientation.value)
        _sourceBitmap.value = testBitmap
        rotationDegrees.value = 0f
        cropRect.value = null
        processCurrentImage()
    }

    fun loadAndSendTestPattern(patternType: String) {
        // Reset color adjustments to defaults
        brightness.value = 0f
        contrast.value = 1.0f
        saturation.value = 1.0f
        gamma.value = 1.0f
        sharpen.value = 0f
        invertColors.value = false
        rotationDegrees.value = 0f
        cropRect.value = null

        // Set DitherMode to NEAREST ("纯色无抖动") for pure solid color test patterns
        ditherMode.value = DitherMode.NEAREST

        val testBitmap = ImageProcessor.generateTestPattern(patternType, screenOrientation.value)
        _sourceBitmap.value = testBitmap
        _isProcessing.value = true

        imageProcessJob?.cancel()
        imageProcessJob = viewModelScope.launch(Dispatchers.Default) {
            val result = ImageProcessor.processImage(
                srcBitmap = testBitmap,
                format = imageFormat.value,
                orientation = screenOrientation.value,
                scaleMode = scaleMode.value,
                ditherMode = DitherMode.NEAREST,
                rotationDegrees = 0f,
                cropRect = null,
                brightness = 0f,
                contrast = 1.0f,
                saturation = 1.0f,
                gamma = 1.0f,
                sharpen = 0f,
                invertColors = false
            )
            withContext(Dispatchers.Main) {
                _processedResult.value = result
                _isProcessing.value = false

                lastPendingPayload = result.rawPayload
                lastPendingFormat = result.format
                lastPendingCrc32 = result.crc32

                // Immediately send test pattern to e-paper
                bleManager.sendImage(result.rawPayload, result.format, result.crc32)
            }
        }
    }

    fun rotateLeft() {
        val newAngle = (rotationDegrees.value - 90f + 360f) % 360f
        rotationDegrees.value = newAngle
        processCurrentImage()
    }

    fun rotateRight() {
        val newAngle = (rotationDegrees.value + 90f) % 360f
        rotationDegrees.value = newAngle
        processCurrentImage()
    }

    fun setCropBounds(rectF: RectF?) {
        cropRect.value = rectF
        processCurrentImage()
    }

    fun resetCrop() {
        cropRect.value = null
        processCurrentImage()
    }

    fun resetAdjustments() {
        brightness.value = 0f
        contrast.value = 1.0f
        saturation.value = 1.0f
        gamma.value = 1.0f
        sharpen.value = 0f
        invertColors.value = false
        processCurrentImage()
    }

    @Volatile
    private var hasPendingProcessRequest = false

    fun processCurrentImage() {
        if (_sourceBitmap.value == null) return

        synchronized(this) {
            hasPendingProcessRequest = true
            if (imageProcessJob?.isActive == true) {
                return
            }

            imageProcessJob = viewModelScope.launch(Dispatchers.Default) {
                _isProcessing.value = true
                while (hasPendingProcessRequest) {
                    hasPendingProcessRequest = false
                    val currentSrc = _sourceBitmap.value ?: break

                    val result = ImageProcessor.processImage(
                        srcBitmap = currentSrc,
                        format = imageFormat.value,
                        orientation = screenOrientation.value,
                        scaleMode = scaleMode.value,
                        ditherMode = ditherMode.value,
                        rotationDegrees = rotationDegrees.value,
                        cropRect = cropRect.value,
                        brightness = brightness.value,
                        contrast = contrast.value,
                        saturation = saturation.value,
                        gamma = gamma.value,
                        sharpen = sharpen.value,
                        invertColors = invertColors.value
                    )

                    withContext(Dispatchers.Main) {
                        _processedResult.value = result
                    }
                }
                withContext(Dispatchers.Main) {
                    _isProcessing.value = false
                }
            }
        }
    }

    private val _toastMessageFlow = MutableSharedFlow<String>(extraBufferCapacity = 10)
    val toastMessageFlow: SharedFlow<String> = _toastMessageFlow.asSharedFlow()

    fun sendCurrentImageToEpaper(skipInitialAbort: Boolean = false) {
        if (connectionState.value == DeviceState.IDLE) {
            viewModelScope.launch {
                _toastMessageFlow.emit("请先点击右上角【搜索设备】连接 8adge 墨水屏")
            }
            bleManager.log("发送中断: 墨水屏未连接，请先搜索设备", LogType.ERROR)
            return
        }

        val result = _processedResult.value ?: run {
            viewModelScope.launch {
                _toastMessageFlow.emit("正在生成图像点阵数据，请稍等...")
            }
            bleManager.log("发送中断: 图像点阵数据正在后台计算中", LogType.WARNING)
            return
        }

        lastPendingPayload = result.rawPayload
        lastPendingFormat = result.format
        lastPendingCrc32 = result.crc32

        bleManager.sendImage(result.rawPayload, result.format, result.crc32, skipInitialAbort)
    }

    fun sendGetInfo() {
        viewModelScope.launch { bleManager.getInfo() }
    }

    fun sendShowHint() {
        viewModelScope.launch { bleManager.showHint() }
    }

    fun sendPing() {
        viewModelScope.launch { bleManager.ping() }
    }

    fun sendAbort() {
        viewModelScope.launch { bleManager.abort() }
    }

    fun clearLogs() {
        bleManager.clearLogsSnapshot()
    }

    fun getLogsSnapshot(): List<LogMessage> {
        return bleManager.getLogsSnapshot()
    }

    fun getFormattedLogsText(): String {
        val list = getLogsSnapshot()
        if (list.isEmpty()) return "暂无日志"
        val sb = StringBuilder()
        sb.append("=== 8adge 设备控制台日志导出 ===\n")
        sb.append("导出时间: ${java.text.SimpleDateFormat("yyyy-MM-dd HH:mm:ss", java.util.Locale.getDefault()).format(java.util.Date())}\n")
        sb.append("设备名称: ${connectedDeviceName.value ?: "未连接"}\n")
        sb.append("----------------------------------------\n")
        for (log in list) {
            sb.append("[${log.timestamp}] [${log.type}] ${log.message}\n")
        }
        return sb.toString()
    }

    override fun onCleared() {
        super.onCleared()
        bleManager.disconnect()
    }
}
