# Mi Alarm Clock TM1652（`mi-alarm-tm1652`）

把 DIY「小米闹钟」自画裸片板接到 [xiaozhi-esp32](../../../README.md) 的适配板。
**不动原厂任何板型配置**，独立 `type`/`name`，避免 OTA 把自定义板刷回原厂固件。

## 硬件

| 项目 | 值 |
| --- | --- |
| 主控 | ESP32-S3 QFN56 rev0.2 |
| Flash | 16 MB（实测） |
| PSRAM | 8 MB OPI (AP_3v3) |
| 主晶振 | 40 MHz |
| 显示 | TM1652 单线 SDA，4 位数码管 + 冒号 + 图标 |
| 功放 | MAX98357A，I2S 直连（无 MCLK） |
| 麦克风 | ICS-43434，I2S 直连 |
| 指示灯 | 2× WS2812B 串联 |
| 按键 | GPIO0（BOOT，低有效）、GPIO2（保留） |

### 引脚

| GPIO | 用途 | 备注 |
| --- | --- | --- |
| 0 | BOOT / 按键 1 | strapping，上电时**不能按住** |
| 1 | TM1652 SDA | 接线表写作 `GPIO1-TM1652EN`，用户确认是数据线不是电源使能 |
| 2 | 按键 2 | 本版未接入 Button 对象 |
| 4 | MIC_WS | ICS-43434 |
| 5 | MIC_SCK | ICS-43434 |
| 6 | MIC_DIN | ICS-43434 |
| 7 | SPK_DOUT | MAX98357 |
| 8 | SPK_BCLK | MAX98357 |
| 9 | SPK_LRCK | MAX98357 |
| 10 | WS2812B_DIN | 2 颗串联 |
| 19 / 20 | USB D− / D+ | 原生 USB，别被其他功能占用 |

与官方 `bread-compact-wifi` 相比，**麦克风 GPIO4/5/6 和功放 DIN GPIO7 完全一致**，
只需把功放 BCLK/LRCK 从官方的 GPIO15/16 改成 GPIO8/9。
本板 GPIO15/16 属于 32.768 kHz 晶振 / RTC 电源域，不能挪用。

### 硬件红线

`VDD3P3_RTC` 必须接 3.3 V（3.0–3.6 V）。悬空会导致芯片完全不启动、
USB 不枚举、BOOT 键无效。32.768 kHz 晶振 X2 建议不装，本固件不使用外部 RTC 时钟。

## Display：数码管当屏幕（平时时钟，唤醒才表情）

小智的 `Display` 基类只有 `Lock()` / `Unlock()` 两个纯虚函数，
其余 `SetStatus` / `SetEmotion` / `ShowNotification` / `SetChatMessage` 都有默认实现，
因此不需要 LVGL 或像素屏。本板实现 `Tm1652Display`：

- 100 ms 刷新任务 + 段码差量发送（TM1652 是锁存型芯片，画面不变就不占总线）
- 单线时序与小米闹钟 Arduino v4.1 固件完全一致：49 µs 位宽、低位在先、偶校验、
  命令间隔 ≥3 ms；每字节关中断约 0.55 ms，一帧 7 字节约 3.8 ms
- 上电第一帧是配网动画，之后由**设备状态**决定显示什么

### 场景由设备状态决定（用户约定）

用户要求：**平时（小智没被唤醒）显示时钟，只在唤醒/交互时切换为表情**。
关键是**不能只用 emotion 字符串判断**：小智在 `Idle` / `Connecting` / `Listening`
下发的都是同一个 `"neutral"`（`application.cc` 里三处），所以 `EffectiveScene()`
以 `Application::GetInstance().GetDeviceState()` 为主判据，emotion 只决定
「交互期间显示哪一种表情」。

