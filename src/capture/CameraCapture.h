// SPDX-License-Identifier: GPL-3.0-only
/*
 * Copyright (C) 2022-present, PenUniverse.
 * This file is part of the PenMods open source project.
 */

#pragma once

#include "mod/Config.h"

#include <QString>
#include <QStringList>
#include <atomic>

namespace mod::capture {

class CameraCapture : public QObject, public Singleton<CameraCapture> {
    Q_OBJECT

    Q_PROPERTY(bool captureEnabled READ isCaptureEnabled WRITE setCaptureEnabled NOTIFY captureEnabledChanged)
    Q_PROPERTY(
        QString defaultStitchDirection READ defaultStitchDirection WRITE setDefaultStitchDirection NOTIFY
            defaultsChanged
    )
    Q_PROPERTY(QString defaultCropMode READ defaultCropMode WRITE setDefaultCropMode NOTIFY defaultsChanged)

public:
    [[nodiscard]] bool isCaptureEnabled() const;

    void setCaptureEnabled(bool val);

    [[nodiscard]] QString defaultStitchDirection() const;
    [[nodiscard]] QString defaultCropMode() const;
    void                  setDefaultStitchDirection(const QString& direction);
    void                  setDefaultCropMode(const QString& mode);

    void onScanComplete();

    Q_INVOKABLE QString cropImage(const QString& base64Data, int x, int y, int w, int h);

    Q_INVOKABLE QString stitchImages(const QStringList& imageList, const QString& direction);

    Q_INVOKABLE void cropImageAsync(quint64 requestId, const QString& base64Data, int x, int y, int w, int h);

    Q_INVOKABLE void stitchImagesAsync(quint64 requestId, const QStringList& imageList, const QString& direction);

signals:

    void captureEnabledChanged();

    void defaultsChanged();

    void captureFailed(const QString& errorMessage);

    void imageCaptured(const QString& base64Data);

    void cropImageCompleted(quint64 requestId, const QString& base64Data, const QString& errorMessage);

    void stitchImagesCompleted(quint64 requestId, const QString& base64Data, const QString& errorMessage);

private:
    friend Singleton<CameraCapture>;
    explicit CameraCapture();

    std::string      mClassName = "capture";
    json             mCfg;
    std::atomic_bool mCaptureEnabled{false};
    std::atomic_int  mDefaultCropMode{1};
};

} // namespace mod::capture
