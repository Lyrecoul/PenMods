// SPDX-License-Identifier: GPL-3.0-only
#include "LyricTimeline.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QStringList>
#include <QXmlStreamReader>
#include <algorithm>
#include <cmath>

namespace mod::filemanager {
namespace {
QString stamp(qint64 ms, bool word = false) {
    ms               = qMax<qint64>(0, ms);
    const auto value = QStringLiteral("%1:%2.%3")
                           .arg(ms / 60000, 2, 10, QLatin1Char('0'))
                           .arg(ms / 1000 % 60, 2, 10, QLatin1Char('0'))
                           .arg(ms % 1000, 3, 10, QLatin1Char('0'));
    return (word ? "<" : "[") + value + (word ? ">" : "]");
}
qint64 xmlTime(const QString& text) {
    static const QRegularExpression clock(QStringLiteral(R"(^(?:(\d+):)?(\d{1,2}):(\d{2}(?:\.\d+)?)$)"));
    const auto                      m     = clock.match(text);
    double                          value = -1;
    if (m.hasMatch() && m.captured(2).toInt() < 60 && m.captured(3).toDouble() < 60)
        value = m.captured(1).toDouble() * 3600000 + m.captured(2).toDouble() * 60000 + m.captured(3).toDouble() * 1000;
    else {
        static const QRegularExpression offset(QStringLiteral(R"(^(\d+(?:\.\d+)?)(ms|s)$)"));
        const auto                      n = offset.match(text);
        if (n.hasMatch()) value = n.captured(1).toDouble() * (n.captured(2) == "s" ? 1000 : 1);
    }
    return std::isfinite(value) && value >= 0 && value <= 604800000 ? qRound64(value) : -1;
}
QVector<LyricLine> parseTtml(const QString& source) {
    QXmlStreamReader xml(source);
    struct Frame {
        qint64  begin = -1, end = -1;
        QString role;
        bool    timed = false;
    };
    QVector<Frame>     stack;
    QString            qrcContent;
    QVector<LyricLine> rows;
    LyricLine          row{};
    int                paragraph = -1;
    bool               valid = true, missingTiming = false;
    while (!xml.atEnd()) {
        const auto token = xml.readNext();
        if (token == QXmlStreamReader::DTD || token == QXmlStreamReader::EntityReference) return {};
        if (xml.isStartElement()) {
            if (stack.size() >= 32 || rows.size() > 10000) return {};
            Frame      frame = stack.isEmpty() ? Frame{} : stack.last();
            const auto attrs = xml.attributes();
            if (xml.name() == "Lyric_1" && attrs.hasAttribute("LyricContent"))
                qrcContent = attrs.value("LyricContent").toString();
            for (const auto& attr : attrs) {
                if (attr.name() == "role") frame.role = attr.value().toString();
            }
            if (attrs.hasAttribute("begin")) {
                frame.begin = xmlTime(attrs.value("begin").toString());
                frame.timed = true;
                if (frame.begin < 0) valid = false;
            }
            if (attrs.hasAttribute("end")) frame.end = xmlTime(attrs.value("end").toString());
            else if (attrs.hasAttribute("dur") && frame.begin >= 0) {
                const auto duration = xmlTime(attrs.value("dur").toString());
                frame.end           = duration >= 0 ? frame.begin + duration : -1;
            }
            stack.append(frame);
            if (xml.name() == "p") {
                if (paragraph >= 0) return {};
                paragraph          = stack.size();
                row                = {frame.begin, {}, {}, {}};
                stack.last().timed = false; // A paragraph duration is not a word duration.
                missingTiming      = false;
                if (row.time < 0) valid = false;
            } else if (xml.name() == "br" && paragraph >= 0) {
                if (frame.role.contains("translation")) row.translation += '\n';
                else {
                    row.text      += '\n';
                    missingTiming  = true;
                }
            }
        } else if (xml.isCharacters() && paragraph >= 0 && !stack.isEmpty()) {
            const auto frame = stack.last();
            const auto text  = xml.text().toString();
            if (frame.role.contains("translation")) row.translation += text;
            else if (!frame.role.contains("roman") && !frame.role.contains("metadata")) {
                // Ignore XML indentation between spans, while preserving meaningful word spaces.
                if (text.trimmed().isEmpty() && text.contains('\n')) continue;
                row.text += text;
                if (frame.timed && stack.size() > paragraph && frame.end > frame.begin) {
                    if (!row.cues.isEmpty() && row.cues.last().time == frame.begin
                        && row.cues.last().until == frame.end)
                        row.cues.last().end = row.text.size();
                    else row.cues.append({frame.begin, row.text.size(), frame.end});
                } else missingTiming = true;
            }
        } else if (xml.isEndElement()) {
            if (paragraph == stack.size()) {
                if (missingTiming) row.cues.clear();
                if (row.text.isEmpty() && !row.translation.isEmpty()) {
                    row.text = row.translation;
                    row.translation.clear();
                }
                if (!row.text.isEmpty()) rows.append(row);
                paragraph = -1;
            }
            if (!stack.isEmpty()) stack.removeLast();
        }
    }
    if (xml.hasError() || !valid) return {};
    if (!qrcContent.isEmpty() && qrcContent.trimmed().startsWith('[')) return parseExtendedLyrics(qrcContent);
    return parseLyrics(serializeLyrics(rows)); // Apply shared ordering and grapheme validation.
}
} // namespace
bool isLyricSuffix(const QString& suffix) {
    return QStringList{"lrc", "yrc", "qrc", "ttml"}.contains(suffix.toLower());
}
QVector<LyricLine> lyricsFromOnlineResponse(const QByteArray& data) {
    if (data.size() > 1024 * 1024) return {};
    const auto root = QJsonDocument::fromJson(data).object();
    if (root.value("code").toInt() != 200) return {};
    const auto field = [&](const char* name) { return root.value(name).toObject().value("lyric").toString(); };
    auto       rows  = parseLyrics(field("yrc"));
    if (rows.isEmpty()
        || std::none_of(rows.cbegin(), rows.cend(), [](const LyricLine& line) { return !line.cues.isEmpty(); }))
        rows = parseLyrics(field("lrc"));
    const auto translated = parseLyrics(field("ytlrc").isEmpty() ? field("tlyric") : field("ytlrc"));
    for (const auto& line : translated) {
        if (line.text.isEmpty()) continue;
        int        best     = -1;
        qint64     distance = 501;
        const auto it = std::lower_bound(rows.cbegin(), rows.cend(), line.time, [](const LyricLine& row, qint64 time) {
            return row.time < time;
        });
        const int  after = int(it - rows.cbegin());
        for (int i : {after - 1, after}) {
            if (i < 0 || i >= rows.size()) continue;
            const auto delta = qAbs(rows[i].time - line.time);
            if (delta < distance) {
                best     = i;
                distance = delta;
            }
        }
        if (best >= 0 && rows[best].text != line.text && !rows[best].translation.split('\n').contains(line.text)) {
            if (!rows[best].translation.isEmpty()) rows[best].translation += '\n';
            rows[best].translation += line.text;
        }
    }
    return rows;
}
QString serializeLyrics(const QVector<LyricLine>& lines) {
    QString result;
    for (const auto& line : lines) {
        auto text = line.text;
        text.replace('\n', ' ');
        result     += stamp(line.time);
        int cursor  = 0;
        for (const auto& cue : line.cues) {
            if (cue.end <= cursor || cue.end > line.text.size()) continue;
            result += stamp(cue.time, true) + text.mid(cursor, cue.end - cursor);
            if (cue.until >= cue.time) result += stamp(cue.until, true);
            cursor = cue.end;
        }
        result += text.mid(cursor) + '\n';
        for (const auto& translation : line.translation.split('\n'))
            if (!translation.isEmpty()) result += stamp(line.time) + translation + '\n';
    }
    return result;
}
QVector<LyricLine> parseExtendedLyrics(const QString& source) {
    if (source.trimmed().startsWith('<')) return parseTtml(source);
    static const QRegularExpression rowTag(QStringLiteral(R"(^\[(\d{1,9}),(\d{1,9})\])"));
    static const QRegularExpression yrcWord(QStringLiteral(R"(\((\d{1,9}),(\d{1,9}),\d+\))"));
    static const QRegularExpression qrcWord(QStringLiteral(R"(\((\d{1,9}),(\d{1,9})\))"));
    QVector<LyricLine>              rows;
    for (const auto& raw : source.split('\n')) {
        const auto m = rowTag.match(raw.trimmed());
        if (!m.hasMatch()) continue;
        const auto body = raw.trimmed().mid(m.capturedEnd());
        LyricLine  row{m.captured(1).toLongLong(), {}, {}, {}};
        const bool yrc     = yrcWord.match(body).hasMatch();
        auto       matches = (yrc ? yrcWord : qrcWord).globalMatch(body);
        int        cursor  = 0;
        qint64     begin = row.time, end = -1;
        bool       valid = true;
        while (matches.hasNext()) {
            const auto word     = matches.next();
            const auto fragment = body.mid(cursor, word.capturedStart() - cursor);
            const auto start = word.captured(1).toLongLong(), duration = word.captured(2).toLongLong();
            if (yrc) {
                row.text += fragment;
                if (!fragment.isEmpty()) row.cues.append({begin, row.text.size(), end});
                begin = start;
                end   = start + duration;
            } else {
                row.text += fragment;
                if (!fragment.isEmpty()) row.cues.append({start, row.text.size(), start + duration});
            }
            cursor = word.capturedEnd();
        }
        const auto tail  = body.mid(cursor);
        row.text        += tail;
        if (yrc && !tail.isEmpty()) row.cues.append({begin, row.text.size(), end});
        else if (!yrc && !tail.isEmpty()) valid = false;
        qint64 previous = row.time;
        for (const auto& cue : row.cues) {
            if (cue.time < previous || cue.until < cue.time) valid = false;
            previous = cue.until;
        }
        if (!valid) row.cues.clear();
        rows.append(row);
    }
    return parseLyrics(serializeLyrics(rows));
}
} // namespace mod::filemanager
