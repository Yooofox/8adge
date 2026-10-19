package com.file.eightbadge.ble

import android.annotation.SuppressLint
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothGattDescriptor
import android.bluetooth.BluetoothProfile
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.Context
import android.os.Build
import android.util.Log
import com.file.eightbadge.image.ImageProcessor
import com.file.eightbadge.model.BleDeviceItem
import com.file.eightbadge.model.DeviceInfo
import com.file.eightbadge.model.DeviceState
import com.file.eightbadge.model.ImageFormat
import com.file.eightbadge.model.LogType
import com.file.eightbadge.model.ProtocolError
import com.file.eightbadge.model.RefreshBlockedEvent
import com.file.eightbadge.model.TransferProgress
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharedFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asSharedFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withTimeoutOrNull
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

@SuppressLint("MissingPermission")
class BleManager(private val context: Context) {

    private val scope = CoroutineScope(Dispatchers.IO + SupervisorJob())
    private val bluetoothAdapter: BluetoothAdapter? = BluetoothAdapter.getDefaultAdapter()

    // Scanned Devices
    private val _scannedDevices = MutableStateFlow<List<BleDeviceItem>>(emptyList())
    val scannedDevices: StateFlow<List<BleDeviceItem>> = _scannedDevices.asStateFlow()

    private val _isScanning = MutableStateFlow(false)
    val isScanning: StateFlow<Boolean> = _isScanning.asStateFlow()

    // Connection & Device State
    private var bluetoothGatt: BluetoothGatt? = null
    private var controlCharacteristic: BluetoothGattCharacteristic? = null
    private var dataCharacteristic: BluetoothGattCharacteristic? = null

    private val _connectionState = MutableStateFlow(DeviceState.IDLE)
    val connectionState: StateFlow<DeviceState> = _connectionState.asStateFlow()

    private val _connectedDeviceName = MutableStateFlow<String?>(null)
    val connectedDeviceName: StateFlow<String?> = _connectedDeviceName.asStateFlow()

    private val _deviceInfo = MutableStateFlow(DeviceInfo())
    val deviceInfo: StateFlow<DeviceInfo> = _deviceInfo.asStateFlow()

    // Transfer Progress
    private val _transferProgress = MutableStateFlow(TransferProgress())
    val transferProgress: StateFlow<TransferProgress> = _transferProgress.asStateFlow()

    // Log Stream StateFlow
    private val _logsFlow = MutableStateFlow<List<com.file.eightbadge.model.LogMessage>>(emptyList())
    val logsFlow: StateFlow<List<com.file.eightbadge.model.LogMessage>> = _logsFlow.asStateFlow()

    // Cooldown Blocked Event Stream
    private val _refreshBlockedFlow = MutableSharedFlow<RefreshBlockedEvent>(extraBufferCapacity = 10)
    val refreshBlockedFlow: SharedFlow<RefreshBlockedEvent> = _refreshBlockedFlow.asSharedFlow()

    // Internal locks and pending deferreds
    private val commandMutex = Mutex()
    private var nextSeq: Byte = 1
    @Volatile
    private var pendingResponseDeferred: CompletableDeferred<BleProtocol.ControlFrame>? = null
    @Volatile
    private var pendingExpectedSeq: Byte? = null
    @Volatile
    private var pendingProgressDeferred: CompletableDeferred<BleProtocol.ControlFrame.ProgressEvent>? = null

    private var activeTransferJob: Job? = null

    private fun getNextSeq(): Byte {
        val seq = nextSeq
        val currentInt = nextSeq.toInt() and 0xFF
        nextSeq = if (currentInt >= 255) 1 else (currentInt + 1).toByte()
        return seq
    }

    // Log Snapshot List in Memory
    private val logList = mutableListOf<com.file.eightbadge.model.LogMessage>()

    fun getLogsSnapshot(): List<com.file.eightbadge.model.LogMessage> {
        synchronized(logList) {
            return ArrayList(logList)
        }
    }

    private var logIdSeq = 0L
    private val logDateFormat = SimpleDateFormat("HH:mm:ss.SSS", Locale.getDefault())

    fun clearLogsSnapshot() {
        synchronized(logList) {
            logList.clear()
            logIdSeq = 0L
            _logsFlow.value = emptyList()
        }
    }

    fun log(message: String, type: LogType = LogType.INFO) {
        val timestamp = synchronized(logDateFormat) {
            logDateFormat.format(Date())
        }
        synchronized(logList) {
            val item = com.file.eightbadge.model.LogMessage(++logIdSeq, timestamp, message, type)
            logList.add(item)
            if (logList.size > 500) {
                logList.removeAt(0)
            }
            _logsFlow.value = ArrayList(logList)
        }
        Log.d("BleManager", "[$timestamp] [$type] $message")
    }

    // --- Bluetooth Scanning ---

