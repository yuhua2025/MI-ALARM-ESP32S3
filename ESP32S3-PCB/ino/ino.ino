/*
 * ============================================================
 *  小米闹钟 · ESP32-S3 主控固件  v4.1
 *  ------------------------------------------------------------
 *  由 ESP32-C3 版 v3.15 移植而来，功能保持一致：
 *    时钟显示 / 12-24H / 5 组闹钟 / 整点报时 / 中文配网页 / NTP 校时
 *  新硬件差异（详见 ../硬件接线表.md 与 README.md）：
 *    - 无 DS1302、无光敏：走时完全依赖 NTP，断电后需重新联网校时
 *    - 音频：PAM8403 + LEDC 方波 → MAX98357，走 I2S（GPIO7/8/9）
 *    - 新增 ICS-43434 I2S 麦克风（GPIO4/5/6），提供 /api/mic 电平自检
 *    - 新增两颗 WS2812B（GPIO10），网页可开关、调亮度
 *    - 灯板 TM1652 单线 SDA 改到 GPIO1；主按键改到 GPIO0(BOOT)
 *  v4.1「唤醒体验」（本轮）：
 *    - SPIFFS 铃声库：首次开机自动合成 3 段内置 WAV，网页可上传自定义 WAV
 *    - i2sFeedTask 双引擎：原 4 种方波旋律 + WAV 流式播放（带重采样/淡变）
 *    - 日出唤醒：响铃前 N 分钟由 WS2812B 从暗红渐亮到暖白
 *    - 助眠模式：循环白噪音 + 呼吸渐暗灯效，到点自动停止
 *  显示驱动与位映射沿用「灯板测试/board_test v4」的实测结论。
 * ============================================================
 */

#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <time.h>
#include <math.h>
#include <SPIFFS.h>           // 分区表里已有的 1.5MB spiffs（default_8MB.csv）
#include <esp_system.h>
#include <esp_wifi.h>
#include <ESP_I2S.h>          // arduino-esp32 3.x 自带（I2SClass / driver/i2s_std.h）

// ============================================================
//  引脚定义（对 ESP32S3-PCB；接线表在 ../../硬件接线表.md）
//  改任何一个脚，固件和接线表要一起改。
// ============================================================
#define PIN_TM1652    1     // TM1652 单线 SDA（接线表的 GPIO1-TM1652EN）
#define PIN_KEY       0     // 主按键 = BOOT 键 → GND（strapping，开机别按着）
#define PIN_KEY2      2     // 副按键 → GND：长按 8 秒恢复出厂
#define PIN_MIC_WS    4     // ICS-43434 WS / LRCK
#define PIN_MIC_SCK   5     // ICS-43434 SCK / BCLK
#define PIN_MIC_DIN   6     // ICS-43434 SD → ESP32
#define PIN_SPK_DOUT  7     // MAX98357 DIN ← ESP32
#define PIN_SPK_BCLK  8     // MAX98357 BCLK
#define PIN_SPK_LRCK  9     // MAX98357 LRCK
#define PIN_WS2812    10    // 两颗 WS2812B 串联
#define WS2812_COUNT  2

// 音量：共 10 档（1~10），映射到 I2S 16bit 方波的幅度。
// 满档只占满量程约 30%，MAX98357 固定增益下够响且不削波；
// 装机后觉得太吵/太轻，只改 I2S_AMP_MAX 一个数即可。
#define VOL_LEVELS    10
#define I2S_AMP_MAX   10000
#define VOL_REF_OLD   50    // 老固件 0~255 音量配置迁移基准

// ============================================================
//  铃声库（闹钟和整点报时各自从这里挑一个）
//  索引顺序一旦定下来就不要改，NVS 里存的就是这个索引号
//  这几个宏必须在 loadCfg() 之前定义（那里要拿来做范围校验）
// ============================================================
#define TONE_UP3      0   // 上行三音：C6-E6-G6，温柔但叫得醒
#define TONE_BELL     1   // 广播报时：四声短「滴」+ 一声长音
#define TONE_RADAR    2   // 电子急促：高频短脉冲连打（iPhone 雷达的味道）
#define TONE_MARIMBA  3   // 木琴：琶音上行再回落（iPhone 木琴的味道）
// v4.1：索引 4 起是 SPIFFS 里的真实 WAV 铃声。前 4 个方波索引保持不变，
// 老 NVS 里存的 0~3 升级上来仍然是原来的声音，不会被悄悄换掉。
#define TONE_SUNRISE  4   // 晨光琶音（内置合成 WAV）
#define TONE_RAIN     5   // 雨声（内置合成 WAV）
#define TONE_OCEAN    6   // 海浪（内置合成 WAV）
#define TONE_CUSTOM   7   // 网页上传的自定义 WAV
#define TONE_WAV_BASE 4   // >= 这个索引走 WAV 引擎
#define TONE_COUNT    8

static const char *TONE_NAMES[TONE_COUNT] = {
  "上行三音", "广播报时", "电子急促", "木琴",
  "晨光（柔和琶音）", "雨声", "海浪", "自定义铃声"
};

// 与 TONE_WAV_BASE..TONE_COUNT-1 一一对应的 SPIFFS 路径
static const char *RING_PATHS[TONE_COUNT - TONE_WAV_BASE] = {
  "/rings/sunrise.wav", "/rings/rain.wav", "/rings/ocean.wav", "/rings/custom.wav"
};
#define RING_DIR "/rings"
#define RING_TMP_PATH "/rings/upload.tmp"   // 上传先写这里，校验通过才替换 custom.wav

// 内置 WAV 统一 22050Hz / 16bit / 单声道 / 6 秒，首尾幅度为 0 便于无缝循环。
// 播放时由 i2sFeedTask 线性插值重采样到 44100Hz（正好 2 倍，不会累积漂移）。
#define RING_RATE   22050
#define RING_SECS   6
#define WAV_HDR     44
// 上传限制：SPIFFS 总共 1.5MB，内置 3 段约 0.8MB，给自定义留 600KB
#define RING_MAX_UPLOAD (600 * 1024)


// 灯板测试架用的 GPIO7 是另一块 S3 测试板的接法，与本板无关；本板固定 GPIO1。

#define FW_VERSION    "4.1"
#define AP_NAME       "XiaoMiClock"

// ---- 发射功率 ----
// C3 自制板因发射链供电/去耦不足，10dBm 以上波形压塌，曾临时限到 6dBm。
// 新板直接用 XIAO ESP32-S3 Sense 模组（U.FL 天线 + 模组内已调好的匹配），
// 射频由模组保证，恢复满功率 20dBm。若以后换成自制天线又出现搜不到热点，
// 再把它调回 6 用来隔离硬件问题。
#define CFG_TX_DBM    20

// 时间统一的返回结构（放最前面：Arduino 会在第一个函数前插原型，
// 类型定义必须比原型更早出现，否则报 "does not name a type"）
struct NowTime {
  int h, m, s;
  uint8_t wd;        // 星期：0=周一 … 6=周日
  uint32_t key;      // 分钟级去重键
  bool valid;        // 只有 SNTP 校时成功才为 true
};

// WAV 流式播放状态。**必须放在文件顶部、第一个函数之前**：
// Arduino builder 会把 wavNext()/wavOpen() 的原型插到第一个函数定义之前，
// 结构体若留在音频段里，那些原型就会引用到尚未声明的 WavState 而编译失败
// （和上面 NowTime 必须前置是同一个坑）。
struct WavState {
  File     f;
  uint16_t rate, channels, blockAlign;
  uint32_t dataSize, dataOff, frameLeft;
  double   phase, step;
  int16_t  cur, next;
  uint8_t  rb[1024];
  uint32_t rbLen, rbPos;
  bool     loop, running;
  float    gain, tgt;
};

// 闹钟：每组独立的开关 / 时间 / 重复星期
#define MAX_ALARMS      5
#define ALARM_DAYS_ALL  0x7F     // 每天
#define ALARM_DAYS_WORK 0x1F     // 周一~周五
#define ALARM_DAYS_WEND 0x60     // 周六、周日
struct Alarm {
  uint8_t enable;
  uint8_t hour;
  uint8_t min;
  uint8_t days;      // bit0=周一 … bit6=周日
};


// ============================================================
//  TM1652 驱动（来自 board_test v4，实测可用）
// ============================================================
#define DRIVE_CURRENT    6
#define TM1652_BITDELAY  49
#define TM1652_WAITCMD   3000L
#define TM1652_CMD_MODE    0x18
#define TM1652_CMD_ADDRESS 0x08

// ---- 位映射 ----
//  BYTE0~3 第 1~4 位数字，段序 BIT0=a … BIT6=g, BIT7=dp
//  BYTE4   图标 BIT0=铃铛 BIT1=静音 BIT2=闹钟 BIT3=AM BIT4=无线 BIT5=PM
//  BYTE5   空置
//  冒号上点 = BYTE1 BIT7   中间横线 = BYTE2 BIT7（不用）   冒号下点 = BYTE3 BIT7
#define DIGIT_COUNT     4
#define ICON_BYTE       4
#define ICON_MASK       0x3F
#define COLON_UP_BYTE   1
#define COLON_DN_BYTE   3
#define COLON_MASK      0x80

#define ICON_BELL       0x01
#define ICON_MUTE       0x02
#define ICON_ALARM      0x04
#define ICON_AM         0x08
#define ICON_WIFI       0x10
#define ICON_PM         0x20

// 0~9 七段码（a~g，不含 dp）
static const uint8_t SEG_NUM[10] = {0x3F, 0x06, 0x5B, 0x4F, 0x66,
                                    0x6D, 0x7D, 0x07, 0x7F, 0x6F};
// 等待配网动画的段码（按原机动图逐帧拆段确认，见 drawWifiWaitFrame 说明）：
//   g 段中横 0x40 是两帧共有的固定底边，f|a|b = 0x23 只在其中一帧出现。
//   所以两帧是 0x40（只剩 G）与 0x63（G+F/A/B），视觉上只有 F/A/B 在闪。
#define SEG_G      0x40   // g 段中横：两帧都亮，固定不动
#define SEG_FAB    0x23   // f|a|b = 0x20|0x01|0x02：参与闪烁的三段
#define SEG_G_FAB  0x63   // g + f|a|b = 0x40|0x23：那个「上半方框」

// ============================================================
//  全局对象
// ============================================================
// 两个 I2S 控制器：0 = MAX98357 播放，1 = ICS-43434 录音。
// 必须显式 setPort()，否则第二个对象也会去抢 I2S_NUM_0。
I2SClass    spk;
I2SClass    mic;
WebServer   server(80);
DNSServer   dnsServer;
Preferences prefs;

// ============================================================
//  配置项（全部掉电保存）
// ============================================================
struct Settings {
  uint8_t  brightness;     // 平时亮度 0~7
  uint8_t  nightOn;        // 1=启用"夜间时段自动调暗"
  uint8_t  nightBright;    // 夜间时段亮度 0~7
  uint16_t nightStart;     // 夜间开始，单位=分钟（0~1439）
  uint16_t nightEnd;       // 夜间结束，单位=分钟（0~1439）
  uint8_t  mode24;         // 1=24H  0=12H
  uint8_t  snoozeMin;      // 0=不贪睡（所有闹钟共用）
  uint8_t  volume;         // 音量档位 1~10（最小 1，不允许静音）
  uint8_t  chime;          // 整点报时
  uint16_t chimeStart;     // 只在此时段内报时，单位=分钟（起止相同=全天）
  uint16_t chimeEnd;
  uint8_t  alarmTone;      // 闹钟铃声索引（TONE_xxx）
  uint8_t  chimeTone;      // 整点报时铃声索引（TONE_xxx）
  uint8_t  rgbOn;          // WS2812B 氛围灯总开关（新硬件）
  uint8_t  rgbBright;      // WS2812B 亮度 1~10（新硬件）
  // v4.1 唤醒体验
  uint8_t  sunriseOn;      // 1=响铃前用彩灯做日出渐亮
  uint8_t  sunriseMin;     // 提前多少分钟开始（5/10/15/20/30）
  uint8_t  sleepSound;     // 助眠音：0=雨声 1=海浪 2=不发声只做灯效
  uint8_t  sleepMin;       // 助眠时长（分钟，0=不自动停）
};
Settings cfg;

// 闹钟列表单独存（NVS 里存成一整块 blob）
Alarm    alarms[MAX_ALARMS];
uint8_t  alarmCount = 0;

// 存盘用的紧凑结构：没有 padding，跨版本读写稳定
struct AlarmBlob {
  uint8_t count;
  Alarm   a[MAX_ALARMS];
};

void loadCfg() {
  prefs.begin("clock", true);
  cfg.brightness     = prefs.getUChar("br",     4);
  // 本板没有光敏，亮度改成「自定义夜间时段」：
  // 默认 22:00 → 07:00 自动降到 1 级，其余时间用平时亮度
  cfg.nightOn        = prefs.getUChar("non",    1);
  cfg.nightBright    = prefs.getUChar("nbr",    1);
  cfg.nightStart     = prefs.getUShort("ns", 22 * 60);
  cfg.nightEnd       = prefs.getUShort("ne",  7 * 60);
  cfg.mode24         = prefs.getUChar("m24",    1);
  cfg.snoozeMin      = prefs.getUChar("sn",     5);
  cfg.volume         = prefs.getUChar("vol",    6);
  cfg.chime          = prefs.getUChar("chime",  0);
  // 报时时段：默认 00:00 → 00:00（起止相同视为全天都报），老固件升级上来行为不变
  cfg.chimeStart     = prefs.getUShort("cs",    0);
  cfg.chimeEnd       = prefs.getUShort("ce",    0);
  // 铃声：老固件没有这两个 key，取默认值——闹钟还是原来的上行三音，报时还是广播报时音
  cfg.alarmTone      = prefs.getUChar("atone",  TONE_UP3);
  cfg.chimeTone      = prefs.getUChar("ctone",  TONE_BELL);
  // 新硬件：WS2812B 氛围灯。老 NVS 里没有这两个 key，直接取默认值即可
  cfg.rgbOn          = prefs.getUChar("rgbon",  1);
  cfg.rgbBright      = prefs.getUChar("rgbb",   4);
  // v4.1：老 NVS 里没有这四个 key，全部取默认值（日出开 15 分钟、助眠雨声 30 分钟）
  cfg.sunriseOn      = prefs.getUChar("sron",   1);
  cfg.sunriseMin     = prefs.getUChar("srm",   15);
  cfg.sleepSound     = prefs.getUChar("sls",    0);
  cfg.sleepMin       = prefs.getUChar("slmin", 30);

  AlarmBlob b;
  memset(&b, 0, sizeof(b));
  bool haveBlob = prefs.isKey("alms");
  if (haveBlob) {
    if (prefs.getBytes("alms", &b, sizeof(b)) != sizeof(b)) b.count = 0;
  }
  // 老固件（只有单/双闹钟）升级上来时，把原来那一组搬成"闹钟 1"
  uint8_t oEn = prefs.getUChar("aen", 0);
  uint8_t oH  = prefs.getUChar("ah",  7);
  uint8_t oM  = prefs.getUChar("am", 30);
  prefs.end();

  if (!haveBlob) {
    b.count = 1;
    b.a[0].enable = oEn ? 1 : 0;
    b.a[0].hour   = (oH > 23) ? 7 : oH;
    b.a[0].min    = (oM > 59) ? 30 : oM;
    b.a[0].days   = ALARM_DAYS_ALL;
  }

  if (b.count > MAX_ALARMS) b.count = MAX_ALARMS;
  for (int i = 0; i < b.count; i++) {
    b.a[i].enable = b.a[i].enable ? 1 : 0;
    if (b.a[i].hour > 23) b.a[i].hour = 7;
    if (b.a[i].min  > 59) b.a[i].min  = 0;
    b.a[i].days &= 0x7F;
    if (b.a[i].days == 0) b.a[i].days = ALARM_DAYS_ALL;   // 0 表示永不响，没意义
  }
  alarmCount = b.count;
  memcpy(alarms, b.a, sizeof(alarms));

  if (cfg.brightness  > 7) cfg.brightness  = 4;
  if (cfg.nightBright > 7) cfg.nightBright = 1;
  if (cfg.nightStart >= 1440) cfg.nightStart = 22 * 60;
  if (cfg.nightEnd   >= 1440) cfg.nightEnd   =  7 * 60;
  cfg.nightOn = cfg.nightOn ? 1 : 0;
  // 报时时段越界就退回"全天"（起止相同 = 全天）
  if (cfg.chimeStart >= 1440) cfg.chimeStart = 0;
  if (cfg.chimeEnd   >= 1440) cfg.chimeEnd   = 0;

  // 老固件的音量是 0~255，现在改成 1~10 档（没有 0 档，不允许静音）。
  // 折算基准是"老固件的 50 = 新固件的 10 档"，因为满档响度就是照 50 校准的。
  // 不能按 0~255 线性折算：那样老的 50 会被算成 2 档，等于把音量砍到五分之一。
  if (cfg.volume > VOL_LEVELS) {
    uint8_t v = cfg.volume;
    cfg.volume = (uint8_t)constrain(((int)v * VOL_LEVELS + VOL_REF_OLD / 2) / VOL_REF_OLD,
                                    1, VOL_LEVELS);
  }
  if (cfg.volume < 1)           cfg.volume = 1;      // 不允许 0 / 静音
  if (cfg.volume > VOL_LEVELS)  cfg.volume = VOL_LEVELS;

  // 亮度同理：最小 1 档，调到 0 会让屏全灭，等于关掉闹钟
  if (cfg.brightness  < 1) cfg.brightness  = 1;
  if (cfg.nightBright < 1) cfg.nightBright = 1;

  // 铃声索引落在库外（比如以后删掉某个音色）就退回默认，别让 playTone 拿到野值
  if (cfg.alarmTone >= TONE_COUNT) cfg.alarmTone = TONE_UP3;
  if (cfg.chimeTone >= TONE_COUNT) cfg.chimeTone = TONE_BELL;
  cfg.rgbOn = cfg.rgbOn ? 1 : 0;
  if (cfg.rgbBright < 1)  cfg.rgbBright = 1;
  if (cfg.rgbBright > 10) cfg.rgbBright = 10;

  cfg.sunriseOn = cfg.sunriseOn ? 1 : 0;
  // 只认网页下拉里那几档，非法值（老配置/手工 curl）退回 15 分钟
  if (cfg.sunriseMin != 5 && cfg.sunriseMin != 10 && cfg.sunriseMin != 15 &&
      cfg.sunriseMin != 20 && cfg.sunriseMin != 30) cfg.sunriseMin = 15;
  if (cfg.sleepSound > 2) cfg.sleepSound = 0;
  // 助眠时长下拉是 0/15/30/45/60，0 表示「一直放到手动停」
  if (cfg.sleepMin != 0  && cfg.sleepMin != 15 && cfg.sleepMin != 30 &&
      cfg.sleepMin != 45 && cfg.sleepMin != 60) cfg.sleepMin = 30;
}

