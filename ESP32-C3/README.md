# 8adge BLE 固件

ESP32-C3-SuperMini + 3.98″ 四色墨水屏（A1 面板）的**纯蓝牙固件**：
手机通过 BLE 把一张做完的图片推给屏幕，设备校验通过后刷屏。

- **没有任何联网能力**：不连 WiFi、没有配网门户、不访问 HTTP/后端、没有 OTA、没有文件系统。
  唯一的入口是 BLE，手机能做的唯一一件事就是发一张图。
  唯一写 Flash 的是一小块 NVS 记录（16 字节，上次刷屏时间），否则防残影的 180 秒冷却
  没法跨断电生效。
- **上电不碰屏幕**：墨水屏断电保留画面，这是设计意图（不是"没刷新"）。
  2 分钟没有连接才画出提示屏（四色循环边框 + 中间黑色提示文字）。
- **刷新冷却（硬性）**：两次**整屏刷新**之间强制 **≥180 秒**。这不是可选项 —— 四色屏一次刷屏是
  15–25 秒的双极性交流电泳，间隔不足会让颜料粒子团聚，造成**不可逆**的串色与残影。
  冷却不足时设备回 `ERR_COOLDOWN` + 发事件，App 弹窗、用户确认后才放行；**默认不确认就不刷**。
  确认是受控例外：距上次不足 60 秒不放行，上次是强刷则需等满 180 秒。设备没有可信时钟，
  同一上电周期内 `SYNC_TIME` 的累计超前受 uptime 限制；上电后的第一次同步是信任边界。
  详见 [`docs/BLE_PROTOCOL.md`](docs/BLE_PROTOCOL.md) 的「刷新冷却」一节。
- **单中心**：固件主动断开第二个连接；提示屏绘制期间也占用帧缓冲，`BEGIN_IMAGE` / `SHOW_HINT`
  一律回 `ERR_BUSY`，不会出现“半张图 + 半张提示屏”。
- **竖屏工作**：手机按 **552×768** 画布作图，设备自己把画布顺时针旋转 90° 送上 768×552 的面板。
- **逐块校验**：4 KB 一块 CRC32，全部通过才刷屏；有坏块时只回传坏块位图，手机只重传那几块。
- **自包含**：提示屏字库的源字体（`assets/fonts/`）与生成好的字形表（`src/ble_font.h`）都在本目录里，
  整个工程只依赖 PlatformIO 的 Arduino-ESP32 与 NimBLE-Arduino，克隆本目录即可编译。

协议全文见 **[`docs/BLE_PROTOCOL.md`](docs/BLE_PROTOCOL.md)**
（完整规范：GATT 结构、逐帧格式、状态机、时序、字节级会话示例、Android 实现清单、自测清单，
可以直接交给实现 App 的开发者或 AI）。

---

## 1. 硬件

| 项 | 值 |
|---|---|
| MCU | ESP32-C3-SuperMini（4 MB Flash，无 PSRAM），PlatformIO board = `esp32-c3-devkitm-1` |
| 屏幕 | 3.98″ 四色墨水屏，面板原生 **768×552 横向**，2bpp（黑/白/黄/红），JD79665 |
| 屏线 | DIN→GPIO6、SCK→GPIO4、DC→GPIO1、CS→GPIO7、RST→GPIO2、BUSY→GPIO10 |
| 状态灯 | 板载蓝灯 GPIO8（**低电平点亮**） |
| 按键 | BOOT 键 GPIO9（`INPUT_PULLUP`，按下为低） |

## 2. 行为

