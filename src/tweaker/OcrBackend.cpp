// SPDX-License-Identifier: GPL-3.0-only
/*
 * Copyright (C) 2022-present, PenUniverse.
 * This file is part of the PenMods open source project.
 */

#include "tweaker/OcrBackend.h"

#include "common/Event.h"
#include "common/Utils.h"

#include <QDir>
#include <QFile>
#include <QQmlContext>

namespace mod {

namespace {

// 安装脚本(见 tools/ppocr-backend/installer)约定的路径
constexpr auto STAGE_SHIM  = "/userdisk/ppocr_backend/libyocr.so";
constexpr auto SHADOW_DIR  = "/userdisk/Qtlib";
constexpr auto SHADOW_SHIM = "/userdisk/Qtlib/libyocr.so";
constexpr auto HEIGHT_FILE = "/userdisk/ppocr_target_h";

constexpr int HEIGHT_ACCURATE = 48;
constexpr int HEIGHT_FAST     = 32;

int readHeight() {
    QFile file(HEIGHT_FILE);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return HEIGHT_FAST;
    }
    bool ok  = false;
    int  val = QString::fromUtf8(file.readAll()).trimmed().toInt(&ok);
    return (ok && val >= 16 && val <= 64) ? val : HEIGHT_FAST;
}

void writeHeight(int value) {
    QFile file(HEIGHT_FILE);
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        file.write(QString::number(value).toUtf8());
        file.close();
    }
}

} // namespace

OcrBackend::OcrBackend() : Logger("OcrBackend") {
    connect(&Event::getInstance(), &Event::beforeUiInitialization, [this](QQuickView& view, QQmlContext* context) {
        context->setContextProperty("ocrBackend", this);
    });
}

bool OcrBackend::installed() const { return QFile::exists(STAGE_SHIM); }

bool OcrBackend::enabled() const { return QFile::exists(SHADOW_SHIM); }

bool OcrBackend::accuracyFirst() const { return readHeight() >= HEIGHT_ACCURATE; }

void OcrBackend::setEnabled(bool value) {
    if (value == enabled()) {
        return;
    }
    if (value && !installed()) {
        warn("PP-OCRv5 backend is not installed; run penmods-ppocr.sh on the device first.");
        return;
    }

    if (value) {
        QDir().mkpath(SHADOW_DIR);
        // 先写临时文件再 rename:正在运行(app 已把旧文件映射进来时)覆盖会被撕裂
        const QString temp = QString(SHADOW_SHIM) + ".new";
        QFile::remove(temp);
        if (!QFile::copy(STAGE_SHIM, temp)) {
            error("Cannot stage the OCR shim to {}", temp.toStdString());
            return;
        }
        QFile::remove(SHADOW_SHIM);
        if (!QFile::rename(temp, SHADOW_SHIM)) {
            error("Cannot move the OCR shim into {}", SHADOW_SHIM);
            return;
        }
        info("PP-OCRv5 OCR backend enabled; restarting to load it.");
    } else {
        QFile::remove(SHADOW_SHIM);
        info("PP-OCRv5 OCR backend disabled; restarting to use the vendor engine.");
    }

    emit stateChanged();
    restartApplication();
}

void OcrBackend::setAccuracyFirst(bool value) {
    const int want = value ? HEIGHT_ACCURATE : HEIGHT_FAST;
    if (readHeight() == want) {
        return;
    }
    writeHeight(want);
    info("PP-OCRv5 target height set to {} (takes effect on the next recognized line).", want);
    emit stateChanged();
}

void OcrBackend::restartApplication() {
    // 放开手再重启:先让 QML 把开关状态画出来,再由守护进程把 app 拉起来。
    // 用子 shell 让 killall 脱离 popen 的生命周期,否则我们自己会先被 exec() 阻塞住。
    exec("sync; (sleep 1; killall YoudaoDictPen) >/dev/null 2>&1 &");
}

} // namespace mod
