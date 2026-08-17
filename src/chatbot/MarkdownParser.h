// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QJsonArray>
#include <QString>

namespace mod::chatbot {

class MarkdownParser {
public:
    static QJsonArray parse(const QString& text);
};

} // namespace mod::chatbot
