// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "common/service/Singleton.h"
#include <QObject>
#include <atomic>

namespace mod {
// Separates explicit stop/takeover from soundEnd: the host also emits end from stop().
class ReadingLoop : public QObject, public Singleton<ReadingLoop> {
    Q_OBJECT
    Q_PROPERTY(double interruptionEpoch READ interruptionEpoch NOTIFY interruptionEpochChanged)
    Q_PROPERTY(bool active READ active NOTIFY activeChanged)
public:
    double           interruptionEpoch() const;
    bool             active() const;
    Q_INVOKABLE void claim(unsigned int playId);
    Q_INVOKABLE void release(unsigned int playId);
    // Only the synchronous vendor screen-off handler may ignore its implicit stop.
    void beginScreenOff();
    void endScreenOff();
    bool shouldSuppressScreenOffStop(unsigned int currentPlayId) const;
    void interrupt();
signals:
    void interruptionEpochChanged();
    void activeChanged();

private:
    friend Singleton<ReadingLoop>;
    ReadingLoop();
    std::atomic<unsigned long long> mEpoch{0};
    std::atomic<unsigned long long> mClaimEpoch{0};
    std::atomic<unsigned int>       mPlayId{0};
};
} // namespace mod
