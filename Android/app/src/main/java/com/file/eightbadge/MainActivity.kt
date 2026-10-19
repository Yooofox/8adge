package com.file.eightbadge

import android.Manifest
import android.content.res.ColorStateList
import android.graphics.BitmapFactory
import android.os.Build
import android.os.Bundle
import android.view.LayoutInflater
import android.view.View
import android.widget.Toast
import androidx.activity.enableEdgeToEdge
import androidx.activity.result.contract.ActivityResultContracts
import androidx.activity.viewModels
import androidx.appcompat.app.AppCompatActivity
import androidx.core.view.ViewCompat
import androidx.core.view.WindowInsetsCompat
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.lifecycleScope
import androidx.lifecycle.repeatOnLifecycle
import androidx.recyclerview.widget.LinearLayoutManager
import com.file.eightbadge.databinding.ActivityMainBinding
import com.file.eightbadge.databinding.DialogCropImageBinding
import com.file.eightbadge.databinding.DialogDeviceSearchBinding
import com.file.eightbadge.databinding.DialogTestPanelBinding
import com.file.eightbadge.image.ImageProcessor
import com.file.eightbadge.model.DeviceState
import com.file.eightbadge.model.DitherMode
import com.file.eightbadge.model.ImageFormat
import com.file.eightbadge.model.ScaleMode
import com.file.eightbadge.model.ScreenOrientation
import com.file.eightbadge.ui.DeviceAdapter
import com.file.eightbadge.ui.LogAdapter
import com.file.eightbadge.ui.MainViewModel
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import kotlinx.coroutines.Job
import kotlinx.coroutines.launch

class MainActivity : AppCompatActivity() {

    private lateinit var binding: ActivityMainBinding
    private val viewModel: MainViewModel by viewModels()

    private var searchDialog: androidx.appcompat.app.AlertDialog? = null
    private var searchDialogJob: Job? = null

    private var testDialog: androidx.appcompat.app.AlertDialog? = null
    private var testDialogJob: Job? = null

    // Gallery Picker Contract
    private val getContentLauncher = registerForActivityResult(ActivityResultContracts.GetContent()) { uri ->
        uri?.let {
            val bitmap = decodeSampledBitmapFromUri(it, reqWidth = 2048, reqHeight = 2048)
            if (bitmap != null) {
                viewModel.setSourceBitmap(bitmap)
            } else {
                Toast.makeText(this, "打开图片失败", Toast.LENGTH_SHORT).show()
            }
        }
    }

    private fun decodeSampledBitmapFromUri(uri: android.net.Uri, reqWidth: Int, reqHeight: Int): android.graphics.Bitmap? {
        return try {
            val options = BitmapFactory.Options().apply {
                inJustDecodeBounds = true
            }
            contentResolver.openInputStream(uri)?.use { stream ->
                BitmapFactory.decodeStream(stream, null, options)
            }

            val srcWidth = options.outWidth
            val srcHeight = options.outHeight
            if (srcWidth <= 0 || srcHeight <= 0) return null

            var inSampleSize = 1
            if (srcWidth > reqWidth || srcHeight > reqHeight) {
                val halfWidth = srcWidth / 2
                val halfHeight = srcHeight / 2
                while ((halfWidth / inSampleSize) >= reqWidth || (halfHeight / inSampleSize) >= reqHeight) {
                    inSampleSize *= 2
                }
            }

            val decodeOptions = BitmapFactory.Options().apply {
                this.inSampleSize = inSampleSize
                inPreferredConfig = android.graphics.Bitmap.Config.ARGB_8888
            }

            contentResolver.openInputStream(uri)?.use { stream ->
                BitmapFactory.decodeStream(stream, null, decodeOptions)
            }
        } catch (_: Throwable) {
            null
        }
    }

