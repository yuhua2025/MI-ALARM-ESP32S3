# 小米闹钟 · XIAO ESP32-S3 主控固件

> 当前版本 **v4.1**（以 `ino.ino` 的 `FW_VERSION` 为准；v4.1 = v4.0 + 「唤醒体验」第 1 步，见第 9 节）
> 由 `MI-ALARM-ESP32C3/ESP32C3-PCB/ino` 的 **v3.15** 移植而来，原 C3 工程保持不动。
> 新硬件：ESP32-S3 自画裸片板 + TM1652 灯板 + MAX98357(I2S) + ICS-43434(I2S) + WS2812B×2 + 双按键
> 完整引脚与电气注意见 [`../硬件接线表.md`](../硬件接线表.md)。

---

## 1. 这轮移植改了什么

| 模块 | C3 版 | S3 版 |
|---|---|---|
| 主控 | ESP32-C3，4MB Flash | XIAO ESP32-S3，8MB Flash（分区 `default_8MB`） |
| 时间 | DS1302 + CR2032 后备电池 | **无外部 RTC**，只靠 NTP；断电丢时间，联网后几秒恢复 |
| 显示 | TM1652 SDA = GPIO6 | TM1652 SDA = **GPIO1**，驱动与段码表原样保留 |
| 按键 | 主 GPIO2 / 副 GPIO10 | 主 **GPIO0(BOOT)** / 副 **GPIO2** |
| 音频 | LEDC 8bit 方波 → PAM8403 | **I2S 44.1kHz/16bit → MAX98357**，独立 FreeRTOS 任务喂 DMA |
| 亮度 | 无光敏，按时段 | 同左（新板也没有光敏） |
| 麦克风 | 无 | **新增 ICS-43434**：I2S_NUM_1，16kHz/32bit，`/api/mic` 电平自检 |
| 彩灯 | 无 | **新增 WS2812B×2**：GPIO10，RMT 单帧 48bit，网页开关 + 10 档亮度 |
| 发射功率 | 临时限 6dBm（旧板硬件缺陷） | 恢复 **20dBm**（模组自带天线/匹配） |
| 配网/网页/闹钟/铃声 | 全部保留 | 除上表外逻辑一行未改；4 种铃声、5 组闹钟、贪睡、整点报时、试听接口全在 |

**没有移植的部分（本轮按你的选择跳过）**：C3 工程里的 `WEB/` 在线烧录页仍是 ESP32-C3 + 4MB 镜像，不要拿它烧新板。

---

## 2. 文件

| 路径 | 说明 |
|---|---|
| `ino.ino` | 主固件：显示 / 时间 / WiFi / Web / 闹钟 / 音频 / 麦克风 / 彩灯 |
| `web_pages.h` | 设置页与配网页 HTML/CSS/JS（放 `.h` 避免 Arduino builder 误判 JS 的 `function`） |
| `verify_contract.py` | 契约自检：网页占位符 ↔ `handleRoot()`、`save()` 参数 ↔ `handleSave()`、引脚宏 ↔ 接线表 |
| `build.sh` | 编译 → 自检 → 导出 bin → sha256 |
| `build/` | 编译产物与发布 bin |
| `../硬件接线表.md` | 完整引脚表、电气风险、上电自检对照 |

---

## 3. 编译

环境（本机实测）：`arduino-cli 1.3.1` + `esp32:esp32 3.3.12`。
`ESP_I2S`、`WiFi`、`WebServer`、`DNSServer`、`Preferences` 都是内核自带，**不需要装 Ds1302 等第三方库**；TM1652 是 GPIO 位模拟，WS2812B 用内核 RMT。

```bash
cd "/Volumes/macmini/DIY/小米闹钟/MI-ALARM-ESP32S3/ESP32S3-PCB/ino"
sh build.sh
```

等价的手动命令：

```bash
arduino-cli compile --fqbn esp32:esp32:XIAO_ESP32S3 --build-path build .
```

> ⚠️ **不要照抄 C3 版的 `CDCOnBoot=cdc`**：`XIAO_ESP32S3` 这个板型的选项含义是反的，
> `CDCOnBoot=cdc` = **Disabled**；默认（`CDCOnBoot=default`）才是 Enabled，日志走 USB CDC。
> 编译产物里已确认 `-DARDUINO_USB_MODE=1 -DARDUINO_USB_CDC_ON_BOOT=1`。
>
> `--build-path build` 不能省，否则 bin 会落到 Arduino 缓存目录而 `build/` 里还是旧文件。

