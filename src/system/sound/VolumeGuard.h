// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "common/service/Singleton.h"
#include <QObject>
#include <array>
#include <atomic>

namespace mod {

// Limits are percentages in the vendor's volume scale, independent of mixer ranges.
class VolumeGuard : public QObject, public Singleton<VolumeGuard> {
    Q_OBJECT
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY settingsChanged)
    Q_PROPERTY(QString outputName READ outputName NOTIFY outputChanged)
    Q_PROPERTY(int currentLimit READ currentLimit NOTIFY outputChanged)
public:
    bool             enabled() const { return mEnabled.load(); }
    void             setEnabled(bool enabled);
    QString          outputName() const;
    int              currentLimit() const;
    Q_INVOKABLE int  limit(int output) const;
    Q_INVOKABLE void setLimit(int output, int value);
    int              clamp(int value, int output) const;
    int              currentOutput() const { return mOutput.load(); }
    static int       parseOutput(const QString& output);
    void             noteVolumeWrite();
    int              measuredVolume() const;

signals:
    void settingsChanged();
    void outputChanged();

private:
    friend Singleton<VolumeGuard>;
    VolumeGuard();
    void                            save();
    void                            refresh();
    void                            enforce();
    void                            synchronize(int measured);
    std::atomic<int>                mMeasured{-1};
    std::atomic<qint64>             mLastWrite{0};
    std::atomic<qint64>             mLastMeasurement{0};
    std::atomic<bool>               mEnabled{true};
    std::array<std::atomic<int>, 4> mLimits{
        {70, 50, 50, 50}
    };
    std::atomic<int> mOutput{-1};
    bool             mReady = false;
};

} // namespace mod
