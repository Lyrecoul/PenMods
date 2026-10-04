# 实验性 OCR 后端（PP-OCRv5 + ncnn）

把词典笔的**逐行识别**换成了飞桨 PP-OCRv5 mobile 识别模型（跑在 ncnn 上）。扫描链路里的其它环节
（摄像头、拼接、行检测/切分）都不动。

> **状态：实验性、可选、默认关闭。** 它通过在 `LD_LIBRARY_PATH` 里“影子覆盖”厂商的一个 `.so` 来生效，
> 不参与 xmake 构建，也不随 OTA 发布。逆向过程与实测数据见 `doc/PPOCR_BACKEND_ANALYSIS.md`。

## 为什么需要它

出厂引擎（`/oem/YoudaoDictPen/output/aarch64_libs/libyocr.so`）是 CRNN + 一个 150 MB 的 FST 语言模型。
在真实笔扫的中文教材页面上它会读错长句：我们的 A/B 里它把
`学员凭此承诺书进入相关科目考试场，在应考技能考试科目` 读成了 `学员赁进进推的能进科目`。
PP-OCRv5 mobile 能把这些句子读对，代价是延迟更高（每行约 0.2–2 s，视输入高度和长度而定；厂商约 0.25 s）。

## 工作原理

`libYoudaoStitch.so` 只从 `libyocr.so` 导入 6 个符号，其中关键的一个是：

```cpp
std::string yocr_recognize(const cv::Mat& lineCrop);
```

`cv::Mat` 和 `std::string` 都是 libstdc++ / OpenCV 3.4 的 C++ ABI，而 `libPenMods.so` 用 libc++（zig）编译，
所以 PenMods 无法安全地 hook 这个接缝。于是我们**用厂商同款工具链**（同样的 GCC 6.x / glibc 2.27 /
libstdc++ 6.0.22 组合）重新实现这 6 个符号，做成一个 shim 去影子覆盖真正的库，在它收到的行图上跑
PP-OCRv5 mobile rec（ncnn 模型）。

## 安装（推荐方式）

```sh
scripts/fetch-deps.sh && scripts/build.sh && scripts/make-bundle.sh
# -> dist/penmods-ppocr.sh，单文件自包含（约 8 MB：shim + 模型）
```

然后在设备上推送这一个文件并执行：

```sh
adb push dist/penmods-ppocr.sh /userdisk/
adb shell 'sh /userdisk/penmods-ppocr.sh'
```

它会解包、安装、启用，**用动态加载器预检** `libyocr.so` 是否解析到 shim 且没有缺库，然后重启 app 并回报结果。
之后同一个文件就是管理入口：

| 命令 | 作用 |
|---|---|
| `sh penmods-ppocr.sh` | 安装 + 启用（默认） |
| `sh penmods-ppocr.sh status` | 当前安装/生效状态，以及最近几次识别日志 |
| `sh penmods-ppocr.sh disable` | 切回厂商引擎（组件仍保留在设备上） |
| `sh penmods-ppocr.sh enable` | 再次启用 |
| `sh penmods-ppocr.sh h 32` | 输入高度 16..64（48 最准），立即生效 |
| `sh penmods-ppocr.sh remove` | 完整卸载 |

`enable` / `disable` 会重启 app，因为 `libyocr.so` 是动态加载器在**进程启动时**解析的；守护进程会在几秒内
把 app 拉回来。

## 主程序里的开关

安装过组件后，也可以在 PenMods 自己的设置里切换：**更多设置 → 系统调整 → 实验性功能**。

| 开关 | 作用 |
|---|---|
| 使用 PP-OCRv5 识别引擎 | 启用/禁用。会替换影子文件并重启 app（几秒），因为 `libyocr.so` 在进程启动时就被解析了。组件未安装时该行显示“未安装”并置灰 |
| 识别精度优先（较慢） | 输入高度 48 ↔ 32，下一条识别即生效，不需要重启 |

它由 `mod::OcrBackend`（`src/tweaker/OcrBackend.cpp`，context property `ocrBackend`）实现，读取的正是 shim
用的那几个文件（`/userdisk/ppocr_backend`、`/userdisk/Qtlib/libyocr.so`、`/userdisk/ppocr_target_h`），
所以命令行和界面永远一致，不存在第二份会走样的配置。

## 从源码构建

```sh
scripts/fetch-deps.sh     # ncnn 20240820 + PP-OCRv5 mobile rec（缓存到 .cache/，不入库）
scripts/build.sh          # -> build/libyocr.so（以及 build/ab_test）
scripts/make-bundle.sh    # -> dist/penmods-ppocr.sh
```