    // Runtime Permission Request Contract
    private val permissionLauncher = registerForActivityResult(
        ActivityResultContracts.RequestMultiplePermissions()
    ) { permissions ->
        val allGranted = permissions.entries.all { it.value }
        if (!allGranted) {
            Toast.makeText(this, "需要蓝牙权限才能扫描与传输墨水屏数据", Toast.LENGTH_LONG).show()
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()

        binding = ActivityMainBinding.inflate(layoutInflater)
        setContentView(binding.root)

        // Handle System Insets to avoid status bar overlap
        ViewCompat.setOnApplyWindowInsetsListener(binding.main) { _, insets ->
            val systemBars = insets.getInsets(WindowInsetsCompat.Type.systemBars())
            binding.appBarLayout.setPadding(0, systemBars.top, 0, 0)
            binding.main.setPadding(systemBars.left, 0, systemBars.right, systemBars.bottom)
            insets
        }

        checkAndRequestPermissions()
        setupUI()
        observeViewModel()
    }

    private fun checkAndRequestPermissions() {
        val permissions = mutableListOf<String>()
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            permissions.add(Manifest.permission.BLUETOOTH_SCAN)
            permissions.add(Manifest.permission.BLUETOOTH_CONNECT)
        } else {
            permissions.add(Manifest.permission.BLUETOOTH)
            permissions.add(Manifest.permission.BLUETOOTH_ADMIN)
            permissions.add(Manifest.permission.ACCESS_FINE_LOCATION)
        }
        permissionLauncher.launch(permissions.toTypedArray())
    }

    private fun setupUI() {
        // Top Bar Buttons
        binding.btnSearchDevice.setOnClickListener {
            showDeviceSearchDialog()
        }

        binding.btnTestPanel.setOnClickListener {
            showTestPanelDialog()
        }

        // Orientation Radio
        binding.radioGroupOrientation.setOnCheckedChangeListener { _, checkedId ->
            val orient = if (checkedId == R.id.radioOrientPortrait) ScreenOrientation.PORTRAIT else ScreenOrientation.LANDSCAPE
            viewModel.screenOrientation.value = orient
            updateOrientationUI(orient)
            viewModel.processCurrentImage()
        }

        // Image Selection & Editing Buttons
        binding.btnPickGallery.setOnClickListener {
            getContentLauncher.launch("image/*")
        }

        binding.btnRotateLeft.setOnClickListener { viewModel.rotateLeft() }
        binding.btnRotateRight.setOnClickListener { viewModel.rotateRight() }
        binding.btnManualCrop.setOnClickListener { showManualCropDialog() }
        binding.btnResetEdit.setOnClickListener {
            viewModel.resetCrop()
        }

        // Color Adjustment Sliders: Saturation, Contrast, Brightness, Gamma, Sharpen, Invert
        binding.sliderSaturation.addOnChangeListener { _, value, fromUser ->
            if (fromUser) {
                viewModel.saturation.value = value
                binding.textSaturationLabel.text = "饱和度: ${"%.2f".format(value)}x"
                viewModel.processCurrentImage()
            }
        }

        binding.sliderContrast.addOnChangeListener { _, value, fromUser ->
            if (fromUser) {
                viewModel.contrast.value = value
                binding.textContrastLabel.text = "对比度: ${"%.2f".format(value)}x"
                viewModel.processCurrentImage()
            }
        }

        binding.sliderBrightness.addOnChangeListener { _, value, fromUser ->
            if (fromUser) {
                viewModel.brightness.value = value
                binding.textBrightnessLabel.text = "亮度: ${value.toInt()}"
                viewModel.processCurrentImage()
            }
        }

        binding.sliderGamma.addOnChangeListener { _, value, fromUser ->
            if (fromUser) {
                viewModel.gamma.value = value
                binding.textGammaLabel.text = "伽马: ${"%.2f".format(value)}"
                viewModel.processCurrentImage()
            }
        }

        binding.sliderSharpen.addOnChangeListener { _, value, fromUser ->
            if (fromUser) {
                viewModel.sharpen.value = value
                binding.textSharpenLabel.text = "锐化: ${value.toInt()}%"
                viewModel.processCurrentImage()
            }
        }

        binding.checkInvertColors.setOnCheckedChangeListener { _, isChecked ->
            viewModel.invertColors.value = isChecked
            viewModel.processCurrentImage()
        }

        binding.btnResetAdjustments.setOnClickListener {
            binding.sliderSaturation.value = 1.0f
            binding.sliderContrast.value = 1.0f
            binding.sliderBrightness.value = 0.0f
            binding.sliderGamma.value = 1.0f
            binding.sliderSharpen.value = 0.0f
            binding.checkInvertColors.isChecked = false

            binding.textSaturationLabel.text = "饱和度: 1.00x"
            binding.textContrastLabel.text = "对比度: 1.00x"
            binding.textBrightnessLabel.text = "亮度: 0"
            binding.textGammaLabel.text = "伽马: 1.00"
            binding.textSharpenLabel.text = "锐化: 0%"

            viewModel.resetAdjustments()
        }

        // Format Radio
        binding.radioGroupFormat.setOnCheckedChangeListener { _, checkedId ->
            viewModel.imageFormat.value = if (checkedId == R.id.radioFormat2bpp) ImageFormat.BPP2 else ImageFormat.BPP1
            viewModel.processCurrentImage()
        }

        // Scale Radio
        binding.radioGroupScale.setOnCheckedChangeListener { _, checkedId ->
            viewModel.scaleMode.value = when (checkedId) {
                R.id.radioScaleFit -> ScaleMode.FIT_PAD
                R.id.radioScaleStretch -> ScaleMode.STRETCH
                else -> ScaleMode.CROP_FILL
            }
            viewModel.processCurrentImage()
        }

        // 14 Dithering Algorithms Vertical Wheel Picker (Scroll like Clock Time Picker)
        val ditherDisplayNames = DitherMode.entries.map { it.displayName }.toTypedArray()
        binding.pickerDitherMode.apply {
            displayedValues = null
            minValue = 0
            maxValue = ditherDisplayNames.size - 1
            displayedValues = ditherDisplayNames
            wrapSelectorWheel = true
            value = 0

            setOnValueChangedListener { _, _, newVal ->
                val selectedMode = DitherMode.entries[newVal]
                viewModel.ditherMode.value = selectedMode
                binding.textDitherDescription.text = selectedMode.description
                viewModel.processCurrentImage()
            }
        }

        // Send Button
        binding.btnSendImage.setOnClickListener {
            viewModel.sendCurrentImageToEpaper()
        }
    }