---

## 4. 烧录

**推荐用带身份校验的一键脚本**：它先用 esptool 自动检测芯片，只有明确识别为 ESP32-S3 才写 Flash，并会主动跳过 ZMK 键盘等非目标串口设备（本机 `/dev/cu.usbmodem312401` 就是一块 ZMK 键盘 dongle，误刷会毁掉键盘固件）。

```bash
sh flash.sh                                      # 自动探测端口，烧 app（保留设置）
sh flash.sh /dev/cu.usbmodemXXXXX                # 指定端口
sh flash.sh /dev/cu.usbmodemXXXXX --full         # 烧 8MB 全片镜像（清 NVS）
```

串口设备名按实际填写（用 `ls /dev/cu.*` 查），工具 `esptool v5.x`。等价的手动命令如下。

**日常升级（保留 WiFi 与全部设置）**

```bash
python3 -m esptool --chip esp32s3 -p /dev/cu.usbmodemXXXXX -b 921600 \
  write-flash 0x10000 build/XiaoMiClock_v4.0_esp32s3_app.bin
```

**新板首烧 / 全片（8MB，会清空 NVS 与设置）**

```bash
python3 -m esptool --chip esp32s3 -p /dev/cu.usbmodemXXXXX -b 921600 \
  write-flash 0x0 build/XiaoMiClock_v4.0_esp32s3_merged.bin
```

连不上时按住板上的 **BOOT(GPIO0)** 再点一下 **RESET(CHIP_PU)**，松开 BOOT 手动进下载模式；
烧完补一条复位：`python3 -m esptool --chip esp32s3 -p /dev/cu.usbmodemXXXXX --after hard-reset flash-id`。
串口日志 115200bps。

---

## 5. 上电与实测对照（拿到板子后按这一节走）

开机串口应当依次出现：

```
[RGB] WS2812B×2 ok  GPIO10
[SPK] MAX98357 I2S ok  BCLK=8 LRCK=9 DIN=7  44100Hz/16bit
[MIC] ICS-43434 I2S ok  SCK=5 WS=4 SD=6  16000Hz/32bit
[BOOT] XiaoMiClock v4.0  WiFi=(未配置)
[AP] 热点 XiaoMiClock 已开  http://192.168.4.1
[NTP] 校时成功 2026-11-15 11:03:20
```

连上 `XiaoMiClock` 热点配网后，用路由器分配的 IP（下用 `$IP` 代替）逐条验证：

```bash
IP=192.168.8.129
curl "$IP/api/status"                              # 时间、WiFi、亮度、响铃状态
curl "$IP/api/mic"                                 # {"ok":true,"rms":N,"peak":N,"rate":16000}
curl "$IP/audiotest?t=alarm"                       # 应听到闹钟铃声（只放一遍）
curl "$IP/audiostop"                               # 立即静音
curl "$IP/save?rgb=1&rgbb=8"                       # 彩灯开、亮度 8 档
curl "$IP/save?rgb=0"                              # 彩灯关（缺参数保留原值）
curl "$IP/sync"                                    # ok = NTP 校时成功
curl "$IP/api/alarms"                              # 闹钟数组
```

麦克风自检：静止时 `peak` 小且平稳，对着麦克风吹气/说话应明显变大；全 0 不动要查 SCK/WS/供电/L-R 选择脚。

---

## 6. HTTP 接口（相对 C3 版的变化）

原有 `/`、`/wifi`、`/scan`、`/connect`、`/netstatus`、`/portal`、`/save`、`/api/status`、`/sync`、`/api/alarms`、`/alarm`、`/alarmadd`、`/alarmdel`、`/audiotest`、`/audiostop`、`/resetwifi` 全部保留、语义不变。

| 新增 | 说明 |
|---|---|
| `GET /api/mic` | 麦克风电平：`ok / rms / peak / rate / nz / raw` |
| `/save?rgb=0\|1` | WS2812B 总开关 |
| `/save?rgbb=1..10` | WS2812B 亮度 |

新增 NVS 键：`clock` 命名空间下的 `rgbon`、`rgbb`。老配置里没有这两个键，直接取默认值（开 / 4 档）。

