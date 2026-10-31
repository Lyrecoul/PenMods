// SPDX-License-Identifier: GPL-3.0-only
#include "LyricTimeline.h"

#include <QMap>
#include <QRegularExpression>
#include <QStringList>
#include <QTextBoundaryFinder>
#include <algorithm>

namespace mod::filemanager {
namespace {
qint64 timestamp(const QRegularExpressionMatch& match) {
    return match.captured(1).toLongLong() * 60000 + match.captured(2).toInt() * 1000
         + match.captured(3).leftJustified(3, QLatin1Char('0')).toInt();
}
} // namespace

QVector<LyricLine> parseLyrics(const QString& source) {
    if (source.size() > 2 * 1024 * 1024) return {};
    if (source.startsWith(QChar(0xfeff))) {
        int start = 0;
        while (start < source.size() && source[start] == QChar(0xfeff)) ++start;
        return parseLyrics(source.mid(start));
    }
    static const QRegularExpression extended(QStringLiteral(R"((?:^|\n)\s*\[\d+,\d+\])"));
    if (source.trimmed().startsWith('<') || extended.match(source).hasMatch()) return parseExtendedLyrics(source);
    static const QRegularExpression stamp(QStringLiteral(R"(\[(\d{1,6}):(\d{2})(?:[.:](\d{1,3}))?\])"));
    static const QRegularExpression word(QStringLiteral(R"(<(\d{1,6}):(\d{2})(?:[.:](\d{1,3}))?>)"));
    static const QRegularExpression offsetTag(
        QStringLiteral(R"(\[offset:([+-]?\d{1,9})\])"),
        QRegularExpression::CaseInsensitiveOption
    );
    const auto              offsetMatch = offsetTag.match(source);
    const qint64            offset      = offsetMatch.captured(1).toLongLong();
    QMap<qint64, LyricLine> rows;
    for (QString raw : source.split('\n')) {
        raw = raw.trimmed();
        if (raw.startsWith(QChar(0xfeff))) raw.remove(0, 1);
        QVector<qint64> times;
        int             start   = 0;
        auto            matches = stamp.globalMatch(raw);
        while (matches.hasNext()) {
            const auto match = matches.next();
            if (match.capturedStart() != start || match.captured(2).toInt() >= 60) break;
            times.append(timestamp(match));
            start = match.capturedEnd();
        }
        if (times.isEmpty()) continue;
        const QString     body = raw.mid(start).trimmed();
        QString           text;
        QVector<LyricCue> cues;
        auto              words   = word.globalMatch(body);
        int               cursor  = 0;
        qint64            cueTime = times.first();
        bool              valid   = true;
        while (words.hasNext()) {
            const auto match  = words.next();
            text             += body.mid(cursor, match.capturedStart() - cursor);
            if (!text.isEmpty() && (cues.isEmpty() || cues.last().end < text.size()))
                cues.append({cueTime, text.size(), timestamp(match)});
            const qint64 next = timestamp(match);
            if (match.captured(2).toInt() >= 60 || next < cueTime) valid = false;
            cueTime = next;
            cursor  = match.capturedEnd();
        }
        text += body.mid(cursor);
        if (cursor > 0 && !text.isEmpty() && (cues.isEmpty() || cues.last().end < text.size()))
            cues.append({cueTime, text.size(), -1});
        QTextBoundaryFinder boundaries(QTextBoundaryFinder::Grapheme, text);
        for (const auto& cue : cues) {
            boundaries.setPosition(cue.end);
            if (!boundaries.isAtBoundary()) valid = false;
        }
        if (!valid || cursor == 0) cues.clear();
        for (const qint64 time : times) {
            const qint64 adjusted = qMax<qint64>(0, time + offset);
            auto         it       = rows.find(adjusted);
            if (it == rows.end()) {
                auto shifted = cues;
                for (auto& cue : shifted) {
                    cue.time = qMax<qint64>(0, cue.time + time - times.first() + offset);
                    if (cue.until >= 0) cue.until = qMax<qint64>(0, cue.until + time - times.first() + offset);
                }
                rows.insert(adjusted, {adjusted, text, {}, shifted});
            } else if (it->text.isEmpty()) {
                it->text = text;
                it->cues = cues;
                for (auto& cue : it->cues) {
                    cue.time = qMax<qint64>(0, cue.time + time - times.first() + offset);
                    if (cue.until >= 0) cue.until = qMax<qint64>(0, cue.until + time - times.first() + offset);
                }
            } else if (!text.isEmpty() && it->text != text && !it->translation.split('\n').contains(text)) {
                if (!it->translation.isEmpty()) it->translation += '\n';
                it->translation += text;
            }
        }
    }
    QVector<LyricLine> result;
    result.reserve(rows.size());
    for (const auto& row : rows) result.append(row);
    return result;
}

int lyricLineAt(const QVector<LyricLine>& lines, qint64 position) {
    const auto it = std::upper_bound(lines.cbegin(), lines.cend(), position, [](qint64 time, const LyricLine& line) {
        return time < line.time;
    });
    return int(it - lines.cbegin()) - 1;
}

QString lyricMarkup(const LyricLine& line, qint64 position, const QString& played, const QString& pending) {
    int end = line.cues.isEmpty() && position >= line.time ? line.text.size() : 0;
    for (const auto& cue : line.cues) {
        if (cue.time > position) break;
        end = cue.end;
    }
    return QStringLiteral("<font color=\"%1\">%2</font><font color=\"%3\">%4</font>")
        .arg(
            played.toHtmlEscaped(),
            line.text.left(end).toHtmlEscaped(),
            pending.toHtmlEscaped(),
            line.text.mid(end).toHtmlEscaped()
        );
}
} // namespace mod::filemanager
