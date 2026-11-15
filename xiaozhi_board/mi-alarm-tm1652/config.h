#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

#include <driver/gpio.h>

/*
 * 小米闹钟自画裸片板（ESP32-S3 QFN56 / 16MB Flash / 8MB OPI PSRAM）
 * 引脚来源：MI-ALARM-ESP32S3/ESP32S3-PCB/硬件接线表.md
 *
 * 与官方 bread-compact-wifi 的差异只有功放 BCLK / LRCK：
 *   官方 15/16，本板 8/9。GPIO15/16 在本板是 32.768kHz 晶振脚，
 *   而 VDD3P3_RTC 电源域已确认必须接 3.3V，不能挪用。
 */

// ---- Audio: MAX98357 (I2S) + ICS-43434 (I2S, 独立控制器) ----
#define AUDIO_INPUT_SAMPLE_RATE  16000
#define AUDIO_OUTPUT_SAMPLE_RATE 24000
#define AUDIO_I2S_METHOD_SIMPLEX

#define AUDIO_I2S_MIC_GPIO_WS    GPIO_NUM_4
#define AUDIO_I2S_MIC_GPIO_SCK   GPIO_NUM_5
#define AUDIO_I2S_MIC_GPIO_DIN   GPIO_NUM_6

#define AUDIO_I2S_SPK_GPIO_DOUT  GPIO_NUM_7
#define AUDIO_I2S_SPK_GPIO_BCLK  GPIO_NUM_8
#define AUDIO_I2S_SPK_GPIO_LRCK  GPIO_NUM_9

// ---- Buttons ----
// GPIO0 是 BOOT/strapping：上电时不能按住，跑起来后当普通按键（低有效）
#define BOOT_BUTTON_GPIO         GPIO_NUM_0
// 第二按键，保留给后续「按下说话」用，本版未接 Button 对象
#define KEY2_BUTTON_GPIO         GPIO_NUM_2

// ---- LEDs: 两颗 WS2812B 串联 ----
#define BUILTIN_LED_GPIO         GPIO_NUM_10
#define BUILTIN_LED_COUNT        2

// ---- Display: TM1652 单线 SDA（4 位数字 + 冒号 + 图标）----
// 接线表写作 GPIO1-TM1652EN，用户确认这就是 SDA 数据线，不是电源使能
#define DISPLAY_TM1652_GPIO      GPIO_NUM_1

// ---- WiFi 射频 ----
// ESP-IDF 的 esp_wifi_set_max_tx_power() 单位是 0.25 dBm，80 = 20 dBm（PHY 上限）。
// 旧 C3 自画板因发射链供电/去耦不足曾限到 6 dBm；本板默认按满功率启动。
// 分档实测结论可用 UDP 命令 SAVE 写入 NVS mi_alarm/rf_best 覆盖本默认值。
#define RF_DEFAULT_TX_QUARTER_DBM   80

#endif // _BOARD_CONFIG_H_