// 恢复出厂进行中：这个标志一旦置上，所有保存动作全部拒绝。
// 原因：清完 NVS 之后 RAM 里的 cfg/alarms 还是旧值，只要有任何一条路径
// 在重启前调了 saveCfg()，刚清掉的配置就被整块写回去了 —— "清了跟没清一样"。
bool wiping = false;

void saveCfg() {
  if (wiping) return;              // 恢复出厂期间一律不落盘，否则刚清掉的配置会被写回去

  // 必须显式 begin(false)：loadCfg() 用的是只读句柄并且已经 end()，
  // 不重新打开写句柄的话，下面这些 putXxx 全都是空操作（写入被静默丢弃）。
  prefs.begin("clock", false);
  prefs.putUChar("br",   cfg.brightness);
  prefs.putUChar("non",  cfg.nightOn);
  prefs.putUChar("nbr",  cfg.nightBright);
  prefs.putUShort("ns",  cfg.nightStart);
  prefs.putUShort("ne",  cfg.nightEnd);
  prefs.putUChar("m24",  cfg.mode24);
  prefs.putUChar("sn",   cfg.snoozeMin);
  prefs.putUChar("vol",  cfg.volume);
  prefs.putUChar("chime",cfg.chime);
  prefs.putUShort("cs",  cfg.chimeStart);
  prefs.putUShort("ce",  cfg.chimeEnd);
  prefs.putUChar("atone",cfg.alarmTone);
  prefs.putUChar("ctone",cfg.chimeTone);
  prefs.putUChar("rgbon",cfg.rgbOn);
  prefs.putUChar("rgbb", cfg.rgbBright);
  prefs.putUChar("sron", cfg.sunriseOn);
  prefs.putUChar("srm",  cfg.sunriseMin);
  prefs.putUChar("sls",  cfg.sleepSound);
  prefs.putUChar("slmin",cfg.sleepMin);

  AlarmBlob b;
  memset(&b, 0, sizeof(b));
  b.count = alarmCount;
  memcpy(b.a, alarms, sizeof(alarms));
  prefs.putBytes("alms", &b, sizeof(b));
  prefs.end();
}

// ============================================================
//  "HH:MM" → 分钟数
//  内部一律按"当天第几分钟"存，这样跨零点时段的判断就是一简单的整数比较，
//  不用管日期。网页现在只让选整点（下拉 00~23 点），但 value 仍是 "HH:00"，
//  所以这个解析函数和 /save 的协议都不用改，手工 curl 传 HH:MM 也照旧兼容。
// ============================================================
// 解析失败返回 -1，调用方据此保留原值
int hhmmToMinute(const String &s) {
  int c = s.indexOf(':');
  if (c < 0) return -1;
  int h = s.substring(0, c).toInt();
  int m = s.substring(c + 1).toInt();
  if (h < 0 || h > 23 || m < 0 || m > 59) return -1;
  return h * 60 + m;
}

// ============================================================
//  TM1652 底层时序
// ============================================================
static uint32_t tLastCmd = 0;
static byte frame[6] = {0, 0, 0, 0, 0, 0};                    // 待显示内容
static byte shown[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};  // 已上屏内容
static int  appliedBright = -1;

byte reverseByte(byte b) {
  b = (b & 0b11110000) >> 4 | (b & 0b00001111) << 4;
  b = (b & 0b11001100) >> 2 | (b & 0b00110011) << 2;
  b = (b & 0b10101010) >> 1 | (b & 0b01010101) << 1;
  return b;
}

void waitCmd() {
  while ((uint32_t)micros() - tLastCmd < TM1652_WAITCMD);
}

void endCmd() {
  tLastCmd = micros();
}

void tmSend(byte data) {
  pinMode(PIN_TM1652, OUTPUT);
  bool fParity = true;
  noInterrupts();

  digitalWrite(PIN_TM1652, LOW);
  delayMicroseconds(TM1652_BITDELAY);

  for (int nBit = 0; nBit < 8; nBit++) {
    if (data & 1) fParity = !fParity;
    digitalWrite(PIN_TM1652, (data & 1) ? HIGH : LOW);
    data >>= 1;
    delayMicroseconds(TM1652_BITDELAY);
  }

  digitalWrite(PIN_TM1652, fParity);
  delayMicroseconds(TM1652_BITDELAY);

  digitalWrite(PIN_TM1652, HIGH);
  interrupts();

  delayMicroseconds(TM1652_BITDELAY);
  delayMicroseconds(TM1652_BITDELAY);
}

void tmSendCommand(byte cmd) {
  waitCmd();
  tmSend(TM1652_CMD_MODE);
  tmSend(cmd);
  endCmd();
}

void tmSetBrightness(uint8_t intensity) {
  intensity &= 0x07;
  if ((int)intensity == appliedBright) return;
  byte cmd = reverseByte(((intensity & 0x07) << 1) | 0x01);
  cmd |= (reverseByte((DRIVE_CURRENT + 0x01) & 0x07) >> 4);
  tmSendCommand(cmd);
  appliedBright = intensity;
}

// 只把有变化的内容推上屏，锁存型芯片不需要持续刷新
void tmFlush(bool force = false) {
  if (!force && memcmp(frame, shown, 6) == 0) return;
  waitCmd();
  tmSend(TM1652_CMD_ADDRESS);
  for (int i = 0; i < 6; i++) tmSend(frame[i]);
  endCmd();
  memcpy(shown, frame, 6);
}

// ============================================================
//  显示层（画面元素操作）
// ============================================================
void dispClear() {
  memset(frame, 0, sizeof(frame));
}

// 写某一位数字；自动屏蔽 dp 位（dp 归冒号/横线用）
void dispDigit(uint8_t pos, byte code) {
  if (pos < DIGIT_COUNT) frame[pos] = code & 0x7F;
}

// 冒号上下两点（一起闪）
void dispColon(bool on) {
  const uint8_t pos[2] = {COLON_UP_BYTE, COLON_DN_BYTE};
  for (int i = 0; i < 2; i++) {
    if (on) frame[pos[i]] |= COLON_MASK;
    else    frame[pos[i]] &= ~COLON_MASK;
  }
}

void dispIcons(byte mask) {
  frame[ICON_BYTE] = mask & ICON_MASK;
}

// ============================================================
//  时间（系统时间 / NTP，本板无外部 RTC）
// ============================================================
unsigned long lastDisplayMs = 0;      // 5Hz 刷屏节拍
unsigned long lastSyncMs    = 0;
unsigned long lastSyncEpoch = 0;      // 0 = 从未校时
bool ntpStarted = false;

int  lastAlarmKey[MAX_ALARMS];        // 每组记"这一分钟已经响过"，默认 0 表示没响过
int  lastChimeKey  = -1;
bool alarmRinging  = false;
int  ringingIdx    = -1;              // 正在响的是第几组（-1 = 没响）
int  snoozeIdx     = 0;               // 贪睡结束后该响哪一组
unsigned long ringStartMs = 0;
bool snoozeArmed     = false;
unsigned long snoozeAtMs = 0;
unsigned long boostUntilMs  = 0;
bool resetHolding = false;        // 第二按键按住超过 5 秒 → 屏上开始倒数
unsigned long key2HoldMs = 0;     // 第二按键当前已按住多久（renderDisplay 用来算倒数）

// 第二按键长按阈值：5 秒开始倒数，8 秒真正执行
#define RESET_HOLD_WARN_MS  5000UL
#define RESET_HOLD_MS       8000UL

#define ALARM_RING_MS   60000UL      // 单次响铃时长
#define SYNC_INTERVAL   21600000UL   // 6 小时自动校时
#define BRIGHT_INTERVAL 500
#define BOOST_MS        3000         // 按键临时点亮时长

// ------------------------------------------------------------
//  统一的"现在几点"
//  新板没有 DS1302，XIAO 模组上也没焊 32.768kHz 后备晶振：
//  系统时间断电即丢，SNTP 拿到时间之前 valid=false（屏上走等待动画）。
//  联网后每 6 小时自动校时；这期间闹钟/报时都不会误触发。
// ------------------------------------------------------------
NowTime getNow() {
  NowTime n;
  n.h = n.m = n.s = 0;
  n.wd = 0;
  n.key = 0;
  n.valid = false;

  time_t t = time(nullptr);
  if (t > 1700000000UL) {                 // SNTP 已经拿到时间
    struct tm tmNow;
    localtime_r(&t, &tmNow);
    n.h = tmNow.tm_hour; n.m = tmNow.tm_min; n.s = tmNow.tm_sec;
    n.wd = (uint8_t)((tmNow.tm_wday + 6) % 7);          // tm_wday 0=周日 → 0=周一
    n.key = ((uint32_t)((tmNow.tm_mon + 1) * 31 + tmNow.tm_mday) * 24 + tmNow.tm_hour) * 60 + tmNow.tm_min;
    n.valid = true;
  }
  return n;
}

// ============================================================
//  WiFi 配网（自实现，全中文）
// ============================================================
char staSSID[33] = "";
char staPass[65] = "";

bool apActive   = false;
bool connecting = false;
unsigned long connectStartMs = 0;
unsigned long apCloseAtMs    = 0;
unsigned long lastRetryMs    = 0;
int  lastFailCode = 0;

// 「本次配网刚成功」：手机拿到 /netstatus 的成功结果后立刻关热点，
// 别让手机一直挂在这个开放热点上（挂着就没法上网，还得手动切 WiFi）。
// 只有配网成功会置位；手动 /portal、按键长按开的 10 分钟热点不受影响。
bool apCloseOnResult = false;

// 延迟重启：绝不在 HTTP 处理函数里直接 ESP.restart()
// 原因：server.send() 只是把响应塞进 lwIP 发送缓冲，立刻硬重启会把还没送达的
// 响应掐断，手机浏览器 fetch 就走 catch 分支弹"失败"，看着像设备死机。
// 这里改成先记一个时间点，回到 loop() 里等缓冲发完再重启。
unsigned long rebootAtMs = 0;
const char*   rebootWhy  = "";

void requestReboot(unsigned long delayMs, const char *why) {
  rebootAtMs = millis() + delayMs;
  rebootWhy  = why;
}

void loadWifiCreds() {
  prefs.begin("wifi", true);
  String s = prefs.getString("ssid", "");
  String p = prefs.getString("pass", "");
  prefs.end();
  s.toCharArray(staSSID, sizeof(staSSID));
  p.toCharArray(staPass, sizeof(staPass));
}

void saveWifiCreds() {
  if (wiping) return;              // 恢复出厂期间不落盘
  prefs.begin("wifi", false);      // 同 saveCfg()：必须重新打开写句柄
  prefs.putString("ssid", staSSID);
  prefs.putString("pass", staPass);
  prefs.end();
}

// 擦掉 ESP-IDF 驱动自己存的那份 WiFi 配置（NVS 分区里的 nvs.net80211）。
// 这是个很容易踩的坑：Preferences 的 "wifi" 命名空间和驱动存储是**两份独立数据**，
// 只清前者的话，下次 STA 一启动驱动就拿着旧记录自己连回上一个路由器 ——
// 表现就是"网页点了清除 WiFi，设备反而更乱/像死机"。
void eraseDriverWifiConfig() {
  WiFi.mode(WIFI_STA);              // 确保驱动已初始化，否则下面几个调用都 NOT_INIT
  delay(30);
  WiFi.disconnect(true, true);      // 第二个参数 true = 顺手清空 STA 配置
  esp_wifi_restore();               // 恢复出厂：彻底擦掉 nvs.net80211 里的记录
  delay(30);
}

// 只清 WiFi 凭据（不清其它设置）。清空结果会打出来：
// prefs.clear() 是有返回值的，之前一直丢掉，所以"到底清没清掉"根本无从判断。
void clearWifiCreds() {
  bool ok = prefs.begin("wifi", false);
  if (ok) ok = prefs.clear();
  prefs.end();
  staSSID[0] = 0;
  staPass[0] = 0;
  // 驱动层那份（nvs.net80211）故意留到重启后再擦：
  // 现在就 esp_wifi_restore() 会拆掉 netif，把还没送出去的 HTTP 响应一起丢掉，
  // 客户端只能看到连接被重置（curl 报 000）。开机时 staSSID 为空，
  // setup() 里的 eraseDriverWifiConfig() 会负责擦干净。
}

// 把发射功率压到 CFG_TX_DBM。必须在 WiFi 已 start（mode 之后）调用，
// 否则返回 ESP_ERR_WIFI_NOT_INIT，设置不生效。
static void applyTxPowerCap() {
  esp_err_t e = esp_wifi_set_max_tx_power(CFG_TX_DBM * 4);
  delay(50);
  // 只在失败时说话：成功是常态，不必刷屏
  if (e != ESP_OK) {
    int8_t rd = -1;
    esp_wifi_get_max_tx_power(&rd);
    Serial.printf("[RF] 限功率失败 err=%d，当前 %.1fdBm\n", (int)e, rd / 4.0f);
  }
}