    private fun showDeviceSearchDialog() {
        val dialogBinding = DialogDeviceSearchBinding.inflate(LayoutInflater.from(this))

        val dialogAdapter = DeviceAdapter { deviceItem ->
            viewModel.connectDevice(deviceItem)
            searchDialog?.dismiss() // Automatically dismiss search dialog on connection
        }

        dialogBinding.dialogRecyclerDevices.apply {
            layoutManager = LinearLayoutManager(this@MainActivity)
            adapter = dialogAdapter
        }

        dialogBinding.dialogBtnRescan.setOnClickListener {
            viewModel.startScan()
        }

        dialogBinding.dialogBtnCloseSearch.setOnClickListener {
            viewModel.stopScan()
            searchDialog?.dismiss()
        }

        searchDialog = MaterialAlertDialogBuilder(this)
            .setView(dialogBinding.root)
            .setOnDismissListener {
                viewModel.stopScan()
                searchDialogJob?.cancel()
                searchDialogJob = null
                searchDialog = null
            }
            .create()

        searchDialog?.show()
        viewModel.startScan() // Trigger scan on dialog open

        searchDialogJob?.cancel()
        searchDialogJob = lifecycleScope.launch {
            repeatOnLifecycle(Lifecycle.State.STARTED) {
                launch {
                    viewModel.scannedDevices.collect { devices ->
                        val sorted = devices.sortedByDescending { it.rssi }
                        dialogAdapter.submitList(sorted)
                    }
                }
                launch {
                    viewModel.isScanning.collect { isScanning ->
                        dialogBinding.dialogSearchProgress.visibility = if (isScanning) View.VISIBLE else View.INVISIBLE
                        dialogBinding.dialogBtnRescan.text = if (isScanning) "搜索中..." else "重新搜索"
                    }
                }
            }
        }
    }

