// SPDX-License-Identifier: GPL-3.0-only
/*
 * Copyright (C) 2022-present, PenUniverse.
 * This file is part of the PenMods open source project.
 */

#include "capture/CameraCapture.h"

#include "common/Event.h"

#include <QBuffer>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QImageReader>
#include <QMetaObject>
#include <QPainter>
#include <QQmlContext>
#include <QQuickView>
#include <QRunnable>
#include <QThreadPool>

#include <cstring>
#include <dlfcn.h>
#include <limits>
#include <spdlog/spdlog.h>
#include <type_traits>
#include <utility>

namespace mod::capture {

namespace {

constexpr int    MaxBase64Chars          = 16 * 1024 * 1024;
constexpr int    MaxStitchBase64Chars    = 24 * 1024 * 1024;
constexpr qint64 MaxImagePixels          = 12 * 1024 * 1024;
constexpr qint64 MaxCompositeImagePixels = 16 * 1024 * 1024;
// Raw (still encoded) frames queued for the scan worker before a frame is processed
// inline instead. Only reached when scans arrive faster than they can be decoded.
constexpr qint64 MaxPendingScanBytes = 24 * 1024 * 1024;

struct ImageProcessResult {
    QString data;
    QString error;
};

template <typename Function>
class ImageTask final : public QRunnable {
public:
    explicit ImageTask(Function function) : mFunction(std::move(function)) {}

