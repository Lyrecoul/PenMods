// SPDX-License-Identifier: GPL-3.0-only
/*
 * Copyright (C) 2022-present, PenUniverse.
 * This file is part of the PenMods open source project.
 */

#pragma once

#include "common/service/Logger.h"
#include "mod/Config.h"

namespace mod {

/**
 * @brief 实验性 OCR 后端(PP-OCRv5 + ncnn)的开关
 *
 * 实际实现见 tools/ppocr-backend/：安装脚本会把 shim(顶替厂商 libyocr.so 的
 * 识别引擎)和模型放到 /userdisk,这里只负责开关和调参,方便用户在设置界面里切换。
 *
 * `libyocr.so` 是动态加载器在**进程启动时**解析的,所以切换引擎必须重启 app;
 * setEnabled() 会改好文件,然后延迟一秒让界面先刷新,再让守护进程把 app 拉起来。
 */
class OcrBackend : public QObject, public Singleton<OcrBackend>, private Logger {
    Q_OBJECT

    Q_PROPERTY(bool installed READ installed NOTIFY stateChanged)
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY stateChanged)
    Q_PROPERTY(bool accuracyFirst READ accuracyFirst WRITE setAccuracyFirst NOTIFY stateChanged)

public:
    /// 安装包(shim + 模型)是否已经就位
    [[nodiscard]] bool installed() const;

    /// 当前 app 进程启动时是否加载了 shim
    [[nodiscard]] bool enabled() const;

    /// true: target height 48(更准更慢); false: 32(更快,密集中文会丢笔画)
    [[nodiscard]] bool accuracyFirst() const;

    void setEnabled(bool value);

    void setAccuracyFirst(bool value);

signals:

    void stateChanged();

private:
    friend Singleton<OcrBackend>;
    explicit OcrBackend();

    void restartApplication();
};

} // namespace mod
