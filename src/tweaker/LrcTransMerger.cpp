// SPDX-License-Identifier: GPL-3.0-only
// Keep the host sentence model for follow-reading/repeat controls; the music page uses our timeline directly.
#include "base/Hook.h"
#include "filemanager/player/MusicPlayer.h"

#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryFile>

PEN_HOOK(void, _ZN19YMediaPlayerManager15loadLrcFromFileERK7QString, void* self, const QString* path) {
    auto& player = mod::filemanager::MusicPlayer::getInstance();
    player.clearLyrics();
    if (!path || !mod::filemanager::isLyricSuffix(QFileInfo(*path).suffix())) {
        origin(self, path);
        return;
    }
    player.loadLyrics(*path);
    QFile file(*path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 2 * 1024 * 1024) {
        origin(self, path);
        return;
    }
    const auto lines = mod::filemanager::parseLyrics(QString::fromUtf8(file.readAll()));
    if (lines.isEmpty()) {
        origin(self, path);
        return;
    }
    QJsonArray array;
    for (int i = 0; i < lines.size(); ++i) {
        const auto& line = lines[i];
        array.append(
            QJsonObject{
                {"text",  line.text                                                             },
                {"trans", line.translation                                                      },
                {"start", line.time / 1000.0                                                    },
                {"end",   (i + 1 < lines.size() ? lines[i + 1].time : line.time + 5000) / 1000.0}
        }
        );
    }
    QTemporaryFile converted(QStringLiteral("/tmp/penmods-lyrics-XXXXXX.json"));
    const auto     bytes = QJsonDocument(array).toJson(QJsonDocument::Compact);
    if (!converted.open() || converted.write(bytes) != bytes.size() || !converted.flush()) {
        origin(self, path);
        return;
    }
    const auto convertedPath = converted.fileName();
    origin(self, &convertedPath);
}
