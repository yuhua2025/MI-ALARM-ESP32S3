#include "tm1652_display.h"

#include "application.h"

#include <esp_err.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_netif_ip_addr.h>
#include <esp_netif_sntp.h>
#include <esp_rom_sys.h>
#include <esp_timer.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

// 小智固件不带任何 NTP 校时，本板自己起 SNTP（联网后自动对齐 UTC+8）
#define NTP_SERVER         "ntp.aliyun.com"

#define TAG "Tm1652Display"

// ---- TM1652 时序常量（与小米闹钟 v4.1 固件逐字节一致，已实测可点亮）----
#define DRIVE_CURRENT      6
#define TM1652_BITDELAY    49
#define TM1652_WAITCMD     3000L
#define TM1652_CMD_MODE    0x18
#define TM1652_CMD_ADDRESS 0x08

// BIT0=a … BIT6=g, BIT7=dp；BYTE0~3 是 4 位数字，BYTE4 是图标，BYTE5 空置
#define DIGIT_COUNT     4
#define ICON_BYTE       4
#define ICON_MASK       0x3F
#define COLON_UP_BYTE   1
#define COLON_DN_BYTE   3
#define COLON_MASK      0x80

#define ICON_BELL       0x01
#define ICON_WIFI       0x10

static const uint8_t SEG_NUM[10] = {0x3F, 0x06, 0x5B, 0x4F, 0x66,
                                    0x6D, 0x7D, 0x07, 0x7F, 0x6F};

// 原机开机配网动画：g 段中横常亮，f|a|b 三段每 500ms 闪一次
#define SEG_G      0x40
#define SEG_G_FAB  0x63

// 少量七段字母，用于 Err / 状态提示
#define SEG_E  0x79
#define SEG_r  0x50
#define SEG_n  0x54
#define SEG_o  0x5C
#define SEG_t  0x78

static portMUX_TYPE s_tm_spinlock = portMUX_INITIALIZER_UNLOCKED;

// 只有 SNTP 真正同步过才算校时成功。RTC 里可能残留一个「看似有效但错误」的时间
// （实测残留值会让 time(nullptr) 早于 2023 判断通过，但显示成 03:50）。
static std::atomic<bool> s_sntp_synced{false};

// 显示只认 NTP 回调给出的 UTC 秒 + 单调时钟增量，不再读 time(nullptr)。
// 实测热复位后偶发一次显示 05:02（真实 21:02，正好差 16h）：系统时间被
// adjtime 平滑校正 / 其它模块改动时，再手工 +8h 就会叠加成 +16h。
static portMUX_TYPE s_time_lock = portMUX_INITIALIZER_UNLOCKED;
static int64_t s_ntp_epoch_sec = 0;
static int64_t s_ntp_at_us = 0;

static void OnSntpSync(struct timeval* tv) {
    portENTER_CRITICAL(&s_time_lock);
    s_ntp_epoch_sec = (int64_t)tv->tv_sec;
    s_ntp_at_us = esp_timer_get_time();
    portEXIT_CRITICAL(&s_time_lock);
    s_sntp_synced.store(true);
    ESP_LOGI(TAG, "SNTP synced, epoch=%ld", (long)tv->tv_sec);
}

