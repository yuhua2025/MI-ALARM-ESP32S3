#include "factory_reset.h"

#include "application.h"
#include "assets/lang_config.h"

#include <esp_log.h>
#include <esp_err.h>
#include <esp_wifi.h>
#include <esp_system.h>
#include <esp_partition.h>
#include <nvs_flash.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>

#define TAG "FactoryReset"

namespace {

std::atomic<bool> s_in_progress{false};

// 兜底：万一 nvs_flash_erase 因残留句柄失败，直接按分区擦掉 nvs
static esp_err_t EraseNvsPartitionRaw() {
    const esp_partition_t* part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, "nvs");
    if (part == nullptr) {
        return ESP_ERR_NOT_FOUND;
    }
    return esp_partition_erase_range(part, 0, part->size);
}

static void ResetTask(void*) {
    // 本地「重置成功」语音约 1.17 秒。先给足最小播放时间，再等解码队列排空，
    // 否则擦 NVS / 重启会把提示音掐断，用户只听到半句。
    vTaskDelay(pdMS_TO_TICKS(1800));
    auto& audio = Application::GetInstance().GetAudioService();
    int waited = 0;
    bool drained = audio.IsPlaybackIdle();
    for (; waited < 30 && !drained; ++waited) {
        vTaskDelay(pdMS_TO_TICKS(100));
        drained = audio.IsPlaybackIdle();
    }
    ESP_LOGI(TAG, "[FACTORY] 解码队列 drained=%d (额外等待 %d x100ms)",
             drained ? 1 : 0, waited);
    ESP_LOGW(TAG, "[FACTORY] 语音播报完成，停 Wi-Fi 并清除 NVS");

    // 先停 Wi-Fi，避免驱动在擦除后把当前凭据重新写回 NVS
    esp_wifi_disconnect();
    esp_wifi_stop();
    vTaskDelay(pdMS_TO_TICKS(200));

    esp_err_t deinit = nvs_flash_deinit();
    ESP_LOGI(TAG, "[FACTORY] nvs_flash_deinit -> %d (%s)",
             (int)deinit, esp_err_to_name(deinit));

    esp_err_t erase = nvs_flash_erase();
    if (erase != ESP_OK) {
        ESP_LOGW(TAG, "[FACTORY] nvs_flash_erase 失败 (%d)，改用分区直擦", (int)erase);
        erase = EraseNvsPartitionRaw();
    }
    ESP_LOGW(TAG, "[FACTORY] NVS erase -> %d (%s)：Wi-Fi 与全部设置已清除",
             (int)erase, esp_err_to_name(erase));

    vTaskDelay(pdMS_TO_TICKS(200));
    ESP_LOGW(TAG, "[FACTORY] esp_restart");
    esp_restart();
}

}  // namespace

void MiAlarmFactoryReset(const char* source) {
    if (s_in_progress.exchange(true)) {
        ESP_LOGW(TAG, "[FACTORY] 已在执行中，忽略重复触发 (%s)", source ? source : "?");
        return;
    }
    ESP_LOGW(TAG, "[FACTORY] 来源=%s：播报「重置成功」→ 清 NVS → 重启进配网",
             source ? source : "?");

    // PlaySound 放到主任务执行，和官方 Alert/提示音走同一条路径
    Application::GetInstance().Schedule([]() {
        auto& app = Application::GetInstance();
        ESP_LOGI(TAG, "[FACTORY] PlaySound(OGG_RESET_SUCCESS) ogg_bytes=%zu",
                 Lang::Sounds::OGG_RESET_SUCCESS.size());
        app.PlaySound(Lang::Sounds::OGG_RESET_SUCCESS);
        ESP_LOGI(TAG, "[FACTORY] 推入解码队列后 idle=%d",
                 app.GetAudioService().IsPlaybackIdle() ? 1 : 0);
        xTaskCreate(ResetTask, "factory_reset", 4096, nullptr, 5, nullptr);
    });
}
