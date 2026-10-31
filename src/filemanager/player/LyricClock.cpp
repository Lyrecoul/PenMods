// SPDX-License-Identifier: GPL-3.0-only
#include "LyricClock.h"
#include <QDebug>
#include <QtMath>
#include <algorithm>

namespace mod::filemanager {
LyricClock::LyricClock(QObject* parent) : QObject(parent) {
    mTime.start();
    mTimer.setInterval(33);
    mTimer.setTimerType(Qt::PreciseTimer);
    connect(&mTimer, &QTimer::timeout, this, &LyricClock::publish);
}
qint64 LyricClock::estimated() const {
    // Stop extrapolating if the host stops advancing (buffering/stall).
    const qint64 elapsed = mPlaying && running() ? qBound<qint64>(0, mTime.elapsed() - mAnchorAt, 1500) : 0;
    return qMax<qint64>(0, mAnchor + qRound64(elapsed * mRate));
}
void LyricClock::publish() {
    const auto value = estimated();
    if (value == mPosition) return;
    mPosition = value;
    emit positionChanged();
}
void LyricClock::reset() {
    mAnchor     = mSource;
    mAnchorAt   = mTime.elapsed();
    mLastSample = -1;
    publish();
}
void LyricClock::setSourcePosition(qint64 value) {
    value = qMax<qint64>(0, value);
    if (value == mSource) return;
    const auto   now       = mTime.elapsed();
    const qint64 predicted = estimated();
    if (running() && mPlaying && mLastSample >= 0 && value > mSource && now - mLastSample < 5000) {
        mSamples.append(now - mLastSample);
        if (mSamples.size() == 32) reportSamples();
    }
    mLastSample        = now;
    const qint64 error = value - predicted;
    // Large jumps and any authoritative backwards movement land immediately.
    mAnchor   = !mPlaying || value < mSource || qAbs(error) > 250 ? value : predicted + error / 4;
    mSource   = value;
    mAnchorAt = now;
    emit sourceChanged();
    publish();
}
void LyricClock::setPlaybackRate(qreal value) {
    if (!qIsFinite(value) || value <= 0 || qFuzzyCompare(value, mRate)) return;
    mAnchor   = estimated();
    mAnchorAt = mTime.elapsed();
    mRate     = value;
    emit sourceChanged();
    publish();
}
void LyricClock::setPlaying(bool value) {
    if (value == mPlaying) return;
    mAnchor   = value ? mSource : estimated();
    mAnchorAt = mTime.elapsed();
    mPlaying  = value;
    emit sourceChanged();
    publish();
}
void LyricClock::setRunning(bool value) {
    if (value == running()) return;
    if (value) {
        mTimer.start();
        reset();
    } else {
        mAnchor = estimated();
        mTimer.stop();
        publish();
        reportSamples();
    }
    emit sourceChanged();
}
void LyricClock::reportSamples() {
    if (mSamples.isEmpty()) return;
    std::sort(mSamples.begin(), mSamples.end());
    qInfo().nospace() << "[LyricClock] samples=" << mSamples.size() << " medianMs=" << mSamples[mSamples.size() / 2]
                      << " p95Ms=" << mSamples[qMin(mSamples.size() - 1, int(mSamples.size() * .95))];
    mSamples.clear();
}
} // namespace mod::filemanager