void startPortal() {
  // 热点已经开着时只是取消待关闭定时器；手动重开 /portal 不该被「配网成功」的
  // 快速关闭逻辑顺手关掉，所以这里把标志一起清掉。
  if (apActive) { apCloseAtMs = 0; apCloseOnResult = false; return; }
  WiFi.mode(WIFI_AP_STA);
  delay(50);
  // AP 模式下必须关掉省电：开着 modem sleep 时射频会周期性入睡，
  // beacon 发不准点，手机扫描窗口正好撞上就一个都扫不到（这是很常见的坑）
  WiFi.setSleep(false);
  delay(20);
  applyTxPowerCap();

  bool ok = WiFi.softAP(AP_NAME);          // 开放热点，连上就能打开配置页
  if (!ok) {                               // 射频刚初始化偶尔会失败，重试一次
    delay(300);
    ok = WiFi.softAP(AP_NAME);
  }

  IPAddress apIp = WiFi.softAPIP();
  dnsServer.start(53, "*", apIp);
  apActive = true;
  apCloseAtMs = 0;
  apCloseOnResult = false;

  if (ok && (uint32_t)apIp != 0) {
    Serial.printf("[AP] 热点 %s 已开  http://%s\n", AP_NAME, apIp.toString().c_str());
  } else {
    Serial.println(F("[AP] 热点启动失败"));
  }
}

void stopPortal() {
  if (!apActive) return;
  bool byConnect = apCloseOnResult;
  dnsServer.stop();
  WiFi.softAPdisconnect(true);  // 内部已把模式切成 WIFI_STA，下面这行是幂等保险
  WiFi.mode(WIFI_STA);          // 切模式会重启射频，发射功率可能被打回默认值
  WiFi.setSleep(false);
  applyTxPowerCap();            // 所以必须重新限一次，否则会以 20dBm 发射而失联
  apActive = false;
  apCloseAtMs = 0;
  apCloseOnResult = false;
  Serial.printf("[AP] 热点已关闭（%s）\n", byConnect ? "配网成功" : "超时/手动");
}

void beginConnect(const char *ssid, const char *pass) {
  apCloseOnResult = false;       // 新一轮配网，先撤掉上一轮的快速关闭
  apCloseAtMs = 0;
  strncpy(staSSID, ssid, 32); staSSID[32] = 0;
  strncpy(staPass, pass, 64); staPass[64] = 0;
  saveWifiCreds();

  WiFi.mode(WIFI_AP_STA);      // 保证驱动处于启动状态，否则下面设功率会 NOT_INIT
  delay(50);
  WiFi.setSleep(false);
  applyTxPowerCap();

  WiFi.begin(staSSID, staPass);
  connecting   = true;
  connectStartMs = millis();
  lastFailCode = 0;
}

String failText(int code) {
  switch (code) {
    case WL_NO_SSID_AVAIL:
      return String(F("找不到这个网络。请确认路由器 2.4G 已开启、名称没写错；ESP32 搜不到 5G 频段。"));
    case WL_CONNECT_FAILED:
      return String(F("密码不对，或路由器用了 WPA3 / 企业级认证。请把加密方式改成 WPA2-PSK。"));
    case WL_CONNECTION_LOST:
      return String(F("连接中断，信号太弱。靠近路由器再试。"));
    case WL_DISCONNECTED:
      return String(F("连不上，请检查密码、路由器 MAC 白名单或是否开了防蹭网。"));
    default:
      return String(F("连接失败（错误码 ")) + String(code) + String(F("），请重试。"));
  }
}

void netTask() {
  unsigned long now = millis();

  if (connecting) {
    if (WiFi.status() == WL_CONNECTED) {
      connecting = false;
      lastFailCode = 0;
      Serial.printf("[NET] 已连接 %s  IP %s\n",
                    staSSID, WiFi.localIP().toString().c_str());
      ntpStarted = false;
      startSntp();
      // 配网成功后别再把开放热点挂 60 秒：手机挂在这个热点上就没法上网，
      // 还得手动切回家里 WiFi。手机轮询 /netstatus 拿到成功结果后，
      // handleNetStatus() 会把关闭时间压到 1 秒；这里只留 8 秒兜底，
      // 万一前端没来拿结果（比如用户直接关了页面）也不会一直开着热点。
      if (apActive) {
        apCloseOnResult = true;
        apCloseAtMs = now + 8000;
      }
      return;
    }
    if (now - connectStartMs > 20000) {           // 20 秒还没连上就算失败
      connecting = false;
      lastFailCode = (int)WiFi.status();
      Serial.printf("[NET] 连接失败 code=%d\n", lastFailCode);
      WiFi.disconnect(true, false);
      startPortal();                              // 重开热点，页面显示失败原因
    }
    return;
  }

  // 到点关热点：配网成功 = 手机拿到结果后 1 秒（或 8 秒兜底）；
  // 手动 /portal、按键长按开的热点 = 10 分钟。
  // 必须放在 loop 里执行，不能在 HTTP handler 里直接 stopPortal()：
  // server.send() 只是把响应塞进 lwIP 缓冲，立刻关 AP 会把还没送达的响应掐断。
  if (apActive && apCloseAtMs && now >= apCloseAtMs &&
      WiFi.status() == WL_CONNECTED) {
    stopPortal();
  }

  // 掉线兜底重连（热点开着的时候不抢，交给用户在配置页操作）
  if (!apActive && staSSID[0] && WiFi.status() != WL_CONNECTED &&
      now - lastRetryMs > 30000) {
    lastRetryMs = now;
    WiFi.reconnect();
    connecting = true;
    connectStartMs = now;
    lastFailCode = 0;
  }
}

// ============================================================
//  NTP 校时
// ============================================================
void startSntp() {
  configTime(8 * 3600, 0, "ntp.aliyun.com", "ntp.tencent.com", "cn.pool.ntp.org");
  ntpStarted = true;
}

// 新板没有 DS1302：所谓「校时」就是确认 SNTP 已把系统时间推上来。
// 保留老接口语义（/sync 返回 ok/fail、开机 5s 一轮催校时、之后 6 小时一次）。
bool syncFromNtp() {
  time_t now = time(nullptr);
  if (now < 1700000000UL) return false;      // SNTP 还没拿到时间

  bool first = (lastSyncEpoch == 0);
  lastSyncMs = millis();
  lastSyncEpoch = (unsigned long)now;
  if (first) {
    struct tm tmNow;
    localtime_r(&now, &tmNow);
    Serial.printf("[NTP] 校时成功 %04d-%02d-%02d %02d:%02d:%02d\n",
                  tmNow.tm_year + 1900, tmNow.tm_mon + 1, tmNow.tm_mday,
                  tmNow.tm_hour, tmNow.tm_min, tmNow.tm_sec);
  }
  return true;
}

void ntpTask() {
  static unsigned long lastCheck = 0;
  unsigned long now = millis();
  // 刚上电时别等 20 秒，先 5 秒一次尽快校上，稳定后只在需要时再校
  if (lastCheck && now - lastCheck < 5000) return;
  lastCheck = now;

  if (WiFi.status() != WL_CONNECTED) return;
  if (!ntpStarted) startSntp();

  // 本次开机还没校过，或者距上次超过 6 小时
  if (lastSyncEpoch == 0 || now - lastSyncMs >= SYNC_INTERVAL) syncFromNtp();
}

// ============================================================
//  音频：I2S → MAX98357（替代 C3 版 LEDC 方波 → PAM8403）
// ------------------------------------------------------------
//  MAX98357 是标准 I2S 从设备：BCLK=GPIO8、LRCK=GPIO9、DIN=GPIO7。
//  I2S_NUM_0 跑 44.1kHz / 16bit / 立体声，左右声道送同一份方波样本。
//  旋律调度仍在 loop() 里（audioTask），真正喂 DMA 的是独立的 FreeRTOS
//  任务：这样 /scan 这类阻塞 2~4 秒的请求不会把声音掐断。
// ============================================================
// ============================================================
//  SPIFFS 铃声库（v4.1）
// ------------------------------------------------------------
//  default_8MB.csv 里已经有 1.5MB spiffs 分区（0x670000），第 1 步不用动 16MB 分区。
//  内置 3 段 WAV 不是在电脑上生成再单独烧一个 spiffs 镜像，而是**首次开机由固件
//  合成写入**：升级固件仍然只需烧一个 app @0x10000，不会漏烧/错烧分区，
//  也避免把分区改造提前拖进这一轮。网页上传的自定义 WAV 覆盖 /rings/custom.wav。
// ============================================================
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static bool     fsReady = false;
static uint32_t rngState = 0x9E3779B9u;

// 确定性 LCG：每次开机合成的 WAV 逐字节一致，方便用 SHA/大小核对
static float ringNoise() {
  rngState = rngState * 1664525u + 1013904223u;
  int32_t v = (int32_t)((rngState >> 16) & 0xFFFF) - 32768;
  return (float)v / 32768.0f;
}

// kind: 0=晨光琶音 1=雨声 2=海浪。返回 [-1,1] 的浮点样本。
static float ringSynth(uint8_t kind, double t, uint32_t i) {
  static float lp = 0, lp2 = 0;
  if (i == 0) { lp = 0; lp2 = 0; }
  if (kind == 0) {
    // 4 个音符 × 1.5s：C5-E5-G5-C6，正弦基频 + 2/3 次谐波 + 指数衰减包络。
    // 比原来的方波柔和得多，适合做渐醒铃声（方波只有基频和奇次谐波，很刺耳）。
    static const float NF[4] = {523.25f, 659.25f, 784.0f, 1046.5f};
    int k = (int)(t / 1.5) % 4;
    double lt = t - k * 1.5;
    float f = NF[k];
    float env = (lt < 0.02) ? (float)(lt / 0.02) : (float)exp(-2.2 * (lt - 0.02));
    float ph = (float)(2.0 * M_PI * f * lt);
    return 0.62f * env * (sinf(ph) + 0.32f * sinf(2 * ph) + 0.12f * sinf(3 * ph));
  }
  float n = ringNoise();
  if (kind == 1) {
    // 雨：中低通底噪 + 一点高频「沙沙」，叠 0.17Hz 慢起伏
    lp += (n - lp) * 0.22f;
    float hp = n - lp;
    float mod = 0.78f + 0.22f * sinf((float)(2.0 * M_PI * 0.17 * t));
    return (lp * 0.85f + hp * 0.28f) * mod;
  }
  // 海浪：更低的低通 + 8 秒周期的潮汐包络
  // 用 s*s 而不是 powf(s,2)：sin 会取负，负数做非整数幂在部分 newlib 上会出 NaN
  lp2 += (n - lp2) * 0.055f;
  float s = sinf((float)(M_PI * t / 8.0));
  float tide = s * s;
  return lp2 * (0.55f + 0.85f * tide) * 1.6f;
}

// 标准 44 字节 PCM 头：22050Hz / 16bit / 单声道
static void writeWavHeader(File &f, uint32_t dataBytes) {
  uint8_t h[WAV_HDR] = {0};
  uint32_t riff = 36 + dataBytes, fmtLen = 16, rate = RING_RATE, byteRate = RING_RATE * 2;
  uint16_t pcm = 1, ch = 1, bits = 16, align = 2;
  memcpy(h,      "RIFF", 4);  memcpy(h + 4,  &riff, 4);
  memcpy(h + 8,  "WAVE", 4);  memcpy(h + 12, "fmt ", 4);
  memcpy(h + 16, &fmtLen, 4); memcpy(h + 20, &pcm, 2);
  memcpy(h + 22, &ch, 2);     memcpy(h + 24, &rate, 4);
  memcpy(h + 28, &byteRate,4);memcpy(h + 32, &align, 2);
  memcpy(h + 34, &bits, 2);   memcpy(h + 36, "data", 4);
  memcpy(h + 40, &dataBytes, 4);
  f.write(h, WAV_HDR);
}

static bool writeBuiltinRing(const char *path, uint8_t kind) {
  File f = SPIFFS.open(path, "w");
  if (!f) return false;
  uint32_t n = (uint32_t)RING_RATE * RING_SECS;
  writeWavHeader(f, n * 2);
  int16_t buf[256];
  rngState = 0x9E3779B9u;
  for (uint32_t i = 0; i < n; ) {
    uint32_t k = n - i;
    if (k > 256) k = 256;
    for (uint32_t j = 0; j < k; j++) {
      double t = (double)(i + j) / RING_RATE;
      float s = ringSynth(kind, t, i + j);
      // 首尾 50ms 归零：循环播放时接缝不会「咔」一声
      float env = 1.0f;
      if (t < 0.05) env *= (float)(t / 0.05);
      double tr = RING_SECS - t;
      if (tr < 0.05) env *= (float)(tr / 0.05);
      int32_t v = (int32_t)(s * env * 14000.0f);
      if (v >  32767) v =  32767;
      if (v < -32767) v = -32767;
      buf[j] = (int16_t)v;
    }
    if (f.write((const uint8_t *)buf, k * 2) != k * 2) { f.close(); return false; }
    i += k;
  }
  f.close();
  return true;
}

bool fsInit() {
  if (!SPIFFS.begin(true)) {          // true = 挂载失败时自动格式化（首次开机）
    Serial.println(F("[FS] SPIFFS 挂载失败（分区表里没有 spiffs 分区？）"));
    return false;
  }
  fsReady = true;
  for (uint8_t i = 0; i < 3; i++) {  // 只补内置 3 段；custom.wav 由网页上传
    const char *p = RING_PATHS[i];
    if (SPIFFS.exists(p)) continue;
    unsigned long ms = millis();
    bool ok = writeBuiltinRing(p, i);
    File chk = SPIFFS.open(p, "r");
    uint32_t sz = chk ? chk.size() : 0;
    if (chk) chk.close();
    Serial.printf("[FS] 合成 %s %s  %u 字节  %lu ms\n", p,
                  ok ? "ok" : "失败", (unsigned)sz, millis() - ms);
  }
  Serial.printf("[FS] SPIFFS %u/%u 字节已用\n",
                (unsigned)SPIFFS.usedBytes(), (unsigned)SPIFFS.totalBytes());
  return true;
}

// 自定义那一项只有在文件真的存在时才列进下拉
bool ringExists(uint8_t idx) {
  if (!fsReady || idx < TONE_WAV_BASE || idx >= TONE_COUNT) return true;
  return SPIFFS.exists(RING_PATHS[idx - TONE_WAV_BASE]);
}

// ============================================================
//  音频引擎：I2S → MAX98357（v4.1 双音源）
// ------------------------------------------------------------
//  同一个 DMA 供给任务 i2sFeedTask 支持两套音源：
//    AUD_TONE  原 4 种方波旋律（audioTask 在 loop 里推进音符）
//    AUD_WAV   SPIFFS 里的 WAV，流式读取 + 线性插值重采样 + 循环
//  切换只走 audReq 请求队列：File/SPIFFS 操作**只在音频任务里**发生，
//  HTTP handler 只置标志，避免 core0/core1 同时操作同一个文件系统。
// ============================================================
#define I2S_SPK_RATE   44100
#define I2S_FRAMES     256                  // 每块 256 立体声帧 ≈ 5.8ms
#define I2S_RAMP_SEC   0.006f               // 音头/收尾 6ms 淡变

enum { AUD_IDLE = 0, AUD_TONE = 1, AUD_WAV = 2 };
enum { AUDREQ_NONE = 0, AUDREQ_WAV = 1, AUDREQ_STOP = 2 };

static volatile uint32_t toneFreq = 0;      // 当前频率（Hz）
static volatile int16_t  toneAmp  = 0;      // 目标幅度（int16 峰值）
static volatile bool     spkReady = false;
static bool     audioOn   = false;          // 只表示「方波旋律在跑」
static const uint16_t *melF = nullptr;
static const uint16_t *melD = nullptr;
static uint8_t  melLen = 0, melIdx = 0;
static bool     melLoop = false;
static unsigned long noteStartMs = 0;

static int16_t i2sBuf[I2S_FRAMES * 2];     // 交错 L/R，静态分配不占任务栈

// ---- WAV 播放状态（只有 i2sFeedTask 会写；类型定义在文件顶部） ----
static WavState wav;

// 请求投递用临界区，保证另一核不会看到「新标志 + 旧路径」
static portMUX_TYPE audMux = portMUX_INITIALIZER_UNLOCKED;
static volatile uint8_t  audReq      = AUDREQ_NONE;
static volatile bool     audReqLoop  = false;
static char              audReqPath[48] = {0};

static volatile uint8_t  audMode      = AUD_IDLE;
static volatile uint32_t audWavPos    = 0;    // 已送出的输出帧数（实测抓手）
static volatile uint32_t audWavFrames = 0;    // 文件内原始帧数
static volatile uint16_t audWavRate   = 0;
static char              audWavPath[48] = {0};

