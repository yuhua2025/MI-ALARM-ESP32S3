#!/usr/bin/env python3
"""固件 / 网页契约自检（不需要硬件）。

检查三类容易在移植时漏掉的联动：
  1. PAGE_HTML 里的 {{占位符}} 是否都在 handleRoot() 里被 replace
  2. 网页 save() 提交的参数是否都在 handleSave() 里被读取
  3. 顶部 PIN_xxx 宏是否和 ../硬件接线表.md 的引脚表一致

用法：python3 verify_contract.py
"""
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ino = (HERE / "ino.ino").read_text(encoding="utf-8")
html = (HERE / "web_pages.h").read_text(encoding="utf-8")
wiring = (HERE.parent / "硬件接线表.md").read_text(encoding="utf-8")

errors = []


def page_html(text: str) -> str:
    m = re.search(r"const char PAGE_HTML\[\] = R\"HTML\((.*?)\)HTML\";", text, re.S)
    return m.group(1) if m else ""


page = page_html(html)
# 占位符检查覆盖**所有** R"HTML(...)" 模板（首页、配网页、验证页），
# 不能只盯 PAGE_HTML —— v4.7.4 的验证页就是独立模板，漏掉它等于没检查。
placeholders = set()
for _name, _body in re.findall(r'const char ([A-Z_0-9]+)\[\] = R"HTML\((.*?)\)HTML";',
                               html, re.S):
    placeholders |= set(re.findall(r"\{\{([A-Z0-9_]+)\}\}", _body))
# v4.7.1 起改成单遍渲染：占位符登记在 handleRoot() 的 TplVar 表里，
# 形如 {"SYNC", vSync} / {"NON", cfg.nightOn ? "checked" : ""}。
# 老写法 page.replace("{{X}}", ...) 也一并认，方便回退和对照。
replaced = set(re.findall(r'page\.replace\("\{\{([A-Z0-9_]+)\}\}"', ino))
replaced |= set(re.findall(r'\{\s*"([A-Z0-9_]+)"\s*,', ino))
missing = placeholders - replaced
extra = replaced - placeholders
if missing:
    errors.append(f"PAGE_HTML 里这些占位符没在 handleRoot() 替换：{sorted(missing)}")
if extra:
    errors.append(f"handleRoot() 替换了页面里不存在的占位符：{sorted(extra)}")
print(f"[1] 占位符：页面 {len(placeholders)} 个 / 固件替换 {len(replaced)} 个")

# save() 的 new URLSearchParams({...}) 里出现的 key: $('id').xxx 或字面量参数
save_fn = re.search(r"function save\(\)\s*\{(.*?)\n\}", page, re.S)
save_body = save_fn.group(1) if save_fn else ""
save_ids = set(re.findall(r"\$\('([a-z0-9]+)'\)", save_body))
save_keys = set(re.findall(r"([a-z][a-z0-9]*)\s*:\s*", save_body))
# v4.7.3 起 m24 改成「用户动过才提交」的 p.set('m24', ...)，
# 不在对象字面量里了，这里也要认，否则这个参数以后漏检
save_keys |= set(re.findall(r"\.set\(\s*'([a-z][a-z0-9]*)'", save_body))
# handleSave 里 hasArg("xxx")
handled = set(re.findall(r'hasArg\("([a-z][a-z0-9]*)"\)', ino))
# 时段参数 ns/ne/cs/ce 走 hhmmToMinute(server.arg(...))，单独识别
period = set(re.findall(r'hhmmToMinute\(server\.arg\("([a-z][a-z0-9]*)"\)\)', ino))
handled |= period
unhandled = save_keys - handled
if unhandled:
    errors.append(f"save() 提交但 handleSave() 没处理的参数：{sorted(unhandled)}")
print(f"[2] /save 参数：网页 {len(save_keys)} 个 / 固件处理 {len(handled & save_keys)} 个（控件 {len(save_ids)} 个）")

# 引脚宏 vs 接线表：只核对固件里出现的 PIN_ 定义与表中 GPIO 号
pin_defs = dict((name, int(num)) for name, num in
                re.findall(r"#define\s+(PIN_[A-Z0-9_]+)\s+(\d+)", ino))
table = set(int(n) for n in re.findall(r"\|\s*GPIO(\d+)\s*\|", wiring))
not_in_table = {k: v for k, v in pin_defs.items() if v not in table}
if not_in_table:
    errors.append(f"固件引脚没出现在接线表里：{not_in_table}")
print(f"[3] 引脚：固件 {len(pin_defs)} 个宏 / 接线表 {len(table)} 个 GPIO")

if errors:
    print("\n检查未通过：")
    for e in errors:
        print("  -", e)
    sys.exit(1)
print("\n全部通过。")
