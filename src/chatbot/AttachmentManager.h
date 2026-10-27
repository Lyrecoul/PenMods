// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QSet>
#include <QString>
#include <QVector>

namespace mod::chatbot {

struct MessagePart;

// 附件（图片）持久化与读取。
// 约定：
//   - 每会话独立目录 <root>/attachments/<safeSessionId>/（safeSessionId 只保留 [A-Za-z0-9_-]）；
//   - 文件名 = 图片内容 SHA-256 + 扩展名（天然按内容去重、原子写入，不会被半截文件覆盖）；
//   - 单张图片上限 9 MiB；一次请求的 mediaParts JSON 上限 12 MiB（见 kMaxMediaJsonBytes）。
class AttachmentManager {
public:
    static constexpr qint64 kMaxImageBytes     = 9LL * 1024 * 1024;
    static constexpr qint64 kMaxMediaJsonBytes = 12LL * 1024 * 1024;

    // data URL 图片落盘（HTTP URL / 已有 localPath 的 part 原样返回 true）。
    static bool    persistImage(const QString& rootPath, const QString& sessionId, MessagePart& part, QString* error);
    // 请求构造时把图片 part 转成可发给 API 的 URL：HTTP/data URL 原样返回；本地文件读成 data URL
    // （有界内存缓存，避免每条历史消息都在 UI 线程重复读大文件 + base64）。
    static QString requestUrl(const MessagePart& part);
    // 整个会话目录删除（删除会话）。
    static void    removeSession(const QString& rootPath, const QString& sessionId);
    // 删除会话目录里不再被任何消息引用的附件文件（删除消息/截断历史/清空历史后调用）。
    // referencedPaths 为当前会话所有消息 parts 里的 localPath 集合（绝对路径）。
    static void pruneSession(const QString& rootPath, const QString& sessionId, const QSet<QString>& referencedPaths);
    // 后台预热历史图片的 data URL 缓存（不阻塞 UI 线程）。
    static void warmCache(const QVector<MessagePart>& imageParts);

private:
    static QString safeSessionId(const QString& sessionId);
};

} // namespace mod::chatbot