    void run() override { mFunction(); }

private:
    Function mFunction;
};

template <typename Function>
QRunnable* makeImageTask(Function&& function) {
    using Task = ImageTask<std::decay_t<Function>>;
    return new Task(std::forward<Function>(function));
}

ImageProcessResult decodeImage(const QString& base64Data, QImage& image) {
    if (base64Data.isEmpty()) {
        return {{}, "图片数据为空"};
    }
    if (base64Data.size() > MaxBase64Chars) {
        return {{}, "图片数据过大"};
    }

    QByteArray decoded = QByteArray::fromBase64(base64Data.toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
    if (decoded.isEmpty() || !image.loadFromData(decoded)) {
        return {{}, "图片解码失败"};
    }
    if (qint64(image.width()) * image.height() > MaxImagePixels) {
        image = {};
        return {{}, "图片分辨率过高"};
    }
    return {};
}

ImageProcessResult cropImageImpl(const QString& base64Data, int x, int y, int w, int h) {
    QImage image;
    auto   decoded = decodeImage(base64Data, image);
    if (!decoded.error.isEmpty()) {
        return decoded;
    }

    x = std::max(0, std::min(image.width() - 1, x));
    y = std::max(0, std::min(image.height() - 1, y));
    w = std::max(1, std::min(image.width() - x, w));
    h = std::max(1, std::min(image.height() - y, h));

    QImage     cropped = image.copy(x, y, w, h);
    QByteArray ba;
    QBuffer    buffer(&ba);
    buffer.open(QIODevice::WriteOnly);
    if (!cropped.save(&buffer, "JPEG", 90)) {
        return {{}, "裁剪图片编码失败"};
    }
    return {QString::fromLatin1(ba.toBase64()), {}};
}

ImageProcessResult stitchImagesImpl(const QStringList& imageList, const QString& direction) {
    if (imageList.size() < 2) {
        return {{}, "至少需要两张图片"};
    }
    if (direction != "horizontal" && direction != "vertical") {
        return {{}, "拼接方向无效"};
    }

    qint64 totalBase64Chars = 0;
    for (const auto& imageData : imageList) {
        totalBase64Chars += imageData.size();
        if (totalBase64Chars > MaxStitchBase64Chars) {
            return {{}, "待拼接图片总大小过大"};
        }
    }

    const bool horizontal  = direction == "horizontal";
    qint64     totalWidth  = 0;
    qint64     totalHeight = 0;
    int        maxWidth    = 0;
    int        maxHeight   = 0;

    QVector<QImage> images;
    images.reserve(imageList.size());
    for (const auto& imageData : imageList) {
        QImage image;
        auto   decoded = decodeImage(imageData, image);
        if (!decoded.error.isEmpty()) {
            return decoded;
        }

        if (horizontal) {
            totalWidth  += image.width();
            maxHeight    = std::max(maxHeight, image.height());
            totalHeight  = maxHeight;
        } else {
            totalHeight += image.height();
            maxWidth     = std::max(maxWidth, image.width());
            totalWidth   = maxWidth;
        }
        if (totalWidth <= 0 || totalHeight <= 0 || totalWidth * totalHeight > MaxCompositeImagePixels
            || totalWidth > std::numeric_limits<int>::max() || totalHeight > std::numeric_limits<int>::max()) {
            return {{}, "拼接结果尺寸过大"};
        }
        images.append(std::move(image));
    }

    QImage composite(static_cast<int>(totalWidth), static_cast<int>(totalHeight), QImage::Format_RGB32);
    if (composite.isNull()) {
        return {{}, "无法分配拼接图片内存"};
    }
    composite.fill(Qt::white);
    QPainter painter(&composite);

    int offset = 0;
    for (const auto& image : images) {
        if (horizontal) {
            painter.drawImage(offset, 0, image);
            offset += image.width();
        } else {
            painter.drawImage(0, offset, image);
            offset += image.height();
        }
    }
    painter.end();

    QByteArray ba;
    QBuffer    buffer(&ba);
    buffer.open(QIODevice::WriteOnly);
    if (!composite.save(&buffer, "JPEG", 90)) {
        return {{}, "拼接图片编码失败"};
    }
    return {QString::fromLatin1(ba.toBase64()), {}};
}

} // namespace

// ── call ydstitch_save_img across ABI boundary ─────────────────────
//
// libYoudaoStitch.so's ydstitch_save_img() returns libstdc++ std::string
// (32 bytes on ARM64) via X8 hidden pointer (Itanium C++ ABI).
// PenMods uses libc++, so we cannot use std::string directly.
//
// A 32-byte POD struct triggers the same X8 calling convention on ARM64
// (AAPCS64: structs > 16 bytes returned via hidden pointer).

struct StitchString {
    char data[32];
};

static_assert(sizeof(StitchString) == 32, "must match libstdc++ std::string size");

static bool parseStitchString(const StitchString& s, std::string& out) {
    const char* data   = nullptr;
    size_t      length = 0;
    std::memcpy(&data, s.data, 8);
    std::memcpy(&length, s.data + 8, 8);

    uint64 dataAddr = reinterpret_cast<uint64>(data);
    uint64 objAddr  = reinterpret_cast<uint64>(&s);

    if (dataAddr >= objAddr && dataAddr < objAddr + sizeof(StitchString)) {
        data = s.data + 16;
    }

    if (!data || length == 0) {
        return false;
    }
    out.assign(data, length);
    return true;
}

static bool callYdstitchSaveImg(std::string& outPath) {
    static void* sym = nullptr;
    if (!sym) {
        sym = dlsym(RTLD_DEFAULT, "ydstitch_save_img");
        if (!sym) {
            static bool once = false;
            if (!once) {
                once = true;
                spdlog::warn("[CameraCapture] ydstitch_save_img not found.");
            }
            return false;
        }
    }

    using Fn = StitchString (*)();
    auto fn  = reinterpret_cast<Fn>(sym);

    StitchString result = fn();

    const bool valid = parseStitchString(result, outPath);
    // The returned path owns libstdc++ storage. Destroy it with its own ABI,
    // otherwise every continuous scan leaks the native path allocation.
    using StringDestructor    = void (*)(StitchString*);
    static auto destroyString = reinterpret_cast<StringDestructor>(
        dlsym(RTLD_DEFAULT, "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEED1Ev")
    );
    if (destroyString) destroyString(&result);
    if (!valid) {
        spdlog::warn("[CameraCapture] ydstitch_save_img returned empty/invalid path.");
        return false;
    }
    spdlog::info("[CameraCapture] ydstitch_save_img -> {}", outPath);
    return true;
}

// ── CameraCapture ──────────────────────────────────────────────────

CameraCapture::CameraCapture() {
    mCfg = Config::getInstance().read(mClassName);
    // Capture is a page-owned runtime state, never a persisted preference.
    mCaptureEnabled     = false;
    const auto cropMode = mCfg.value("default_crop_mode", std::string("upper"));
    mDefaultCropMode    = cropMode == "upper" ? 1 : cropMode == "lower" ? 2 : 0;

    // Scan frames are decoded/re-encoded here instead of on the capture thread. A single
    // worker keeps them in capture order (QThreadPool runs queued tasks FIFO) and never
    // holds more than one decoded frame at a time.
    mScanPool.setMaxThreadCount(1);

    connect(&Event::getInstance(), &Event::beforeUiInitialization, [this](QQuickView& view, QQmlContext* context) {
        context->setContextProperty("cameraCapture", this);
    });
}

bool CameraCapture::isCaptureEnabled() const { return mCaptureEnabled; }

void CameraCapture::setCaptureEnabled(bool val) {
    if (mCaptureEnabled.exchange(val) != val) {
        emit captureEnabledChanged();
    }
}

QString CameraCapture::defaultStitchDirection() const {
    return mCfg.value("default_stitch_direction", std::string("vertical")) == "horizontal" ? "horizontal" : "vertical";
}

QString CameraCapture::defaultCropMode() const {
    const int mode = mDefaultCropMode.load();
    return mode == 1 ? "upper" : mode == 2 ? "lower" : "none";
}

void CameraCapture::setDefaultStitchDirection(const QString& direction) {
    if ((direction != "horizontal" && direction != "vertical") || direction == defaultStitchDirection()) return;
    UPDATE_CFG("default_stitch_direction", direction.toStdString());
    emit defaultsChanged();
}

void CameraCapture::setDefaultCropMode(const QString& mode) {
    if ((mode != "upper" && mode != "lower" && mode != "none") || mode == defaultCropMode()) return;
    mDefaultCropMode = mode == "upper" ? 1 : mode == "lower" ? 2 : 0;
    UPDATE_CFG("default_crop_mode", mode.toStdString());
    emit defaultsChanged();
}

void CameraCapture::onScanComplete() {
    if (!mCaptureEnabled) {
        return;
    }

    // One sequence number per scan, taken before anything can fail: imageCaptured and
    // captureFailed are then emitted in exactly the order the scans happened, even though
    // most of the work now runs on the scan worker (the old code emitted inline, in order).
    const quint64 seq = mScanSeq++;
    QElapsedTimer captureTimer;
    captureTimer.start();

    std::string origPath;
    if (!callYdstitchSaveImg(origPath)) {
        postScanResult(seq, {}, "扫描图片保存失败，请重新扫描");
        return;
    }

    // Snapshot the vendor's file instead of decoding it here. The vendor reuses that path
    // for the next scan, so the bytes have to be taken now, but a plain read costs a
    // fraction of the decode + q85 re-encode that used to block the next frame.
    QByteArray raw;
    {
        QFile file(QString::fromStdString(origPath));
        if (!file.open(QIODevice::ReadOnly)) {
            spdlog::warn("[CameraCapture] onScanComplete: failed to open {}", origPath);
            postScanResult(seq, {}, "扫描图片读取失败，请重新扫描");
            return;
        }
        raw = file.readAll();
    }
    if (raw.isEmpty()) {
        spdlog::warn("[CameraCapture] onScanComplete: failed to read image");
        postScanResult(seq, {}, "扫描图片读取失败，请重新扫描");
        return;
    }

    // Only the dimensions are needed here: the pixel cap and the crop geometry must both be
    // decided on this thread (the vendor's crop_result globals belong to *this* frame), and
    // neither needs the pixels.
    QSize frameSize;
    {
        QBuffer header(&raw);
        header.open(QIODevice::ReadOnly);
        QImageReader reader(&header);
        if (reader.canRead()) {
            frameSize = reader.size();
        }
    }
    if (frameSize.isEmpty()) {
        // Header probe could not report a size (exotic container). Decode once to obtain the
        // dimensions the old code read, so the checks below keep identical outcomes.
        QImage probe;
        if (!probe.loadFromData(raw) || probe.isNull()) {
            spdlog::warn("[CameraCapture] onScanComplete: failed to load image");
            postScanResult(seq, {}, "扫描图片读取失败，请重新扫描");
            return;
        }
        frameSize = probe.size();
    }
    if (frameSize.isEmpty() || qint64(frameSize.width()) * frameSize.height() > MaxImagePixels) {
        spdlog::warn("[CameraCapture] onScanComplete: image resolution is too high");
        postScanResult(seq, {}, "扫描图片分辨率过高");
        return;
    }

    const int cropMode = mDefaultCropMode.load();
    QRect     cropRect;
    if (cropMode != 0) {
        // Native save_img stacks crop_result above result_middle_line, not all_result.
        // Only read cv::Mat's stable initial int fields; never cross its C++ ABI.
        const auto* upper       = static_cast<const char*>(dlsym(RTLD_DEFAULT, "crop_result"));
        const auto* lower       = static_cast<const char*>(dlsym(RTLD_DEFAULT, "result_middle_line"));
        int         upperHeight = 0, upperWidth = 0, lowerHeight = 0, lowerWidth = 0;
        if (upper && lower) {
            std::memcpy(&upperHeight, upper + 8, sizeof(int));
            std::memcpy(&upperWidth, upper + 12, sizeof(int));
            std::memcpy(&lowerHeight, lower + 8, sizeof(int));
            std::memcpy(&lowerWidth, lower + 12, sizeof(int));
        }
        if (upperHeight <= 0 || lowerHeight <= 0 || upperWidth <= 0 || lowerWidth <= 0
            || qint64(upperHeight) + lowerHeight != frameSize.height()
            || std::max(upperWidth, lowerWidth) != frameSize.width()) {
            // 这里以前直接失败（"无法确定原图与校正图区域"）。但落图的目的是把这一次扫描的图交给
            // 拍摄页，裁剪区域只是"少留一段重复画面"的优化：几何读不到时用整图，比丢掉整张图好得多
            // （真机反馈的"约一半扫描取不到图"里就有这一条）。cropRect 保持空 = 不裁剪。
            spdlog::warn(
                "[CameraCapture] scan {} 裁剪区域与整图尺寸不一致（{}x{} vs {}x{}），改为使用整图",
                seq,
                upperWidth,
                upperHeight,
                frameSize.width(),
                frameSize.height()
            );
            cropRect = QRect();
        } else {
            cropRect =
                cropMode == 1 ? QRect(0, 0, upperWidth, upperHeight) : QRect(0, upperHeight, lowerWidth, lowerHeight);
        }
    }

    dispatchScanFrame(seq, raw, cropRect);
    // Per-scan timing on the capture thread: this used to include the JPEG decode and the
    // q85 re-encode (the expensive part, now on the worker).
    spdlog::info(
        "[CameraCapture] scan {} accepted in {} ms ({} KB raw)",
        seq,
        captureTimer.elapsed(),
        raw.size() / 1024
    );
}

void CameraCapture::dispatchScanFrame(quint64 seq, QByteArray rawData, const QRect& cropRect) {
    const qint64 frameBytes = rawData.size();
    // Bounded memory: when the worker is backed up (rapid scanning) decode this frame on
    // the calling thread instead of queueing one more undecoded frame. The result is the
    // same; emitScanResult() keeps the emission order intact either way.
    if (mPendingScanBytes.load(std::memory_order_relaxed) + frameBytes > MaxPendingScanBytes) {
        spdlog::warn("[CameraCapture] onScanComplete: scan worker is busy, decoding inline");
        processScanFrame(seq, rawData, cropRect);
        return;
    }

    mPendingScanBytes.fetch_add(frameBytes, std::memory_order_relaxed);
    mScanPool.start(makeImageTask([this, seq, rawData = std::move(rawData), cropRect, frameBytes]() {
        QElapsedTimer decodeTimer;
        decodeTimer.start();
        processScanFrame(seq, rawData, cropRect);
        mPendingScanBytes.fetch_sub(frameBytes, std::memory_order_relaxed);
        spdlog::info("[CameraCapture] scan {} decoded/encoded in {} ms", seq, decodeTimer.elapsed());
    }));
}

void CameraCapture::processScanFrame(quint64 seq, const QByteArray& rawData, const QRect& cropRect) {
    QImage img;
    if (!img.loadFromData(rawData) || img.isNull()) {
        postScanResult(seq, {}, "扫描图片读取失败，请重新扫描");
        return;
    }
    if (qint64(img.width()) * img.height() > MaxImagePixels) {
        postScanResult(seq, {}, "扫描图片分辨率过高");
        return;
    }

    if (!cropRect.isNull()) {
        // The rect came from the header dimensions of these very bytes; clamp against the
        // decoded image so a size mismatch can never read outside the frame.
        const int x = std::max(0, std::min(img.width() - 1, cropRect.x()));
        const int y = std::max(0, std::min(img.height() - 1, cropRect.y()));
        const int w = std::max(1, std::min(img.width() - x, cropRect.width()));
        const int h = std::max(1, std::min(img.height() - y, cropRect.height()));
        img         = img.copy(x, y, w, h);
    }

    QByteArray ba;
    QBuffer    buffer(&ba);
    buffer.open(QIODevice::WriteOnly);
    if (!img.save(&buffer, "JPEG", 85) || ba.size() > (MaxBase64Chars / 4) * 3) {
        postScanResult(seq, {}, "扫描图片编码失败或图片过大");
        return;
    }
    postScanResult(seq, QString::fromLatin1(ba.toBase64()), {});
}

void CameraCapture::postScanResult(quint64 seq, const QString& data, const QString& error) {
    // QString is implicitly shared, so this hand-off does not copy the payload.
    QMetaObject::invokeMethod(
        this,
        [this, seq, data, error]() { emitScanResult(seq, data, error); },
        Qt::QueuedConnection
    );
}

void CameraCapture::emitScanResult(quint64 seq, const QString& data, const QString& error) {
    mPendingScanResults.insert(seq, qMakePair(data, error));
    // Emit strictly in scan order: a frame processed inline must not overtake frames that
    // are still decoding on the worker (the old code emitted inside the capture hook).
    while (true) {
        auto it = mPendingScanResults.find(mNextScanEmit);
        if (it == mPendingScanResults.end()) {
            break;
        }
        const auto result = it.value();
        mPendingScanResults.erase(it);
        ++mNextScanEmit;
        if (!result.second.isEmpty()) {
            spdlog::warn("[CameraCapture] onScanComplete: {}", result.second.toStdString());
            emit captureFailed(result.second);
        } else {
            // QML 在 onImageCaptured 槽里同步读 lastImageSeq()，用来判断这张图是不是"这一次扫描"的。
            mLastImageSeq = seq;
            emit imageCaptured(result.first);
        }
    }
}

QString CameraCapture::cropImage(const QString& base64Data, int x, int y, int w, int h) {
    auto result = cropImageImpl(base64Data, x, y, w, h);
    if (!result.error.isEmpty()) {
        spdlog::warn("[CameraCapture] cropImage: {}", result.error.toStdString());
    }
    return result.data;
}

QString CameraCapture::stitchImages(const QStringList& imageList, const QString& direction) {
    auto result = stitchImagesImpl(imageList, direction);
    if (!result.error.isEmpty()) {
        spdlog::warn("[CameraCapture] stitchImages: {}", result.error.toStdString());
    }
    return result.data;
}

void CameraCapture::cropImageAsync(quint64 requestId, const QString& base64Data, int x, int y, int w, int h) {
    QThreadPool::globalInstance()->start(makeImageTask([this, requestId, base64Data, x, y, w, h]() {
        auto result = cropImageImpl(base64Data, x, y, w, h);
        QMetaObject::invokeMethod(
            this,
            [this, requestId, result = std::move(result)]() {
                emit cropImageCompleted(requestId, result.data, result.error);
            },
            Qt::QueuedConnection
        );
    }));
}

void CameraCapture::stitchImagesAsync(quint64 requestId, const QStringList& imageList, const QString& direction) {
    QThreadPool::globalInstance()->start(makeImageTask([this, requestId, imageList, direction]() {
        auto result = stitchImagesImpl(imageList, direction);
        QMetaObject::invokeMethod(
            this,
            [this, requestId, result = std::move(result)]() {
                emit stitchImagesCompleted(requestId, result.data, result.error);
            },
            Qt::QueuedConnection
        );
    }));
}

} // namespace mod::capture

// ── Hooks ──────────────────────────────────────────────────────────

PEN_HOOK(uint64, _ZN8YCapture9lines_OCREii, uint64 self, uint32 a2, uint32 a3) {
    uint64 ret = origin(self, a2, a3);
    mod::capture::CameraCapture::getInstance().onScanComplete();
    return ret;
}

PEN_HOOK(uint64, _ZN8YCapture14paragraphs_OCREii, uint64 self, uint32 a2, uint32 a3) {
    uint64 ret = origin(self, a2, a3);
    mod::capture::CameraCapture::getInstance().onScanComplete();
    return ret;
}
