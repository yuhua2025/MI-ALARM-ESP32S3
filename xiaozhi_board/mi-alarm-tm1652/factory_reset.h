#pragma once

/*
 * 恢复出厂设置：播报中文「重置成功」→ 清除 Wi-Fi 配网与全部 NVS 设置 → 重启进配网。
 *
 * 按键长按与 UDP 测试命令共用这一个入口，保证两条触发路径行为完全一致。
 * source 只用于串口日志，方便区分触发来源。
 */
void MiAlarmFactoryReset(const char* source);
