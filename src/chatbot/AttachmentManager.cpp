// SPDX-License-Identifier: GPL-3.0-only

#include "chatbot/AttachmentManager.h"

#include "chatbot/Backend.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

namespace mod::chatbot {

namespace {

constexpr qsizetype MAX_IMAGE_BYTES = 9 * 1024 * 1024;

QString extensionForMimeType(const QString& mimeType) {
    if (mimeType == "image/jpeg") return "jpg";
    if (mimeType == "image/png") return "png";
    if (mimeType == "image/webp") return "webp";
    if (mimeType == "image/gif") return "gif";
    return {};
}

} // namespace

QString AttachmentManager::safeSessionId(const QString& sessionId) {
    QString safe = sessionId;
    safe.remove(QRegularExpression("[^A-Za-z0-9_-]"));
    return safe;
}

bool AttachmentManager::persistImage(
    const QString& rootPath,
    const QString& sessionId,
    MessagePart&   part,
    QString*       error
) {
    if (part.type != "image_url" || !part.url.startsWith("data:", Qt::CaseInsensitive)) return true;

    const int separator = part.url.indexOf(',');
    if (separator <= 5) {
        if (error) *error = "图片数据格式无效";
        return false;
    }

    const QString header    = part.url.mid(5, separator - 5);
    const QString mimeType  = header.section(';', 0, 0).toLower();
    const QString extension = extensionForMimeType(mimeType);
    if (extension.isEmpty() || !header.contains(";base64", Qt::CaseInsensitive)) {
        if (error) *error = "不支持的图片格式";
        return false;
    }

    const QByteArray bytes = QByteArray::fromBase64(part.url.mid(separator + 1).toLatin1());
    if (bytes.isEmpty() || bytes.size() > MAX_IMAGE_BYTES) {
        if (error) *error = bytes.isEmpty() ? "图片数据为空或损坏" : "图片数据过大";
        return false;
    }

    const QString directoryPath = QDir(rootPath).filePath("attachments/" + safeSessionId(sessionId));
    QDir          directory;
    if (!directory.mkpath(directoryPath)) {
        if (error) *error = "无法创建附件目录";
        return false;
    }

    const QString digest   = QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
    const QString fileName = digest + "." + extension;
    const QString filePath = QDir(directoryPath).filePath(fileName);
    if (!QFileInfo::exists(filePath)) {
        QSaveFile file(filePath);
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
            if (error) *error = "无法保存图片附件";
            return false;
        }
    }

    part.localPath = filePath;
    part.name      = part.name.isEmpty() ? "图片." + extension : part.name;
    part.mimeType  = mimeType;
    part.size      = bytes.size();
    return true;
}

QString AttachmentManager::requestUrl(const MessagePart& part) {
    if (!part.url.isEmpty()) return part.url;
    if (part.localPath.isEmpty() || part.mimeType.isEmpty()) return {};

    QFile file(part.localPath);
    if (!file.open(QIODevice::ReadOnly) || file.size() > MAX_IMAGE_BYTES) return {};
    const QByteArray bytes = file.readAll();
    if (bytes.isEmpty()) return {};
    return "data:" + part.mimeType + ";base64," + QString::fromLatin1(bytes.toBase64());
}

void AttachmentManager::removeSession(const QString& rootPath, const QString& sessionId) {
    QDir(QDir(rootPath).filePath("attachments/" + safeSessionId(sessionId))).removeRecursively();
}

} // namespace mod::chatbot