| DeviceState | 场景 | 数码管 |
| --- | --- | --- |
| `Idle` / `Starting` / `Connecting` / `Unknown` | Clock | **HH:MM**，冒号 1Hz 闪，WiFi 图标常亮 |
| `WifiConfiguring` / `Activating` | WifiConfig | **原机开机配网动画** |
| `Listening` | Listening | 上半框动画，节奏快一倍 |
| `Speaking` | Speaking | 四位单段扫掠流光 |
| `FatalError` | Error | `Err` + 铃铛图标闪 |
| `Notifying` / `Upgrading` / `AudioTesting` | Listening | 动态表情 |

`gear`（配网）与 `link`（激活）在 emotion 层也都映射到 `WifiConfig`；调用链
`WifiBoard::StartWifiConfigMode() → Alert(...,"gear",...)`、
`Application::ShowActivationCode() → Alert(...,"link",...)` 已核对。
WifiConfig 复刻原机 `drawWifiWaitFrame()`：g 段中横常亮、f/a/b 三段 500 ms 闪、
中间下方小方点常亮、右下 WiFi 图标同频闪。

emotion 补充映射（仅在交互状态生效）：`warning`/`cancel`/`sad`/`angry` → Error，
`thinking`/`loading`/`sleepy` → Listening，`happy`/`laughing` → Speaking，其余 → 状态默认。

4 位数码管显示不了文字，因此 `SetStatus` / `ShowNotification` / `SetChatMessage`
只写日志、不切换场景——否则 `Alert()` 里紧跟的表情会被状态覆盖。
`SetEmotion()` 恒返回 `true`，避免 `application.cc` 在 `robot_2` 失败时回退到 `neutral`。

### 时钟与校时

小智官方固件**不含任何 NTP 校时**，所以本板自己起 SNTP：

- `NtpTask` 启动 3 s 后 `esp_netif_sntp_init()`（`ntp.aliyun.com`），未同步时
  每 15 s 重发一次请求；同步后停止重试
- 用 `sync_cb` 置「已同步」标志，**只有 SNTP 真正同步过才显示数字**。不能用
  `time(nullptr) > 某年` 判断：实测 RTC 残留了一个看似有效但错误的时间（曾显示
  `03:50`），会让 SNTP 被整个跳过
- 不用 `TZ` 环境变量（POSIX TZ 字符串容易解析反），直接
  `gmtime_r(time(nullptr) + 8*3600)` 得到北京时间
- 未校时显示 `----` + 冒号闪，明确是「时钟模式但没有时间」
- 构造时做一次 `12:34` 段码自检，串口留下 `Clock selftest 12:34 -> 06 DB 4F E6 10 00`

## 构建

项目默认就是 16 MB Flash + `partitions/v2/16m.csv` + OPI PSRAM，与本板一致，
所以 `config.json` 不需要任何 `sdkconfig_append` 覆盖。

```sh
source $HOME/esp/esp-idf/export.sh       # 需要 IDF v6.0.1+，本机是 release/v6.1
cd /Volumes/macmini/DIY/xiaozhi-esp32
python3 scripts/build.py mi-alarm-tm1652 --language zh-CN
# 或者直接用固化脚本（推荐，避免漏参数）：
# /Volumes/macmini/DIY/小米闹钟/MI-ALARM-ESP32S3/xiaozhi_port/build_zh.sh
```

> **必须带 `--language zh-CN`**：`scripts/build.py` 默认 `en-US`，漏掉参数会编出
> 英文提示音、英文配网文案和 `Hi ESP` 唤醒词。中文版会换成 `locales/zh-CN/*.ogg`
> 语音包和「你好小智」(`wn9_nihaoxiaozhi_tts`)。按 `docs/custom-board.md` 约定，
> 语言属于用户构建选项，**不能**写进板级 `config.json`。

## 回滚到小米闹钟 v4.1

小智固件的分区表与 Arduino 固件**完全不同**，烧录会清空原 NVS/铃声库。
完整备份在：

```
/Volumes/macmini/DIY/小米闹钟/MI-ALARM-ESP32S3/firmware_backup/v4.1_20261115_1834/
```

