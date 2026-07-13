// SPDX-License-Identifier: GPL-3.0-only
/*
 * Copyright (C) 2022-present, PenUniverse.
 * This file is part of the PenMods open source project.
 */

#include "capture/CameraCapture.h"

#include "common/Event.h"

#include <QBuffer>
#include <QImage>
#include <QPainter>
#include <QQmlContext>
#include <QQuickView>

#include <cstring>
#include <dlfcn.h>
#include <spdlog/spdlog.h>

namespace mod::capture {

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
    std::memcpy(&data,   s.data,      8);
    std::memcpy(&length, s.data + 8,   8);

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
            if (!once) { once = true; spdlog::warn("[CameraCapture] ydstitch_save_img not found."); }
            return false;
        }
    }

    using Fn = StitchString (*)();
    auto fn = reinterpret_cast<Fn>(sym);

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

    QByteArray ba;
    QBuffer    buffer(&ba);
    buffer.open(QIODevice::WriteOnly);
    img.save(&buffer, "JPEG", 85);
    buffer.close();

    emit imageCaptured(ba.toBase64());
}

QString CameraCapture::cropImage(const QString& base64Data, int x, int y, int w, int h) {
    QByteArray decoded = QByteArray::fromBase64(base64Data.toLatin1());
    QImage     img;
    img.loadFromData(decoded);
    if (img.isNull()) {
        spdlog::warn("[CameraCapture] cropImage: failed to decode base64");
        return {};
    }

    x = std::max(0, std::min(img.width() - 1, x));
    y = std::max(0, std::min(img.height() - 1, y));
    w = std::max(1, std::min(img.width() - x, w));
    h = std::max(1, std::min(img.height() - y, h));

    QImage cropped = img.copy(x, y, w, h);

    QByteArray ba;
    QBuffer    buffer(&ba);
    buffer.open(QIODevice::WriteOnly);
    cropped.save(&buffer, "JPEG", 90);
    buffer.close();

    return QString::fromLatin1(ba.toBase64());
}

QString CameraCapture::stitchImages(const QStringList& imageList, const QString& direction) {
    if (imageList.size() < 2) {
        spdlog::warn("[CameraCapture] stitchImages: need at least 2 images, got {}", imageList.size());
        return {};
    }

    QVector<QImage> images;
    for (const auto& b64 : imageList) {
        if (b64.isEmpty()) continue;
        QByteArray decoded = QByteArray::fromBase64(b64.toLatin1());
        QImage img;
        img.loadFromData(decoded);
        if (!img.isNull()) images.append(img);
    }

    if (images.size() < 2) {
        spdlog::warn("[CameraCapture] stitchImages: need at least 2 images, got {}", images.size());
        return {};
    }

    bool horizontal = (direction == "horizontal");
    int totalW = 0, totalH = 0;
    int maxW = 0, maxH = 0;
    for (const auto& img : images) {
        if (horizontal) {
            totalW += img.width();
            maxH = std::max(maxH, img.height());
        } else {
            totalH += img.height();
            maxW = std::max(maxW, img.width());
        }
    }
    if (horizontal) totalH = maxH;
    else totalW = maxW;

    QImage composite(totalW, totalH, QImage::Format_RGB32);
    composite.fill(Qt::white);
    QPainter painter(&composite);

    int offset = 0;
    for (const auto& img : images) {
        if (horizontal) {
            painter.drawImage(offset, 0, img);
            offset += img.width();
        } else {
            painter.drawImage(0, offset, img);
            offset += img.height();
        }
    }
    painter.end();

    QByteArray ba;
    QBuffer buffer(&ba);
    buffer.open(QIODevice::WriteOnly);
    composite.save(&buffer, "JPEG", 90);
    buffer.close();

    return QString::fromLatin1(ba.toBase64());
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
