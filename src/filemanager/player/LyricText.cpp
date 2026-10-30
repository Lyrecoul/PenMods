// SPDX-License-Identifier: GPL-3.0-only
#include "LyricText.h"

#include <QMap>
#include <QRegularExpression>
#include <QStringList>

namespace mod::filemanager {
namespace {

QMap<qint64, QString> timedLines(const QString& lrc) {
    static const QRegularExpression stamp(QStringLiteral("\\[(\\d+):(\\d{2})(?:[.:](\\d{1,3}))?\\]"));
    static const QRegularExpression offsetTag(
        QStringLiteral("\\[offset:([+-]?\\d+)\\]"),
        QRegularExpression::CaseInsensitiveOption
    );
    const auto            offsetMatch = offsetTag.match(lrc);
    const auto            offset      = offsetMatch.hasMatch() ? offsetMatch.captured(1).toLongLong() : 0;
    QMap<qint64, QString> lines;
    for (const auto& line : lrc.split('\n')) {
        auto          matches = stamp.globalMatch(line);
        QList<qint64> times;
        int           textStart = 0;
        while (matches.hasNext()) {
            const auto match = matches.next();
            // Time labels must form a prefix; bracketed timestamps in the lyrics are literal text.
            if (match.capturedStart() != textStart) break;
            const auto seconds = match.captured(2).toInt();
            if (seconds >= 60) break;
            const auto ms = match.captured(3).leftJustified(3, QLatin1Char('0')).toInt();
            times << qMax<qint64>(0, match.captured(1).toLongLong() * 60000 + seconds * 1000 + ms + offset);
            textStart = match.capturedEnd();
        }
        const auto text = line.mid(textStart).trimmed();
        for (const auto time : times) {
            if (!lines.contains(time) || lines.value(time).isEmpty()) lines[time] = text;
            else if (!text.isEmpty() && lines.value(time) != text) lines[time] += " / " + text;
        }
    }
    return lines;
}

} // namespace

QString bilingualLrc(const QString& original, const QString& translation, bool& hasTranslation) {
    const auto originals    = timedLines(original);
    const auto translations = timedLines(translation);
    hasTranslation          = false;
    QString result;
    for (auto it = originals.cbegin(); it != originals.cend(); ++it) {
        const auto time        = it.key();
        const auto timestamp   = QStringLiteral("[%1:%2.%3]")
                                     .arg(time / 60000, 2, 10, QLatin1Char('0'))
                                     .arg(time / 1000 % 60, 2, 10, QLatin1Char('0'))
                                     .arg(time % 1000, 3, 10, QLatin1Char('0'));
        result                += timestamp + it.value() + '\n';
        const auto translated  = translations.value(time);
        if (!it.value().isEmpty() && !translated.isEmpty() && translated != it.value()) {
            result         += timestamp + translated + '\n';
            hasTranslation  = true;
        }
    }
    return result;
}

} // namespace mod::filemanager
