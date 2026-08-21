// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QString>

namespace mod::chatbot {

struct MessagePart;

class AttachmentManager {
public:
    static bool    persistImage(const QString& rootPath, const QString& sessionId, MessagePart& part, QString* error);
    static QString requestUrl(const MessagePart& part);
    static void    removeSession(const QString& rootPath, const QString& sessionId);

private:
    static QString safeSessionId(const QString& sessionId);
};

} // namespace mod::chatbot