**v4.1 追加**（详见第 9 节）：

| 新增 | 说明 |
|---|---|
| `GET /api/rings` | SPIFFS 用量 + 4 个 WAV 铃声文件清单（是否存在 / 字节数） |
| `GET /api/wavprobe?i=4..7` | 读回 WAV 头（RIFF/WAVE/声道/采样率/位深/dataBytes）与前 8 个样本 |
| `GET /api/audio` | 音频引擎状态：`mode / melody / path / rate / frames / pos / loop / vol` |
| `GET /api/sleep` | 查助眠状态；`?on=1[&min=…]` 开始，`?on=0` 停止 |
| `GET /api/sunrise` | 日出开关、提前分钟数、当前 `progress`（不在窗口返回 `null`） |
| `POST /api/ring` | multipart 上传自定义 WAV（字段名 `file`），返回 `ok / size` |
| `/save?sron=0\|1` | 日出唤醒开关 |
| `/save?srm=5\|10\|15\|20\|30` | 日出提前分钟数 |
| `/save?sls=0\|1\|2` | 助眠音效：雨声 / 海浪 / 仅灯效 |
| `/save?slmin=0\|15\|30\|45\|60` | 助眠时长，0 = 手动停止 |
| `/save?atone=4..7`、`?ctone=4..7` | 铃声索引扩展，4 起是 WAV 铃声 |

新增 NVS 键：`sron`、`srm`、`sls`、`slmin`。老配置没有这些键时取默认值（日出开 / 15 分钟 / 雨声 / 30 分钟）。
`/save` 仍是「带参数才改」，手工 curl 可以只改一个字段；非法档位会被忽略而不是钳到边界。

---

## 7. 已知限制 / 装机前要确认

1. **没有 RTC**：断电后时间丢失，屏上会走等待动画，联网校时后才显示时间；断网期间闹钟不会响。这是硬件决定的，固件无解。要掉电走时只能加 DS1302/PCF85063 之类并留出 3 根 GPIO。
2. **GPIO1 = TM1652 单线数据线**，按你的确认处理。原始速记里的 `TM1652EN` 若是别的意思（比如电源使能），告诉我实际 SDA 脚位，`#define PIN_TM1652` 和接线表一起改即可。
3. **别叠 Sense 扩展板**：扩展板 SD 卡占 GPIO7/8/9（带上拉）、摄像头占 GPIO10，正好和本板 I2S 音频、彩灯打架。详见接线表第 6 节。
4. **GPIO0 是 strapping 脚**：上电瞬间按住 BOOT 键会进下载模式；跑起来后当主按键用没问题。
5. TM1652 与 WS2812B 都是 5V 器件、3.3V 数据，处在 VIH 边缘；换批次点亮异常时按接线表加电平转换或降压。
6. ICS-43434 的 L/R 选择脚要固定到 GND 或 VDD，别悬空；固件统计两个声道，接哪边都能读数。
7. `I2S_AMP_MAX = 10000` 是保守值，装机后按实际响度调；MAX98357 的 SD/GAIN 脚按 datasheet 接好，否则会表现为「完全无声」。
8. **Flash 实测 16MB，但固件仍按 `default_8MB.csv` 编译**：spiffs 分区只有 **1,438,481 字节**（约 1.4MB）。v4.1 内置 3 段 WAV 占 801,192 字节，自定义铃声上限 600KB，基本用满。要放长音频 / 语音模型再切 16MB 分区（`XIAO_ESP32S3` 板型菜单里没有 16MB 选项，需要自定义 partition CSV + `build.flash_size`）。
9. **首次开机有约 15 秒不响应 HTTP**：`SPIFFS.begin(true)` 首次会格式化分区（串口出现一次 `E (266) SPIFFS: mount failed, -10025`，是自动格式化的正常提示，不是故障），随后合成 3 段 WAV（实测 9.3s + 2.6s + 2.7s）。之后开机检测到文件已存在就跳过，2 秒内正常联网。
10. **上传 WAV 只接受 16bit PCM、单/双声道、8k~48kHz、≤600KB**。播放时线性插值重采样到 44.1kHz；首尾不为零的 WAV 循环播放时接缝可能有「咔」声，内置 3 段已做 50ms 首尾淡入淡出。

