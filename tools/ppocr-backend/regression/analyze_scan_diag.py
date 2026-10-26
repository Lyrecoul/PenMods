#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""
多行扫描诊断日志分析器。

把设备上两份日志对齐后逐层对比，回答「句子是在哪一层丢的」：

    /userdata/applog/DictPen_*.log   PenMods 的 [ScanDiag] 行 + 原厂 =>OCR Result 行
    /userdisk/ppocr_shim.log         PP-OCR shim 的逐次识别文本/耗时（启用 shim 时）

两层视角：

1) **段落视角（主结论）**：多行扫描是一串「按-放」手势；每个手势的 OCR 结果由
   `=>OCR Result` 打印，厂商在 ContinueScan 窗口结束后才发一次 `onScanFinish`
   （内容 = 累加后的整段文本）。把每段里所有手势的 `=>OCR Result` 与最终
   `onScanFinish(POST-MERGE)` 逐条比对，就能判定是否有一行「识别出来了但没进最终文本」。

2) **session 视角**：每个 `onScanStart` 一个 session，用于看开关状态、延迟、多层文本。

层级对应：

    recognize (shim)           原厂 OCR 线程每次拿到的行裁切图识别结果
    =>OCR Result               原厂按手势发布的单行文本（多行=逐行）
    onOcrComplete(PRE-JOIN)    YCapture 交给 YSystemBase 的原始文本（多行=整段拼接结果）
    onScanFinish(RAW)          onScanFinish 收到的文本
    onScanFinish(POST-MERGE)   合并后 → QML 的 systemBase.ocrCompletedResult（显示/查询用）
    showDictPage(DISPLAY)      词典页/结果页实际拿到的文本

用法:
    python3 analyze_scan_diag.py \
        --applog  build/diag/scan/collected/DictPen_*.log \
        --shimlog build/diag/scan/collected/ppocr_shim.log \
        --samples tools/ppocr-backend/regression/samples.json \
        -o build/diag/scan/report.md
