#include "factory_reset.h"

#include "application.h"
#include "assets/lang_config.h"

#include <esp_log.h>
#include <esp_err.h>
#include <esp_wifi.h>
#include <esp_system.h>
#include <esp_partition.h>
#include <nvs_flash.h>
#include <nvs.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>
#include <cstring>
#include <vector>

#define TAG "FactoryReset"

namespace {

std::atomic<bool> s_in_progress{false};

// RF 校准数据存在 NVS 的 "phy" namespace（cal_data / cal_mac / cal_version）。
// 如果恢复出厂把它一起擦掉，重启后首次上电会走 full calibration，
// Wi-Fi 链路不稳定，正好撞上配网时路由器的 Comeback Time 窗口，
// 用户就得「配两次才成功」。所以擦 NVS 前先把它备份到内存，擦完写回。
struct PhyEntry {
    char key[NVS_KEY_NAME_MAX_SIZE];
    nvs_type_t type;
    uint32_t u32;
    std::vector<uint8_t> blob;
};

static std::vector<PhyEntry> BackupPhyCal() {
    std::vector<PhyEntry> out;
    nvs_iterator_t it = nullptr;
    esp_err_t e = nvs_entry_find(NVS_DEFAULT_PART_NAME, "phy", NVS_TYPE_ANY, &it);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "[FACTORY] 无 phy namespace，跳过校准数据备份 (%d)", (int)e);
        return out;
    }
    nvs_handle_t h;
    if (nvs_open("phy", NVS_READONLY, &h) != ESP_OK) {
        nvs_release_iterator(it);
        return out;
    }
    while (it != nullptr) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);
        // 只保留公开类型；blob 的分片 entry 类型是内部值，跳过它们
        if (info.type == NVS_TYPE_U32 || info.type == NVS_TYPE_BLOB) {
            PhyEntry item = {};
            strncpy(item.key, info.key, sizeof(item.key) - 1);
            item.type = info.type;
            if (info.type == NVS_TYPE_U32) {
                nvs_get_u32(h, info.key, &item.u32);
            } else {
                size_t len = 0;
                if (nvs_get_blob(h, info.key, nullptr, &len) == ESP_OK && len > 0) {
                    item.blob.resize(len);
                    nvs_get_blob(h, info.key, item.blob.data(), &len);
                }
            }
            out.push_back(std::move(item));
        }
        nvs_entry_next(&it);
    }
    nvs_release_iterator(it);
    nvs_close(h);
    ESP_LOGI(TAG, "[FACTORY] 已备份 phy 校准数据 %d 项", (int)out.size());
    return out;
}

static void RestorePhyCal(const std::vector<PhyEntry>& items) {
    if (items.empty()) {
        return;
    }
    esp_err_t e = nvs_flash_init();
    ESP_LOGI(TAG, "[FACTORY] nvs_flash_init -> %d (%s)", (int)e, esp_err_to_name(e));
    if (e != ESP_OK) {
        return;
    }
    nvs_handle_t h;
    if (nvs_open("phy", NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "[FACTORY] 打开 phy namespace 失败，校准数据未恢复");
        return;
    }
    for (const auto& item : items) {
        if (item.type == NVS_TYPE_U32) {
            nvs_set_u32(h, item.key, item.u32);
        } else {
            nvs_set_blob(h, item.key, item.blob.data(), item.blob.size());
        }
    }
    e = nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "[FACTORY] 恢复 phy 校准数据 %d 项 commit=%d", (int)items.size(), (int)e);
}

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

    // 备份必须在 nvs_flash_deinit() 之前做，deinit 后句柄全部失效
    auto phy_backup = BackupPhyCal();

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

    // 只把 RF 校准数据写回；Wi-Fi 凭据/音量/射频档位等业务设置仍然全清
    RestorePhyCal(phy_backup);

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