---

## 8. 本轮验证边界

- ✅ `arduino-cli 1.3.1 / esp32:esp32 3.3.12` 以 `esp32:esp32:XIAO_ESP32S3` **编译通过**（app 1,004,480 B，占 3MB 分区 30%；RAM 48,432 B / 14%），`--warnings all` 下源码无告警。
- ✅ ELF 符号核对：新模块 `audioInit / i2sFeedTask / micInit / micMeasure / wsInit / wsSend / wsTask / handleMic / syncFromNtp` 全部在，`Ds1302` 相关符号已消失。
- ✅ 编译宏核对：`ARDUINO_USB_MODE=1`、`ARDUINO_USB_CDC_ON_BOOT=1`；分区 `app0 @ 0x10000`、merged 镜像 8MB。
- ✅ `verify_contract.py`：19 个网页占位符、15 个 `/save` 参数、10 个引脚宏全部对齐。
- ⚠️ **未做烧录 / 串口 / curl 实测**：11:17 用户接入设备后复查，系统里只有两个 USB 串口——`/dev/cu.usbmodem312401` 持续输出 `<dbg> zmk: split_central_device_found` 日志，是 ZMK 键盘 dongle（USB 名 `Dayu k30 Dongle`，VID 0x1D50），**不是目标板**；`/dev/cu.wchusbserial2110` 是 CH340 转换器（VID 0x1A86 / PID 0x7522），三种波特率、DTR/RTS 手动复位时序下均 0 字节、esptool 无响应。USB 设备树里没有 Espressif VID 0x303A，`arduino-cli board list` 也识别不到任何板卡。因此**主动停止，没有执行任何 write-flash**，避免刷掉键盘固件。
- 新增 `flash.sh`：自动探测端口 → 校验芯片确为 ESP32-S3 → 才允许烧录；当前运行结果正确挡下两个非目标端口。接上新板后直接 `sh flash.sh` 即可，随后按第 5 节做串口与 curl 实测。
- 11:40 起用 `wait_flash.sh` 挂了 300 秒等待「手动 BOOT+RESET 进下载模式」，**超时未检测到 ESP32-S3，未写入任何数据**。期间对 `cu.wchusbserial2110` 反复探测（自动检测 / `--chip esp32s3` / `no-reset` / `default-reset` / `usb-reset` / DTR-RTS 手动时序）全部 0 字节；`arduino-cli upload --fqbn esp32:esp32:XIAO_ESP32S3 -p /dev/cu.wchusbserial2110`（等价 Arduino IDE 上传）同样报 `Failed to connect to ESP32-S3: No serial data received`。判定：该 CH340 口后面没有进入下载模式或没上电的 ESP32，属硬件连接问题，不是端口/脚本选择问题。
- **注意日志出口**：固件按 `XIAO_ESP32S3` 默认编译，`Serial` 走模组原生 USB CDC（hwcdc）。若最终通过 CH340/UART0 烧录成功，开机日志不会从 CH340 出来，需要再接模组自带的 USB-C 看 115200 日志；必要时可改一版让日志同时输出到 `Serial0`(UART0)。
- 原 C3 工程与 `WEB/` 目录未改动。

### 16:08 根因定位 + 首次烧录 / 串口实测通过（上面 11:17、11:40 的受阻记录到此作废）

