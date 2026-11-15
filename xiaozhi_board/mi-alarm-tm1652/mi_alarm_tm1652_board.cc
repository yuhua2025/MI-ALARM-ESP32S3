#include "wifi_board.h"
#include "codecs/no_audio_codec.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "led/circular_strip.h"
#include "tm1652_display.h"
#include "rf_tools.h"

#include <esp_log.h>
#include <esp_wifi.h>

#define TAG "MiAlarmTm1652Board"

/*
 * 小米闹钟自画裸片板的小智适配版。
 * 硬件：ESP32-S3 / 16MB Flash / 8MB OPI PSRAM
 *   TM1652 数码管 (GPIO1) → 当作 Display，配网/激活显示原机开机动画
 *   MAX98357 (GPIO7/8/9) + ICS-43434 (GPIO4/5/6) → 两路独立 I2S 直连
 *   2× WS2812B (GPIO10) → CircularStrip
 *   GPIO0 BOOT 按键 → 开机进配网 / 运行时切换对话
 */
class MiAlarmTm1652Board : public WifiBoard {
private:
    Button boot_button_;
    Tm1652Display display_;

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });
    }

public:
    MiAlarmTm1652Board()
        : boot_button_(BOOT_BUTTON_GPIO),
          display_(DISPLAY_TM1652_GPIO) {
        InitializeButtons();
        ESP_LOGI(TAG, "board initialized: TM1652 + MAX98357 + ICS-43434 + 2xWS2812B");
    }

    virtual Led* GetLed() override {
        static CircularStrip led(BUILTIN_LED_GPIO, BUILTIN_LED_COUNT);
        return &led;
    }

    virtual AudioCodec* GetAudioCodec() override {
        // Simplex：麦克风与功放各占一个 I2S 控制器，和 Arduino v4.1 固件的接法一致
        static NoAudioCodecSimplex audio_codec(
            AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_SPK_GPIO_BCLK, AUDIO_I2S_SPK_GPIO_LRCK, AUDIO_I2S_SPK_GPIO_DOUT,
            AUDIO_I2S_MIC_GPIO_SCK, AUDIO_I2S_MIC_GPIO_WS, AUDIO_I2S_MIC_GPIO_DIN);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return &display_;
    }

    virtual void StartNetwork() override {
        WifiBoard::StartNetwork();
        // 射频诊断/分档实测服务（UDP 3333），只在本板启用
        RfToolsStart();
    }

    virtual void SetPowerSaveLevel(PowerSaveLevel level) override {
        (void)level;
        // 官方 Application 在 Idle 会切 LOW_POWER -> WIFI_PS_MAX_MODEM，
        // 射频按 DTIM 周期睡眠，实测 ping RTT 400ms+ 且逐包递增。
        // 这台设备是插电闹钟，不需要省那点电，一律保持 PERFORMANCE(NONE)。
        // 发射功率与省电是两回事：功率由 rf_tools 单独管。
        WifiBoard::SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
    }
};

DECLARE_BOARD(MiAlarmTm1652Board);