```sh
# 只恢复 app（会保留 v4.1 的 NVS 设置，但两版 NVS 键不通用，建议用 merged 全片）
python3 -m esptool --chip esp32s3 -p /dev/cu.usbmodem21101 -b 921600 \
  write-flash 0x0 ".../XiaoMiClock_v4.1_esp32s3_merged.bin"
```

## 实机验证（2026-11-15，编译 → 烧录 → 串口全链）

**构建**：ESP-IDF v6.1（release/v6.1-dirty），2244 个目标，0 error / 0 warning。
`build/xiaozhi.bin` 2,077,072 B（0x1fb100），app 分区 0x3f0000，剩余 50%；
`build/merged-binary.bin` 8,949,712 B（0x888fcd），`generated_assets.bin` 548 KB。
两个板级源文件 `tm1652_display.cc` / `mi_alarm_tm1652_board.cc` 均进入 `main` 组件。

**烧录**：`esptool --chip esp32s3 write-flash 0x0 merged-binary.bin` →
`Hash of data verified`；增量版只刷 app @ `0x20000` 复测，同样校验通过。
从设备 `0x20000` 读回 2,077,072 B，与归档 app 逐字节一致
（SHA-256 `463c410e…09fddc`）。

**串口**（115200，USB CDC `/dev/cu.usbmodem21101`）：

```text
I (207)  Board: UUID=7a8c72d4-fdce-40e5-aa10-db0da24d8f74 SKU=mi-alarm-tm1652
I (257)  Tm1652Display: TM1652 frame #2: 00 63 63 80 10 00
I (267)  Tm1652Display: TM1652 ready on GPIO1, boot scene = wifi config animation
I (267)  MiAlarmTm1652Board: board initialized: TM1652 + MAX98357 + ICS-43434 + 2xWS2812B
I (2037) WifiConfigurationAp: Access Point started with SSID Xiaozhi-1021
W (2067) Application: Alert [gear] Wi-Fi Configuration Mode: ... http://192.168.4.1
I (2077) Tm1652Display: SetEmotion(gear) -> scene=0 [开机配网表情]
I (2167) Tm1652Display: TM1652 frame #3:  00 63 63 80 10 00
I (2667) Tm1652Display: TM1652 frame #4:  00 40 40 80 00 00
I (3167) Tm1652Display: TM1652 frame #5:  00 63 63 80 10 00
I (3667) Tm1652Display: TM1652 frame #6:  00 40 40 80 00 00
```

两帧严格 500 ms 交替，与 Arduino v4.1 的 `drawWifiWaitFrame()` 完全一致：
`frame[1]/frame[2]` 在 `0x63`（g+f/a/b）与 `0x40`（只剩 g）之间闪，
`frame[3]=0x80` 中间小方点常亮，`frame[4]` 在 `0x10`（WiFi 图标）与 `0x00` 之间闪。
全程无 panic / assert，`Assets: Skipping UI assets: LVGL display is unavailable`
属预期（数码管不需要 LVGL 资源）。

### 19:33 中文提示音修正（用户反馈「听到的是英文提示音」）

原因：首版构建用了 `scripts/build.py` 默认 `--language en-US`，
`assets` 分区打包的是 `locales/en-US/*.ogg`，配网播的是英文语音。
修复：`python3 scripts/build.py mi-alarm-tm1652 --language zh-CN` 全量重编，
唤醒词自动从 `wn9_hiesp` 换成 `wn9_nihaoxiaozhi_tts`。

实测：全片重烧 `Hash verified`；设备读回 app 2,071,216 B 与归档逐字节一致
（当前固件 SHA-256 `40537ecd…891a6f412a31ddb23ea09963b1439b1c`）；串口文案变成中文：

```text
W (2058) Application: Alert [gear] 配网模式: 手机连接热点 Xiaozhi-1021，浏览器访问 http://192.168.4.1
I (2068) Tm1652Display: SetStatus: 配网模式
I (2078) Tm1652Display: SetEmotion(gear) -> scene=0 [开机配网表情]
I (2168) Tm1652Display: TM1652 frame #3:  00 63 63 80 10 00
I (2668) Tm1652Display: TM1652 frame #4:  00 40 40 80 00 00
```

