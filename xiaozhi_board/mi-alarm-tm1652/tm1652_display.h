#ifndef MI_ALARM_TM1652_DISPLAY_H_
#define MI_ALARM_TM1652_DISPLAY_H_

#include "display.h"

#include <driver/gpio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <atomic>
#include <cstdint>

/*
 * 把小米闹钟的 TM1652 数码管接成小智的 Display。
 *
 * 小智 Display 基类只有 Lock()/Unlock() 两个纯虚函数，其余 UI 方法都有默认实现，
 * 所以不需要 LVGL、不需要像素屏。
 *
 * 核心约定（用户指定）：
 *   1. 平时（小智未被唤醒）显示时钟 HH:MM；
 *   2. 唤醒 / 对话 / 配网 / 激活时才切成表情动画；
 *   3. 激活("link") 与配网("gear") 都显示原机「开机配网表情」；
 *   4. 未校时的空窗期继续显示「开机配网表情」，不显示 ---- 占位；
 *      SNTP 一同步就直接切到 HH:MM，开机画面不跳变。
 *
 * 「是否被唤醒」不能只看 emotion 字符串：小智在 Idle / Connecting / Listening
 * 下发的都是同一个 "neutral"。因此用 Application::GetDeviceState() 作为主判据，
 * emotion 只决定交互期间显示哪一种表情。
 *
 * 小智固件本身没有 NTP 校时，所以本板自己起 SNTP（联网后自动对齐 UTC+8）。
 */
class Tm1652Display : public Display {
public:
    explicit Tm1652Display(gpio_num_t sda_gpio);
    ~Tm1652Display() override;

    void SetStatus(const char* status) override;
    void ShowNotification(const char* notification, int duration_ms = 3000) override;
    bool SetEmotion(const char* emotion) override;
    void SetChatMessage(const char* role, const char* content) override;
    void SetPowerSaveMode(bool on) override;
    bool IsMonochrome() const override { return true; }

protected:
    bool Lock(int timeout_ms = 0) override;
    void Unlock() override;

private:
    enum class Scene : uint8_t {
        WifiConfig = 0,  // 配网 / 激活：原机开机动画
        Idle,            // 中性表情（状态机兜底用）
        Listening,       // 聆听 / 思考
        Speaking,        // 说话 / 开心
        Error,           // 告警
        Clock,           // 平时：HH:MM
    };

    gpio_num_t sda_gpio_;
    SemaphoreHandle_t mutex_ = nullptr;
    TaskHandle_t refresh_task_ = nullptr;
    TaskHandle_t ntp_task_ = nullptr;
    std::atomic<bool> stopping_{false};
    std::atomic<uint8_t> scene_{static_cast<uint8_t>(Scene::WifiConfig)};
    Scene last_scene_ = static_cast<Scene>(0xFF);  // 哨兵值，让首帧一定打一条场景日志

    uint8_t frame_[6] = {0, 0, 0, 0, 0, 0};
    uint8_t shown_[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    int64_t last_cmd_us_ = 0;
    int applied_brightness_ = -1;

    // TM1652 单线时序
    static uint8_t ReverseByte(uint8_t b);
    void WaitCmd();
    void SendByte(uint8_t data);
    void SendCommand(uint8_t cmd);
    void SetBrightness(uint8_t intensity);
    void Flush(bool force = false);

    // 画面元素
    void ClearFrame();
    void SetDigit(uint8_t pos, uint8_t code);
    void DrawWifiWaitFrame(bool phase);
    // force_hour >= 0 时用于上电自检，画一个固定时间而不改系统时钟
    void DrawClock(bool colon_on, int force_hour = -1, int force_min = -1);
    void RenderFrame();
    static void RefreshTask(void* arg);

    // 时钟 / 状态
    static bool TimeSynced();
    static void NtpTask(void* arg);
    Scene EffectiveScene() const;
    static const char* SceneName(Scene scene);

    Scene MapEmotion(const char* emotion) const;
};

#endif // MI_ALARM_TM1652_DISPLAY_H_
