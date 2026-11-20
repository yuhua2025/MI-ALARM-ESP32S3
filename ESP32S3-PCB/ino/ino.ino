/*
 * ============================================================
 *  小米闹钟 · ESP32-S3 主控固件  v4.7
 *  ------------------------------------------------------------
 *  由 ESP32-C3 版 v3.15 移植而来，保留核心：
 *    时钟显示 / 12-24H / 5 组闹钟 / 整点报时 / 中文配网页 / NTP 校时
 *  新硬件差异（详见 ../硬件接线表.md 与 README.md）：
 *    - 无 DS1302、无光敏：走时完全依赖 NTP，断电后需重新联网校时
 *    - 音频：PAM8403 + LEDC 方波 → MAX98357，走 I2S（GPIO7/8/9）
 *    - 两颗 WS2812B（GPIO10），网页可开关、调亮度
 *    - TM1652 单线 SDA = GPIO1；主按键 = GPIO0(BOOT)
 *  v4.2：彻底移除自定义铃声上传 / 日出唤醒 / 助眠模式；整点报时改真人语音。
 *  v4.3（本轮，按用户要求）：
 *    - v4.2 把 13 段碎片（现在时间/0~9/十/点整）在设备端拼接，听起来生硬
 *      （每段独立语调 + 首尾静音，段间有明显停顿）。
 *    - 改为**每个整点预生成一整句**：Tingting 一次读完「现在时间 X 点整」，
 *      24 段整句（0~23 点）由 voice/generate.py 预生成，编进固件，首次开机
 *      写入 SPIFFS /voice/hour_*.wav；到点只播对应那一段，无拼接、自然流畅。
 *    - 采样率 16000Hz（中文语音清晰，24 段约 1.8MB，app 总量约 2.6MB < 3.19MB 分区）。
 *  v4.4：修复报时时段端点不报的问题。旧逻辑拿「触发时刻的小时」
 *    （(X-1):59）判断、且用左闭右开区间，导致 7:00-22:00 的 7 点不报。
 *    改为按「被报整点 X」判断 + 闭区间含两端，跨零点同样含两端。
 *  v4.5（本轮）：
 *    - 分区表切到 16MB（本板 Flash 实测 16MB，之前一直按 8MB 编）：
 *      app 3.34MB → 6.55MB，SPIFFS 1.43MB → 3.54MB，见 build.sh。
 *    - 闹钟铃声彻底换掉：不再用设备端方波现算，改为 ringtone/generate.py
 *      离线合成的 6 段真实 WAV（多分音 + 各自衰减 + 音量包络 + 混响），
 *      编进 flash 循环播放，音色接近手机闹钟、有层次感。
 *    - 删除方波旋律引擎（melody 表 / playMelody / audioTask）。
 *  v4.6（本轮）：射频自检与加固。
 *    - C3 原版把发射功率限到 6dBm（自制板发射链不足），S3 移植时已恢复
 *      20dBm；本版把"关省电 + 满功率"合并成 applyRfPolicy()，并在
 *      STA 连上之后再下发一次，避免 mode 切换把 PHY 打回默认。
 *    - 新增 GET /api/rf：返回 rssi / tx_dbm / ps / channel，用于区分
 *      "省电导致的抖动" 和 "天线/距离导致的弱信号"。
 *  v4.7：网页头部第二行加实时信号质量（在「亮度」之前）：分档词 + dBm，
 *    随 /api/status 每秒刷新。
 *  显示驱动与位映射沿用「灯板测试/board_test v4」的实测结论。
 * ============================================================
 */

#include <WiFi.h>
#include <WiFiUdp.h>        // 自建 SNTP 查询用（不等 IDF 的首次发包时机）
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <time.h>
#include <stdlib.h>           // setenv + tzset：开机先把时区钉成北京时间
#include <math.h>
#include <SPIFFS.h>           // 16MB 分区表里的 3.54MB spiffs（default_16MB.csv）
#include <esp_system.h>
#include <esp_wifi.h>
#include <esp_heap_caps.h>    // heap_caps_get_largest_free_block：区分"没内存"和"没有连续内存"
#include <esp_sntp.h>        // 校时回调：只有真的收到 NTP 回包才认为"校时成功"
#include <ESP_I2S.h>          // arduino-esp32 3.x 自带（I2SClass / driver/i2s_std.h）
#include "chime_voice.h"      // 整点报时语音素材（自动生成，勿手改）
#include "ringtone_data.h"    // 闹钟铃声 WAV 数据（自动生成，勿手改）

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
//  闹钟铃声（v4.5）：6 段离线合成的真实 WAV
// ------------------------------------------------------------
//  上一版是「方波 + 音符表」在设备上现算 —— 只有基频和奇次谐波，
//  听起来又干又尖，像老式电子表。手机铃声的「层次感」来自
//  多分音各自衰减 + 音量包络 + 混响，这些实时算太吃 CPU，
//  改成在电脑上用 ringtone/generate.py 离线合成 16bit WAV，
//  编进 flash（ringtone_data.h），设备端只负责读出来循环播。
//
//  索引顺序 = 下拉框顺序 = NVS 里存的 atone 值，**不要随意调整**。
// ============================================================
#define TONE_COUNT    RING_COUNT
#define TONE_DEFAULT  0     // 晨曦
static const char *const *TONE_NAMES = RING_NAMES;

// 语音片段统一 22050Hz / 16bit / 单声道，播放时重采样到 44100Hz。
#define WAV_HDR     44


// 灯板测试架用的 GPIO7 是另一块 S3 测试板的接法，与本板无关；本板固定 GPIO1。

#define FW_VERSION    "4.8.12"
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
  // v4.3：语音整句编在 app 的 C 数组里（flash），直接播放，不再复制到 SPIFFS。
  // fromMem=true 时从 mem[memOff..memSize) 读，否则从 f 读。
  const uint8_t *mem;
  uint32_t memOff, memSize;
  bool     fromMem;
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
  uint8_t  rgbOn;          // WS2812B 氛围灯总开关
  uint8_t  rgbBright;      // WS2812B 亮度 1~10
  // v4.2：整点报时固定语音播报，不再有独立报时音索引
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

// ============================================================
//  启动/运行诊断（v4.7.1）
// ------------------------------------------------------------
//  优化不能靠感觉。这一层只回答四个问题，全部可测量：
//    · 开机用了多久、卡在哪一段   → bootMark() 分阶段打点
//    · 堆还剩多少、最大连续块多大 → /api/diag + [MEM] 一行
//    · 主循环真的跑到多少 Hz      → loopHz（delay(5) 理论上限 200）
//    · 上次为什么重启             → esp_reset_reason()
//  分阶段打点只在开机跑一遍，开销可以忽略。
// ============================================================
#define BOOT_STAGE_MAX 14
static uint32_t bootT0Us = 0;          // setup() 第一行的 micros()
static uint32_t bootStageUs[BOOT_STAGE_MAX];
static const char *bootStageName[BOOT_STAGE_MAX];
static int      bootStageN = 0;
static uint32_t bootToWifiMs = 0;      // 复位 → WiFi 连上（0 = 本次还没连上）
static uint32_t loopCount = 0;         // loop() 迭代计数
static uint16_t loopHz = 0;            // 最近一秒的实际循环频率
static unsigned long lastHzMs = 0;
// 每轮循环里"真正干活"的耗时（不含 delay(5)）。这个数决定了循环频率能不能到 200Hz，
// 也是判断"有没有东西在偷偷阻塞"最直接的证据。
static uint32_t loopWorkSum = 0;
static uint32_t loopWorkMax = 0;
static uint16_t loopWorkAvg = 0;

// 分项耗时：loopWorkAvg 只知道"一共花了 1.2ms"，不知道是谁花的。
// 这里按任务累计，每秒归一化成「每轮多少微秒」，一眼能看出主犯。
// 代价是每项两头各一次 micros()（约 2µs），9 项加起来不到 40µs，可以接受。
#define PROF_N 9
static uint32_t profSum[PROF_N];
static uint32_t profMax[PROF_N];
static uint16_t profAvg[PROF_N];
static const char *const PROF_NAME[PROF_N] = {
  "web", "key", "alarm", "chime", "net", "disp", "light", "ws", "fs"
};
enum { PROF_WEB, PROF_KEY, PROF_ALARM, PROF_CHIME, PROF_NET,
       PROF_DISP, PROF_LIGHT, PROF_WS, PROF_FS };

#define PROF_MEAS(i, stmt) do {                       \
    uint32_t _t0 = micros();                          \
    stmt;                                             \
    uint32_t _d = micros() - _t0;                     \
    profSum[i] += _d;                                 \
    if (_d > profMax[i]) profMax[i] = _d;              \
  } while (0)

// 首页三段耗时（拷贝模板 / 20 次替换 / 发送），由 handleRoot() 写入
static uint32_t pageBuildUs = 0, pageSendUs = 0, pageTotalUs = 0;

// 模板填表用的一对 key/value。必须定义在第一个函数之前：Arduino 会把
// 函数原型插到首个函数定义前，原型里引用的自定义类型得先存在。
struct TplVar {
  const char *key;
  const char *val;
};

// ------------------------------------------------------------
//  getNow() 实测 32µs（内部 time() + localtime_r()）。
//  alarmTask/chimeTask/wsTask 每轮都调它，一秒几百次纯属浪费 ——
//  这些判断的精度要求只有"秒"，100ms 缓存一次足够，
//  最坏情况是整点报时晚 0.1 秒触发（报时窗口有 5 秒，无影响）。
// ------------------------------------------------------------
static NowTime  nowCache;
static uint32_t nowCacheMs = 0;
static bool     nowCacheOk = false;

static const NowTime &nowFast() {
  uint32_t ms = millis();
  if (!nowCacheOk || ms - nowCacheMs >= 100) {
    nowCache   = getNow();
    nowCacheMs = ms;
    nowCacheOk = true;
  }
  return nowCache;
}

static void bootMark(const char *name) {
  uint32_t us = micros() - bootT0Us;
  if (bootStageN < BOOT_STAGE_MAX) {
    bootStageUs[bootStageN] = us;
    bootStageName[bootStageN] = name;
    bootStageN++;
  }
  Serial.printf("[BOOT] +%5lums  %s\n", (unsigned long)(us / 1000), name);
}

// 复位原因 → 短标签，用来区分"我点的重启"和"自己崩了/看门狗"
static const char *resetReasonText() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:  return "上电";
    case ESP_RST_EXT:      return "外部复位";
    case ESP_RST_SW:       return "软件重启";
    case ESP_RST_PANIC:    return "崩溃(panic)";
    case ESP_RST_INT_WDT:  return "中断看门狗";
    case ESP_RST_TASK_WDT: return "任务看门狗";
    case ESP_RST_WDT:      return "看门狗";
    case ESP_RST_BROWNOUT: return "电压跌落";
    case ESP_RST_DEEPSLEEP:return "深睡唤醒";
    case ESP_RST_USB:      return "USB 复位";
    case ESP_RST_JTAG:     return "JTAG 复位";
    default:               return "未知";
  }
}

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
  cfg.alarmTone      = prefs.getUChar("atone",  TONE_DEFAULT);
  // 新硬件：WS2812B 氛围灯。老 NVS 里没有这两个 key，直接取默认值即可
  cfg.rgbOn          = prefs.getUChar("rgbon",  1);
  cfg.rgbBright      = prefs.getUChar("rgbb",   4);

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
  if (cfg.alarmTone >= TONE_COUNT) cfg.alarmTone = TONE_DEFAULT;
  cfg.rgbOn = cfg.rgbOn ? 1 : 0;
  if (cfg.rgbBright < 1)  cfg.rgbBright = 1;
  if (cfg.rgbBright > 10) cfg.rgbBright = 10;
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
  prefs.putUChar("rgbon",cfg.rgbOn);
  prefs.putUChar("rgbb", cfg.rgbBright);

  AlarmBlob b;
  memset(&b, 0, sizeof(b));
  b.count = alarmCount;
  memcpy(b.a, alarms, sizeof(alarms));
  prefs.putBytes("alms", &b, sizeof(b));
  prefs.end();
}

