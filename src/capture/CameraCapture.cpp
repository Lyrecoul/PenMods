// SPDX-License-Identifier: GPL-3.0-only
/*
 * Copyright (C) 2022-present, PenUniverse.
 * This file is part of the PenMods open source project.
 */

#include "capture/CameraCapture.h"

#include "common/Event.h"

#include <QBuffer>
#include <QImage>
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

    if (!parseStitchString(result, outPath)) {
        spdlog::warn("[CameraCapture] ydstitch_save_img returned empty/invalid path.");
        return false;
    }
    spdlog::info("[CameraCapture] ydstitch_save_img -> {}", outPath);
    return true;
}

// ── CameraCapture ──────────────────────────────────────────────────

CameraCapture::CameraCapture() {
    mCfg            = Config::getInstance().read(mClassName);
    mCaptureEnabled = mCfg.value("enabled", false);

    connect(&Event::getInstance(), &Event::beforeUiInitialization, [this](QQuickView& view, QQmlContext* context) {
        context->setContextProperty("cameraCapture", this);
    });
}

bool CameraCapture::isCaptureEnabled() const { return mCaptureEnabled; }

void CameraCapture::setCaptureEnabled(bool val) {
    if (mCaptureEnabled != val) {
        mCaptureEnabled = val;
        UPDATE_CFG("enabled", val);
        emit captureEnabledChanged();
    }
}

void CameraCapture::onScanComplete() {
    if (!mCaptureEnabled) {
        return;
    }

    std::string origPath;
    if (!callYdstitchSaveImg(origPath)) {
        return;
    }

    QImage img(QString::fromStdString(origPath));
    if (img.isNull()) {
        spdlog::warn("[CameraCapture] onScanComplete: failed to load image");
        return;
    }
    if (qint64(img.width()) * img.height() > MaxImagePixels) {
        spdlog::warn("[CameraCapture] onScanComplete: image resolution is too high");
        return;
    }

    QByteArray ba;
    QBuffer    buffer(&ba);
    buffer.open(QIODevice::WriteOnly);
    if (!img.save(&buffer, "JPEG", 85) || ba.size() > (MaxBase64Chars / 4) * 3) {
        spdlog::warn("[CameraCapture] onScanComplete: encoded image is too large");
        return;
    }

    emit imageCaptured(ba.toBase64());
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
