# 第三方组件与参考项目许可说明

本文件记录 8adge 使用到的第三方代码、资源与参考项目，以及各自的许可证。
**结论：与本项目相关的两个源码项目都是 MIT，和 8adge 的 MIT 发布方式完全兼容**；
唯一需要额外注意的是 MakerWorld 上的外壳模型（不是代码，授权由模型作者在页面声明）。

## 1. 直接引用的源码（已保留上游版权声明）

| 组件 | 来源 | 许可证 | 本项目中的位置 |
|---|---|---|---|
| JD79665 寄存器序列 | [krstc/InkSight_adapt_HUAWEI_eink](https://github.com/krstc/InkSight_adapt_HUAWEI_eink)（`firmware/src/epd_driver.cpp`） | **MIT**，Copyright (c) 2026 datascale-ai | [`ESP32-C3/src/epd_398_4color.h`](ESP32-C3/src/epd_398_4color.h)、[`epd_398_4color.cpp`](ESP32-C3/src/epd_398_4color.cpp) |
| 上游 `firmware/src/` 的**逐字副本**（`config.h`、`display.{h,cpp}`、`epd_driver.{h,cpp}`） | 同上 | **MIT**，Copyright (c) 2026 datascale-ai | [`ESP32-C3/github-dev/`](ESP32-C3/github-dev/README.md)（对照用，**不参与编译**） |

上游 MIT 许可证要求保留版权与许可声明，因此：

- 上游声明已写入仓库根目录的 [`LICENSE`](LICENSE) 与本文件；
- 驱动源码文件头也写明了寄存器序列来自该 MIT 项目及被修改之处（VRES = 600、
  交错栅极寻址、按行窗口）。

## 2. 参考项目（**未使用其源码**）

| 项目 | 说明 | 许可证 | 是否使用其代码 |
|---|---|---|---|
| [墨鱼 / InkSight（datascale-ai/inksight）](https://github.com/datascale-ai/inksight)、[官网](https://www.inksight.site/zh) | 华为 3.98″ 面板适配生态的上游项目（WiFi 联网 + 后端 + 模式广场） | **MIT** | **否**（8adge 是纯 BLE 固件，架构与协议均自行编写） |
| [华为 3.98 寸墨水屏（支持 A0/A1 固件）ESP32-C3 mini 外壳适配 InkSight 墨水屏项目](https://makerworld.com.cn/zh/models/2694038)（MakerWorld 模型 2694038） | 3D 打印外壳与 8 线飞线硬件方案 | MakerWorld 模型授权，**由模型作者在模型页面声明**（MakerWorld 的模型授权常限制商用） | **否**（仅采用外壳与硬件方案，代码未使用；本仓库也不包含模型文件） |

> 说明：MakerWorld 页面有 Cloudflare 保护，无法在构建时自动读取其许可条款；
> 复用/再分发外壳模型（`.3mf`/`.stl`）前请以模型页面显示的授权为准。
> 该授权只约束模型文件本身，不影响以 MIT 发布的 8adge 代码。

## 3. 随仓库分发的资源

| 资源 | 来源 | 许可证 |
|---|---|---|
| `ESP32-C3/assets/fonts/HarmonyOS_Sans_SC_Bold.ttf` | HarmonyOS Sans（华为） | HarmonyOS Sans Fonts License，全文见 [`ESP32-C3/assets/fonts/HarmonyOS_Sans_LICENSE.txt`](ESP32-C3/assets/fonts/HarmonyOS_Sans_LICENSE.txt) |
| `ESP32-C3/src/ble_font.h` | 由上方字体生成（`tools/gen_ble_font.py`） | 同上（字形位图属于字体的衍生数据） |

## 4. 运行时依赖（**不随本仓库分发**，由各自的包管理器拉取）

| 依赖 | 许可证 |
|---|---|
| [NimBLE-Arduino](https://github.com/h2zero/NimBLE-Arduino) 1.4.3（PlatformIO） | Apache-2.0 |
| [Arduino-ESP32](https://github.com/espressif/arduino-esp32) 2.0.17 / ESP-IDF（PlatformIO） | LGPL-2.1-or-later（Arduino core）/ Apache-2.0（ESP-IDF 组件） |
| [PlatformIO](https://platformio.org/)（含 `espressif32` 平台） | Apache-2.0 |
| AndroidX（core-ktx / appcompat / activity / navigation / constraintlayout） | Apache-2.0 |
| [Material Components for Android](https://github.com/material-components/material-components-android) | Apache-2.0 |
| [JUnit 4](https://junit.org/junit4/)（`testImplementation`） | EPL-1.0 |
| AndroidX Test / Espresso（`androidTestImplementation`） | Apache-2.0 |

以上依赖均只影响分发二进制时的合规要求，不改变 8adge 源码本身的 MIT 授权。