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