Tm1652Display::Tm1652Display(gpio_num_t sda_gpio) : sda_gpio_(sda_gpio) {
    // 4 位字符型「屏幕」，让任何按 width()/height() 判断的上层逻辑拿到合理值
    width_ = 4;
    height_ = 1;
    mutex_ = xSemaphoreCreateMutex();

    // 时钟不走 TZ 环境变量：直接用 UTC epoch + 8h 换算，避免 TZ 字符串解析歧义

    gpio_config_t cfg = {};
    cfg.pin_bit_mask = (1ULL << sda_gpio_);
    cfg.mode = GPIO_MODE_OUTPUT;
    cfg.pull_up_en = GPIO_PULLUP_ENABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    cfg.intr_type = GPIO_INTR_DISABLE;
    ESP_ERROR_CHECK(gpio_config(&cfg));

    gpio_set_level(sda_gpio_, 1);  // 单线总线空闲为高
    vTaskDelay(pdMS_TO_TICKS(50));

    SetBrightness(6);
    ClearFrame();
    Flush(true);

    // 上电第一帧就是开机配网表情，不必等 Application::Alert() 调用。
    // 未校时的空窗期继续沿用这套动画，SNTP 同步后直接切成 HH:MM，画面不跳变。
    DrawWifiWaitFrame(true);
    frame_[ICON_BYTE] = ICON_WIFI;
    Flush(true);

    // 上电自检：只算帧、不实际上屏，用来在串口留下时钟段码的硬证据
    uint8_t saved_frame[6];
    memcpy(saved_frame, frame_, sizeof(frame_));
    ClearFrame();
    DrawClock(true, 12, 34);
    ESP_LOGI(TAG, "Clock selftest 12:34 -> %02X %02X %02X %02X %02X %02X",
             frame_[0], frame_[1], frame_[2], frame_[3], frame_[4], frame_[5]);
    memcpy(frame_, saved_frame, sizeof(frame_));

    xTaskCreate(RefreshTask, "tm1652_refresh", 2048, this, 4, &refresh_task_);
    xTaskCreate(NtpTask, "tm1652_ntp", 3072, nullptr, 3, &ntp_task_);
    ESP_LOGI(TAG, "TM1652 ready on GPIO%d, clock=UTC+8, SNTP=%s", sda_gpio_, NTP_SERVER);
}

Tm1652Display::~Tm1652Display() {
    stopping_.store(true);
    if (refresh_task_ != nullptr) {
        vTaskDelete(refresh_task_);
        refresh_task_ = nullptr;
    }
    if (ntp_task_ != nullptr) {
        vTaskDelete(ntp_task_);
        ntp_task_ = nullptr;
    }
    if (mutex_ != nullptr) {
        vSemaphoreDelete(mutex_);
        mutex_ = nullptr;
    }
}

bool Tm1652Display::Lock(int timeout_ms) {
    if (mutex_ == nullptr) return false;
    TickType_t ticks = timeout_ms > 0 ? pdMS_TO_TICKS(timeout_ms) : portMAX_DELAY;
    return xSemaphoreTake(mutex_, ticks) == pdTRUE;
}

void Tm1652Display::Unlock() {
    if (mutex_ != nullptr) xSemaphoreGive(mutex_);
}

// ---------------------------------------------------------------------------
// 单线协议：低位在先 + 偶校验 + 起始/停止位，每字节约 0.55ms
// 关中断保护位宽，一次完整帧 7 字节约 3.8ms；只在画面变化时发送，
// 配网动画 500ms 才变一次，不会持续干扰 WiFi / I2S。
// ---------------------------------------------------------------------------
uint8_t Tm1652Display::ReverseByte(uint8_t b) {
    b = (uint8_t)(((b & 0xF0) >> 4) | ((b & 0x0F) << 4));
    b = (uint8_t)(((b & 0xCC) >> 2) | ((b & 0x33) << 2));
    b = (uint8_t)(((b & 0xAA) >> 1) | ((b & 0x55) << 1));
    return b;
}

void Tm1652Display::WaitCmd() {
    int64_t elapsed = esp_timer_get_time() - last_cmd_us_;
    if (elapsed < TM1652_WAITCMD) {
        esp_rom_delay_us((uint32_t)(TM1652_WAITCMD - elapsed));
    }
}

