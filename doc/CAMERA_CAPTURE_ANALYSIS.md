# 摄像头扫描拍照功能逆向分析报告

> 目标: 逆向工程分析有道词典笔摄像头扫描 + 拼图拼接管线，为拍照功能支持提供依据
> 
> 分析日期: 2026-07-01
> 
> 目标文件: `binary/YoudaoDictPen` (ARM64 Linux, Qt 5.15.2)

---

## 1. 摄像头扫描数据流全貌

```
用户按下扫描键
       │
       ▼
YCapture::run() [主循环线程, 0x447094]
       │
       ├─ 信号量等待 (camera_mode_sem)
       │
       ├─ camera_reset_sw_buff() 重置软件缓冲
       │
       ├─ OID 检测阶段 (YCapture::run_OID)
       │
       ├─ camera_set_sw_buff_timeout() 设置超时
       │
       ├─ YCapture::OCR_capture() [核心帧捕获循环, 0x445684]
       │   │
       │   ├─ camera_get_frame(video_hd, 0, &timestamp, &buf, &size)
       │   │   └─ 从摄像头驱动获取一帧 YUYV 原始数据
       │   │
       │   ├─ [调试可选] 保存原始帧到:
       │   │   /userdata/DictPenData/CapData/test_{W}x{H}_{SEQ}_S{FRAME}.yuyv
       │   │
       │   ├─ ydstitch_push_rawdata_new(yuyv_data, &stitched_output, 0)
       │   │   └─ 调用 libYoudaoStitch.so 进行帧拼接/合成
       │   │
       │   ├─ 检测拼接结果是否变化 → 发出 scanning() 信号
       │   │
       │   └─ 循环多帧直到拼接完成
       │
       ├─ saveOCRRawPicFinished() [条件触发]
       │
       ├─ paragraphs_OCR() / lines_OCR() [OCR 识别]
       │
       ├─ saveOCRRawPicStart(path, seqno) [信号: 保存原图]
       │
       └─ ocrComplete(result, seqno) [信号: OCR 完成]
              │
              ▼
         YScanWordsResultLoader (QML)
              │
              ├─ onOcrCompletedResultChanged → 逐字显示
              │
              └─ onOcrStop → searchWord() → entryResult() 查词
```

---

## 2. 关键符号地址

### 2.1 导入函数 (来自 libYoudaoStitch.so)

| 函数名 | 地址 (导入表) | 说明 |
|--------|-------------|------|
| `ydstitch_init` | `0xb9ff98` | 初始化拼接引擎 |
| `ydstitch_uninit` | `0xb9ff88` | 反初始化 |
| `ydstitch_start` | `0xb9ff80` | 开始拼接 |
| `ydstitch_push_rawdata` | `0xb9e638` | 推送原始帧数据 |
| `ydstitch_push_rawdata_new` | `0xb9e630` | **推送原始帧数据(新)** |
| `ydstitch_push_images` | `0xb9ff38` | 推送已处理图像 |
| **`ydstitch_get_img`** | **`0xb9ffd0`** | **获取拼接结果图像** |
| **`ydstitch_save_img`** | **`0xb9ff58`** | **保存拼接图像到文件** |
| `ydstitch_load_ocr` | `0xb9fff8` | 加载 OCR 模块 |
| `ydstitch_release_ocr` | `0xb9ffe0` | 释放 OCR 模块 |
| `ydstitch_get_ocr_result` | `0xb9ffb8` | 获取 OCR 文本结果 |

### 2.2 YCapture 类方法

| 函数名 | 地址 | 大小 | 说明 |
|--------|------|------|------|
| `YCapture::OCR_capture()` | `0x445684` | `0x3c0` | **核心帧捕获循环** |
| `YCapture::run()` | `0x447094` | `0x6e4` | 主循环线程 |
| `YCapture::saveOCRRawPicStart()` | `0x5d6840` | `0x3c` | Qt Signal: 开始保存原图 |
| `YCapture::saveOCRRawPicFinished()` | `0x5d673c` | `0x34` | Qt Signal: 保存完毕 |
| `YCapture::capture()` | — | — | 通用捕获入口 |
| `YCapture::scanning()` | — | — | 通知 QML 扫描中 |

