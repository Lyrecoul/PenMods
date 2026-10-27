// SPDX-License-Identifier: GPL-3.0-only

#include "chatbot/AttachmentManager.h"

#include "chatbot/Backend.h"

#include <QCache>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMimeDatabase>
#include <QMutex>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QRunnable>
#include <QSaveFile>
#include <QThreadPool>

#include <utility>

namespace mod::chatbot {

namespace {

// data URL 内存缓存：key = localPath|size|mtime，value = 完整 data URL。
// 目的：历史/重新生成时 messageToJson() 每条消息都要 requestUrl()，无缓存会在 UI 线程反复读盘 + base64。
// 预算按字符数计（QString 内部 UTF-16，实际内存约 2 倍），16 Mi 字符 ≈ 32 MiB 上限。
constexpr int            kDataUrlCacheBudgetChars = 16 * 1024 * 1024;
QMutex                   g_dataUrlCacheMutex;
QCache<QString, QString> g_dataUrlCache(kDataUrlCacheBudgetChars);

QString cacheKey(const QFileInfo& info) {
    return info.absoluteFilePath() + "|" + QString::number(info.size()) + "|"
         + QString::number(info.lastModified().toMSecsSinceEpoch());
}

QString buildDataUrl(const QString& localPath, const QString& mimeType) {
    QFile file(localPath);
    if (!file.open(QIODevice::ReadOnly)) return {};
    if (file.size() <= 0 || file.size() > AttachmentManager::kMaxImageBytes) return {};
    const QByteArray bytes = file.read(AttachmentManager::kMaxImageBytes + 1);
    if (bytes.isEmpty() || bytes.size() > AttachmentManager::kMaxImageBytes) return {};
    QString mime = mimeType;
    if (mime.isEmpty()) mime = QMimeDatabase().mimeTypeForFile(localPath, QMimeDatabase::MatchExtension).name();
    if (mime.isEmpty() || !mime.startsWith("image/")) mime = "image/jpeg";
    return "data:" + mime + ";base64," + QString::fromLatin1(bytes.toBase64());
}

void cacheInsert(const QString& localPath, const QString& dataUrl) {
    if (dataUrl.isEmpty() || dataUrl.size() > AttachmentManager::kMaxMediaJsonBytes) return;
    const QFileInfo info(localPath);
    if (!info.exists()) return;
    const QString key = cacheKey(info);
    QMutexLocker  lock(&g_dataUrlCacheMutex);
    // 超出预算的旧条目由 QCache LRU 淘汰。
    g_dataUrlCache.insert(key, new QString(dataUrl), static_cast<int>(dataUrl.size()));
}

QString cacheLookup(const QString& localPath) {
    if (localPath.isEmpty()) return {};
    const QFileInfo info(localPath);
    if (!info.exists()) return {};
    const QString key = cacheKey(info);
    QMutexLocker  lock(&g_dataUrlCacheMutex);
    if (const QString* cached = g_dataUrlCache.object(key)) return *cached;
    return {};
}

QString extensionForMimeType(const QString& mimeType) {
    if (mimeType == "image/jpeg") return "jpg";
    if (mimeType == "image/png") return "png";
    if (mimeType == "image/webp") return "webp";
    if (mimeType == "image/gif") return "gif";
    return {};
}

// 后台预热：读盘 + base64 后放进缓存；失败静默（真正请求时还会走同步兜底）。
class WarmCacheTask : public QRunnable {
public:
    explicit WarmCacheTask(QVector<MessagePart> parts) : m_parts(std::move(parts)) {}

    void run() override {
        for (const auto& part : m_parts) {
            if (part.localPath.isEmpty() || !part.url.isEmpty()) continue;
            if (!cacheLookup(part.localPath).isEmpty()) continue;
            const QString dataUrl = buildDataUrl(part.localPath, part.mimeType);
            cacheInsert(part.localPath, dataUrl);
        }
    }

private:
    QVector<MessagePart> m_parts;
};

bool looksLikeDigestFileName(const QString& baseName) {
    if (baseName.size() != 64) return false;
    for (const QChar ch : baseName) {
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) return false;
    }
    return true;
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
    if (bytes.isEmpty() || bytes.size() > kMaxImageBytes) {
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
    // 落盘后把 data URL 放进内存缓存，后续请求/重新生成不必再读盘。
    cacheInsert(filePath, "data:" + mimeType + ";base64," + QString::fromLatin1(bytes.toBase64()));
    return true;
}

QString AttachmentManager::requestUrl(const MessagePart& part) {
    if (!part.url.isEmpty()) return part.url;
    if (part.localPath.isEmpty()) return {};

    // 先查缓存（命中即返回，不读盘）。
    const QString cached = cacheLookup(part.localPath);
    if (!cached.isEmpty()) return cached;

    // 缓存未命中：受限读盘（单张 ≤ 9 MiB），结果再入缓存。后台预热失败时走这里兜底。
    const QString dataUrl = buildDataUrl(part.localPath, part.mimeType);
    if (dataUrl.isEmpty()) return {};
    cacheInsert(part.localPath, dataUrl);
    return dataUrl;
}

void AttachmentManager::removeSession(const QString& rootPath, const QString& sessionId) {
    QDir(QDir(rootPath).filePath("attachments/" + safeSessionId(sessionId))).removeRecursively();
}

void AttachmentManager::pruneSession(
    const QString&       rootPath,
    const QString&       sessionId,
    const QSet<QString>& referencedPaths
) {
    const QString directoryPath = QDir(rootPath).filePath("attachments/" + safeSessionId(sessionId));
    QDir          directory(directoryPath);
    if (!directory.exists()) return;

    const QFileInfoList files = directory.entryInfoList(QDir::Files | QDir::NoDotAndDotDot);
    for (const auto& file : files) {
        if (referencedPaths.contains(file.absoluteFilePath())) continue;
        // 只删本管理器生成的附件（sha256.扩展名），不碰用户/其它模块放进来的文件。
        if (!looksLikeDigestFileName(file.completeBaseName())) continue;
        QFile::remove(file.absoluteFilePath());
    }

    // 目录空了顺手删掉（removeSession 之外再清一次空目录）。
    if (directory.entryList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot).isEmpty()) QDir().rmdir(directoryPath);
}

void AttachmentManager::warmCache(const QVector<MessagePart>& imageParts) {
    QVector<MessagePart> pending;
    for (const auto& part : imageParts) {
        if (part.type != "image_url" || part.localPath.isEmpty() || !part.url.isEmpty()) continue;
        if (!cacheLookup(part.localPath).isEmpty()) continue;
        pending.append(part);
    }
    if (pending.isEmpty()) return;
    QThreadPool::globalInstance()->start(new WarmCacheTask(std::move(pending)));
}

} // namespace mod::chatbot