    private fun showTestPanelDialog() {
        val dialogBinding = DialogTestPanelBinding.inflate(LayoutInflater.from(this))

        val logAdapter = LogAdapter()
        dialogBinding.dialogRecyclerLogs.apply {
            layoutManager = LinearLayoutManager(this@MainActivity)
            adapter = logAdapter
        }

        dialogBinding.dialogBtnGetInfo.setOnClickListener { viewModel.sendGetInfo() }
        dialogBinding.dialogBtnShowHint.setOnClickListener { viewModel.sendShowHint() }
        dialogBinding.dialogBtnPing.setOnClickListener { viewModel.sendPing() }
        dialogBinding.dialogBtnDisconnect.setOnClickListener { viewModel.disconnectDevice() }

        // Load AND immediately send test pattern on click
        dialogBinding.dialogBtnTestYellowRed.setOnClickListener {
            viewModel.loadAndSendTestPattern("YELLOW_RED_TEST")
        }
        dialogBinding.dialogBtnTestGrid.setOnClickListener {
            viewModel.loadAndSendTestPattern("GRID_CHESS")
        }
        dialogBinding.dialogBtnTestWhite.setOnClickListener {
            viewModel.loadAndSendTestPattern("ALL_WHITE")
        }
        dialogBinding.dialogBtnTestBlack.setOnClickListener {
            viewModel.loadAndSendTestPattern("ALL_BLACK")
        }
        dialogBinding.dialogBtnTestRainbow.setOnClickListener {
            viewModel.loadAndSendTestPattern("RAINBOW_GRADIENT")
        }

        dialogBinding.dialogBtnClearLogs.setOnClickListener {
            viewModel.clearLogs()
        }

        dialogBinding.dialogBtnCopyLogs.setOnClickListener {
            val logsText = viewModel.getFormattedLogsText()
            val clipboard = getSystemService(CLIPBOARD_SERVICE) as android.content.ClipboardManager
            val clip = android.content.ClipData.newPlainText("8adge Logs", logsText)
            clipboard.setPrimaryClip(clip)
            Toast.makeText(this, "控制台日志已复制到剪贴板", Toast.LENGTH_SHORT).show()
        }

        dialogBinding.dialogBtnExportLogs.setOnClickListener {
            val logsText = viewModel.getFormattedLogsText()
            try {
                val logFile = java.io.File(cacheDir, "8adge_console_logs.txt")
                logFile.writeText(logsText)
                val contentUri = androidx.core.content.FileProvider.getUriForFile(
                    this,
                    "$packageName.fileprovider",
                    logFile
                )
                val sendIntent = android.content.Intent().apply {
                    action = android.content.Intent.ACTION_SEND
                    putExtra(android.content.Intent.EXTRA_STREAM, contentUri)
                    type = "text/plain"
                    addFlags(android.content.Intent.FLAG_GRANT_READ_URI_PERMISSION)
                }
                val shareIntent = android.content.Intent.createChooser(sendIntent, "分享导出 8adge 控制台日志")
                startActivity(shareIntent)
            } catch (e: Exception) {
                Toast.makeText(this, "导出日志文件失败: ${e.message}", Toast.LENGTH_SHORT).show()
            }
        }

        val dialog = MaterialAlertDialogBuilder(this)
            .setView(dialogBinding.root)
            .setOnDismissListener {
                testDialogJob?.cancel()
                testDialogJob = null
                testDialog = null
            }
            .create()

        dialogBinding.dialogBtnCloseTest.setOnClickListener { dialog.dismiss() }

        testDialog = dialog
        dialog.show()

        testDialogJob?.cancel()
        testDialogJob = lifecycleScope.launch {
            repeatOnLifecycle(Lifecycle.State.STARTED) {
                viewModel.logs.collect { logs ->
                    logAdapter.submitList(logs) {
                        if (logs.isNotEmpty()) {
                            dialogBinding.dialogRecyclerLogs.scrollToPosition(logs.size - 1)
                        }
                    }
                }
            }
        }
    }

    private var refreshBlockedDialog: androidx.appcompat.app.AlertDialog? = null

