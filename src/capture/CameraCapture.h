// SPDX-License-Identifier: GPL-3.0-only
/*
 * Copyright (C) 2022-present, PenUniverse.
 * This file is part of the PenMods open source project.
 */

#pragma once

#include "mod/Config.h"

#include <QByteArray>
#include <QHash>
#include <QPair>
#include <QRect>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QThreadPool>
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

    /// 最近一次 `imageCaptured` 携带的扫描序号（每次 onScanComplete 自增一次）。
    /// QML 在 onImageCaptured 里同步读取它就能把落图和"这一次扫描"对上，从而丢弃上一次
    /// 扫描排队到现在的残留图（异步落图 + 连续扫描时，先到的图不一定是本次扫描的）。
    [[nodiscard]] Q_INVOKABLE quint64 lastImageSeq() const { return mLastImageSeq; }

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

    // Scan frames are validated (cheap, header only) on the capture thread and then
    // decoded/cropped/re-encoded on mScanPool. Every result walks through
    // emitScanResult() so imageCaptured/captureFailed stay in scan order even if a
    // frame had to be processed inline.
    void dispatchScanFrame(quint64 seq, QByteArray rawData, const QRect& cropRect);
    void processScanFrame(quint64 seq, const QByteArray& rawData, const QRect& cropRect);
    void postScanResult(quint64 seq, const QString& data, const QString& error);
    void emitScanResult(quint64 seq, const QString& data, const QString& error);

    std::string      mClassName = "capture";
    json             mCfg;
    std::atomic_bool mCaptureEnabled{false};
    std::atomic_int  mDefaultCropMode{1};

    // One worker only: QThreadPool runs queued tasks in submission order, so frames
    // complete in capture order and at most one decoded frame is alive at a time.
    QThreadPool          mScanPool;
    std::atomic<qint64>  mPendingScanBytes{0};
    std::atomic<quint64> mScanSeq{0};
    // UI-thread only: results waiting for their predecessor so emission order == scan order.
    quint64                                 mNextScanEmit = 0;
    QHash<quint64, QPair<QString, QString>> mPendingScanResults;
    // UI-thread only: 最近一次 imageCaptured 的序号（见 lastImageSeq()）。
    quint64 mLastImageSeq = 0;
};

} // namespace mod::capture
