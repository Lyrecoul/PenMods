// SPDX-License-Identifier: GPL-3.0-only
/*
 * Copyright (C) 2022-present, PenUniverse.
 * This file is part of the PenMods open source project.
 */

#include "tweaker/OcrBackend.h"

#include "common/Event.h"
#include "common/Utils.h"
#include "common/util/System.h"
#include "mod/Mod.h"
#include "mod/PpOcrPayload.h"
#include <cstdio>

#include <QDir>
#include <QFile>
#include <QQmlContext>

namespace mod {

namespace {

// 安装脚本(见 tools/ppocr-backend/installer)约定的路径
constexpr auto STAGE_SHIM  = "/userdisk/ppocr_backend/libyocr.so";
constexpr auto MODEL_DIR   = "/userdisk/ppocr_models";
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

bool OcrBackend::installed() const {
    return !ppocr::resolveShim(util::getUpdateDirectory(), STAGE_SHIM, MODEL_DIR).isEmpty();
}

bool OcrBackend::enabled() const { return ppocr::hasCompletePayload(SHADOW_SHIM, MODEL_DIR); }

bool OcrBackend::accuracyFirst() const { return readHeight() >= HEIGHT_ACCURATE; }

void OcrBackend::setEnabled(bool value) {
    const QFileInfo shadow(SHADOW_SHIM);
    const bool shadowEntryExists = shadow.exists() || shadow.isSymLink();
    if (value == enabled() && (value || !shadowEntryExists)) return;

    if (value) {
        const auto source = ppocr::resolveShim(util::getUpdateDirectory(), STAGE_SHIM, MODEL_DIR);
        if (source.isEmpty()) {
            warn("PP-OCRv5 backend or its required model pair is missing; install the component first.");
            return;
        }
        QDir().mkpath(SHADOW_DIR);
        // Publish through a temporary path so a running process never sees a partial shim.
        const QString temp = QString(SHADOW_SHIM) + ".new";
        QFile::remove(temp);
        const auto managed = QDir(util::getUpdateDirectory()).filePath("active/ppocr/libyocr.so");
        const bool staged = source == managed ? QFile::link(source, temp) : QFile::copy(source, temp);
        if (!staged) {
            error("Cannot stage the OCR shim to {}", temp.toStdString());
            return;
        }
        if (::rename(QFile::encodeName(temp).constData(), SHADOW_SHIM) != 0) {
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

void OcrBackend::restartApplication() { Mod::getInstance().restartApplication(); }

} // namespace mod
