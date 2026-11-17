#include "rf_tools.h"
#include "config.h"
#include "factory_reset.h"
#include "board.h"

#include <esp_log.h>
#include <esp_err.h>
#include <esp_wifi.h>
#include <esp_netif.h>
#include <esp_netif_ip_addr.h>
#include <esp_timer.h>
#include <nvs.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <lwip/sockets.h>
#include <arpa/inet.h>

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <strings.h>

#define TAG        "RfTools"
#define RF_PORT    3333
#define RF_BUFSZ   256

static const char* PsName(wifi_ps_type_t ps) {
    switch (ps) {
        case WIFI_PS_NONE:       return "NONE";
        case WIFI_PS_MIN_MODEM:  return "MIN_MODEM";
        case WIFI_PS_MAX_MODEM:  return "MAX_MODEM";
        default:                 return "UNKNOWN";
    }
}

// 与 Tm1652Display::SetBrightnessPercent() 保持同一映射，便于 UDP 实测核对档位
static int BrightnessLevel(int percent) {
    if (percent <= 0) return 0;
    int level = (percent * 7 + 50) / 100;
    if (level < 1) level = 1;
    if (level > 7) level = 7;
    return level;
}

// 官方配网页写入的历史值：namespace "wifi" / key "max_tx_power"，单位 0.25dBm
static int8_t LegacyTxPower() {
    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READONLY, &h) != ESP_OK) return 0;
    int8_t v = 0;
    if (nvs_get_i8(h, "max_tx_power", &v) != ESP_OK) v = 0;
    nvs_close(h);
    return v;
}

static void ClearLegacyTxPower() {
    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_erase_key(h, "max_tx_power");
    nvs_commit(h);
    nvs_close(h);
}

// 测试结论存在自己的 namespace，避免和官方配网页互相踩
static int8_t SavedBestTxPower() {
    nvs_handle_t h;
    if (nvs_open("mi_alarm", NVS_READONLY, &h) != ESP_OK) return 0;
    int8_t v = 0;
    if (nvs_get_i8(h, "rf_best", &v) != ESP_OK) v = 0;
    nvs_close(h);
    return v;
}

static bool SaveBestTxPower(int8_t quarter_dbm) {
    nvs_handle_t h;
    if (nvs_open("mi_alarm", NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t e = (quarter_dbm == 0) ? nvs_erase_key(h, "rf_best")
                                     : nvs_set_i8(h, "rf_best", quarter_dbm);
    if (e == ESP_OK || e == ESP_ERR_NVS_NOT_FOUND) e = nvs_commit(h);
    nvs_close(h);
    return e == ESP_OK;
}

// 等 WiFi 驱动 start 后，覆盖掉任何历史功率限制并关掉省电
static void ApplyDefaultPower() {
    for (int i = 0; i < 60; i++) {
        int8_t cur = 0;
        if (esp_wifi_get_max_tx_power(&cur) == ESP_OK) break;
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    int8_t legacy = LegacyTxPower();
    int8_t best = SavedBestTxPower();
    int8_t target = best != 0 ? best : RF_DEFAULT_TX_QUARTER_DBM;

    if (legacy != 0 && legacy < target) {
        ESP_LOGW(TAG, "[RF] 发现历史功率限制 NVS wifi/max_tx_power=%d (%.2f dBm)，清除",
                 legacy, legacy / 4.0);
        ClearLegacyTxPower();
    }

    esp_err_t e = esp_wifi_set_max_tx_power(target);
    wifi_ps_type_t ps = WIFI_PS_MAX_MODEM;
    esp_wifi_get_ps(&ps);
    esp_wifi_set_ps(WIFI_PS_NONE);

    int8_t actual = 0;
    esp_wifi_get_max_tx_power(&actual);
    ESP_LOGI(TAG, "[RF] 默认功率 legacy=%d best=%d target=%d (%.2f dBm) actual=%d (%.2f dBm) err=%d ps %s->NONE",
             legacy, best, target, target / 4.0, actual, actual / 4.0, (int)e, PsName(ps));
}

static void BuildStat(char* out, size_t len) {
    int8_t tx = 0;
    esp_err_t e = esp_wifi_get_max_tx_power(&tx);
    wifi_ps_type_t ps = WIFI_PS_MAX_MODEM;
    esp_wifi_get_ps(&ps);

    int rssi = 0, ch = 0;
    char ssid[33] = {0};
    wifi_ap_record_t ap = {};
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        rssi = ap.rssi;
        ch = ap.primary;
        memcpy(ssid, ap.ssid, sizeof(ap.ssid) > 32 ? 32 : sizeof(ap.ssid));
        ssid[32] = 0;
    }

    char ip[16] = "0.0.0.0";
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t info = {};
    if (netif && esp_netif_get_ip_info(netif, &info) == ESP_OK) {
        snprintf(ip, sizeof(ip), IPSTR, IP2STR(&info.ip));
    }

    snprintf(out, len,
             "OK tx=%d dbm=%.2f ps=%d psname=%s rssi=%d ch=%d ssid=%s ip=%s legacy=%d best=%d err=%d",
             (int)tx, tx / 4.0, (int)ps, PsName(ps), rssi, ch, ssid, ip,
             (int)LegacyTxPower(), (int)SavedBestTxPower(), (int)e);
}

static void DoBurst(const char* host, int port, int count, int interval_ms,
                    char* out, size_t len) {
    if (count < 1 || count > 1000 || interval_ms < 1 || interval_ms > 1000) {
        snprintf(out, len, "ERR bad burst args");
        return;
    }
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        snprintf(out, len, "ERR socket");
        return;
    }
    struct sockaddr_in dst = {};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &dst.sin_addr) != 1) {
        close(sock);
        snprintf(out, len, "ERR bad host");
        return;
    }

    int64_t t0 = esp_timer_get_time();
    char pkt[64];
    int sent = 0;
    for (int i = 0; i < count; i++) {
        int64_t ms = (esp_timer_get_time() - t0) / 1000;
        int n = snprintf(pkt, sizeof(pkt), "RF,%d,%lld", i, (long long)ms);
        if (sendto(sock, pkt, n, 0, (struct sockaddr*)&dst, sizeof(dst)) >= 0) sent++;
        vTaskDelay(pdMS_TO_TICKS(interval_ms));
    }
    close(sock);
    snprintf(out, len, "OK sent=%d/%d", sent, count);
    ESP_LOGI(TAG, "[RF] burst -> %s:%d sent=%d", host, port, sent);
}

