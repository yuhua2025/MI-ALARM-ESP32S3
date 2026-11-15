#!/bin/sh
# 一键回滚到小米闹钟 Arduino v4.1（8MB merged 全片镜像，含 NVS 格式重置）
# 用法：./rollback_v41.sh [串口] --yes
set -e

PORT="${1:-/dev/cu.usbmodem21101}"
CONFIRM="${2:-}"
BAK_DIR="/Volumes/macmini/DIY/小米闹钟/MI-ALARM-ESP32S3/firmware_backup/v4.1_20261115_1834"
IMG="$BAK_DIR/XiaoMiClock_v4.1_esp32s3_merged.bin"
EXPECT_SHA="5e7ce0a3380c44c7d08dcbe783b5cc57c54a1a711ddd6f820652e39f20d4dcb7"

if [ ! -f "$IMG" ]; then
  echo "备份镜像不存在：$IMG"
  exit 1
fi

ACTUAL_SHA=$(shasum -a 256 "$IMG" | awk '{print $1}')
if [ "$ACTUAL_SHA" != "$EXPECT_SHA" ]; then
  echo "备份镜像 SHA-256 不匹配，拒绝烧录！"
  echo "  期望 $EXPECT_SHA"
  echo "  实际 $ACTUAL_SHA"
  exit 1
fi

echo "目标串口 : $PORT"
echo "回滚镜像 : $IMG"
echo "SHA-256  : $ACTUAL_SHA (校验通过)"
echo
echo "注意：回滚会写回 8MB 分区表，小智的 16MB 分区/NVS/assets 全部被覆盖。"
echo "      回滚后需要重新配网（AP 热点 192.168.4.1）并重新设置闹钟参数。"
echo

if [ "$CONFIRM" != "--yes" ]; then
  echo "确认无误后执行： ./rollback_v41.sh $PORT --yes"
  exit 1
fi

python3 -m esptool --chip esp32s3 -p "$PORT" -b 921600 \
  --before default-reset --after hard-reset write-flash 0x0 "$IMG"

echo "回滚完成。设备应重新出现 v4.1 的配网热点。"