`assets.bin` SHA 从 `a8979a86…`（en-US）变成 `ed037155…`（zh-CN），
内含 `wn9_nihaoxiaozhi_tts` 模型，确认语音包确实换了；段码动画不受影响，无 panic。
**当前设备跑的是本节的中文版**，上一段的产物 SHA 是首版 en-US，仅作历史记录。

### 19:59 平时时钟 / 唤醒才表情（用户澄清需求）

用户澄清「没有唤醒小智的时候还是显示时钟，只在唤醒时切换为表情」。
改 `Tm1652Display`：新增 `Scene::Clock`、`EffectiveScene()` 按设备状态选场景、
`DrawClock()`、`NtpTask` SNTP 校时。

实测（设备已连 `Home`，IP `192.168.8.129`，已激活，中文唤醒词「你好小智」）：

```text
I (267)  Tm1652Display: Clock selftest 12:34 -> 06 DB 4F E6 10 00
I (767)  Tm1652Display: scene -> clock (state=1, time_synced=0)
I (767)  Tm1652Display: TM1652 frame #3:  40 40 40 40 00 00     ← 未校时 ----
I (1907) WifiBoard: Connected to WiFi: Home
I (1967) Tm1652Display: scene -> wifi_config (state=9, time_synced=0)  ← 激活 → 配网表情
I (3267) Tm1652Display: SNTP init(ntp.aliyun.com): ESP_OK
I (3667) Tm1652Display: scene -> clock (state=3, time_synced=0)  ← Idle → 回时钟
I (7087) Tm1652Display: SNTP synced, epoch=1794743958
I (7167) Tm1652Display: TM1652 frame #17: 06 EF 6D EF 10 00     ← 19:59 冒号亮
I (7667) Tm1652Display: TM1652 frame #18: 06 6F 6D 6F 10 00     ← 19:59 冒号灭
```

`epoch=1794743958` = UTC 11:59:18 / 北京 19:59:18；`06 EF 6D EF` 拆段码为
`1 9 5 9` + 冒号亮，`06 6F 6D 6F` 为冒号灭，冒号 500 ms 闪，与当前时间完全一致。
`state=9 (Activating)` 触发 `wifi_config` 场景，**「激活显示开机配网表情」也在真机实测到**。
原配网动画 `00 63 63 80 10 00` / `00 40 40 80 00 00` 未受影响，无 panic。

当前固件 app SHA-256 `4534fad5…e7672c7`，设备读回逐字节一致。

### 尚未实测

- 真正被唤醒词唤醒后的 `Listening` / `Speaking` 表情切换：代码已实现，
  需对着设备喊「你好小智」，串口应出现 `scene -> listening`；本机无法发声触发。
- `SetEmotion("link")` 单独路径：设备已激活过，本次走的是 `Activating` 状态触发
  配网表情（效果一致），首次激活码弹窗路径仍需新设备复现。
  代码上 `link` 与 `gear` 走 `MapEmotion()` 的同一分支，官方调用链
  `Application::ShowActivationCode() → Alert(..., "link", ...)` 已核对。
  串口出现 `SetEmotion(link) -> scene=0 [开机配网表情]` 即验证通过。
- 麦克风录音、功放放音、WS2812B 灯效、唤醒词、服务端对话、OTA 的实机表现。

## 已知限制（首版移植验证用）

- `assets` 分区已生成并烧录（548 KB），但 UI 表情资源因无 LVGL 显示被跳过；
  数码管只用段码，不加载图片字体
- 第二按键 GPIO2 未接 `Button`，后续可做「按下说话」
- TM1652 与 WS2812B 是 5 V 器件，3.3 V 数据电平处于 VIH 边缘；原 Arduino 固件实测可点亮
- 唤醒词 / 服务端对话 / OTA 尚未在本板做完整实机验证

## WiFi 射频优化与功率分档实测（用户要求「看看是不是限制了功率，修复并测最适合的功率」）

### 排查结论：没有把功率压低的常量，但有两条限制通道 + 一个真凶