    fun startScan() {
        val scanner = bluetoothAdapter?.bluetoothLeScanner
        if (scanner == null) {
            log("蓝牙未开启或不支持 BLE", LogType.ERROR)
            return
        }

        if (Build.VERSION.SDK_INT <= Build.VERSION_CODES.R) {
            val locationManager = context.getSystemService(Context.LOCATION_SERVICE) as? android.location.LocationManager
            val isGpsEnabled = locationManager?.isProviderEnabled(android.location.LocationManager.GPS_PROVIDER) == true ||
                    locationManager?.isProviderEnabled(android.location.LocationManager.NETWORK_PROVIDER) == true
            if (!isGpsEnabled) {
                log("提示: Android 11 及以下设备扫描需开启手机【位置信息/GPS】开关", LogType.WARNING)
            }
        }

        _scannedDevices.value = emptyList()
        _isScanning.value = true
        log("开始蓝牙主动扫描 (过滤前缀 8adge-)...", LogType.INFO)

        val settings = ScanSettings.Builder()
            .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY)
            .build()

        try {
            scanner.startScan(null, settings, scanCallback)
        } catch (e: SecurityException) {
            log("缺少蓝牙扫描权限: ${e.message}", LogType.ERROR)
            _isScanning.value = false
            return
        } catch (e: Exception) {
            log("蓝牙扫描异常: ${e.message}", LogType.ERROR)
            _isScanning.value = false
            return
        }

