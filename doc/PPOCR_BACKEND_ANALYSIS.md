# OCR 引擎替换分析(实验性 PP-OCRv5 backend)

> 目标: 逆向有道词典笔的 OCR 链路,把"认字"这一步换成飞桨 PP-OCRv5 模型,并评估可行性
>
> 分析日期: 2026-10-04
>
> 目标文件: `binary/YoudaoDictPen` + `/oem/YoudaoDictPen/output/aarch64_libs/libyocr.so`
>
> 代码: `tools/ppocr-backend/`(shim + 构建/部署脚本),配合 `doc/CAMERA_CAPTURE_ANALYSIS.md`

---

## 1. 厂商 OCR 栈

```
摄像头 YUYV
  └─ YCapture::OCR_capture()            0x445684   抓帧
       └─ ydstitch_push_rawdata_new()             libYoudaoStitch.so 拼接
            └─ ydstitch_get_ocr_result()          libYoudaoStitch.so OCR 线程
                 ├─ SegLine::seg_middle_line()    行检测/切分(在 stitch 库内部!)
                 └─ yocr_recognize(cv::Mat)       逐行识别  ← 我们替换的就是这里
                      libyocr.so = ncnn + Eigen + FST 语言模型解码
```

模型目录 `/oem/YoudaoDictPen/output/ocr_model/`:

| 文件 | 大小 | 用途 |
|---|---|---|
| `crnn_dictpen_{std_v2.3,pro_v2.2,es_v2.2}.param/.bin` | 2.0 MB | ncnn CRNN 识别模型 |
| `deploy_hv_line.param/.bin` | 3.8 MB | ncnn 检测模型(ydet_ncnn) |
| `ocr_model_{std,pro,es}_vX.Y_DATE` | 16 MB | Caffe 风格 FC/LSTM 权重块(`load_CaffePara_FC/LSTM`) |
| `TG_c.fst` | 150 MB | FST 语言模型 |
| `label.23500_with_index`, `words.txt` | — | 字典 |

`libyocr.so` 编译自 `/home/xubin/DictPen_OCR/OCR_DictPen3/yocr_ncnn/`,即**厂商自己也在用 ncnn**
(同时带 Eigen、以及 kaldi/openfst 派生的 `fstdecode`)。Eigen 只做小矩阵(`gevm_neon_m16` 之类)。

启动日志(设备 `/userdata/applog`)证实实际配置:

```
 [OCR PATH] ocr_model_pro
 [yocr_ncnn]: recog_model dictpen_pro_v2.2     ← 用的是 pro 变体
 [yocr_ncnn]: load caffe model succeed
 [seg_line ]: load adaboost model succeed      ← 行切分在 stitch 库里
 [ydet_ncnn]: load detect model succeed
```

**重要**:app 的导入表里没有 `ydstitch_load_lang_model`,而 `yocr_enable_lang_model` 只被
`ydstitch_load_lang_model` 调用 —— 也就是说扫描链路里**根本没有启用那个 150 MB 的 FST 语言模型**,
厂商走的是 greedy 解码。做精度对比时必须按这个前提来。

---

## 2. 接缝:libyocr.so 只暴露 6 个符号

用 `libYoudaoStitch.so` 的未定义符号与 `libyocr.so` 的导出符号求交集,恰好 6 个:

```cpp
std::string yocr_recognize(const cv::Mat& lineCrop);          // ← 唯一的识别入口
void        yocr_init_dictpen_model(std::string dir, std::string variant);   // 返回值被检查(w0)
void        yocr_enable_lang_model(std::string, std::string); // 未启用
void        yocr_release_dictpen_model();
bool        yocr_get_running_interrupt();
void        yocr_set_running_interrupt(bool);
```

行检测/切分(`seg_middle_line`、`SegLine::*`)其实定义在 `libYoudaoStitch.so` 自己里面,
所以替换 `libyocr.so` **完全不碰检测**。