要求：`aarch64-linux-gnu-g++` 交叉工具链，**带 OpenMP 且 sysroot 是 glibc 2.27**（Arch 上是
`aarch64-linux-gnu-gcc`）。**不能用 zig**：ncnn 每一层都靠 `#pragma omp parallel for` 并行，而 zig 的
`-fopenmp` 没有 `omp.h`/运行时，整个模型会退化成单线程（实测慢 3.8 倍）。

## 开发者路径（`scripts/deploy.sh`）

`scripts/deploy.sh` 直接从构建目录部署，迭代时更顺手：

| 命令 | 作用 |
|---|---|
| `scripts/deploy.sh on [h]` | 推送构建产物 + 模型，启用并重启 |
| `scripts/deploy.sh off` | 回退到厂商引擎 |
| `scripts/deploy.sh h <n>` / `status` / `log` | 调参 / 查看状态 / 查看逐行日志 |

## 设备上的文件

全部在 `/userdisk`，不修改 `/oem` 与系统分区：

| 路径 | 用途 |
|---|---|
| `/userdisk/Qtlib/libyocr.so` | shim；该目录是 app 的 `LD_LIBRARY_PATH` 首项，用来影子覆盖厂商库 |
| `/userdisk/ppocr_backend/libyocr.so` | 安装脚本暂存的 shim 本体（`enable`/`disable` 只切换影子文件） |
| `/userdisk/ppocr_models/PP_OCRv5_mobile_rec.ncnn.{param,bin}` | 模型（8.2 MB） |
| `/userdisk/ppocr_target_h` | 目标高度，16..64（默认 32） |
| `/userdisk/ppocr_shim.log` | 每次识别的日志：输入/输出尺寸、耗时、文本 |

回退就是删掉 `/userdisk/Qtlib/libyocr.so` 再重启 app。

## 精度 / 延迟权衡

`/userdisk/ppocr_target_h` 是主要旋钮。app 在调用识别前已经把每行裁切图统一缩放到 ~48 px 高，所以填 `48`
意味着“不再二次缩放”，填更小的值就是再缩一次。

| target_h | 长行（约 1400×50） | 密集中文精度 |
|---|---|---|
| 48 | ~1.9–2.1 s | 最好，能保住 `流程控制结构` 这类词 |
| 40 | ~1.6 s | 介于两者之间 |
| 32 | ~0.9–1.0 s | 密集中文丢笔画（`流程控制结构` → `流程介绍`） |
| 24 | ~0.3 s | 长行不可用 |

英文对高度不敏感（字母间距大），32 也能全对。

## 评测工具

`build/ab_test` 用**同一张** `cv::Mat` 同时跑厂商引擎和 PP-OCRv5，打印两者的文本与耗时。注意必须在
**厂商库生效**的加载路径下运行（先 `deploy.sh off`，或者 `LD_LIBRARY_PATH` 里不含 `/userdisk/Qtlib`），
否则两边都是 shim：

```sh
scripts/deploy.sh off
adb push build/ab_test /userdisk/ && adb push some.ppm /userdisk/
adb shell 'cd /userdisk && LD_LIBRARY_PATH=/oem/YoudaoDictPen/output/libs \
           ./ab_test ocr_model_pro ocr_model /userdisk/ppocr_models/PP_OCRv5_mobile_rec.ncnn.param \
           /userdisk/ppocr_models/PP_OCRv5_mobile_rec.ncnn.bin /userdisk/some.ppm'
```

## 已经踩过并写进代码的坑

1. OpenCV 的通道数是 `cn - 1` 编码，要用 `((flags >> 3) & 511) + 1`；写成 `(flags >> 3) & 511`
   会把所有 `CV_8UC3` 裁切图判为不支持。
2. 裁切图可能是**非连续 ROI**，必须按 `step.p[0]` 逐行取；按 `cols * cn` 取会越界读未映射页，直接把 app 打崩。
3. 网络输出比字典**多一类**：那一类是空格。丢掉它会让英文单词粘在一起。
4. app 运行中不要用 `adb push` 覆盖 `/userdisk/Qtlib/libyocr.so`（见 `deploy.sh` 里的说明）。
5. 交叉工具链的 glibc 必须是 2.27，且 shim 必须**动态**链接 libstdc++，与 app 共用同一份；静态链接会让
   `std::string` 跨两个不兼容的运行时。
