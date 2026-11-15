#!/bin/sh
# 小米闹钟 ESP32-S3 固件：编译 → 契约自检 → 导出发布 bin → 打印校验和
# 用法：sh build.sh
set -e
cd "$(dirname "$0")"

FQBN="esp32:esp32:XIAO_ESP32S3"
VER="v4.1"

# --build-path build 不能省：不带它产物会落到 ~/Library/Caches/arduino/sketches/，
# build/ 里还是上一次的旧 bin，曾经因此把旧固件烧进去。
arduino-cli compile --fqbn "$FQBN" --build-path build .

python3 verify_contract.py

cp build/ino.ino.bin        "build/XiaoMiClock_${VER}_esp32s3_app.bin"
cp build/ino.ino.merged.bin "build/XiaoMiClock_${VER}_esp32s3_merged.bin"
shasum -a 256 "build/XiaoMiClock_${VER}_esp32s3_app.bin" \
              "build/XiaoMiClock_${VER}_esp32s3_merged.bin"
