#!/bin/sh
# 小米闹钟 ESP32-S3 固件：编译 → 契约自检 → 导出发布 bin → 打印校验和
# 用法：sh build.sh
set -e
cd "$(dirname "$0")"

FQBN="esp32:esp32:XIAO_ESP32S3"
VER="v4.8.12"

# 本板 Flash 是 16MB（esptool flash_id 实测 Detected flash size: 16MB），
# 但 XIAO_ESP32S3 这个板型的菜单里只给了 8MB 方案（app 只有 3.34MB）。
# 这里直接覆盖三个 build 属性切到 16MB 布局：
#   app0  0x10000  0x640000 = 6.55MB   ← 放得下真人语音 + 真实 WAV 铃声
#   app1  0x650000 0x640000            ← OTA 备份区
#   spiffs 0xc90000 0x360000 = 3.54MB
# upload.maximum_size 也要一起改，否则 arduino-cli 会按 3.34MB 判超限。
# 注意：**不要**加 CDCOnBoot=cdc —— 这个板型加了等于关掉 USB CDC 日志。
PART_OPTS="--build-property build.flash_size=16MB \
--build-property build.partitions=default_16MB \
--build-property upload.maximum_size=6553600"

# --build-path build 不能省：不带它产物会落到 ~/Library/Caches/arduino/sketches/，
# build/ 里还是上一次的旧 bin，曾经因此把旧固件烧进去。
arduino-cli compile --fqbn "$FQBN" $PART_OPTS --build-path build .

python3 verify_contract.py

cp build/ino.ino.bin        "build/XiaoMiClock_${VER}_esp32s3_app.bin"
cp build/ino.ino.merged.bin "build/XiaoMiClock_${VER}_esp32s3_merged.bin"
shasum -a 256 "build/XiaoMiClock_${VER}_esp32s3_app.bin" \
              "build/XiaoMiClock_${VER}_esp32s3_merged.bin"