初始化调用顺序(app 在 `YSystemBasePrivate` 构造里做,`nm`+反汇编确认):

```cpp
ydstitch_init();
ydstitch_load_ocr("ocr_model_pro", "ocr_model");   // variant ∈ {ocr_model_standard, _pro, _es}
```
`ydstitch_load_ocr` 收到的是模型**变体名**和基础目录名(相对 app 的 cwd `/oem/YoudaoDictPen/output`),
内部再转成 yocr 的模型名(`ocr_model_pro` → `dictpen_pro_v2.2`)。

`yocr_recognize` 收到的 Mat 尺寸 = 行裁切图被统一缩放到 **高 48**(日志里的 `1349x48`),灰度(cn=1)。

---

## 3. 为什么是 shim 而不是 hook

| 方案 | 结论 |
|---|---|
| 在 libPenMods 里 hook `yocr_recognize` | ✗ 签名含 `cv::Mat` / `std::string`,是 libstdc++ 6.0.22 + OpenCV 3.4 的 C++ ABI;libPenMods 用 zig/libc++ 静态链接,跨 ABI 返回 std::string 会直接崩 |
| **重写 libyocr.so(本方案)** | ✓ 用**厂商同款工具链**编译,ABI 天然一致;只实现 6 个符号 |
| 替换模型文件 | ✗ 厂商 CRNN 的解码/字典不匹配 PP-OCR 模型 |

部署用 `LD_LIBRARY_PATH` 影子覆盖:`/usr/bin/runDictPen` 设的是
`/userdisk/Qtlib/:...:/oem/YoudaoDictPen/output/libs`,而 `/userdisk/Qtlib` 原本**不存在**,
放一个 `libyocr.so` 进去即可优先加载;删掉即回滚,`/oem` 一动不动。

---

## 4. 实测:精度

同一张行裁切图分别喂给厂商引擎和 PP-OCRv5(`ab_test` 工具,greedy vs greedy):

| 真实扫描裁切 | 厂商 | PP-OCRv5 |
|---|---|---|
| 柳州市工人医院 (×2) | ✓ | ✓ |
| 学员凭此承诺书进入相关科目考试场，在应考技能考试科目 | `学员赁进进推的能进科目` ✗ | **✓** |
| 同上(第二行) | `学员泵诺书进入相关科目考试场,在应考技能考试科百` ✗ | **✓** |
| 准教员对 学员郑重承诺：在对您 | `推新品好些昂重承诺` ✗ | `准教员对学员郑重承诺：` |

合成图(渲染文本)上两边都基本正确,厂商偶尔错 1 字(`PaddleoCR`)。

结论:**在真实笔扫的图像上 PP-OCRv5 明显更准**,代价是延迟(见下)。

---

## 5. 实测:性能

单行 677×48,4 线程,总计 937 ms 的逐层剖析(ncnn `NCNN_BENCHMARK`):

| 层类型 | 总耗时 | 层数 | 平均 |
|---|---|---|---|
| Convolution | 447 ms | 24 | 18.6 ms |
| BinaryOp | 199 ms | 118 | 1.69 ms |
| ConvolutionDepthWise | 116 ms | 14 | 8.3 ms |
| Gemm | 90 ms | 5 | 18.0 ms |
| HardSwish | 50 ms | 28 | 1.8 ms |

排除过的猜想:

- **功耗策略不是原因**:governor 钉 `performance` + 关 `cpu-sleep/cluster-sleep`,879 ms → 881 ms;
- **最后的 18385 类 FC 不是瓶颈**:砍到 1000 类只降 11%;
- **fp16 存储无用**:RK3326 只有 `asimd`,没有 fp16 运算(`asimdhp`),开启 `use_fp16_storage` 反而略慢。