    override fun onDestroy() {
        searchDialogJob?.cancel()
        searchDialogJob = null
        searchDialog?.dismiss()
        searchDialog = null

        testDialogJob?.cancel()
        testDialogJob = null
        testDialog?.dismiss()
        testDialog = null

        refreshBlockedDialog?.dismiss()
        refreshBlockedDialog = null

        super.onDestroy()
    }

    private fun showManualCropDialog() {
        val src = viewModel.sourceBitmap.value ?: run {
            Toast.makeText(this, "请先选择一张图片", Toast.LENGTH_SHORT).show()
            return
        }

        val rotatedSrc = ImageProcessor.rotateBitmap(src, viewModel.rotationDegrees.value)
        val dialogBinding = DialogCropImageBinding.inflate(LayoutInflater.from(this))
        val currentOrient = viewModel.screenOrientation.value

        dialogBinding.dialogCropOverlayView.targetAspectRatio = currentOrient.width.toFloat() / currentOrient.height.toFloat()
        dialogBinding.dialogCropOverlayView.setImageBitmap(rotatedSrc)
        dialogBinding.dialogCheckLockRatio.text = "锁定 ${currentOrient.width}:${currentOrient.height} 比例"

        val dialog = MaterialAlertDialogBuilder(this)
            .setView(dialogBinding.root)
            .setCancelable(true)
            .setOnDismissListener {
                if (rotatedSrc !== src && !rotatedSrc.isRecycled) {
                    rotatedSrc.recycle()
                }
            }
            .create()

        dialogBinding.dialogCheckLockRatio.setOnCheckedChangeListener { _, isChecked ->
            dialogBinding.dialogCropOverlayView.isRatioLocked = isChecked
        }

        dialogBinding.dialogBtnResetCrop.setOnClickListener {
            dialogBinding.dialogCropOverlayView.resetCropFrame()
        }

        dialogBinding.dialogBtnCancel.setOnClickListener {
            dialog.dismiss()
        }

        dialogBinding.dialogBtnApplyCrop.setOnClickListener {
            val normalizedRect = dialogBinding.dialogCropOverlayView.getNormalizedCropRect()
            viewModel.setCropBounds(normalizedRect)
            dialog.dismiss()
        }

        dialog.show()
    }

    private fun updateOrientationUI(orient: ScreenOrientation) {
        binding.textPreviewSub.text = "预览"
    }

