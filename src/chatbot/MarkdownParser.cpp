// SPDX-License-Identifier: GPL-3.0-only

#include "chatbot/MarkdownParser.h"

#include <QJsonObject>
#include <QRegularExpression>
#include <QStringList>

namespace mod::chatbot {
namespace {

QJsonObject textSegment(const QString& text) {
    return QJsonObject{
        {"type",    "text"},
        {"content", text  }
    };
}

QJsonArray inlineSegments(const QString& text) {
    QJsonArray segments;
    int        start = 0;
    int        pos   = 0;
    while (pos < text.size()) {
        int open   = text.indexOf("\\(", pos);
        int dollar = text.indexOf('$', pos);
        if (open < 0 || (dollar >= 0 && dollar < open)) open = dollar;
        if (open < 0) break;

        const bool paren     = text.mid(open, 2) == "\\(";
        const int  bodyStart = open + (paren ? 2 : 1);
        const int  close     = paren ? text.indexOf("\\)", bodyStart) : text.indexOf('$', bodyStart);
        if (close < 0) {
            pos = bodyStart;
            continue;
        }
        if (open > start) segments.append(textSegment(text.mid(start, open - start)));
        segments.append(
            QJsonObject{
                {"type", "math"},
                {"content", text.mid(bodyStart, close - bodyStart).trimmed()}
        }
        );
        start = close + (paren ? 2 : 1);
        pos   = start;
    }
    if (start < text.size()) segments.append(textSegment(text.mid(start)));
    if (segments.isEmpty()) segments.append(textSegment(text));
    return segments;
}

QStringList tableCells(QString line) {
    line = line.trimmed();
    if (line.startsWith('|')) line.remove(0, 1);
    if (line.endsWith('|')) line.chop(1);
    QStringList cells = line.split('|');
    for (QString& cell : cells) cell = cell.trimmed();
    return cells;
}

bool isTableSeparator(const QString& line) {
    const QStringList cells = tableCells(line);
    if (cells.isEmpty()) return false;
    static const QRegularExpression separator(R"(^:?-+:?$)");
    for (const QString& cell : cells) {
        if (!separator.match(cell).hasMatch()) return false;
    }
    return true;
}

bool startsBlock(const QStringList& lines, int index) {
    const QString                   line    = lines[index];
    const QString                   trimmed = line.trimmed();
    static const QRegularExpression heading(R"(^#{1,6}\s+)");
    static const QRegularExpression unordered(R"(^[-*+]\s+)");
    static const QRegularExpression ordered(R"(^\d+\.\s+)");
    static const QRegularExpression rule(R"(^([-*_])(?:\s*\1){2,}\s*$)");
    return trimmed.startsWith("```") || trimmed.startsWith("$$") || trimmed.startsWith("\\[")
        || heading.match(trimmed).hasMatch() || unordered.match(trimmed).hasMatch() || ordered.match(trimmed).hasMatch()
        || trimmed.startsWith('>') || rule.match(trimmed).hasMatch()
        || (index + 1 < lines.size() && trimmed.startsWith('|') && isTableSeparator(lines[index + 1]));
}

} // namespace

QJsonArray MarkdownParser::parse(const QString& text) {
    QJsonArray                      blocks;
    const QStringList               lines = text.split('\n', Qt::KeepEmptyParts);
    static const QRegularExpression heading(R"(^(#{1,6})\s+(.+)$)");
    static const QRegularExpression unordered(R"(^[-*+]\s+(.+)$)");
    static const QRegularExpression ordered(R"(^\d+\.\s+(.+)$)");
    static const QRegularExpression rule(R"(^([-*_])(?:\s*\1){2,}\s*$)");

    for (int i = 0; i < lines.size();) {
        const QString trimmed = lines[i].trimmed();
        if (trimmed.isEmpty()) {
            ++i;
            continue;
        }

        if (trimmed.startsWith("```")) {
            const QString language = trimmed.mid(3).trimmed();
            QStringList   content;
            for (++i; i < lines.size() && !lines[i].trimmed().startsWith("```"); ++i) content.append(lines[i]);
            if (i < lines.size()) ++i;
            blocks.append(
                QJsonObject{
                    {"type",     "code_block"      },
                    {"content",  content.join('\n')},
                    {"language", language          }
            }
            );
            continue;
        }

        if (trimmed.startsWith("$$") || trimmed.startsWith("\\[")) {
            const bool    brackets = trimmed.startsWith("\\[");
            const QString open     = brackets ? "\\[" : "$$";
            const QString close    = brackets ? "\\]" : "$$";
            QString       body     = trimmed.mid(open.size());
            if (body.endsWith(close)) {
                body.chop(close.size());
                ++i;
            } else {
                QStringList parts{body};
                for (++i; i < lines.size() && !lines[i].trimmed().endsWith(close); ++i) parts.append(lines[i]);
                if (i < lines.size()) {
                    QString last = lines[i].trimmed();
                    last.chop(close.size());
                    parts.append(last);
                    ++i;
                }
                body = parts.join('\n');
            }
            blocks.append(
                QJsonObject{
                    {"type",    "math_block"  },
                    {"content", body.trimmed()}
            }
            );
            continue;
        }

        const auto headingMatch = heading.match(trimmed);
        if (headingMatch.hasMatch()) {
            blocks.append(
                QJsonObject{
                    {"type",     "heading"                               },
                    {"level",    headingMatch.captured(1).size()         },
                    {"segments", inlineSegments(headingMatch.captured(2))}
            }
            );
            ++i;
            continue;
        }
        if (rule.match(trimmed).hasMatch()) {
            blocks.append(
                QJsonObject{
                    {"type", "hr"}
            }
            );
            ++i;
            continue;
        }

        if (trimmed.startsWith('>')) {
            QStringList quote;
            while (i < lines.size() && lines[i].trimmed().startsWith('>')) {
                quote.append(lines[i].trimmed().mid(1).trimmed());
                ++i;
            }
            blocks.append(
                QJsonObject{
                    {"type",     "blockquote"                    },
                    {"segments", inlineSegments(quote.join('\n'))}
            }
            );
            continue;
        }

        const bool isUnordered = unordered.match(trimmed).hasMatch();
        const bool isOrdered   = ordered.match(trimmed).hasMatch();
        if (isUnordered || isOrdered) {
            QJsonArray items;
            while (i < lines.size()) {
                const auto match = (isOrdered ? ordered : unordered).match(lines[i].trimmed());
                if (!match.hasMatch()) break;
                items.append(inlineSegments(match.captured(1)));
                ++i;
            }
            blocks.append(
                QJsonObject{
                    {"type",    "list_block"},
                    {"ordered", isOrdered   },
                    {"items",   items       }
            }
            );
            continue;
        }

        if (i + 1 < lines.size() && trimmed.startsWith('|') && isTableSeparator(lines[i + 1])) {
            const QStringList headerCells = tableCells(lines[i]);
            QJsonArray        headers;
            for (const QString& cell : headerCells) headers.append(cell);
            QJsonArray rows;
            i += 2;
            while (i < lines.size() && lines[i].trimmed().startsWith('|')) {
                QStringList cells = tableCells(lines[i]);
                while (cells.size() < headerCells.size()) cells.append(QString());
                QJsonArray row;
                for (int column = 0; column < headerCells.size(); ++column) row.append(cells.value(column));
                rows.append(row);
                ++i;
            }
            blocks.append(
                QJsonObject{
                    {"type",    "table"},
                    {"headers", headers},
                    {"rows",    rows   }
            }
            );
            continue;
        }

        QStringList paragraph;
        while (i < lines.size() && !lines[i].trimmed().isEmpty() && (paragraph.isEmpty() || !startsBlock(lines, i))) {
            paragraph.append(lines[i]);
            ++i;
        }
        if (paragraph.isEmpty()) {
            paragraph.append(lines[i]);
            ++i;
        }
        blocks.append(
            QJsonObject{
                {"type",     "paragraph"                         },
                {"segments", inlineSegments(paragraph.join('\n'))}
        }
        );
    }
    return blocks;
}

} // namespace mod::chatbot