// 从读缓冲里取下一个「单声道化」的原始样本
static bool wavNext(WavState &w, int16_t *out) {
  // 两轮机会：循环播放时若恰好卡在数据块末尾（读缓冲不足 2 字节），
  // 第一轮会失败并把 frameLeft 清零，第二轮自动 seek 回开头继续，
  // 不会因为「最后半个样本」就把循环提前停掉。
  for (uint8_t attempt = 0; attempt < 2; attempt++) {
    if (w.frameLeft == 0) {
      if (!w.loop) return false;
      w.f.seek(w.dataOff);
      w.rbLen = w.rbPos = 0;
      w.frameLeft = w.dataSize / w.blockAlign;
      if (!w.frameLeft) return false;
    }
    int32_t acc = 0;
    bool ok = true;
    for (uint16_t c = 0; c < w.channels; c++) {
      if (w.rbPos + 2 > w.rbLen) {
        uint32_t want = sizeof(w.rb);
        uint32_t left = w.frameLeft * w.blockAlign;
        if (want > left) want = left;
        if (want < 2) want = 2;
        w.rbLen = w.f.read(w.rb, want);
        w.rbPos = 0;
        if (w.rbLen < 2) { w.frameLeft = 0; ok = false; break; }
      }
      acc += (int16_t)(uint16_t)(w.rb[w.rbPos] | ((uint16_t)w.rb[w.rbPos + 1] << 8));
      w.rbPos += 2;
    }
    if (!ok) continue;
    w.frameLeft--;
    *out = (int16_t)(acc / (int32_t)w.channels);
    return true;
  }
  return false;
}

// 按块遍历 RIFF：能兼容带 LIST/fact 等非 44 字节头的 WAV（上传时常见）
static bool wavOpen(WavState &w, const char *path, bool loop) {
  if (w.f) w.f.close();
  w.f = SPIFFS.open(path, "r");
  if (!w.f) return false;
  uint32_t size = w.f.size();
  uint8_t rh[12];
  if (size < WAV_HDR || w.f.read(rh, 12) != 12 ||
      memcmp(rh, "RIFF", 4) || memcmp(rh + 8, "WAVE", 4)) {
    w.f.close(); return false;
  }
  uint16_t channels = 0, bits = 0, align = 0, format = 0;
  uint32_t rate = 0, dataSize = 0, dataOff = 0;
  uint8_t cb[8];
  uint32_t p = 12;
  while (p + 8 <= size) {
    w.f.seek(p);
    if (w.f.read(cb, 8) != 8) break;
    uint32_t sz = (uint32_t)cb[4] | ((uint32_t)cb[5] << 8) |
                  ((uint32_t)cb[6] << 16) | ((uint32_t)cb[7] << 24);
    if (!memcmp(cb, "fmt ", 4)) {
      uint8_t fm[16];
      if (sz >= 16 && w.f.read(fm, 16) == 16) {
        format   = (uint16_t)(fm[0] | (fm[1] << 8));
        channels = (uint16_t)(fm[2] | (fm[3] << 8));
        rate     = (uint32_t)fm[4] | ((uint32_t)fm[5] << 8) |
                   ((uint32_t)fm[6] << 16) | ((uint32_t)fm[7] << 24);
        align    = (uint16_t)(fm[12] | (fm[13] << 8));
        bits     = (uint16_t)(fm[14] | (fm[15] << 8));
      }
    } else if (!memcmp(cb, "data", 4)) {
      dataOff = p + 8;
      dataSize = sz;
      if (dataOff + dataSize > size) dataSize = size - dataOff;
      break;
    }
    if (sz > size - p - 8) break;
    p += 8 + sz + (sz & 1);
  }
  if (!dataOff || !dataSize || format != 1 || bits != 16 ||
      (channels != 1 && channels != 2) || rate < 8000 || rate > 48000 ||
      align < channels * 2) {
    w.f.close(); return false;
  }
  w.rate = (uint16_t)rate;
  w.channels = channels;
  w.blockAlign = align;
  w.dataSize = dataSize;
  w.dataOff = dataOff;
  w.frameLeft = dataSize / align;
  w.rbLen = w.rbPos = 0;
  w.phase = 0;
  w.step = (double)rate / I2S_SPK_RATE;
  w.loop = loop;
  w.running = true;
  w.gain = 0;
  w.tgt = (float)cfg.volume / VOL_LEVELS;
  w.f.seek(dataOff);
  int16_t first;
  if (!wavNext(w, &first)) { w.f.close(); return false; }
  w.cur = first;
  if (!wavNext(w, &w.next)) w.next = 0;
  return true;
}

static void wavClose(WavState &w) {
  if (w.f) w.f.close();
  w.running = false;
  w.frameLeft = 0;
}

static void audPost(uint8_t req, const char *path = nullptr, bool loop = false) {
  taskENTER_CRITICAL(&audMux);
  if (path) {
    strncpy(audReqPath, path, sizeof(audReqPath) - 1);
    audReqPath[sizeof(audReqPath) - 1] = 0;
    audReqLoop = loop;
  }
  audReq = req;
  taskEXIT_CRITICAL(&audMux);
}

// WAV → I2S：线性插值重采样到 44100Hz + 音量淡变；播完/停止后自动回 IDLE
static void wavFill(int16_t *dst, int frames) {
  WavState &w = wav;
  const float ga = 1.0f / (I2S_SPK_RATE * I2S_RAMP_SEC);
  for (int i = 0; i < frames; i++) {
    w.gain += (w.tgt - w.gain) * ga;
    int16_t v = 0;
    if (w.running) {
      double s = (double)w.cur + (double)(w.next - w.cur) * w.phase;
      int32_t sv = (int32_t)(s * w.gain);
      if (sv >  32767) sv =  32767;
      if (sv < -32767) sv = -32767;
      v = (int16_t)sv;
      w.phase += w.step;
      while (w.phase >= 1.0) {
        w.phase -= 1.0;
        w.cur = w.next;
        int16_t nx;
        if (wavNext(w, &nx)) w.next = nx;
        else { w.next = 0; w.running = false; w.tgt = 0; break; }
        if (!w.running) break;
      }
    }
    dst[i * 2]     = v;
    dst[i * 2 + 1] = v;
  }
  audWavPos += frames;
  if (!w.running && w.tgt == 0 && w.gain < 0.01f) {
    wavClose(w);                 // 淡出彻底完成才关文件、释放模式
    audMode = AUD_IDLE;
  }
}

// 方波 → I2S：v4.0 原逻辑，一行没动
static void toneFill(int16_t *dst, int frames, float &phase, float &amp) {
  uint32_t f = toneFreq;
  int16_t  target = toneAmp;
  const float alpha = 1.0f / (I2S_SPK_RATE * I2S_RAMP_SEC);
  for (int i = 0; i < frames; i++) {
    amp += ((float)target - amp) * alpha;
    int16_t v = 0;
    if (f) {
      phase += (float)f / I2S_SPK_RATE;
      if (phase >= 1.0f) phase -= 1.0f;
      v = (phase < 0.5f) ? (int16_t)amp : (int16_t)(-amp);
    }
    dst[i * 2]     = v;
    dst[i * 2 + 1] = v;
  }
}

// 持续喂 I2S：写满一块就阻塞等 DMA 腾空间，天然与采样率同步。
static void i2sFeedTask(void *) {
  float phase = 0.0f, amp = 0.0f;
  char path[48];
  bool loopReq = false;
  for (;;) {
    uint8_t req;
    taskENTER_CRITICAL(&audMux);
    req = audReq;
    if (req == AUDREQ_WAV) {
      strncpy(path, audReqPath, sizeof(path) - 1);
      path[sizeof(path) - 1] = 0;
      loopReq = audReqLoop;
    }
    if (req) audReq = AUDREQ_NONE;
    taskEXIT_CRITICAL(&audMux);

    if (req == AUDREQ_WAV) {
      if (wavOpen(wav, path, loopReq)) {
        audMode       = AUD_WAV;
        audWavPos     = 0;
        audWavFrames  = wav.frameLeft;
        audWavRate    = wav.rate;
        strncpy(audWavPath, path, sizeof(audWavPath) - 1);
        audWavPath[sizeof(audWavPath) - 1] = 0;
        Serial.printf("[WAV] 播放 %s  %uHz/%uch  %u 帧  %s\n", path,
                      (unsigned)wav.rate, (unsigned)wav.channels,
                      (unsigned)audWavFrames, loopReq ? "循环" : "一次");
      } else {
        audMode = AUD_IDLE;
        Serial.printf("[WAV] 打开失败或格式不支持 %s\n", path);
      }
    } else if (req == AUDREQ_STOP) {
      if (audMode == AUD_WAV) {
        // 必须同时把 running 置 false：只把 tgt 归零的话，文件会继续被读到尾、
        // pos 一直涨、mode 卡在 AUD_WAV，只是听不见而已（实测踩过这个坑）。
        wav.running = false;
        wav.tgt     = 0;                        // gain 6ms 淡出，别「啪」一声
      } else audMode = AUD_IDLE;
    }

    if (audMode == AUD_WAV) wavFill(i2sBuf, I2S_FRAMES);
    else                    toneFill(i2sBuf, I2S_FRAMES, phase, amp);

    if (!spkReady) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }
    spk.write((const uint8_t *)i2sBuf, sizeof(i2sBuf));
  }
}

bool audioInit() {
  if (!spk.setPort(I2S_NUM_0)) {
    Serial.println(F("[SPK] I2S 端口选择失败"));
    return false;
  }
  spk.setPins(PIN_SPK_BCLK, PIN_SPK_LRCK, PIN_SPK_DOUT);
  if (!spk.begin(I2S_MODE_STD, I2S_SPK_RATE, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO)) {
    Serial.println(F("[SPK] MAX98357 I2S 初始化失败"));
    return false;
  }
  // 绑到 core 0：Arduino loop() 默认在 core 1，声音不被网页/扫网阻塞。
  if (xTaskCreatePinnedToCore(i2sFeedTask, "i2s_spk", 4096, nullptr, 3, nullptr, 0) != pdPASS) {
    Serial.println(F("[SPK] 音频任务创建失败"));
    return false;
  }
  audMode = AUD_IDLE;
  spkReady = true;
  Serial.printf("[SPK] MAX98357 I2S ok  BCLK=%d LRCK=%d DIN=%d  %dHz/16bit\n",
                PIN_SPK_BCLK, PIN_SPK_LRCK, PIN_SPK_DOUT, I2S_SPK_RATE);
  return true;
}

#define N_C5 523
#define N_E5 659
#define N_G5 784
#define N_C6 1047
#define N_E6 1319
#define N_G6 1568

static const uint16_t T0_F[] = {N_C6, N_E6, N_G6, 0, N_C6, N_E6, N_G6, 0, N_G6, N_E6, N_C6, 0};
static const uint16_t T0_D[] = {150, 150, 280, 130, 150, 150, 280, 130, 150, 150, 320, 520};

// 1 广播授时信号：前四声 800Hz 短促「滴」，第五声 1600Hz 拖长。
// 全国广播用的是同一套，整点报时默认就是它。
static const uint16_t T1_F[] = {800, 0, 800, 0, 800, 0, 800, 0, 1600};
static const uint16_t T1_D[] = {110, 890, 110, 890, 110, 890, 110, 890, 1200};

// 2 电子急促：1900Hz 打 45ms、停 75ms，连打 8 下循环（周期 120ms ≈ 每秒 8 下）。
//    iPhone「雷达」是带和声和包络的合成音，方波复刻不了，但这种急促的"哒哒哒"
//    节奏是它最好认的特征。
static const uint16_t T2_F[] = {1900,0,1900,0,1900,0,1900,0,1900,0,1900,0,1900,0,1900,0,1900};
static const uint16_t T2_D[] = {45,75,45,75,45,75,45,75,45,75,45,75,45,75,45,75,45};

// 3 木琴：C5-E5-G5-C6-E6 上行再落回，像八音盒
static const uint16_t T3_F[] = {N_C5, N_E5, N_G5, N_C6, N_E6, N_C6, N_G5, 0};
static const uint16_t T3_D[] = {110, 110, 110, 150, 280, 150, 110, 420};

struct Tone { const uint16_t *f; const uint16_t *d; uint8_t len; };
static const Tone TONES[TONE_COUNT] = {
  { T0_F, T0_D, sizeof(T0_F) / sizeof(T0_F[0]) },
  { T1_F, T1_D, sizeof(T1_F) / sizeof(T1_F[0]) },
  { T2_F, T2_D, sizeof(T2_F) / sizeof(T2_F[0]) },
  { T3_F, T3_D, sizeof(T3_F) / sizeof(T3_F[0]) },
};

// 按索引播一个铃声。闹钟要 loop=true（一直响到你按键），报时 loop=false
// v4.1：索引 >= TONE_WAV_BASE 走 SPIFFS WAV 引擎；文件不存在（比如被删了）
// 就退回方波 0 号，保证闹钟无论如何都会响，不会出现「到点静音」。
bool playWav(const char *path, bool loop) {
  if (!fsReady || !SPIFFS.exists(path)) return false;
  // 切到 WAV 前必须把方波目标幅度清零：否则 WAV 播完切回 AUD_IDLE 后，
  // toneFill() 会把残留的 toneAmp 继续输出，冒出一段莫名其妙的方波。
  toneAmp  = 0;
  audioOn  = false;
  melF     = nullptr;
  audPost(AUDREQ_WAV, path, loop);
  return true;
}

void playTone(uint8_t idx, bool loop) {
  if (idx >= TONE_COUNT) idx = TONE_UP3;
  if (idx >= TONE_WAV_BASE) {
    if (playWav(RING_PATHS[idx - TONE_WAV_BASE], loop)) {
      audioOn = false;              // 停掉可能还在推进的方波旋律状态
      melF = nullptr;
      return;
    }
    idx = TONE_UP3;
  }
  audPost(AUDREQ_STOP);             // 从 WAV 切回方波时先把文件淡出
  playMelody(TONES[idx].f, TONES[idx].d, TONES[idx].len, loop);
}

// 幅度渐变由 i2sFeedTask 按 6ms 时间常数完成，主循环只改目标值。
// 老版要自己延时爬坡，是因为占空比一阶跃会经隔直电容耦合出「哒」声；
// I2S 一样会有这个阶跃，所以淡入/淡出语义原样保留。
static void audioSetTarget(uint16_t freq, int16_t amp) {
  toneAmp = amp;
  if (freq) toneFreq = freq;   // freq=0 保留上一个频率，让淡出自然走完
}

void audioApplyFreq(uint16_t freq) {
  if (freq == 0) { audioSetTarget(0, 0); return; }   // 收尾淡出
  int16_t amp = (int16_t)((int32_t)I2S_AMP_MAX * cfg.volume / VOL_LEVELS);
  audioSetTarget(freq, amp);
}

void audioStop() {
  audioApplyFreq(0);
  audioOn = false;
  melF = nullptr;
  melD = nullptr;
  melIdx = 0;
  melLen = 0;
  audPost(AUDREQ_STOP);             // WAV 那边同样要淡出并关文件
}

// chimeTask 用：WAV 播放期间也算「正在出声」，别让整点报时打断助眠/试听
bool audioActive() { return audioOn || audMode == AUD_WAV; }

void playMelody(const uint16_t *f, const uint16_t *d, uint8_t len, bool loop) {
  audioOn = true;
  melF = f; melD = d; melLen = len; melIdx = 0; melLoop = loop;
  noteStartMs = millis();
  audioApplyFreq(melF[0]);
}

void audioTask() {
  if (!audioOn || melF == nullptr) return;
  unsigned long now = millis();
  if (now - noteStartMs < melD[melIdx]) return;

  noteStartMs = now;
  melIdx++;
  if (melIdx >= melLen) {
    if (melLoop) melIdx = 0;
    else { audioStop(); return; }
  }
  audioApplyFreq(melF[melIdx]);
}

// ============================================================
//  亮度
//  本板没有光敏电阻，不做环境光采样。
//  亮度只由两件事决定：平时亮度 + 一个自定义时间段的夜间亮度。
// ============================================================
static float smoothBright = -1;

