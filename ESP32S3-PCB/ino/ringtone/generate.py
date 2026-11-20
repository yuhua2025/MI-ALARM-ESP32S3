#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
小米闹钟 · 闹钟铃声离线合成器（v4.5）

为什么要离线合成：
  老固件用「方波 + 音符表」在设备上现算，只有基频和奇次谐波，听起来又干又尖，
  像老式电子表。真正手机铃声的「层次感」来自三件事，方波都给不了：
    1) 一个音里同时有多个分音，各自衰减速度不同（钟声/木琴就是这样）
    2) 每一下有音量包络（音头快、尾巴自然衰减），不是硬开关
    3) 空间感（混响、早期反射）
  这些在 ESP32 上实时算太吃 CPU，所以在电脑上离线合成成 WAV，
  直接编进固件 flash，设备端只做「读 flash → 重采样 → 出声」。

输出：
  ringtone/*.wav          每段 8 秒，22050Hz / 16bit / 单声道，可循环无缝
  ringtone/ringtone_data.h  给固件用的 C 数组

无缝循环的做法：所有音符和混响都写入同一个「环形缓冲」，
尾巴自动绕回开头，所以首尾天然接得上，不会每 8 秒「咔」一下。
"""

import math
import os
import wave
import numpy as np

RATE = 22050
LOOP_SEC = 8.0
N = int(RATE * LOOP_SEC)
PEAK = 12000          # 与整点报时语音一致（12000/32767 ≈ 37% 满量程）

# ---------------------------------------------------------------- 音高
_SEMI = {'C': 0, 'C#': 1, 'D': 2, 'D#': 3, 'E': 4, 'F': 5, 'F#': 6,
         'G': 7, 'G#': 8, 'A': 9, 'A#': 10, 'B': 11}


def hz(name):
    """'C5' -> 523.25，A4 = 440Hz"""
    pitch, octave = name[:-1], int(name[-1])
    return 440.0 * 2 ** ((_SEMI[pitch] + (octave - 4) * 12 - 9) / 12.0)


# ---------------------------------------------------------------- 音色
# 每个分音：(频率比, 相对幅度, 衰减时间常数秒)；tau 很大 = 不衰减（持续音）
BELL = [(1.00, 1.00, 2.40), (2.00, 0.50, 1.50), (2.76, 0.30, 1.00),
        (4.07, 0.18, 0.60), (5.43, 0.10, 0.40), (8.10, 0.05, 0.22)]
MALLET = [(1.00, 1.00, 0.90), (2.00, 0.14, 0.45), (3.93, 0.32, 0.22),
          (9.20, 0.07, 0.08)]
PLUCK = [(1.0, 1.00, 0.70), (2.0, 0.45, 0.40), (3.0, 0.22, 0.25),
         (4.0, 0.12, 0.15), (5.0, 0.07, 0.10)]
PULSE = [(1.0, 1.00, 0.11), (3.0, 0.34, 0.06), (5.0, 0.20, 0.04),
         (7.0, 0.12, 0.03)]
ORGAN = [(1.0, 1.00, 9e9), (2.0, 0.52, 9e9), (3.0, 0.33, 9e9),
         (4.0, 0.20, 9e9), (5.0, 0.11, 9e9)]
SUB = [(1.0, 1.00, 0.36), (2.0, 0.16, 0.20)]


# ---------------------------------------------------------------- 环形缓冲
def add_circ(buf, start, sig):
    """把 sig 叠加到 buf 的 start 处，超出末尾自动绕回开头"""
    if len(sig) == 0:
        return
    s = start % N
    n = len(sig)
    if s + n <= N:
        buf[s:s + n] += sig
    else:
        k = N - s
        buf[s:] += sig[:k]
        buf[:n - k] += sig[k:]


def render_note(buf, t0, freq, dur, timbre, amp,
                atk=0.005, rel=0.35, detune_cents=0.0):
    """合成一个音：多分音 + 各自衰减 + 音头音尾包络"""
    n = int(dur * RATE)
    if n < 16:
        return
    t = np.arange(n, dtype=np.float64) / RATE
    env = np.minimum(1.0, t / max(atk, 1e-4))
    if rel > 0:
        env *= np.clip((dur - t) / rel, 0.0, 1.0)

    sig = np.zeros(n)
    k = 2 ** (detune_cents / 1200.0)
    for ratio, pa, tau in timbre:
        f = freq * ratio * k
        if f > RATE * 0.45:          # 超过奈奎斯特就丢掉，避免混叠成噪声
            continue
        aenv = np.exp(-t / tau) if tau < 1e6 else 1.0
        # 给每个分音一个相位偏移，避免所有分音在 t=0 同相叠加成爆音
        ph = 0.37 * ratio
        sig += pa * aenv * np.sin(2 * math.pi * f * t + ph)
    add_circ(buf, int(t0 * RATE), sig * env * amp)


def add_pad(buf, freqs, amp, breaths=1, detune_cents=5.0):
    """铺底持续音：整段都在，幅度用余弦缓慢起伏，因此首尾天然接得上"""
    t = np.arange(N) / RATE
    env = 0.55 + 0.45 * np.cos(2 * math.pi * breaths * t / LOOP_SEC - math.pi)
    sig = np.zeros(N)
    for f in freqs:
        for c in (-detune_cents, detune_cents):
            k = 2 ** (c / 1200.0)
            sig += np.sin(2 * math.pi * f * k * t + 0.3)
    sig *= env * amp / (2 * len(freqs))
    buf += sig


# ---------------------------------------------------------------- 混响
def reverb(buf, wet=0.20, decay=0.62, pre_ms=18):
    """循环卷积混响：脉冲响应按环形长度取，结果仍然严格周期，接缝无缝"""
    n_ir = int(decay * 3.2 * RATE)
    t = np.arange(n_ir) / RATE
    rng = np.random.default_rng(20261117)
    ir = rng.standard_normal(n_ir) * np.exp(-t / decay)
    # 简单低通，去掉刺耳的高频噪声尾巴
    b = np.copy(ir)
    for i in range(1, n_ir):
        b[i] = 0.72 * b[i] + 0.28 * b[i - 1]
    ir = b
    ir[:int(pre_ms * 1e-3 * RATE)] = 0.0
    ir /= np.sqrt(np.sum(ir ** 2)) + 1e-9

    ir_full = np.zeros(N)
    m = min(n_ir, N)
    ir_full[:m] = ir[:m]
    # 剩下的尾巴绕回开头，保持周期性
    if n_ir > N:
        ir_full[:n_ir - N] += ir[N:]

    X = np.fft.rfft(buf)
    H = np.fft.rfft(ir_full)
    wet_sig = np.fft.irfft(X * H, n=N)
    buf += wet * wet_sig
    return buf


# ---------------------------------------------------------------- 归一化
def finish(buf, peak=PEAK, drive=1.15):
    buf = np.tanh(buf * drive) / np.tanh(drive)     # 柔和限幅，压掉偶发尖峰
    buf -= np.mean(buf)                              # 去直流，防止功放偏置
    m = np.max(np.abs(buf))
    if m > 1e-9:
        buf *= (peak / 32767.0) / m
    return buf


def write_wav(path, buf):
    data = np.clip(np.round(buf * 32767.0), -32768, 32767).astype('<i2')
    with wave.open(path, 'wb') as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(data.tobytes())
    return len(data) * 2


# ================================================================ 六段铃声
def tone_dawn():
    """晨曦：钟声琶音 + 低频铺底，温暖、有厚度"""
    b = np.zeros(N)
    add_pad(b, [hz('C3'), hz('G3')], amp=0.30, breaths=2)
    phrase = [(0.00, 'C5'), (0.42, 'E5'), (0.84, 'G5'), (1.26, 'C6')]
    phrase2 = [(4.00, 'C5'), (4.42, 'E5'), (4.84, 'G5'), (5.26, 'E6')]
    for t0, nm in phrase:
        render_note(b, t0, hz(nm), 3.4, BELL, amp=0.52, atk=0.004, rel=1.1)
    for t0, nm in phrase2:
        render_note(b, t0, hz(nm), 3.4, BELL, amp=0.58, atk=0.004, rel=1.1)
    # 第二遍加一层弱低八度，让「钟」更厚
    for t0, nm in phrase2:
        render_note(b, t0, hz(nm) / 2, 2.6, BELL, amp=0.16, atk=0.006, rel=1.0)
    return finish(reverb(b, wet=0.22, decay=0.70))


def tone_radar():
    """雷达：短促脉冲连打 + 低频撞击，iPhone 雷达那种紧迫感"""
    b = np.zeros(N)
    period = 1.0
    for g in range(8):
        base = g * period
        for i in range(4):
            t0 = base + i * 0.135
            render_note(b, t0, 1720, 0.085, PULSE, amp=0.50, atk=0.002, rel=0.05)
            render_note(b, t0, 108, 0.16, SUB, amp=0.26, atk=0.003, rel=0.10)
    return finish(reverb(b, wet=0.14, decay=0.42))


def tone_marimba():
    """木琴：木槌琶音，暖、圆、有木头味"""
    b = np.zeros(N)
    seq = [(0.00, 'C5'), (0.28, 'E5'), (0.56, 'G5'), (0.84, 'C6'),
           (1.12, 'G5'), (1.40, 'E5'), (1.68, 'D5'), (1.96, 'G5'),
           (2.52, 'E5'), (2.80, 'G5'), (3.08, 'C6'), (3.36, 'E6'),
           (3.64, 'C6'), (3.92, 'G5')]
    for t0, nm in seq:
        render_note(b, t0, hz(nm), 1.5, MALLET, amp=0.50, atk=0.003, rel=0.55)
    for t0, nm in seq:
        render_note(b, t0 + 4.0, hz(nm), 1.5, MALLET, amp=0.52, atk=0.003, rel=0.55)
    # 低音木琴打底
    for t0, nm in [(0.0, 'C4'), (2.0, 'G3'), (4.0, 'C4'), (6.0, 'G3')]:
        render_note(b, t0, hz(nm), 1.8, MALLET, amp=0.30, atk=0.004, rel=0.6)
    return finish(reverb(b, wet=0.17, decay=0.55))


def tone_broadcast():
    """广播授时：四短一长，但用风琴音色（有泛音）而不是方波"""
    b = np.zeros(N)
    for t0 in (0.0, 1.0, 2.0, 3.0):
        render_note(b, t0, 800, 0.14, ORGAN, amp=0.40, atk=0.006, rel=0.05)
        render_note(b, t0, 400, 0.14, ORGAN, amp=0.16, atk=0.006, rel=0.05)
    render_note(b, 4.0, 1600, 1.35, ORGAN, amp=0.46, atk=0.010, rel=0.30)
    render_note(b, 4.0, 800, 1.35, ORGAN, amp=0.20, atk=0.010, rel=0.30)
    return finish(reverb(b, wet=0.16, decay=0.60))


def tone_signal():
    """信号：明亮的三音上行动机，回声拉开空间"""
    b = np.zeros(N)
    motifs = [
        (0.0, ['E5', 'B5', 'E6'], 0.34),
        (2.0, ['E5', 'B5', 'E6'], 0.34),
        (4.0, ['G5', 'D6', 'G6'], 0.30),
        (6.0, ['E5', 'B5', 'E6'], 0.34),
    ]
    for base, notes, step in motifs:
        for i, nm in enumerate(notes):
            render_note(b, base + i * step, hz(nm), 1.2, PLUCK,
                        amp=0.44 + 0.05 * i, atk=0.002, rel=0.45)
    return finish(reverb(b, wet=0.26, decay=0.75, pre_ms=26))


def tone_rise():
    """渐强：铺底持续升温 + 加速上行的钟声，越逼越紧"""
    b = np.zeros(N)
    add_pad(b, [hz('C4'), hz('G4')], amp=0.26, breaths=3)
    hits = [(0.00, 'C5', 0.34), (1.20, 'D5', 0.37), (2.15, 'E5', 0.40),
            (2.95, 'G5', 0.44), (3.55, 'A5', 0.47), (4.00, 'C6', 0.50),
            (4.35, 'E6', 0.54), (4.62, 'G6', 0.57), (4.84, 'C6', 0.60),
            (5.05, 'E6', 0.63)]
    for t0, nm, amp in hits:
        render_note(b, t0, hz(nm), 2.2, BELL, amp=amp, atk=0.003, rel=0.9)
    return finish(reverb(b, wet=0.20, decay=0.68))


# 顺序 = 固件里的铃声索引 = NVS 里存的 atone 值，**不要随便调整顺序**
TONES = [
    ("晨曦", tone_dawn),
    ("雷达", tone_radar),
    ("木琴", tone_marimba),
    ("广播", tone_broadcast),
    ("信号", tone_signal),
    ("渐强", tone_rise),
]


# ================================================================ 导出
def emit_header(path, wav_bytes):
    total = sum(len(d) for d in wav_bytes)
    with open(path, 'w', encoding='utf-8') as f:
        f.write("// ============================================================\n")
        f.write("//  闹钟铃声数据（自动生成，勿手改）\n")
        f.write("//  由 ringtone/generate.py 用加法合成 + 包络 + 混响离线生成，\n")
        f.write("//  每段 8 秒 / 22050Hz / 16bit / 单声道，可无缝循环。\n")
        f.write(f"//  合计 {total} 字节（{total/1024/1024:.2f} MB）\n")
        f.write("// ============================================================\n")
        f.write("#pragma once\n#include <stdint.h>\n\n")
        f.write("static const char *const RING_NAMES[] = {")
        f.write(", ".join('"%s"' % nm for nm, _ in TONES))
        f.write("};\n#define RING_COUNT %d\n\n" % len(TONES))
        for i, data in enumerate(wav_bytes):
            f.write("static const uint8_t RING_%d[] = {\n" % i)
            line = []
            for j, byte in enumerate(data):
                line.append("0x%02x," % byte)
                if len(line) == 28:
                    f.write("  " + "".join(line) + "\n")
                    line = []
            if line:
                f.write("  " + "".join(line) + "\n")
            f.write("};\n#define RING_%d_LEN %d\n\n" % (i, len(data)))


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    wav_bytes = []
    for i, (name, fn) in enumerate(TONES):
        buf = fn()
        wav = os.path.join(here, "ring_%d_%s.wav" % (i, name))
        size = write_wav(wav, buf)
        wav_bytes.append(open(wav, 'rb').read())
        peak = np.max(np.abs(buf))
        print("%d %-4s %-28s %7d 字节  %5.2f 秒  峰值 %5.0f"
              % (i, name, os.path.basename(wav), size, LOOP_SEC, peak * 32767))
    total = sum(len(d) for d in wav_bytes)
    print("合计 %d 字节（%.2f MB）" % (total, total / 1024 / 1024))
    hdr = os.path.join(os.path.dirname(here), "ringtone_data.h")
    emit_header(hdr, wav_bytes)
    print("已生成", hdr, "%.1f MB" % (os.path.getsize(hdr) / 1024 / 1024))


if __name__ == "__main__":
    main()
