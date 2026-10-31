// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <QColor>
#include <QFont>
#include <QImage>
#include <QQuickItem>
#include <QVariantMap>

namespace mod::filemanager {
class LyricLineItem : public QQuickItem {
    Q_OBJECT
    Q_PROPERTY(QVariantMap line READ line WRITE setLine NOTIFY appearanceChanged)
    Q_PROPERTY(QFont font READ font WRITE setFont NOTIFY appearanceChanged)
    Q_PROPERTY(QColor pendingColor READ pendingColor WRITE setPendingColor NOTIFY appearanceChanged)
    Q_PROPERTY(QColor playedColor READ playedColor WRITE setPlayedColor NOTIFY appearanceChanged)
    Q_PROPERTY(qint64 position READ position WRITE setPosition NOTIFY positionChanged)
    Q_PROPERTY(qreal focusY READ focusY NOTIFY focusYChanged)
    Q_PROPERTY(bool active READ active WRITE setActive NOTIFY activeChanged)
public:
    explicit LyricLineItem(QQuickItem* parent = nullptr);
    QVariantMap line() const { return mLine; }
    QFont       font() const { return mFont; }
    QColor      pendingColor() const { return mPending; }
    QColor      playedColor() const { return mPlayed; }
    qint64      position() const { return mPosition; }
    qreal       focusY() const { return mFocusY; }
    bool        active() const { return mActive; }
    void        setLine(const QVariantMap& value);
    void        setFont(const QFont& value);
    void        setPendingColor(const QColor& value);
    void        setPlayedColor(const QColor& value);
    void        setPosition(qint64 value);
    void        setActive(bool value);
signals:
    void appearanceChanged();
    void positionChanged();
    void focusYChanged();
    void activeChanged();

protected:
    void     updatePolish() override;
    QSGNode* updatePaintNode(QSGNode* old, UpdatePaintNodeData*) override;
    void     geometryChanged(const QRectF& next, const QRectF& previous) override;

private:
    struct Fragment {
        QRectF rect;
        int    cue;
        qreal  offset, total;
    };
    void                           updateFocus();
    void                           invalidate();
    QVector<QRectF>                highlightRects() const;
    QVariantMap                    mLine;
    QFont                          mFont;
    QColor                         mPending{"#c0c4cc"}, mPlayed{"#8ac8ff"};
    QImage                         mBase, mBright;
    QVector<Fragment>              mFragments;
    QVector<QPair<qint64, qint64>> mTimes;
    qreal                          mFocusY     = 0;
    qint64                         mPosition   = 0;
    int                            mGeneration = 0;
    bool                           mDirty = true, mActive = false;
};
} // namespace mod::filemanager
