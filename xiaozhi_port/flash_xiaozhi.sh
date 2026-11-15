#!/bin/sh
# 把小智固件（16MB merged 全片镜像）烧到小米闹钟自画裸片板
# 用法：./flash_xiaozhi.sh [串口] --yes
set -e

PORT="${1:-/dev/cu.usbmodem21101}"
CONFIRM="${2:-}"
PROJ="/Volumes/macmini/DIY/xiaozhi-esp32"

IMG=""
for f in "$PROJ/build/merged-binary.bin" "$PROJ/build/merged-flash.bin"; do
  if [ -f "$f" ]; then IMG="$f"; break; fi
done

if [ -z "$IMG" ]; then
  echo "找不到合并镜像。先编译："
  echo "  source \$HOME/esp/esp-idf/export.sh"
  echo "  cd $PROJ && python3 scripts/build.py mi-alarm-tm1652"
  exit 1
fi

echo "目标串口 : $PORT"
echo "镜像     : $IMG"
echo "镜像大小 : $(stat -f%z "$IMG") bytes"
echo
echo "注意：小智用 16MB v2 分区表（app @0x20000, assets @0x800000），"
echo "      与 Arduino v4.1 的 8MB 分区表完全不同。全片烧录会清空"
echo "      原 NVS 设置、Wi-Fi 配网记录和 SPIFFS 铃声库。"
echo

if [ "$CONFIRM" != "--yes" ]; then
  echo "确认无误后执行： ./flash_xiaozhi.sh $PORT --yes"
  exit 1
fi

python3 -m esptool --chip esp32s3 -p "$PORT" -b 921600 \
  --before default-reset --after hard-reset write-flash 0x0 "$IMG"

echo "烧录完成。观察串口日志：python3 -m serial.tools.miniterm $PORT 115200"
