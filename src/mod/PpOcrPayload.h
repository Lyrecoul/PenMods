// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QDir>
#include <QFileInfo>
#include <QString>

namespace mod::ppocr {

inline constexpr auto kParamModel = "PP_OCRv5_mobile_rec.ncnn.param";
inline constexpr auto kBinModel   = "PP_OCRv5_mobile_rec.ncnn.bin";

inline bool isReadableNonEmptyFile(const QString& path) {
    const QFileInfo file(path);
    return file.isFile() && file.isReadable() && file.size() > 0;
}

inline bool hasCompleteModels(const QString& models) {
    return isReadableNonEmptyFile(QDir(models).filePath(QString::fromLatin1(kParamModel)))
        && isReadableNonEmptyFile(QDir(models).filePath(QString::fromLatin1(kBinModel)));
}

inline bool hasCompletePayload(const QString& shim, const QString& models) {
    return isReadableNonEmptyFile(shim) && hasCompleteModels(models);
}

inline bool managedPayloadReady(const QString& shim, const QString& runtimeModels) {
    return !shim.isEmpty() && hasCompletePayload(shim, QFileInfo(shim).dir().filePath("models"))
        && hasCompleteModels(runtimeModels);
}

inline QString resolveShim(const QString& updateRoot, const QString& legacyShim, const QString& legacyModels) {
    const QString managed = QDir(updateRoot).filePath("active/ppocr");
    const QString managedShim = QDir(managed).filePath("libyocr.so");
    if (managedPayloadReady(managedShim, legacyModels)) return managedShim;
    if (hasCompletePayload(legacyShim, legacyModels)) return legacyShim;
    return {};
}

} // namespace mod::ppocr
