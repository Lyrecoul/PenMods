// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QString>
#include <QVector>

namespace mod::filemanager {

struct LyricCue {
    qint64 time;
    int    end;        // UTF-16 boundary in text, never splits a grapheme.
    qint64 until = -1; // Unknown end: highlight at the start, without inventing a singing duration.
};
struct LyricLine {
    qint64            time;
    QString           text;
    QString           translation;
    QVector<LyricCue> cues;
};

// Equal timestamps form one bilingual row. Word timestamps are absolute Enhanced LRC times.
QVector<LyricLine> lyricsFromOnlineResponse(const QByteArray& data);
QString            serializeLyrics(const QVector<LyricLine>& lines);
QVector<LyricLine> parseExtendedLyrics(const QString& source);
bool               isLyricSuffix(const QString& suffix);
QVector<LyricLine> parseLyrics(const QString& source);
int                lyricLineAt(const QVector<LyricLine>& lines, qint64 position);
QString            lyricMarkup(const LyricLine& line, qint64 position, const QString& played, const QString& pending);

} // namespace mod::filemanager
