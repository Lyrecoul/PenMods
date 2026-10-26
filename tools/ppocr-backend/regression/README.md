# 多行扫描回归样例与诊断工具

用于 GitHub Issue #22 第 2 项「多行扫描漏句子」。这里提供：

| 文件 | 作用 |
|---|---|
| `gen_samples.py` | 生成 6 张 A4 200 dpi 可打印样张（S0–S5） |
| `samples/`（gitignore，`build/diag/scan/samples/` 放实际产物） | 打印用 PNG |
| `samples.json` | 样张正文（期望识别结果，作为 ground truth） |
| `collect.sh` | 从设备采集 app 日志 / `ppocr_shim.log` / 配置快照 / 结果页截图 |
| `analyze_scan_diag.py` | 把 `[ScanDiag]` 日志与 shim 日志对齐，逐层判断丢失位置 |

## 样张一览（ground truth 见 `samples.json`）

| ID | 场景 | 覆盖点 |
|---|---|---|
| S0 | 单行中文 | 单行基线 |
| S1 | 多行中文 5 行 | 多行整段 + 连续扫描 |
| S2 | 中英混合 4 行 | 英文空格、中文标点 |
| S3 | 行尾标点 + 空行 | 问号/感叹号/无标点/空行 |
| S4 | 重复句 5 行 | 同句出现两次，检查去重误删 |
| S5 | 连续扫描 4 行 | 分两次手势，检查跨手势拼接 |

生成样张：

```sh
python3 tools/ppocr-backend/regression/gen_samples.py build/diag/scan/samples
```

用户操作步骤见 `build/diag/scan/USER-SCANS.md`。

## 采集

设备已连 adb 时：

```sh
sh tools/ppocr-backend/regression/collect.sh build/diag/scan/collected          # 采集日志
sh tools/ppocr-backend/regression/collect.sh build/diag/scan/collected --screenshot  # 另抓当前屏幕
```

采集内容：最新 `DictPen_*.log`、`/userdisk/ppocr_shim.log`、`env.txt`（PP-OCR 开关 / target_h /
continuescan / auto_send_scan / libPenMods sha256）。

## 分析

```sh
python3 tools/ppocr-backend/regression/analyze_scan_diag.py \
    --applog  build/diag/scan/collected/DictPen_*.log \
    --shimlog build/diag/scan/collected/ppocr_shim.log \
    -o build/diag/scan/report.md
```

报告给出每个扫描 session 的：

- `onScanFinish(RAW)`（原厂交给 `YSystemBase` 的文本）、`POST-MERGE`（合并后 = QML 显示文本）、
  `showDictPage`（结果页实际文本）三层；
- shim 逐行识别结果与耗时、行组数、**最后一次识别完成时刻 vs 扫描收尾时刻**（正值 = 尾部滞后）；
- 每段「手势节奏（抬笔 → 下次按下）」及是否超过厂商 2 s ContinueScan 窗口；
- 与样张 ground truth 逐行比对：`缺失` 数（识别出了但没进最终文本）、`shim识别:✓/✗ / 合并后:✓/✗ / 显示:✓/✗`，据此判定丢句层级。

## 判定规则

| 现象 | 结论 |
|---|---|
| `shim识别 ✗`，`合并后 ✓` | shim 只做逐行识别；该行来自别的裁切或属于拼接补齐，正常 |
| `shim识别 ✓`，`合并后 ✗` | 丢在**原厂拼接/结果发布**（YCapture / libYoudaoStitch，闭源）或 PenMods 拦截 |
| `合并后 ✓`，`显示 ✗` | 丢在 **PenMods/QML 显示**（可修） |
| 三处都 ✓ 但用户看不到 | 丢在 UI 布局/字体/查询层，查截图与 QML 日志 |
| 尾部滞后 > 0 且缺最后几行 | 识别慢于扫描收尾：慢引擎（PP-OCR）的典型丢句窗口 |
| 手势间隔 > 2 s 且缺前面的行 | 超过厂商 ContinueScan 窗口：前面的行被丢弃 / 另起查询（用户等结果时最常见的表现） |

## 注意

- 只读采集；不要在本脚本里重启 app。
- PP-OCR 开关/`target_h` 改变需要重启 app（`libyocr.so` 在进程启动时解析），由维护者执行。
- 样张必须打印或用另一块屏幕显示；用屏幕时请记录，屏幕反光会降低识别率。
