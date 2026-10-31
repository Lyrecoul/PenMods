// SPDX-License-Identifier: GPL-3.0-only
#include "filemanager/player/LyricText.h"
#include "filemanager/player/MusicPlayer.h"

#include "base/YPointer.h"
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSaveFile>
#include <QUrlQuery>
#include <algorithm>

namespace mod::filemanager {

void MusicPlayer::clearLyrics() {
    mLyrics.clear();
    emit lyricsChanged();
}

void MusicPlayer::loadLyrics(const QString& path) {
    QFile file(path);
    mLyrics.clear();
    if (file.open(QIODevice::ReadOnly) && file.size() <= 2 * 1024 * 1024)
        mLyrics = parseLyrics(QString::fromUtf8(file.readAll()));
    emit lyricsChanged();
}

QVariantList MusicPlayer::lyricRows() const {
    QVariantList rows;
    for (const auto& line : mLyrics) {
        QVariantList cues;
        for (const auto& cue : line.cues)
            cues.append(
                QVariantMap{
                    {"time",  cue.time },
                    {"until", cue.until},
                    {"end",   cue.end  }
            }
            );
        rows.append(
            QVariantMap{
                {"mainLrc",  line.text       },
                {"transLrc", line.translation},
                {"time",     line.time       },
                {"cues",     cues            }
        }
        );
    }
    return rows;
}

bool MusicPlayer::hasLyricTranslation() const {
    return std::any_of(mLyrics.cbegin(), mLyrics.cend(), [](const LyricLine& line) {
        return !line.translation.isEmpty();
    });
}

QString MusicPlayer::lyricText(int index, qint64 position, const QString& played, const QString& pending) const {
    if (index < 0 || index >= mLyrics.size()) return {};
    return lyricMarkup(mLyrics[index], position, played, pending);
}

bool MusicPlayer::canFetchLyrics() const {
    if (hasTimeline()) return false;
    if (!mIsTakeOver || !mCurrentPlaying.mFile || mCurrentPlaying.mIsEnd) return false;
    auto* manager = YPointer<YMediaPlayerManager>::getInstance();
    return manager && !PEN_CALL(bool, "_ZNK19YMediaPlayerManager6hasLrcEv", void*)(manager);
}

QString MusicPlayer::lyricSearchKeyword() const {
    return canFetchLyrics() ? mCurrentPlaying.mFile->completeBaseName() : QString();
}

void MusicPlayer::cancelOnlineLyrics() {
    if (mLyricReply) {
        auto* reply = mLyricReply.data();
        mLyricReply.clear();
        reply->disconnect(this);
        reply->abort();
        reply->deleteLater();
    }
    mLyricPreview.clear();
    mLyricDownload.clear();
    mLyricCandidates.clear();
    mLyricTarget.clear();
    mLyricStatus.clear();
    emit onlineLyricsChanged();
}

void MusicPlayer::lyricRequest(const QUrl& url, std::function<void(const QByteArray&)> done) {
    if (!mLyricNetwork) mLyricNetwork = new QNetworkAccessManager(this);
    QNetworkRequest request(url);
    request.setRawHeader("Referer", "https://music.163.com/");
    request.setRawHeader("User-Agent", "Mozilla/5.0");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    auto* reply = mLyricNetwork->get(request);
    mLyricReply = reply;
    auto* timer = new QTimer(reply);
    timer->setSingleShot(true);
    connect(timer, &QTimer::timeout, reply, &QNetworkReply::abort);
    timer->start(15000);
    // Bound the response before parsing; neither endpoint needs large media payloads.
    connect(reply, &QNetworkReply::readyRead, this, [reply]() {
        if (reply->bytesAvailable() > 1024 * 1024) reply->abort();
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, done]() {
        mLyricReply.clear();
        reply->deleteLater();
        if (!canFetchLyrics() || mCurrentPlaying.mFile->absoluteFilePath() != mLyricTarget) {
            cancelOnlineLyrics();
            return;
        }
        if (reply->error() != QNetworkReply::NoError || reply->bytesAvailable() > 1024 * 1024) {
            mLyricStatus = QStringLiteral("获取失败，请检查网络后重试");
        } else {
            done(reply->readAll());
        }
        emit onlineLyricsChanged();
    });
    emit onlineLyricsChanged();
}

void MusicPlayer::searchOnlineLyrics(const QString& keyword) {
    cancelOnlineLyrics();
    if (!canFetchLyrics() || keyword.trimmed().isEmpty()) return;
    mLyricTarget = mCurrentPlaying.mFile->absoluteFilePath();
    mLyricStatus = QStringLiteral("正在搜索网易云音乐…");
    QUrl      url(QStringLiteral("https://music.163.com/api/search/get/web"));
    QUrlQuery query;
    query.addQueryItem("s", keyword.trimmed().left(200));
    query.addQueryItem("type", "1");
    query.addQueryItem("limit", "8");
    url.setQuery(query);
    lyricRequest(url, [this](const QByteArray& data) {
        const auto root = QJsonDocument::fromJson(data).object();
        if (root.value("code").toInt() != 200) {
            mLyricStatus = QStringLiteral("搜索服务异常，请稍后重试");
            return;
        }
        const auto songs = root.value("result").toObject().value("songs").toArray();
        for (const auto& value : songs) {
            const auto   song  = value.toObject();
            const double id    = song.value("id").toDouble();
            const auto   title = song.value("name").toString();
            if (id <= 0 || title.isEmpty()) continue;
            QStringList artists;
            for (const auto& artist : song.value("artists").toArray()) {
                artists << artist.toObject().value("name").toString();
            }
            const int seconds = song.value("duration").toInt() / 1000;
            mLyricCandidates << QVariantMap{
                {"id", QString::number(id, 'f', 0)},
                {"title", title},
                {"detail",
                 artists.join(" / ") + QString(" · %1:%2").arg(seconds / 60).arg(seconds % 60, 2, 10, QLatin1Char('0'))}
            };
        }
        mLyricStatus = mLyricCandidates.isEmpty() ? QStringLiteral("未找到歌曲，可修改关键词重试")
                                                  : QStringLiteral("请选择对应歌曲，下载并保存歌词");
    });
}

void MusicPlayer::saveOnlineLyrics(int index) {
    if (lyricBusy() || !canFetchLyrics() || index < 0 || index >= mLyricCandidates.size()
        || mCurrentPlaying.mFile->absoluteFilePath() != mLyricTarget)
        return;
    const auto id = mLyricCandidates.at(index).toMap().value("id").toString();
    QUrl       url(QStringLiteral("https://music.163.com/api/song/lyric/v1"));
    QUrlQuery  query;
    query.addQueryItem("id", id);
    query.addQueryItem("lv", "-1");
    query.addQueryItem("tv", "-1");
    query.addQueryItem("yv", "-1");
    query.addQueryItem("rv", "-1");
    url.setQuery(query);
    mLyricPreview.clear();
    mLyricDownload.clear();
    mLyricStatus = QStringLiteral("正在下载歌词预览…");
    lyricRequest(url, [this](const QByteArray& data) {
        const auto rows = lyricsFromOnlineResponse(data);
        if (rows.isEmpty()) {
            mLyricStatus = QStringLiteral("该歌曲没有可用的同步歌词，请选其他版本");
            return;
        }
        mLyricDownload        = serializeLyrics(rows);
        bool        wordTimed = false, bilingual = false;
        QStringList preview;
        for (const auto& row : rows) {
            wordTimed = wordTimed || !row.cues.isEmpty();
            bilingual = bilingual || !row.translation.isEmpty();
            if (!row.text.isEmpty()) {
                preview << row.text;
                if (!row.translation.isEmpty()) preview << row.translation;
            }
        }
        mLyricPreview = preview.join('\n').left(12000);
        mLyricStatus =
            (wordTimed ? QStringLiteral("逐字歌词") : QStringLiteral("逐句歌词"))
            + (bilingual ? QStringLiteral(" · 含译文。核对后点击保存") : QStringLiteral(" · 暂无译文。核对后点击保存"));
    });
}

void MusicPlayer::commitOnlineLyrics() {
    if (lyricBusy() || !canFetchLyrics() || mLyricDownload.isEmpty() || !mCurrentPlaying.mFile
        || mCurrentPlaying.mFile->absoluteFilePath() != mLyricTarget)
        return;
    const auto      combined = mLyricDownload;
    const QFileInfo audio(mLyricTarget);
    const auto      path = audio.absoluteDir().filePath(audio.completeBaseName() + ".lrc");
    // Never overwrite a user's sidecar, even if the host could not parse it.
    if (QFileInfo::exists(path)) {
        mLyricStatus = QStringLiteral("同名歌词已存在，未覆盖原文件");
        emit onlineLyricsChanged();
        return;
    }
    QSaveFile  file(path);
    const auto bytes = combined.toUtf8();
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
        mLyricStatus = QStringLiteral("保存失败，请检查音乐目录是否可写");
        emit onlineLyricsChanged();
        return;
    }
    auto* manager = YPointer<YMediaPlayerManager>::getInstance();
    PEN_CALL(void, "_ZN19YMediaPlayerManager15loadLrcFromFileERK7QString", void*, const QString&)(manager, path);
    loadLyrics(path);
    mLyricStatus = QStringLiteral("歌词已保存到音乐目录，逐字时间与译文已保留");
    mLyricCandidates.clear();
    mLyricDownload.clear();
    mLyricPreview.clear();
    emit onlineLyricsChanged();
}

} // namespace mod::filemanager