| 场景 | 表现 |
|---|---|
| 上电 | 初始化 BLE，广播 `8adge-XXXXX`（`XXXXX` = 站点 MAC 后 5 位十六进制，大写）；不动面板 |
| 无连接满 2 分钟 | 画提示屏：四周 24 块四色循环色块（黑→白→红→黄，绕屏 6 圈）+ 内侧 3 px 黑色细线 + 中间三行黑字。**冷启动且 App 还没连上时可能推迟到开机约 3 分钟**（无时钟，冷却只能按 uptime 保守判断）|
| 收到图片 | 逐块校验 CRC32 → 全部通过才刷屏（15–25 s）→ 发 `EVT_STATE(DISPLAYED)`；校验不过屏幕保持原样（失败响应后设备留在接收态并补发 `EVT_STATE(RECEIVING)`，App 不会卡在 `VERIFYING`）|
| **冷却不足** | 距上次整屏刷新 <180 s 时，`BEGIN_IMAGE` / `SHOW_HINT` 回 `ERR_COOLDOWN` + `EVT_REFRESH_BLOCKED`；App 弹窗，用户确认（`CONFIRM_REFRESH`）后才刷，不确认就不刷。确认是**受控例外**：距上次不足 60 秒不放行，上次是强刷则需等满 180 秒 |
| 刷屏期间 | `BEGIN_IMAGE` / `SHOW_HINT` 都回 `ERR_BUSY`（图片和提示屏都会占用帧缓冲）；连接不断，通知照发（刷屏在 `loop()` 里做，不在 BLE 回调里） |
| 刷屏期间还在流水线写 `DATA` | 字节被忽略（不能改写正在送屏的缓冲）。这类写入不再有任何回包，所以：之后若发 `END_IMAGE`，回 `ERR_STATE` + 1 字节 `data_hint = 1`；App 应据此**重算窗口计数**并重新 `BEGIN_IMAGE`。设备限频打一行日志 |
| 多发/重复发的尾块 | 超出 `total_len` 的字节被忽略，**不会**再让一张已通过校验的图作废；只有最终校验失败时才回 `ERR_OVERFLOW` |
| 校验完成但断线 | 已通过全部 CRC 的图片**不会丢**，仍会照常刷出；只有 `ABORT` 才取消 |
| MTU 太小 | `GET_INFO` 回 `ERR_MTU`（而不是发半截帧）；块校验模式下失败位图装不下时 `BEGIN_IMAGE` 也回 `ERR_MTU` |
| 以后再次空闲 | **不再**自动覆盖提示屏（一旦成功显示过手机图片） |
| App 主动要提示屏 | `SHOW_HINT`（也可以用 `SET_OPTIONS` 关掉自动提示） |
| 状态灯 | 慢闪 = 广播中；常亮 = 已连接；快闪 = 传输中 / 出错 |
| BOOT 键 | 短按 = 立刻画提示屏；长按 8 秒 = 重启 |

**竖屏**：画布 `552 × 768`（宽 × 高），面板 `768 × 552`；旋转是纯像素置换，不缩放不裁切，
所以 2bpp 仍是 105984 字节、1bpp 仍是 52992 字节。旋转角由 `src/config.h` 的 `BLE_ROTATION`
决定（只能 90 或 270），手机端**不需要**做任何旋转。

## 3. 编译 / 烧录

```powershell
# 依赖 PlatformIO；首次编译会自动拉取 espressif32 平台与 NimBLE-Arduino 1.4.3
pio run                       # 编译
pio run -t upload             # 编译并烧录
pio device monitor            # 串口日志，115200
```

`pio run` 结束后 `merge_firmware.py` 会把 bootloader + 分区表 + app 合并成一个镜像，
发布到本工程自己的 `bin/` 目录：

```
bin/8adge_BLE_398_A1.bin     烧 0x0，一个文件搞定
bin/SHA256.txt                文件名 / 字节数 / SHA256
```

```powershell
# 只用合并镜像烧录（不需要再单独烧 bootloader / partitions / boot_app0）
esptool.py --chip esp32c3 --port COMx write_flash 0x0 bin\8adge_BLE_398_A1.bin
```

关键依赖与占用：`h2zero/NimBLE-Arduino@1.4.3`（**必须锁 1.4.x**：2.x 需要 Arduino-ESP32 3.x / IDF 5，
本项目 core 3.20017 = Arduino-ESP32 2.0.17）；NimBLE 比内置 Bluedroid 省约 400 KB flash；
分区表 `min_spiffs.csv`；v1.3.1 实测 **RAM 7.5%（24572 B）/ Flash 29.6%（581342 B）**
（`pio run` release，framework-arduinoespressif32 3.20017 + NimBLE-Arduino 1.4.3，无警告）；
合并镜像 `bin/8adge_BLE_398_A1.bin` 为 674352 字节。

## 4. 目录结构