### 2.3 YSystemBase 信号

| 函数名 | 地址 | 说明 |
|--------|------|------|
| `YSystemBase::ocrStart()` | `0x5d9698` | Qt Signal: OCR 开始 |
| `YSystemBase::onScanning()` | — | 扫描进行中 |
| `YSystemBase::onOcrScanning()` | — | OCR 进行中 |
| `YSystemBase::onScanFinish()` | — | 扫描完成 |
| `YSystemBase::onOcrComplete()` | — | OCR 完成 |

### 2.4 摄像头 HAL 接口

| 函数名 | 说明 |
|--------|------|
| `camera_init` | 初始化摄像头 |
| `camera_uninit` | 反初始化 |
| `camera_get_frame` | 获取一帧 YUYV 数据 |
| `camera_set_sw_buff_timeout` | 设置软件缓冲超时 |
| `camera_get_sw_buff_info` | 获取软缓冲信息 |
| `camera_reset_sw_buff` | 重置软缓冲 |

---

## 3. OCR_capture 核心逻辑详解

`YCapture::OCR_capture()` (0x445684, 0x3c0 字节) 是核心函数：

```
while (1) {
    do {
        // 如果有缓存的帧数据则直接使用，否则 camera_get_frame()
        camera_get_frame(video_hd, 0, &timestamp, &buf, &size);
    } while (size == 0);

    // 检查是否需要重启 OCR（帧序号超时判断）
    if (timestamp 超过阈值 && frame_num < max_frame)
        ocr_restart = 1;

    // 保存时间戳
    this->timestamp = timestamp;

    if (ocr_restart) break;  // 跳出循环

    // 环境变量 UPLOAD_SCAN_IMAGE 控制帧保存
    if (env.UPLOAD_SCAN_IMAGE && (frame_num <= 0x2CF || ocr_mode == 10))
        保存帧到 /userdata/DictPenData/CapData/test_...yuyv

    // 调用拼接库
    ydstitch_push_rawdata_new(buf, &stitched_text, 0);
    
    // 拼接结果有变化 → 通知 QML
    if (stitched_text != last_text && !is_continue_scan)
        scanning(stitched_text);
    
    clock_gettime();
}
```

---

## 4. ydstitch_get_img 接口说明

```
int ydstitch_get_img(
    unsigned char **outBuf,   // [输出] 拼接图像 RGB24 数据
    int *outW,                // [输出] 图像宽度
    int *outH                 // [输出] 图像高度
);
```

- 返回值: 0 成功, 非0 失败
- **输出格式**: RGB24 (3 字节/像素, R-G-B 顺序)
- 内存分配: 由库内部管理，调用者无需释放
- 可用状态: 调用 `ydstitch_push_rawdata_new()` 推送帧数据后可用

---

## 5. ydstitch_save_img 接口说明

```cpp
// YDP02X 实机 libYoudaoStitch.so 导出；并不是 int(const char*)。
std::string ydstitch_save_img();
```
- 返回实际保存路径；运行时通过 ARM64 X8 隐藏返回指针接收 libstdc++ 字符串，不能直接跨 libc++ ABI 调用。
- 2026-11-14 从实机读取库后反汇编确认：`ydstitch_save_img` 位于 `0x31a60`；保存路径为 `/userdata/DictPenData/CapData/result_<时间>_<序号>.jpg`。
- `0x320c0–0x32300` 构造输出画布：宽度为两个源图宽度最大值，高度为两个源图高度之和；`crop_result` 放在 `(0, 0)`，`result_middle_line` 放在 `(0, crop_result.rows)`。用户所称“上部原图 / 下部校正图”对应这两个区域，**不是简单对半裁剪**；两图可以不同高、不同宽。此前把下部写成 `all_result` 是错误的：保存函数的 GOT 地址 `0x25ab80` 对应 `result_middle_line`，不是 `all_result` 所在的 `0x25abd0`。
- 两个导出对象都是 `cv::Mat`，其开头的 `int rows/cols` 分别在 `+8/+12`。CameraCapture 只读取这四个整数，不调用 OpenCV C++ 对象；裁剪前校验总高度和最大宽度与实际 JPEG 一致。不匹配时明确报错，不猜测中点或静默保存错误区域。