// 当前时刻是否落在夜间时段内。
// 时段允许跨零点（比如 22:00 → 07:00，此时 start > end），必须分两种情况判断，
// 只写 now>=start && now<end 的话跨零点那段会整段失效。
bool inNightWindow(int nowMin) {
  if (!cfg.nightOn) return false;
  int s = cfg.nightStart;
  int e = cfg.nightEnd;
  if (s == e) return false;                 // 起止相同视为不启用
  if (s < e)  return nowMin >= s && nowMin < e;
  return nowMin >= s || nowMin < e;         // 跨零点
}

// 当前时刻是否在"允许报时"的时段内。
// 和夜间调暗相反：这里起止相同 = 全天都报（默认值就是 00:00 → 00:00）。
// 理由：报时的总开关是 cfg.chime，时段只是"限制范围"，
// 不设范围时应该保持老行为（每小時都报），而不是突然一声都不响。
// 跨零点同样要单独处理：22:00 → 07:00 这种只报夜里那几小时。
bool inChimeWindow(int nowMin) {
  int s = cfg.chimeStart;
  int e = cfg.chimeEnd;
  if (s == e) return true;                  // 全天
  if (s < e)  return nowMin >= s && nowMin < e;
  return nowMin >= s || nowMin < e;         // 跨零点
}

// 当前应该用的亮度等级（不含短按点亮/响铃这类临时覆盖）
int baseBrightness() {
  NowTime t = getNow();
  if (!t.valid) return cfg.brightness;      // 时间没设好 → 老老实实用平时亮度
  int nowMin = t.h * 60 + t.m;
  return inNightWindow(nowMin) ? cfg.nightBright : cfg.brightness;
}

void brightTask() {
  unsigned long now = millis();

  int target;
  if (now < boostUntilMs)  target = 7;          // 短按临时点亮
  else if (alarmRinging)   target = 7;          // 响铃必须看得见
  else                     target = baseBrightness();

  if (smoothBright < 0) smoothBright = target;
  smoothBright += (target - smoothBright) * 0.35f;   // 平滑过渡，避免亮度跳变
  int lv = (int)(smoothBright + 0.5f);
  lv = constrain(lv, 0, 7);

  tmSetBrightness(lv);
}

// ============================================================
//  新外设 1：ICS-43434 I2S 麦克风（只做电平自检，不做录音/唤醒）
// ------------------------------------------------------------
//  WS=GPIO4、SCK=GPIO5、SD=GPIO6；32bit 帧、左声道为有效数据。
//  单独占 I2S_NUM_1，和功放互不干扰。/api/mic 返回一段 PCM 的 RMS/峰值，
//  装机时对着麦克风吹口气，数值应明显变化，用来确认焊接与极性。
// ============================================================
#define I2S_MIC_RATE  16000
static bool micReady = false;

bool micInit() {
  if (!mic.setPort(I2S_NUM_1)) {
    Serial.println(F("[MIC] I2S 端口选择失败"));
    return false;
  }
  mic.setPins(PIN_MIC_SCK, PIN_MIC_WS, -1, PIN_MIC_DIN);
  if (!mic.begin(I2S_MODE_STD, I2S_MIC_RATE, I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO)) {
    Serial.println(F("[MIC] ICS-43434 I2S 初始化失败"));
    return false;
  }
  micReady = true;
  Serial.printf("[MIC] ICS-43434 I2S ok  SCK=%d WS=%d SD=%d  %dHz/32bit\n",
                PIN_MIC_SCK, PIN_MIC_WS, PIN_MIC_DIN, I2S_MIC_RATE);
  return true;
}

// 诊断用：原始 32bit 样本里有多少个非 0，以及前 8 个样本的原值。
// 有 nz 但 rms=0  -> 数据落在低 8 位，被下面的 >>8 抹掉了（固件问题）
// nz=0 且 raw 全 0 -> SD 线上根本没有信号（供电 / LR / 焊接 / 时钟问题）
struct MicDiag {
  uint32_t nz;
  uint32_t raw[8];
};

bool micMeasure(uint32_t *rms, uint32_t *peak, MicDiag *diag = nullptr) {
  if (!micReady) return false;
  int32_t buf[128];
  MicDiag d = {0, {0}};
  // 先丢一块：DMA 里可能积着上次读剩下的旧样本
  if (mic.readBytes((char *)buf, sizeof(buf)) != sizeof(buf)) return false;
  if (mic.readBytes((char *)buf, sizeof(buf)) != sizeof(buf)) return false;
  uint64_t sum = 0;
  uint32_t mx = 0;
  for (int i = 0; i < 128; i++) {
    int32_t raw = buf[i];
    if (raw != 0) d.nz++;
    if (i < 8) d.raw[i] = (uint32_t)raw;
    int32_t v = raw >> 8;                 // 32bit 帧里的 24bit 有效数据
    uint32_t a = (uint32_t)(v < 0 ? -v : v);
    sum += (uint64_t)a * a;
    if (a > mx) mx = a;
  }
  *rms  = (uint32_t)sqrt((double)(sum / 128));
  *peak = mx;
  if (diag) *diag = d;
  return true;
}

// ============================================================
//  新外设 2：两颗 WS2812B（GPIO10，RMT 单帧推 48bit）
// ------------------------------------------------------------
//  内核的 rgbLedWrite() 一次只能推一颗灯，分两次调用会因帧间 >50µs
//  被识别成两帧，第二颗永远拿不到数据，所以这里自己拼 2×24 个 RMT 符号。
// ============================================================
static bool    wsReady = false;
static uint8_t wsLast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

static void wsSend(uint8_t r1, uint8_t g1, uint8_t b1,
                   uint8_t r2, uint8_t g2, uint8_t b2) {
  if (!wsReady) return;
  uint8_t c[6] = {g1, r1, b1, g2, r2, b2};     // WS2812B 线序 GRB
  if (memcmp(c, wsLast, 6) == 0) return;        // 没变化就别刷 RMT
  memcpy(wsLast, c, 6);
  rmt_data_t d[24 * WS2812_COUNT];
  for (int i = 0; i < 24 * WS2812_COUNT; i++) {
    bool one = (c[i / 8] >> (7 - (i % 8))) & 1;
    d[i].level0 = 1;  d[i].level1 = 0;
    d[i].duration0 = one ? 8 : 4;               // 10MHz tick：0.8/0.4µs
    d[i].duration1 = one ? 4 : 8;
  }
  rmtWrite(PIN_WS2812, d, RMT_SYMBOLS_OF(d), RMT_WAIT_FOR_EVER);
}

bool wsInit() {
  if (!rmtInit(PIN_WS2812, RMT_TX_MODE, RMT_MEM_NUM_BLOCKS_2, 10000000)) {
    Serial.println(F("[RGB] RMT 初始化失败"));
    return false;
  }
  rmtSetEOT(PIN_WS2812, 0);                     // 发完保持低电平（>50µs 即复位）
  wsReady = true;
  memset(wsLast, 0xFF, sizeof(wsLast));
  wsSend(0, 0, 0, 0, 0, 0);                    // 上电先灭，别乱闪
  Serial.printf("[RGB] WS2812B×%d ok  GPIO%d\n", WS2812_COUNT, PIN_WS2812);
  return true;
}

// ============================================================
//  v4.1 灯效场景：日出唤醒 / 助眠
// ------------------------------------------------------------
//  日出：只对「今天会响」的闹钟生效，找最近的一个，提前 cfg.sunriseMin 分钟
//        从暗红 → 橙 → 暖白渐亮；到点后交给 alarmRinging 的红闪。
//  助眠：循环白噪音 + 8 秒周期的呼吸渐暗，整体亮度随剩余时间继续往下走，
//        倒计时到点自动停；按主按键或网页「停止助眠」也能立刻结束。
// ============================================================
static bool     sleepOn       = false;
static unsigned long sleepStartMs = 0;
static unsigned long sleepEndMs   = 0;      // 0 = 一直放到手动停
static uint8_t  sleepActualMin = 0;         // 本次实际时长（可能和 cfg.sleepMin 不同）

// 返回日出进度 0.0~1.0；不在日出窗口返回 -1
float sunriseProgress() {
  if (!cfg.sunriseOn || !cfg.rgbOn || alarmRinging) return -1;
  NowTime t = getNow();
  if (!t.valid) return -1;
  double nowMin = t.h * 60 + t.m + t.s / 60.0;
  double best = 1e9;
  for (int i = 0; i < alarmCount; i++) {
    const Alarm &a = alarms[i];
    if (!a.enable || !(a.days & (uint8_t)(1 << t.wd))) continue;
    double target = a.hour * 60 + a.min;
    double d = target - nowMin;
    if (d < 0) d += 1440.0;                  // 跨零点：算到明天的同一时刻
    if (d < best) best = d;
  }
  if (best > cfg.sunriseMin) return -1;
  float p = 1.0f - (float)(best / cfg.sunriseMin);
  if (p < 0) p = 0;
  if (p > 1) p = 1;
  return p;
}

bool sleepStart(uint8_t minutes) {
  if (!fsReady) return false;
  sleepOn        = true;
  sleepActualMin = minutes;
  sleepStartMs   = millis();
  sleepEndMs     = minutes ? millis() + (unsigned long)minutes * 60000UL : 0;
  if      (cfg.sleepSound == 0) playWav(RING_PATHS[TONE_RAIN  - TONE_WAV_BASE], true);
  else if (cfg.sleepSound == 1) playWav(RING_PATHS[TONE_OCEAN - TONE_WAV_BASE], true);
  else                          audioStop();        // 2 = 只做灯效，不发声
  Serial.printf("[SLEEP] 开始 音效=%u 时长=%u 分钟\n",
                (unsigned)cfg.sleepSound, (unsigned)minutes);
  return true;
}

void sleepStop() {
  if (!sleepOn) return;
  sleepOn    = false;
  sleepEndMs = 0;
  audioStop();
  Serial.println(F("[SLEEP] 结束"));
}

void sleepTask() {
  if (!sleepOn) return;
  if (sleepEndMs && (long)(millis() - sleepEndMs) >= 0) sleepStop();
}

void wsTask() {
  if (!wsReady) return;
  static unsigned long last = 0;
  if (millis() - last < 100) return;            // 10Hz 足够，省 RMT
  last = millis();

  if (!cfg.rgbOn) { wsSend(0, 0, 0, 0, 0, 0); return; }

  bool blink = ((millis() / 500) & 1) == 0;
  uint8_t r = 0, g = 0, b = 0;
  if (alarmRinging) {                           // 闹铃：红色脉冲（最高优先级）
    r = blink ? 255 : 40;
  } else if (apActive && WiFi.status() != WL_CONNECTED) {   // 配网：蓝色慢闪
    b = blink ? 160 : 0;
  } else {
    float sp = sunriseProgress();
    if (sp >= 0) {
      // 暗红(12,0,0) → 橙(190,70,8) → 暖白(255,158,71)。
      // sp 取平方：开头几乎察觉不到，临近响铃才明显变亮，接近真实日出。
      float k = sp * sp;
      float r0, g0, b0;
      if (k < 0.6f) {
        float q = k / 0.6f;
        r0 = 12  + (190 - 12) * q;
        g0 =  0  + ( 70 -  0) * q;
        b0 =  0  + (  8 -  0) * q;
      } else {
        float q = (k - 0.6f) / 0.4f;
        r0 = 190 + (255 - 190) * q;
        g0 =  70 + (158 -  70) * q;
        b0 =   8 + ( 71 -   8) * q;
      }
      // 跟用户亮度走，但日出需要够亮才叫得醒，最低保底 0.35
      float scale = (float)cfg.rgbBright / 10.0f;
      if (scale < 0.35f) scale = 0.35f;
      r = (uint8_t)(r0 * scale);
      g = (uint8_t)(g0 * scale);
      b = (uint8_t)(b0 * scale);
    } else if (sleepOn) {
      // 8 秒呼吸 + 随剩余时间整体压暗；暖橙低蓝光，不刺眼
      float breath = 0.5f + 0.5f * sinf((float)(2.0 * M_PI * (millis() - sleepStartMs) / 8000.0));
      float fade = 1.0f;
      if (sleepEndMs) {
        unsigned long total = sleepEndMs - sleepStartMs;
        unsigned long left  = (millis() < sleepEndMs) ? (sleepEndMs - millis()) : 0;
        fade = total ? (float)left / (float)total : 0;
        if (fade < 0.05f) fade = 0.05f;
      }
      float lvl = 90.0f * (0.18f + 0.82f * breath) * fade *
                  (0.4f + 0.6f * (float)cfg.rgbBright / 10.0f);
      r = (uint8_t)lvl;
      g = (uint8_t)(lvl * 0.45f);
      b = (uint8_t)(lvl * 0.16f);
    } else {                                    // 平时：暖白夜灯
      NowTime t = getNow();
      int level = (int)cfg.rgbBright * 22;       // 1~10 → 22~220
      if (millis() < boostUntilMs) level = 220;  // 短按临时点亮
      else if (cfg.nightOn && t.valid && inNightWindow(t.h * 60 + t.m))
        level = level * cfg.nightBright / 7;     // 夜间跟着夜灯档位压暗
      if (level > 255) level = 255;
      r = (uint8_t)level;
      g = (uint8_t)(level * 0.62f);
      b = (uint8_t)(level * 0.28f);
    }
  }
  wsSend(r, g, b, r, g, b);
}

// ============================================================
//  闹钟 / 整点报时
// ============================================================
void startAlarm(int idx) {
  // 闹钟到点优先于助眠：先把助眠的白噪音/呼吸灯收掉，再放闹铃
  if (sleepOn) sleepStop();
  alarmRinging = true;
  ringingIdx   = idx;
  ringStartMs  = millis();
  snoozeArmed  = false;
  playTone(cfg.alarmTone, true);        // 循环放，直到按键停铃
  Serial.println(F("[ALARM] 响铃"));
}

void stopAlarm(bool allowSnooze) {
  audioStop();
  alarmRinging = false;
  if (allowSnooze && cfg.snoozeMin > 0) {
    snoozeArmed = true;
    snoozeIdx   = (ringingIdx >= 0) ? ringingIdx : 0;   // 贪睡后响的还是同一组
    snoozeAtMs  = millis() + (unsigned long)cfg.snoozeMin * 60000UL;
  } else {
    snoozeArmed = false;
  }
  ringingIdx = -1;
}

// 这一分钟该不该响：时间对上 + 今天在重复的星期里 + 这一分钟还没响过
bool alarmDue(int i, const NowTime &t) {
  const Alarm &a = alarms[i];
  if (!a.enable) return false;
  if (a.hour != t.h || a.min != t.m) return false;
  if (!(a.days & (uint8_t)(1 << t.wd))) return false;
  return t.key != lastAlarmKey[i];
}

bool anyAlarmEnabled() {
  for (int i = 0; i < alarmCount; i++) if (alarms[i].enable) return true;
  return false;
}

void alarmTask() {
  unsigned long now = millis();

  if (alarmRinging) {
    // 超时自动停，不再贪睡（只有人为按键停止才安排贪睡，避免无限重响）
    if (now - ringStartMs >= ALARM_RING_MS) stopAlarm(false);
    return;
  }

  if (snoozeArmed && (long)(now - snoozeAtMs) >= 0) {         // 贪睡到点
    startAlarm(snoozeIdx);
    return;
  }

  NowTime t = getNow();
  if (!t.valid) return;

  for (int i = 0; i < alarmCount; i++) {
    if (!alarmDue(i, t)) continue;
    // 同一时刻的多组只响一次，别轮着响
    for (int j = 0; j < alarmCount; j++) {
      if (alarms[j].hour == alarms[i].hour && alarms[j].min == alarms[i].min)
        lastAlarmKey[j] = t.key;
    }
    startAlarm(i);
    return;
  }
}