```
ESP32-C3/
├── platformio.ini              单一环境 ble_398_a1_c3_supermini
├── merge_firmware.py           合并镜像 → bin/8adge_BLE_398_A1.bin + bin/SHA256.txt
├── assets/fonts/
│   ├── HarmonyOS_Sans_SC_Bold.ttf   提示屏字库的源字体（HarmonyOS Sans Fonts License）
│   └── HarmonyOS_Sans_LICENSE.txt   字体许可证
├── docs/
│   └── BLE_PROTOCOL.md         ★★ 完整 BLE 协议规范（写 App 的唯一依据）
├── src/
│   ├── main.cpp                上电 / 2 分钟空闲计时 / 按键 / 刷屏调度
│   ├── ble_protocol.h          ★ 协议常量（UUID、opcode、状态码、格式、画布尺寸）
│   ├── ble_geometry.h          ★ 画布→面板旋转（竖屏的唯一实现处）
│   ├── ble_service.{h,cpp}     ★ NimBLE GATT 服务 + 协议引擎（收发、CRC、流控、旋转写入）
│   ├── ble_screen.{h,cpp}      提示屏（四色循环边框 + 黑字）
│   ├── ble_font.h              提示屏字形表（**生成物**，勿手改）
│   ├── refresh_guard.{h,cpp}   ★ 整屏刷新冷却（180 秒，防残影；时钟来自 App）
│   ├── epd_398_4color.{h,cpp}  屏驱（JD79665 寄存器序列 + 本面板交织栅极寻址）
│   ├── led.{h,cpp}             非阻塞 LED 状态机
│   └── config.h                引脚、BLE 名称规则、BLE_ROTATION、提示屏尺寸、计时、版本号
└── tools/
    ├── gen_ble_font.py         从 assets/fonts 的 TTF 生成 src/ble_font.h
    ├── preview_hint_screen.py  在电脑上把提示屏渲染成 PNG（不烧录就能验版式，并自检）
    ├── check_receive.py        ★ 逐字节对拍接收路径（旋转/块校验/重传/整图重算）
    ├── check_refresh_guard.py  ★ 对拍 180 秒冷却状态机（时钟钳制/强刷下限/授权绑定）
    └── check_docs.py           核对 docs/BLE_PROTOCOL.md 里的常量与头文件一致
```

## 5. 工具

`gen_ble_font.py` 与 `preview_hint_screen.py` 只需要 Pillow（`pip install pillow`），
`check_receive.py`、`check_refresh_guard.py` 与 `check_docs.py` 只用 Python 标准库，
五个脚本都不依赖本工程以外的任何文件。

```powershell
python tools\gen_ble_font.py                      # 改提示屏文案后重新生成 src/ble_font.h
python tools\preview_hint_screen.py --out out.png # 不烧录验版式（输出竖屏画布 + 面板两张 PNG，并自检）
python tools\check_receive.py                     # 改 handleData() / 块校验 / BLE_ROTATION 后必跑
python tools\check_refresh_guard.py               # 改 refresh_guard.cpp 后必跑
python tools\check_docs.py                        # 改协议常量或协议文档后必跑
```

- **改提示屏文案**要同步三处：`tools/gen_ble_font.py` 的 `BLE_STRINGS`、`src/ble_screen.cpp` 的
  `HINT_TITLE`/`HINT_FOOTER`，然后重跑 `gen_ble_font.py`。
- `check_receive.py` 是 `ble_service.cpp::handleData()` / `recomputeWholeCrc()` 的一份逐行转写，
  改那两个函数必须同步改它（它已经抓出过两个真 bug）。

## 6. 改代码前必读

- **协议格式号必须用 `uint8_t` 存，不能写成 `bool`**：`BLE_FMT_2BPP = 1`、`BLE_FMT_1BPP = 2`。
  用 `bool` 会把 1bpp 截成 `true`（=1），接收、旋转和 `END_IMAGE` 重算全部按 2bpp 走，
  现象就是 1bpp 52992 字节收满却永远 `ERR_CRC`。`tools/check_receive.py` 会静态检查这一行。
- **传输静默超时**：`loop()` 每轮调用 `blePollTransferTimeout()`；连接保持但 15 秒没有
  DATA / `BLOCK_CRCS` / `RETRY_BLOCK` 就自动复位到 `CONNECTED`（`BEGIN_IMAGE` 之后、
  首个 DATA 之前是 60 秒宽限），避免 App 异常退出后设备永久停在 `RECEIVING`
  （表现为后续 `BEGIN_IMAGE` 一直 `ERR_STATE`）。
- **只分配一块 105984 B 帧缓冲并全程复用**（在 `NimBLEDevice::init()` 之前 `malloc`，保证连续）；
  提示屏共用它，否则 BLE 栈起来后再要第二块 106 KB 很可能失败。
- **两条绘制路径都必须先占用帧缓冲**：图片用 `bleTakeRenderRequest()`（由它置位），
  提示屏用 `bleBeginHintPaint()`；结束后统一 `bleRenderFinished()`。漏掉提示屏这一处，
  手机就能在提示屏绘制的 15–25 秒里开始传图，把数据写进正在送屏的缓冲。
