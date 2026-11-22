// SPDX-License-Identifier: GPL-3.0-only
/*
 * Copyright (C) 2022-present, PenUniverse.
 * This file is part of the PenMods open source project.
 */

#pragma once

#include "mod/Config.h"

enum ScanType : int { POINT, SCAN };

namespace mod {

class KeyBoard : public QObject, public Singleton<KeyBoard> {
    Q_OBJECT
    Q_PROPERTY(bool inputPageShowing READ inputPageShowing WRITE setInputPageShowing NOTIFY inputPageShowingChanged)
    Q_PROPERTY(bool autoSendScan READ autoSendScan WRITE setAutoSendScan NOTIFY autoSendScanChanged)
    Q_PROPERTY(bool autoSendScanConfig READ autoSendScanConfig WRITE setAutoSendScanConfig NOTIFY autoSendScanConfigChanged)
    // 扫描结束后的默认行为：send / input / capture / off
    //   send    = 识别文字并直接发送（旧「扫描结果快速发送」开启）
    //   input   = 识别文字追加到输入框，不发送
    //   capture = 跳转「拍摄图片」并把这次扫描的图作为第一张
    //   off     = 不拦截，走原厂扫描（旧「扫描结果快速发送」关闭）
    Q_PROPERTY(QString scanBehavior READ scanBehavior WRITE setScanBehavior NOTIFY scanBehaviorChanged)
    // 是否拦截扫描（scanBehavior != off）；聊天页进入/离开时用它置运行期闸门 autoSendScan。
    Q_PROPERTY(bool scanInterceptConfig READ scanInterceptConfig NOTIFY scanBehaviorChanged)
    // 键盘按键布局: "native"(原生) / "compact"(紧凑, 字母键使用 QWERTY 排列)
    Q_PROPERTY(QString keyboardLayout READ keyboardLayout WRITE setKeyboardLayout NOTIFY keyboardLayoutChanged)
public:
    bool inputPageShowing() const { return m_inputPageShowing; }
    void setInputPageShowing(bool value);

    bool autoSendScan() const { return m_autoSendScan; }
    void setAutoSendScan(bool value);

    bool autoSendScanConfig() const { return m_scanBehavior == "send"; }
    void setAutoSendScanConfig(bool value);

    QString scanBehavior() const { return m_scanBehavior; }
    void    setScanBehavior(const QString& value);
    bool    scanInterceptConfig() const { return m_scanBehavior != "off"; }

    QString keyboardLayout() const { return m_keyboardLayout; }
    void    setKeyboardLayout(const QString& value);

    Q_INVOKABLE bool startVoiceInput(QObject* speechManager);
    Q_INVOKABLE bool stopVoiceInput(QObject* speechManager);

    bool isStartingVoiceInput() const { return m_startingVoiceInput; }

signals:
    void scanFinished(QString result);
    void inputPageShowingChanged();
    void autoSendScanChanged();
    void autoSendScanConfigChanged();
    void scanBehaviorChanged();
    void keyboardLayoutChanged();

private:
    friend Singleton<KeyBoard>;
    explicit KeyBoard();
    bool    m_inputPageShowing   = false;
    bool    m_autoSendScan       = false;
    QString m_scanBehavior        = "send";
    bool    m_startingVoiceInput = false;
    QString     m_keyboardLayout = "native";
    std::string mClassName       = "keyboard";
    json        mCfg;
};

} // namespace mod