---

## 6. OCR 完成后的数据流

```
YCapture::run()
    → (条件判断是否保存原图)
      YCapture::saveOCRRawPicStart(path, seqno)  [信号]
      ↕ (Save thread)
      YCapture::saveOCRRawPicFinished(success)   [信号]
    
    → YCapture::ocrComplete(text, seqno) [信号]
    → systemBase.ocrCompletedResult 更新
    → YScanWordsResultLoader 逐字显示
    → onOcrStop → searchWord() → resultManager.entryResult()
```

---

## 7. 关键文件路径

| 路径 | 用途 |
|------|------|
| `/userdata/DictPenData/CapData/test_*.yuyv` | 调试用原始帧保存 |
| `/userdisk/PenMods/capture/` | 推荐拍照保存目录 |

---

## 8. 符号名 (C++ Mangled)

```
// YCapture 类
_ZN8YCapture11OCR_captureEv              ← YCapture::OCR_capture()
_ZN8YCapture3runEv                        ← YCapture::run()
_ZN8YCapture18saveOCRRawPicStartERK7QStringi  ← saveOCRRawPicStart()
_ZN8YCapture21saveOCRRawPicFinishedEb     ← saveOCRRawPicFinished()
_ZN8YCapture11ocrCompleteERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEi  ← ocrComplete()

// YSystemBase 信号
_ZN11YSystemBase8ocrStartEv               ← YSystemBase::ocrStart()

// 拼接库导入 (通过 SymDB 查询)
ydstitch_get_img
ydstitch_save_img
ydstitch_push_rawdata_new

// 摄像头 HAL
camera_get_frame
camera_init
camera_set_sw_buff_timeout
camera_reset_sw_buff
```

## 9. AI 助手连续拍摄

- 进入“拍摄图片”即启动会话；每次扫描直接追加并按默认方向异步拼接，预览时不再需要点击“继续拍摄”。正在拼接时的新扫描进入有界队列，处理结束后按扫描顺序合入。
- “裁剪”可手动编辑最新一帧；编辑时再次扫描会先异步确认当前裁剪，再加入新帧。取消裁剪保留已接受帧；页面返回则取消整个拍摄会话，晚到异步结果不会进入草稿。
- “逐张保存”仍把各帧作为独立附件；“完成保存”返回拼图。最多 8 帧，已存帧和排队帧总计最多 16 MiB base64 字符，达到上限后停止接收并提示，保存或清空后再继续。
- 助手设置中的 `capture.default_stitch_direction`（`vertical` / `horizontal`，默认 `vertical`）和 `capture.default_crop_mode`（`none` / `upper` / `lower`，默认 `upper`）持久化。默认仅输出上部原图，排除同一扫描的下部校正副本；完整上下两图仍可显式选择。默认区域在原生扫描图读取后、JPEG 编码前裁剪，后续手动裁剪基于该区域。`captureEnabled` 是页面拥有的临时状态，不随设置写盘。
- `capture.region_defaults_version=1` 标记默认区域的一次性迁移：旧配置为 `none` 或缺少区域时改为 `upper`，已选 `upper` / `lower` 保留。迁移后主动选择 `none` 不会在重启时被覆盖。旧版无法区分主动选择 `none` 与旧默认值，因此首次均迁移。
- 前次注入测试仅验证了给定区域后的裁剪编码，且把 `all_result` 同时当作下部测试对象，未覆盖厂商真实保存函数，无法证明真实扫描可用。随后用户扫描触发区域失配：原图 `crop_result=178×109`，校正图 `result_middle_line=161×55`，保存 JPEG 为 `178×164`，而 `all_result=244×109`。修复改为读取 `result_middle_line`，保留严格尺寸校验，不用猜测比例或退回完整重复图来掩盖错误。
- 修复验证使用用户失败的公式 JPEG 和实测的三组矩阵尺寸，在设备内调用真实 OpenCV 矩阵构造、真实 `ydstitch_save_img` 和实际 `onScanComplete()`，未替换保存函数。旧库原图/校正图均报区域错误；修复后分别输出 178×109、161×55、178×164，三种模式通过。
