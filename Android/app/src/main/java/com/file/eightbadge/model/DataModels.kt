package com.file.eightbadge.model

import android.bluetooth.BluetoothDevice

/**
 * Screen Orientation
 */
enum class ScreenOrientation(val width: Int, val height: Int, val displayName: String) {
    PORTRAIT(width = 552, height = 768, displayName = "竖屏"),
    LANDSCAPE(width = 768, height = 552, displayName = "横屏")
}

/**
 * Image Format options
 */
enum class ImageFormat(val code: Byte, val bpp: Int, val expectedSize: Int, val displayName: String) {
    BPP2(code = 1, bpp = 2, expectedSize = 105984, displayName = "4色 (高画质)"),
    BPP1(code = 2, bpp = 1, expectedSize = 52992, displayName = "黑白 (极速)")
}

/**
 * Scale Mode for image resizing.
 */
enum class ScaleMode(val displayName: String) {
    CROP_FILL("等比裁剪"),
    FIT_PAD("等比留白"),
    STRETCH("拉伸铺满")
}

/**
 * Clean Customer-Facing Dithering Mode selection.
 */
enum class DitherMode(val displayName: String, val description: String) {
    FLOYD_STEINBERG("Floyd-Steinberg 经典", "适合常规照片与人像。色彩过渡自然，是通用性最强的经典效果。"),
    ATKINSON("Atkinson 高清", "适合文字、漫画与高对比线稿。画面干净，高光区域清晰无杂点。"),
    JJN("JJN 柔和", "适合风景摄影与渐变图。色彩过渡极度平滑柔和。"),
    STUCKI("Stucki 细节", "适合高精细建筑与复杂纹理。细节丰富，边缘锐利。"),
    BURKES("Burkes 自然", "适合自然风光与人物肖像。过渡平滑且处理快速。"),
    SIERRA_3("Sierra 柔和", "适合柔和人像与艺术写真。影调过渡温和细腻。"),
    SIERRA_2("Sierra 平衡", "适合各种日常图像。画面清晰度与过渡效果均衡。"),
    SIERRA_LITE("Sierra 极速", "适合速写与简笔线条画。快速清晰呈现。"),
    BAYER_2X2("Bayer 粗网格", "适合复古像素画与表格。有序网格风格，复古质感。"),
    BAYER_4X4("Bayer 复古点阵", "适合复古插画与图标。有序点阵排列，结构清晰。"),
    BAYER_8X8("Bayer 细腻网格", "适合高分辨率插画。细致交叉点阵，规整自然。"),
    CLUSTER_DOT_4X4("半色调网点", "适合复刻报纸与复古印刷品。经典网点排版质感。"),
    RANDOM("艺术胶片颗粒", "适合黑白海报与艺术照片。带有个性化胶片胶质颗粒感。"),
    NEAREST("纯色无抖动", "适合图标、矢量图与文字线稿。纯色填充无杂点，边缘干净。")
}

/**
 * Device info parsed from GET_INFO (opcode 0x81)
 */
data class DeviceInfo(
    val protocolVersion: Int = 3,
    val fwMajor: Int = 1,
    val fwMinor: Int = 2,
    val fwPatch: Int = 0,
    val canvasWidth: Int = 552,
    val canvasHeight: Int = 768,
    val panelWidth: Int = 768,
    val panelHeight: Int = 552,
    val bpp: Int = 2,
    val colorCount: Int = 4,
    val len2bpp: Int = 105984,
    val len1bpp: Int = 52992,
    val mtu: Int = 517,
    val chunkSize: Int = 512,
    val windowChunks: Int = 32,
    val rotation: Int = 90,
    val blockShiftMin: Int = 11,
    val blockShiftMax: Int = 14,
    val maxBlocks: Int = 128,
    val deviceName: String = "",
    val refreshMinGapS: Int = 180,
    val lastRefreshUtc: Long = 0L,
    val clockKnown: Boolean = false
) {
    val fwVersionString: String
        get() = "$fwMajor.$fwMinor.$fwPatch"
}

/**
 * State enum representing device EVT_STATE
 */
enum class DeviceState(val code: Int, val description: String) {
    IDLE(0, "空闲"),
    ADVERTISING(1, "广播中"),
    CONNECTED(2, "已连接"),
    RECEIVING(3, "正在接收数据"),
    VERIFYING(4, "校验中"),
    RENDERING(5, "正在刷新屏幕"),
    DISPLAYED(6, "刷新完成"),
    HINT_SHOWN(7, "提示屏"),
    ERROR(8, "设备异常"),
    FIXING(9, "重传修复中");

    companion object {
        fun fromCode(code: Int): DeviceState {
            return entries.firstOrNull { it.code == code } ?: ERROR
        }
    }
}

/**
 * Error code enum from responses and EVT_STATE error
 */
enum class ProtocolError(val code: Int, val message: String) {
    OK(0x00, "成功"),
    ERR_OPCODE(0x01, "未知命令"),
    ERR_LENGTH(0x02, "数据长度异常"),
    ERR_STATE(0x03, "状态不允许"),
    ERR_FORMAT(0x04, "格式不支持"),
    ERR_SIZE(0x05, "尺寸不匹配"),
    ERR_CRC(0x06, "校验失败"),
    ERR_MEM(0x07, "内存不足"),
    ERR_PANEL(0x08, "刷新超时"),
    ERR_BUSY(0x09, "设备忙，刷屏中"),
    ERR_OVERFLOW(0x0A, "数据溢出"),
    ERR_OPTIONS(0x0B, "选项非法"),
    ERR_BLOCKS(0x0C, "数据块需重传"),
    ERR_COOLDOWN(0x0D, "刷新冷却中，需授权确认");

    companion object {
        fun fromCode(code: Int): ProtocolError {
            return entries.firstOrNull { it.code == code } ?: OK
        }
    }
}

/**
 * Data payload received when EVT_REFRESH_BLOCKED (opcode 0xC3) occurs
 */
data class RefreshBlockedEvent(
    val token: Byte,
    val reason: Int, // 0 = 无时钟(下界), 1 = 有时钟(精确), 2 = App时间早于记录
    val forOpcode: Byte, // 被拦截的命令 (0x02 BEGIN_IMAGE 或 0x05 SHOW_HINT)
    val waitSeconds: Int,
    val minGapSeconds: Int,
    val elapsedSeconds: Long
)

/**
 * Scanned BLE device item
 */
data class BleDeviceItem(
    val device: BluetoothDevice,
    val name: String,
    val rssi: Int,
    val address: String
)

/**
 * Realtime image transfer progress tracking
 */
data class TransferProgress(
    val isTransferring: Boolean = false,
    val isRendering: Boolean = false,
    val sentBytes: Int = 0,
    val totalBytes: Int = 0,
    val sentChunks: Int = 0,
    val totalChunks: Int = 0,
    val speedKbps: Float = 0f,
    val statusText: String = "",
    val errorText: String? = null
) {
    val progressPercent: Int
        get() = if (totalBytes > 0) ((sentBytes.toFloat() / totalBytes) * 100).toInt().coerceIn(0, 100) else 0
}

/**
 * Console log message
 */
data class LogMessage(
    val id: Long = 0L,
    val timestamp: String,
    val message: String,
    val type: LogType = LogType.INFO
)

enum class LogType {
    INFO, SUCCESS, WARNING, ERROR, BLE_TX, BLE_RX
}
