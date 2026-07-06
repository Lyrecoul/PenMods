// SPDX-License-Identifier: GPL-3.0-only
/*
 * Copyright (C) 2022-present, PenUniverse.
 * This file is part of the PenMods open source project.
 */

#pragma once

enum ScanType : int { POINT, SCAN };

namespace mod {

class KeyBoard : public QObject, public Singleton<KeyBoard> {
    Q_OBJECT
    Q_PROPERTY(bool autoSendScan READ autoSendScan WRITE setAutoSendScan NOTIFY autoSendScanChanged)
    Q_PROPERTY(bool autoSendScanConfig READ autoSendScanConfig WRITE setAutoSendScanConfig NOTIFY autoSendScanConfigChanged)
public:
    bool autoSendScan() const { return m_autoSendScan; }
    void setAutoSendScan(bool value);

    bool autoSendScanConfig() const { return m_autoSendScanConfig; }
    void setAutoSendScanConfig(bool value);

signals:
    void scanFinished(QString result);
    void autoSendScanChanged();
    void autoSendScanConfigChanged();

private:
    friend Singleton<KeyBoard>;
    explicit KeyBoard();
    bool m_autoSendScan = false;
    bool m_autoSendScanConfig = true;
};

} // namespace mod