    private fun observeViewModel() {
        lifecycleScope.launch {
            repeatOnLifecycle(Lifecycle.State.STARTED) {
                launch {
                    viewModel.connectionState.collect { state ->
                        updateConnectionStateUI(state, viewModel.connectedDeviceName.value)
                    }
                }

                launch {
                    viewModel.screenOrientation.collect { orient ->
                        if (orient == ScreenOrientation.PORTRAIT) {
                            binding.radioOrientPortrait.isChecked = true
                        } else {
                            binding.radioOrientLandscape.isChecked = true
                        }
                        updateOrientationUI(orient)
                    }
                }

                launch {
                    viewModel.processedResult.collect { result ->
                        if (result != null) {
                            binding.imagePreview.setImageBitmap(result.previewBitmap)
                            (binding.imagePreview.drawable as? android.graphics.drawable.BitmapDrawable)?.isFilterBitmap = false
                            binding.btnSendImage.text = "发送"
                        }
                    }
                }

                launch {
                    viewModel.transferProgress.collect { progress ->
                        binding.progressTransfer.visibility = if (progress.isTransferring || progress.isRendering) View.VISIBLE else View.GONE
                        binding.progressTransfer.progress = progress.progressPercent

                        binding.btnSendImage.isEnabled = !progress.isTransferring && !progress.isRendering

                        if (progress.errorText != null) {
                            Toast.makeText(this@MainActivity, progress.errorText, Toast.LENGTH_SHORT).show()
                        }
                    }
                }

                launch {
                    viewModel.toastMessageFlow.collect { msg ->
                        Toast.makeText(this@MainActivity, msg, Toast.LENGTH_SHORT).show()
                    }
                }

                launch {
                    viewModel.refreshBlockedEvent.collect { event ->
                        showRefreshBlockedDialog(event)
                    }
                }

                launch {
                    viewModel.brightness.collect { b ->
                        if (binding.sliderBrightness.value != b) {
                            binding.sliderBrightness.value = b
                            binding.textBrightnessLabel.text = "亮度: ${b.toInt()}"
                        }
                    }
                }

                launch {
                    viewModel.contrast.collect { c ->
                        if (binding.sliderContrast.value != c) {
                            binding.sliderContrast.value = c
                            binding.textContrastLabel.text = "对比度: ${"%.2f".format(c)}x"
                        }
                    }
                }

                launch {
                    viewModel.saturation.collect { s ->
                        if (binding.sliderSaturation.value != s) {
                            binding.sliderSaturation.value = s
                            binding.textSaturationLabel.text = "饱和度: ${"%.2f".format(s)}x"
                        }
                    }
                }

                launch {
                    viewModel.gamma.collect { g ->
                        if (binding.sliderGamma.value != g) {
                            binding.sliderGamma.value = g
                            binding.textGammaLabel.text = "伽马: ${"%.2f".format(g)}"
                        }
                    }
                }

                launch {
                    viewModel.sharpen.collect { sh ->
                        if (binding.sliderSharpen.value != sh) {
                            binding.sliderSharpen.value = sh
                            binding.textSharpenLabel.text = "锐化: ${sh.toInt()}%"
                        }
                    }
                }

                launch {
                    viewModel.invertColors.collect { inv ->
                        if (binding.checkInvertColors.isChecked != inv) {
                            binding.checkInvertColors.isChecked = inv
                        }
                    }
                }

                launch {
                    viewModel.ditherMode.collect { mode ->
                        val idx = DitherMode.entries.indexOf(mode)
                        if (idx >= 0 && binding.pickerDitherMode.value != idx) {
                            binding.pickerDitherMode.value = idx
                            binding.textDitherDescription.text = mode.description
                        }
                    }
                }
            }
        }
    }

    private fun showRefreshBlockedDialog(event: com.file.eightbadge.model.RefreshBlockedEvent) {
        refreshBlockedDialog?.dismiss()
        val elapsedText = if (event.reason == 0) "至少 ${event.elapsedSeconds} 秒" else "${event.elapsedSeconds} 秒"
        val message = "距上次刷屏 $elapsedText（建议 ≥ ${event.minGapSeconds} 秒，还需等待 ${event.waitSeconds} 秒）。\n\n" +
                "四色墨水屏刷新间隔不足会导致颜料粒子团聚，造成不可逆的物理残影与串色损伤。\n\n" +
                "确定要现在强制放行并刷新吗？"

        refreshBlockedDialog = MaterialAlertDialogBuilder(this)
            .setTitle("面板尚未冷却")
            .setMessage(message)
            .setIcon(android.R.drawable.ic_dialog_alert)
            .setPositiveButton("仍然刷新") { _, _ ->
                viewModel.confirmAndRetryRefresh(event.token, event.forOpcode)
            }
            .setNegativeButton("取消", null)
            .setOnDismissListener {
                refreshBlockedDialog = null
            }
            .show()
    }

    private fun updateConnectionStateUI(state: DeviceState, deviceName: String?) {
        val (statusText, colorHex) = when (state) {
            DeviceState.IDLE -> "未连接" to "#D32F2F"
            DeviceState.CONNECTED -> "已连接" to "#111111"
            DeviceState.RECEIVING -> "正在接收..." to "#1976D2"
            DeviceState.VERIFYING -> "正在校验..." to "#7B1FA2"
            DeviceState.RENDERING -> "刷屏中..." to "#F57C00"
            DeviceState.DISPLAYED -> "已连接" to "#111111"
            DeviceState.HINT_SHOWN -> "提示屏" to "#0288D1"
            DeviceState.ERROR -> "设备异常" to "#D32F2F"
            else -> "未连接" to "#D32F2F"
        }

        binding.btnConnectionStatus.text = statusText
        binding.btnConnectionStatus.backgroundTintList = ColorStateList.valueOf(android.graphics.Color.parseColor(colorHex))
    }
}
