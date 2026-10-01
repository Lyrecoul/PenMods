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
    // 键盘按键布局: "native"(原生) / "compact"(紧凑, 字母键使用 QWERTY 排列)
    Q_PROPERTY(QString keyboardLayout READ keyboardLayout WRITE setKeyboardLayout NOTIFY keyboardLayoutChanged)
public:
    bool inputPageShowing() const { return m_inputPageShowing; }
    void setInputPageShowing(bool value);

    bool autoSendScan() const { return m_autoSendScan; }
    void setAutoSendScan(bool value);

    bool autoSendScanConfig() const { return m_autoSendScanConfig; }
    void setAutoSendScanConfig(bool value);

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
    void keyboardLayoutChanged();

private:
    friend Singleton<KeyBoard>;
    explicit KeyBoard();
    bool    m_inputPageShowing   = false;
    bool    m_autoSendScan       = false;
    bool    m_autoSendScanConfig = true;
    bool    m_startingVoiceInput = false;
    QString     m_keyboardLayout = "native";
    std::string mClassName       = "keyboard";
    json        mCfg;
};

} // namespace mod
