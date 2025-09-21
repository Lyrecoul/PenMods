#pragma once

#include "common/service/Logger.h"

namespace mod {

class ASound : public QObject, public Singleton<ASound>, private Logger {
    Q_OBJECT

public:
    void onUiCompleted();

    struct VoiceDb {
        float min;
        float max;
    };

    bool setDb(VoiceDb);

    VoiceDb getDb();

private:
    friend Singleton<ASound>;
    explicit ASound();

    struct Config {
        std::string mPath;
        std::string mContent;
    };

    VoiceDb mVoiceDb{};

    bool _resetConfig();

    Config _getConfig();

    std::string _getRawConfigure(const char* model);
};

} // namespace mod