1. **Arduino v4.1** `CFG_TX_DBM 20` → `esp_wifi_set_max_tx_power(80)` = 20 dBm，
   这是 ESP32-S3 的 PHY 上限，**不是限功率**。注释里「限功率」「XIAO 模组」「20dBm 会失联」
   是旧 C3 6 dBm 版本遗留，且与本板「自画裸片 + 自制天线匹配」的事实不符。
   闹钟线已暂停，本轮**未改 v4.1 源码**，恢复该线时顺手修注释即可（数值 20 是对的）。
2. **小智编译层已满功率**：`CONFIG_ESP_PHY_MAX_WIFI_TX_POWER=20`、
   `CONFIG_ESP_PHY_MAX_TX_POWER=20`，`CONFIG_ESP_PHY_REDUCE_TX_POWER` 未开启。
3. **运行期限制通道**：配网页「高级设置」可写 NVS `wifi/max_tx_power`（单位 0.25 dBm），
   `WifiStation::Start()` 读到非 0 就会覆盖 PHY 上限；`WifiConfigurationAp::Start()`
   也会在配网模式应用它。本设备实测 `legacy=0`（没被写过），但机制保留 → 已加开机自检/自动清除。
4. **真正的「信号差」元凶是省电模式**：`Application` 在 Idle 调
   `board.SetPowerSaveLevel(LOW_POWER)` → `WIFI_PS_MAX_MODEM`，射频按 DTIM 周期睡眠。
   修复前 ping **420–493 ms 且逐包递增**，看起来就像信号极差。

### 修复（只改本板目录，不动官方源码和其它板）

- `config.h`：新增 `RF_DEFAULT_TX_QUARTER_DBM 80`（= 20 dBm）。
- `rf_tools.h/.cc`：开机等待 WiFi start → 读取并清除历史 NVS 限制 → 显式设满功率 →
  `esp_wifi_set_ps(WIFI_PS_NONE)`；UDP 3333 测试服务
  `STAT / TXP <q> / PS <0|1|2> / BURST <host> <port> <n> <ms> / SAVE <q> / CLR`。
  测试结论写 NVS `mi_alarm/rf_best`，与官方 `wifi/max_tx_power` 隔离。
- `mi_alarm_tm1652_board.cc`：覆写 `SetPowerSaveLevel()` 一律按 PERFORMANCE 处理
  （插电闹钟不需要 modem sleep），`StartNetwork()` 后启动 rf_tools。
- `xiaozhi_port/flash_app.sh`：app-only 烧录 @0x20000，保留 NVS / Wi-Fi 配网 / 中文 assets。
- `xiaozhi_port/rf_sweep.py`：主机分档实测脚本，结果存 `rf_sweep_result.json`。

### 实测数据（设备 192.168.8.129，AP `Home` ch11，RSSI -63 ~ -79 dBm）

省电模式对比（两轮）：

| 模式 | 第 1 轮 ping avg | 第 2 轮 ping avg | 丢包 |
| --- | --- | --- | --- |
| `MAX_MODEM`（旧默认 LOW_POWER） | 331.05 ms | 389.37 ms | 0% |
| `NONE`（修复后） | 6.04 ms | 5.43 ms | 0% |

功率分档（每档 150 个上行 UDP 包 + 12 个 ping）：

| dBm | q | 实际上限 | 上行收包 | ping 丢包 | ping 均值 |
| --- | --- | --- | --- | --- | --- |
| 8 | 32 | 8.00 | 100% | 0% | 9.52 ms |
| 11 | 44 | 11.00 | 100% | 0% | 7.75 ms |
| 14 | 56 | 14.00 | 100% | 0% | 4.76 ms |
| 17 | 68 | 17.00 | 100% | 0% | 3.78 ms |
| 20 | 80 | 20.00 | 100% | 0% | 4.08 ms |