static void RfTask(void* arg) {
    ApplyDefaultPower();

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "socket failed");
        vTaskDelete(nullptr);
        return;
    }
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(RF_PORT);
    if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        ESP_LOGE(TAG, "bind :%d failed", RF_PORT);
        close(sock);
        vTaskDelete(nullptr);
        return;
    }
    ESP_LOGI(TAG, "[RF] UDP 测试服务已启动: 0.0.0.0:%d (STAT/TXP/PS/BURST/SAVE/CLR/BRIGHT/FACTORY)", RF_PORT);

    char buf[RF_BUFSZ];
    char reply[RF_BUFSZ];
    while (true) {
        struct sockaddr_in cli = {};
        socklen_t clen = sizeof(cli);
        int n = recvfrom(sock, buf, sizeof(buf) - 1, 0, (struct sockaddr*)&cli, &clen);
        if (n <= 0) continue;
        buf[n] = 0;
        for (char* p = buf; *p; p++) {
            if (*p == '\r' || *p == '\n') { *p = 0; break; }
        }

        char cmd[32] = {0};
        if (sscanf(buf, "%31s", cmd) != 1) {
            snprintf(reply, sizeof(reply), "ERR empty");
        } else if (strcasecmp(cmd, "STAT") == 0) {
            BuildStat(reply, sizeof(reply));
        } else if (strcasecmp(cmd, "TXP") == 0) {
            int q = 0;
            if (sscanf(buf, "%*s %d", &q) == 1 && q >= 8 && q <= 84) {
                esp_err_t e = esp_wifi_set_max_tx_power((int8_t)q);
                int8_t actual = 0;
                esp_wifi_get_max_tx_power(&actual);
                snprintf(reply, sizeof(reply), "OK set=%d actual=%d dbm=%.2f err=%d",
                         q, (int)actual, actual / 4.0, (int)e);
                ESP_LOGI(TAG, "[RF] TXP -> %d (%.2f dBm), actual=%d err=%d",
                         q, q / 4.0, (int)actual, (int)e);
            } else {
                snprintf(reply, sizeof(reply), "ERR range 8..84 (2..21 dBm)");
            }
        } else if (strcasecmp(cmd, "PS") == 0) {
            int v = 0;
            if (sscanf(buf, "%*s %d", &v) == 1 && v >= 0 && v <= 2) {
                wifi_ps_type_t ps = v == 0 ? WIFI_PS_NONE
                                  : v == 1 ? WIFI_PS_MIN_MODEM : WIFI_PS_MAX_MODEM;
                esp_err_t e = esp_wifi_set_ps(ps);
                snprintf(reply, sizeof(reply), "OK ps=%d %s err=%d", v, PsName(ps), (int)e);
            } else {
                snprintf(reply, sizeof(reply), "ERR ps 0=NONE 1=MIN 2=MAX");
            }
        } else if (strcasecmp(cmd, "BURST") == 0) {
            char host[32] = {0};
            int port = 0, count = 0, interval = 0;
            if (sscanf(buf, "%*s %31s %d %d %d", host, &port, &count, &interval) == 4) {
                DoBurst(host, port, count, interval, reply, sizeof(reply));
            } else {
                snprintf(reply, sizeof(reply), "ERR usage BURST <host> <port> <count> <interval_ms>");
            }
        } else if (strcasecmp(cmd, "SAVE") == 0) {
            int q = 0;
            if (sscanf(buf, "%*s %d", &q) == 1 && q >= 8 && q <= 84) {
                bool ok = SaveBestTxPower((int8_t)q);
                snprintf(reply, sizeof(reply), "OK saved=%d (%.2f dBm) %s",
                         q, q / 4.0, ok ? "committed" : "nvs_error");
                ESP_LOGI(TAG, "[RF] 已持久化最适功率 %d (%.2f dBm)", q, q / 4.0);
            } else {
                snprintf(reply, sizeof(reply), "ERR range 8..84");
            }
        } else if (strcasecmp(cmd, "CLR") == 0) {
            SaveBestTxPower(0);
            ClearLegacyTxPower();
            snprintf(reply, sizeof(reply), "OK cleared");
        } else if (strcasecmp(cmd, "BRIGHT") == 0) {
            // 语音走的是 self.screen.set_brightness -> Backlight::SetBrightness(v, true)。
            // UDP 用完全相同的入口，便于不联网时也能实测 0~100 -> TM1652 0~7 档
            // 以及 NVS display/brightness 的持久化。TEMP 子命令只改当前亮度不落盘。
            auto* backlight = Board::GetInstance().GetBacklight();
            if (backlight == nullptr) {
                snprintf(reply, sizeof(reply), "ERR no backlight on this board");
            } else {
                char sub[16] = {0};
                int v = -1;
                int argc = 0;
                if (sscanf(buf, "%*s %15s %d", sub, &v) == 2) {
                    argc = strcasecmp(sub, "TEMP") == 0 ? 2 : -1;
                } else if (sscanf(buf, "%*s %d", &v) == 1) {
                    argc = 1;
                } else if (sscanf(buf, "%*s %15s", sub) == 1) {
                    argc = -1;
                }
                if (argc == 2 && v >= 0 && v <= 100) {
                    backlight->SetBrightness((uint8_t)v, false);
                    vTaskDelay(pdMS_TO_TICKS(550));  // 等 5ms/step 渐变走完再回报
                    snprintf(reply, sizeof(reply), "OK temp=%d level=%d current=%u persist=no",
                             v, BrightnessLevel(v), (unsigned)backlight->brightness());
                    ESP_LOGI(TAG, "[BRIGHT] 临时亮度 %d%% -> TM1652 档位 %d", v, BrightnessLevel(v));
                } else if (argc == 1 && v >= 0 && v <= 100) {
                    backlight->SetBrightness((uint8_t)v, true);
                    vTaskDelay(pdMS_TO_TICKS(550));
                    snprintf(reply, sizeof(reply), "OK set=%d level=%d current=%u persist=yes",
                             v, BrightnessLevel(v), (unsigned)backlight->brightness());
                    ESP_LOGI(TAG, "[BRIGHT] 持久亮度 %d%% -> TM1652 档位 %d (NVS display/brightness)",
                             v, BrightnessLevel(v));
                } else if (argc == 0) {
                    unsigned cur = backlight->brightness();
                    snprintf(reply, sizeof(reply), "OK current=%u level=%d (usage: BRIGHT <0..100> | BRIGHT TEMP <0..100>)",
                             cur, BrightnessLevel((int)cur));
                } else {
                    snprintf(reply, sizeof(reply), "ERR usage BRIGHT <0..100> | BRIGHT TEMP <0..100>");
                }
            }
        } else if (strcasecmp(cmd, "FACTORY") == 0) {
            // 软件触发入口：与 GPIO2 长按调用同一个 MiAlarmFactoryReset()，
            // 便于无人值守时用串口/网络验证整条链路（也方便后续远程维护）。
            // 必须带 CONFIRM，避免误发一条 UDP 就把设备清空。
            char confirm[32] = {0};
            if (sscanf(buf, "%*s %31s", confirm) == 1 &&
                strcasecmp(confirm, "CONFIRM") == 0) {
                MiAlarmFactoryReset("UDP FACTORY CONFIRM");
                snprintf(reply, sizeof(reply),
                         "OK factory reset started: voice -> erase nvs -> reboot");
            } else {
                snprintf(reply, sizeof(reply),
                         "ERR 需确认: FACTORY CONFIRM (会清除 Wi-Fi 与全部设置)");
            }
        } else {
            snprintf(reply, sizeof(reply),
                     "ERR unknown; STAT|TXP <q>|PS <0|1|2>|BURST <host> <port> <n> <ms>|SAVE <q>|CLR|BRIGHT <0..100>|BRIGHT TEMP <0..100>|FACTORY CONFIRM");
        }

        sendto(sock, reply, strlen(reply), 0, (struct sockaddr*)&cli, clen);
    }
}

void RfToolsStart() {
    xTaskCreate(RfTask, "rf_tools", 4096, nullptr, 3, nullptr);
}