void Tm1652Display::SendByte(uint8_t data) {
    bool parity = true;
    portENTER_CRITICAL(&s_tm_spinlock);
    gpio_set_level(sda_gpio_, 0);
    esp_rom_delay_us(TM1652_BITDELAY);
    for (int i = 0; i < 8; ++i) {
        if (data & 1) parity = !parity;
        gpio_set_level(sda_gpio_, data & 1);
        data >>= 1;
        esp_rom_delay_us(TM1652_BITDELAY);
    }
    gpio_set_level(sda_gpio_, parity ? 1 : 0);
    esp_rom_delay_us(TM1652_BITDELAY);
    gpio_set_level(sda_gpio_, 1);
    portEXIT_CRITICAL(&s_tm_spinlock);
    esp_rom_delay_us(TM1652_BITDELAY);
    esp_rom_delay_us(TM1652_BITDELAY);
}

void Tm1652Display::SendCommand(uint8_t cmd) {
    WaitCmd();
    SendByte(TM1652_CMD_MODE);
    SendByte(cmd);
    last_cmd_us_ = esp_timer_get_time();
}

void Tm1652Display::SetBrightness(uint8_t intensity) {
    intensity &= 0x07;
    if ((int)intensity == applied_brightness_) return;
    uint8_t cmd = ReverseByte((uint8_t)(((intensity & 0x07) << 1) | 0x01));
    cmd |= (uint8_t)(ReverseByte((uint8_t)((DRIVE_CURRENT + 0x01) & 0x07)) >> 4);
    SendCommand(cmd);
    applied_brightness_ = intensity;
}

void Tm1652Display::Flush(bool force) {
    if (!force && memcmp(frame_, shown_, 6) == 0) return;

    // 移植诊断：把前若干帧真正推上总线的 6 字节段码打到串口，
    // 用来证明配网动画/时钟确实在刷，而不只是软件场景变量在变。
    // 限次输出，不会长期刷屏。
    static int frame_trace = 0;
    if (frame_trace < 24) {
        ESP_LOGI(TAG, "TM1652 frame #%d: %02X %02X %02X %02X %02X %02X",
                 ++frame_trace,
                 frame_[0], frame_[1], frame_[2], frame_[3], frame_[4], frame_[5]);
    }

    WaitCmd();
    SendByte(TM1652_CMD_ADDRESS);
    for (int i = 0; i < 6; ++i) SendByte(frame_[i]);
    last_cmd_us_ = esp_timer_get_time();
    memcpy(shown_, frame_, 6);
}

// ---------------------------------------------------------------------------
// 画面元素
// ---------------------------------------------------------------------------
void Tm1652Display::ClearFrame() {
    memset(frame_, 0, sizeof(frame_));
}

void Tm1652Display::SetDigit(uint8_t pos, uint8_t code) {
    if (pos < DIGIT_COUNT) frame_[pos] = code & 0x7F;  // dp 位归冒号用
}

void Tm1652Display::DrawWifiWaitFrame(bool phase) {
    // 只写中间两位；下半段 e/c/d 全程不参与。右下小方点（BYTE3 BIT7）常亮。
    uint8_t glyph = phase ? SEG_G_FAB : SEG_G;
    SetDigit(1, glyph);
    SetDigit(2, glyph);
    frame_[COLON_DN_BYTE] |= COLON_MASK;
}

