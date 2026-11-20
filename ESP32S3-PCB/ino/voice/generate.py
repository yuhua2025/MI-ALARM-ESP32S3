#!/usr/bin/env python3
"""生成整点报时语音并导出固件可直接编译的 C 数组头文件（v4.3）。

v4.2 是把 13 段碎片（现在时间 / 0~9 / 十 / 点整）在设备端拼接，
听起来生硬：每段独立语调 + 首尾静音，段与段之间有明显停顿。
v4.3 改为**每个整点预生成一整句**：Tingting 一次读完
「现在时间十三点整」，语调连贯、无拼接停顿；到点直接播对应那一段。

0~23 共 24 句。采样率用 16000Hz（语音清晰可懂，24 段约 1.6MB，
编译进 app 后总量约 2.6MB，稳在 3.19MB app 分区内；22050Hz 会顶到上限）。

输出：
  voice/hour_0.wav ~ hour_23.wav   16000Hz / 16bit / mono，首尾加短静音
  chime_voice.h                    供 ino.ino include 的字节数组

重新生成：python3 voice/generate.py
"""
import array
import os
import subprocess
import tempfile
import wave
from pathlib import Path

HERE = Path(__file__).resolve().parent
RATE = 16000
HEAD_MS = 100       # 句首静音：避免音头爆音
TAIL_MS = 180       # 句尾静音：留一点余韵，不急促
PEAK = 12000        # 归一化峰值，MAX98357 固定增益下不削波
VOICE = "Tingting"  # macOS 中文女声；想换音色改这里重生成
SPEECH_RATE = None  # None = 系统默认语速；报时想更慢更清晰可设为 170 之类


def hour_text(h: int) -> str:
    """0~23 点的中文读法：0→零，10→十，11~19→十一~十九，20~23→二十~二十三。"""
    d = "零一二三四五六七八九"
    if h < 10:
        num = d[h]
    elif h == 10:
        num = "十"
    elif h < 20:
        num = "十" + (d[h - 10] if h > 10 else "")
    else:
        num = d[2] + "十" + (d[h - 20] if h > 20 else "")
    return f"现在时间{num}点整"


def synth(text: str, wav_path: Path) -> None:
    cmd = ["say", "-v", VOICE, "-o", str(wav_path).replace(".wav", ".aiff"), text]
    if SPEECH_RATE is not None:
        cmd = ["say", "-v", VOICE, "-r", str(SPEECH_RATE),
               "-o", str(wav_path).replace(".wav", ".aiff"), text]
    subprocess.run(cmd, check=True, capture_output=True)
    aiff = Path(str(wav_path).replace(".wav", ".aiff"))
    subprocess.run(
        ["afconvert", "-f", "WAVE", "-d", f"LEI16@{RATE}", "-c", "1",
         str(aiff), str(wav_path)],
        check=True, capture_output=True,
    )
    aiff.unlink(missing_ok=True)


def pad_and_normalize(src: Path, dst: Path) -> None:
    with wave.open(str(src), "rb") as w:
        assert w.getnchannels() == 1, w.getnchannels()
        assert w.getsampwidth() == 2, w.getsampwidth()
        assert w.getframerate() == RATE, w.getframerate()
        raw = w.readframes(w.getnframes())
    samples = array.array("h")
    samples.frombytes(raw)
    if samples:
        peak = max(1, max(abs(int(s)) for s in samples))
        scale = min(1.0, PEAK / peak)
        samples = array.array("h", (int(s * scale) for s in samples))
    head = int(RATE * HEAD_MS / 1000)
    tail = int(RATE * TAIL_MS / 1000)
    out = array.array("h", [0] * head) + samples + array.array("h", [0] * tail)
    with wave.open(str(dst), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(out.tobytes())


def to_c_array(cname: str, data: bytes) -> str:
    lines = [f"static const uint8_t {cname}[] = {{"]
    for i in range(0, len(data), 16):
        lines.append("  " + ",".join(f"0x{b:02X}" for b in data[i:i + 16]) + ",")
    lines.append("};")
    lines.append(f"#define {cname}_LEN {len(data)}")
    return "\n".join(lines)


def main() -> None:
    header = [
        "// 自动生成，勿手工编辑。重新生成：python3 voice/generate.py",
        f"// 整点报时语音：{RATE}Hz / 16bit / mono，每整点一整句，首尾加短静音。",
        "// v4.3：24 段整句（hour_0~hour_23），不再在设备端拼接碎片。",
        "#pragma once",
        "#include <cstdint>",
        "",
    ]
    total = 0
    for h in range(24):
        text = hour_text(h)
        with tempfile.TemporaryDirectory() as td:
            raw = Path(td) / "raw.wav"
            synth(text, raw)
            final = HERE / f"hour_{h}.wav"
            pad_and_normalize(raw, final)
        data = final.read_bytes()
        total += len(data)
        cname = f"VOICE_H{h}"
        print(f"{h:>2}点  {text:10s} {len(data):7d} bytes  {final.name}")
        header.append(to_c_array(cname, data))
        header.append("")

    out = HERE.parent / "chime_voice.h"
    out.write_text("\n".join(header), encoding="utf-8")

    # 清掉 v4.2 的 13 段碎片，避免和新整句混淆
    legacy = ["prefix", "ten", "oclock"] + [str(i) for i in range(10)]
    removed = 0
    for name in legacy:
        p = HERE / f"{name}.wav"
        if p.exists():
            p.unlink()
            removed += 1

    print(f"\n合计 {total} bytes -> {out}")
    print(f"WAV 总时长约 {total / (RATE * 2):.1f} 秒")
    print(f"已清理 {removed} 个旧碎片 WAV")


if __name__ == "__main__":
    main()