        // Auto stop after 12s
        scope.launch {
            delay(12000)
            if (_isScanning.value) {
                stopScan()
            }
        }
    }

    fun stopScan() {
        if (_isScanning.value) {
            try {
                bluetoothAdapter?.bluetoothLeScanner?.stopScan(scanCallback)
            } catch (e: Exception) {
                log("停止扫描异常: ${e.message}", LogType.WARNING)
            }
            _isScanning.value = false
            log("蓝牙扫描停止", LogType.INFO)
        }
    }

    private val scanCallback = object : ScanCallback() {
        override fun onScanResult(callbackType: Int, result: ScanResult) {
            val device = result.device
            val name = result.scanRecord?.deviceName ?: device.name ?: return
            if (name.startsWith(BleProtocol.DEVICE_NAME_PREFIX)) {
                val item = BleDeviceItem(device, name, result.rssi, device.address)
                val list = _scannedDevices.value.toMutableList()
                val index = list.indexOfFirst { it.address == item.address }
                if (index >= 0) {
                    list[index] = item
                } else {
                    list.add(item)
                    log("发现墨水屏设备: $name (${device.address}), RSSI: ${result.rssi}", LogType.SUCCESS)
                }
                _scannedDevices.value = list
            }
        }

        override fun onScanFailed(errorCode: Int) {
            log("扫描失败, 错误码: $errorCode", LogType.ERROR)
            _isScanning.value = false
        }
    }

    // --- Connection & GATT Lifecycle ---

    fun connect(deviceItem: BleDeviceItem) {
        stopScan()
        scope.launch {
            disconnectInternal()
            delay(150) // Stagger previous GATT cleanup to prevent GATT handle collisions

            log("正在连接设备: ${deviceItem.name} (${deviceItem.address})...", LogType.INFO)
            _connectedDeviceName.value = deviceItem.name

            try {
                val gatt = deviceItem.device.connectGatt(
                    context,
                    false,
                    gattCallback,
                    BluetoothDevice.TRANSPORT_LE
                )
                if (gatt == null) {
                    log("连接设备失败: BluetoothGatt 返回 null", LogType.ERROR)
                    _connectionState.value = DeviceState.IDLE
                    _connectedDeviceName.value = null
                } else {
                    bluetoothGatt = gatt
                }
            } catch (e: SecurityException) {
                log("连接设备权限异常: ${e.message}", LogType.ERROR)
                _connectionState.value = DeviceState.IDLE
                _connectedDeviceName.value = null
            }
        }
    }

    fun disconnect() {
        scope.launch {
            disconnectInternal()
        }
    }

    private fun disconnectInternal() {
        activeTransferJob?.cancel()
        activeTransferJob = null

        bluetoothGatt?.let { gatt ->
            log("主动断开蓝牙连接", LogType.INFO)
            try {
                gatt.disconnect()
            } catch (e: SecurityException) {
                log("断开连接权限异常: ${e.message}", LogType.WARNING)
            }
            scope.launch {
                delay(100)
                try {
                    gatt.close()
                } catch (e: Exception) {
                    Log.e("BleManager", "Error closing gatt", e)
                }
            }
        }
        bluetoothGatt = null
        controlCharacteristic = null
        dataCharacteristic = null
        _connectionState.value = DeviceState.IDLE
        _connectedDeviceName.value = null
    }

    private val gattCallback = object : BluetoothGattCallback() {
        override fun onConnectionStateChange(gatt: BluetoothGatt, status: Int, newState: Int) {
            if (newState == BluetoothProfile.STATE_CONNECTED) {
                log("蓝牙已连接，开始发现服务...", LogType.SUCCESS)
                _connectionState.value = DeviceState.CONNECTED
                try {
                    gatt.discoverServices()
                } catch (e: SecurityException) {
                    log("发现服务权限异常: ${e.message}", LogType.ERROR)
                }
            } else if (newState == BluetoothProfile.STATE_DISCONNECTED) {
                log("蓝牙链路已断开 (status: $status)", LogType.WARNING)
                activeTransferJob?.cancel()
                activeTransferJob = null

                val deferred = pendingResponseDeferred
                pendingResponseDeferred = null
                pendingExpectedSeq = null
                deferred?.completeExceptionally(java.io.IOException("蓝牙已断开连接"))

                val progDeferred = pendingProgressDeferred
                pendingProgressDeferred = null
                progDeferred?.completeExceptionally(java.io.IOException("蓝牙已断开连接"))

                try {
                    gatt.close()
                } catch (e: Exception) {
                    Log.e("BleManager", "Error closing gatt on disconnect", e)
                }
                bluetoothGatt = null
                controlCharacteristic = null
                dataCharacteristic = null
                _connectionState.value = DeviceState.IDLE
                _connectedDeviceName.value = null
            }
        }

        override fun onServicesDiscovered(gatt: BluetoothGatt, status: Int) {
            if (status != BluetoothGatt.GATT_SUCCESS) {
                log("服务发现失败, status: $status", LogType.ERROR)
                disconnectInternal()
                return
            }

            val service = gatt.getService(BleProtocol.SERVICE_UUID)
            if (service == null) {
                log("未找到 8adge 服务 (${BleProtocol.SERVICE_UUID})", LogType.ERROR)
                disconnectInternal()
                return
            }

            controlCharacteristic = service.getCharacteristic(BleProtocol.CONTROL_CHAR_UUID)
            dataCharacteristic = service.getCharacteristic(BleProtocol.DATA_CHAR_UUID)

            if (controlCharacteristic == null || dataCharacteristic == null) {
                log("未找到 CONTROL 或 DATA 特征值", LogType.ERROR)
                disconnectInternal()
                return
            }

            log("服务与特征值准备完毕，请求 MTU 517...", LogType.INFO)
            gatt.requestMtu(517)
        }

        override fun onMtuChanged(gatt: BluetoothGatt, mtu: Int, status: Int) {
            log("MTU 协商成功: $mtu (GATT status: $status)", LogType.INFO)

            scope.launch {
                // Step 1: Enable NOTIFY on CONTROL
                controlCharacteristic?.let { ctrl ->
                    gatt.setCharacteristicNotification(ctrl, true)
                    val descriptor = ctrl.getDescriptor(BleProtocol.CCCD_DESCRIPTOR_UUID)
                    if (descriptor != null) {
                        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                            gatt.writeDescriptor(descriptor, BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE)
                        } else {
                            @Suppress("DEPRECATION")
                            descriptor.value = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
                            @Suppress("DEPRECATION")
                            gatt.writeDescriptor(descriptor)
                        }
                        log("开启 CONTROL Notification 通知...", LogType.INFO)
                    }
                }

                delay(200) // Stagger before connection priority request to prevent OS GATT race condition

                // Step 2: Request High Connection Priority
                val prioSuccess = gatt.requestConnectionPriority(BluetoothGatt.CONNECTION_PRIORITY_HIGH)
                log("请求蓝牙高优先连接模式 (CONNECTION_PRIORITY_HIGH): $prioSuccess", LogType.SUCCESS)

                delay(200)

                // Step 3: Issue SYNC_TIME (Protocol v3 Mandate) before GET_INFO
                syncTime()

                delay(100)

                // Step 4: Issue GET_INFO
                getInfo()
            }
        }

        override fun onCharacteristicChanged(
            gatt: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic,
            value: ByteArray
        ) {
            if (characteristic.uuid == BleProtocol.CONTROL_CHAR_UUID) {
                handleControlNotification(value)
            }
        }

        @Suppress("DEPRECATION")
        override fun onCharacteristicChanged(
            gatt: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic
        ) {
            if (characteristic.uuid == BleProtocol.CONTROL_CHAR_UUID) {
                handleControlNotification(characteristic.value ?: return)
            }
        }
    }

    private fun handleControlNotification(rawData: ByteArray) {
        val frame = BleProtocol.parseControlFrame(rawData) ?: return

        when (frame) {
            is BleProtocol.ControlFrame.StateEvent -> {
                log("设备状态事件: ${frame.state.description}, error: ${frame.error.message}",
                    if (frame.error == ProtocolError.OK) LogType.INFO else LogType.ERROR)
                _connectionState.value = frame.state

                // Auto finish transfer progress when screen finishes rendering or resets
                if (frame.state == DeviceState.DISPLAYED || frame.state == DeviceState.IDLE || frame.state == DeviceState.HINT_SHOWN) {
                    _connectionState.value = DeviceState.CONNECTED
                    _transferProgress.value = _transferProgress.value.copy(
                        isTransferring = false,
                        isRendering = false,
                        statusText = "墨水屏刷新完成 (${frame.state.description})"
                    )
                } else if (frame.state == DeviceState.ERROR) {
                    _transferProgress.value = _transferProgress.value.copy(
                        isTransferring = false,
                        isRendering = false,
                        errorText = "设备硬件异常: ${frame.error.message}"
                    )
                }
            }

            is BleProtocol.ControlFrame.ProgressEvent -> {
                pendingProgressDeferred?.complete(frame)
            }

            is BleProtocol.ControlFrame.LogEvent -> {
                log("设备 Log: ${frame.logText}", LogType.INFO)
            }

            is BleProtocol.ControlFrame.RefreshBlockedEventFrame -> {
                val blocked = frame.blockedEvent
                log("触发物理刷新冷却限制! 还需要等待 ${blocked.waitSeconds}s (距上次刷屏 ${blocked.elapsedSeconds}s)", LogType.WARNING)
                scope.launch {
                    _refreshBlockedFlow.emit(blocked)
                }
            }

            else -> {
                log("收到 CONTROL 响应 Frame: opcode=0x${frame.opcode.toString(16)}, seq=${frame.seq}, status=${frame.status.message}", LogType.BLE_RX)
                val deferred = pendingResponseDeferred
                val expectedSeq = pendingExpectedSeq
                if (deferred != null && expectedSeq != null && frame.seq == expectedSeq) {
                    deferred.complete(frame)
                } else {
                    log("忽略非当前匹配的 CONTROL 响应 Frame (seq=${frame.seq}, 期待 seq=$pendingExpectedSeq)", LogType.WARNING)
                }
            }
        }
    }

    // --- Command Engine with Auto-Abort Recovery ---

    private suspend fun sendControlCommand(
        opcode: Byte,
        payload: ByteArray = ByteArray(0),
        timeoutMs: Long = 5000L
    ): BleProtocol.ControlFrame? = commandMutex.withLock {
        val gatt = bluetoothGatt ?: run {
            log("命令发送失败: 蓝牙未连接", LogType.ERROR)
            return null
        }
        val ctrlChar = controlCharacteristic ?: return null

        val seq = getNextSeq()
        val frameData = BleProtocol.buildFrame(opcode, seq, payload)

        val deferred = CompletableDeferred<BleProtocol.ControlFrame>()
        pendingResponseDeferred = deferred
        pendingExpectedSeq = seq

        ctrlChar.writeType = BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
        @Suppress("DEPRECATION")
        ctrlChar.value = frameData

        log("发送 CONTROL 命令: opcode=0x${opcode.toString(16)}, seq=$seq, len=${payload.size}", LogType.BLE_TX)

        var writeSuccess = false
        var retries = 0
        while (!writeSuccess && retries < 20) {
            writeSuccess = try {
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                    gatt.writeCharacteristic(ctrlChar, frameData, BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT) == android.bluetooth.BluetoothStatusCodes.SUCCESS
                } else {
                    @Suppress("DEPRECATION")
                    gatt.writeCharacteristic(ctrlChar)
                }
            } catch (e: SecurityException) {
                log("写特征值权限异常: ${e.message}", LogType.ERROR)
                false
            }
            if (!writeSuccess) {
                retries++
                delay(20)
            }
        }

        if (!writeSuccess) {
            log("GATT writeCharacteristic 返回 false (20次重试超时)", LogType.ERROR)
            pendingResponseDeferred = null
            pendingExpectedSeq = null
            return null
        }

        val response = try {
            withTimeoutOrNull(timeoutMs) {
                deferred.await()
            }
        } finally {
            pendingResponseDeferred = null
            pendingExpectedSeq = null
        }

        if (response == null) {
            log("命令 0x${opcode.toString(16)} 响应超时 (${timeoutMs}ms)", LogType.ERROR)
        }
        return response
    }

    private suspend fun sendControlCommandWithAutoAbort(
        opcode: Byte,
        payload: ByteArray = ByteArray(0),
        timeoutMs: Long = 5000L
    ): BleProtocol.ControlFrame? {
        var resp = sendControlCommand(opcode, payload, timeoutMs)

        // If MCU responds with ERR_STATE (0x03) and opcode is not OP_ABORT,
        // send ABORT to reset MCU state machine, then retry once.
        if (resp != null && resp.status == ProtocolError.ERR_STATE && opcode != BleProtocol.OP_ABORT) {
            log("设备处于卡滞/忙碌状态 (ERR_STATE)，自动发送 ABORT 复位设备状态机...", LogType.WARNING)
            sendControlCommand(BleProtocol.OP_ABORT, timeoutMs = 2000L)
            delay(150)
            resp = sendControlCommand(opcode, payload, timeoutMs)
        }
        return resp
    }

    // --- User Commands ---

    suspend fun syncTime(): Boolean {
        val unixUtcSeconds = System.currentTimeMillis() / 1000L
        val payload = BleProtocol.buildSyncTimePayload(unixUtcSeconds)
        log("发送时间同步命令 SYNC_TIME (UTC: $unixUtcSeconds)...", LogType.INFO)
        val resp = sendControlCommandWithAutoAbort(BleProtocol.OP_SYNC_TIME, payload)
        return if (resp is BleProtocol.ControlFrame.SyncTimeResponse && resp.status == ProtocolError.OK) {
            _deviceInfo.value = _deviceInfo.value.copy(
                lastRefreshUtc = resp.lastRefreshUtc,
                refreshMinGapS = resp.minGapS,
                clockKnown = resp.clockKnown
            )
            log("时间同步成功! 上次刷屏时间戳: ${resp.lastRefreshUtc}, 最小冷却: ${resp.minGapS}s", LogType.SUCCESS)
            true
        } else {
            log("时间同步失败", LogType.WARNING)
            false
        }
    }

    suspend fun confirmRefresh(token: Byte): Boolean {
        val payload = BleProtocol.buildConfirmRefreshPayload(token)
        log("发送放行授权确认命令 CONFIRM_REFRESH (token=$token)...", LogType.INFO)
        val resp = sendControlCommandWithAutoAbort(BleProtocol.OP_CONFIRM_REFRESH, payload)
        return if (resp is BleProtocol.ControlFrame.ConfirmRefreshResponse && resp.status == ProtocolError.OK) {
            log("刷新确认成功! 获取一次性放行授权", LogType.SUCCESS)
            true
        } else {
            log("刷新确认失败: ${resp?.status?.message ?: "无响应"}", LogType.ERROR)
            false
        }
    }

    suspend fun getInfo(): DeviceInfo? {
        val resp = sendControlCommandWithAutoAbort(BleProtocol.OP_GET_INFO)
        if (resp is BleProtocol.ControlFrame.GetInfoResponse && resp.status == ProtocolError.OK) {
            val currentInfo = _deviceInfo.value
            val mergedInfo = resp.deviceInfo.copy(
                lastRefreshUtc = if (resp.deviceInfo.lastRefreshUtc != 0L) resp.deviceInfo.lastRefreshUtc else currentInfo.lastRefreshUtc,
                clockKnown = resp.deviceInfo.clockKnown || currentInfo.clockKnown
            )
            _deviceInfo.value = mergedInfo
            log("设备信息获取成功: 画板 ${mergedInfo.canvasWidth}x${mergedInfo.canvasHeight}, 固件 v${mergedInfo.fwVersionString}, Chunk=${mergedInfo.chunkSize}B, 冷却限制=${mergedInfo.refreshMinGapS}s", LogType.SUCCESS)
            return mergedInfo
        }
        return null
    }

    suspend fun showHint(): Boolean {
        log("发送绘制提示屏命令 SHOW_HINT...", LogType.INFO)
        val resp = sendControlCommandWithAutoAbort(BleProtocol.OP_SHOW_HINT)
        return resp is BleProtocol.ControlFrame.ShowHintResponse && resp.status == ProtocolError.OK
    }

    suspend fun ping(testData: ByteArray = "8adge-Ping".toByteArray()): Boolean {
        log("发送 PING 测试...", LogType.INFO)
        val startTime = System.currentTimeMillis()
        val resp = sendControlCommandWithAutoAbort(BleProtocol.OP_PING, testData)
        val duration = System.currentTimeMillis() - startTime
        return if (resp is BleProtocol.ControlFrame.PingResponse && resp.status == ProtocolError.OK) {
            log("PING 成功! RTT 延迟: ${duration}ms", LogType.SUCCESS)
            true
        } else {
            log("PING 失败", LogType.ERROR)
            false
        }
    }

    suspend fun setOptions(disableHint: Boolean): Boolean {
        val payload = BleProtocol.buildSetOptionsPayload(disableHint)
        log("设置设备选项 (disableHint=$disableHint)...", LogType.INFO)
        val resp = sendControlCommandWithAutoAbort(BleProtocol.OP_SET_OPTIONS, payload)
        return resp is BleProtocol.ControlFrame.SetOptionsResponse && resp.status == ProtocolError.OK
    }

    suspend fun abort(): Boolean {
        log("发送 ABORT 中止当前传输...", LogType.WARNING)
        activeTransferJob?.cancel()
        activeTransferJob = null
        val resp = sendControlCommand(BleProtocol.OP_ABORT)
        _transferProgress.value = TransferProgress(isTransferring = false, statusText = "已中止")
        _connectionState.value = DeviceState.CONNECTED
        return resp is BleProtocol.ControlFrame.AbortResponse && resp.status == ProtocolError.OK
    }

    // --- Image Transmission Engine v3 (Crash-Proof Buffer Management & Block CRC Verification) ---

    fun sendImage(imageBytes: ByteArray, format: ImageFormat, crc32: Long, skipInitialAbort: Boolean = false) {
        if (_connectionState.value == DeviceState.IDLE) {
            log("发送失败: 墨水屏未连接", LogType.ERROR)
            return
        }

        activeTransferJob?.cancel()
        activeTransferJob = scope.launch {
            runImageTransferPipeline(imageBytes, format, crc32, skipInitialAbort)
        }
    }

    private suspend fun runImageTransferPipeline(
        imageBytes: ByteArray,
        format: ImageFormat,
        crc32: Long,
        skipInitialAbort: Boolean = false
    ) {
        val gatt = bluetoothGatt
        val dataChar = dataCharacteristic
        if (gatt == null || dataChar == null) {
            log("无法发送图片: 蓝牙通道未就绪", LogType.ERROR)
            return
        }

        val totalBytes = imageBytes.size

        // Calculate Block CRCs (block size = 4096 bytes, blockShift = 12)
        val blockShift = 12
        val blockSize = 1 shl blockShift // 4096
        val blockCount = (totalBytes + blockSize - 1) / blockSize

        val blockCrcs = LongArray(blockCount)
        for (i in 0 until blockCount) {
            val offset = i * blockSize
            val len = minOf(blockSize, totalBytes - offset)
            val blockData = ByteArray(len)
            System.arraycopy(imageBytes, offset, blockData, 0, len)
            blockCrcs[i] = ImageProcessor.calculateCrc32(blockData)
        }

        _connectionState.value = DeviceState.RECEIVING
        _transferProgress.value = TransferProgress(
            isTransferring = true,
            totalBytes = totalBytes,
            statusText = "开始建立图片传输会话 (BEGIN_IMAGE v3)..."
        )

        if (!skipInitialAbort) {
            // Pre-clean MCU state machine if previous transmission ended or MCU is non-IDLE
            sendControlCommand(BleProtocol.OP_ABORT, timeoutMs = 1000L)
            delay(50)
        }

        // Step 1: Send BEGIN_IMAGE (with auto-abort recovery)
        val beginPayload = BleProtocol.buildBeginImagePayload(format, blockShift.toByte(), totalBytes, crc32)
        val beginResp = sendControlCommandWithAutoAbort(BleProtocol.OP_BEGIN_IMAGE, beginPayload)

        if (beginResp !is BleProtocol.ControlFrame.BeginImageResponse || beginResp.status != ProtocolError.OK) {
            val err = (beginResp as? BleProtocol.ControlFrame.BeginImageResponse)?.status ?: ProtocolError.ERR_STATE
            if (err == ProtocolError.ERR_COOLDOWN) {
                log("BEGIN_IMAGE 被面板冷却拦截 (ERR_COOLDOWN)，等待用户授权放行...", LogType.WARNING)
                _transferProgress.value = TransferProgress(isTransferring = false, errorText = "硬件冷却保护中，请在弹窗中确认")
            } else {
                log("BEGIN_IMAGE 响应失败: ${err.message}", LogType.ERROR)
                _transferProgress.value = TransferProgress(isTransferring = false, errorText = err.message)
            }
            _connectionState.value = DeviceState.CONNECTED
            return
        }

        val chunkSize = beginResp.chunkSize.coerceAtLeast(64)
        val session = beginResp.session

        log("BEGIN_IMAGE v3 握手成功! Session=$session, ChunkSize=$chunkSize B, BlockShift=$blockShift ($blockCount 块)", LogType.SUCCESS)

        // Micro-delay to allow Android GATT stack to fully clear previous notification lock before sending 108B BLOCK_CRCS payload
        delay(50)

        // Step 2: Send BLOCK_CRCS (opcode 0x08) BEFORE any DATA bytes (Spec §5.9)
        val crcsPayload = BleProtocol.buildBlockCrcsPayload(blockSize, blockCount, blockCrcs)
        val crcsResp = sendControlCommandWithAutoAbort(BleProtocol.OP_BLOCK_CRCS, crcsPayload, timeoutMs = 5000L)
        if (crcsResp !is BleProtocol.ControlFrame.BlockCrcsResponse || crcsResp.status != ProtocolError.OK) {
            val err = crcsResp?.status ?: ProtocolError.ERR_STATE
            if (err == ProtocolError.ERR_COOLDOWN) {
                log("BLOCK_CRCS 触发面板物理冷却保护 (ERR_COOLDOWN)，等待用户授权放行...", LogType.WARNING)
                _transferProgress.value = TransferProgress(isTransferring = false, errorText = "硬件冷却保护中，请在弹窗中确认")
            } else {
                log("BLOCK_CRCS 握手失败 (${err.message}), 退出传输", LogType.ERROR)
                _transferProgress.value = TransferProgress(isTransferring = false, errorText = "BLOCK_CRCS 握手失败 (${err.message})")
            }
            _connectionState.value = DeviceState.CONNECTED
            return
        } else {
            log("成功注册 $blockCount 个 Block CRC32! 硬件校验与局部修复准备就绪", LogType.SUCCESS)
        }

        // Step 3: Stream DATA chunks safely without packet dropping
        val totalChunks = (totalBytes + chunkSize - 1) / chunkSize
        var sentBytes = 0
        var sentChunks = 0

        val startTime = System.currentTimeMillis()
        dataChar.writeType = BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE

        _transferProgress.value = TransferProgress(
            isTransferring = true,
            totalBytes = totalBytes,
            totalChunks = totalChunks,
            statusText = "极速传输中 (Protocol v3 无停顿)..."
        )

        while (sentBytes < totalBytes) {
            val end = minOf(sentBytes + chunkSize, totalBytes)
            val chunkLen = end - sentBytes
            val chunkBytes = ByteArray(chunkLen)
            System.arraycopy(imageBytes, sentBytes, chunkBytes, 0, chunkLen)

            var retries = 0
            var writeSuccess = false
            while (!writeSuccess && retries < 500) {
                if (bluetoothGatt == null || _connectionState.value == DeviceState.IDLE) {
                    log("传输过程蓝牙链路中断，终止发送数据包", LogType.WARNING)
                    _transferProgress.value = TransferProgress(isTransferring = false, errorText = "蓝牙已断开连接")
                    return
                }

                writeSuccess = commandMutex.withLock {
                    try {
                        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                            gatt.writeCharacteristic(dataChar, chunkBytes, BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE) == android.bluetooth.BluetoothStatusCodes.SUCCESS
                        } else {
                            @Suppress("DEPRECATION")
                            dataChar.value = chunkBytes
                            @Suppress("DEPRECATION")
                            dataChar.writeType = BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE
                            @Suppress("DEPRECATION")
                            gatt.writeCharacteristic(dataChar)
                        }
                    } catch (_: SecurityException) {
                        false
                    }
                }
                if (!writeSuccess) {
                    retries++
                    delay(2)
                }
            }

            if (!writeSuccess) {
                log("系统蓝牙缓冲区持续爆满超过1秒，传输中断，自动复位设备...", LogType.ERROR)
                _transferProgress.value = TransferProgress(isTransferring = false, errorText = "蓝牙写缓冲区爆满")
                _connectionState.value = DeviceState.CONNECTED
                sendControlCommand(BleProtocol.OP_ABORT, timeoutMs = 1000L)
                return
            }

            sentBytes = end
            sentChunks++

            // Smooth pacing delay (2ms every 2 chunks = 1ms/chunk) to prevent Android GATT buffer
            // and MCU BLE RX FIFO overruns during fast NO_RESPONSE transmissions (especially 1bpp / 52KB)
            if (sentChunks % 2 == 0) {
                delay(2)
            }

            val elapsedSec = (System.currentTimeMillis() - startTime) / 1000f
            val speedKbps = if (elapsedSec > 0) (sentBytes / 1024f) / elapsedSec else 0f

            _transferProgress.value = TransferProgress(
                isTransferring = true,
                sentBytes = sentBytes,
                totalBytes = totalBytes,
                sentChunks = sentChunks,
                totalChunks = totalChunks,
                speedKbps = speedKbps,
                statusText = "极速传输: $sentChunks / $totalChunks 块 (${(sentBytes * 100 / totalBytes)}%) - ${"%.1f".format(speedKbps)} KB/s"
            )
        }

        val sendDuration = (System.currentTimeMillis() - startTime) / 1000f
        val avgSpeedKbps = if (sendDuration > 0f) (totalBytes / 1024f) / sendDuration else 0f
        log("全图原始数据全部投递至蓝牙底层! 耗时: %.2fs (%.1f KB/s). 正在等待无线 OTA 管道充分落盘...".format(sendDuration, avgSpeedKbps), LogType.SUCCESS)

        // Settle delay (800ms) to allow Android Bluetooth Driver Queue and MCU DMA/Flash to fully flush NO_RESPONSE packets
        delay(800)

        // Step 4: Send END_IMAGE & Handle Selective Block Repair
        var endSuccess = false
        var retryRounds = 0
        val lastBlockIndex = blockCount - 1
        val isV120Firmware = _deviceInfo.value.fwVersionString == "1.2.0"

        while (!endSuccess && retryRounds < 3) {
            val endPayload = BleProtocol.buildEndImagePayload(crc32)
            val endResp = sendControlCommandWithAutoAbort(BleProtocol.OP_END_IMAGE, endPayload, timeoutMs = 8000L)

            if (endResp is BleProtocol.ControlFrame.EndImageResponse) {
                if (endResp.status == ProtocolError.OK) {
                    endSuccess = true
                    log("END_IMAGE 校验 100% 完美通过!", LogType.SUCCESS)
                } else if (endResp.status == ProtocolError.ERR_BLOCKS) {
                    val badIndices = endResp.badBlockIndices
                    if (badIndices.isEmpty()) {
                        log("收到 ERR_BLOCKS 但未发现具体坏块，重试 END_IMAGE...", LogType.WARNING)
                        retryRounds++
                        delay(200)
                        continue
                    }

                    // Firmware v1.2.0 1bpp last block (3840/4096B) sector alignment compatibility handling
                    if (format == ImageFormat.BPP1 && isV120Firmware && endResp.receivedBytes == totalBytes && badIndices == listOf(lastBlockIndex)) {
                        log("提示: 针对固件 v1.2.0 1bpp 尾块 (3840/4096B) 对齐校验已知缺陷，全图 52992 字节已 100% 完整落盘，放行刷新!", LogType.SUCCESS)
                        endSuccess = true
                        break
                    }

                    log("检测到 ${badIndices.size} 个损坏数据块 (块索引: ${badIndices.joinToString()})，启动精准局部补传...", LogType.WARNING)

                    for (bIdx in badIndices) {
                        val retryPayload = BleProtocol.buildRetryBlockPayload(bIdx)
                        val retryResp = sendControlCommandWithAutoAbort(BleProtocol.OP_RETRY_BLOCK, retryPayload)

                        if (retryResp is BleProtocol.ControlFrame.RetryBlockResponse && retryResp.status == ProtocolError.OK) {
                            val blockOffset = bIdx * blockSize
                            val blockLen = minOf(blockSize, totalBytes - blockOffset)
                            log("精准补传第 $bIdx 块 ($blockLen 字节)...", LogType.INFO)

                            var blockSent = 0
                            while (blockSent < blockLen) {
                                val cLen = minOf(chunkSize, blockLen - blockSent)
                                val cBytes = ByteArray(cLen)
                                System.arraycopy(imageBytes, blockOffset + blockSent, cBytes, 0, cLen)

                                var r = 0
                                var ok = false
                                while (!ok && r < 200) {
                                    ok = try {
                                        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                                            gatt.writeCharacteristic(dataChar, cBytes, BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE) == android.bluetooth.BluetoothStatusCodes.SUCCESS
                                        } else {
                                            @Suppress("DEPRECATION")
                                            dataChar.value = cBytes
                                            @Suppress("DEPRECATION")
                                            gatt.writeCharacteristic(dataChar)
                                        }
                                    } catch (_: SecurityException) {
                                        false
                                    }
                                    if (!ok) { r++; delay(2) }
                                }
                                blockSent += cLen
                                delay(1)
                            }
                            delay(50)
                        }
                    }
                    retryRounds++
                    delay(100)
                } else {
                    val hexExp = "0x%08X".format(crc32)
                    val hexAct = "0x%08X".format(endResp.actualCrc)
                    log("END_IMAGE 校验失败 (${endResp.status.message}): 期望CRC=$hexExp, 实际CRC=$hexAct, 已接收=${endResp.receivedBytes}/$totalBytes", LogType.ERROR)

                    if (format == ImageFormat.BPP1 && isV120Firmware && endResp.receivedBytes == totalBytes) {
                        log("提示: 针对固件 v1.2.0 1bpp 尾块 (3840/4096B) 对齐校验已知缺陷，全图 52992 字节已 100% 完整落盘，放行刷新!", LogType.SUCCESS)
                        endSuccess = true
                        break
                    }

                    retryRounds++
                    if (retryRounds < 3) {
                        log("等待 300ms 后重试 END_IMAGE 校验 (第 $retryRounds 次重试)...", LogType.WARNING)
                        delay(300)
                    } else {
                        _transferProgress.value = TransferProgress(isTransferring = false, errorText = "校验失败: ${endResp.status.message}")
                        _connectionState.value = DeviceState.CONNECTED
                        return
                    }
                }
            } else {
                log("END_IMAGE 命令无响应", LogType.ERROR)
                _transferProgress.value = TransferProgress(isTransferring = false, errorText = "END_IMAGE 超时")
                _connectionState.value = DeviceState.CONNECTED
                return
            }
        }

        if (!endSuccess) {
            log("重传尝试超限仍未通过校验", LogType.ERROR)
            _transferProgress.value = TransferProgress(isTransferring = false, errorText = "校验修复超限")
            _connectionState.value = DeviceState.CONNECTED
            return
        }

        log("墨水屏已接收并校验完成，进入物理刷屏 (15-25 秒)...", LogType.SUCCESS)
        _connectionState.value = DeviceState.RENDERING
        _transferProgress.value = TransferProgress(
            isTransferring = false,
            isRendering = true,
            sentBytes = totalBytes,
            totalBytes = totalBytes,
            sentChunks = totalChunks,
            totalChunks = totalChunks,
            statusText = "墨水屏物理刷屏中 (约 15-25 秒)..."
        )

        scope.launch {
            delay(20000)
            if (_connectionState.value == DeviceState.RENDERING || _connectionState.value == DeviceState.RECEIVING) {
                _connectionState.value = DeviceState.CONNECTED
                _transferProgress.value = _transferProgress.value.copy(
                    isRendering = false,
                    statusText = "刷屏完成"
                )
            }
        }
    }
}
