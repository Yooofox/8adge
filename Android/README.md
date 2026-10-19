# 8adge - 8线低成本四色墨水屏 Android 客户端

**8adge**（取自 Badge 徽章/电子牌，数字 8 代表 8 根飞线 DIY）是一款专为 **3.98 英寸 4色（黑白黄红）及 2色（黑白）电子墨水屏（原生 768 × 552 面板，E-Paper / E-Ink）** 设计的高性能 Android 客户端应用。仅需焊接 8 根线即可低成本搭建极客四色墨水屏显示器 / 电子桌牌，手机供电刷新完断电即可永久保持显示。

- **应用通过 **BLE（Bluetooth Low Energy，低功耗蓝牙）** 与墨水屏硬件设备进行通信，集成了**图像高保真处理、14 种图像抖动算法、自定义调色与裁剪、Protocol V3 极速无丢包传输协议、物理刷新冷却保护以及局部坏块自动修复**等核心功能。

---

## 目录

- [项目简介](#项目简介)
- [核心功能与技术亮点](#核心功能与技术亮点)
- [项目技术栈](#项目技术栈)
- [目录结构](#目录结构)
- [蓝牙通信协议 (Protocol V3)](#蓝牙通信协议-protocol-v3)
- [图像处理与抖动算法引擎](#图像处理与抖动算法引擎)
- [UI 界面与交互设计](#ui-界面与交互设计)
- [单元测试与校验向量](#单元测试与校验向量)
- [编译与运行指南](#编译与运行指南)

---

## 项目简介

- **应用名称**：`8adge`
- **应用包名**：`com.file.eightbadge`
- **目标平台**：Android（最低支持 SDK 26 / Android 8.0，编译与目标 SDK 37 / Android 16）
- **硬件特性**：8 根飞线低成本 DIY，手机/数据线即插即刷，断电零功耗保持显示。
- **适配屏幕规格**：
  - **竖屏（Portrait）**：画布分辨率 `552 × 768` 像素
  - **横屏（Landscape）**：画布分辨率 `768 × 552` 像素
- **色彩模式与数据大小**：
  - **BPP2（4色：黑、白、黄、红，高画质）**：每像素 2 bits，标准数据量为 **105,984 字节**（$552 \times 768 \times 2 / 8$）
  - **BPP1（2色：黑、白，极速刷屏）**：每像素 1 bit，标准数据量为 **52,992 字节**（$552 \times 768 \times 1 / 8$）

---

## 核心功能与技术亮点

### 1. 协议 V3 低功耗蓝牙传输引擎
- **自动化设备发现**：支持过滤以 `8adge-` 为前缀的 BLE 广播设备，实时展示 RSSI 信号强度。
- **高吞吐量协商与时间同步**：连接后自动协商 **MTU 517**、请求 **`CONNECTION_PRIORITY_HIGH`**，并自动下发 UTC 时间同步指令（`OP_SYNC_TIME`），校准墨水屏内部时钟。
- **物理刷新冷却保护**：支持 Protocol V3 物理刷新间隔防护（`EVT_REFRESH_BLOCKED`），防止频繁刷屏导致颜料粒子团聚损坏；支持放行授权确认（`OP_CONFIRM_REFRESH`）。
- **无丢包防爆缓冲区控制**：在发送 `WRITE_TYPE_NO_RESPONSE` 数据块时，建立动态避让与缓冲区防爆机制，确保全图数据连续高频投递不丢失。
- **4KB 块 CRC32 预注册与精准补传**：在传输前向 MCU 发送各 4KB 数据块的 CRC32 校验数组（`OP_BLOCK_CRCS`）。若传输后 MCU 校验发现局部坏块（`ERR_BLOCKS`），应用可通过 `OP_RETRY_BLOCK` 进行**单块精准补传修复**，无需重传全图。
- **状态机异常自动复位**：当设备响应 `ERR_STATE`（状态不匹配）时，应用会自动触发 `OP_ABORT` 重置 MCU 状态机至 IDLE，实现强鲁棒性容错恢复。

### 2. 全功能图像处理与 14 种抖动算法引擎
- **智能适配与裁剪**：支持等比裁剪（`CROP_FILL`）、等比留白（`FIT_PAD`）、拉伸铺满（`STRETCH`）三种缩放模式，并提供交互式比例锁定/自由拉伸矩形裁剪（`CropOverlayView`）。
- **专业级色彩调校**：支持饱和度、对比度、亮度、Gamma 矫正、拉普拉斯算子锐化（Sharpening）以及一键反色调整。
- **14 种内置图像抖动算法**（支持 4 色及 2 色半影过渡）：
  1. **Floyd-Steinberg 经典**：色彩过渡自然，通用性极强。
  2. **Atkinson 高清**：适合文字、漫画与高对比度线稿，画面干净无杂点。
  3. **JJN (Jarvis-Judice-Ninke) 柔和**：适合风景与平滑渐变图。
  4. **Stucki 细节**：适合精细建筑与复杂纹理。
  5. **Burkes 自然**：影调平滑且算法执行快速。
  6. **Sierra 3 柔和**：柔和人像与艺术写真。
  7. **Sierra 2 平衡**：过渡与清晰度均衡。
  8. **Sierra Lite 极速**：适合速写与简笔线条画。
  9. **Bayer 2x2 粗网格**：复古像素画质感。
  10. **Bayer 4x4 复古点阵**：经典有序点阵风格。
  11. **Bayer 8x8 细腻网格**：高分辨率细致交叉点阵。
  12. **Cluster Dot 4x4 半色调**：复刻报纸印刷品网点。
  13. **Random Noise 胶片颗粒**：带有艺术胶片质感。
  14. **Nearest Color 纯色无抖动**：适合图标、矢量图与文字线稿。
- **物理画板旋转匹配**：自动将转置后的调色板数据正确打包并按屏幕物理点阵存储（MSB 优先）。

### 3. 可视化交互与硬件测试工具
- **实时预渲染预览**：所有调色、旋转、裁剪及抖动效果均在后台协程中快速计算并实时呈现像素级预览。
- **硬件测试面板 Dialog**：内置红黄测试条、灰度渐变网格等专用测试图样，可一键测试屏幕颜色渲染；支持手动 PING 延迟测试、绘制提示屏（SHOW_HINT）及强行中止（ABORT）。
- **实时日志控制台**：直观展示蓝牙 TX/RX 指令帧、设备事件（`EVT_STATE`）、传输速度（KB/s）及错误日志。

---

## 项目技术栈

- **编程语言**：Kotlin (Java 11 字节码兼容)
- **构建系统**：Gradle 8.x + Android Gradle Plugin 9.4.1
- **UI 框架**：Material Design 3 + ViewBinding + Custom Views
- **架构模式**：MVVM（ViewModel + StateFlow / SharedFlow + Coroutines）
- **蓝牙通信**：Android Native BLE GATT (BluetoothGatt, ScanTracker)
- **单元测试**：JUnit 4

---

## 目录结构

```
8adge/
├── app/
│   ├── build.gradle.kts                # 子项目 Gradle 构建脚本
│   └── src/
│       ├── main/
│       │   ├── AndroidManifest.xml     # 清单文件（权限申明与 Activity 入口）
│       │   ├── java/com/file/eightbadge/
│       │   │   ├── MainActivity.kt     # 主界面 Activity，负责 UI 布局与事件交互
│       │   │   ├── ble/
│       │   │   │   ├── BleManager.kt   # BLE 连接管理、状态机控制与数据传输管道
│       │   │   │   └── BleProtocol.kt  # BLE 通信协议帧构建、解析与命令定义
│       │   │   ├── image/
│       │   │   │   └── ImageProcessor.kt # 图像处理引擎、调色、14 种抖动算法与打包
│       │   │   ├── model/
│       │   │   │   └── DataModels.kt   # 数据模型（屏幕方向、格式、抖动模式、设备信息等）
│       │   │   └── ui/
│       │   │       ├── MainViewModel.kt    # 主 ViewModel，管理 UI 状态与后台任务
│       │   │       ├── CropOverlayView.kt  # 自定义图片交互裁剪视图
│       │   │       ├── DeviceAdapter.kt    # 扫描设备列表 RecyclerView 适配器
│       │   │       └── LogAdapter.kt       # 控制台日志列表 RecyclerView 适配器
│       │   └── res/                        # 布局文件、图标及样式资源
│       └── test/                           # 单元测试
│           └── java/com/file/eightbadge/
│               ├── BleProtocolUnitTest.kt  # 协议帧构建与解析单元测试
│               ├── Crc32UnitTest.kt        # 标准 CRC32 校验向量测试
│               └── ImageProcessorUnitTest.kt # 2bpp/1bpp 打包与尺寸测试
├── gradle/
│   └── libs.versions.toml              # 依赖版本管理 (Version Catalog)
├── build.gradle.kts                    # 根构建脚本
├── settings.gradle.kts                 # 项目 Setting 脚本
└── README.md                           # 项目说明文档
```

---

## 蓝牙通信协议 (Protocol V3)

8adge 设备通过标准的 GATT 服务与特征值进行通信（UUID 包含 ASCII "BADG" `42414447`）：

- **Service UUID**: `42414447-0001-4e5a-9c1a-6b7c8d9e0f11`
- **Control Characteristic (WRITE / NOTIFY)**: `42414447-0002-4e5a-9c1a-6b7c8d9e0f11`
- **Data Characteristic (WRITE_WITHOUT_RESPONSE)**: `42414447-0003-4e5a-9c1a-6b7c8d9e0f11`

---

## 编译与运行指南

### 环境要求
- **Android Studio**：Ladybug (2024.2.1) 或更高版本
- **JDK**：Java 11 或 Java 17
- **Android 设备**：运行 Android 8.0 (API 26) 及以上系统的真实手机（蓝牙调试需要真实硬件支持）

### 构建步骤
1. 使用 Android Studio 打开项目根目录。
2. 等待 Gradle Sync 完成。
3. 连接 Android 设备，确保系统蓝牙与定位服务已开启。
4. 在 Android Studio 中运行 `:app` 模块（点击 Run 'app'）。