// v4.2：一次性清掉旧版本遗留的 NVS 键（保留闹钟/外观/音量/报时等有效设置）。
// 不清也没功能影响，但设置页已经没有这些控件了，留着会让人误以为还有隐藏开关。
static void cleanupLegacyKeys() {
  static const char *oldKeys[] = {"ctone", "sron", "srm", "sls", "slmin"};
  prefs.begin("clock", false);
  for (const char *k : oldKeys) {
    if (prefs.isKey(k)) { prefs.remove(k); Serial.printf("[CFG] 清除旧键 %s\n", k); }
  }
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

// ============================================================
//  网页访问验证（v4.7.4）
//
//  问题：80 端口在局域网里对所有人开放，路由器 NAT 挡不住同网设备，
//  谁打开浏览器都能改闹钟、清 WiFi、让喇叭出声。加密码又回到"记密码"的老路。
//
//  做法：要求「物理在场」。打开网页先要一个 4 位验证码，这个码**只显示在
//  闹钟的数码管上**。能看见这台机器的人才读得到，看不见的人拿不到。
//  验证通过后码立即作废，会话存在内存里，页面关掉就失效。
//
//  代价（要先说清楚）：人在外面就没法远程改设置了。这是这个方案的必然结果，
//  不是 bug。
// ============================================================
#define AUTH_SESS_MAX   4                 // 同时最多几个会话（手机+电脑够用）
#define AUTH_PIN_MS     30000UL           // 验证码在屏幕上亮多久
#define AUTH_GET_COOL_MS 60000UL          // 两次「获取验证码」之间至少隔多久
#define AUTH_IDLE_MS    120000UL          // 页面这么久没发请求就当已经关了
#define AUTH_FAIL_MAX   5                 // 同一个码最多试错几次
#define AUTH_BLOCK_MS   3000UL            // 试错后冷却，防脚本枚举

static uint32_t authSessSeen[AUTH_SESS_MAX];
static char     authSessTok[AUTH_SESS_MAX][33];

static uint16_t      authPin    = 0;      // 当前验证码，0 = 没有
static unsigned long authPinMs  = 0;      // 生成时刻
static uint8_t       authFail   = 0;      // 这个码已经错了多少次
static unsigned long authLastTry = 0;     // 上次提交时刻（3 秒冷却用）
static unsigned long authLastGet = 0;     // 上次「获取验证码」时刻（60 秒冷却用）

// 码还有效吗（有效 = 屏幕正显示它）
static bool authPinFresh() {
  return authPin && (millis() - authPinMs) < AUTH_PIN_MS;
}

// 距离下次能「获取验证码」还剩多少毫秒
static unsigned long authGetCoolLeft() {
  if (!authLastGet) return 0;
  unsigned long d = millis() - authLastGet;
  return d < AUTH_GET_COOL_MS ? AUTH_GET_COOL_MS - d : 0;
}

// 生成新码。**只有用户主动点「获取验证码」才会走到这里**。
// 打开网页、未登录访问、提交过期码都不生成 —— 否则闹钟屏幕会莫名其妙跳数字，
// 这正是这个功能最招人烦的地方。
static void authNewPin(const char *why) {
  authPin    = (uint16_t)(1000 + esp_random() % 9000);   // 1000~9999，恒 4 位
  authPinMs  = millis();
  authLastGet = authPinMs;
  authFail   = 0;
  Serial.printf("[AUTH] 验证码 %04u（%s），屏幕亮 %lu 秒，%lu 秒后可再获取\n",
                (unsigned)authPin, why,
                (unsigned long)(AUTH_PIN_MS / 1000),
                (unsigned long)(AUTH_GET_COOL_MS / 1000));
}

// 从 Cookie 头里抠 sid。WebServer 不做 cookie 解析，自己找。
static const char *authCookieSid() {
  static char buf[33];
  String h = server.header("Cookie");
  int at = h.indexOf("sid=");
  if (at < 0) return nullptr;
  int i = 0;
  for (int p = at + 4; p < (int)h.length() && i < 32; p++) {
    char c = h[p];
    if (c == ';' || c == ' ') break;
    buf[i++] = c;
  }
  buf[i] = 0;
  return buf;
}

// 会话有效？顺便刷新"最后活跃时间"，页面开着就一直续期。
static bool authSessionValid() {
  const char *sid = authCookieSid();
  if (!sid || !*sid) return false;
  unsigned long now = millis();
  for (int i = 0; i < AUTH_SESS_MAX; i++) {
    if (!authSessTok[i][0]) continue;
    if (now - authSessSeen[i] > AUTH_IDLE_MS) {   // 过期就回收
      authSessTok[i][0] = 0;
      continue;
    }
    if (strcmp(authSessTok[i], sid) == 0) {
      authSessSeen[i] = now;
      return true;
    }
  }
  return false;
}

// 开一个新会话，把 token 写进 out[33]
static void authSessionNew(char *out) {
  unsigned long now = millis();
  int slot = -1;
  for (int i = 0; i < AUTH_SESS_MAX; i++) {
    if (!authSessTok[i][0] || now - authSessSeen[i] > AUTH_IDLE_MS) { slot = i; break; }
  }
  if (slot < 0) {                        // 满了就踢最久没动的
    slot = 0;
    for (int i = 1; i < AUTH_SESS_MAX; i++)
      if ((long)(authSessSeen[i] - authSessSeen[slot]) < 0) slot = i;
  }
  snprintf(authSessTok[slot], sizeof(authSessTok[slot]), "%08lx%08lx%08lx%08lx",
           (unsigned long)esp_random(), (unsigned long)esp_random(),
           (unsigned long)esp_random(), (unsigned long)esp_random());
  authSessSeen[slot] = now;
  strcpy(out, authSessTok[slot]);
  Serial.printf("[AUTH] 会话建立（槽 %d），单页 %lu 秒无请求即失效\n",
                slot, (unsigned long)(AUTH_IDLE_MS / 1000));
}

// 主动注销：找到这个 sid 的槽清掉
static void authSessionDrop() {
  const char *sid = authCookieSid();
  if (!sid || !*sid) return;
  for (int i = 0; i < AUTH_SESS_MAX; i++) {
    if (authSessTok[i][0] && strcmp(authSessTok[i], sid) == 0) {
      authSessTok[i][0] = 0;
      Serial.println(F("[AUTH] 会话已注销"));
      return;
    }
  }
}


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
//
// 【v4.7.8】① 同一帧连写两遍；② 每 3 秒无条件重写一次。
// TM1652 是**单向写**，写进去什么读不回来，一旦某一帧写歪了（WiFi/音频中断
// 插在字节之间，芯片会误判帧结束把后面的字节丢掉），错误会稳稳留在屏幕上，
// 直到下次有内容变化才会重写 —— 表现出来就是"某一位数字缺笔画"。
// 连写两遍把单帧损坏的概率压到可以忽略；3 秒重写兜住"帧内容没变但屏上是错的"
// 这种差量比较看不见的情况。两条都很便宜（合计约 0.3% 的总线时间）。
static uint32_t tmFrameWrites = 0;   // 真正上总线的帧数（连写两遍只记 1）
static uint32_t tmForceWrites = 0;   // 其中由 3 秒兜底触发的次数

void tmFlush(bool force = false) {
  static unsigned long lastForce = 0;
  bool byTimer = false;
  if (!force && millis() - lastForce >= 3000) { force = true; byTimer = true; }
  if (!force && memcmp(frame, shown, 6) == 0) return;
  lastForce = millis();
  if (byTimer) tmForceWrites++;
  tmFrameWrites++;
  // 连写两遍：这一帧要是被写歪了，没有任何办法发现（TM1652 只能写不能读）。
  // 连写两遍后「同一帧连着两次都歪」的概率可以忽略；代价约 3.5ms/次，
  // 正常一分钟才写完一次，占空比可以忽略。
  for (int pass = 0; pass < 2; pass++) {
    waitCmd();
    tmSend(TM1652_CMD_ADDRESS);
    for (int i = 0; i < 6; i++) tmSend(frame[i]);
    endCmd();
  }
  memcpy(shown, frame, 6);
}

// ============================================================
//  数码管段自检（v4.7.8）
// ------------------------------------------------------------
//  起因：照片里"小时十位"那一格显示成一个**固件根本写不出来**的图案。
//  固件在 digit0 只可能写两个值：SEG_NUM[0]=0x3F「0」、SEG_NUM[1]=0x06「1」。
//  照片实测是 acdf / adef（像"5"、像"C"），两者都不是。
//  所以只剩两种可能：
//    ① 那一帧在总线上被写歪了（中断插在字节之间，芯片误判帧结束）；
//    ② 那一位的 LED / 焊接有问题。
//  光看代码分不出来 —— 让屏幕自己把七段全点亮一次，眼睛一看就定了。
//
//  GET /api/segtest?p=127   四位全写 0x7F → 应该看到"8888"
//  GET /api/segtest?p=0     退出自检，立刻回时钟
//  可选 &hold=秒（默认 20，最多 600）：人要走到设备前面看一眼，20 秒太赶。
// ============================================================
static byte          segTestPat   = 0;
static unsigned long segTestUntil = 0;

void handleSegTest() {
  byte p = 0;
  if (server.hasArg("p")) p = (byte)(server.arg("p").toInt() & 0x7F);
  unsigned long hold = 20;
  if (server.hasArg("hold")) {
    hold = (unsigned long)server.arg("hold").toInt();
    if (hold < 5)   hold = 5;
    if (hold > 600) hold = 600;
  }
  segTestPat   = p;
  segTestUntil = p ? (millis() + hold * 1000UL) : 0;
  Serial.printf("[SEGTEST] 图案 0x%02X，保持 %lu 秒%s\n", (unsigned)p,
                p ? hold : 0UL,
                p ? "（屏幕应显示 4 个完整 8，缺哪段就是哪段有问题）" : "");
  char buf[96];
  snprintf(buf, sizeof(buf), "{\"p\":%u,\"hold_s\":%lu}", (unsigned)p,
           p ? hold : 0UL);
  server.send(200, "application/json; charset=utf-8", buf);
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

// 整点报时语音（v4.3）：每个整点是一整句，直接播 hour_N.wav，无需片段队列。
// 放文件顶部是因为 audioStop()（定义在较前）也要能清这个标志：
// 用户点「立即静音」/闹钟响起时，别让报时状态卡在「播报中」。
static bool chimeSpeaking = false;   // 整句报时是否正在播
static bool chimeSeenWav = false;    // 本段是否进过 AUD_WAV（区分「未投递」与「播完」）
static unsigned long chimePostMs = 0; // 本段投递时刻

bool resetHolding = false;        // 第二按键按住超过 5 秒 → 屏上开始倒数
unsigned long key2HoldMs = 0;     // 第二按键当前已按住多久（renderDisplay 用来算倒数）

// 第二按键长按阈值：5 秒开始倒数，8 秒真正执行
#define RESET_HOLD_WARN_MS  5000UL
#define RESET_HOLD_MS       8000UL

// 主按键（v4.8.2）：按下即静音；单击提亮；双击切 12/24；长按 8 秒进配网。
// 中按（0.8~3 秒）按用户要求取消，松手什么都不做。
#define KEY_CLICK_MS       800UL    // 短于这个才算「一下」，超过就不算单击
#define KEY_DBL_GAP_MS     500UL    // 两次「按下」之间的最大间隔（按下沿判定，不含按住时长）
#define KEY_AP_HOLD_MS     8000UL   // 长按进配网
#define KEY_AP_WARN_MS     3000UL   // 按住 3 秒后屏上开始倒数 5…1
bool          apHolding   = false;  // renderDisplay 用来画配网倒数
unsigned long apHoldMs    = 0;      // 本次按下时刻

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
// 时区钉死为北京时间（UTC+8）。
// POSIX 的 TZ 字符串里 "CST-8" 是「名称为 CST，比 UTC 快 8 小时」，
// 符号跟直觉相反（写成 CST+8 会得到 UTC-8），别改错。
// configTime() 内部也会设一次，但那要等到 startSntp()（联网之后）；
// 开机到那一刻之间任何 localtime_r 都会按 UTC 算，所以这里必须先设。
static void fixTimezone() {
  setenv("TZ", "CST-8", 1);
  tzset();
}

NowTime getNow() {
  NowTime n;
  n.h = n.m = n.s = 0;
  n.wd = 0;
  n.key = 0;
  n.valid = false;

  // 时间只认「本次开机校时成功之后」的值（v4.7.2）。
  // 复位（USB/软件/看门狗）不会清掉 RTC 里的 epoch，但 TZ 环境变量在 RAM 里、复位即丢，
  // 于是 localtime_r 按 UTC 算 —— 屏幕会在拿到 NTP 之前先显示"早 8 小时"的时间，
  // 用户看到的就是"开机几秒钟是另一个时区"。这段时间干脆什么都不显示，停在等待表情上。
  // 顺带避免了更糟的一种：残留时间刚好撞上闹钟时刻，开机就误响。
  //
  // 兜底：已经联网但 SNTP 一直没回（路由器没外网）时，30 秒后允许用残留值。
  // 否则屏幕会永远停在表情上，那比时间差几秒更难理解。
  static bool rtcFallback = false;
  time_t t = time(nullptr);
  if (lastSyncEpoch == 0) {
    if (!rtcFallback && millis() > 30000UL && t > 1700000000UL) {
      rtcFallback = true;
      Serial.println(F("[RTC] 30 秒未校时成功，改用复位残留时间"));
    }
    if (!rtcFallback) return n;
  }

  if (t > 1700000000UL) {                 // SNTP 已经拿到时间（或已启用残留兜底）
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

// 【v4.8.12】异步 WiFi 扫描缓存：热点一开就后台扫，/scan 直接读这里零等待。
// 定义在 startPortal() 之前，因为开热点时要调用 startPortalScanReset()。
#define SCAN_MAX       15
#define SCAN_RESCAN_MS 10000UL
struct ScanItem {
  char    ssid[33];
  uint8_t bars;
  uint8_t enc;      // 0 = 开放，1 = 需密码
};
static ScanItem      scanCache[SCAN_MAX];
static int           scanCount       = 0;
static bool          scanRunning     = false;
static unsigned long scanLastStartMs = 0;
static bool          scanEverDone    = false;   // 本次热点开启后是否扫过，保证首轮立即启动

void startPortalScanReset() {                   // 热点刚开时调用：作废旧结果，下一轮主循环立即扫
  scanCount = 0;
  scanRunning = false;
  scanEverDone = false;
  scanLastStartMs = 0;
}

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

// 把射频调到「满功率 + 不省电」。必须在 WiFi 已 start（mode 之后）调用，
// 否则返回 ESP_ERR_WIFI_NOT_INIT，设置不生效。
//
// 这里有两件事，缺一件都会表现为「信号差」，但原因完全不同：
//   1) 省电模式：WIFI_PS_MAX_MODEM 下射频会周期性入睡。症状是首次 TCP
//      建连慢、ping 400ms 级抖动、HTTP 偶发超时，而 RSSI 看着完全正常，
//      所以特别容易被误判成「发射功率不够」。小智项目里真正的根因就是这个。
//   2) 发射功率：C3 自制板当年因为发射链供电不足，曾限到 6dBm；S3 换成
//      XIAO 模组后已恢复 20dBm（硬件上限，单位是 1/4 dBm，故 ×4 = 80）。
static void applyRfPolicy(const char *when) {
  esp_err_t ep = esp_wifi_set_ps(WIFI_PS_NONE);
  esp_err_t e  = esp_wifi_set_max_tx_power(CFG_TX_DBM * 4);
  delay(30);

  int8_t dbm = -1;
  esp_wifi_get_max_tx_power(&dbm);
  wifi_ps_type_t ps = WIFI_PS_NONE;
  esp_wifi_get_ps(&ps);
  const char *psTxt = (ps == WIFI_PS_NONE) ? "关"
                    : (ps == WIFI_PS_MIN_MODEM) ? "min" : "max";
  Serial.printf("[RF] %s 发射 %.1fdBm(%s)  省电=%s(%s)\n",
                when, dbm / 4.0f, e == ESP_OK ? "ok" : "fail",
                psTxt, ep == ESP_OK ? "ok" : "fail");
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
  applyRfPolicy("开热点");

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
  startPortalScanReset();   // v4.8.12：热点开了，下一轮主循环立即异步扫 WiFi 列表

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
  applyRfPolicy("关热点");       // 切模式会重启射频，这里必须重新下发一次
  apActive = false;
  apCloseAtMs = 0;
  apCloseOnResult = false;
  Serial.printf("[AP] 热点已关闭（%s）\n", byConnect ? "配网成功" : "超时/手动");
}

// ------------------------------------------------------------
//  定向关联（v4.7.1）
//  实测「复位 → 连上 WiFi」里 3.2 秒全花在关联上：默认 WiFi.begin()
//  会把 2.4G 的 1~13 信道逐个扫描找同名 SSID。上次连上时的信道和
//  BSSID 存进 NVS，下次直接对准那一个 AP 连，跳过扫描。
//  风险是路由器换了信道 / Mesh 换了节点 —— 所以 4 秒没连上就退回
//  普通扫描，绝不能让"优化"变成"连不上"。
// ------------------------------------------------------------
static uint8_t apCh = 0;
static uint8_t apBssid[6] = {0};
static bool    fastAssoc = false;

static bool assocHintValid() {
  return apCh > 0 && (apBssid[0] | apBssid[1] | apBssid[2] |
                      apBssid[3] | apBssid[4] | apBssid[5]) != 0;
}

static void loadAssocHint() {
  prefs.begin("clock", true);
  apCh = prefs.getUChar("wch", 0);
  size_t n = prefs.getBytes("wbssid", apBssid, 6);
  prefs.end();
  if (n != 6) { apCh = 0; memset(apBssid, 0, 6); }
#ifdef ASSOC_HINT_FORCE_BAD       // 测试钩子：编译时加这个宏，就把信道故意指错，
                                  // 用来验证"定向关联失败 → 退回扫描"这条路真的能救回来
  if (apCh) { apCh = (apCh == 1) ? 6 : 1; }
#endif
  if (assocHintValid())
    Serial.printf("[NET] 关联提示 信道 %d  BSSID %02X:%02X:%02X:%02X:%02X:%02X\n",
                  (int)apCh, apBssid[0], apBssid[1], apBssid[2],
                  apBssid[3], apBssid[4], apBssid[5]);
}

static void saveAssocHint() {
  int ch = WiFi.channel();
  uint8_t *b = WiFi.BSSID();
  if (ch <= 0 || b == nullptr) return;
  if (ch == (int)apCh && memcmp(b, apBssid, 6) == 0) return;   // 没变就不写 NVS
  memcpy(apBssid, b, 6);
  apCh = (uint8_t)ch;
  prefs.begin("clock", false);
  prefs.putUChar("wch", apCh);
  prefs.putBytes("wbssid", apBssid, 6);
  prefs.end();
  Serial.printf("[NET] 关联提示已更新 信道 %d  BSSID %02X:%02X:%02X:%02X:%02X:%02X\n",
                (int)apCh, apBssid[0], apBssid[1], apBssid[2],
                apBssid[3], apBssid[4], apBssid[5]);
}

void beginConnect(const char *ssid, const char *pass) {
  apCloseOnResult = false;       // 新一轮配网，先撤掉上一轮的快速关闭
  apCloseAtMs = 0;
  strncpy(staSSID, ssid, 32); staSSID[32] = 0;
  strncpy(staPass, pass, 64); staPass[64] = 0;
  saveWifiCreds();

  // 开机正常联网时本来就是 STA 模式，再切一次 AP_STA 等于白白重启一次射频，
  // 只会拖慢关联。只有从配网热点过来的才需要 AP_STA。
  if (apActive) WiFi.mode(WIFI_AP_STA);
  else          WiFi.mode(WIFI_STA);
  delay(50);
  WiFi.setSleep(false);
  applyRfPolicy("配网连接");

  if (assocHintValid()) {
    fastAssoc = true;
    Serial.println(F("[NET] 定向关联（跳过全信道扫描）"));
    WiFi.begin(staSSID, staPass, (int32_t)apCh, apBssid, true);
  } else {
    fastAssoc = false;
    WiFi.begin(staSSID, staPass);
  }
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

// ============================================================
//  异步 WiFi 扫描（v4.8.12）—— 照小智 esp-wifi-connect 的思路
// ------------------------------------------------------------
//  旧版 /scan 里同步 WiFi.scanNetworks() 阻塞 2~4 秒，配网页一点"扫描"
//  就转圈，这是"加载慢"的元凶。现在改成：
//    ① 热点一开就用 scanNetworks(true) 异步扫描，不挡主循环；
//    ② 结果收进 scanCache，/scan 直接读缓存零等待；
//    ③ 配网期间每 10 秒静默重扫一次，列表自己保持最新（小智同款）。
//  扫描只在「热点开着 + STA 没连上 + 没在连接中」时进行，避免抢射频。
// ============================================================
void scanTask() {
  unsigned long now = millis();

  // 非配网状态（已连上/正在连接）不扫；若上一轮还挂着，清掉结果释放内存
  if (!apActive || connecting || WiFi.status() == WL_CONNECTED) {
    if (scanRunning) { WiFi.scanDelete(); scanRunning = false; }
    return;
  }

  if (scanRunning) {
    int16_t n = WiFi.scanComplete();
    if (n == WIFI_SCAN_RUNNING) return;              // 还在扫，等下一轮
    if (n < 0) {                                     // 失败，稍后再试
      scanRunning = false;
      scanLastStartMs = now;
      return;
    }
    int cnt = 0;
    for (int i = 0; i < n && cnt < SCAN_MAX; i++) {
      String s = WiFi.SSID(i);
      if (s.length() == 0) continue;
      int rssi = WiFi.RSSI(i);
      strncpy(scanCache[cnt].ssid, s.c_str(), sizeof(scanCache[cnt].ssid) - 1);
      scanCache[cnt].ssid[sizeof(scanCache[cnt].ssid) - 1] = 0;
      scanCache[cnt].bars = (rssi >= -55) ? 4 : (rssi >= -67) ? 3 : (rssi >= -75) ? 2 : 1;
      scanCache[cnt].enc  = (WiFi.encryptionType(i) == WIFI_AUTH_OPEN) ? 0 : 1;
      cnt++;
    }
    scanCount = cnt;
    WiFi.scanDelete();
    scanRunning = false;
    scanEverDone = true;
    scanLastStartMs = now;
    Serial.printf("[SCAN] 异步扫描完成 %d 个网络\n", cnt);
    return;
  }

  // 没在扫：首轮（scanEverDone=false）立即启动；之后每 10 秒静默重扫一次
  if (!scanEverDone || now - scanLastStartMs >= SCAN_RESCAN_MS) {
    int16_t r = WiFi.scanNetworks(true, false, false);   // async=true
    if (r == WIFI_SCAN_RUNNING || r >= 0) {
      scanRunning = true;
      scanLastStartMs = now;
      Serial.println(F("[SCAN] 启动异步扫描"));
    }
  }
}

void netTask() {
  unsigned long now = millis();

  scanTask();   // v4.8.12：异步扫描引擎，热点开着时常驻后台刷新列表

  if (connecting) {
    if (WiFi.status() == WL_CONNECTED) {
      connecting = false;
      lastFailCode = 0;
      Serial.printf("[NET] 已连接 %s  IP %s\n",
                    staSSID, WiFi.localIP().toString().c_str());
      // 连接耗时 + 信道/BSSID：3 秒多的连接时间里，如果信道跟上次不一样，
      // 说明每次都在做全信道扫描（可以用"上次的信道+BSSID 定向连接"省掉）
      Serial.printf("[NET] 关联耗时 %lums  信道 %d  RSSI %d  BSSID %s\n",
                    (unsigned long)(now - connectStartMs), (int)WiFi.channel(),
                    (int)WiFi.RSSI(), WiFi.BSSIDstr().c_str());
      fastAssoc = false;
      saveAssocHint();
      if (!bootToWifiMs) {          // 只在本次开机第一次连上时记一次
        bootToWifiMs = millis();
        Serial.printf("[BOOT] 复位→WiFi 连上 %lums\n", (unsigned long)bootToWifiMs);
      }
      // 连上之后再下发一次射频策略：某些核心/mode 切换会重置 PHY，
      // 这里兜底保证"满功率 + 不省电"在真正开始通信时是生效的。
      applyRfPolicy("已连接");
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
    // 定向关联没成 → 大概率是路由器换了信道 / Mesh 换了节点，退回全信道扫描。
    // 必须先把正在进行的连接停掉：STA 处于 connecting 状态时再调 WiFi.begin()，
    // 驱动会直接拒掉（日志 `wifi:sta is connecting, cannot set config`），
    // 结果就是"以为退回了扫描、其实什么都没发生"，只能干等 30 秒兜底重连。
    // 宁可慢 3 秒，也不能连不上。
    if (fastAssoc && now - connectStartMs > 6000) {
      fastAssoc = false;
      Serial.println(F("[NET] 定向关联未成，退回全信道扫描"));
      WiFi.disconnect(false, false);      // 只断 STA，不擦 NVS、不关射频
      delay(30);
      WiFi.begin(staSSID, staPass);
      connectStartMs = now;               // 20 秒超时重新计时
      lastFailCode = 0;
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
// ------------------------------------------------------------
//  「校时成功」必须以"真的收到 NTP 回包"为准（v4.7.2）。
//  原来 syncFromNtp() 用 `time(nullptr) > 1700000000` 当判据，这在复位后是错的：
//  复位不会清掉 RTC 里的 epoch，残留值天然满足条件 → 刚开机就被当成校时成功，
//  网页显示"最近校时 0 分钟前（网络时间）"，其实一次 NTP 都没交互过。
//  改用 SNTP 的同步回调置一个自己的标志，只有回包到了才算数。
//  （不能用 sntp_get_sync_status() 轮询：IDF 在置 COMPLETED 之后马上又复位成 RESET，
//   查到的概率很低。）
// ============================================================
static volatile bool sntpSynced = false;
static void onSntpSync(struct timeval *tv) {
  (void)tv;
  sntpSynced = true;
  // 从"发起"到"回包"的毫秒数，就是这段等待的全部成本（含 DNS + 一次来回）
  Serial.printf("[NTP] 收到回包 @%lums\n", (unsigned long)millis());
}

// 阿里 NTP 的固定 IP（2026-11-17 用 dig 查 ntp.aliyun.com 得到）。
// 用它当第一个服务器就是为了省 DNS：三个域名各解析一次，实测从"发起校时"
// 到"收到回包"要 3.4 秒，几乎全耗在解析上（一次 UDP 来回只有几十毫秒）。
// 后两个仍然填域名 —— 万一哪天这个 IP 换了机器，域名还能兜住，
// 只是慢回 3 秒，绝不会变成"校不上时间"。
#define NTP_IP_ALIYUN   "203.107.6.88"
#define NTP_HOST_FALLBACK1 "ntp.tencent.com"
#define NTP_HOST_FALLBACK2 "cn.pool.ntp.org"

void startSntp() {
  // 回调要在 configTime（内部 esp_sntp_init）之前注册
  esp_sntp_set_time_sync_notification_cb(onSntpSync);
  Serial.printf("[NTP] 发起校时 @%lums（%s + 2 个域名兜底）\n",
                (unsigned long)millis(), NTP_IP_ALIYUN);
  configTime(8 * 3600, 0, NTP_IP_ALIYUN, NTP_HOST_FALLBACK1, NTP_HOST_FALLBACK2);
  ntpStarted = true;
}

// 新板没有 DS1302：所谓「校时」就是确认 SNTP 已把系统时间推上来。
// 保留老接口语义（/sync 返回 ok/fail、开机 5s 一轮催校时、之后 6 小时一次）。
bool syncFromNtp() {
  if (!sntpSynced) return false;             // 本次开机还没收到过 NTP 回包
  time_t now = time(nullptr);
  if (now < 1700000000UL) return false;      // 回包到了但系统时间还没写进去

  bool first = (lastSyncEpoch == 0);
  lastSyncMs = millis();
  lastSyncEpoch = (unsigned long)now;
  if (first) {
    struct tm tmNow;
    localtime_r(&now, &tmNow);
    Serial.printf("[NTP] 校时成功 %04d-%02d-%02d %02d:%02d:%02d\n",
                  tmNow.tm_year + 1900, tmNow.tm_mon + 1, tmNow.tm_mday,
                  tmNow.tm_hour, tmNow.tm_min, tmNow.tm_sec);
    // 这才是用户真正能感知的"开机完成"：屏幕从表情变成走字的时刻
    bootMark("时钟就绪");
  }
  return true;
}

// ------------------------------------------------------------
//  开机第一次校时：自己发一个最朴素的 SNTP 请求
// ------------------------------------------------------------
//  IDF 的 SNTP 首次请求时机不受控：实测"发起→回包"在 0.4~4.3 秒之间乱跳
//  （同一台设备、同一个路由器，连测三次都不一样）。而同一时刻在电脑上问同一个
//  服务器只要 20~47ms —— 所以不是服务器慢，是板子侧首次发包的时间点问题，
//  而这个点在 IDF 内部（启动随机延迟 / 首次请求重传），sketch 里改不了。
//  那就别等它：自己用普通 UDP 直接问一次，收到就 settimeofday 落地。
//  每次最多等 150ms，没回就下一轮再试，不会长时间占住主循环；
//  6 小时一次的定期校时仍然交给 IDF SNTP（它有平滑处理，不需要我们管）。
static bool ntpQueryOnce(const char *ipStr, uint16_t waitMs) {
  IPAddress ip;
  if (!ip.fromString(ipStr)) return false;
  WiFiUDP udp;
  if (!udp.begin(0)) return false;
  uint8_t pkt[48];
  memset(pkt, 0, sizeof(pkt));
  pkt[0] = 0x1B;                       // LI=0 VN=3 Mode=3（client）
  udp.beginPacket(ip, 123);
  udp.write(pkt, sizeof(pkt));
  udp.endPacket();

  bool ok = false;
  uint32_t t0 = millis();
  while (millis() - t0 < waitMs) {
    if (udp.parsePacket() >= 48) {
      uint8_t r[48];
      if (udp.read(r, sizeof(r)) >= 48) {
        // 第 40~43 字节 = transmit timestamp 的整数秒（1900 纪元，大端）
        uint32_t secs = ((uint32_t)r[40] << 24) | ((uint32_t)r[41] << 16) |
                        ((uint32_t)r[42] << 8)  |  (uint32_t)r[43];
        if (secs > 2208988800UL) {           // 1900 纪元 → Unix 纪元
          struct timeval tv;
          tv.tv_sec  = (time_t)(secs - 2208988800UL);
          tv.tv_usec = 0;
          settimeofday(&tv, nullptr);
          ok = true;
        }
      }
      break;
    }
    delay(5);
  }
  udp.stop();
  return ok;
}

void ntpTask() {
  static unsigned long lastCheck = 0;
  unsigned long now = millis();

  // 还没校上时间时快轮询（200ms）：SNTP 一个来回通常 100~300ms 就回来了，
  // 原来固定 5 秒节流，会让"其实系统时间已经有效"白等几秒才被确认 ——
  // 这段等待用户看到的就是屏幕空着。校上之后回到 5 秒节流，不再多耗。
  unsigned long gap = (ntpStarted && lastSyncEpoch == 0) ? 200UL : 5000UL;
  if (lastCheck && now - lastCheck < gap) return;
  lastCheck = now;

  if (WiFi.status() != WL_CONNECTED) return;
  if (!ntpStarted) startSntp();

  // 首次校时走自建查询，通常几十毫秒就有结果（实测连上后 50~100ms）。
  // 次数上限 12 次：每次最多占 150ms 主循环，网络长时间不通时不能一直这么占着，
  // 否则刷屏会变卡 —— 用完了就交给 IDF 的 SNTP 自己慢慢校。
  static uint8_t selfTries = 0;
  if (!sntpSynced && selfTries < 12) {
    selfTries++;
    if (ntpQueryOnce(NTP_IP_ALIYUN, 150)) {
      sntpSynced = true;
      Serial.printf("[NTP] 自建查询拿到时间 @%lums（第 %u 次）\n",
                    (unsigned long)millis(), (unsigned)selfTries);
    }
  }

  // 本次开机还没校过，或者距上次超过 6 小时
  if (lastSyncEpoch == 0 || now - lastSyncMs >= SYNC_INTERVAL) syncFromNtp();
}

// ============================================================
//  音频：I2S → MAX98357（替代 C3 版 LEDC 方波 → PAM8403）
// ------------------------------------------------------------
//  MAX98357 是标准 I2S 从设备：BCLK=GPIO8、LRCK=GPIO9、DIN=GPIO7。
//  I2S_NUM_0 跑 44.1kHz / 16bit / 立体声，左右声道送同一份样本。
//  真正喂 DMA 的是独立的 FreeRTOS 任务：这样 /scan 这类阻塞 2~4 秒的
//  请求不会把声音掐断。
// ============================================================
// ============================================================
//  flash 语音（v4.3 起）
// ------------------------------------------------------------
//  0~23 点各一整句「现在时间 X 点整」在电脑上用 TTS 预生成
//  （voice/generate.py），作为 C 数组编进固件，直接从 flash 播放。
//  v4.5 起闹钟铃声同样走这条路（ringtone/generate.py）。
//  SPIFFS 只负责挂载与清理历史遗留文件，不再存音频。
// ============================================================
static bool fsReady = false;

struct VoiceAsset {
  const char   *path;
  const uint8_t *data;
  uint32_t      len;
};

// 索引 = 小时 0~23；到点直接播 VOICE_ASSETS[hour]，无需拼接
static const VoiceAsset VOICE_ASSETS[] = {
  {"/voice/hour_0.wav",  VOICE_H0,  VOICE_H0_LEN},
  {"/voice/hour_1.wav",  VOICE_H1,  VOICE_H1_LEN},
  {"/voice/hour_2.wav",  VOICE_H2,  VOICE_H2_LEN},
  {"/voice/hour_3.wav",  VOICE_H3,  VOICE_H3_LEN},
  {"/voice/hour_4.wav",  VOICE_H4,  VOICE_H4_LEN},
  {"/voice/hour_5.wav",  VOICE_H5,  VOICE_H5_LEN},
  {"/voice/hour_6.wav",  VOICE_H6,  VOICE_H6_LEN},
  {"/voice/hour_7.wav",  VOICE_H7,  VOICE_H7_LEN},
  {"/voice/hour_8.wav",  VOICE_H8,  VOICE_H8_LEN},
  {"/voice/hour_9.wav",  VOICE_H9,  VOICE_H9_LEN},
  {"/voice/hour_10.wav", VOICE_H10, VOICE_H10_LEN},
  {"/voice/hour_11.wav", VOICE_H11, VOICE_H11_LEN},
  {"/voice/hour_12.wav", VOICE_H12, VOICE_H12_LEN},
  {"/voice/hour_13.wav", VOICE_H13, VOICE_H13_LEN},
  {"/voice/hour_14.wav", VOICE_H14, VOICE_H14_LEN},
  {"/voice/hour_15.wav", VOICE_H15, VOICE_H15_LEN},
  {"/voice/hour_16.wav", VOICE_H16, VOICE_H16_LEN},
  {"/voice/hour_17.wav", VOICE_H17, VOICE_H17_LEN},
  {"/voice/hour_18.wav", VOICE_H18, VOICE_H18_LEN},
  {"/voice/hour_19.wav", VOICE_H19, VOICE_H19_LEN},
  {"/voice/hour_20.wav", VOICE_H20, VOICE_H20_LEN},
  {"/voice/hour_21.wav", VOICE_H21, VOICE_H21_LEN},
  {"/voice/hour_22.wav", VOICE_H22, VOICE_H22_LEN},
  {"/voice/hour_23.wav", VOICE_H23, VOICE_H23_LEN},
};
#define VOICE_ASSET_COUNT (sizeof(VOICE_ASSETS) / sizeof(VOICE_ASSETS[0]))

bool fsInit() {
  if (!SPIFFS.begin(true)) {          // true = 挂载失败时自动格式化（首次开机）
    Serial.println(F("[FS] SPIFFS 挂载失败（分区表里没有 spiffs 分区？）"));
    return false;
  }
  fsReady = true;

  // v4.3：语音整句直接编在 app 的 C 数组里、从 flash 播放，不再写入 SPIFFS。
  // 清掉 v4.1 / v4.2 遗留的铃声、碎片，以及 v4.3 早期可能写了一半的 hour_*.wav。
  static const char *legacy[] = {
    "/rings/sunrise.wav", "/rings/rain.wav", "/rings/ocean.wav",
    "/rings/custom.wav",  "/rings/upload.tmp",
    "/voice/prefix.wav", "/voice/ten.wav", "/voice/oclock.wav",
    "/voice/0.wav", "/voice/1.wav", "/voice/2.wav", "/voice/3.wav",
    "/voice/4.wav", "/voice/5.wav", "/voice/6.wav", "/voice/7.wav",
    "/voice/8.wav", "/voice/9.wav",
    "/voice/hour_0.wav",  "/voice/hour_1.wav",  "/voice/hour_2.wav",
    "/voice/hour_3.wav",  "/voice/hour_4.wav",  "/voice/hour_5.wav",
    "/voice/hour_6.wav",  "/voice/hour_7.wav",  "/voice/hour_8.wav",
    "/voice/hour_9.wav",  "/voice/hour_10.wav", "/voice/hour_11.wav",
    "/voice/hour_12.wav", "/voice/hour_13.wav", "/voice/hour_14.wav",
    "/voice/hour_15.wav", "/voice/hour_16.wav", "/voice/hour_17.wav",
    "/voice/hour_18.wav", "/voice/hour_19.wav", "/voice/hour_20.wav",
    "/voice/hour_21.wav", "/voice/hour_22.wav", "/voice/hour_23.wav"
  };
  int removed = 0;
  for (const char *q : legacy) {
    if (SPIFFS.exists(q)) { SPIFFS.remove(q); removed++; }
  }
  SPIFFS.rmdir("/rings");
  SPIFFS.rmdir("/voice");

  // v4.7.1：清掉 v4.9 实验版留下的语音样本（/kws/*.wav）。
  // 语音问答已整体撤掉，这些文件不但白占 1.5MB，而且**文件越多挂载越慢**
  // （实测 24 条 → 挂载 5.6 秒，清空后回到百毫秒级）。
  // 注意：必须"先列全再逐个删"——边遍历边删会让 SPIFFS 目录迭代器失效，
  // 表现是删一半就停了（v4.9 踩过这个坑）。
  static char kwsNames[40][32];
  int kwsN = 0;
  {
    File kdir = SPIFFS.open("/kws");
    if (kdir && kdir.isDirectory()) {
      File f = kdir.openNextFile();
      while (f && kwsN < 40) {
        if (!f.isDirectory()) {
          const char *full = f.name();                 // 核心 3.x 给的是全路径
          const char *base = strrchr(full, '/');
          snprintf(kwsNames[kwsN], sizeof(kwsNames[0]), "%s", base ? base + 1 : full);
          if (kwsNames[kwsN][0]) kwsN++;
        }
        f = kdir.openNextFile();
      }
      kdir.close();
    }
  }
  for (int i = 0; i < kwsN; i++) {
    String p = String("/kws/") + kwsNames[i];
    if (SPIFFS.remove(p)) removed++;
  }
  if (kwsN) SPIFFS.rmdir("/kws");

  Serial.printf("[FS] 清理旧语音文件 %d 个；24 段整句直接从 flash 播放，不占 SPIFFS\n", removed);
  return true;
}

// fsInit 为什么不在 setup() 里跑：
//   SPIFFS.begin() 实测要 **5.6 秒**（3.24MB 分区里有东西时更久），而 v4.7
//   的功能一个都不用它——铃声和报时全部从 flash 里的 C 数组播放。它唯一的作用
//   是清理老版本遗留的文件。放在 setup() 里等于让每次开机白等 5.6 秒，而本板
//   没有 RTC，这段时间屏幕是空的，用户感知非常明显（实测启动到联网 9.5 秒，
//   其中 5.6 秒卡在这里）。
//   改成联网之后再挂载：启动到能用从 9.5s 降到约 3.9s，清理照常做。
static bool fsPending = true;        // 还没跑过 fsInit
static bool fsDone    = false;       // 跑完了（成功或失败都算，不重试）
static bool fsSkipped = false;       // 本次开机因为"以前清过"而完全没挂载

// NVS 里记一个"清理已做过"的标记。
// 为什么需要它：SPIFFS 空分区挂载实测也要 **5.6 秒**（3.24MB / 256B 页 = 12672 页，
// 挂载要逐页扫），跟文件多少无关。v4.7 又不使用文件系统，为了一次性的清理让
// 每次开机都付这 5.6 秒毫无意义 —— 而且这 5.6 秒是**阻塞 loop** 的，表现为
// 开机后几秒里时钟停住不动。
static bool fsCleanFlagged() {
  prefs.begin("clock", true);
  bool done = prefs.getBool("fsclean", false);
  prefs.end();
  return done;
}
static void fsFlagClean() {
  prefs.begin("clock", false);
  prefs.putBool("fsclean", true);
  prefs.end();
}

static void fsDeferredTask() {
  if (!fsPending || fsDone) return;
  // 等 Wi-Fi 连上再干；12 秒兜底，避免没配网时永远不清理
  if (WiFi.status() != WL_CONNECTED && millis() < 12000) return;
  fsPending = false;
  fsDone = true;

  if (fsCleanFlagged()) {
    fsSkipped = true;
    Serial.println(F("[FS] 历史文件已清理过，本次跳过挂载（省 5.6 秒）"));
    return;
  }
  uint32_t t0 = micros();
  bool ok = fsInit();
  uint32_t dt = (uint32_t)((micros() - t0) / 1000);
  if (ok) fsFlagClean();          // 清完就记账，之后不再挂载
  Serial.printf("[FS] 挂载%s  耗时 %lums  占用 %u/%u 字节\n",
                ok ? "成功" : "失败", (unsigned long)dt,
                (unsigned)SPIFFS.usedBytes(), (unsigned)SPIFFS.totalBytes());
}

// ============================================================
//  音频引擎：I2S → MAX98357
// ------------------------------------------------------------
//  同一个 DMA 供给任务 i2sFeedTask 支持两套音源：
//    (方波)    v4.5 起已无调用者，toneFill 只负责输出静音
//    AUD_WAV   16bit PCM：flash 里的 C 数组（报时/铃声）或 SPIFFS 文件，
//              线性插值重采样到 44.1kHz，可循环
//  切换只走 audReq 请求队列：File/SPIFFS 操作**只在音频任务里**发生，
//  HTTP handler 只置标志，避免 core0/core1 同时操作同一个文件系统。
// ============================================================
#define I2S_SPK_RATE   44100
#define I2S_FRAMES     256                  // 每块 256 立体声帧 ≈ 5.8ms
#define I2S_RAMP_SEC   0.006f               // 音头/收尾 6ms 淡变

enum { AUD_IDLE = 0, AUD_TONE = 1, AUD_WAV = 2 };enum { AUDREQ_NONE = 0, AUDREQ_WAV = 1, AUDREQ_STOP = 2, AUDREQ_WAV_MEM = 3 };

static volatile uint32_t toneFreq = 0;      // 当前频率（Hz）—— 已无方波音源，留作收尾
static volatile int16_t  toneAmp  = 0;      // 目标幅度（int16 峰值）
static volatile bool     spkReady = false;

static int16_t i2sBuf[I2S_FRAMES * 2];     // 交错 L/R，静态分配不占任务栈

// ---- WAV 播放状态（只有 i2sFeedTask 会写；类型定义在文件顶部） ----
static WavState wav;

// 请求投递用临界区，保证另一核不会看到「新标志 + 旧路径」
static portMUX_TYPE audMux = portMUX_INITIALIZER_UNLOCKED;
static volatile uint8_t  audReq      = AUDREQ_NONE;
static volatile bool     audReqLoop  = false;
static char              audReqPath[48] = {0};
// v4.3：内存 WAV 请求带的数据指针与长度（指向 flash 里的 const 数组，稳定）
static const uint8_t    *audReqMem   = nullptr;
static volatile uint32_t audReqMemLen = 0;

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
      if (w.fromMem) w.memOff = w.dataOff;
      else           w.f.seek(w.dataOff);
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
        if (w.fromMem) {
          // 从 flash 里的 C 数组读：剩余不足则只读剩余，且必须偶数（按 16bit 样本）
          if (w.memOff + want > w.memSize) want = w.memSize - w.memOff;
          want &= ~1u;
          if (want < 2) { w.rbLen = 0; }
          else { memcpy(w.rb, w.mem + w.memOff, want); w.memOff += want; w.rbLen = want; }
        } else {
          w.rbLen = w.f.read(w.rb, want);
        }
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

// v4.3：从 flash 里的 C 数组（整句语音）打开 WAV，逻辑同 wavOpen，只是数据源是内存。
// 解析一个 RIFF 头：data 块偏移 / 采样率 / 声道 / 位深。生成脚本保证标准 44 字节头。
static bool wavOpenMem(WavState &w, const uint8_t *data, uint32_t len, bool loop) {
  if (w.f) w.f.close();
  if (!data || len < WAV_HDR ||
      memcmp(data, "RIFF", 4) || memcmp(data + 8, "WAVE", 4)) return false;
  // 扫 RIFF chunks 找 fmt / data（兼容非 44 字节头）
  uint16_t channels = 0, bits = 0, align = 0, format = 0;
  uint32_t rate = 0, dataSize = 0, dataOff = 0;
  uint32_t p = 12;
  while (p + 8 <= len) {
    const uint8_t *cb = data + p;
    uint32_t sz = (uint32_t)cb[4] | ((uint32_t)cb[5] << 8) |
                  ((uint32_t)cb[6] << 16) | ((uint32_t)cb[7] << 24);
    if (!memcmp(cb, "fmt ", 4) && sz >= 16 && p + 8 + 16 <= len) {
      const uint8_t *fm = cb + 8;
      format   = (uint16_t)(fm[0] | (fm[1] << 8));
      channels = (uint16_t)(fm[2] | (fm[3] << 8));
      rate     = (uint32_t)fm[4] | ((uint32_t)fm[5] << 8) |
                 ((uint32_t)fm[6] << 16) | ((uint32_t)fm[7] << 24);
      align    = (uint16_t)(fm[12] | (fm[13] << 8));
      bits     = (uint16_t)(fm[14] | (fm[15] << 8));
    } else if (!memcmp(cb, "data", 4)) {
      dataOff = p + 8;
      dataSize = sz;
      if (dataOff + dataSize > len) dataSize = len - dataOff;
      break;
    }
    if (sz > len - p - 8) break;
    p += 8 + sz + (sz & 1);
  }
  if (!dataOff || !dataSize || format != 1 || bits != 16 ||
      (channels != 1 && channels != 2) || rate < 8000 || rate > 48000 ||
      align < channels * 2) return false;

  w.fromMem = true;
  w.mem = data;
  w.memSize = len;
  w.memOff = dataOff;
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
  int16_t first;
  if (!wavNext(w, &first)) return false;
  w.cur = first;
  if (!wavNext(w, &w.next)) w.next = 0;
  return true;
}

static void wavClose(WavState &w) {
  if (w.f) w.f.close();
  w.fromMem = false;
  w.mem = nullptr;
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

// v4.3：投递 flash 内存里的 WAV（报时整句 / 闹钟铃声）
static void audPostMem(const uint8_t *data, uint32_t len, const char *label,
                       bool loop = false) {
  taskENTER_CRITICAL(&audMux);
  audReqMem = data;
  audReqMemLen = len;
  audReqLoop = loop;
  strncpy(audReqPath, label ? label : "/flash", sizeof(audReqPath) - 1);
  audReqPath[sizeof(audReqPath) - 1] = 0;
  audReq = AUDREQ_WAV_MEM;
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
  const uint8_t *memPtr = nullptr;
  uint32_t memLen = 0;
  for (;;) {
    uint8_t req;
    taskENTER_CRITICAL(&audMux);
    req = audReq;
    if (req == AUDREQ_WAV || req == AUDREQ_WAV_MEM) {
      strncpy(path, audReqPath, sizeof(path) - 1);
      path[sizeof(path) - 1] = 0;
      loopReq = audReqLoop;
    }
    if (req == AUDREQ_WAV_MEM) {
      memPtr = audReqMem;
      memLen = audReqMemLen;
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
    } else if (req == AUDREQ_WAV_MEM) {
      if (wavOpenMem(wav, memPtr, memLen, loopReq)) {
        audMode       = AUD_WAV;
        audWavPos     = 0;
        audWavFrames  = wav.frameLeft;
        audWavRate    = wav.rate;
        strncpy(audWavPath, path, sizeof(audWavPath) - 1);
        audWavPath[sizeof(audWavPath) - 1] = 0;
        Serial.printf("[WAV] 播放(flash) %s  %uHz/%uch  %u 帧  %s\n", path,
                      (unsigned)wav.rate, (unsigned)wav.channels,
                      (unsigned)audWavFrames, loopReq ? "循环" : "一次");
      } else {
        audMode = AUD_IDLE;
        Serial.printf("[WAV] flash 语音打开失败 %s\n", path);
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

// ---- 闹钟铃声：flash 里的 WAV 资源表 ----
struct RingAsset { const uint8_t *data; uint32_t len; };
static const RingAsset RINGS[TONE_COUNT] = {
  { RING_0, RING_0_LEN },
  { RING_1, RING_1_LEN },
  { RING_2, RING_2_LEN },
  { RING_3, RING_3_LEN },
  { RING_4, RING_4_LEN },
  { RING_5, RING_5_LEN },
};

// 播放 flash C 数组里的 WAV —— 整点报时语音和闹钟铃声都走这里。
// loop=true 时播到末尾自动绕回开头：闹钟要一直响到用户按键停。
bool playWavMem(const uint8_t *data, uint32_t len, const char *label, bool loop) {
  if (!data || len < WAV_HDR) return false;
  // 切到 WAV 前必须把方波目标幅度清零：否则 WAV 播完切回 AUD_IDLE 后，
  // toneFill() 会把残留的 toneAmp 继续输出，冒出一段莫名其妙的方波。
  toneAmp = 0;
  audPostMem(data, len, label, loop);
  return true;
}

// 闹钟铃声：直接放 flash 里的真实 WAV（8 秒无缝循环），不再用方波现算。
void playTone(uint8_t idx, bool loop) {
  if (idx >= TONE_COUNT) idx = TONE_DEFAULT;
  playWavMem(RINGS[idx].data, RINGS[idx].len, RING_NAMES[idx], loop);
}

// 幅度渐变由 i2sFeedTask 按 6ms 时间常数完成，主循环只改目标值。
// 现在没有方波音源了，这套只留给 audioStop() 把残留幅度收干净。
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
  chimeSpeaking = false;            // 停声即清报时状态，避免试听/静音后状态卡住
  audPost(AUDREQ_STOP);             // WAV 淡出并停下
}

// chimeTask 用：WAV 播放期间算「正在出声」，别让整点报时打断试听
bool audioActive() { return audMode == AUD_WAV; }


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
// v4.4 修复：按「被报的整点」判断是否报时，且起止两端都包含。
// 关键：报「X 点整」是在 (X-1):59:55 触发的，所以必须拿 nextHour=X 去判断，
// 不能用触发时刻 t.h*60+t.m（那会早一小时，导致 7:00-22:00 的 7 点不报）。
// 时段是整点（网页下拉只给 HH:00，/save 也把分钟归零），所以除以 60 取小时即可。
// 闭区间：7:00-22:00 → 7、8、…、22 点都报（含 7 和 22）；跨零点同理含两端。
bool shouldChimeHour(int hour) {
  int s = cfg.chimeStart / 60;
  int e = cfg.chimeEnd / 60;
  if (s == e) return true;                  // 全天（默认 00:00→00:00）
  if (s < e)  return hour >= s && hour <= e;
  return hour >= s || hour <= e;            // 跨零点：22:00→07:00 报 22,23,0..7
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
//  新外设：两颗 WS2812B（GPIO10，RMT 单帧推 48bit）
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
//  氛围灯场景（v4.2：只保留闹铃红闪 / 配网蓝闪 / 平时暖白夜灯）
// ============================================================
void wsTask() {
  if (!wsReady) return;
  static unsigned long last = 0;
  if (millis() - last < 100) return;            // 10Hz 足够，省 RMT
  last = millis();

  // 闹铃优先级最高，而且**不受 cfg.rgbOn 约束**（v4.8.1，用户明确要求）：
  // 「不管氛围灯开关开还是关，闹钟响都要闪」。这条线是给"看得见它在响"用的，
  // 不能被日常夜灯开关连坐 —— 关灯是不想夜里屋里有光，不等于响铃时也不想被提醒。
  static bool wsAlarmLit = false;
  if (alarmRinging) {
    if (!wsAlarmLit) {
      wsAlarmLit = true;
      Serial.printf("[RGB] 闹铃闪烁开始（忽略氛围灯开关，rgbOn=%d）\n", cfg.rgbOn);
    }
    // 2Hz 明灭：250ms 一个半拍、暗态全灭。
    // 原来 1 秒一个周期、暗态还给 40，隔着透光壳几乎看不出在闪。
    uint8_t v = ((millis() / 250) & 1) ? 0 : 255;
    wsSend(v, 0, 0, v, 0, 0);
    return;
  }
  if (wsAlarmLit) { wsAlarmLit = false; Serial.println(F("[RGB] 闹铃闪烁结束，回氛围灯设置")); }

  if (!cfg.rgbOn) { wsSend(0, 0, 0, 0, 0, 0); return; }

  bool blink = ((millis() / 500) & 1) == 0;
  uint8_t r = 0, g = 0, b = 0;
  if (apActive && WiFi.status() != WL_CONNECTED) {   // 配网：蓝色慢闪
    b = blink ? 160 : 0;
  } else {                                      // 平时：暖白夜灯
    const NowTime &t = nowFast();
    int level = (int)cfg.rgbBright * 22;        // 1~10 → 22~220
    if (millis() < boostUntilMs) level = 220;   // 短按临时点亮
    else if (cfg.nightOn && t.valid && inNightWindow(t.h * 60 + t.m))
      level = level * cfg.nightBright / 7;      // 夜间跟着夜灯档位压暗
    if (level > 255) level = 255;
    r = (uint8_t)level;
    g = (uint8_t)(level * 0.62f);
    b = (uint8_t)(level * 0.28f);
  }
  wsSend(r, g, b, r, g, b);
}

// ============================================================
//  闹钟 / 整点报时
// ============================================================
void startAlarm(int idx) {
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
    Serial.printf("[ALARM] 贪睡到点 i=%d\n", snoozeIdx);
    startAlarm(snoozeIdx);
    return;
  }

  const NowTime &t = nowFast();
  if (!t.valid) return;

  for (int i = 0; i < alarmCount; i++) {
    if (!alarmDue(i, t)) continue;
    // 同一时刻的多组只响一次，别轮着响
    for (int j = 0; j < alarmCount; j++) {
      if (alarms[j].hour == alarms[i].hour && alarms[j].min == alarms[i].min)
        lastAlarmKey[j] = t.key;
    }
    Serial.printf("[ALARM] 到点 i=%d %02d:%02d key=%u\n",
                  i, alarms[i].hour, alarms[i].min, (unsigned)t.key);
    startAlarm(i);
    return;
  }
}

// ---- 整点报时语音（v4.3）：每整点一整句，直接从 flash C 数组播放 ----
// v4.2 是 13 段碎片在设备端拼接，段间停顿明显、语调割裂。v4.3 每个整点
// 预生成一整句编进 app，播放只需一次 audPostMem，从根上消除拼接生硬。
// 24 段共 1.8MB > SPIFFS 1.43MB，所以不复制到 SPIFFS，直接读 flash。
static void chimeSpeakHour(int hour) {
  if (hour < 0) hour = 0;
  if (hour > 23) hour = 23;
  const VoiceAsset &a = VOICE_ASSETS[hour];
  chimeSpeaking = true;
  chimeSeenWav = false;
  chimePostMs = millis();
  playWavMem(a.data, a.len, a.path, false);
  Serial.printf("[CHIME] 播报整句 %s\n", a.path);
}

// 派发播报状态：整句播完（曾进 WAV、现在退出）就清掉标志。
// playWav 后到 wavOpen 之间 audMode 还是 IDLE，所以用 chimeSeenWav
// 区分「还没起来」和「已播完」；连续试听时 chimeSpeakHour 会重置这些状态。
static void chimePump() {
  if (!chimeSpeaking) return;
  if (audMode == AUD_WAV) {
    chimeSeenWav = true;
    return;
  }
  if (!chimeSeenWav) {
    if (millis() - chimePostMs < 200UL) return;  // 还没被音频任务处理
    // 200ms 还没进 WAV = 打开失败，放弃
    chimeSpeaking = false;
    Serial.println(F("[CHIME] 打开失败，跳过"));
    return;
  }
  // 曾进过 WAV、现在退出 = 播完了（WAV 引擎已有 6ms 淡出，不必再消抖）
  chimeSpeaking = false;
  Serial.println(F("[CHIME] 整句播完"));
}

void chimeTask() {
  chimePump();                                   // 先更新播报状态
  if (!cfg.chime || alarmRinging || audioActive()) return;

  const NowTime &t = nowFast();
  if (!t.valid) return;

  // 整点前 5 秒（59:55）开始，整句约 2~3 秒播完正好压在整点上；报「下一个整点」。
  // lastChimeKey 是分钟键，55~59 秒这几轮里只会真正触发一次。
  if (t.m == 59 && t.s >= 55 && t.key != lastChimeKey) {
    int nextHour = (t.h + 1) % 24;
    // 用「被报的整点 nextHour」判断时段（不是触发时刻的小时），起止两端都包含。
    // 这样 7:00-22:00 会如实播报 7 点和 22 点整句。
    if (!shouldChimeHour(nextHour)) return;
    lastChimeKey = t.key;
    chimeSpeakHour(nextHour);
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

  // 段自检优先级最高：直接铺一个已知图案，用来看"某一段到底能不能亮"。
  // 这里用普通的 tmFlush()（不 force）：图案不变时后几次调用会跳过，
  // 只有第一帧真的上总线。万一这一帧写歪了，3 秒强制重写会兜住。
  if (segTestPat) {
    if (now < segTestUntil) {
      dispClear();
      for (int i = 0; i < DIGIT_COUNT; i++) dispDigit(i, segTestPat);
      dispIcons(0);
      tmFlush();
      return;
    }
    segTestPat = 0;
    Serial.println(F("[SEGTEST] 自检结束，回正常显示"));
  }

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

  // 主按键按住 3 秒后倒数 5→4→3→2→1，8 秒到才进配网。
  // 和第二按键恢复出厂用同一套显示办法：回答"还要按多久"，
  // 免得按到 7.x 秒以为没反应就松手（之前 3 秒静默进配网就有这个投诉）。
  if (apHolding) {
    dispClear();
    int left = (int)((KEY_AP_HOLD_MS - (millis() - apHoldMs) + 999) / 1000);
    if (left < 1) left = 1;
    if (left > 9) left = 9;
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

  // 验证码优先级最高（除了响铃时的图标）：
  // 有人打开网页 → 屏幕跳成 4 位数字 → 关掉冒号，一眼就能和时间区分开。
  // 30 秒没人用就自己消失，屏幕回时钟，什么也不用按。
  //
  // 下面两行是给"屏幕上到底有没有亮过"留证据的：不打印就没法从串口判断，
  // 只能靠人盯着屏幕看（每次调这块都得站起来一次，太费劲）。
  static uint16_t pinOnScreen = 0;
  bool pinUp = authPinFresh();
  if (pinUp && pinOnScreen != authPin) {
    pinOnScreen = authPin;
    Serial.printf("[DISP] 屏幕显示验证码 %04u（%lu 秒后自动回时钟）\n",
                  (unsigned)authPin, (unsigned long)(AUTH_PIN_MS / 1000));
  } else if (!pinUp && pinOnScreen) {
    pinOnScreen = 0;
    Serial.println(F("[DISP] 验证码结束，屏幕回时钟"));
  }

  if (pinUp) {
    dispDigit(0, SEG_NUM[(authPin / 1000) % 10]);
    dispDigit(1, SEG_NUM[(authPin / 100) % 10]);
    dispDigit(2, SEG_NUM[(authPin / 10) % 10]);
    dispDigit(3, SEG_NUM[authPin % 10]);
    dispColon(false);

    byte ic = 0;
    if (alarmRinging)                          ic |= ICON_BELL;
    else if (anyAlarmEnabled())                ic |= ICON_ALARM;
    if (WiFi.status() == WL_CONNECTED)         ic |= ICON_WIFI;
    dispIcons(ic);
    tmFlush();
    return;
  }

  // 各打一次，开机日志里能直接看到"表情从哪一刻起、时间从哪一刻起"，
  // 不用靠推理 —— 排查"开机先显示一个错时间"这类问题全靠这两行。
  static bool loggedWait = false, loggedTime = false;

  if (!t.valid) {
    // 没校时之前一律停在等待表情上（复位残留的 RTC 值不显示，见 getNow()）
    if (!loggedWait) {
      loggedWait = true;
      Serial.printf("[DISP] 显示等待表情 @%lums（本次开机还没校时）\n",
                    (unsigned long)millis());
    }
    drawWifiWaitFrame(blink);
  } else {
    if (!loggedTime) {
      loggedTime = true;
      Serial.printf("[DISP] 开始显示时间 @%lums\n", (unsigned long)millis());
    }
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
// v4.8.10：按键诊断日志开关，默认关闭。
// 115200 波特率下一行中文日志约 60~80 字节（每字节 87µs），双击一次 5 行 ≈ 20ms，
// 而且全压在按键边沿判定路径上 —— 松手后要等串口吐完才提亮，手感就是"发涩"。
// 需要排查按键时序时把 KEY_DIAG 改成 1 重新编译即可，功能代码一行不动。
#define KEY_DIAG  0

static unsigned long keyChangeMs = 0, keyDownMs = 0;
static bool keyLongFired = false;
static bool keyArmed = false;   // 开机时若按键已按下，必须先松开一次才生效

// v4.8.0：主按键「按下」的瞬间就把所有声音掐掉，不等抬手、也不管按多久。
// 用户明确要求覆盖三种出声场景：
//   ① 闹钟到点响铃      → stopAlarm(true)：停铃 + 按设置排贪睡
//   ② 网页「试听铃声」  → 只是试听，不能顺手排出一次贪睡
//   ③ 网页「试听报时」  → 同上；audioStop() 会清 chimeSpeaking，
//                          否则 chimePump 会在 200ms 后误报「打开失败」
// 放在按下沿而不是短按释放沿，是因为「立即静音」的语义是手一碰就静，
// 而且长按进配网、双击切 12/24 小时的时候也要先把铃声掐掉。
static bool keySilenced = false;   // 本次按下是否已经掐过声音

bool keySilence() {
  if (alarmRinging) {
    stopAlarm(true);
    #if KEY_DIAG
    Serial.println(F("[KEY] 响铃中按下 → 停铃"));
    #endif
    return true;
  }
  if (audioActive() || chimeSpeaking) {
    audioStop();
    #if KEY_DIAG
    Serial.println(F("[KEY] 放音中按下 → 静音"));
    #endif
    return true;
  }
  return false;
}

// 【v4.8.11 双击修复】
// 旧版在「松开沿」用 second.release - first.release 判双击，这中间混进了第二下
// 自己的按住时长（实测人双击第二下常按 100~200ms）。450ms 窗口一扣就没了，
// 于是本该是双击的操作被拆成两次单击 → 用户看到的就是「双击没生效」。
// 新版改成「按下沿」用 second.press - first.press 判，与按住时长彻底无关；
// 单击也就不用再挂起等窗口了，松手立即提亮，手感更跟手。
static unsigned long lastPressMs    = 0;
static bool          lastPressValid = false;

void onSingleClick() {
  // 静音的那一下不再顺带提亮：保持「按铃即停」原有的手感，别让屏幕跟着炸亮
  if (keySilenced) return;
  boostUntilMs = millis() + BOOST_MS;           // 夜里看一眼时间
}

void onDoubleClick() {
  cfg.mode24 = !cfg.mode24;
  saveCfg();
  // 双击改的是设备全局状态，网页那个开关靠 /api/status 里的 m24 跟着走。
  // 双击一天也就按几次，这行短日志留着当"到底生效没有"的硬证据，开销可忽略。
  Serial.printf("[KEY] 双击生效 → %u 小时制\n", cfg.mode24 ? 24 : 12);
}

// 【v4.8.11 长按卡顿修复】
// startPortal() 里有 WiFi.mode() 切换 + delay(50/20/300) + WiFi.softAP() +
// dnsServer.start()，整套下来会把主循环冻住几百毫秒。旧版直接在 keyTask() 里
// 同步调用它：8 秒一到就卡死，屏上倒数停住、按键失灵，用户体感就是"长按卡顿"。
// 现在只置一个标志，真正的开热点挪到 loop() 里做，按键任务立刻返回。
static bool portalPending = false;

void onLongPress() {
  portalPending = true;
  Serial.println(F("[KEY] 长按 8 秒到 → 排队开配网热点"));
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
  if (now - keyChangeMs < 25) { apHolding = false; return; }   // 去抖

  if (keyStable == HIGH && now > 1000) keyArmed = true;

  if (raw != keyStable) {
    keyStable = raw;
    if (keyStable == LOW) {
      keyDownMs = now;
      apHoldMs  = now;
      apHolding = false;
      keyLongFired = false;
      keySilenced = keySilence();       // v4.8.0：按下即静音，先于任何时长判定
      // 【v4.8.11】双击在「按下沿」判定：只比两次按下的时刻，与按住多久无关。
      // 第二下按得再实、再慢都不会把窗口吃掉。用完立刻作废，
      // 避免第三下又和第二次凑成一次新双击。
      if (lastPressValid && (now - lastPressMs) <= KEY_DBL_GAP_MS) {
        lastPressValid = false;
        onDoubleClick();
      } else {
        lastPressValid = true;
        lastPressMs    = now;
      }
      #if KEY_DIAG
      Serial.println(F("[KEY] ↓ 按下"));
      #endif
    } else {
      unsigned long dur = now - keyDownMs;
      apHolding = false;
      if (!keyLongFired) {
        if (dur < KEY_CLICK_MS) {
          // 单击不再挂起等窗口，松手立即提亮（双击已在按下沿切过小时制）
          onSingleClick();
          #if KEY_DIAG
          Serial.printf("[KEY] ↑ %lu ms → 单击提亮\n", dur);
          #endif
        } else {
          lastPressValid = false;       // 中按作废，别让它和下一次短按凑成双击
          #if KEY_DIAG
          Serial.printf("[KEY] ↑ %lu ms → 中按，忽略\n", dur);
          #endif
        }
      }
      // 中按已取消：0.8~8 秒松手什么都不做
    }
  }

  if (keyStable == LOW && keyArmed && !keyLongFired) {
    unsigned long held = now - keyDownMs;
    if (held >= KEY_AP_WARN_MS && !apHolding) {
      apHolding = true;       // 屏上倒数，回答"还要按多久"
      #if KEY_DIAG
      Serial.println(F("[KEY] 按住 3 秒 → 屏上开始倒数"));
      #endif
    }
    if (held >= KEY_AP_HOLD_MS) {
      keyLongFired   = true;
      apHolding      = false;
      lastPressValid = false;           // 长按作废双击状态
      #if KEY_DIAG
      Serial.println(F("[KEY] 长按 8 秒 → 进配网"));
      #endif
      onLongPress();
    }
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
  // 配网页不缓存：手机连上热点自动弹页时，缓存的旧页面可能带过期的扫描/连接状态
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/html; charset=utf-8", WIFI_HTML);
}

// 生成闹钟铃声下拉框的 <option> 串，当前选中的那个带 selected。
// v4.5：6 段都是 flash 里的真实 WAV，全部可选。
String toneOptions(uint8_t cur) {
  String s;
  uint8_t sel = (cur < TONE_COUNT) ? cur : TONE_DEFAULT;
  for (uint8_t i = 0; i < TONE_COUNT; i++) {
    s += F("<option value=\"");
    s += String((int)i);
    s += '"';
    if (i == sel) s += F(" selected");
    s += '>';
    s += TONE_NAMES[i];
    s += F("</option>");
  }
  return s;
}

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

// ------------------------------------------------------------
//  单遍模板渲染（v4.7.1）
//  原来用 18 次 String::replace() 填表，每次都要从结尾把 17.6KB
//  全文扫一遍（lastIndexOf 逐次回扫），实测 33ms —— 占了首页一半时间。
//  这里改成从头到尾只走一遍：碰到 {{NAME}} 查表填值，其余原样拷贝。
//  一次 malloc + 一次 String 赋值，剩余开销是一次 17.6KB 的 memcpy。
// ------------------------------------------------------------
static const char *tplLookup(const TplVar *v, int n, const char *key, size_t klen) {
  for (int i = 0; i < n; i++) {
    if (strlen(v[i].key) == klen && memcmp(v[i].key, key, klen) == 0) return v[i].val;
  }
  return nullptr;
}

static void tplRender(String &out, const char *tpl, const TplVar *v, int n) {
  size_t cap = strlen(tpl) + 2048;
  for (int i = 0; i < n; i++) cap += strlen(v[i].val);
  char *buf = (char *)malloc(cap);
  if (!buf) { out = ""; return; }
  size_t o = 0;
  const char *p = tpl;
  while (*p && o + 1 < cap) {
    if (p[0] == '{' && p[1] == '{') {
      const char *e = strchr(p + 2, '}');
      if (e && e[1] == '}') {
        const char *val = tplLookup(v, n, p + 2, (size_t)(e - (p + 2)));
        size_t vl = val ? strlen(val) : 0;
        if (val && o + vl < cap) {
          memcpy(buf + o, val, vl);
          o += vl;
          p = e + 2;
          continue;
        }
      }
    }
    buf[o++] = *p++;
  }
  buf[o] = 0;
  out = buf;
  free(buf);
}

// ------------------------------------------------------------
//  验证页（v4.7.4 引入，v4.7.6 改成「主动获取」）
// ------------------------------------------------------------
// 倒计时秒数塞进页面，这样刷新页面也绕不过 60 秒冷却
static void sendLoginPage(const char *err) {
  char cool[8];
  snprintf(cool, sizeof(cool), "%lu", (authGetCoolLeft() + 999) / 1000);
  const TplVar v[] = {{"ERR", err}, {"COOL", cool}};
  String out;
  tplRender(out, LOGIN_HTML, v, 2);
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/html; charset=utf-8", out);
}

void handleLogin() {
  // 【v4.7.6】只是打开验证页**不生成**验证码。
  // 原来一打开就让屏幕跳数字，误点一个链接、浏览器预取、自己随手点开都会亮一次，
  // 太打扰了。现在必须点「获取验证码」才出。
  if (server.hasArg("get")) {
    unsigned long left = authGetCoolLeft();
    if (left) {
      char b[48];
      snprintf(b, sizeof(b), "{\"e\":\"wait\",\"s\":%lu}", (left + 999) / 1000);
      server.send(200, "application/json; charset=utf-8", b);
      return;
    }
    authNewPin("用户点了获取验证码");
    char b[48];
    snprintf(b, sizeof(b), "{\"ok\":1,\"cool\":%lu,\"pin\":%lu}",
             (unsigned long)(AUTH_GET_COOL_MS / 1000),
             (unsigned long)(AUTH_PIN_MS / 1000));
    server.send(200, "application/json; charset=utf-8", b);
    return;
  }
  sendLoginPage("");
}

// 验证失败的统一出口。
// ajax=1 时回 JSON，页面原地显示提示、已输入的数字不清空；
// 不带 ajax（JS 没跑起来 / 老页面）就退回整页渲染的老路子。
static void loginFail(bool ajax, const char *code, const char *msg) {
  if (ajax) {
    String j = "{\"e\":\"";
    j += code;
    j += "\",\"m\":\"";
    j += msg;
    j += "\"}";
    server.send(200, "application/json; charset=utf-8", j);
  } else {
    sendLoginPage(msg);
  }
}

void handleLoginPost() {
  bool ajax = server.hasArg("ajax");
  unsigned long now = millis();
  if (authLastTry && now - authLastTry < AUTH_BLOCK_MS) {
    loginFail(ajax, "slow", "提交太快了，等两秒再试");
    return;
  }
  authLastTry = now;

  if (!authPinFresh()) {
    // 这里**不**自动补发新码，否则又变成"屏幕自己跳数字"
    loginFail(ajax, "expired", "验证码已过期，点「获取验证码」重新取一个");
    return;
  }

  String in = server.arg("pin");
  if (in != String(authPin)) {
    authFail++;
    Serial.printf("[AUTH] 验证码不对（第 %u/%u 次）\n",
                  (unsigned)authFail, (unsigned)AUTH_FAIL_MAX);
    if (authFail >= AUTH_FAIL_MAX) {
      // 作废并且把获取冷却从现在重新开始算，等于逼攻击者每轮至少等 60 秒
      authPin   = 0;
      authLastGet = millis();
      Serial.println(F("[AUTH] 错 5 次，验证码作废"));
      loginFail(ajax, "fail", "错太多次了，点「获取验证码」重新取一个");
    } else {
      loginFail(ajax, "bad", "不对，再看看屏幕");
    }
    return;
  }

  char sid[33];
  authSessionNew(sid);
  authPin = 0;                       // 一次性，用过即废
  server.sendHeader("Set-Cookie",
                    String("sid=") + sid + "; Path=/; HttpOnly; SameSite=Lax");
  server.sendHeader("Cache-Control", "no-store");
  if (ajax) {
    // 页面拿到 {"ok":1} 自己 location.replace('/')，不用走 303 重定向，
    // 少一次往返，手机上也不会闪一下白屏
    server.send(200, "application/json; charset=utf-8", "{\"ok\":1}");
  } else {
    server.sendHeader("Location", "/");
    server.send(303, "text/plain", "ok");
  }
}

void handleLogout() {
  authSessionDrop();
  server.sendHeader("Set-Cookie", "sid=; Path=/; Max-Age=0; HttpOnly; SameSite=Lax");
  // v4.8.3：原来只回一段 "bye"，点了像没反应。现在直接 303 回验证码页，
  // 用户回来看到的就是「需要验证 / 获取验证码」—— 和从别处跳进来被拦时一致。
  // 303 而不是 302：GET/POST 都统一转成 GET，刷新不会重提注销。
  server.sendHeader("Location", "/login");
  server.sendHeader("Cache-Control", "no-store");
  server.send(303, "text/plain", "bye");
}

// 需要验证时的统一出口：页面回 303 跳验证页，接口回 401 JSON
// （接口要是也跳转，网页那边 fetch 会拿到一段 HTML 再去 JSON.parse，报错更难查）
// 注意：这里**不生成验证码**，把"什么时候亮数字"的决定权留给用户。
static void authDeny() {
  if (server.uri().startsWith("/api/")) {
    server.send(401, "application/json; charset=utf-8", "{\"e\":\"auth\"}");
  } else {
    server.sendHeader("Location", "/login");
    server.sendHeader("Cache-Control", "no-store");
    server.send(303, "text/plain", "need pin");
  }
}

void handleRoot() {
  bool viaAp = reqFromAp();

  // 还没连上网，或者手机是连着本机热点进来的 → 首页给配网页
  if (WiFi.status() != WL_CONNECTED || viaAp) {
    sendWifiPage();
    return;
  }

  uint32_t tCopy0 = micros();

  static char vSync[64], vBright[8], vNbr[8], vSnz[8], vVol[8], vRgbB[8];
  static char vWifi[72], vIp[24];

  if (lastSyncEpoch == 0) {
    snprintf(vSync, sizeof(vSync), "等待联网校时（本板无 RTC）");
  } else {
    int mins = (int)((millis() - lastSyncMs) / 60000UL);
    if (mins < 60) snprintf(vSync, sizeof(vSync), "%d 分钟前（网络时间）", mins);
    else           snprintf(vSync, sizeof(vSync), "%d 小时前（网络时间）", mins / 60);
  }
  snprintf(vBright, sizeof(vBright), "%d", (int)cfg.brightness);
  snprintf(vNbr,    sizeof(vNbr),    "%d", (int)cfg.nightBright);
  snprintf(vSnz,    sizeof(vSnz),    "%d", (int)cfg.snoozeMin);
  snprintf(vVol,    sizeof(vVol),    "%d", (int)cfg.volume);
  snprintf(vRgbB,   sizeof(vRgbB),   "%d", (int)cfg.rgbBright);
  snprintf(vWifi,   sizeof(vWifi),   "%s · %s", staSSID,
           WiFi.localIP().toString().c_str());
  snprintf(vIp,     sizeof(vIp),     "%s", WiFi.localIP().toString().c_str());

  // 下拉列表动辄 1.2KB（24 个 <option>），放静态数组要么开很大、要么被截断。
  // 用局部 String 托着，c_str() 在 tplRender 调用期间一直有效。
  String sNsOpts = hourOptions(cfg.nightStart);
  String sNeOpts = hourOptions(cfg.nightEnd);
  String sCsOpts = hourOptions(cfg.chimeStart);
  String sCeOpts = hourOptions(cfg.chimeEnd);
  String sTone   = toneOptions(cfg.alarmTone);

  const TplVar vars[] = {
    {"SYNC",       vSync},
    {"BRIGHT",     vBright},
    {"NON",        cfg.nightOn  ? "checked" : ""},
    {"NBR",        vNbr},
    {"NS_OPTS",    sNsOpts.c_str()},
    {"NE_OPTS",    sNeOpts.c_str()},
    {"M24",        cfg.mode24   ? "checked" : ""},
    {"SNOOZE",     vSnz},
    {"VOL",        vVol},
    {"CHIME",      cfg.chime    ? "checked" : ""},
    {"ATONE_OPTS", sTone.c_str()},
    {"CS_OPTS",    sCsOpts.c_str()},
    {"CE_OPTS",    sCeOpts.c_str()},
    {"RGBON",      cfg.rgbOn    ? "checked" : ""},
    {"RGBB",       vRgbB},
    {"FW",         FW_VERSION},
    {"WIFI",       vWifi},
    {"IP",         vIp},
  };

  String page;
  tplRender(page, PAGE_HTML, vars, (int)(sizeof(vars) / sizeof(vars[0])));
  uint32_t tBuild1 = micros();

  // 两段分别计时：到底是"拼页面慢"还是"发出去慢"，别靠猜
  server.send(200, "text/html; charset=utf-8", page);
  uint32_t tSend1 = micros();
  pageBuildUs = tBuild1 - tCopy0;
  pageSendUs  = tSend1  - tBuild1;
  pageTotalUs = tSend1  - tCopy0;
}

void handleWifiPage() {
  sendWifiPage();
}

void handleScan() {
  // v4.8.12：直接读后台异步扫描的缓存，零等待（旧版同步扫描阻塞 2~4 秒）。
  // "busy"=1 表示首轮扫描还没出结果，前端可短暂等一下再取，不用干等 2~4 秒。
  String j = "{\"busy\":";
  j += (scanRunning && scanCount == 0) ? "1" : "0";
  j += ",\"n\":[";
  for (int i = 0; i < scanCount; i++) {
    if (i) j += ",";
    j += "{\"s\":\"" + jsonEscape(scanCache[i].ssid) + "\",\"bars\":" +
         String(scanCache[i].bars) + ",\"e\":" + String(scanCache[i].enc) + "}";
  }
  j += "]}";
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
  // 新硬件：WS2812B 氛围灯。同样「带参数才改」，老页面/手工 curl 不会误关灯
  if (server.hasArg("rgb"))  cfg.rgbOn     = server.arg("rgb").toInt() ? 1 : 0;
  if (server.hasArg("rgbb")) cfg.rgbBright = constrain(server.arg("rgbb").toInt(), 1, 10);

  saveCfg();

  smoothBright = -1;                               // 亮度重新收敛（立即生效，不等 2Hz 任务）
  tmSetBrightness(baseBrightness());
  // v4.8.1：这里原来有一句 `for (i) lastAlarmKey[i] = 0;`（"允许当天重新触发"），
  // 是单闹钟时代留下的 —— 那时闹钟就在首页上、保存首页就等于改闹钟。
  // 现在闹钟走 /alarm 单独接口，/save 根本碰不到闹钟，这句只会造成事故：
  // 闹钟正在响的那一分钟里，网页上随手拨个音量/氛围灯开关，就能让按停后的闹钟立刻复燃
  // （2026-11-17 21:28 实测复现）。删掉。


  server.send(200, "text/plain", "ok");
}

// 射频自检：把"到底限没限功率、省电开没开、链路多强"一次问清楚。
// 排查"信号差"时先看这里——RSSI 正常但 ping 抖，那是省电；RSSI 很低，
// 那才是天线/匹配/距离问题，跟固件无关。
void handleRf() {
  wifi_ps_type_t ps = WIFI_PS_NONE;
  esp_wifi_get_ps(&ps);
  int8_t dbm = -1;
  esp_wifi_get_max_tx_power(&dbm);
  char buf[220];
  snprintf(buf, sizeof(buf),
           "{\"rssi\":%d,\"ch\":%d,\"tx_dbm\":%.1f,\"ps\":%d,\"st\":%d,\"ip\":\"%s\",\"cfg_dbm\":%d}",
           (int)WiFi.RSSI(), (int)WiFi.channel(), dbm / 4.0f, (int)ps,
           (int)WiFi.status(), WiFi.localIP().toString().c_str(), CFG_TX_DBM);
  server.send(200, "application/json", buf);
}

// RSSI 分档，给网页头部显示用。阈值按 2.4G 实测经验：
//   >= -55 贴着路由器；-55~-65 同房间；-65~-75 隔一堵墙；-75 以下开始丢包。
static const char *rssiWord(int rssi) {
  if (rssi >= -55) return "极佳";
  if (rssi >= -65) return "良好";
  if (rssi >= -75) return "一般";
  if (rssi >= -85) return "偏弱";
  return "很弱";
}

// GET /api/diag —— 一次拿齐设备"健康状况"，稳定版的体检入口。
// 关键设计：用 static 定长缓冲拼 JSON，不碰 String —— 这个接口存在的意义
// 就是排查内存问题，它自己再去堆上拼字符串会很讽刺。
void handleDiag() {
  static char b[1760];
  // getNow() 单次成本：它内部要 time() + localtime_r()，每秒被调用几百次的话
  // 就是纯浪费。这里顺便量一下，改完再对比。
  uint32_t t0 = micros();
  volatile int dummy = 0;
  for (int i = 0; i < 100; i++) { NowTime t = getNow(); dummy += t.s; }
  uint16_t nowUs = (uint16_t)((micros() - t0) / 100);
  (void)dummy;

  int n = snprintf(b, sizeof(b),
    "{\"fw\":\"%s\",\"uptime\":%lu,\"reset\":\"%s\","
    "\"heap\":%u,\"minHeap\":%u,\"largest\":%u,\"heapTotal\":%u,"
    "\"bootToWifi\":%lu,\"loopHz\":%u,\"loopUs\":%u,\"loopMaxUs\":%lu,\"nowUs\":%u,"
    "\"wifi\":{\"st\":%d,\"rssi\":%d,\"ch\":%d,\"ip\":\"%s\"},"
    "\"audio\":{\"mode\":%u,\"rate\":%u,\"pos\":%lu,\"frames\":%lu},"
    "\"spiffs\":{\"used\":%u,\"total\":%u,\"skipped\":%d},"
    "\"page\":{\"build\":%lu,\"send\":%lu,\"total\":%lu},"
    "\"disp\":{\"w\":%lu,\"f\":%lu},\"boot\":[",
    FW_VERSION, (unsigned long)(millis() / 1000), resetReasonText(),
    (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap(),
    (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
    (unsigned)ESP.getHeapSize(),
    (unsigned long)bootToWifiMs, (unsigned)loopHz, (unsigned)loopWorkAvg,
    (unsigned long)loopWorkMax, (unsigned)nowUs,
    (int)WiFi.status(), (int)WiFi.RSSI(), (int)WiFi.channel(),
    WiFi.localIP().toString().c_str(),
    (unsigned)audMode, (unsigned)audWavRate,
    (unsigned long)audWavPos, (unsigned long)audWavFrames,
    (unsigned)(fsReady ? SPIFFS.usedBytes() : 0),
    (unsigned)(fsReady ? SPIFFS.totalBytes() : 0),
    fsSkipped ? 1 : 0,
    (unsigned long)pageBuildUs, (unsigned long)pageSendUs,
    (unsigned long)pageTotalUs,
    (unsigned long)tmFrameWrites, (unsigned long)tmForceWrites);
  if (n < 0) n = 0;
  if (n >= (int)sizeof(b)) n = sizeof(b) - 1;
  for (int i = 0; i < bootStageN; i++) {
    if ((int)sizeof(b) - n < 48) break;
    int w = snprintf(b + n, sizeof(b) - n, "%s[\"%s\",%lu]",
                     i ? "," : "", bootStageName[i],
                     (unsigned long)(bootStageUs[i] / 1000));
    if (w < 0 || w >= (int)sizeof(b) - n) break;
    n += w;
  }
  if ((int)sizeof(b) - n > 8) {
    int w = snprintf(b + n, sizeof(b) - n, "],\"prof\":[");
    if (w > 0 && w < (int)sizeof(b) - n) n += w;
  }
  // 每项：["任务名", 每轮平均微秒, 峰值微秒]
  for (int i = 0; i < PROF_N; i++) {
    if ((int)sizeof(b) - n < 40) break;
    int w = snprintf(b + n, sizeof(b) - n, "%s[\"%s\",%u,%lu]",
                     i ? "," : "", PROF_NAME[i],
                     (unsigned)profAvg[i], (unsigned long)profMax[i]);
    if (w < 0 || w >= (int)sizeof(b) - n) break;
    n += w;
  }
  if ((int)sizeof(b) - n > 4) {
    int w = snprintf(b + n, sizeof(b) - n, "]}");
    if (w > 0 && w < (int)sizeof(b) - n) n += w;
  }
  server.send(200, "application/json; charset=utf-8", b);
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
  if (WiFi.status() == WL_CONNECTED) {
    int rssi = WiFi.RSSI();
    j += "WiFi 已连接 · 信号 ";
    j += rssiWord(rssi);
    j += " ";
    j += String(rssi);
    j += "dBm";
  }
  else if (apActive)                 j += "配网热点已开启";
  else                               j += "WiFi 未连接";

  if (!t.valid) j += " · 等待 NTP 校时";
  // 30 秒兜底启用后时间照走，但得说清楚它没跟网络对过
  else if (lastSyncEpoch == 0) j += " · 未校时（复位残留时间）";

  j += " · 亮度 ";
  j += String(appliedBright < 0 ? 0 : appliedBright);
  if (cfg.nightOn && t.valid && inNightWindow(t.h * 60 + t.m)) j += "（夜间时段）";
  if (alarmRinging && ringingIdx >= 0) j += " · 闹钟" + String(ringingIdx + 1) + " 响铃";
  else if (alarmRinging)               j += " · 响铃中";
  else if (snoozeArmed)                j += " · 贪睡中";

  // 12/24 小时制：网页靠这个字段把开关跟设备对齐。
  // 中按也能切，网页手里那个开启页面时的旧值必须能被纠正，
  // 所以状态里必须有真值，不能只靠模板渲染一次。
  j += "\",\"m24\":";
  j += (cfg.mode24 ? "1" : "0");
  j += "}";

  server.send(200, "application/json; charset=utf-8", j);
}

void handleSync() {
  if (WiFi.status() == WL_CONNECTED && syncFromNtp()) {
    server.send(200, "text/plain", "ok");
  } else {
    server.send(200, "text/plain", "fail");
  }
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

  // v4.8.0：改之前先记下「触发条件」，用来判断这次要不要清去重键
  uint8_t oldH = alarms[i].hour, oldM = alarms[i].min, oldD = alarms[i].days;

  if (server.hasArg("h")) alarms[i].hour   = constrain(server.arg("h").toInt(), 0, 23);
  if (server.hasArg("m")) alarms[i].min    = constrain(server.arg("m").toInt(), 0, 59);
  if (server.hasArg("e")) alarms[i].enable = server.arg("e").toInt() ? 1 : 0;
  if (server.hasArg("d")) {
    uint8_t d = (uint8_t)(server.arg("d").toInt() & 0x7F);
    alarms[i].days = d ? d : ALARM_DAYS_ALL;
  }
  saveCfg();
  // 只有「响铃时刻」真的变了才清去重键（lastAlarmKey 是这一分钟已响过的凭据）。
  // 原来这里无条件 lastAlarmKey[i] = 0：闹钟正在响的那一分钟里，网页哪怕只拨了
  // 一下开关（时刻没动）也会把凭据清掉，于是用户按停之后 alarmDue 立刻又匹配上，
  // 同一分钟内再响一遍 —— 2026-11-17 21:17 真机复现过。
  if (alarms[i].hour != oldH || alarms[i].min != oldM || alarms[i].days != oldD)
    lastAlarmKey[i] = 0;
  // v4.8.0 诊断：闹钟表被"谁"改了必须留痕。之前出现过「按键停铃后又立刻响、
  // 且 7:30 那组莫名变成当前时刻」，靠串口这一行才能分清是网页写进来的还是固件自己改的。
  Serial.printf("[ALM] set i=%d → %02d:%02d e=%d d=%d   (h=%s m=%s e=%s d=%s)\n",
                i, alarms[i].hour, alarms[i].min, alarms[i].enable, alarms[i].days,
                server.arg("h").c_str(), server.arg("m").c_str(),
                server.arg("e").c_str(), server.arg("d").c_str());
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
  Serial.printf("[ALM] del i=%d → 剩 %d 组\n", i, alarmCount);
  server.send(200, "text/plain", "ok");
}

// 试听：?t=alarm 放一遍闹钟铃声，?t=chime（或不带参数）试听整点语音报时。
// v4.2：报时改用语音，试听就按「下一个整点」拼一段「现在时间 X 点整」；
// 未校时则用 8 点演示，保证任何时候按下都有声音。
void handleAudioTest() {
  if (!alarmRinging) {
    if (server.arg("t") == "alarm") {
      playTone(cfg.alarmTone, false);
    } else {
      NowTime t = getNow();
      int demoHour = t.valid ? ((t.h + 1) % 24) : 8;
      chimeSpeakHour(demoHour);
    }
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
void captiveRedirect() {
  if (reqFromAp()) {
    String url = String("http://") + WiFi.softAPIP().toString() + "/";
    server.sendHeader("Cache-Control", "no-store");
    server.sendHeader("Location", url);
    server.send(302, "text/plain", "");
  } else {
    server.send(404, "text/plain", "404");
  }
}

void handleNotFound() {
  captiveRedirect();
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
  bootT0Us = micros();               // 复位→这里的耗时就是引导+固件启动
  Serial.begin(115200);
  delay(200);                        // 等 USB CDC 枚举完，别丢掉开机日志
  Serial.printf("[BOOT] 复位→setup %lums（引导+启动代码）\n",
                (unsigned long)(bootT0Us / 1000));

  // ── 时区 + 复位残留时间（v4.7.2）────────────────────────────
  // 复位（USB/软件/看门狗）不会清掉 RTC 里的 epoch，但 TZ 环境变量在 RAM 里、
  // 复位即丢。不先把时区钉死，localtime_r 就会按 UTC 算 —— 屏幕在拿到 NTP 之前
  // 先显示"早 8 小时"的时间。这里两件事一起做：
  //   ① fixTimezone()：时区兜底，任何路径下换算出来的都是北京时间；
  //   ② 把残留值打出来，确认它到底有没有被保留（排查时不用猜）。
  fixTimezone();
  {
    time_t tRtc = time(nullptr);
    if (tRtc > 1700000000UL) {
      // 同一时刻按 UTC 和本地各打一次：两个值差 8 小时就说明时区确实生效了。
      // 这一行是「开机先显示另一个时区」那次问题的直接证据来源。
      struct tm g, l;
      gmtime_r(&tRtc, &g);
      localtime_r(&tRtc, &l);
      Serial.printf("[RTC] 复位后系统时间仍在：%04d-%02d-%02d %02d:%02d:%02d UTC"
                    " = %02d:%02d:%02d 本地（不显示，等校时成功）\n",
                    g.tm_year + 1900, g.tm_mon + 1, g.tm_mday,
                    g.tm_hour, g.tm_min, g.tm_sec,
                    l.tm_hour, l.tm_min, l.tm_sec);
    } else {
      Serial.println(F("[RTC] 无残留时间（冷启动），等 SNTP"));
    }
  }

  // GPIO0 是 BOOT/strapping 脚：上电瞬间被 ROM 采样，开机别按着它，
  // 否则直接进下载模式。跑起来之后当普通按键用没问题（内部上拉）。
  pinMode(PIN_KEY,  INPUT_PULLUP);
  pinMode(PIN_KEY2, INPUT_PULLUP);

  // TM1652 上电（单线总线空闲为高）
  pinMode(PIN_TM1652, OUTPUT);
  digitalWrite(PIN_TM1652, HIGH);
  delay(50);
  bootMark("引脚就绪");

  // 新硬件：彩灯 / I2S 功放，两者互不共用引脚
  wsInit();
  bootMark("彩灯");
  audioInit();
  bootMark("功放");
  // 注意：SPIFFS 不在这里挂载。fsDeferredTask() 会在联网之后再做，
  // 因为 SPIFFS.begin() 要 5.6 秒，而本版功能一个都不用文件系统。

  // 配置要在自检之前读出来：开机自检用「设定亮度」，不能固定拉满 7 档
  loadCfg();
  cleanupLegacyKeys();          // v4.2：清掉 v4.1 遗留的 ctone/sron/srm/sls/slmin
  bootMark("读配置");
  bootAnim();
  bootMark("开机自检");

  // 亮度先定下来，避免开机一瞬间全亮；彩灯同步给一次初始状态
  smoothBright = -1;
  brightTask();
  wsTask();

  // WiFi 必须先初始化：WebServer 底下是 esp_http_server，依赖 lwIP/netif，
  // 顺序反过来会在 httpd_start 里 xQueueSemaphoreTake 断言，开机就重启。
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  loadWifiCreds();
  loadAssocHint();          // 上次连上时的信道/BSSID，用来跳过全信道扫描

  if (staSSID[0]) {
    beginConnect(staSSID, staPass);
  } else {
    // 我们这边没有凭据 → 顺手把驱动层的旧记录也擦掉。
    // 否则 AP_STA 一开，驱动会拿 nvs.net80211 里的记录自动连回上一个路由器：
    // 热点刚起来就连上、60 秒后又被自动关掉，用户完全看不懂发生了什么。
    eraseDriverWifiConfig();
    startPortal();
  }
  bootMark("WiFi 启动");

  // Web 服务（必须在 WiFi 初始化之后）
  //
  // 【v4.7.4】除 /login、/logout 外全部要过验证。
  // 从本机热点（192.168.4.x）进来的请求免验证：那个热点只能靠长按按键 3 秒亲手打开，
  // "物理在场"这件事已经证实过了。留这条也是怕死锁——设备装在高处、屏幕看不见时，
  // 至少还能长按进配网页把 WiFi 改掉。
  #define AUTH_ROUTE(uri, fn) server.on(uri, []() {            \
      if (reqFromAp() || authSessionValid()) { fn(); return; }  \
      authDeny(); })
  // 不先 collectHeaders 声明，server.header() 读不到 Cookie（默认只收 Content-Type）
  static const char *authHdrKeys[] = {"Cookie"};
  server.collectHeaders(authHdrKeys, 1);

  AUTH_ROUTE("/",           handleRoot);
  AUTH_ROUTE("/wifi",       handleWifiPage);
  AUTH_ROUTE("/scan",       handleScan);
  AUTH_ROUTE("/connect",    handleConnect);
  AUTH_ROUTE("/netstatus",  handleNetStatus);
  AUTH_ROUTE("/portal",     handlePortal);
  AUTH_ROUTE("/save",       handleSave);
  AUTH_ROUTE("/api/status", handleStatus);
  AUTH_ROUTE("/api/rf",     handleRf);
  AUTH_ROUTE("/api/diag",   handleDiag);
  AUTH_ROUTE("/api/segtest", handleSegTest);
  AUTH_ROUTE("/sync",       handleSync);
  AUTH_ROUTE("/api/alarms", handleAlarmList);
  AUTH_ROUTE("/alarm",      handleAlarmSet);
  AUTH_ROUTE("/alarmadd",   handleAlarmAdd);
  AUTH_ROUTE("/alarmdel",   handleAlarmDel);
  AUTH_ROUTE("/audiotest",  handleAudioTest);
  AUTH_ROUTE("/audiostop",  handleAudioStop);
  AUTH_ROUTE("/resetwifi",  handleResetWifi);

  server.on("/login",  HTTP_GET,  handleLogin);
  server.on("/login",  HTTP_POST, handleLoginPost);
  server.on("/logout", HTTP_POST, handleLogout);
  server.on("/logout", HTTP_GET,  handleLogout);

  // 【v4.8.12】captive portal：手机/PC 连上热点后会主动探测这些 URL 判断
  // 「这个网络要不要登录」。显式接住并 302 到配网页，iOS/安卓/Windows 才会
  // 稳定弹出配网页（和 onNotFound 的兜底相比，命中更快、语义更明确）。
  static const char *captivePaths[] = {
    "/hotspot-detect.html",        // Apple
    "/generate_204",               // Android / Chrome
    "/gen_204",                    // Android 变体
    "/mobile/status.php",          // Android
    "/check_network_status.txt",   // Windows
    "/ncsi.txt",                   // Windows
    "/connectivity-check.html",    // Firefox
    "/success.txt",                // 通用
    "/portal.html",                // 通用
    "/library/test/success.html"   // Apple
  };
  for (const char *p : captivePaths) server.on(p, HTTP_GET, captiveRedirect);

  server.onNotFound(handleNotFound);
  // WebServer::handleClient() 在没有请求时默认会 delay(1)（_nullDelay=true），
  // 主循环每轮就被白吃 1 毫秒 —— 实测占掉 loop 里 84% 的干活时间，循环卡在 164Hz。
  // 关掉它，空转立即返回；每轮末尾本来就有 delay(5) 让出 CPU，不会忙等。
  server.enableDelay(false);
  server.begin();
  bootMark("HTTP 就绪");

  // 只留这一行开机标识：确认版本、确认 WiFi 配置在不在，够用且不刷屏
  Serial.printf("[BOOT] XiaoMiClock v%s  WiFi=%s\n",
                FW_VERSION, staSSID[0] ? staSSID : "(未配置)");
  // 堆水位：Free 大不代表 Largest 大，两个数一起看才有意义
  Serial.printf("[MEM] 堆 Free=%u 最小=%u 最大连续=%u 总=%u\n",
                (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap(),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
                (unsigned)ESP.getHeapSize());
}

// ============================================================
//  主循环
// ============================================================
void loop() {
  uint32_t tLoop = micros();

  // 循环频率 + 干活耗时统计：delay(5) 的理论上限是 200Hz，
  // 实测低于 120Hz 或 loopWorkAvg 偏大，就说明有东西在阻塞主循环。
  loopCount++;
  {
    unsigned long nowMs = millis();
    if (nowMs - lastHzMs >= 1000) {
      loopHz = (uint16_t)((loopCount * 1000UL) / (nowMs - lastHzMs));
      loopWorkAvg = (uint16_t)(loopWorkSum / loopCount);
      for (int i = 0; i < PROF_N; i++) {
        profAvg[i] = (uint16_t)(profSum[i] / loopCount);
        profSum[i] = 0;
        profMax[i] = 0;
      }
      loopCount = 0;
      loopWorkSum = 0;
      loopWorkMax = 0;
      lastHzMs = nowMs;
    }
  }

  // 延迟重启放在最前面：等 HTTP 响应发完、显示刷完，再统一走一次 reboot
  if (rebootAtMs && (long)(millis() - rebootAtMs) >= 0) {
    Serial.printf("[SYS] 现在重启（%s）\n", rebootWhy);
    Serial.flush();
    delay(20);
    ESP.restart();
  }

  if (apActive) PROF_MEAS(PROF_WEB, dnsServer.processNextRequest());
  PROF_MEAS(PROF_WEB,   server.handleClient());
  PROF_MEAS(PROF_KEY,   keyTask());
  PROF_MEAS(PROF_KEY,   key2Task());

  // 【v4.8.11】长按下来的开热点请求在这里执行，不在 keyTask 的按键沿里同步做。
  // 手动热点给 10 分钟上限，没人配就自动关（和网页 /portal 行为一致）。
  if (portalPending) {
    portalPending = false;
    startPortal();
    apCloseAtMs = millis() + 600000UL;
  }

  PROF_MEAS(PROF_ALARM, alarmTask());
  PROF_MEAS(PROF_CHIME, chimeTask());
  PROF_MEAS(PROF_NET,   netTask());

  // 5Hz 刷屏；tmFlush 内部做差量比较，不会白刷总线
  if (millis() - lastDisplayMs >= 200) {
    lastDisplayMs = millis();
    PROF_MEAS(PROF_DISP, renderDisplay());
  }

  // 亮度按「平时亮度 + 夜间时段」计算，2Hz 刷新足够
  static unsigned long lastLight = 0;
  if (millis() - lastLight >= BRIGHT_INTERVAL) {
    lastLight = millis();
    PROF_MEAS(PROF_LIGHT, brightTask());
  }

  PROF_MEAS(PROF_WS,  wsTask());
  PROF_MEAS(PROF_NET, ntpTask());
  PROF_MEAS(PROF_FS,  fsDeferredTask());   // 联网后才挂载 SPIFFS 并清理老文件，不挡启动

  // 本轮实际干活耗时（不含下面这行 delay）
  {
    uint32_t work = micros() - tLoop;
    loopWorkSum += work;
    if (work > loopWorkMax) loopWorkMax = work;
  }

  delay(5);
}
