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

```
int ydstitch_save_img(
    const char *filepath      // [输入] 保存路径
);
```

- 直接将当前拼接结果保存为图像文件
- 输出格式由库根据文件扩展名决定

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