RTT 3–10 ms 的起伏是采样噪声，与功率档无相关性。**所有档位都零丢包、上行 100%**，
说明当前距离下发射功率不是瓶颈；最高档也没有出现旧 C3 那种「10 dBm 以上波形压塌」的劣化，
证明本板供电/去耦能承受满功率。但 RSSI 会瞬时跌到 -79 dBm，插电设备需要链路余量，
因此**推荐 20 dBm（q=80）**，已 `SAVE` 到 NVS `mi_alarm/rf_best`。
若以后要省电或降低干扰，`powersave` 策略下 8 dBm 也够用（脚本 `--policy powersave`）。

### 验收（编译 → 烧录 → 串口/网络实测全链）

- `build_zh.sh` 编译 0 error，app `2,079,872 B`，SHA-256 `2f245f73…d8`；
  app 分区剩余 50%。
- `flash_app.sh` 烧录 `Hash verified`；从 `0x20000` 读回 2,079,872 B 与本地产物逐字节一致。
- 串口实测：
  `I (547) RfTools: [RF] 默认功率 legacy=0 best=80 target=80 (20.00 dBm) actual=80 err=0 ps MIN_MODEM->NONE`
  / `[RF] UDP 测试服务已启动: 0.0.0.0:3333` / `Connected to WiFi: Home` / `SNTP synced`。
  命令日志：`[RF] TXP -> 32 (8.00 dBm) actual=32`、`[RF] TXP -> 80 (20.00 dBm) actual=80`、
  `[RF] 已持久化最适功率 80`。
- 真实复位后 STAT：`tx=80 dbm=20.00 ps=0 psname=NONE ssid=Home ip=192.168.8.129 best=80`，
  ping `2.103/8.693/19.955 ms`、0 丢包。
- 归档：`xiaozhi_port/firmware/*_rf_*.bin` + `SHA256SUMS`（app `2f245f73…` /
  assets `ed037155…` / merged `ef55da23…`）。

### 尚未验证

- 远距离 / 隔墙弱信号场景下的档位差异（当前 RSSI 太好，各档都满收包）；
  若要把设备搬到别处，重跑 `rf_sweep.py --policy coverage` 即可。
- 20 dBm 连续大流量下的温升与电源纹波（本轮 UDP 负载仅 ~20 KB/s）。

## 21:17 去掉开机 `----` 占位（用户：空窗期继续显示开机表情）

用户指出开机 `----` + 冒号闪「不需要，导致开机显示混乱」，「既然都连好 Wi-Fi 了，肯定能拿到 NTP 时间」；随后补充「空窗期可以继续显示开机表情的啊」。

### 改动（只动 `tm1652_display.cc/.h`）

- `DrawClock()` 未校时分支不再画四条中横，直接 `return`；空窗画面统一交给 `RenderFrame()`。
- `RenderFrame()` 里 `Clock` 的未校时分支、`Idle`/default 兜底都改成
  `DrawWifiWaitFrame(blink)` + WiFi 图标闪，与上电第一帧、配网、激活**同一套动画**；
  构造函数第一帧配网表情保留。开机从头到尾一个画面，不跳变、不熄灭、`----` 彻底消失。
- `NtpTask()` 不再固定等 3s：轮询 STA IP（200ms 一次、最多 20s），一拿到 IP 就
  `esp_netif_sntp_init()`；兜底重试定 8s。试过 3s——`esp_netif_sntp_start()` 会
  stop+init 打断正在进行的请求，实测同步反而拖到 8.6s；改 8s 后同步 2.5~5.2s。
- **顺带加固时钟**：`OnSntpSync(tv)` 记录回调给出的 UTC 秒 + `esp_timer_get_time()`，
  显示用 `base + Δt + 8h` 再 `gmtime_r`，**不再读 `time(nullptr)`**。
  原因：热复位实测偶发一次显示 `05:02`（真实 21:02，差 16h）——系统时间被 adjtime
  平滑校正/其它模块改动时，再手工 +8h 会叠加成 +16h；之前踩过的「RTC 残留 03:50」
  也一并被 NTP 基准彻底隔离。

### 实测（冷启动 + 热复位两轮，段码程序化解码后与 PC 时间比对）