真正的瓶颈就是 4×A35@1.2 GHz 跑 fp32 卷积的算力;**zig 工具链会让它更慢**——ncnn 每层靠
`#pragma omp parallel for` 并行,而 zig 的 `-fopenmp` 没有 `omp.h`/运行时,整个模型变成单线程
(实测 1 站 1288 ms → 改用 `aarch64-linux-gnu-g++` 后 473 ms,**3.8×**)。

### target height 权衡

app 已经把裁切行缩到 ~48 px 再交给我们,所以 `target_h=48` 表示"不再二次缩放":

| target_h | 长行(~1400×50) | 密集中文精度 |
|---|---|---|
| 48 | ~1.9–2.1 s | 最好(能读对 `流程控制结构`) |
| 40 | ~1.6 s | 中间 |
| 32 | ~0.9–1.0 s | 密集中文丢笔画(`流程控制结构` → `流程介绍`) |
| 24 | ~0.3 s | 长行崩坏 |

英文对高度不敏感(字母间距大),32 也能全对。

---

## 6. 踩过的坑(实现层面)

1. **OpenCV 的通道数是 `cn-1` 编码**:`((flags >> 3) & 511) + 1`;写成 `(flags >> 3) & 511`
   会把所有 `CV_8UC3` 裁切图判为不支持(第一次预检就是这样被拦下来的)。
2. **Mat 可能是非连续 ROI**(未置 `CV_CONTINUOUS_FLAG`);必须按 `step.p[0]` 逐行取,否则越界读
   未映射页 → **直接把 app 打崩**。
3. **网络输出比字典多一类**:18385 = blank + 18383 字典字符 + **额外一个空格类**。
   丢掉那一类会让英文单词粘在一起(`HelloWorld`)。映射成 ASCII 空格。
4. **不要 `adb push` 覆盖正在被映射的 `libyocr.so`**:会改写活映射的页,app 执行错位代码而段错误,
   连续崩溃会被固件升级成 `misc` 的 `boot-recovery` 重启并进恢复模式。必须先 push 到临时名再 `mv`。
5. 交叉工具链的 sysroot 必须是 **glibc 2.27**;shim 必须**动态**链接 libstdc++,与 app 共用同一份,
   否则 `std::string` 会跨两个不兼容运行时。

---

## 7. 未做的事 / 后续可选项

- **int8 量化**:这颗 SoC 没有 `asimddp`(dotprod),收益有限;
- **换 PP-OCRv4 mobile rec**:骨干更轻(约再快 1.5–1.7×),但中文精度低于 v5;
- **行宽切块**:超长行切成若干块再拼,收益与拼接开销基本抵消,除非同时要提精度;
- **FST/热词解码**:厂商保留了 150 MB FST 却没启用;若给 PP-OCR 接一个轻量语言模型/热词表,
  形近字错误(`深人`/`深入`、`赁`/`凭`)有望改善;
- **运行时开关**:用 dlopen 转发回厂商引擎看似可行,但存在符号 interposition 递归风险,暂不做;
  现在的开关方式就是 `deploy.sh on/off`。

---

## 8. 多行扫描「漏句子」诊断（2026-10-26，issue #22-2）

用户报告「多行扫描漏句子非常严重」。先确认链路，再判断丢在哪一层；**不要**把所有扫描图拼起来重新 OCR
（延迟/内存都不可接受）。

### 8.1 多行链路（反汇编 `binary/YoudaoDictPen` 确认）

```
YCapture::OCR_capture()                       每帧 camera_get_frame → ydstitch_push_rawdata_new()
  └─ YCapture::lines_OCR(int,int)             单行：一行扫一次
     YCapture::paragraphs_OCR(int,int)        多行整段：循环 OCR_capture() → ydstitch_get_ocr_result()
        每轮把新文本 append 到 YCapture+0x88（含行尾 '-' 合并）；最后发一次
        YCapture::ocrComplete(text, int)      ← 多行在这一层已经是「整段文本」
          │ (queued, libstdc++ ABI)
          ▼
YSystemBase::onOcrComplete(std::string const&, int)
  └─ YSystemBase::onScanFinish(QString const&, int)
       scanType==0: 结果 = 新文本（替换）
       scanType!=0: 结果 = StrUtils::joinString(上次基串, 新文本)（追加）
       → ocrCompletedResultChanged()
          ▼
QML systemBase.ocrCompletedResult → YScanWordsResultLoader 逐字显示 / entryResult() 查询
```

