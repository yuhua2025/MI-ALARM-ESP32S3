#!/bin/sh
# 等待式烧录：持续轮询串口，一旦板子进入下载模式（或插上原生 USB）就立刻烧 app。
#
#   sh wait_flash.sh            # 自动遍历所有 USB 串口，最长等 300 秒
#   sh wait_flash.sh /dev/cu.wchusbserial2110   # 只盯这一个口
#
# 典型用法：先跑起来，再「按住 BOOT(GPIO0) → 点一下 RESET(CHIP_PU) → 松开 BOOT」。
# 只有 esptool 自动检测出 ESP32-S3 才会 write-flash；ZMK 键盘等端口直接跳过。
set -e
cd "$(dirname "$0")"

APP="build/XiaoMiClock_v4.7_esp32s3_app.bin"
[ -f "$APP" ] || { echo "缺少 $APP，先跑 sh build.sh" >&2; exit 1; }

ONLY="$1"
WAIT="${WAIT_SEC:-300}"
DEADLINE=$(( $(date +%s) + WAIT ))
SKIP=" "          # 已排除的非目标端口
LOOPS=0

echo "等待 ESP32-S3 进入下载模式（最长 ${WAIT}s）…"
echo "  现在操作：按住 BOOT(GPIO0) → 点一下 RESET(CHIP_PU) → 松开 BOOT"
echo "  或直接把 XIAO 模组自带的 USB-C 插到电脑"
echo

candidates() {
  if [ -n "$ONLY" ]; then echo "$ONLY"; return; fi
  ls /dev/cu.usbmodem* /dev/cu.wchusbserial* /dev/cu.usbserial* 2>/dev/null || true
}

is_zmk() {
  python3 - "$1" <<'PY'
import os, select, time, sys
try:
    fd = os.open(sys.argv[1], os.O_RDONLY | os.O_NOCTTY)
except OSError:
    sys.exit(1)
buf = b''; end = time.time() + 0.3
while time.time() < end:
    r, _, _ = select.select([fd], [], [], 0.1)
    if r:
        try: buf += os.read(fd, 512)
        except OSError: break
os.close(fd)
sys.exit(0 if b'zmk' in buf.lower() else 1)
PY
}

while [ "$(date +%s)" -lt "$DEADLINE" ]; do
  LOOPS=$((LOOPS + 1))
  for p in $(candidates); do
    case "$SKIP" in *" $p "*) continue ;; esac
    if is_zmk "$p"; then
      echo "[跳过] $p 是 ZMK 键盘，不参与烧录"
      SKIP="$SKIP$p "
      continue
    fi
    for r in no-reset default-reset; do
      out=$(python3 -m esptool -p "$p" -b 115200 --before "$r" \
              --connect-attempts 1 flash-id 2>&1) || continue
      printf '%s' "$out" | grep -q "ESP32-S3" || continue
      echo
      echo ">>> 检测到 ESP32-S3：$p（--before $r），开始烧录 app"
      printf '%s\n' "$out" | grep -E "Detecting chip type|Manufacturer|Device:|Detected flash" | sed 's/^/    /'
      python3 -m esptool --chip esp32s3 -p "$p" -b 921600 --before "$r" \
        write-flash 0x10000 "$APP"
      echo
      echo ">>> 烧录完成，设备已复位。请查看 115200 串口日志。"
      exit 0
    done
  done
  # 每约 15 轮报一次心跳，避免刷屏
  if [ $((LOOPS % 12)) -eq 0 ]; then
    echo "  仍在等待…（剩余约 $(( DEADLINE - $(date +%s) )) 秒）"
  fi
done

echo "超时：${WAIT}s 内没有检测到 ESP32-S3，未写入任何数据。" >&2
exit 1
