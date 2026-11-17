#include "tm1652_backlight.h"
#include "settings.h"

#include <esp_log.h>

#define TAG "Tm1652Backlight"

Tm1652Backlight::Tm1652Backlight(Tm1652Display* display) : Backlight(), display_(display) {
}

void Tm1652Backlight::RestoreBrightness() {
    Settings settings("display");
    int saved = settings.GetInt("brightness", 75);
    if (saved < 0) saved = 0;
    if (saved > 100) saved = 100;
    ESP_LOGI(TAG, "恢复数码管亮度 %d%% (NVS display/brightness)", saved);
    SetBrightness((uint8_t)saved, false);
    // 基类 brightness_ 初始就是 0：保存值恰好为 0 时 SetBrightness() 会提前返回，
    // 不会经过 SetBrightnessImpl()，所以这里直接投一次 0 档，避免开机停在默认 6 档。
    if (saved == 0 && brightness() == 0 && display_ != nullptr) {
        display_->SetBrightnessPercent(0);
    }
}

void Tm1652Backlight::SetBrightnessImpl(uint8_t brightness) {
    if (display_ != nullptr) {
        display_->SetBrightnessPercent(brightness);
    }
}