`onScanStart()` 清空显示串，`onStopContinueScan()` 清空追加基串。PenMods 在普通查词流程里对这些
slot 是 pass-through（仅 `inputPageShowing`/`autoSendScan` 时拦截）。所以词典多行流程里 PenMods 唯一
会改文本的地方是 `QueryTweaks::entryResult` 的小写/空格折叠（不删句）。

因此「漏句」只可能发生在：①原厂帧拼接/行切分；②`yocr_recognize` 逐行识别；③`paragraphs_OCR`
把结果发布给 `ocrComplete` 之前（尾部行还没识别完/被中断）；④PenMods 拦截路径（AI 快速发送）；
⑤QML 显示/查询。

### 8.2 PP-OCR 的延迟结构（`/userdisk/ppocr_shim.log` 实测，484 次识别）

原厂 OCR 线程对同一行会**反复识别逐渐变宽的裁切图**（帧拼接越长，裁切越宽），最后取最宽那次：

```
68x40 229ms → 138x46 360ms → 358x47 654ms → 416x46 658ms → 731x47 1483ms → 1371x48 2079ms
```

按「同一行、宽度单调增长」把 484 次识别归并成 160 个行终稿：

| 指标 | 数值 |
|---|---|
| 行终稿（最宽裁切）识别耗时 | 86.7 s（160 次） |
| 被后续更宽裁切取代的中间识别耗时 | 111.0 s（324 次） |
| 中间识别占比 | **56.2%**（全量日志；单次多行会话实测 38.4%，见 §8.6/8.7） |

单行总耗时常见 2–4.5 s，6 行长英文段落约 27 s；厂商引擎约 0.25 s/次，同样的重复识别在厂商引擎下
只花约 1/8 时间。**这是多行场景下 PP-OCR 与厂商引擎差距最大的地方，也是「尾部行在扫描收尾前来不及
识别」的主要嫌疑**（厂商 OCR 线程等待结果时，`paragraphs_OCR` 的发布时机/`yocr_set_running_interrupt(1)`
会让未完成的行被丢弃或留在队列里；single-line 不受影响）。

注意：以上只是根因假设，必须用真机扫描 + 下面第 8.4 节的逐层日志确认；**不要在未确认前改识别算法**。

### 8.3 已加入的可观测性（PenMods 侧，只读、不改变行为）

`src/tweaker/KeyBoard.cpp` 增加 `[ScanDiag]` 日志，每次扫描按 session 序号串起所有层：

| 日志事件 | 层 |
|---|---|
| `onScanStart` | 手势开始 + `continueScan/inputPageShowing/autoSendScan/vendorScanType` |
| `onScanning`（debug） | 多帧拼接过程中的实时文本（每帧变化一条） |
| `onOcrComplete(PRE-JOIN)` | `YCapture` 交给 `YSystemBase` 的原始文本（多行 = 整段） |
| `onScanFinish(RAW)` / `(POST-MERGE)` | 合并前 / 合并后（后者就是 QML 显示/查询的文本） |
| `onContinueScan` / `onStopContinueScan` | 原厂连续扫描追加/清空追加基串 |
| `ocrCompletedResultChanged(DISPLAY)` | 显示文本变更（被拦截时也记一条 `SUPPRESSED`） |
| `showDictPage(DISPLAY)` | 词典页实际拿到的文本（最终显示长度） |

