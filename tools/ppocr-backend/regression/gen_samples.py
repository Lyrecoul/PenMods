#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""
多行扫描回归样张生成器。

生成 6 张 A4 200dpi 的 PNG 样张（正文大字号、行距宽松），用于在真机上验证
「多行扫描漏句子」：单行、多行中文、中英混合、行尾标点+空行、重复句、连续扫描。
每张样张顶部有编号和标题，正文与标题之间有分隔线——**只扫分隔线以下的正文**。

用法:
    python3 gen_samples.py [输出目录]        # 默认写 samples/
    python3 gen_samples.py --json            # 只打印 samples.json

依赖: Pillow + 一个 CJK 字体（默认 Noto Sans CJK，可用 CJK_FONT 环境变量覆盖）。
"""
import json
import os
import sys

from PIL import Image, ImageDraw, ImageFont

FONT_CANDIDATES = [
    os.environ.get("CJK_FONT", ""),
    "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
    "/home/lyrecoul/.local/share/fonts/windows/NotoSansSC-VF.ttf",
]
FONT_LATIN = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"

# 页面：200 dpi 的 A4
DPI = 200
PAGE_W, PAGE_H = int(8.27 * DPI), int(11.69 * DPI)
MARGIN_X = int(0.8 * DPI)
BODY_TOP = int(2.6 * DPI)
LINE_H = 100          # 行距：100 px @200dpi ≈ 12.7 mm
FONT_SIZE = 44        # 字号：44 px @200dpi ≈ 5.6 mm em
HEADER_SIZE = 30
NOTE_SIZE = 24

SAMPLES = [
    {
        "id": "S0",
        "name": "单行中文",
        "note": "单行基线：整行扫一次",
        "lines": ["柳州市工人医院门诊部今天正常开诊。"],
    },
    {
        "id": "S1",
        "name": "多行中文（5 行）",
        "note": "整段扫一次（多行扫描翻译），再分 2 次手势连续扫描",
        "lines": [
            "春天的早晨，阳光洒在校园的操场上。",
            "同学们排着整齐的队伍走进教室。",
            "老师翻开课本，开始讲解新的课文。",
            "窗外的树枝随风轻轻摇动。",
            "远处传来一阵清脆的鸟鸣声。",
        ],
    },
    {
        "id": "S2",
        "name": "中英混合（4 行）",
        "note": "整段扫一次；检查英文单词间空格与中文标点",
        "lines": [
            "今天我们学习 computer science 的基本概念。",
            'The teacher said: "Practice makes perfect."',
            "编程不仅仅是写代码，还需要逻辑思维。",
            "We should finish the exercise before Friday.",
        ],
    },
    {
        "id": "S3",
        "name": "行尾标点 + 空行（5 行）",
        "note": "整段扫一次；检查问号/感叹号/无标点行与中间空行",
        "lines": [
            "第一行：这是完整的句子。",
            "第二行：结尾有问号吗？",
            "",
            "第四行：结尾是感叹号！",
            "第五行：最后一行没有标点",
        ],
    },
    {
        "id": "S4",
        "name": "重复句（5 行）",
        "note": "整段扫一次；检查同一句出现两次时是否被去重误删",
        "lines": [
            "重复句测试开始。",
            "这是一句会被重复扫描的话。",
            "中间插入一行不同的内容。",
            "这是一句会被重复扫描的话。",
            "重复句测试结束。",
        ],
    },
    {
        "id": "S5",
        "name": "连续扫描（4 行，分两次手势）",
        "note": "第 1 次手势扫前两行，2 秒内再扫后两行；检查跨手势拼接是否漏行",
        "lines": [
            "连续扫描第一部分的第一个句子。",
            "连续扫描第一部分的第二个句子。",
            "连续扫描第二部分的第一个句子。",
            "连续扫描第二部分的第二个句子。",
        ],
    },
]


def find_font(size, cjk=True):
    paths = FONT_CANDIDATES if cjk else [FONT_LATIN] + FONT_CANDIDATES
    for path in paths:
        if path and os.path.exists(path):
            try:
                return ImageFont.truetype(path, size)
            except OSError:
                continue
    raise SystemExit("找不到可用字体，请设置 CJK_FONT=<path/to/font>")


def render(sample, out_dir):
    img = Image.new("RGB", (PAGE_W, PAGE_H), "white")
    draw = ImageDraw.Draw(img)
    header = find_font(HEADER_SIZE)
    note = find_font(NOTE_SIZE)
    body = find_font(FONT_SIZE)

    draw.text((MARGIN_X, int(0.7 * DPI)), f"{sample['id']}  {sample['name']}", font=header, fill=(0, 0, 0))
    draw.text((MARGIN_X, int(1.25 * DPI)), sample["note"], font=note, fill=(90, 90, 90))
    draw.text(
        (MARGIN_X, int(1.75 * DPI)),
        "只扫描分隔线以下的正文；每张样张按 README 的步骤扫。",
        font=note,
        fill=(90, 90, 90),
    )
    rule_y = int(2.25 * DPI)
    draw.line([(MARGIN_X, rule_y), (PAGE_W - MARGIN_X, rule_y)], fill=(120, 120, 120), width=3)

    y = BODY_TOP
    for line in sample["lines"]:
        if line:
            draw.text((MARGIN_X, y), line, font=body, fill=(0, 0, 0))
        y += LINE_H

    path = os.path.join(out_dir, f"{sample['id']}.png")
    img.save(path, "PNG")
    return path


def main():
    if "--json" in sys.argv[1:]:
        print(json.dumps(SAMPLES, ensure_ascii=False, indent=2))
        return
    out_dir = next((a for a in sys.argv[1:] if not a.startswith("-")), os.path.join(os.path.dirname(__file__), "samples"))
    os.makedirs(out_dir, exist_ok=True)
    for sample in SAMPLES:
        print(render(sample, out_dir))
    with open(os.path.join(os.path.dirname(__file__), "samples.json"), "w", encoding="utf-8") as fh:
        json.dump(SAMPLES, fh, ensure_ascii=False, indent=2)
        fh.write("\n")
    print("samples.json written")


if __name__ == "__main__":
    main()
