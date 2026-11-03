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
public:
    double interruptionEpoch() const;
    void   interrupt();
signals:
    void interruptionEpochChanged();

private:
    friend Singleton<ReadingLoop>;
    ReadingLoop();
    std::atomic<unsigned long long> mEpoch{0};
};
} // namespace mod