void chimeTask() {
  if (!cfg.chime || alarmRinging || audioActive()) return;

  NowTime t = getNow();
  if (!t.valid) return;

  // 只在自己设定的时段里报时。判的是"现在这一分钟"，
  // 而报时从 :59:55 起响，所以时段终点按分钟算就行，不用管那 5 秒。
  if (!inChimeWindow(t.h * 60 + t.m)) return;

  // 报时按广播的规矩来：整点前 5 秒（59:55）开始，五声走完正好压在整点上。
  // 原来是在整点那一刻才开始响，听着更像"提醒"，不像"报时"。
  // lastChimeKey 是分钟键，55~59 秒这几轮里只会真正触发一次。
  if (t.m == 59 && t.s >= 55 && t.key != lastChimeKey) {
    lastChimeKey = t.key;
    playTone(cfg.chimeTone, false);      // 只放一遍
  }
}

// ============================================================
//  等待配网时的动画（复刻小米原机开机配对画面）
// ------------------------------------------------------------
//  原机动图 2 帧循环、每帧 500ms（PIL 拆帧 + 放大数段确认）：
//    frame0：左右各一条中横（g 段）+ 中间下方一个小方点
//    frame1：那条中横变成「上半个方框」的底边，多出顶横 a、上左 f、上右 b；小方点不变
//  => g 段和中间小方点两帧都在（固定不动），**只有 f/a/b 三段在闪**。
//  段码：G = 0x40 固定；F|A|B = 0x23 闪；两帧分别是 0x40 与 0x40|0x23 = 0x63。
//  易错点：完整数字 0 = 0x3F 不含 g 段，绝不能套 0x3F；下半段 e/c/d 全程不参与，
//  所以「上半部分」不是只亮 a，也不是完整 0，而是 a+f+b 加一条固定的 g 底边。
//  四位数码管只写中间两位（digit1/2），冒号下点 BYTE3 BIT7 当那个常亮小方点，
//  左右两位留空。只往 frame 里画、不 flush，图标和刷新交给调用方
//  （配网时调用方会让右下角无线图标跟着同一个 blink 一起闪）。
// ============================================================
static void drawWifiWaitFrame(bool phase) {
  byte glyph = phase ? SEG_G_FAB : SEG_G;
  dispDigit(1, glyph);
  dispDigit(2, glyph);
  frame[COLON_DN_BYTE] |= COLON_MASK;    // 中间小方点，两帧都亮（固定不动）
}

// ============================================================
//  画面渲染
// ============================================================
void renderDisplay() {
  unsigned long now = millis();

  // 第二按键按住 5 秒后：屏上倒数 3 → 2 → 1，数到 0 才真正重置。
  // 之前这里是四个 0 一起闪，看起来像"已经开始重置了"，很多人就在这一秒松手了
  // （实测有人按到 5.9 秒松手，差 2 秒，结果什么都没清）。
  // 倒数是为了回答"还要按多久"，而不是让人猜。
  if (resetHolding) {
    dispClear();
    int left = (int)((RESET_HOLD_MS - key2HoldMs + 999) / 1000);  // 3/2/1
    if (left < 1) left = 1;
    if (left > 9) left = 9;
    // 靠在最右边一位，左边留空，看起来就是个倒数
    dispDigit(DIGIT_COUNT - 1, SEG_NUM[left]);
    tmFlush();
    return;
  }

  dispClear();
  bool blink = ((now / 500) & 1) == 0;
  byte icons = 0;

  // 等待配网：g 段中横常亮，f/a/b 三段每 500ms 闪一次，一直循环；
  // 右下角无线图标跟着同一个 blink 一起闪（v3.9 的原行为，v3.11 被误删，v3.14 恢复）。
  // 中间下方小方点常亮、左右两位留空。连上 WiFi 后走正常时钟，图标改为常亮。
  if (apActive && WiFi.status() != WL_CONNECTED) {
    drawWifiWaitFrame(blink);
    dispIcons(blink ? ICON_WIFI : 0);
    tmFlush();
    return;
  }

  NowTime t = getNow();

  if (!t.valid) {
    // 刚上电、还没联网校时也用同一套闪烁，别再摆四个横线
    drawWifiWaitFrame(blink);
  } else {
    int hh = t.h;
    int mm = t.m;

    if (!cfg.mode24) {
      if (hh == 0) hh = 12;
      else if (hh > 12) hh -= 12;
    }

    // 4 位数码管始终写满 4 位：1:05 也要显示成 01:05，不留空位
    dispDigit(0, SEG_NUM[hh / 10]);
    dispDigit(1, SEG_NUM[hh % 10]);
    dispDigit(2, SEG_NUM[mm / 10]);
    dispDigit(3, SEG_NUM[mm % 10]);

    dispColon(blink);

    if (!cfg.mode24) icons |= (t.h < 12 ? ICON_AM : ICON_PM);
  }

  if (alarmRinging) {
    icons |= ICON_BELL;
    if (blink) icons |= ICON_ALARM;
  } else if (anyAlarmEnabled()) {
    icons |= ICON_ALARM;             // 有启用的闹钟就亮闹钟图标
  }
  // 音量最小就是 1 档，不存在静音，所以静音图标不再点亮
  if (WiFi.status() == WL_CONNECTED) icons |= ICON_WIFI;

  dispIcons(icons);
  tmFlush();
}

// ============================================================
//  按键
// ============================================================
static int keyStable = HIGH, keyLastRead = HIGH;
static unsigned long keyChangeMs = 0, keyDownMs = 0;
static bool keyLongFired = false;
static bool keyArmed = false;   // 开机时若按键已按下，必须先松开一次才生效

void onShortPress() {
  if (alarmRinging)      stopAlarm(true);         // 停止，并按设置安排贪睡
  else if (sleepOn)      sleepStop();             // 助眠中：短按直接结束
  else                   boostUntilMs = millis() + BOOST_MS;   // 夜里看一眼时间
}

void onMediumPress() {
  cfg.mode24 = !cfg.mode24;
  saveCfg();
}

void onLongPress() {
  startPortal();
  // 手动开的配网热点也给它个上限，10 分钟没人配就自动关，
  // 免得留一个开放热点一直开着（和网页里的 /portal 行为一致）
  apCloseAtMs = millis() + 600000UL;
}

// 恢复出厂共用流程：网页按钮和副按键长按走同一条路径，避免两边行为不一致。
// 顺序很关键：
//   1. 先上 wiping 锁 → 之后任何 saveCfg()/saveWifiCreds() 都被拒绝，
//      否则清完到重启这 1 秒里被写回去，"清了跟没清一样"
//   2. 清两个 NVS 命名空间，并把 clear() 的返回值打出来
//      （这是唯一能确认"到底清干净没有"的证据，之前返回值一直被丢掉）
//   3. RAM 里的 cfg/alarms 一起复位，否则界面还拿着旧值
//   4. 交给主循环延迟重启
// 注意：这里**不调 stopPortal()**。切模式会重启射频、拆掉 netif，
// 网页那个请求的响应还没送达就会被一起丢掉（客户端只能看到连接被重置）。
// 反正 400ms 后就重启了，热点关不关无所谓。
void wipeAllAndReboot(const char *why) {
  wiping = true;
  Serial.printf("[RESET] 恢复出厂（%s）\n", why);

  clearWifiCreds();

  bool ok = prefs.begin("clock", false);
  if (ok) ok = prefs.clear();
  prefs.end();

  // NVS 已经空了，loadCfg() 会把编译期默认值填回 RAM
  loadCfg();
  memset(alarms, 0, sizeof(alarms));
  alarmCount = 0;

  requestReboot(400, why);
}

// 副按键长按 8 秒：屏上闪四个 0 作为反馈，再走共用流程
void factoryReset() {
  wipeAllAndReboot("按键长按 8 秒");

  for (int n = 0; n < 2; n++) {
    dispClear();
    for (int i = 0; i < DIGIT_COUNT; i++) dispDigit(i, SEG_NUM[0]);
    tmFlush(true);
    delay(200);
    dispClear();
    tmFlush(true);
    delay(150);
  }
}

void keyTask() {
  int raw = digitalRead(PIN_KEY);
  unsigned long now = millis();

  if (raw != keyLastRead) {
    keyLastRead = raw;
    keyChangeMs = now;
  }
  if (now - keyChangeMs < 25) return;             // 去抖

  if (keyStable == HIGH && now > 1000) keyArmed = true;

  if (raw != keyStable) {
    keyStable = raw;
    if (keyStable == LOW) {
      keyDownMs = now;
      keyLongFired = false;
    } else {
      unsigned long dur = now - keyDownMs;
      if (!keyLongFired) {
        if (dur < 800)       onShortPress();
        else if (dur < 3000) onMediumPress();
      }
    }
  }

  if (keyStable == LOW && keyArmed && !keyLongFired && now - keyDownMs >= 3000) {
    keyLongFired = true;
    onLongPress();
  }
}

// ============================================================
//  第二按键（GPIO2）：只负责长按 8 秒恢复出厂
//  和主按键分开，避免"长按配网"和"长按重置"在同一颗键上抢时长，
//  手误多按几秒就把 WiFi 清掉了。
// ============================================================
static int key2Stable = HIGH, key2LastRead = HIGH;
static unsigned long key2ChangeMs = 0, key2DownMs = 0;
static bool key2Armed  = false;   // 开机时若已按下，必须先松开一次才生效
static bool key2Fired  = false;

void key2Task() {
  int raw = digitalRead(PIN_KEY2);
  unsigned long now = millis();

  if (raw != key2LastRead) {
    key2LastRead = raw;
    key2ChangeMs = now;
  }
  if (now - key2ChangeMs < 25) { resetHolding = false; return; }   // 去抖

  if (key2Stable == HIGH && now > 1000) key2Armed = true;

  if (raw != key2Stable) {
    key2Stable = raw;
    if (key2Stable == LOW) {
      key2DownMs = now;
      key2Fired  = false;
    }
  }

  if (key2Stable != LOW || !key2Armed) {
    resetHolding = false;
    key2HoldMs   = 0;
    return;
  }

  unsigned long held = now - key2DownMs;
  key2HoldMs   = held;
  resetHolding = (held >= RESET_HOLD_WARN_MS);

  if (!key2Fired && held >= RESET_HOLD_MS) {
    key2Fired    = true;
    resetHolding = false;
    factoryReset();
  }
}

#include "web_pages.h"

// ============================================================
//  HTTP 处理
// ============================================================
String jsonEscape(const String &s) {
  String o;
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '"' || c == '\\') o += '\\';
    o += c;
  }
  return o;
}

void sendWifiPage() {
  server.send(200, "text/html; charset=utf-8", WIFI_HTML);
}

// 生成铃声下拉框的 <option> 串，当前选中的那个带 selected。
// 直接在固件里拼好塞进页面，比在 JS 里再回填一次少一次闪烁。
String toneOptions(uint8_t cur) {
  String s;
  uint8_t sel = ringExists(cur) ? cur : TONE_UP3;
  for (uint8_t i = 0; i < TONE_COUNT; i++) {
    if (!ringExists(i)) continue;        // 「自定义」没上传过就不列进下拉
    s += F("<option value=\"");
    s += String((int)i);
    s += '"';
    if (i == sel) s += F(" selected");
    s += '>';
    if (i >= TONE_WAV_BASE) s += F("[WAV] ");
    s += TONE_NAMES[i];
    s += F("</option>");
  }
  return s;
}

// 通用的「数值 → 下拉」生成器：日出时长 / 助眠音效 / 助眠时长共用
String numOptions(const uint16_t *vals, const char *const *labels,
                  uint8_t n, uint16_t cur) {
  String s;
  for (uint8_t i = 0; i < n; i++) {
    s += F("<option value=\"");
    s += String((unsigned)vals[i]);
    s += '"';
    if (vals[i] == cur) s += F(" selected");
    s += '>';
    s += labels[i];
    s += F("</option>");
  }
  return s;
}
static const uint16_t SRM_VALS[]   = {5, 10, 15, 20, 30};
static const char *const SRM_LBL[] = {"5 分钟", "10 分钟", "15 分钟", "20 分钟", "30 分钟"};
static const uint16_t SLMIN_VALS[]   = {0, 15, 30, 45, 60};
static const char *const SLMIN_LBL[] = {"手动停止", "15 分钟", "30 分钟", "45 分钟", "60 分钟"};
static const uint16_t SLS_VALS[]   = {0, 1, 2};
static const char *const SLS_LBL[] = {"雨声", "海浪", "仅灯效（静音）"};

// 生成"整点下拉"的 <option> 串（00 点 ~ 23 点），共给 4 个控件复用：
// 夜间时段的起/止、报时时段的起/止。
// 显示不再带分钟；value 仍然是 "HH:00"，这样 /save 那边的 hhmmToMinute
// 和内部按分钟存的逻辑一行都不用动。
// 传进来的 curMin 是分钟数，老配置万一存了 09:30，这里按小时取整显示 09 点。
String hourOptions(uint16_t curMin) {
  int cur = curMin / 60;
  if (cur > 23) cur = 0;
  String s;
  char b[8];
  for (int h = 0; h < 24; h++) {
    sprintf(b, "%02d", h);
    s += F("<option value=\"");
    s += b;
    s += F(":00\"");
    if (h == cur) s += F(" selected");
    s += '>';
    s += b;
    s += F(" 点</option>");
  }
  return s;
}

// 这个请求是不是从本机热点（192.168.4.x）进来的？
// 手机连着热点时，socket 的"本地地址"就是热点自己的 IP；走路由器时是 STA 的 IP。
// 点"重新配网"时设备仍然连着路由器，两种访问会同时存在，必须靠这个区分。
bool reqFromAp() {
  if (!apActive) return false;
  WiFiClient c = server.client();
  IPAddress li = c.localIP();
  if ((uint32_t)li != 0) return li == WiFi.softAPIP();
  // 个别情况下拿不到 localIP，退回按网段判断
  IPAddress ri = c.remoteIP();
  return ((uint32_t)ri & 0xFFFFFF00UL) == ((uint32_t)WiFi.softAPIP() & 0xFFFFFF00UL);
}

void handleRoot() {
  bool viaAp = reqFromAp();

  // 还没连上网，或者手机是连着本机热点进来的 → 首页给配网页
  if (WiFi.status() != WL_CONNECTED || viaAp) {
    sendWifiPage();
    return;
  }

  String page = PAGE_HTML;

  if (lastSyncEpoch == 0) {
    page.replace("{{SYNC}}", "等待联网校时（本板无 RTC）");
  } else {
    char buf[48];
    int mins = (int)((millis() - lastSyncMs) / 60000UL);
    if (mins < 60) sprintf(buf, "%d 分钟前", mins);
    else           sprintf(buf, "%d 小时前", mins / 60);
    page.replace("{{SYNC}}", String(buf) + "（网络时间）");
  }

  page.replace("{{BRIGHT}}", String(cfg.brightness));
  page.replace("{{NON}}",    cfg.nightOn ? "checked" : "");
  page.replace("{{NBR}}",    String(cfg.nightBright));
  page.replace("{{NS_OPTS}}", hourOptions(cfg.nightStart));
  page.replace("{{NE_OPTS}}", hourOptions(cfg.nightEnd));
  page.replace("{{M24}}",    cfg.mode24 ? "checked" : "");
  page.replace("{{SNOOZE}}", String(cfg.snoozeMin));
  page.replace("{{VOL}}",    String(cfg.volume));
  page.replace("{{CHIME}}",  cfg.chime ? "checked" : "");
  page.replace("{{ATONE_OPTS}}", toneOptions(cfg.alarmTone));
  page.replace("{{CTONE_OPTS}}", toneOptions(cfg.chimeTone));
  page.replace("{{CS_OPTS}}", hourOptions(cfg.chimeStart));
  page.replace("{{CE_OPTS}}", hourOptions(cfg.chimeEnd));
  page.replace("{{RGBON}}",  cfg.rgbOn ? "checked" : "");
  page.replace("{{RGBB}}",   String(cfg.rgbBright));
  page.replace("{{SRON}}",   cfg.sunriseOn ? "checked" : "");
  page.replace("{{SRM_OPTS}}",  numOptions(SRM_VALS,   SRM_LBL,   5, cfg.sunriseMin));
  page.replace("{{SLS_OPTS}}",  numOptions(SLS_VALS,   SLS_LBL,   3, cfg.sleepSound));
  page.replace("{{SLMIN_OPTS}}",numOptions(SLMIN_VALS, SLMIN_LBL, 5, cfg.sleepMin));
  char fsBuf[64];
  if (fsReady) snprintf(fsBuf, sizeof(fsBuf), "已用 %u KB / 共 %u KB",
                        (unsigned)(SPIFFS.usedBytes() / 1024),
                        (unsigned)(SPIFFS.totalBytes() / 1024));
  else         snprintf(fsBuf, sizeof(fsBuf), "SPIFFS 未就绪");
  page.replace("{{FSINFO}}", String(fsBuf));
  page.replace("{{FW}}",     FW_VERSION);
  page.replace("{{WIFI}}",   String(staSSID) + " · " + WiFi.localIP().toString());
  page.replace("{{IP}}",     WiFi.localIP().toString());

  server.send(200, "text/html; charset=utf-8", page);
}

