#ifndef MI_ALARM_TM1652_BACKLIGHT_H_
#define MI_ALARM_TM1652_BACKLIGHT_H_

#include "backlight.h"
#include "tm1652_display.h"

/*
 * 把小智标准的 Backlight（self.screen.set_brightness MCP 工具）接到
 * TM1652 数码管的 8 级亮度上。
 *
 * 官方 Backlight 已经处理好了 0~100 渐变、NVS 持久化（namespace "display"、
 * key "brightness"）与 RestoreBrightness()，这里只需实现 SetBrightnessImpl()
 * 把百分比转交给 Tm1652Display。这样大模型说「把屏幕调暗一点」时，
 * mcp_server 注册的 self.screen.set_brightness 就能直接生效。
 */
class Tm1652Backlight : public Backlight {
public:
    explicit Tm1652Backlight(Tm1652Display* display);

    // 官方 RestoreBrightness() 会把保存的 0 强制改成 10%，这里保留 0%（最暗档）
    void RestoreBrightness();

protected:
    void SetBrightnessImpl(uint8_t brightness) override;

private:
    Tm1652Display* display_;
};

#endif // MI_ALARM_TM1652_BACKLIGHT_H_
