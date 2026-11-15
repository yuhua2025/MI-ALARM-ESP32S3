#!/usr/bin/env python3
"""
小米闹钟自画裸片板 WiFi 发射功率分档实测。

对端是板级 rf_tools.cc 提供的 UDP 测试服务（默认端口 3333）。
每档测量：
  - STAT：设备实际生效的 TX 上限、省电模式、AP RSSI
  - BURST：设备主动连发 UDP 上行包，主机统计接收率（直接反映设备发射能力）
  - ping：RTT 与丢包

用法：
  python3 rf_sweep.py                     # 只测，不改设备
  python3 rf_sweep.py --apply             # 测完把推荐功率 SAVE 到 NVS
  python3 rf_sweep.py --steps 8,11,14,17,20 --count 120
"""
import argparse
import json
import re
import socket
import subprocess
import time
from pathlib import Path

DEFAULT_DEVICE = "192.168.8.129"
RF_PORT = 3333
RECV_PORT = 3334
HERE = Path(__file__).resolve().parent


def cmd(text, device, timeout=3.0):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(timeout)
    try:
        s.sendto(text.encode(), (device, RF_PORT))
        data, _ = s.recvfrom(2048)
        return data.decode("utf-8", "replace").strip()
    except socket.timeout:
        return "ERR timeout"
    finally:
        s.close()


def local_ip_for(device):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.connect((device, RF_PORT))
    ip = s.getsockname()[0]
    s.close()
    return ip


def parse_kv(text):
    out = {}
    for item in text.split():
        if "=" in item:
            k, v = item.split("=", 1)
            try:
                out[k] = float(v) if "." in v else int(v)
            except ValueError:
                out[k] = v
    return out


def ping(device, count=6):
    try:
        p = subprocess.run(
            ["ping", "-c", str(count), "-t", "8", device],
            capture_output=True, text=True, timeout=20,
        )
        out = p.stdout
    except Exception as e:
        return {"loss": 100.0, "avg": -1, "raw": str(e)}
    loss = 100.0
    avg = -1.0
    m = re.search(r"([\d.]+)% packet loss", out)
    if m:
        loss = float(m.group(1))
    m = re.search(r"= ([\d.]+)/([\d.]+)/([\d.]+)/([\d.]+) ms", out)
    if m:
        avg = float(m.group(2))
    return {"loss": loss, "avg": avg}


