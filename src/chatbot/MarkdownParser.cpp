// SPDX-License-Identifier: GPL-3.0-only

#include "chatbot/MarkdownParser.h"

#include <QJsonObject>
#include <QRegularExpression>
#include <QStringList>
#include <QVector>

namespace mod::chatbot {
namespace {

// -----------------------------------------------------------------------
// 行内片段
//
// 输出三类片段：
//   {"type":"text","content":…,"bold":true,"italic":true,"code":true,"strike":true}
//   {"type":"math","content":…,"display":true}
// 强调标记在解析阶段就被消化掉并转成标志位，因此粗体、斜体可以跨越公式片段
// （`**能量 $E$**` 这种写法），公式也不会因为落在标记中间而丢失。
// -----------------------------------------------------------------------

QJsonObject textSegment(const QString& text, bool bold, bool italic, bool code, bool strike) {
    QJsonObject segment{
        {"type",    "text"},
        {"content", text  }
    };
    if (bold) segment.insert("bold", true);
    if (italic) segment.insert("italic", true);
    if (code) segment.insert("code", true);
    if (strike) segment.insert("strike", true);
    return segment;
}

bool isWordChar(const QChar& c) { return c.isLetterOrNumber(); }

// 强调标记的配对：`_` 不能在单词内部配对（snake_case 保持原样），单字符标记两侧
// 不能是空白。找不到配对就当作普通字符，避免把正文里的 `*` 吃掉。
int matchEmphasis(const QString& text, int from, const QChar marker, int wanted) {
    for (int i = from; i < text.size();) {
        if (text.at(i) != marker) {
            ++i;
            continue;
        }
        if (i > 0 && text.at(i - 1) == '\\') {
            ++i;
            continue;
        }
        int run = 0;
        while (i + run < text.size() && text.at(i + run) == marker) ++run;
        if (run < wanted) {
            i += run;
            continue;
        }
        if (i == from || text.at(i - 1).isSpace()) {
            i += run;
            continue;
        }
        if (marker == '_' && i + run < text.size() && isWordChar(text.at(i + run))) {
            i += run;
            continue;
        }
        return i;
    }
    return -1;
}

QJsonArray inlineSegments(const QString& text) {
    QJsonArray segments;
    QString    plain;
    bool       bold   = false;
    bool       italic = false;
    bool       code   = false;
    bool       strike = false;

    // 强调区间：记录闭合位置与闭合后要恢复的状态。
    struct Scope {
        int  closeAt;
        int  length;
        bool bold;
        bool italic;
        bool strike;
    };
    QVector<Scope> scopes;

    const auto flushPlain = [&]() {
        if (plain.isEmpty()) return;
        segments.append(textSegment(plain, bold, italic, code, strike));
        plain.clear();
    };
    const auto pushMath = [&](const QString& body, bool display) {
        flushPlain();
        QJsonObject segment{
            {"type",    "math"},
            {"content", body  }
        };
        if (display) segment.insert("display", true);
        segments.append(segment);
    };

    for (int pos = 0; pos < text.size();) {
        // 强调区间结束：先按当前状态输出缓冲区，再恢复外层状态。
        if (!scopes.isEmpty() && pos == scopes.last().closeAt) {
            flushPlain();
            const Scope scope  = scopes.takeLast();
            bold               = scope.bold;
            italic             = scope.italic;
            strike             = scope.strike;
            pos               += scope.length;
            continue;
        }

        const QChar ch      = text.at(pos);
        const bool  escaped = pos > 0 && text.at(pos - 1) == '\\';

        // ── 行内代码：内容原样输出 ──
        if (ch == '`' && !escaped) {
            const int end = text.indexOf('`', pos + 1);
            if (end > pos + 1) {
                flushPlain();
                segments.append(textSegment(text.mid(pos + 1, end - pos - 1), false, false, true, false));
                pos = end + 1;
                continue;
            }
        }

        // ── 公式 ──
        if (!escaped && (ch == '$' || ch == '\\')) {
            const QString two    = text.mid(pos, 2);
            const bool    paren  = two == "\\(";
            const bool    square = two == "\\[";
            const bool    dbl    = two == "$$";
            const bool    single = ch == '$' && !dbl;

            if (paren || square || dbl || single) {
                const int     openLength = single ? 1 : 2;
                const QString close      = paren ? "\\)" : (square ? "\\]" : (dbl ? "$$" : "$"));
                const int     bodyStart  = pos + openLength;
                int           closeAt    = -1;

                if (single) {
                    // 结尾的 $ 前面不能是空白，且跳过 $$ 避免错配。
                    for (int scan = bodyStart; scan < text.size();) {
                        const int found = text.indexOf('$', scan);
                        if (found < 0) break;
                        if (text.mid(found, 2) == "$$") {
                            scan = found + 2;
                            continue;
                        }
                        if (found > bodyStart && !text.at(found - 1).isSpace()) {
                            closeAt = found;
                            break;
                        }
                        scan = found + 1;
                    }
                } else {
                    closeAt = text.indexOf(close, bodyStart);
                }

                const QString body = closeAt < 0 ? QString() : text.mid(bodyStart, closeAt - bodyStart).trimmed();
                // `$ 5` 这类金额、`$$` 单独一行留下的空片段都不算公式。
                if (closeAt >= 0 && !body.isEmpty() && (!single || !text.at(bodyStart).isSpace())) {
                    pushMath(body, dbl || square);
                    pos = closeAt + close.size();
                    continue;
                }
            }
        }

        // ── 强调 ──
        if (!escaped && (ch == '*' || ch == '_')) {
            // `_` 不在单词内部开启强调，`snake_case_name` 保持原样。
            const bool intraword = ch == '_' && pos > 0 && isWordChar(text.at(pos - 1));

            int run = 0;
            while (!intraword && pos + run < text.size() && text.at(pos + run) == ch) ++run;

            const int length    = run >= 3 ? 3 : run;
            const int bodyStart = pos + length;
            if (!intraword && bodyStart < text.size() && !text.at(bodyStart).isSpace()) {
                const int closeAt = matchEmphasis(text, bodyStart, ch, length);
                if (closeAt > bodyStart) {
                    flushPlain();
                    scopes.append(Scope{closeAt, length, bold, italic, strike});
                    if (length >= 2) bold = true;
                    if (length != 2) italic = true;
                    pos = bodyStart;
                    continue;
                }
            }
        }

        if (!escaped && ch == '~' && text.mid(pos, 2) == "~~") {
            const int closeAt = text.indexOf("~~", pos + 2);
            if (closeAt > pos + 2) {
                flushPlain();
                scopes.append(Scope{closeAt, 2, bold, italic, strike});
                strike  = true;
                pos    += 2;
                continue;
            }
        }

        plain.append(ch);
        ++pos;
    }

    flushPlain();
    return segments;
}

// -----------------------------------------------------------------------
// 表格
// -----------------------------------------------------------------------

// 单元格切分：公式与行内代码里的 `|` 不是分隔符（`| $|x|$ |` 必须保持成一个单元格）。
QStringList tableCells(const QString& line) {
    QString body = line.trimmed();
    if (body.startsWith('|')) body.remove(0, 1);
    if (body.endsWith('|')) body.chop(1);

    QStringList cells;
    QString     cell;
    int         mode = 0; // 0 普通 / 1 行内代码 / 2 $…$ / 3 \(…\) / 4 \[…\] / 5 $$…$$
    for (int i = 0; i < body.size(); ++i) {
        const QChar ch = body.at(i);

        if (mode == 1) {
            if (ch == '`') mode = 0;
            cell.append(ch);
            continue;
        }
        if (mode == 2) {
            if (ch == '$') mode = 0;
            cell.append(ch);
            continue;
        }
        if (mode == 3 || mode == 4) {
            const QChar expected = mode == 3 ? ')' : ']';
            if (ch == '\\' && i + 1 < body.size() && body.at(i + 1) == expected) {
                mode = 0;
                cell.append(ch);
                cell.append(body.at(++i));
                continue;
            }
            cell.append(ch);
            continue;
        }
        if (mode == 5) {
            if (ch == '$' && i + 1 < body.size() && body.at(i + 1) == '$') {
                mode = 0;
                cell.append("$$");
                ++i;
                continue;
            }
            cell.append(ch);
            continue;
        }

        if (ch == '|') {
            cells.append(cell.trimmed());
            cell.clear();
            continue;
        }
        if (ch == '`') {
            mode = 1;
        } else if (ch == '$') {
            if (i + 1 < body.size() && body.at(i + 1) == '$') {
                mode = 5;
                cell.append("$$");
                ++i;
                continue;
            }
            mode = 2;
        } else if (ch == '\\' && i + 1 < body.size() && (body.at(i + 1) == '(' || body.at(i + 1) == '[')) {
            mode = body.at(i + 1) == '(' ? 3 : 4;
        }
        cell.append(ch);
    }
    cells.append(cell.trimmed());
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
    QStringList                     lines = text.split('\n', Qt::KeepEmptyParts);
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
            const int     bodyAt   = lines[i].indexOf(open) + open.size();
            QStringList   parts;
            // 闭合标记之后的同行内容留在 lines[i] 里继续解析，否则 `$$a$$ 说明` 会把
            // 后面的正文一起吞进公式。
            const int sameLine = lines[i].indexOf(close, bodyAt);
            if (sameLine >= 0) {
                parts.append(lines[i].mid(bodyAt, sameLine - bodyAt));
                lines[i] = lines[i].mid(sameLine + close.size());
            } else {
                parts.append(lines[i].mid(bodyAt));
                for (++i; i < lines.size(); ++i) {
                    const int closeAt = lines[i].indexOf(close);
                    if (closeAt >= 0) {
                        parts.append(lines[i].left(closeAt));
                        lines[i] = lines[i].mid(closeAt + close.size());
                        break;
                    }
                    parts.append(lines[i]);
                }
            }
            blocks.append(
                QJsonObject{
                    {"type",    "math_block"              },
                    {"content", parts.join('\n').trimmed()}
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
            // 单元格按片段输出，表格里的公式与粗体才能和段落一样渲染。
            QJsonArray headers;
            for (const QString& cell : headerCells) headers.append(inlineSegments(cell));
            QJsonArray rows;
            i += 2;
            while (i < lines.size() && lines[i].trimmed().startsWith('|')) {
                QStringList cells = tableCells(lines[i]);
                while (cells.size() < headerCells.size()) cells.append(QString());
                QJsonArray row;
                for (int column = 0; column < headerCells.size(); ++column)
                    row.append(inlineSegments(cells.value(column)));
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