- ✅ **根因是硬件电源**：板上 `VDD3P3_RTC` 悬空未接 3.3V。官方《ESP32-S3 原理图设计指南》明确该脚是「RTC 及部分数字电源」，工作范围 3.0–3.6 V，**不能单独供电、所有电源必须全部上电**；`VDD_SPI` 由它经片内约 14 Ω 供电，进而给 3.3 V Flash / PSRAM 供电。官方 Schematic Checklist 的电源域表还显示 `CHIP_PU`、GPIO0~14、GPIO19/20（USB D+/D−）、`XTAL_32K_P/N` 全都挂在 `VDD3P3_RTC` 域——它没电就等于复位采样、USB PHY、Flash 全部失效，所以芯片完全不启动、USB 不枚举、BOOT+RESET 也无效。这一条解释了此前所有现象。
- 用户拆掉 GPIO15/16 上的 32.768 kHz 晶振并把 `VDD3P3_RTC` 补到 3.3 V 后，`/dev/cu.usbmodem21101` 立刻枚举为 `Espressif / USB JTAG_serial debug unit`（VID 0x303A），`arduino-cli board list` 识别为 ESP32 Family Device。**真正必要的是补电源；晶振可装可不装**，详见 `../硬件接线表.md` 第 9 节。
- ✅ `esptool flash-id` 实测：`ESP32-S3 (QFN56) rev v0.2`、8MB 内嵌 PSRAM (AP_3v3)、40 MHz 主晶振、MAC `dc:b4:d9:37:10:20`、**Flash 实测 16MB**；`flash 0x0` 已有 bootloader 头（非空片）。注意当前固件仍按 8MB 分区编译，能正常跑，只是没用满后 8MB，后续可换 16MB 分区重编译。
- ✅ `sh flash.sh /dev/cu.usbmodem21101 --full` 成功：`Wrote 8388608 bytes (673644 compressed) at 0x00000000 in 24.9 seconds`、`Hash of data verified`。
- ✅ 真实串口 115200 开机日志（复位后抓取，234 字节，全部外设初始化成功）：
  ```
  [RGB] WS2812B×2 ok  GPIO10
  [SPK] MAX98357 I2S ok  BCLK=8 LRCK=9 DIN=7  44100Hz/16bit
  [MIC] ICS-43434 I2S ok  SCK=5 WS=4 SD=6  16000Hz/32bit
  [AP] 热点 XiaoMiClock 已开  http://192.168.4.1
  [BOOT] XiaoMiClock v4.0  WiFi=(未配置)
  ```
- ✅ **16:42 curl HTTP 实测通过，验收链闭合**。设备已从 AP 配网切到家里 WiFi `Home`，STA IP `192.168.8.129`（ARP 表 MAC `dc:b4:d9:37:10:20` 与 esptool 读出的一致）。配网成功后开放热点会自动关闭，所以 `192.168.4.1` 不再可用；**正式网页端入口是 `http://192.168.8.129/`**。
  ```
  GET /              → 200  <title>小米闹钟</title>  正式设置页（非配网页）
  GET /api/status    → 200  {"t":"16:42","d":"2026-11-15 · WiFi 已连接 · 亮度 4"}
  GET /api/mic       → 200  {"ok":true,"rms":0,"peak":0,"rate":16000}
  GET /api/alarms    → 200  [{"h":7,"m":30,"d":127,"e":0}]
  GET /netstatus     → 200  {"st":2,"ip":"192.168.8.129","ssid":"Home","msg":""}
  ```
  `/api/status` 返回当前时间说明 NTP 校时已生效；麦克风 `rms/peak=0` 是安静环境下的正常值（接口 `ok:true` 表示 I2S 读通）。`/audiotest?t=alarm`、`/save` 等写/发声类接口本轮未触发，避免突然响铃或改动设置。
- 找设备 IP 的经验：端口扫描可能因设备正在重启而漏掉，用 `arp -a | grep dc:b4:d9` 按 esptool 读到的 MAC 定位最可靠。

### 16:50 麦克风「测试没作用」→ 定位到硬件 SD 线无信号

- 现象：`GET /api/mic` 连续 8 次都是 `{"ok":true,"rms":0,"peak":0,"rate":16000}`，网页 toast 显示「峰值 0 / RMS 0」。`ok:true` 说明 `mic.readBytes()` 读满 512 字节、I2S RX 通道正常；网页 `toast()` / `micTest()` 也都在，按钮没坏。
- 为区分「SD 线全 0」与「固件 `buf[i] >> 8` 抹掉低位数据」，新增 `MicDiag`（`nz` = 128 个 32bit 样本中非零个数、`raw` = 前 8 个原始样本 hex），`/api/mic` 增加 `nz`/`raw` 字段。诊断版编译通过（1,004,672 B）、`verify_contract.py` 通过，产物 `build/XiaoMiClock_v4.0_esp32s3_micdiag_app.bin`（SHA-256 `4b0b8428…`）；**正式产物的 app / merged 两个 bin 未改动**。
- 烧 app-only @ `0x10000`（保留 NVS，WiFi 与设置不丢），`Hash of data verified`，2 秒后 WiFi 自动重连、IP 仍是 `192.168.8.129`。实测 5 次全部 `nz:0`、`raw:"00000000,..."`
  → **GPIO6 上采到的就是精确的 0，排除固件与 I2S 配置，判定 SD 线无信号**。
