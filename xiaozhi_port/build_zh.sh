#!/bin/bash
# 用中文（zh-CN）构建小智固件。
# 注意：scripts/build.py 不带 --language 时默认 en-US，提示音/文案/唤醒词全是英文。
# 中文默认唤醒词会变成「你好小智」(wn9_nihaoxiaozhi_tts)。
set -e

PROJ="/Volumes/macmini/DIY/xiaozhi-esp32"

source "$HOME/esp/esp-idf/export.sh" >/dev/null 2>&1
cd "$PROJ"
python3 -u scripts/build.py mi-alarm-tm1652 --language zh-CN

echo
echo "中文固件已生成："
echo "  $PROJ/build/merged-binary.bin"
echo "  $PROJ/build/generated_assets.bin  (中文 OGG 提示音 + 你好小智唤醒词)"
echo "烧录：./flash_xiaozhi.sh /dev/cu.usbmodem21101 --yes"
