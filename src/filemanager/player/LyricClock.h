// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <QElapsedTimer>
#include <QObject>
#include <QTimer>
#include <QVector>

namespace mod::filemanager {
// A display clock only: never changes the decoder position.
class LyricClock : public QObject {
    Q_OBJECT
    Q_PROPERTY(qint64 sourcePosition READ sourcePosition WRITE setSourcePosition NOTIFY sourceChanged)
    Q_PROPERTY(qreal playbackRate READ playbackRate WRITE setPlaybackRate NOTIFY sourceChanged)
    Q_PROPERTY(bool playing READ playing WRITE setPlaying NOTIFY sourceChanged)
    Q_PROPERTY(bool running READ running WRITE setRunning NOTIFY sourceChanged)
    Q_PROPERTY(qint64 position READ position NOTIFY positionChanged)
public:
    explicit LyricClock(QObject* parent = nullptr);
    qint64           sourcePosition() const { return mSource; }
    qreal            playbackRate() const { return mRate; }
    bool             playing() const { return mPlaying; }
    bool             running() const { return mTimer.isActive(); }
    qint64           position() const { return mPosition; }
    void             setSourcePosition(qint64 value);
    void             setPlaybackRate(qreal value);
    void             setPlaying(bool value);
    void             setRunning(bool value);
    Q_INVOKABLE void reset();
signals:
    void sourceChanged();
    void positionChanged();

private:
    qint64          estimated() const;
    void            publish();
    void            reportSamples();
    QElapsedTimer   mTime;
    QTimer          mTimer;
    qint64          mSource = 0, mPosition = 0, mAnchor = 0, mAnchorAt = 0, mLastSample = -1;
    qreal           mRate    = 1;
    bool            mPlaying = false;
    QVector<qint64> mSamples;
};
} // namespace mod::filemanager