- 根因方向（ICS-43434 DS-000069 v1.2）：VDD（pin5，1.65–3.63 V）未供 / LR（pin2）悬空 / SCK·WS 未到麦克风 / SD 到 GPIO6 虚焊 / 6 脚 LGA 焊反 180°。详见 `../硬件接线表.md` 第 4 节与故障表。
- 17:00 用户重新加焊后麦克风恢复正常，`nz`/`raw` 作为有价值的自检能力**保留进正式固件**（`/api/mic` 向后兼容，网页忽略多余字段）。工程状态已收敛，详见下一段。

### 17:00 重新加焊 → 麦克风恢复，源码 / 产物 / 设备三者一致

- 加焊后实测：`/api/mic` 连续 10 次 `nz=64`、`rms` 4349~9278；再统计 15 次，RMS 1441~5820、峰值 5076~13770，`raw` 呈 `fffb7600,00000000,00018400,00000000` 交替。128 个 32bit 样本里正好 64 个非零 = **单麦克风固定在一个声道**（另一声道 SD 三态为 0），符合 datasheet；高 24 位有效、低 8 位为 0，也**证实固件 `>>8` 处理是正确的**。
- **根因确认：麦克风虚焊（SD 线无信号）**，不是固件、不是 I2S 配置、不是 `>>8`。
- `sh build.sh` 重新生成正式产物（源码已含 `nz`/`raw`）：app `c17f167c…`（1,004,672 B）、merged `d072e488…`（8 MB），`verify_contract.py` 通过。
- 刷正式 app @ `0x10000`（保留 NVS），`Hash of data verified`，2 秒重连；最终实测 `/api/mic` 三次 rms 5672 / 4603 / 2434、`nz=64`，`/api/alarms`、`/netstatus` 回归正常。
- `build/XiaoMiClock_v4.0_esp32s3_micdiag_app.bin` 是 16:50 的历史诊断产物（SHA `4b0b8428…`），仅编译路径不同、功能相同；**正式固件以 `XiaoMiClock_v4.0_esp32s3_app.bin` 为准**。

---

## 9. v4.1「唤醒体验」第 1 步（2026-11-15 17:46 完成）

> 这一节是**编译 → 烧录 → 真实串口日志 → curl HTTP 实测**全链闭合后的记录，不是设计稿。
> 烧的是 app-only @ `0x10000`，保留 NVS（WiFi `Home` 与全部设置不丢）。

### 9.1 范围与一个反直觉结论

**第 1 步不需要先切 16MB 分区。** 当前 `default_8MB.csv` 已有 `app0/app1` 各 3.3MB 和 1.5MB `spiffs`，v4.0 固件才约 1MB。切 16MB 要自定义 partition CSV + 改 `build.flash_size`（`XIAO_ESP32S3` 菜单里没有 16MB 选项），跟铃声播放毫无关系。**直接用现成的 spiffs 分区**就能放 2~3 段短铃声，把分区改造留到真需要长音频/语音模型时。

内置 WAV **不是**在电脑上生成再单独烧一个 spiffs 镜像，而是**首次开机由固件合成写入 SPIFFS**（`fsInit()` → `writeBuiltinRing()`）。理由：升级固件仍然只需烧一个 app @ `0x10000`，不会漏烧/错烧分区，也避免引入 mkspiffs 工具链。文件已存在就跳过。

### 9.2 铃声库与音频引擎

| 索引 | 名称 | 音源 |
|---|---|---|
| 0~3 | 上行三音 / 广播报时 / 电子急促 / 木琴 | 原方波旋律，**索引不变**，老 NVS 升级上来声音不变 |
| 4 | 晨光（柔和琶音） | `/rings/sunrise.wav` 内置合成 |
| 5 | 雨声 | `/rings/rain.wav` 内置合成 |
| 6 | 海浪 | `/rings/ocean.wav` 内置合成 |
| 7 | 自定义铃声 | `/rings/custom.wav` 网页上传，文件不存在时不出现在下拉里 |

内置 3 段统一 **22050Hz / 16bit / 单声道 / 6 秒 / 264,644 字节**（44 头 + 264,600 数据），带 50ms 首尾淡入淡出以便无缝循环。文件缺失或格式不支持时 `playTone()` 自动退回方波 0 号，保证闹钟不会「到点静音」。