每条都带 `len / utf8 / lines / newlines / blanks / preview`，足够判断「原厂原始文本 → 合并文本 →
显示文本」在哪一步少了句子。日志在发布构建里也可用（`info` 级；只有逐帧的 `onScanning` 是 `debug`）。

`tools/ppocr-backend/regression/` 提供配套工具：

- `gen_samples.py`：生成 6 张可打印多行样张（单行/多行中文/中英混合/行尾标点+空行/重复句/连续扫描），
  打印步骤见 `build/diag/scan/USER-SCANS.md`；
- `collect.sh`：采集 `DictPen_*.log` + `ppocr_shim.log` + 配置快照（PP-OCR 开关、`target_h`、
  `continuescan`、`auto_send_scan`）+ 结果页截图；
- `analyze_scan_diag.py`：按 session 对齐两份日志，输出「shim 识别 / 合并后 / 显示」逐行命中表和
  **最后一次识别完成时刻 vs 扫描收尾时刻**（正值 = 尾部滞后）。

### 8.4 判定规则与实验设计

- 基线：当前配置（PP-OCR 开、`target_h=48`、`continuescan=true`、`auto_send_scan=true`）；
- 差分：`penmods-ppocr.sh disable`（或删 `/userdisk/Qtlib/libyocr.so`）+ 重启 app，用厂商引擎重复同样样张。
  两次都跑同一组样张，才能把「原厂固有问题」和「PP-OCR 引入的问题」分开；
- 判定：`shim识别 ✓ / 合并后 ✗` → 丢在原厂发布层（闭源，只能给可观测性/降级）；
  `合并后 ✓ / 显示 ✗` → 丢在 PenMods/QML（可修）；`尾部滞后 > 0` 且缺最后几行 → 识别慢于扫描收尾。

### 8.5 不能破坏的约束

- 仍然只替换 `yocr_recognize` 这一层；`SegLine`/`seg_middle_line`/检测模型不动；
- OpenCV ROI 必须按 `step.p[0]` 取（非连续 Mat）、通道数按 `cn-1` 解析；
- 网络输出多出的 space 类别必须保留（否则英文粘词）；
- 不改 `target_h` 默认语义（48 = 不再二次缩放）；
- 任何「跳过/降速中间裁切识别」的优化必须用同一组样张验证**最终文本不退化**，并且默认关闭、
  由文件开关显式启用后才实验，不能直接替换现有行为。

### 8.6 真机 12 步回归结果与根因判定（2026-10-26）

在 YDP02X（Exam_V0，firmware 2.1.2，PenMods 诊断构建）上按 `build/diag/scan/USER-SCANS.md`
跑了 6 张样张，日志与报告见 `build/diag/scan/report.md`（574 次 shim 识别、28 个手势、6 段完整发布）。

| 段 | 样张 | 手势 | 最终文本 | 逐手势结果是否都进了最终文本 | 结论 |
|---|---|---|---|---|---|
| 1 | S0 单行 | 1 | 17 字完整 | 1/1 | 无丢失 |
| 2 | S1 多行中文 | 5 | 5 句完整 | 5/5 | 无丢失（4 个换行被 joinString 去掉属设计行为） |
| 3 | S2 中英混合 | 4 | 4 行完整 | 4/4 | 第 1 遍最后一行句末 `.` 在 OCR 层就没识别到；第 2 遍正常 |
| 4 | S3 行尾标点+空行 | 4 | 4 个非空行完整 | 4/4 | 空行后没有丢行 |
| 5 | S4 重复句 | 10 | 整段出现两遍 | 10/10 | 用户两次扫同一张纸（都在 2s 窗口内）→ 厂商累加、**无去重**；第 2 遍 PP-OCR 有 2 处错字 |
| 6 | S2 重复扫描 | 4 | 4 行完整 | 4/4 | 无丢失 |