void handleWifiPage() {
  sendWifiPage();
}

void handleScan() {
  int n = WiFi.scanNetworks();          // 阻塞 2~4 秒，只在打开配网页时调用
  String j = "{\"n\":[";
  int cnt = 0;
  for (int i = 0; i < n && cnt < 15; i++) {
    String s = WiFi.SSID(i);
    if (s.length() == 0) continue;
    int rssi = WiFi.RSSI(i);
    int bars = (rssi >= -55) ? 4 : (rssi >= -67) ? 3 : (rssi >= -75) ? 2 : 1;
    if (cnt) j += ",";
    j += "{\"s\":\"" + jsonEscape(s) + "\",\"bars\":" + String(bars) +
         ",\"e\":" + String(WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? 0 : 1) + "}";
    cnt++;
  }
  j += "]}";
  WiFi.scanDelete();
  server.send(200, "application/json; charset=utf-8", j);
}

void handleConnect() {
  String s = server.arg("ssid");
  String p = server.arg("pass");
  if (s.length() == 0) { server.send(200, "text/plain", "no-ssid"); return; }
  beginConnect(s.c_str(), p.c_str());
  server.send(200, "text/plain", "ok");
}

void handleNetStatus() {
  String j = "{";
  bool gotSuccess = false;
  if (WiFi.status() == WL_CONNECTED) {
    gotSuccess = true;
    j += "\"st\":2,\"ip\":\"" + WiFi.localIP().toString() +
         "\",\"ssid\":\"" + jsonEscape(String(staSSID)) + "\",\"msg\":\"\"";
  } else if (connecting) {
    j += "\"st\":1,\"ip\":\"\",\"ssid\":\"\",\"msg\":\"\"";
  } else if (lastFailCode) {
    j += "\"st\":3,\"ip\":\"\",\"ssid\":\"\",\"msg\":\"" + failText(lastFailCode) + "\"";
  } else {
    j += "\"st\":0,\"ip\":\"\",\"ssid\":\"\",\"msg\":\"\"";
  }
  j += "}";
  server.send(200, "application/json; charset=utf-8", j);

  // 手机已经拿到「连接成功 + IP」这条响应了，1 秒后关掉开放热点，
  // 手机就会自动回连家里 WiFi，不用手动切换。
  // 只在「本次刚配网成功」时生效：正常运行时访问 /netstatus、或手动 /portal
  // 开的 10 分钟热点，都不会被这里误关。
  // 注意只设定时器、绝不在这里直接 stopPortal() —— 那样会掐断刚 send 的响应。
  if (gotSuccess && apCloseOnResult) apCloseAtMs = millis() + 1000;
}

void handlePortal() {
  startPortal();
  // 手动开的配网热点：没人操作的话 10 分钟后自动关，不要一直开着
  apCloseAtMs = millis() + 600000UL;

  String html = F("<!DOCTYPE html><html lang='zh-CN'><head><meta charset='utf-8'>"
                  "<meta name='viewport' content='width=device-width,initial-scale=1'>"
                  "<title>配网页已开启</title></head>"
                  "<body style=\"font:16px/1.75 -apple-system,'PingFang SC',sans-serif;"
                  "padding:24px;background:#f2f2f7;color:#1c1c1e\">"
                  "<h3 style='margin:0 0 10px'>配网热点已开启</h3>"
                  "<p>1. 打开手机的 WiFi 列表，连上 <b>" AP_NAME "</b>（无密码）<br>"
                  "2. 连上后浏览器打开 <b>http://192.168.4.1</b>，通常会自动弹出配网页<br>"
                  "3. 选家里的 WiFi、输密码，点“连接”</p>"
                  "<p style='color:#8e8e93;font-size:14px'>连上新 WiFi 后热点会立刻自动关闭，"
                  "手机会自动切回家里的 WiFi，不用手动切换。</p>"
                  "</body></html>");
  server.send(200, "text/html; charset=utf-8", html);
}

void handleSave() {
  // 所有数值/开关一律「带了参数才改」。
  // 起因：实测时发了一条只带 cs/ce 的请求，server.arg("chime").toInt() 对
  // 不存在的参数返回 0，把 chime/m24/non/sn 全关了、vol 还被 constrain 成 1。
  // 网页正常提交时字段是齐的，但手工 curl、浏览器缓存的旧页面（还没有某个字段）
  // 都会踩这个坑，所以缺参数必须保留原值。
  if (server.hasArg("br"))    cfg.brightness  = constrain(server.arg("br").toInt(), 1, 7);  // 1~7，0 会全灭
  if (server.hasArg("non"))   cfg.nightOn     = server.arg("non").toInt() ? 1 : 0;
  if (server.hasArg("nbr"))   cfg.nightBright = constrain(server.arg("nbr").toInt(), 1, 7);

  // 网页现在只给整点下拉；这里再把分钟归零，保证手工 curl 传 "09:30" 时
  // 内部也统一是 09:00，不会出现"网页显示 09 点、实际 09:30"的不一致。
  // 解析失败（空串 / 缺参数）返回 -1，保留原值。
  int ns = hhmmToMinute(server.arg("ns"));
  int ne = hhmmToMinute(server.arg("ne"));
  if (ns >= 0) cfg.nightStart = (ns / 60) * 60;
  if (ne >= 0) cfg.nightEnd   = (ne / 60) * 60;

  // 报时时段同理
  int cs = hhmmToMinute(server.arg("cs"));
  int ce = hhmmToMinute(server.arg("ce"));
  if (cs >= 0) cfg.chimeStart = (cs / 60) * 60;
  if (ce >= 0) cfg.chimeEnd   = (ce / 60) * 60;

  if (server.hasArg("m24"))   cfg.mode24      = server.arg("m24").toInt() ? 1 : 0;
  if (server.hasArg("sn"))    cfg.snoozeMin   = constrain(server.arg("sn").toInt(), 0, 60);
  if (server.hasArg("vol"))   cfg.volume      = constrain(server.arg("vol").toInt(), 1, VOL_LEVELS);   // 1~10，没有静音档
  if (server.hasArg("chime")) cfg.chime       = server.arg("chime").toInt() ? 1 : 0;
  // 铃声索引：只有「带了这个参数」且「落在库范围内」才改。
  // 用 constrain 钳的话，手工传个 99 会变成最后一档（木琴），听着莫名其妙；
  // 用 hasArg 判断则是老页面（还没有这两个下拉）提交时不会把铃声重置掉。
  if (server.hasArg("atone")) {
    int at = server.arg("atone").toInt();
    if (at >= 0 && at < TONE_COUNT) cfg.alarmTone = (uint8_t)at;
  }
  if (server.hasArg("ctone")) {
    int ct = server.arg("ctone").toInt();
    if (ct >= 0 && ct < TONE_COUNT) cfg.chimeTone = (uint8_t)ct;
  }
  // 新硬件：WS2812B 氛围灯。同样「带参数才改」，老页面/手工 curl 不会误关灯
  if (server.hasArg("rgb"))  cfg.rgbOn     = server.arg("rgb").toInt() ? 1 : 0;
  if (server.hasArg("rgbb")) cfg.rgbBright = constrain(server.arg("rgbb").toInt(), 1, 10);
  // v4.1 唤醒体验：同样「带参数才改」，且只接受下拉里的合法档位，
  // 手工 curl 传野值不会写进 NVS 把下次开机的校验搞乱
  if (server.hasArg("sron")) cfg.sunriseOn = server.arg("sron").toInt() ? 1 : 0;
  if (server.hasArg("srm")) {
    int v = server.arg("srm").toInt();
    if (v == 5 || v == 10 || v == 15 || v == 20 || v == 30) cfg.sunriseMin = (uint8_t)v;
  }
  if (server.hasArg("sls")) cfg.sleepSound = constrain(server.arg("sls").toInt(), 0, 2);
  if (server.hasArg("slmin")) {
    int v = server.arg("slmin").toInt();
    if (v == 0 || v == 15 || v == 30 || v == 45 || v == 60) cfg.sleepMin = (uint8_t)v;
  }

  saveCfg();

  smoothBright = -1;                               // 亮度重新收敛（立即生效，不等 2Hz 任务）
  tmSetBrightness(baseBrightness());
  for (int i = 0; i < MAX_ALARMS; i++) lastAlarmKey[i] = 0;   // 允许当天重新触发


  server.send(200, "text/plain", "ok");
}

void handleStatus() {
  NowTime t = getNow();

  String j = "{\"t\":\"";
  if (t.valid) {
    // 跟屏幕保持一致：12 小时制时网页也显示 01:10 PM，不要一个 01:10 一个 13:10
    int hh = t.h;
    const char *ap = "";
    if (!cfg.mode24) {
      if (hh == 0) hh = 12;
      else if (hh > 12) hh -= 12;
      ap = (t.h < 12) ? " AM" : " PM";
    }
    char b[12];
    sprintf(b, "%02d:%02d%s", hh, t.m, ap);
    j += b;
  } else {
    j += "--:--";
  }
  j += "\",\"d\":\"";

  if (t.valid) {
    char b[36];
    time_t now = time(nullptr);
    struct tm tmNow;
    localtime_r(&now, &tmNow);
    sprintf(b, "%04d-%02d-%02d · ", tmNow.tm_year + 1900, tmNow.tm_mon + 1, tmNow.tm_mday);
    j += b;
  }
  if (WiFi.status() == WL_CONNECTED) j += "WiFi 已连接";
  else if (apActive)                 j += "配网热点已开启";
  else                               j += "WiFi 未连接";

  if (!t.valid) j += " · 等待 NTP 校时";

  j += " · 亮度 ";
  j += String(appliedBright < 0 ? 0 : appliedBright);
  if (cfg.nightOn && t.valid && inNightWindow(t.h * 60 + t.m)) j += "（夜间时段）";
  if (alarmRinging && ringingIdx >= 0) j += " · 闹钟" + String(ringingIdx + 1) + " 响铃";
  else if (alarmRinging)               j += " · 响铃中";
  else if (snoozeArmed)                j += " · 贪睡中";
  j += "\"}";

  server.send(200, "application/json; charset=utf-8", j);
}

void handleSync() {
  if (WiFi.status() == WL_CONNECTED && syncFromNtp()) {
    server.send(200, "text/plain", "ok");
  } else {
    server.send(200, "text/plain", "fail");
  }
}

// 新硬件自检：读一段 ICS-43434 的 PCM，返回 RMS / 峰值（24bit 量化）。
// 静止环境一般是几十~几百，对着麦克风吹气/说话会明显变大；
// 全 0 且不动 = 没焊好、L/R 选错声道或 SCK/WS 接反。
void handleMic() {
  uint32_t rms = 0, peak = 0;
  MicDiag diag = {0, {0}};
  bool ok = micMeasure(&rms, &peak, &diag);
  char b[224];
  snprintf(b, sizeof(b),
           "{\"ok\":%s,\"rms\":%u,\"peak\":%u,\"rate\":%d,\"nz\":%u,"
           "\"raw\":\"%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x\"}",
           ok ? "true" : "false", (unsigned)rms, (unsigned)peak, I2S_MIC_RATE,
           (unsigned)diag.nz,
           (unsigned)diag.raw[0], (unsigned)diag.raw[1],
           (unsigned)diag.raw[2], (unsigned)diag.raw[3],
           (unsigned)diag.raw[4], (unsigned)diag.raw[5],
           (unsigned)diag.raw[6], (unsigned)diag.raw[7]);
  server.send(200, "application/json", b);
}

// ============================================================
//  v4.1 HTTP：铃声库 / 音频引擎 / 日出 / 助眠
// ============================================================

// 上传后校验 WAV：只接受 16bit PCM、单/双声道、8k~48kHz。
// 不合格直接删掉，避免下拉里出现一个「选了却没声音」的坏文件。
bool wavCheck(const char *path) {
  File f = SPIFFS.open(path, "r");
  if (!f) return false;
  uint8_t h[12];
  if (f.read(h, 12) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) {
    f.close(); return false;
  }
  uint32_t size = f.size(), p = 12;
  bool fmtOk = false, dataOk = false;
  uint8_t cb[8];
  while (p + 8 <= size) {
    f.seek(p);
    if (f.read(cb, 8) != 8) break;
    uint32_t sz = (uint32_t)cb[4] | ((uint32_t)cb[5] << 8) |
                  ((uint32_t)cb[6] << 16) | ((uint32_t)cb[7] << 24);
    if (!memcmp(cb, "fmt ", 4) && sz >= 16) {
      uint8_t fm[16];
      if (f.read(fm, 16) == 16) {
        uint16_t format = (uint16_t)(fm[0] | (fm[1] << 8));
        uint16_t ch     = (uint16_t)(fm[2] | (fm[3] << 8));
        uint16_t bits   = (uint16_t)(fm[14] | (fm[15] << 8));
        uint32_t rate   = (uint32_t)fm[4] | ((uint32_t)fm[5] << 8) |
                          ((uint32_t)fm[6] << 16) | ((uint32_t)fm[7] << 24);
        fmtOk = (format == 1 && bits == 16 && (ch == 1 || ch == 2) &&
                 rate >= 8000 && rate <= 48000);
      }
    } else if (!memcmp(cb, "data", 4)) {
      dataOk = (sz > 0);
      break;
    }
    if (sz > size - p - 8) break;
    p += 8 + sz + (sz & 1);
  }
  f.close();
  return fmtOk && dataOk;
}

static File     ringUploadFile;
static uint32_t ringUploadSize = 0;
static bool     ringUploadOk   = false;

void handleRingUpload() {
  const char *dst = RING_PATHS[TONE_CUSTOM - TONE_WAV_BASE];
  HTTPUpload &u = server.upload();
  if (u.status == UPLOAD_FILE_START) {
    // 先停掉正在播的 WAV，别一边读一边覆盖同一个文件
    audioStop();
    unsigned long t0 = millis();
    while (audMode == AUD_WAV && millis() - t0 < 200) delay(5);
    ringUploadSize = 0;
    ringUploadOk   = fsReady;
    // 先写临时文件：校验不通过时只删临时文件，原有的自定义铃声原封不动。
    // 直接覆盖写 custom.wav 的话，「传错一次就把原来的铃声弄丢」了。
    if (fsReady) ringUploadFile = SPIFFS.open(RING_TMP_PATH, "w");
    if (!ringUploadFile) ringUploadOk = false;
  } else if (u.status == UPLOAD_FILE_WRITE) {
    if (ringUploadOk) {
      ringUploadSize += u.currentSize;
      if (ringUploadSize > RING_MAX_UPLOAD ||
          ringUploadFile.write(u.buf, u.currentSize) != u.currentSize) ringUploadOk = false;
    }
  } else if (u.status == UPLOAD_FILE_END) {
    if (ringUploadFile) ringUploadFile.close();
    if (ringUploadOk) ringUploadOk = wavCheck(RING_TMP_PATH);
    if (ringUploadOk) {
      SPIFFS.remove(dst);
      if (!SPIFFS.rename(RING_TMP_PATH, dst)) {
        // 个别平台 SPIFFS 不支持 rename：逐块复制兜底
        File src = SPIFFS.open(RING_TMP_PATH, "r");
        File out = SPIFFS.open(dst, "w");
        ringUploadOk = (src && out);
        if (ringUploadOk) {
          uint8_t b[512];
          size_t n;
          while ((n = src.read(b, sizeof(b))) > 0) {
            if (out.write(b, n) != n) { ringUploadOk = false; break; }
          }
        }
        if (src) src.close();
        if (out) out.close();
        SPIFFS.remove(RING_TMP_PATH);
        if (!ringUploadOk) SPIFFS.remove(dst);
      }
    }
    if (!ringUploadOk) {
      SPIFFS.remove(RING_TMP_PATH);
      Serial.println(F("[RING] 上传失败：超过 600KB 或不是 16bit PCM WAV（原铃声保留）"));
    } else {
      Serial.printf("[RING] 上传完成 %u 字节 -> %s\n", (unsigned)ringUploadSize, dst);
    }
  }
}

