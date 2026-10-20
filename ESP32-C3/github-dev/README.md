# 上游参考副本（**不是**本固件的构建输入）

本目录保存 [krstc/InkSight_adapt_HUAWEI_eink](https://github.com/krstc/InkSight_adapt_HUAWEI_eink)
`firmware/src/` 的**逐字副本**，只用于与本工程的 `../src/` 对照、定位差异与复核寄存器序列。

| 本目录文件 | 上游路径 |
|---|---|
| [`config.h`](config.h) | `firmware/src/config.h` |
| [`display.cpp`](display.cpp) / [`display.h`](display.h) | `firmware/src/display.cpp` / `display.h` |
| [`epd_driver.cpp`](epd_driver.cpp) / [`epd_driver.h`](epd_driver.h) | `firmware/src/epd_driver.cpp` / `epd_driver.h` |

- **许可证**：MIT License，Copyright (c) 2026 datascale-ai，与上游仓库一致。上游版权与许可声明
  随仓库根目录的 [`LICENSE`](../../LICENSE)、[`THIRD_PARTY_NOTICES.md`](../../THIRD_PARTY_NOTICES.md) 保留。
- **不参与编译**：PlatformIO 只编译 `../src/`（见 [`../platformio.ini`](../platformio.ini)），
  本目录既不在 `src_dir` 里，也没有被任何 `-I` 加入包含路径。
  本工程真正使用的屏驱是 [`../src/epd_398_4color.{h,cpp}`](../src/epd_398_4color.cpp)，
  其 JD79665 寄存器序列取自 `epd_driver.cpp`，并修正了刷屏问题
  （VRES = 600、交错栅极寻址、按行窗口；理由写在 `epd_398_4color.h` 的文件头）。
- **注意**：本目录的 `config.h` 是上游的（头文件保护宏是 `INKSIGHT_CONFIG_H`），
  与本工程自己的 [`../src/config.h`](../src/config.h) 无关，不要混用。