**结论：这一轮词典多行流程没有任何一层丢句。** PenMods 在普通查词多行流程里是 pass-through，
逐层日志（原厂 `=>OCR Result` → `onOcrComplete` → `onScanFinish(POST-MERGE)` → `showDictPage`）
全部闭环。

**「漏句子」的最可能机制（待 differential 确认）**：厂商多行继续扫描要求「抬笔后 <2 s 内开始下一行」
才累加；实测 PP-OCR 每行结果在抬笔后 **1.4–2.9 s** 才出现，本轮回访用户手势间隔只有 0.6–1.0 s，
所以都成功累加。普通用户会**看到文字出现后再扫下一行**（间隔 >2 s），窗口过期后厂商结束当前段落、
下一行另起新查询，结果页只剩后几行 —— 表现就是「多行漏句子非常严重」。厂商引擎约 0.25 s/行，
用户不会等，所以不触发。验证步骤见 `build/diag/scan/USER-SCANS-2.md`（慢节奏 A/B + 厂商引擎差分）。

### 8.7 shim 侧可做的延迟优化（默认关闭，需样张回归验证）

先量化清楚（同一轮真机日志）：**最终裁切图的识别占 61.6% 时间（28.3 s / 45.9 s）**，这部分是识别
精度所在，不能省；被更宽裁切取代的中间裁切图占 38.4%，其中宽度 <300px 的占 27%。也就是说：
**shim 侧最多只能把每行延迟降 ~20–38%，无法追平厂商引擎的 0.25 s**，所以它是缓解手段，不是根治手段。

已实现的可选开关（默认关闭 = 与原行为逐字节一致）：

```sh
# 打开：同一行连续变宽、且裁切宽度 <=300px 的中间裁切图用 24px 高度识别
adb shell 'echo 24 > /userdisk/ppocr_adaptive_h'      # 16..48，删除即关闭
adb shell 'rm -f /userdisk/ppocr_adaptive_h'          # 回滚
```

约束与依据：

- 只对「同一行、宽度严格变宽、距上次调用 <4 s、宽度 ≤300px、宽度至少增加 20%」的调用降档；
  实测该会话所有**最终**裁切图宽度 ≥308px，因此最终文本的裁切图仍按 `target_h` 全质量识别；
- 无法在线区分「异常短的最终行」与中间裁切图 → 所以是**用户显式开启的实验开关**，不是默认行为；
- 每次降档都会在 `/userdisk/ppocr_shim.log` 写 `adaptive: ...` 和 `recognize[adaptive]: ...`，
  便于统计命中次数与实际节省；
- 验证方法：用 `tools/ppocr-backend/regression/` 的 6 张样张分别在开关关闭/开启下扫描，最终文本必须
  与 ground truth（`samples.json`）逐行一致；再在真实书页上抽 5 页对比。开关关闭时不改变任何现有行为。

**更省事、无代码风险的降级方案**（推荐普通用户）：

| 方案 | 命令/入口 | 效果 |
|---|---|---|
| 关「精度优先」（target_h=32） | 设置 → 实验性功能 → 识别精度优先 关；或 `adb shell 'echo 32 > /userdisk/ppocr_target_h'` | 每行约减半延迟，密集中文可能丢笔画 |
| 多行场景不用 PP-OCR | 设置里关闭，或 `adb shell 'rm -f /userdisk/Qtlib/libyocr.so'` + 重启 app | 多行翻译恢复正常（单行精度让位） |
| 完全卸载 | `sh /userdisk/penmods-ppocr.sh remove` | 回厂商引擎 |

部署/回滚（shim 改动只影响实验后端，不参与 xmake 主构建）：

```sh
cd tools/ppocr-backend
sh scripts/build.sh              # -> build/libyocr.so
sh scripts/deploy.sh on 48       # 原子替换 + loader 预检 + 重启 app
sh scripts/deploy.sh off         # 回滚到厂商引擎并重启
sh scripts/deploy.sh status      # 确认当前加载的是 shim 还是厂商库
```