`i2sFeedTask()` 仍是同一个 core 0 的 DMA 供给任务，现在有两套音源，通过 `audReq` 请求队列切换（**File/SPIFFS 操作只在音频任务里发生**，HTTP handler 只置标志，用 `portMUX` 临界区保证不会看到「新标志 + 旧路径」）：

- `AUD_TONE`：v4.0 的方波，逐样本 6ms 幅度渐变，`audioTask()` 在 loop 里推进音符。
- `AUD_WAV`：按块读 SPIFFS（1KB 缓冲）→ 线性插值重采样到 44100Hz → 音量淡变 → 写 I2S。支持单/双声道（双声道取平均）、8k~48kHz、按 RIFF 块遍历解析（能兼容带 LIST/fact 的非 44 字节头）、循环播放到文件尾自动 seek 回 data 块。

`/api/audio` 的 `mode`：`2` = WAV 流式播放（`pos` 是已送出的输出帧数）；`0` = 空闲或方波（此时看 `melody` 字段，`true` 表示方波旋律在跑）。

### 9.3 日出唤醒 / 助眠

- **日出**：只对「启用且今天在重复星期里」的闹钟生效，取最近一个；提前 `cfg.sunriseMin` 分钟开始，按 `progress²` 从暗红(12,0,0) → 橙(190,70,8) → 暖白(255,158,71) 渐亮。需要 `cfg.rgbOn=1`；响铃时让位给红闪。跨零点按 `d<0 → +1440` 处理。
- **助眠**：循环播放雨声/海浪（或「仅灯效」静音）+ 8 秒周期呼吸渐暗，整体亮度随剩余时间继续下压；到点自动停。闹钟到点、主按键短按、网页「停止助眠」都能结束。整点报时用 `audioActive()` 判断，不会打断助眠或试听。

### 9.4 产物（源码 / 产物 / 设备三者一致）

```
build/XiaoMiClock_v4.1_esp32s3_app.bin       1,060,368 B
  SHA-256 574ea42db4a81cdba3b65623184f205c4e57f4b3f9e654e10507206f8430407b
build/XiaoMiClock_v4.1_esp32s3_merged.bin    8,388,608 B
  SHA-256 5e7ce0a3380c44c7d08dcbe783b5cc57c54a1a711ddd6f820652e39f20d4dcb7
```

`build.sh` / `flash.sh` / `wait_flash.sh` 的文件名都升到 `v4.1`。编译 `Sketch uses 1060213 bytes (31%)`、`--warnings all` 源码无告警；`verify_contract.py` 三项通过（占位符 24/24、`/save` 参数 19/19、引脚 10 宏）；`esptool image-info` = ESP32-S3 / 8MB / 80m / DIO；`merged@0x10000 == app`。

### 9.5 实测记录

**串口（首次开机，含自动格式化与合成）**
```
[RGB] WS2812B×2 ok  GPIO10
[SPK] MAX98357 I2S ok  BCLK=8 LRCK=9 DIN=7  44100Hz/16bit
[MIC] ICS-43434 I2S ok  SCK=5 WS=4 SD=6  16000Hz/32bit
E (266) SPIFFS: mount failed, -10025          ← begin(true) 自动格式化的正常提示
[FS] 合成 /rings/sunrise.wav ok  264644 字节  9323 ms
[FS] 合成 /rings/rain.wav    ok  264644 字节  2639 ms
[FS] 合成 /rings/ocean.wav   ok  264644 字节  2666 ms
[FS] SPIFFS 801192/1438481 字节已用
[BOOT] XiaoMiClock v4.1  WiFi=Home
[NET] 已连接 Home  IP 192.168.8.129
[NTP] 校时成功 2026-11-15 17:41:55
```
二次开机跳过合成，2 秒内联网，同样打出 `[FS] SPIFFS 801192/…`、`[BOOT] v4.1`。

**WAV 头读回**（`/api/wavprobe?i=4/5/6`）：全部 `riff:"RIFF" wave:"WAVE" ch:1 rate:22050 bits:16 dataBytes:264600`，前 8 个样本 `0,0,0,0,0,0,0,1`（晨光）/ `0,2,-3,-3,9,-14,1,-14`（雨声）/ `0,0,0,0,0,-1,-1,-3`（海浪），首尾归零生效。

