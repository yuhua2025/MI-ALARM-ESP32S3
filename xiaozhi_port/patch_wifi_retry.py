#!/usr/bin/env python3
"""幂等补丁：加长 esp-wifi-connect 配网测试连接的重试窗口。

背景：小智配网走 APSTA，提交时 STA 要切到家里 AP 的信道，路由器回
status=30 (Refused Temporarily) + Comeback Time。官方只等 3s 重试一次，
本板（尤其是恢复出厂后 RF 刚全校准）实测需要 ~8s 路由器才稳定，
所以用户必须点第二次提交才成功。改成 3 次 / 8s 后一次即可。

managed_components/ 由 IDF 组件管理器托管，`idf.py reconfigure` 有可能
覆盖，故每次构建前跑本脚本重新应用（幂等）。
"""
from pathlib import Path
import sys

TARGET = Path("/Volumes/macmini/DIY/xiaozhi-esp32/managed_components/"
              "78__esp-wifi-connect/wifi_configuration_ap.cc")

OLD = """    constexpr int kMaxAttempts = 2;
    constexpr int kRetryDelayMs = 3000;"""
NEW = """    // mi-alarm-tm1652 实测：恢复出厂后首次配网仍需 ~8s 路由器才稳定，
    // 官方的 3s 重试不够，用户必须手动点第二次。这里放宽到 3 次 / 8s，
    // 让一次提交就能成功（代价是最坏情况多等 8~16s，配网场景可接受）。
    constexpr int kMaxAttempts = 3;
    constexpr int kRetryDelayMs = 8000;"""

# 已打过补丁时的形态（含注释）
PATCHED = """    constexpr int kMaxAttempts = 3;
    constexpr int kRetryDelayMs = 8000;"""


def main() -> int:
    if not TARGET.is_file():
        print(f"找不到 {TARGET}", file=sys.stderr)
        return 1
    text = TARGET.read_text(encoding="utf-8")
    if OLD in text:
        TARGET.write_text(text.replace(OLD, NEW, 1), encoding="utf-8")
        print("已应用配网重试补丁：3 次 / 8s")
    elif PATCHED in text or NEW in text:
        print("配网重试补丁已在位，跳过")
    else:
        print("未找到目标代码片段，请检查组件版本是否变化", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