| 场景 | PC 时刻 | 数码管帧 | `----` 帧 | 空窗配网表情 | SNTP |
| --- | --- | --- | --- | --- | --- |
| 冷启动 | 21:16 | `5B 06 06 7D 10 00` = **21:16** | 0 | 14 帧 | epoch=1794748616 |
| 热复位 | 21:17 | `5B 06 06 07 10 00` = **21:17** | 0 | 8 帧 | epoch=1794748624 |

冒号 1Hz 闪保留（数字帧 `5B 86 …` / `5B 06 …` 交替），无 panic。
射频回归 `tx=80 ps=NONE`，ping `1.868/4.806/12.827 ms`、0 丢包。
SNTP init 由旧版 3.3s 提前到 ~1.9s，同步由 7.1s 缩到 2.5~5.2s。

产物 app `2,080,192 B`，SHA-256 `855402f1…81`，烧录 `Hash verified`、读回逐字节一致；
归档 `xiaozhi_port/firmware/*_clock_*.bin` + `SHA256SUMS`。

## 15:5x 长按按键2恢复出厂 + 中文语音「重置成功」

需求：长按按键2（GPIO2）清除 Wi-Fi 配网和所有设置，并语音提醒「重置成功」。

### 改动清单

| 文件 | 改动 |
| --- | --- |
| `config.h` | 新增 `KEY2_FACTORY_RESET_HOLD_MS 5000`（5 秒防误触）；GPIO2 不是 strapping 脚 |
| `mi_alarm_tm1652_board.cc` | 新增 `Button key2_button_`，`OnLongPress()` → `MiAlarmFactoryReset("GPIO2 long press")`；开机打印长按时长 |
| `factory_reset.h/.cc`（新增） | 唯一恢复出厂入口，按键与 UDP 共用 |
| `rf_tools.cc` | UDP 3333 新增 `FACTORY CONFIRM`；不带 `CONFIRM` 一律拒绝，防止误发一条包就清空设备 |
| `main/assets/common/reset_success.ogg`（新增） | 中文「重置成功」，Ogg Opus / mono / 16 kHz / 1.17 s / 4,450 B |
| `main/CMakeLists.txt` | 语言头生成规则的 `DEPENDS` 补上 `${LANG_SOUNDS}` `${COMMON_SOUNDS}` |

`MiAlarmFactoryReset()` 的执行顺序（顺序很重要）：

1. `Application::Schedule()` 里调 `PlaySound(Lang::Sounds::OGG_RESET_SUCCESS)`，走官方提示音同一条播放链路；
2. 独立任务先等 1.8 s（语音 1.17 s），再轮询 `AudioService::IsPlaybackIdle()` 最多 3 s，
   **确认语音播完才动手**，否则重启会把提示音掐成半句；
3. `esp_wifi_disconnect()` + `esp_wifi_stop()`，避免驱动在擦除后把当前凭据写回 NVS；
4. `nvs_flash_deinit()` → `nvs_flash_erase()`；失败兜底用
   `esp_partition_erase_range()` 直擦 `nvs` 分区；
5. `esp_restart()`，重启后因无凭据自动进配网 AP。`std::atomic<bool>` 防重复触发。

`nvs_flash_erase()` 擦的是整个默认 NVS 分区（0x9000 / 0x4000），所以 Wi-Fi 凭据、`wifi/max_tx_power`、
`mi_alarm/rf_best`、音量/唤醒词等所有设置一起清掉；`assets` 分区与 `otadata` 不动，
重启仍从 `ota_0` 启动，中文语音包不会丢。

### 中文语音怎么来的（本机无 ffmpeg/opusenc）

macOS `say -v Tingting` → `afconvert` 重采样成 16 kHz 单声道 WAV →
Python `soundfile`(libsndfile 1.2.2) 写 Ogg Opus。macOS 自带 `afconvert` 虽列出
`Oggf` + `opus`，实测写入一律 `ExtAudioFileWrite failed ('fmt?')`，不能用。