**流式播放速率**（试听晨光，每 1.5 秒采样 `pos`）：
```
67328 帧 / 1.521s = 44276 帧/秒
67072 帧 / 1.531s = 43803 帧/秒
67328 帧 / 1.519s = 44326 帧/秒   ← ≈44100，重采样与 DMA 供给都正常
```
**助眠循环**：`pos` 111104 → 222208 → 334336，`loop:true`，超过单遍 264600 帧说明已跨文件尾 seek 成功。

**停止 / 自然结束**：`/audiostop`、`/api/sleep?on=0` 后 0.4 秒 `mode` 回到 0；非循环试听 6 秒后自动 `mode:0`、`pos=265728`（≈6×44100）。

**日出**：把 `srm=30`、闹钟临时设为「现在 +10 分钟」并启用，`/api/sunrise` 返回 `progress:0.674`，与公式 `1-10/30` 的理论值 **偏差 0.000**；`/save?sron=0` 后返回 `progress:null`。测完已恢复闹钟 7:30 / `e:0`、`srm=15`、`sron=1`。

**自定义上传**：上传 44100Hz/16bit/mono/3s WAV（264,644 B）→ `{"ok":true,"size":264644}`，`/api/rings` 出现 `custom.wav`，首页两个下拉都多出 `[WAV] 自定义铃声`，选它试听得到 `mode:2 path:/rings/custom.wav rate:44100`（44100Hz 直通路径正常）。再传一个纯文本文件 → `{"ok":false,"size":23}`，**原 custom.wav 仍在、大小不变**。该文件是实测留下的示例铃声，可在网页随时覆盖。

**原功能回归**：`/api/mic` → `nz:64, rms:9765`（麦克风仍正常）；`/api/status`、`/api/alarms`、`/netstatus`、`/sync` 全部正常；方波铃声 `melody:true` → 停止后 `false`；首页 0 个残留 `{{占位符}}`。

**设置已恢复实测前基线**：`atone=0`（上行三音）、`vol=6`、闹钟 `[{"h":7,"m":30,"d":127,"e":0}]`、`srm=15 / sron=1 / sls=0 / slmin=30`、助眠关闭。测试期间检测到音量一度变成 10，已用受控实验确认「单参数 `/save` 和 multipart 上传都不会串改 volume」（属于外部改动），并恢复为 6。

### 9.6 实测暴露并修掉的问题

1. **STOP 只淡出音量、没结束播放**（实测发现）：`/audiostop` / 停止助眠后 `mode` 仍卡在 2、`pos` 继续涨，只是听不见。修复：`AUDREQ_STOP` 同时置 `wav.running=false`，让 `wavFill()` 淡出完成后关文件、回 `AUD_IDLE`。
2. **上传失败会删掉原有自定义铃声**（设计缺陷）：改为先写 `/rings/upload.tmp`，`wavCheck()` 通过后才 `remove(dst)` + `rename`，rename 不可用时逐块复制兜底；失败只删临时文件。
3. **`/api/sleep` 的 `min` 返回配置值而不是本次时长**：加 `sleepActualMin`，返回 `min`（本次实际）+ `cfgMin`（配置默认）。
4. 预防性修复（编译/推演阶段）：`struct WavState` 必须放在第一个函数之前，否则 Arduino 自动插入的函数原型引用未声明类型而编译失败；`wavNext()` 循环播放卡在数据块末尾时两轮重试；`playWav()` 切换前清 `toneAmp`，避免 WAV 结束后冒出残留方波；海浪包络用 `s*s` 代替 `powf(s,2)` 避免负数幂 NaN。

### 9.7 尚未验证 / 留给下一步

- **声音的主观听感与灯效颜色**没人在本机听过/看过，实测只证明了数据链路正确（帧速率、文件头、循环、状态机）。装机后按实际响度调 `I2S_AMP_MAX`，按观感调日出配色。
- 麦克风自适应音量、拍手停铃、睡眠记录、网页 OTA 属于第 2/3 步，本轮未做。
- 恢复出厂（`wipeAllAndReboot`）只清 NVS，**不删 SPIFFS 里的 WAV**；如需清空铃声要另加接口或全片擦除。