void Tm1652Display::DrawClock(bool colon_on, int force_hour, int force_min) {
    if (force_hour >= 0) {
        // 上电自检：画固定时间，不动系统时钟
        SetDigit(0, SEG_NUM[(force_hour / 10) % 10]);
        SetDigit(1, SEG_NUM[force_hour % 10]);
        SetDigit(2, SEG_NUM[(force_min / 10) % 10]);
        SetDigit(3, SEG_NUM[force_min % 10]);
        frame_[ICON_BYTE] = ICON_WIFI;
    } else if (TimeSynced()) {
        // 以 SNTP 回调拿到的 UTC 秒为基准 + 单调时钟增量，再手动 +8h 用 gmtime_r。
        // 不读 time(nullptr)：它可能是 RTC 残留、被 adjtime 平滑校正或被别的模块改动。
        int64_t base, at;
        portENTER_CRITICAL(&s_time_lock);
        base = s_ntp_epoch_sec;
        at = s_ntp_at_us;
        portEXIT_CRITICAL(&s_time_lock);
        time_t now = (time_t)(base + (esp_timer_get_time() - at) / 1000000) + 8 * 3600;
        struct tm local;
        gmtime_r(&now, &local);
        SetDigit(0, SEG_NUM[(local.tm_hour / 10) % 10]);
        SetDigit(1, SEG_NUM[local.tm_hour % 10]);
        SetDigit(2, SEG_NUM[(local.tm_min / 10) % 10]);
        SetDigit(3, SEG_NUM[local.tm_min % 10]);
        frame_[ICON_BYTE] = ICON_WIFI;  // 联网/校时成功，WiFi 图标常亮
    } else {
        // 尚未校时：这里什么都不画。空窗期画面由 RenderFrame 统一给开机配网表情，
        // 不再出现 ---- + 冒号闪的过渡态；也绝不用 time(nullptr) 兜底，
        // RTC 残留值会显示成错误时间（实测会骗过「年份判断」显示 03:50）。
        return;
    }
    // 冒号 1Hz 闪（同 v4.1）。必须在 SetDigit 之后写：dp 位会被 SetDigit 掩掉
    if (colon_on) {
        frame_[COLON_UP_BYTE] |= COLON_MASK;
        frame_[COLON_DN_BYTE] |= COLON_MASK;
    }
}

bool Tm1652Display::TimeSynced() {
    return s_sntp_synced.load();
}