"""
import argparse
import glob
import json
import os
import re
import sys
from datetime import datetime, timedelta

STAMP_RE = re.compile(r"^\[(?P<ts>\d\d:\d\d:\d\d\.\d\d\d)\]")
SCAN_EVENT_RE = re.compile(
    r"\[ScanDiag\] #(?P<session>\d+) t=\+(?P<t>\d+)ms (?P<event>[A-Za-z_()\- ]+?)(?: (?P<detail>.*))?$"
)
DESC_RE = re.compile(r"len=(\d+) utf8=(\d+) lines=(\d+) newlines=(\d+) blanks=(\d+) preview=\"(.*)\"")
SHIM_RE = re.compile(
    r"^\[(?P<ts>\d\d:\d\d:\d\d\.\d\d\d)\] recognize(?:\[adaptive\])?: (?P<w>\d+)x(?P<h>\d+) cn=(?P<cn>\d+) "
    r"-> (?P<tw>\d+)x(?P<th>\d+), (?P<ms>[\d.]+) ms, text=\"(?P<text>.*)\"$"
)
SHIM_INTERRUPT_RE = re.compile(r"^\[(?P<ts>\d\d:\d\d:\d\d\.\d\d\d)\] yocr_set_running_interrupt\((?P<v>[01])\)$")
VENDOR_RESULT_RE = re.compile(r"=>OCR Result: (?P<text>.*)$")
PRESS_RE = re.compile(r"pressed button_id\s*=\s*(?P<id>\d+)")
RELEASE_RE = re.compile(r"released button_id\s*=\s*(?P<id>\d+)")


def parse_stamp(line):
    m = STAMP_RE.match(line)
    return datetime.strptime(m.group("ts"), "%H:%M:%S.%f") if m else None


def parse_desc(detail):
    m = DESC_RE.search(detail or "")
    if not m:
        return None, {}
    preview = m.group(6)
    for esc, raw in (("\\\\", "\\"), ("\\n", "\n"), ("\\r", "\r"), ("\\t", "\t")):
        preview = preview.replace(esc, raw)
    return preview, {
        "len": int(m.group(1)),
        "utf8": int(m.group(2)),
        "lines": int(m.group(3)),
        "newlines": int(m.group(4)),
        "blanks": int(m.group(5)),
    }


def normalize(text):
    return re.sub(r"\s+", "", text or "")


def coverage(line, text):
    """返回 (found, ratio)：忽略空白后是否包含；否则最长公共子串覆盖率。"""
    a, b = normalize(line), normalize(text)
    if not a:
        return True, 1.0
    if a in b:
        return True, 1.0
    best = 0
    for i in range(len(a)):
        for j in range(i + best + 1, len(a) + 1):
            if a[i:j] in b:
                best = max(best, j - i)
            else:
                break
    return False, best / len(a)


def parse_applog(paths):
    events, vendor_results, gestures = [], [], []
    for path in paths:
        with open(path, encoding="utf-8", errors="replace") as fh:
            for line in fh:
                dt = parse_stamp(line)
                if dt is None:
                    continue
                m = SCAN_EVENT_RE.search(line)
                if m:
                    events.append((dt, int(m.group("session")), m.group("event").strip(), m.group("detail") or ""))
                    continue
                m = VENDOR_RESULT_RE.search(line)
                if m:
                    vendor_results.append((dt, m.group("text").strip()))
                    continue
                m = PRESS_RE.search(line)
                if m:
                    gestures.append((dt, "press", int(m.group("id"))))
                    continue
                m = RELEASE_RE.search(line)
                if m:
                    gestures.append((dt, "release", int(m.group("id"))))
    events.sort(key=lambda e: e[0])
    vendor_results.sort(key=lambda e: e[0])
    gestures.sort(key=lambda e: e[0])
    return events, vendor_results, gestures


def parse_shimlog(path):
    recognizes, interrupts = [], []
    if not path or not os.path.exists(path):
        return recognizes, interrupts
    with open(path, encoding="utf-8", errors="replace") as fh:
        for line in fh:
            m = SHIM_RE.match(line.rstrip("\n"))
            if m:
                recognizes.append(
                    {
                        "t": datetime.strptime(m.group("ts"), "%H:%M:%S.%f"),
                        "w": int(m.group("w")),
                        "h": int(m.group("h")),
                        "ms": float(m.group("ms")),
                        "text": m.group("text"),
                    }
                )
                continue
            m = SHIM_INTERRUPT_RE.match(line.rstrip("\n"))
            if m:
                interrupts.append({"t": datetime.strptime(m.group("ts"), "%H:%M:%S.%f"), "v": int(m.group("v"))})
    recognizes.sort(key=lambda r: r["t"])
    interrupts.sort(key=lambda i: i["t"])
    return recognizes, interrupts


def collect_sessions(events):
    sessions, order = {}, []
    for dt, sid, event, detail in events:
        if sid not in sessions:
            sessions[sid] = {
                "start": dt,
                "events": [],
                "raw": [],
                "post": [],
                "completed": [],
                "display": [],
            }
            order.append(sid)
        sessions[sid]["events"].append((dt, event, detail))
        preview, stats = parse_desc(detail)
        if event == "onScanFinish(RAW)":
            sessions[sid]["raw"].append((dt, preview, stats))
        elif event == "onScanFinish(POST-MERGE)":
            sessions[sid]["post"].append((dt, preview, stats))
        elif event == "ocrCompletedResultChanged(DISPLAY)":
            sessions[sid]["completed"].append((dt, preview, stats))
        elif event == "showDictPage(DISPLAY)":
            sessions[sid]["display"].append((dt, preview, stats))
    for idx, sid in enumerate(order):
        end = sessions[order[idx + 1]]["start"] if idx + 1 < len(order) else sessions[sid]["start"] + timedelta(seconds=60)
        sessions[sid]["end"] = end
    return sessions, order


def pick_max(entries):
    """取最长的一条（文本层后续会有清零/重置事件，不能简单取最后一条）。"""
    if not entries:
        return None
    return max(entries, key=lambda e: (e[2] or {}).get("len", len(e[1] or "")))


def pick_last(entries):
    return entries[-1] if entries else None


def gesture_gaps(presses, releases):
    """配对 press/release，返回 [(press, release, gap_to_next_press_s)]。"""
    pairs = []
    for gi, gdt in enumerate(presses):
        nxt = presses[gi + 1] if gi + 1 < len(presses) else None
        rel = next((r[0] for r in releases if gdt <= r[0] < (nxt if nxt else gdt + timedelta(seconds=10))), None)
        pairs.append((gdt, rel, None))
    for i in range(len(pairs) - 1):
        if pairs[i][1] is not None:
            gap = (pairs[i + 1][0] - pairs[i][1]).total_seconds()
            pairs[i] = (pairs[i][0], pairs[i][1], gap)
    return pairs


def build_paragraphs(events, vendor_results, gestures):
    """以 onScanFinish(RAW) 为段边界：一段 = 一串手势 + 段末发布的整段文本。"""
    paragraphs = []
    last_boundary = datetime.min
    for dt, sid, event, detail in events:
        if event != "onScanFinish(RAW)":
            continue
        preview, stats = parse_desc(detail)
        post = []
        for edt, esid, eevent, edetail in events:
            if esid == sid and eevent == "onScanFinish(POST-MERGE)" and dt <= edt < dt + timedelta(seconds=3):
                pp, ps = parse_desc(edetail)
                post.append((edt, pp, ps))
        final = pick_last(post) or (dt, preview, stats)
        seg_gestures = [gdt for gdt, kind, _ in gestures if kind == "press" and last_boundary < gdt <= dt]
        seg_results = [(rdt, txt) for rdt, txt in vendor_results if last_boundary < rdt <= dt]
        releases = [g for g in gestures if g[1] == "release"]
        pairs = gesture_gaps(seg_gestures, releases)
        gaps = [g[2] for g in pairs if g[2] is not None]
        paragraphs.append(
            {
                "end": dt,
                "session": sid,
                "raw": (dt, preview, stats),
                "final": final,
                "gestures": seg_gestures,
                "pairs": pairs,
                "results": seg_results,
                "max_gap": max(gaps) if gaps else None,
                "expired": [g for g in gaps if g > 2.0],
            }
        )
        last_boundary = dt
    return paragraphs


def match_sample(text, samples):
    best = (None, 0.0)
    for sample in samples or []:
        total = [ln for ln in sample["lines"] if ln]
        if not total:
            continue
        found = sum(1 for ln in total if coverage(ln, text)[0])
        ratio = found / len(total)
        if ratio > best[1]:
            best = (sample["id"], ratio)
    return best


def latency_stats(paragraphs, gestures):
    """每段的「该手势抬笔 → 对应行 OCR 结果出现」延迟（秒）。

    原厂按手势逐行识别，第 i 个手势的结果通常在第 i 个结果位置（结果可能晚于下一次按下），
    所以按顺序把手势 release 与 results 配对，比按时间窗口配对更准。
    """
    releases = [g for g in gestures if g[1] == "release"]
    stats = []
    for idx, p in enumerate(paragraphs, 1):
        pairs = []
        for gi, gdt in enumerate(p["gestures"]):
            nxt = p["gestures"][gi + 1] if gi + 1 < len(p["gestures"]) else p["end"] + timedelta(seconds=5)
            rel = next((r[0] for r in releases if gdt <= r[0] < nxt), None)
            if rel is not None:
                pairs.append(rel)
        results = [rt for rt, _ in p["results"]]
        lat = []
        for rel, rt in zip(pairs, results):
            if rt >= rel:
                lat.append((rt - rel).total_seconds())
        if lat:
            stats.append((idx, len(lat), min(lat), max(lat), sum(lat) / len(lat)))
    return stats


def render_report(data):
    sessions, order, paragraphs, samples = data["sessions"], data["order"], data["paragraphs"], data["samples"]
    out = []
    out.append("# 多行扫描诊断报告\n")
    out.append(f"- 日志: {', '.join('`' + os.path.basename(p) + '`' for p in data['applog_paths'])}")
    out.append(f"- shim 识别调用: {len(data['recognizes'])}，中断请求: {len(data['interrupts'])}")
    out.append(f"- 扫描手势（按下）: {data['press_count']}，完整段落发布: {len(paragraphs)}\n")
    if data["shim_missing"]:
        out.append("> 未找到 shim 日志：本次只有 `[ScanDiag]`/原厂日志，无法做 shim 逐行核对。\n")

    out.append("## 段落级判定（主结论）\n")
    out.append(
        "多行扫描由一串手柄手势组成；原厂在 ContinueScan 窗口结束后才发一次 `onScanFinish`。"
        "下表把每段里每个手势的 `=>OCR Result` 与段末最终文本（`POST-MERGE`）逐条比对 —— "
        "`缺失` 就是「识别出来了但没进最终文本」，再按层级判断是原厂合并还是显示丢的。\n"
    )
    out.append(
        "| 段 | 发布时刻 | 手势数 | 匹配样张 | 每手势结果 | 最终文本长度 | 缺失(未进最终文本) "
        "| 最大手势间隔(抬笔→下次按下) | 超 2s 窗口 | 层级判定 |"
    )
    out.append("|---|---|---|---|---|---|---|---|---|---|")
    for idx, p in enumerate(paragraphs, 1):
        final_text = p["final"][1] or ""
        missing = 0
        for _, txt in p["results"]:
            if not coverage(txt, final_text)[0]:
                missing += 1
        sample_id, sample_ratio = match_sample(final_text, samples)
        layer = "—" if missing == 0 else "原厂合并/发布（识别有、最终无；PenMods pass-through）"
        gap = f"{p['max_gap']*1000:.0f}ms" if p.get("max_gap") is not None else "-"
        expired = len(p.get("expired") or [])
        out.append(
            f"| {idx} | {p['end'].strftime('%H:%M:%S.%f')[:-3]} | {len(p['gestures'])} | "
            f"{sample_id or '-'}({sample_ratio*100:.0f}%) | {len(p['results'])} | "
            f"{(p['final'][2] or {}).get('len', len(final_text))} | {missing} | {gap} | {expired} | {layer} |"
        )
    out.append("")

    for idx, p in enumerate(paragraphs, 1):
        final_text = p["final"][1] or ""
        sample_id, sample_ratio = match_sample(final_text, samples)
        out.append(f"### 段 {idx} — {p['end'].strftime('%H:%M:%S.%f')[:-3]}（匹配 {sample_id or '未知样张'}）\n")
        out.append(
            f"- 最终文本 len={(p['final'][2] or {}).get('len', len(final_text))} "
            f"newlines={(p['final'][2] or {}).get('newlines', '?')} blanks={(p['final'][2] or {}).get('blanks', '?')}"
        )
        out.append("  ```")
        out.append("  " + (final_text or "").replace("\n", "\n  "))
        out.append("  ```")
        if p.get("pairs"):
            out.append("- 手势节奏（抬笔 → 下次按下）：")
            for gdt, rel, gap in p["pairs"]:
                if gap is None:
                    out.append(f"  - {gdt.strftime('%H:%M:%S.%f')[:-3]} 按下 → 最后一次手势")
                else:
                    flag = " ⚠ 超过厂商 2s ContinueScan 窗口" if gap > 2.0 else ""
                    out.append(f"  - {gdt.strftime('%H:%M:%S.%f')[:-3]} 按下 → 间隔 {gap*1000:.0f}ms{flag}")
        if p["results"]:
            out.append("- 逐手势 OCR 结果（`=>OCR Result`）：")
            for rdt, txt in p["results"]:
                ok, ratio = coverage(txt, final_text)
                mark = "OK" if ok else f"MISS({ratio*100:.0f}%)"
                out.append(f"  - [{mark}] `{txt}`")
        out.append("")

    lat = data["latency"]
    if lat:
        out.append("## 延迟：抬笔 → 该手势 OCR 结果出现\n")
        out.append("| 段 | 手势数 | 最小 | 最大 | 平均 |")
        out.append("|---|---|---|---|---|")
        for idx, n, lo, hi, avg in lat:
            out.append(f"| {idx} | {n} | {lo*1000:.0f}ms | {hi*1000:.0f}ms | {avg*1000:.0f}ms |")
        out.append(
            "\n> 多行继续扫描要求「抬笔后 <2s 内开始下一行」。延迟长期 >2s 时，用户若等结果出现再扫下一行，"
            "厂商会结束当前段落并开新查询，表现就是「前面的行消失」。\n"
        )

    out.append("## Session 视角（逐层日志）\n")
    out.append("| # | 开始 | 匹配样张 | RAW长度 | POST-MERGE长度 | DISPLAY长度 | 识别调用 | 识别耗时ms |")
    out.append("|---|---|---|---|---|---|---|---|")
    for sid in order:
        s = sessions[sid]
        raw = pick_last(s["raw"])
        post = pick_last(s["post"])
        comp = pick_max(s["completed"])
        recs = [r for r in data["recognizes"] if s["start"] - timedelta(milliseconds=200) <= r["t"] < s["end"]]
        text_for_match = (post[1] if post else None) or (s["completed"][-1][1] if s["completed"] else "")
        sample_id, _ = match_sample(text_for_match, samples)
        out.append(
            f"| {sid} | {s['start'].strftime('%H:%M:%S.%f')[:-3]} | {sample_id or '-'} | "
            f"{(raw[2] or {}).get('len', '?') if raw else '-'} | "
            f"{(post[2] or {}).get('len', '?') if post else '-'} | "
            f"{(comp[2] or {}).get('len', '?') if comp else '-'} | {len(recs)} | "
            f"{sum(r['ms'] for r in recs):.0f} |"
        )
    out.append("")
    return "\n".join(out) + "\n"


def analyze(applog_paths, shimlog_path, samples_path):
    events, vendor_results, gestures = parse_applog(applog_paths)
    recognizes, interrupts = parse_shimlog(shimlog_path)
    samples = []
    if samples_path and os.path.exists(samples_path):
        with open(samples_path, encoding="utf-8") as fh:
            samples = json.load(fh)
    sessions, order = collect_sessions(events)
    paragraphs = build_paragraphs(events, vendor_results, gestures)
    return {
        "events": events,
        "sessions": sessions,
        "order": order,
        "paragraphs": paragraphs,
        "samples": samples,
        "recognizes": recognizes,
        "interrupts": interrupts,
        "applog_paths": applog_paths,
        "shim_missing": not recognizes,
        "press_count": sum(1 for g in gestures if g[1] == "press"),
        "latency": latency_stats(paragraphs, gestures),
    }


def main():
    ap = argparse.ArgumentParser(description="多行扫描 [ScanDiag] 日志分析")
    ap.add_argument("--applog", nargs="+", default=["/userdata/applog/DictPen_*.log"], help="DictPen 日志（可多个）")
    ap.add_argument("--shimlog", default="/userdisk/ppocr_shim.log", help="PP-OCR shim 日志")
    ap.add_argument("--samples", default=os.path.join(os.path.dirname(__file__), "samples.json"))
    ap.add_argument("-o", "--output", default="-")
    args = ap.parse_args()

    paths = []
    for pattern in args.applog:
        paths.extend(glob.glob(pattern))
    if not paths:
        sys.exit(f"找不到 app 日志: {args.applog}")

    data = analyze(paths, args.shimlog, args.samples)
    report = render_report(data)
    if args.output == "-":
        sys.stdout.write(report)
    else:
        os.makedirs(os.path.dirname(os.path.abspath(args.output)), exist_ok=True)
        with open(args.output, "w", encoding="utf-8") as fh:
            fh.write(report)
        print(args.output)


if __name__ == "__main__":
    main()