- **跨任务状态必须拿锁**：`ble_service.cpp` 的会话状态同时被 NimBLE host 任务和 `loop()`
  访问（超时复位、渲染交接），只用 `volatile` 不够；所有入口用 `StateLock`（递归互斥）。
  `refresh_guard.cpp` 内部同样有 `GuardLock`。锁顺序固定为 **state → guard**。
- **不要用 `setValue()+notify()` 发帧**：两者不是原子对，可能把 A 的字节配 B 的通知发出去；
  `notify(value, len)` 一次完成拷贝，且 CONTROL 本来就没有 READ 属性。
- **上电不要调 `epd398Init()`**（它会复位面板，整屏闪一下）；引脚配置推迟到第一次真正要画
  （`ensurePanelGpio()`，驱动内部也会兜底）。
- **刷屏不在 BLE 回调里做**：`bleTakeRenderRequest()` 把校验通过的图交回 `loop()`，
  并锁住帧缓冲（`bleRenderFinished()` 释放），否则手机在 `DISPLAYED` 之前发下一张会改写屏幕正在用的字节。
- **旋转在接收时逐像素完成**（`bleSetCanvasPixel()`），所以只需要一块缓冲，且分块边界落在哪里都不影响结果。
- **整图 CRC 在 `END_IMAGE` 时从帧缓冲重算**，不要边收边算 —— 重传之后运行中的 CRC 必然失真。
- **广播包里只放名字，服务 UUID 放扫描响应**：`3 + 18 + 16 > 31`，`addServiceUUID()` 会把名字挤出去，
  被动扫描就看不到 `8adge-XXXXX`。
- **MAC 字节序**：名字后缀用 `esp_read_mac(mac, ESP_MAC_WIFI_STA)`（大端，`mac[0]` 是第一个字节）。
  用 `ESP.getEfuseMac()` 时低字节才是第一个字节；`ESP_MAC_WIFI_STA` 是**枚举值不是宏**，
  写成 `#if defined(...)` 恒为假，会把整段路径编译掉（曾因此得到倒序名字）。
  自检：串口日志的 `device name ... (mac ...)` 与 `esptool` 的 `MAC:` 行对照。
- **NimBLE 1.4.3 的回调签名**是 `onConnect(NimBLEServer*, ble_gap_conn_desc*)` /
  `onWrite(NimBLECharacteristic*, ble_gap_conn_desc*)`（`NimBLEConnInfo&` 只在 2.x 存在）。
- `platformio.ini` **不能有 UTF-8 BOM**，否则报 `File contains no section headers`。
- 色序自检：提示屏的四色边框本身就是测试图（黑→白→红→黄绕一圈，红黄颠倒一眼可见）。
- **每次整屏刷屏后必须调 `refreshGuardNoteRefresh(opcode)`**（图片传 `BLE_OP_BEGIN_IMAGE`，
  App 请求的提示屏传 `BLE_OP_SHOW_HINT`，按键/空闲提示屏传 `0`；一视同仁）。
  漏一处就等于给面板留了一个可以无限高频刷新的入口，而那种损伤是不可逆的。
- **刷屏前的准入用 `refreshGuardMayRefreshFor(opcode)`**，不要自己拿 `millis()` 去算间隔：
  设备没有时钟（不联网、RTC 断电即失），冷启动/拔电后的间隔只能靠 uptime 下界保守推断，
  这套逻辑集中在 `refresh_guard.cpp` 里。时钟由 App 的 `SYNC_TIME`（UTC 秒）提供，
  且同一上电周期内累计超前 uptime 超过 30 秒的同步会被拒；时钟落后于 NVS 记录时退回
  uptime 判断（不会永久锁死）。App 确认的授权只对被拒的那条命令有效，强刷还有 60 秒
  / 180 秒的下限。
- 冷却规则改动时，`docs/BLE_PROTOCOL.md` 的「刷新冷却」、`ble_protocol.h` 的
  `BLE_REFRESH_MIN_GAP_S` 与 `config.h` 的 `BLE_REFRESH_MIN_GAP_MS` 必须一致
  （`refresh_guard.cpp` 里有 `static_assert` 拦一条，跨文档的靠 `tools/check_docs.py`）。
- **`END_IMAGE` 成功响应只能发 8 字节**：`r[]` 数组是给失败位图用的（26 字节），
  成功路径里 `sizeof(r)` 会把 18 字节未初始化栈内存发出去，App 侧严格校验长度就会失败。
- **`total_len` 之后的 DATA 一律忽略**（`take` 截断 + `s_strayBytes` 计数），
  不要在 `handleData()` 里把它当致命错误：那会让一张已通过校验的图整张作废。
  只有校验失败**且**存在 `s_strayBytes` 时才回 `ERR_OVERFLOW`。