void Tm1652Display::NtpTask(void* arg) {
    // 不再固定等 3 秒：一拿到 STA IP 就立刻发起 SNTP，把开机表情的空窗压到最短。
    // 已配网设备连上 WiFi 后必然能拿到 NTP，所以不需要为这几秒设计 ---- 占位画面。
    for (int i = 0; i < 100; i++) {  // 最多等 20s；无网 / 配网模式由下方重试兜底
        esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        esp_netif_ip_info_t info = {};
        if (netif && esp_netif_get_ip_info(netif, &info) == ESP_OK) {
            char ipstr[16] = {0};
            snprintf(ipstr, sizeof(ipstr), IPSTR, IP2STR(&info.ip));
            if (strcmp(ipstr, "0.0.0.0") != 0) {
                ESP_LOGI(TAG, "STA IP %s ready, start SNTP", ipstr);
                break;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    bool inited = false;
    while (true) {
        if (TimeSynced()) {
            vTaskDelay(pdMS_TO_TICKS(60000));
            continue;
        }
        if (!inited) {
            esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(NTP_SERVER);
            config.start = true;
            config.sync_cb = OnSntpSync;
            esp_err_t err = esp_netif_sntp_init(&config);
            ESP_LOGI(TAG, "SNTP init(%s): %s", NTP_SERVER, esp_err_to_name(err));
            if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) inited = true;
        } else {
            esp_netif_sntp_start();  // sntp_stop + sntp_init，重发一次请求
            ESP_LOGI(TAG, "SNTP retry: time not synced yet");
        }
        // 首次 NTP 请求给足 8s 再兜底重试。实测 3s 就调 esp_netif_sntp_start()
        // 会 stop+init 打断正在进行的请求，反而把同步拖到 8.6s（旧版 7.1s）。
        // 连上 WiFi 后正常一轮请求就能同步，这个分支只在 DNS/网络异常时才走到。
        vTaskDelay(pdMS_TO_TICKS(8000));
    }
}

const char* Tm1652Display::SceneName(Scene scene) {
    switch (scene) {
        case Scene::WifiConfig: return "wifi_config";
        case Scene::Idle:       return "idle";
        case Scene::Listening:  return "listening";
        case Scene::Speaking:   return "speaking";
        case Scene::Error:      return "error";
        case Scene::Clock:      return "clock";
        default:                return "unknown";
    }
}

Tm1652Display::Scene Tm1652Display::EffectiveScene() const {
    // 关键：不能只看 emotion 判「是否唤醒」。
    // 小智在 Idle / Connecting / Listening 下发的都是 "neutral"，
    // 只有设备状态机才能区分「待机」和「已被唤醒」。
    DeviceState state = Application::GetInstance().GetDeviceState();
    Scene mapped = static_cast<Scene>(scene_.load());

    switch (state) {
        case kDeviceStateWifiConfiguring:
        case kDeviceStateActivating:
            return Scene::WifiConfig;  // 用户要求：配网/激活都显示开机配网表情
        case kDeviceStateFatalError:
            return Scene::Error;
        case kDeviceStateListening:
            return mapped == Scene::Error ? mapped : Scene::Listening;
        case kDeviceStateSpeaking:
            return mapped == Scene::Error ? mapped : Scene::Speaking;
        case kDeviceStateIdle:
        case kDeviceStateStarting:
        case kDeviceStateConnecting:
        case kDeviceStateUnknown:
            return Scene::Clock;       // 没被唤醒：显示时钟
        default:  // Notifying / Upgrading / AudioTesting
            return Scene::Listening;
    }
}

void Tm1652Display::RenderFrame() {
    if (!Lock(20)) return;

    int64_t now_us = esp_timer_get_time();
    bool blink = ((now_us / 500000) & 1) == 0;
    bool fast_blink = ((now_us / 250000) & 1) == 0;
    Scene scene = EffectiveScene();

    if (scene != last_scene_) {
        ESP_LOGI(TAG, "scene -> %s (state=%d, time_synced=%d)",
                 SceneName(scene),
                 (int)Application::GetInstance().GetDeviceState(),
                 TimeSynced() ? 1 : 0);
        last_scene_ = scene;
    }

    ClearFrame();
    switch (scene) {
        case Scene::WifiConfig:
            // 开机配网 / 激活：与原机 v4.1 完全相同的两帧 + WiFi 图标同频闪
            DrawWifiWaitFrame(blink);
            frame_[ICON_BYTE] = blink ? ICON_WIFI : 0;
            break;

        case Scene::Clock:
            // 校时后：HH:MM + 冒号 1Hz 闪；
            // 未校时的空窗期：和上电第一帧 / 配网 / 激活同一套开机配网表情，
            // 从头到尾一个画面，不再有 ---- 占位，也不会熄灭。
            if (TimeSynced()) {
                DrawClock(blink);
            } else {
                DrawWifiWaitFrame(blink);
                frame_[ICON_BYTE] = blink ? ICON_WIFI : 0;
            }
            break;

        case Scene::Listening:
            // 思考 / 聆听：同一套上半框动画，但节奏快一倍，WiFi 图标常亮
            DrawWifiWaitFrame(fast_blink);
            frame_[ICON_BYTE] = ICON_WIFI;
            break;

        case Scene::Speaking:
            // 说话 / 开心：四位数码管做单段扫掠，像一条流动的光
            {
                uint8_t phase = (uint8_t)((now_us / 120000) % 7);
                for (int i = 0; i < DIGIT_COUNT; ++i) {
                    SetDigit(i, (uint8_t)(1 << ((phase + i) % 7)));
                }
                frame_[ICON_BYTE] = ICON_WIFI;
                frame_[COLON_DN_BYTE] |= COLON_MASK;
            }
            break;

        case Scene::Error:
            SetDigit(0, SEG_E);
            SetDigit(1, SEG_r);
            SetDigit(2, SEG_r);
            frame_[ICON_BYTE] = blink ? ICON_BELL : 0;
            break;

        case Scene::Idle:
        default:
            // 兜底同样用开机配网表情，不再画四条中横
            DrawWifiWaitFrame(blink);
            frame_[ICON_BYTE] = blink ? ICON_WIFI : 0;
            break;
    }
    Flush();
    Unlock();
}

void Tm1652Display::RefreshTask(void* arg) {
    auto* self = static_cast<Tm1652Display*>(arg);
    // 等 Application 单例构造完成：Board 是在 Application 构造过程中创建的，
    // 立刻调用 GetInstance() 会撞上尚未完成的函数内静态初始化。
    vTaskDelay(pdMS_TO_TICKS(500));
    TickType_t last_wake = xTaskGetTickCount();
    while (!self->stopping_.load()) {
        self->RenderFrame();
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(100));
    }
    vTaskDelete(nullptr);
}

Tm1652Display::Scene Tm1652Display::MapEmotion(const char* emotion) const {
    if (emotion == nullptr) return Scene::Idle;

    // 用户指定：激活(link) 与开机配网(gear) 显示同一个「开机配网表情」
    if (strcmp(emotion, "gear") == 0 || strcmp(emotion, "link") == 0 ||
        strcmp(emotion, "wifi") == 0) {
        return Scene::WifiConfig;
    }
    if (strcmp(emotion, "warning") == 0 || strcmp(emotion, "cancel") == 0 ||
        strcmp(emotion, "cloud_off") == 0 || strcmp(emotion, "sad") == 0 ||
        strcmp(emotion, "crying") == 0 || strcmp(emotion, "angry") == 0 ||
        strcmp(emotion, "confused") == 0) {
        return Scene::Error;
    }
    if (strcmp(emotion, "thinking") == 0 || strcmp(emotion, "loading") == 0 ||
        strcmp(emotion, "cloud_download") == 0 || strcmp(emotion, "download") == 0 ||
        strcmp(emotion, "sleepy") == 0 || strcmp(emotion, "listening") == 0 ||
        strcmp(emotion, "microphone") == 0) {
        return Scene::Listening;
    }
    if (strcmp(emotion, "happy") == 0 || strcmp(emotion, "laughing") == 0 ||
        strcmp(emotion, "winking") == 0 || strcmp(emotion, "surprised") == 0 ||
        strcmp(emotion, "excited") == 0 || strcmp(emotion, "cool") == 0 ||
        strcmp(emotion, "speaking") == 0) {
        return Scene::Speaking;
    }
    return Scene::Idle;  // neutral / robot_2 / staticstate / 服务端未知表情
}

// ---------------------------------------------------------------------------
// Display 接口：4 位数码管显示不了文字，状态/通知/对话只记日志，不抢场景，
// 否则 Application::Alert() 里紧跟 SetEmotion() 的激活表情会被覆盖掉。
// ---------------------------------------------------------------------------
void Tm1652Display::SetStatus(const char* status) {
    ESP_LOGI(TAG, "SetStatus: %s", status ? status : "");
}

void Tm1652Display::ShowNotification(const char* notification, int duration_ms) {
    ESP_LOGI(TAG, "Notification(%dms): %s", duration_ms, notification ? notification : "");
}

void Tm1652Display::SetChatMessage(const char* role, const char* content) {
    ESP_LOGI(TAG, "Chat[%s]: %s", role ? role : "", content ? content : "");
}

bool Tm1652Display::SetEmotion(const char* emotion) {
    Scene scene = MapEmotion(emotion);
    scene_.store(static_cast<uint8_t>(scene));
    ESP_LOGI(TAG, "SetEmotion(%s) -> scene=%d%s", emotion ? emotion : "(null)", (int)scene,
             scene == Scene::WifiConfig ? " [开机配网表情]" : "");
    return true;  // 已接受并交给渲染任务，避免上层回退到 neutral
}

void Tm1652Display::SetPowerSaveMode(bool on) {
    if (Lock(50)) {
        SetBrightness(on ? 1 : 6);
        Unlock();
    }
}
