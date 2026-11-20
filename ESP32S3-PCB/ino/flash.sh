#!/bin/sh
# 小米闹钟 ESP32-S3 烧录（带芯片身份校验，防止误刷别的 USB 串口设备）
#
#   sh flash.sh                              # 自动找 ESP32-S3，烧 app（0x10000，保留设置）
#   sh flash.sh /dev/cu.usbmodemXXXX         # 指定端口
#   sh flash.sh /dev/cu.usbmodemXXXX --full # 烧 16MB 全片镜像（清 NVS/设置）
#   sh flash.sh /dev/cu.usbmodemXXXX --part # 换引导+分区表+app（保留 NVS/设置）
#
# 安全规则：**只有 esptool 自动检测出该端口是 ESP32-S3 才会执行 write-flash**。
# 本机 /dev/cu.usbmodem312401 是 ZMK 键盘 dongle（Dayu k30 Dongle），
# 它在持续刷 BLE 日志，脚本会先认出来并跳过，绝不能被当成目标板刷掉。
set -e
cd "$(dirname "$0")"

APP="build/XiaoMiClock_v4.8.12_esp32s3_app.bin"
FULL="build/XiaoMiClock_v4.8.12_esp32s3_merged.bin"
BOOT="build/ino.ino.bootloader.bin"
PART="build/ino.ino.partitions.bin"
BOOTAPP="build/boot_app0.bin"

PORT=""
MODE="app"
for arg in "$@"; do
  case "$arg" in
    --full) MODE="full" ;;
    --part) MODE="part" ;;
    *)      PORT="$arg" ;;
  esac
done

# 读一小段串口输出，识别 ZMK 键盘等非目标设备
is_zmk() {
  python3 - "$1" <<'PY'
import os, select, time, sys
fd = os.open(sys.argv[1], os.O_RDONLY | os.O_NOCTTY)
buf = b''
end = time.time() + 0.5
while time.time() < end:
    r, _, _ = select.select([fd], [], [], 0.1)
    if r:
        try:
            buf += os.read(fd, 1024)
        except OSError:
            break
os.close(fd)
sys.exit(0 if b'zmk' in buf.lower() else 1)
PY
}

# 自动检测芯片：成功且输出里出现 ESP32-S3 才算数，并把探测结果打到 stdout
probe() {
  out=$(python3 -m esptool -p "$1" -b 115200 --before "$2" \
          --connect-attempts 1 flash-id 2>&1) || return 1
  printf '%s' "$out" | grep -q "ESP32-S3" || return 1
  printf '%s' "$out"
}

FOUND=""
RESET=""
CHIPINFO=""
try_port() {
  p="$1"
  if is_zmk "$p"; then
    printf '  %-36s -> ZMK 设备（跳过，禁止烧录）\n' "$p"
    return
  fi
  printf '  %-36s ' "$p"
  for r in default-reset usb-reset; do
    if info=$(probe "$p" "$r"); then
      FOUND="$p"; RESET="$r"; CHIPINFO="$info"
      echo "-> ESP32-S3 OK (--before $r)"
      return
    fi
  done
  echo "-> 无响应 / 非 ESP32-S3（跳过）"
}

if [ -z "$PORT" ]; then
  echo "正在自动探测 ESP32-S3 端口…"
  for p in /dev/cu.usbmodem* /dev/cu.wchusbserial* /dev/cu.usbserial* /dev/cu.SLAB_USBtoUART*; do
    [ -e "$p" ] || continue
    try_port "$p"
    [ -n "$FOUND" ] && break
  done
else
  echo "校验指定端口 $PORT …"
  try_port "$PORT"
fi

if [ -z "$FOUND" ]; then
  echo
  echo "没有找到可烧录的 ESP32-S3，未写入任何数据。逐项检查："
  echo "  1) 用 XIAO 模组自带的 USB-C 直连电脑（推荐，原生 USB CDC，不需 CH340）"
  echo "  2) 若走外接 USB-TTL：确认板子已上电、TX↔RX 交叉、共地"
  echo "  3) 手动进下载模式：按住 BOOT(GPIO0) → 点一下 RESET(CHIP_PU) → 松开 BOOT"
  echo "  4) 当前端口列表："
  ls /dev/cu.* 2>/dev/null | sed 's/^/     /'
  exit 1
fi

PORT="$FOUND"
echo
printf '%s\n' "$CHIPINFO" | grep -E "Detecting chip type|Manufacturer|Device:|Detected flash" | sed 's/^/  /'

if [ "$MODE" = "full" ]; then
  [ -f "$FULL" ] || { echo "缺少 $FULL，先跑 sh build.sh" >&2; exit 1; }
  echo
  echo "!! 全片烧录会清空 NVS（WiFi 与全部设置）!!  端口=$PORT  镜像=$FULL"
  python3 -m esptool --chip esp32s3 -p "$PORT" -b 921600 --before "$RESET" \
    write-flash 0x0 "$FULL"
elif [ "$MODE" = "part" ]; then
  # 分区表换布局（8MB → 16MB）时必须连引导一起烧：
  #   0x0    bootloader   引导里编着 flash 容量，旧的是按 8MB 编的
  #   0x8000 partitions   新的 16MB 分区表
  #   0xe000 boot_app0    OTA 选择器占位（全 0xFF = 直接引导 ota_0）
  #   0x10000 app
  # 这四段都落在 0x9000(NVS) 之外，所以 Wi-Fi 和全部设置原样保留。
  for f in "$BOOT" "$PART" "$BOOTAPP" "$APP"; do
    [ -f "$f" ] || { echo "缺少 $f，先跑 sh build.sh" >&2; exit 1; }
  done
  echo
  echo "烧录 引导+分区表+app（保留 NVS）  端口=$PORT"
  python3 -m esptool --chip esp32s3 -p "$PORT" -b 921600 --before "$RESET" \
    write-flash 0x0 "$BOOT" 0x8000 "$PART" 0xe000 "$BOOTAPP" 0x10000 "$APP"
else
  [ -f "$APP" ] || { echo "缺少 $APP，先跑 sh build.sh" >&2; exit 1; }
  echo
  echo "烧录 app（保留设置）  端口=$PORT  镜像=$APP  偏移=0x10000"
  python3 -m esptool --chip esp32s3 -p "$PORT" -b 921600 --before "$RESET" \
    write-flash 0x10000 "$APP"
fi

echo
echo "复位设备…"
python3 -m esptool --chip esp32s3 -p "$PORT" --after hard-reset flash-id 2>&1 | tail -6
echo
echo "完成。下一步："
echo "  串口 115200 看 [RGB]/[SPK]/[MIC]/[BOOT]/[AP]/[NTP] 开机日志"
echo "  配网后 curl http://<设备IP>/api/status 与 /api/mic 做实测"
