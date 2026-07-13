// SPDX-License-Identifier: GPL-3.0-only
/*
 * Copyright (C) 2022-present, PenUniverse.
 * This file is part of the PenMods open source project.
 */

#pragma once

#include "mod/Config.h"

#include <QString>
#include <QStringList>

namespace mod::capture {

class CameraCapture : public QObject, public Singleton<CameraCapture> {
    Q_OBJECT

    Q_PROPERTY(bool captureEnabled READ isCaptureEnabled WRITE setCaptureEnabled NOTIFY captureEnabledChanged)

public:
    [[nodiscard]] bool isCaptureEnabled() const;

    void setCaptureEnabled(bool val);

    void onScanComplete();

    Q_INVOKABLE QString cropImage(const QString& base64Data, int x, int y, int w, int h);

    Q_INVOKABLE QString stitchImages(const QStringList& imageList, const QString& direction);

signals:

    void captureEnabledChanged();

    void imageCaptured(const QString& base64Data);

private:
    friend Singleton<CameraCapture>;
    explicit CameraCapture();

    std::string mClassName = "capture";
    json        mCfg;
    bool        mCaptureEnabled;
};

} // namespace mod::capture