- **`END_IMAGE` 无会话时的答复是 `ERR_STATE` + 1 字节 `data_hint`**（`ble_protocol.h` 的
  `BLE_HINT_*`，文档 §6.3）。它是流水线 App 唯一的线索：刷屏期间写进去的 DATA 不会得到
  任何回包，`EVT_PROGRESS` 也不会再来。改这里要同步 §6.3 和真机自测清单第 33 条。
- **收包路径的日志要限频**（`throttleLogDue()`，`BLE_LOG_THROTTLE_MS`）：`Serial.printf`
  跑在 NimBLE host 任务上，而 USB CDC 在“主机连着却不取数据”时会阻塞，坏块日志一多
  就可能把 BLE 拖住。逐块日志一律走 `throttleLogDue()`。
- **面板几何只有一处**：`epd_398_4color.h` 的 `EPD398_*`。不要再往 `config.h` 或
  `platformio.ini` 的 `-D` 里放第二份 —— 画布/面板的关系由 `ble_geometry.h` 的
  `static_assert` 在编译期把关。
- **驱动公开入口都要 `ensureGpio()`**（`epd398Init/Upload2bpp/PowerOff/Sleep/WaitBusyTimed`）：
  头文件承诺“可以单独调用”，而漏掉一处就会往未配置的引脚写电平，表现为面板毫无反应。
- **引脚映射与 `EPD398_VRES` 由编译器把关**（`config.h` 的 `pinsUnique`/`pinIn` 断言 +
  `epd_398_4color.h` 的 VRES 上下界）：重复引脚、让面板占用 strapping 脚（GPIO8/9）或
  USB 脚（GPIO18/19）、把 VRES 改回 552（会造成"下半屏缺失 + 24 行残影"），都会**编译失败**
  并给出中文/英文说明，而不是等到烧录后看现象。
- **`RETRY_BLOCK` 一次只允许一块**：`s_fixing` 为真（上一块还差字节）时再请求别的块要回
  `ERR_BUSY`，不要退回“静默换块”——那会丢掉 App 已经发出去的那半块，而 App 只会看到位图
  没变，于是重复发同一段。文档 §5.9 与自测清单第 37 条是这条规则的对照点。
- **改完必须真编译一次（`pio run`）**：协议路径里有几类错误只有编译器会抓，脚本与人工复核都看不见——
  本轮 1.3.1 的修复就是靠一次真实编译才发现 `recomputeWholeCrc()` 被误删，以及
  NimBLE 1.4.3 的 `updateConnParams()` / `setDataLen()` 返回的是 `void`（不能接 `int`）。
  注意：`pio run` 会顺带跑 `merge_firmware.py`，重新生成 `bin/8adge_BLE_398_A1.bin` 与 `bin/SHA256.txt`。

## 7. 来源与许可

- **屏驱有上游**：`src/epd_398_4color.{h,cpp}` 的 JD79665 寄存器序列取自
  [krstc/InkSight_adapt_HUAWEI_eink](https://github.com/krstc/InkSight_adapt_HUAWEI_eink)
  的 `firmware/src/epd_driver.cpp`（**MIT**，© 2026 datascale-ai）。本项目在其基础上修正了
  刷屏问题（VRES = 600、交错栅极寻址、按行窗口），每条改动的理由都写在
  `epd_398_4color.h` 的文件头里；上游版权与许可声明按 MIT 要求保留。
- **其余固件代码自行编写**：BLE 协议 v3、协议引擎、180 秒刷新冷却、提示屏，均未复制
  上游「墨鱼 / InkSight」项目的任何源码（该项目同为 MIT，本固件只是参考对象）。
- **上游参考副本**：[`github-dev/`](github-dev/README.md) 是 krstc 那份华为 A1 适配
  `firmware/src/` 的逐字副本（**MIT**，© 2026 datascale-ai），只用于对照，**不参与编译**
  （PlatformIO 只编译 `src/`）。
- **外壳与硬件方案**来自 MakerWorld 模型 2694038（「华为 3.98 寸墨水屏（支持 A0/A1 固件）
  ESP32-C3 mini 外壳适配 InkSight 墨水屏项目」），仅采用其外壳与接线方案，未使用其代码。
- 本项目采用 **MIT License**，详见仓库根目录
  [`LICENSE`](../LICENSE)、[`THIRD_PARTY_NOTICES.md`](../THIRD_PARTY_NOTICES.md)、
  以及 [根 README 的「参考项目与血缘」](../README.md#参考项目与血缘建议优先阅读)。