产物放进 `main/assets/common/` 后由 CMake 的 `COMMON_SOUNDS` GLOB 自动嵌入 app，
`gen_lang.py` 自动生成 `Lang::Sounds::OGG_RESET_SUCCESS`（引用 `_binary_reset_success_ogg_start`）。
**坑**：官方 `add_custom_command()` 只依赖 `language.json`，新增 `.ogg` 不会触发
`lang_config.h` 重生成，第一次编译报 `'OGG_RESET_SUCCESS' is not a member of 'Lang::Sounds'`。
已在 DEPENDS 补上音效列表并手动跑过一次 `gen_lang.py`，以后新增语音不会再踩。

板级源码副本目录里也放了一份 `reset_success.ogg`，复现时把它复制到小智项目的
`main/assets/common/reset_success.ogg`。

### 实测（编译 → 烧录 → 真实串口 + UDP，2026-11-16）

编译 `0 error / 0 warning`，app `2,086,656 B`，SHA-256 `b9405844…a47`；
只烧 app @ `0x20000`，`Hash verified`，保留 NVS / 配网 / 中文 assets。

| 步骤 | 实测结果 |
| --- | --- |
| 防误触 | UDP `FACTORY` → `ERR 需确认: FACTORY CONFIRM` |
| 触发 | UDP `FACTORY CONFIRM` → `OK factory reset started` |
| 语音 | `PlaySound(OGG_RESET_SUCCESS) ogg_bytes=4450`；`AudioService: Resampling audio from 16000 to 24000`；推入后 `idle=0` → `drained=1`，无 `OggDemuxer` 报错 |
| 擦除 | `nvs_flash_deinit -> 0 (ESP_OK)`、`NVS erase -> 0 (ESP_OK)` |
| 重启 | `esp_restart` → `rst:0xc (RTC_SW_CPU_RST)` |
| 清 Wi-Fi | `SsidManager: NVS namespace wifi doesn't exist`；`Starting config AP`；热点 `Xiaozhi-1021` / `192.168.4.1` |
| 清全部设置 | 重启后 `RfTools: legacy=0 best=0`（连 `mi_alarm/rf_best` 也没了） |
| 显示 | 空窗/配网仍是开机配网表情两帧交替，无 `----`、无熄灭、无 panic |
| 读回 app | 从 `0x20000` 读回 `2,086,656 B` 与归档 bin `cmp` 逐字节一致 |
| 回滚验证 | 写回 `nvs_backup_before_factory_test.bin` 到 `0x9000`，1 秒内 `STAT` → `ssid=Home ip=192.168.8.129 best=80`；SNTP `time_synced=1`、Idle → `scene -> clock` |

NVS 分区读回：擦除后 `0xFF` 占 86.65%（重启后系统会重建 NVS 页头并写入新的运行期键值，
所以不是全 `0xFF`）；**功能层面的硬证据是 `wifi` namespace 不存在 + 自动进配网 AP**。
测试前备份的 NVS 已写回，设备当前恢复原状，无需重新配网。

归档 `xiaozhi_port/firmware/*_factory_*.bin` + `SHA256SUMS`。

### 尚未验证 / 需手动确认

- **GPIO2 物理长按 5 秒**：自动化实测走的是 UDP 同一函数入口；物理按键路径已编译注册
  （开机日志 `GPIO2 长按 5000 ms = 恢复出厂`，Button 构造失败会直接 abort），
  但需要人手按住按键2 满 5 秒做最终确认。
- **「重置成功」的听感**：串口能证明 Ogg 被解复用、16k→24k 重采样、解码队列排空，
  本机没有录音设备做声学回放确认。

### 安全提示

- UDP 3333 的 `FACTORY CONFIRM` 对局域网内任何主机开放；不需要远程维护时，
  删掉 `rf_tools.cc` 里该分支即可只保留按键触发。
- `xiaozhi_port/firmware/nvs_backup_before_factory_test.bin` 含本机 Wi-Fi 凭据，
  已加入 `.gitignore`，**不要上传公开仓库或外传**。
