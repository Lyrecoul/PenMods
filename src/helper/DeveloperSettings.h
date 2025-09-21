#pragma once

#include "mod/Config.h"

namespace mod {

class DeveloperSettings : public QObject, public Singleton<DeveloperSettings> {
    Q_OBJECT

    Q_PROPERTY(bool offlineRM READ getOfflineRM WRITE setOfflineRM NOTIFY offlineRMChanged);

public:
    [[nodiscard]] bool getOfflineRM() const;
    void               setOfflineRM(bool);

signals:

    void offlineRMChanged();

private:
    friend Singleton<DeveloperSettings>;
    explicit DeveloperSettings();

    std::string mClassName = "dev";
    json        mCfg;

    bool mOfflineRM;
};

} // namespace mod
