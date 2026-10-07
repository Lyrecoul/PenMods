#pragma once

#include "FFmpegVideoPlayerQml.hpp"
#include <QQmlExtensionPlugin>

class FFmpegPlayerPlugin : public QQmlExtensionPlugin {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID QQmlExtensionInterface_iid)

public:
    void registerTypes(const char *uri) override {
        qmlRegisterType<ffmpeg_player::VideoPlayer>(uri, 1, 0, "VideoPlayer");
    }
};
