#!/bin/sh
# 只烧小智 app 分区（ota_0 @0x20000），保留 NVS / Wi-Fi 配网 / assets 中文语音包。
# 日常迭代用这个；只有从 Arduino v4.1 互刷或分区表损坏时才用 flash_xiaozhi.sh 全片烧。
# 用法：./flash_app.sh [串口]
set -e

PORT="${1:-/dev/cu.usbmodem21101}"
PROJ="/Volumes/macmini/DIY/xiaozhi-esp32"
APP="$PROJ/build/xiaozhi.bin"

if [ ! -f "$APP" ]; then
  echo "找不到 $APP，先跑 ./build_zh.sh"
  exit 1
fi

echo "目标串口 : $PORT"
echo "app 镜像 : $APP ($(stat -f%z "$APP") bytes) @ 0x20000"
echo "保留      : NVS / Wi-Fi 配网 / assets"
echo

python3 -m esptool --chip esp32s3 -p "$PORT" -b 921600 \
  --before default-reset --after hard-reset write-flash 0x20000 "$APP"

echo "烧录完成。等待设备重启后可用 python3 rf_sweep.py 做功率实测。"
