// SPDX-License-Identifier: GPL-3.0-only
#include "mod/PpOcrPayload.h"

#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>
#include <cstdio>
#include <cstdlib>

static void check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL %s\n", message);
        std::exit(1);
    }
}

static void writeFile(const QString& path, const QByteArray& bytes = "payload") {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    check(file.open(QIODevice::WriteOnly), "create fixture file");
    check(file.write(bytes) == bytes.size(), "write fixture file");
}

static void installPayload(const QString& shim, const QString& models) {
    writeFile(shim);
    writeFile(QDir(models).filePath(QString::fromLatin1(mod::ppocr::kParamModel)));
    writeFile(QDir(models).filePath(QString::fromLatin1(mod::ppocr::kBinModel)));
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QTemporaryDir temporary;
    check(temporary.isValid(), "temporary root");

    const QString root = temporary.path();
    const QString updateRoot = QDir(root).filePath("update-system");
    const QString managedDir = QDir(updateRoot).filePath("active/ppocr");
    const QString managedShim = QDir(managedDir).filePath("libyocr.so");
    const QString managedModels = QDir(managedDir).filePath("models");
    const QString legacyShim = QDir(root).filePath("legacy/ppocr_backend/libyocr.so");
    const QString legacyModels = QDir(root).filePath("legacy/ppocr_models");

    check(mod::ppocr::resolveShim(updateRoot, legacyShim, legacyModels).isEmpty(), "missing payload rejected");

    const QString pendingDir = QDir(updateRoot).filePath("pending/ppocr");
    installPayload(QDir(pendingDir).filePath("libyocr.so"), QDir(pendingDir).filePath("models"));
    check(mod::ppocr::resolveShim(updateRoot, legacyShim, legacyModels).isEmpty(), "pending payload is not installed");

    installPayload(managedShim, managedModels);
    check(mod::ppocr::hasCompletePayload(managedShim, managedModels), "managed payload accepted");
    check(mod::ppocr::resolveShim(updateRoot, legacyShim, legacyModels).isEmpty(), "missing runtime model alias rejected");
    QDir().mkpath(QFileInfo(legacyModels).absolutePath());
    check(QFile::link(managedModels, legacyModels), "create runtime model alias");
    check(mod::ppocr::managedPayloadReady(managedShim, legacyModels), "managed runtime paths ready");
    check(mod::ppocr::resolveShim(updateRoot, legacyShim, legacyModels) == managedShim, "managed works without legacy shim alias");

    check(QFile::remove(QDir(managedModels).filePath(QString::fromLatin1(mod::ppocr::kBinModel))), "remove managed bin model");
    check(!mod::ppocr::hasCompletePayload(managedShim, managedModels), "missing managed model rejected");
    check(!mod::ppocr::managedPayloadReady(managedShim, legacyModels), "missing model disables installed state");
    check(QFile::remove(legacyModels), "remove runtime model alias");
    installPayload(legacyShim, legacyModels);
    check(mod::ppocr::resolveShim(updateRoot, legacyShim, legacyModels) == legacyShim, "complete legacy fallback selected");

    check(QFile::remove(QDir(legacyModels).filePath(QString::fromLatin1(mod::ppocr::kParamModel))), "remove legacy param model");
    check(mod::ppocr::resolveShim(updateRoot, legacyShim, legacyModels).isEmpty(), "incomplete managed and legacy rejected");
    writeFile(QDir(legacyModels).filePath(QString::fromLatin1(mod::ppocr::kParamModel)), QByteArray());
    check(mod::ppocr::resolveShim(updateRoot, legacyShim, legacyModels).isEmpty(), "empty model rejected");

    check(QFile::remove(legacyShim), "remove legacy shim");
    check(QFile::link(QDir(root).filePath("absent.so"), legacyShim), "create dangling shim alias");
    check(!mod::ppocr::isReadableNonEmptyFile(legacyShim), "dangling shim rejected");
    check(!mod::ppocr::isReadableNonEmptyFile(legacyModels), "directory rejected");
    std::puts("PASS managed/legacy selection, runtime alias deletion, missing model pair, pending exclusion, empty and dangling files");
}