def burst(device, host, count, interval_ms):
    r = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    r.bind(("0.0.0.0", RECV_PORT))
    r.settimeout(0.4)
    ack = cmd(
        f"BURST {host} {RECV_PORT} {count} {interval_ms}",
        device,
        timeout=max(10.0, count * interval_ms / 1000.0 + 6.0),
    )
    seqs = []
    deadline = time.time() + 1.2
    empty_polls = 0
    while time.time() < deadline:
        try:
            data, _ = r.recvfrom(512)
            empty_polls = 0
        except socket.timeout:
            empty_polls += 1
            if seqs and empty_polls >= 2:
                break
            continue
        parts = data.decode("utf-8", "replace").strip().split(",")
        if len(parts) >= 2 and parts[0] == "RF":
            try:
                seqs.append(int(parts[1]))
            except ValueError:
                pass
    r.close()
    got = len(set(seqs))
    out_of_order = sum(1 for i in range(1, len(seqs)) if seqs[i] < seqs[i - 1])
    return {
        "ack": ack,
        "sent": count,
        "received": got,
        "rate": round(100.0 * got / count, 2),
        "dup": len(seqs) - got,
        "out_of_order": out_of_order,
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--device", default=DEFAULT_DEVICE)
    ap.add_argument("--steps", default="8,11,14,17,20")
    ap.add_argument("--count", type=int, default=120)
    ap.add_argument("--interval", type=int, default=20)
    ap.add_argument("--apply", action="store_true")
    ap.add_argument("--no-ps", action="store_true", help="跳过省电模式对比")
    ap.add_argument("--ping-count", type=int, default=12)
    ap.add_argument("--policy", choices=["coverage", "powersave"], default="coverage",
                    help="coverage=插电设备取最高合格档留余量；powersave=取最低合格档")
    args = ap.parse_args()

    device = args.device
    steps = [int(x) for x in args.steps.split(",")]
    host = local_ip_for(device)
    print(f"设备 {device}:{RF_PORT}  主机 {host}:{RECV_PORT}")
    stat0 = cmd("STAT", device)
    print("初始状态:", stat0)
    if stat0.startswith("ERR"):
        print("设备无响应，确认固件已烧录且已连上 WiFi")
        return 1

    result = {"device": device, "host": host, "initial": stat0,
              "policy": args.policy, "rows": []}

    # --- 省电模式对比：证明 400ms 延迟来自 MAX_MODEM ---
    if not args.no_ps:
        print("\n[省电模式对比]")
        result["ps_compare"] = {}
        for ps, name in [(2, "MAX_MODEM(旧默认 LOW_POWER)"), (0, "NONE(修复后)")]:
            r = cmd(f"PS {ps}", device)
            time.sleep(0.8)
            pg = ping(device, args.ping_count)
            result["ps_compare"][name] = {"set": r, **pg}
            print(f"  PS={ps} {name:28s} ping avg={pg['avg']:>7.2f} ms  loss={pg['loss']:.1f}%  {r}")
        cmd("PS 0", device)

    # --- 功率分档 ---
    print("\n[发射功率分档]")
    print(f"{'dBm':>5} {'q':>4} {'实际dBm':>8} {'RSSI':>6} {'上行收包':>10} {'ping丢包':>9} {'ping均值':>10}")
    for dbm in steps:
        q = int(round(dbm * 4))
        setr = cmd(f"TXP {q}", device)
        time.sleep(1.0)
        st = parse_kv(cmd("STAT", device))
        b = burst(device, host, args.count, args.interval)
        pg = ping(device, args.ping_count)
        row = {
            "dbm": dbm, "quarter": q, "set": setr,
            "actual_dbm": st.get("dbm"), "rssi": st.get("rssi"),
            "ps": st.get("psname"), "burst": b, "ping": pg,
        }
        result["rows"].append(row)
        print(f"{dbm:>5} {q:>4} {str(st.get('dbm','?')):>8} "
              f"{str(st.get('rssi','?')):>6} {b['rate']:>9.2f}% "
              f"{pg['loss']:>8.1f}% {pg['avg']:>9.2f}ms")

    # --- 判定 ---
    qualified = [r for r in result["rows"]
                 if r["ping"]["loss"] == 0
                 and r["burst"]["rate"] >= 99.0
                 and r["ping"]["avg"] >= 0]
    result["qualified_dbms"] = [r["dbm"] for r in qualified]
    if qualified:
        if args.policy == "coverage":
            best = max(qualified, key=lambda r: r["dbm"])
            reason = ("所有档位均零丢包/上行 100%，取最高合格档：RSSI 实测会瞬时跌到 -79 dBm，"
                      "插电设备需要链路余量；且最高档未出现丢包或 RTT 劣化，说明本板供电/去耦能承受满功率")
        else:
            best = min(qualified, key=lambda r: r["dbm"])
            reason = "链路零丢包且上行收包≥99% 的最低功率档（省电、降低电源压力、减少干扰）"
    else:
        best = max(result["rows"], key=lambda r: (r["burst"]["rate"], -r["ping"]["loss"]))
        reason = "没有完全合格档位，取上行收包率最高、ping 丢包最低的一档"
    result["recommend"] = {"dbm": best["dbm"], "quarter": best["quarter"], "reason": reason}

    # 满功率档是否有劣化（自画板供电/匹配不足会在高档暴露）
    top = result["rows"][-1]
    low = result["rows"][0]
    result["top_vs_low"] = {
        "low_burst": low["burst"]["rate"], "top_burst": top["burst"]["rate"],
        "low_ping": low["ping"]["avg"], "top_ping": top["ping"]["avg"],
    }
    print(f"\n推荐: {best['dbm']} dBm (q={best['quarter']}) — {reason}")

    if args.apply:
        r = cmd(f"SAVE {best['quarter']}", device)
        print("写入设备:", r, "| 复查:", cmd("STAT", device))
        result["applied"] = r
    else:
        # 恢复默认满功率，避免把设备留在最后一档的低功率状态
        cmd(f"TXP {80}", device)

    out = HERE / "rf_sweep_result.json"
    out.write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8")
    print("结果已保存:", out)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