void handleRingUploadDone() {
  char b[80];
  snprintf(b, sizeof(b), "{\"ok\":%s,\"size\":%u}",
           (ringUploadOk && ringUploadSize) ? "true" : "false",
           (unsigned)ringUploadSize);
  server.send(200, "application/json", b);
}

// /api/rings：铃声库文件清单 + SPIFFS 用量（实测抓手）
void handleRings() {
  String j = "{\"fs\":";
  j += fsReady ? "true" : "false";
  j += ",\"total\":" + String((unsigned)(fsReady ? SPIFFS.totalBytes() : 0));
  j += ",\"used\":"  + String((unsigned)(fsReady ? SPIFFS.usedBytes()  : 0));
  j += ",\"rings\":[";
  for (uint8_t i = TONE_WAV_BASE; i < TONE_COUNT; i++) {
    if (i > TONE_WAV_BASE) j += ",";
    const char *p = RING_PATHS[i - TONE_WAV_BASE];
    bool ex = fsReady && SPIFFS.exists(p);
    uint32_t sz = 0;
    if (ex) { File f = SPIFFS.open(p, "r"); sz = f ? f.size() : 0; if (f) f.close(); }
    j += "{\"i\":" + String((int)i) + ",\"n\":\"" + TONE_NAMES[i] +
         "\",\"p\":\"" + p + "\",\"ok\":" + (ex ? "true" : "false") +
         ",\"size\":" + String((unsigned)sz) + "}";
  }
  j += "]}";
  server.send(200, "application/json; charset=utf-8", j);
}

// /api/wavprobe?i=4..7：把 WAV 头和前 8 个样本读回来。
// 只证明「文件存在」不够，得能证明固件合成/上传的确实是合法 PCM，
// 而且首尾样本接近 0（循环播放不爆音）。
void handleWavProbe() {
  int i = server.arg("i").toInt();
  if (i < TONE_WAV_BASE || i >= TONE_COUNT) {
    server.send(200, "text/plain", "bad-index"); return;
  }
  const char *p = RING_PATHS[i - TONE_WAV_BASE];
  File f = SPIFFS.open(p, "r");
  if (!f) { server.send(200, "text/plain", "no-file"); return; }
  uint8_t h[WAV_HDR];
  size_t hn = f.read(h, WAV_HDR);
  int16_t s[8] = {0};
  size_t sn = f.read((uint8_t *)s, sizeof(s));
  uint32_t sz = f.size();
  f.close();
  if (hn < WAV_HDR) { server.send(200, "text/plain", "short-header"); return; }
  uint32_t rate = (uint32_t)h[24] | ((uint32_t)h[25] << 8) |
                  ((uint32_t)h[26] << 16) | ((uint32_t)h[27] << 24);
  uint32_t dataBytes = (uint32_t)h[40] | ((uint32_t)h[41] << 8) |
                       ((uint32_t)h[42] << 16) | ((uint32_t)h[43] << 24);
  uint16_t ch   = (uint16_t)(h[22] | (h[23] << 8));
  uint16_t bits = (uint16_t)(h[34] | (h[35] << 8));
  char b[256];
  snprintf(b, sizeof(b),
           "{\"i\":%d,\"path\":\"%s\",\"size\":%u,\"riff\":\"%c%c%c%c\","
           "\"wave\":\"%c%c%c%c\",\"ch\":%u,\"rate\":%u,\"bits\":%u,"
           "\"dataBytes\":%u,\"first\":\"",
           i, p, (unsigned)sz, h[0], h[1], h[2], h[3],
           h[8], h[9], h[10], h[11], (unsigned)ch, (unsigned)rate,
           (unsigned)bits, (unsigned)dataBytes);
  String j = b;
  for (size_t k = 0; k < sn / 2; k++) {
    if (k) j += ",";
    j += String((int)s[k]);
  }
  j += "\"}";
  server.send(200, "application/json", j);
}

// /api/audio：当前是哪个音源、WAV 播到哪一帧。pos 持续增长 = 流式播放真的在跑
void handleAudioState() {
  char b[192];
  snprintf(b, sizeof(b),
           "{\"mode\":%u,\"melody\":%s,\"path\":\"%s\",\"rate\":%u,"
           "\"frames\":%u,\"pos\":%u,\"loop\":%s,\"vol\":%u}",
           (unsigned)audMode, audioOn ? "true" : "false", audWavPath,
           (unsigned)audWavRate, (unsigned)audWavFrames, (unsigned)audWavPos,
           wav.loop ? "true" : "false", (unsigned)cfg.volume);
  server.send(200, "application/json", b);
}

// /api/sleep?on=1[&min=30] 开始，?on=0 停止；不带参数只查状态
void handleSleep() {
  if (server.hasArg("on")) {
    if (server.arg("on").toInt()) {
      uint8_t mins = cfg.sleepMin;
      if (server.hasArg("min")) {
        int v = server.arg("min").toInt();
        if (v == 0 || v == 15 || v == 30 || v == 45 || v == 60) mins = (uint8_t)v;
      }
      if (!sleepStart(mins)) { server.send(200, "text/plain", "no-fs"); return; }
    } else {
      sleepStop();
    }
  }
  unsigned long left = 0;
  if (sleepOn && sleepEndMs && millis() < sleepEndMs) left = (sleepEndMs - millis()) / 1000;
  char b[144];
  snprintf(b, sizeof(b),
           "{\"on\":%s,\"left\":%lu,\"sound\":%u,\"min\":%u,\"cfgMin\":%u}",
           sleepOn ? "true" : "false", (unsigned long)left,
           (unsigned)cfg.sleepSound,
           (unsigned)(sleepOn ? sleepActualMin : cfg.sleepMin),
           (unsigned)cfg.sleepMin);
  server.send(200, "application/json", b);
}

// /api/sunrise：progress 为 null 表示当前不在日出窗口（实测可直接读数值）
void handleSunrise() {
  float p = sunriseProgress();
  String j = "{\"on\":" + String((unsigned)cfg.sunriseOn) +
             ",\"min\":" + String((unsigned)cfg.sunriseMin) + ",\"progress\":";
  if (p < 0) j += "null"; else j += String(p, 3);
  j += ",\"ringing\":";
  j += alarmRinging ? "true" : "false";
  j += "}";
  server.send(200, "application/json", j);
}

// ---------------- 闹钟列表接口 ----------------
void handleAlarmList() {
  String j = "[";
  for (int i = 0; i < alarmCount; i++) {
    if (i) j += ",";
    j += "{\"h\":" + String(alarms[i].hour) +
         ",\"m\":" + String(alarms[i].min) +
         ",\"d\":" + String(alarms[i].days) +
         ",\"e\":" + String(alarms[i].enable ? 1 : 0) + "}";
  }
  j += "]";
  server.send(200, "application/json; charset=utf-8", j);
}

// 改某一组：/alarm?i=0&h=7&m=30&d=127&e=1（只改传进来的字段）
void handleAlarmSet() {
  int i = server.arg("i").toInt();
  if (i < 0 || i >= alarmCount) { server.send(200, "text/plain", "bad-index"); return; }

  if (server.hasArg("h")) alarms[i].hour   = constrain(server.arg("h").toInt(), 0, 23);
  if (server.hasArg("m")) alarms[i].min    = constrain(server.arg("m").toInt(), 0, 59);
  if (server.hasArg("e")) alarms[i].enable = server.arg("e").toInt() ? 1 : 0;
  if (server.hasArg("d")) {
    uint8_t d = (uint8_t)(server.arg("d").toInt() & 0x7F);
    alarms[i].days = d ? d : ALARM_DAYS_ALL;
  }
  saveCfg();
  lastAlarmKey[i] = 0;
  server.send(200, "text/plain", "ok");
}

void handleAlarmAdd() {
  if (alarmCount >= MAX_ALARMS) { server.send(200, "text/plain", "full"); return; }
  Alarm &a = alarms[alarmCount];
  a.enable = 1;
  a.hour   = 7;
  a.min    = 0;
  a.days   = ALARM_DAYS_ALL;
  lastAlarmKey[alarmCount] = 0;
  alarmCount++;
  saveCfg();
  server.send(200, "text/plain", "ok");
}

void handleAlarmDel() {
  int i = server.arg("i").toInt();
  if (i < 0 || i >= alarmCount) { server.send(200, "text/plain", "bad-index"); return; }
  for (int k = i; k < alarmCount - 1; k++) {
    alarms[k] = alarms[k + 1];
    lastAlarmKey[k] = lastAlarmKey[k + 1];
  }
  alarmCount--;
  memset(&alarms[alarmCount], 0, sizeof(Alarm));
  saveCfg();
  server.send(200, "text/plain", "ok");
}

// 试听：?t=alarm 放闹钟铃声，?t=chime（或不带参数）放整点报时。
// 两边都只放一遍——闹钟真响的时候是循环的，试听要是也循环就停不下来了，
// 只能靠「立即静音」或者断电。
void handleAudioTest() {
  if (!alarmRinging) {
    if (server.arg("t") == "alarm") playTone(cfg.alarmTone, false);
    else                           playTone(cfg.chimeTone, false);
  }
  server.send(200, "text/plain", "ok");
}

void handleAudioStop() {
  stopAlarm(false);
  server.send(200, "text/plain", "ok");
}

// 网页「清除 WiFi 与全部设置」——和副按键长按走同一个 wipeAllAndReboot()
// 有一点必须说清楚：清完之后 STA 凭据就没了，设备重启会在 192.168.4.1
// 重新开热点，**不再回到原来的路由器网络**。所以手机/电脑刚才那个页面的
// 地址会彻底失效——这是清除成功，不是死机。
void handleResetWifi() {
  server.send(200, "text/plain", "ok");
  server.client().flush();          // 先把 ok 推给浏览器再动 NVS
  wipeAllAndReboot("网页按钮");
}

// 任意未知路径：从热点进来的请求跳到配网页（触发手机弹「登录网络」）；
// 走路由器的请求保持 404，别乱跳到一个它根本连不上的 192.168.4.1
void handleNotFound() {
  if (reqFromAp()) {
    String url = String("http://") + WiFi.softAPIP().toString() + "/";
    server.sendHeader("Location", url);
    server.send(302, "text/plain", "");
  } else {
    server.send(404, "text/plain", "404");
  }
}

// ============================================================
//  初始化
// ============================================================
void bootAnim() {
  // 开机自检：只确认 6 位都能点亮，不再拉满亮度、也不久留。
  // 以前是"亮度 7 + 全亮 400ms"，装机后每次上电都刺眼一下；
  // 现在用你自己设定的亮度（loadCfg 已经先跑过），总共 300ms，
  // 而且亮度从一开始就是目标值，不会出现"先很亮、再降下来"的跳变。
  tmSetBrightness(cfg.brightness);
  memset(frame, 0xFF, 6);            // 全亮自检
  tmFlush(true);
  delay(180);
  dispClear();
  // 自检全亮那一下已经把每一段（含 g 段）都点过了，收尾不再摆四个横线，
  // 直接进「G 固定 + F/A/B 亮」那一帧，开机画面到配网闪烁是连着的
  drawWifiWaitFrame(true);
  tmFlush(true);
  delay(120);
}

void setup() {
  Serial.begin(115200);
  delay(200);                        // 等 USB CDC 枚举完，别丢掉开机日志

  // GPIO0 是 BOOT/strapping 脚：上电瞬间被 ROM 采样，开机别按着它，
  // 否则直接进下载模式。跑起来之后当普通按键用没问题（内部上拉）。
  pinMode(PIN_KEY,  INPUT_PULLUP);
  pinMode(PIN_KEY2, INPUT_PULLUP);

  // TM1652 上电（单线总线空闲为高）
  pinMode(PIN_TM1652, OUTPUT);
  digitalWrite(PIN_TM1652, HIGH);
  delay(50);

  // 新硬件：彩灯 / I2S 功放 / I2S 麦克风，三者互不共用引脚
  wsInit();
  audioInit();
  micInit();
  // v4.1：首次开机把 3 段内置 WAV 合成进 SPIFFS（已存在则跳过，约 1~2 秒）
  fsInit();

  // 配置要在自检之前读出来：开机自检用「设定亮度」，不能固定拉满 7 档
  loadCfg();
  bootAnim();

  // 亮度先定下来，避免开机一瞬间全亮；彩灯同步给一次初始状态
  smoothBright = -1;
  brightTask();
  wsTask();

  // WiFi 必须先初始化：WebServer 底下是 esp_http_server，依赖 lwIP/netif，
  // 顺序反过来会在 httpd_start 里 xQueueSemaphoreTake 断言，开机就重启。
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  loadWifiCreds();

  if (staSSID[0]) {
    beginConnect(staSSID, staPass);
  } else {
    // 我们这边没有凭据 → 顺手把驱动层的旧记录也擦掉。
    // 否则 AP_STA 一开，驱动会拿 nvs.net80211 里的记录自动连回上一个路由器：
    // 热点刚起来就连上、60 秒后又被自动关掉，用户完全看不懂发生了什么。
    eraseDriverWifiConfig();
    startPortal();
  }

  // Web 服务（必须在 WiFi 初始化之后）
  server.on("/",           handleRoot);
  server.on("/wifi",       handleWifiPage);
  server.on("/scan",       handleScan);
  server.on("/connect",    handleConnect);
  server.on("/netstatus",  handleNetStatus);
  server.on("/portal",     handlePortal);
  server.on("/save",       handleSave);
  server.on("/api/status", handleStatus);
  server.on("/sync",       handleSync);
  server.on("/api/mic",    handleMic);
  server.on("/api/rings",  handleRings);
  server.on("/api/wavprobe",handleWavProbe);
  server.on("/api/audio",  handleAudioState);
  server.on("/api/sleep",  handleSleep);
  server.on("/api/sunrise",handleSunrise);
  // POST /api/ring：网页上传自定义 WAV（multipart，字段名 file）
  server.on("/api/ring", HTTP_POST, handleRingUploadDone, handleRingUpload);
  server.on("/api/alarms", handleAlarmList);
  server.on("/alarm",      handleAlarmSet);
  server.on("/alarmadd",   handleAlarmAdd);
  server.on("/alarmdel",   handleAlarmDel);
  server.on("/audiotest",  handleAudioTest);
  server.on("/audiostop",  handleAudioStop);
  server.on("/resetwifi",  handleResetWifi);
  server.onNotFound(handleNotFound);
  server.begin();

  // 只留这一行开机标识：确认版本、确认 WiFi 配置在不在，够用且不刷屏
  Serial.printf("[BOOT] XiaoMiClock v%s  WiFi=%s\n",
                FW_VERSION, staSSID[0] ? staSSID : "(未配置)");
}

// ============================================================
//  主循环
// ============================================================
void loop() {
  // 延迟重启放在最前面：等 HTTP 响应发完、显示刷完，再统一走一次 reboot
  if (rebootAtMs && (long)(millis() - rebootAtMs) >= 0) {
    Serial.printf("[SYS] 现在重启（%s）\n", rebootWhy);
    Serial.flush();
    delay(20);
    ESP.restart();
  }

  if (apActive) dnsServer.processNextRequest();
  server.handleClient();
  keyTask();
  key2Task();
  audioTask();
  alarmTask();
  chimeTask();
  netTask();
  sleepTask();

  // 5Hz 刷屏；tmFlush 内部做差量比较，不会白刷总线
  if (millis() - lastDisplayMs >= 200) {
    lastDisplayMs = millis();
    renderDisplay();
  }

  // 亮度按「平时亮度 + 夜间时段」计算，2Hz 刷新足够
  static unsigned long lastLight = 0;
  if (millis() - lastLight >= BRIGHT_INTERVAL) {
    lastLight = millis();
    brightTask();
  }

  wsTask();
  ntpTask();

  delay(5);
}
