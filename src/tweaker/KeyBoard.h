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
public:
    bool autoSendScan() const { return m_autoSendScan; }
    void setAutoSendScan(bool value);

signals:
    void scanFinished(QString result);
    void autoSendScanChanged();

private:
    friend Singleton<KeyBoard>;
    explicit KeyBoard();
    bool m_autoSendScan = false;
};

} // namespace mod