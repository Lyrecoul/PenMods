// SPDX-License-Identifier: GPL-3.0-only
#include "LyricLineItem.h"
#include <QPainter>
#include <QQuickWindow>
#include <QSGSimpleTextureNode>
#include <QTextBoundaryFinder>
#include <QTextLayout>
#include <algorithm>
#include <cmath>

namespace mod::filemanager {
namespace {
struct LineNode : QSGNode {
    QSGTexture *                   base = nullptr, *bright = nullptr;
    int                            generation = -1;
    QVector<QSGSimpleTextureNode*> bands;
    ~LineNode() override {
        // Children do not own the shared textures.
        while (auto* child = firstChild()) {
            removeChildNode(child);
            delete child;
        }
        delete base;
        delete bright;
    }
};
} // namespace
LyricLineItem::LyricLineItem(QQuickItem* parent) : QQuickItem(parent) {
    setFlag(ItemHasContents, true);
    mFont.setPixelSize(20);
}
void LyricLineItem::invalidate() {
    mDirty = true;
    polish();
    update();
}
void LyricLineItem::setLine(const QVariantMap& value) {
    if (mLine == value) return;
    mLine = value;
    invalidate();
    emit appearanceChanged();
}
void LyricLineItem::setFont(const QFont& value) {
    if (mFont == value) return;
    mFont = value;
    invalidate();
    emit appearanceChanged();
}
void LyricLineItem::setPendingColor(const QColor& value) {
    if (mPending == value) return;
    mPending = value;
    invalidate();
    emit appearanceChanged();
}
void LyricLineItem::setPlayedColor(const QColor& value) {
    if (mPlayed == value) return;
    mPlayed = value;
    invalidate();
    emit appearanceChanged();
}
void LyricLineItem::setPosition(qint64 value) {
    if (mPosition == value) return;
    mPosition = value;
    updateFocus();
    if (mActive) update();
    emit positionChanged();
}
void LyricLineItem::setActive(bool value) {
    if (mActive == value) return;
    mActive = value;
    update();
    emit activeChanged();
}
void LyricLineItem::geometryChanged(const QRectF& next, const QRectF& previous) {
    QQuickItem::geometryChanged(next, previous);
    if (next.width() != previous.width()) invalidate();
}
void LyricLineItem::updatePolish() {
    if (!mDirty) return;
    mDirty = false;
    mFragments.clear();
    mTimes.clear();
    const QString text = mLine.value("mainLrc").toString();
    const auto    cues = mLine.value("cues").toList();
    QVector<int>  ends;
    for (const auto& value : cues) {
        const auto cue = value.toMap();
        ends.append(cue.value("end").toInt());
        mTimes.append({cue.value("time").toLongLong(), cue.value("until", -1).toLongLong()});
    }
    if (ends.isEmpty()) {
        ends.append(text.size());
        mTimes.append({mLine.value("time").toLongLong(), -1});
    }
    QTextLayout layout(text, mFont);
    QTextOption option;
    option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    layout.setTextOption(option);
    QVector<int>        boundaries{0};
    QTextBoundaryFinder finder(QTextBoundaryFinder::Grapheme, text);
    while (finder.toNextBoundary() >= 0) boundaries.append(finder.position());
    const int      pixels = qBound(1, int(std::ceil(width())), 1024);
    QVector<qreal> totals(ends.size(), 0);
    layout.beginLayout();
    qreal y = 0;
    while (y < 4096) {
        auto line = layout.createLine();
        if (!line.isValid()) break;
        line.setLineWidth(pixels);
        line.setPosition(QPointF(0, y));
        const qreal lineHeight = qMax<qreal>(26, std::ceil(line.height()) + 2);
        const int   end        = line.textStart() + line.textLength();
        auto        first      = std::lower_bound(boundaries.cbegin(), boundaries.cend(), line.textStart());
        for (auto it = first; it != boundaries.cend() && *it < end; ++it) {
            if (it + 1 == boundaries.cend()) break;
            const int begin = *it, finish = qMin(*(it + 1), end);
            const int cue = int(std::upper_bound(ends.cbegin(), ends.cend(), begin) - ends.cbegin());
            if (cue >= ends.size()) continue;
            const qreal x1      = line.cursorToX(begin, QTextLine::Leading);
            const qreal x2      = line.cursorToX(finish - 1, QTextLine::Trailing);
            const qreal advance = qAbs(x2 - x1);
            mFragments.append({QRectF(x1, y, x2 - x1, lineHeight), cue, totals[cue], 0});
            totals[cue] += advance;
        }
        y += lineHeight;
    }
    layout.endLayout();
    for (auto& fragment : mFragments) fragment.total = totals[fragment.cue];
    mBase = QImage(pixels, qMax(1, int(std::ceil(y))), QImage::Format_ARGB32_Premultiplied);
    mBase.fill(Qt::transparent);
    mBright = mBase.copy();
    {
        QPainter painter(&mBase);
        painter.setPen(mPending);
        layout.draw(&painter, QPointF());
    }
    {
        QPainter painter(&mBright);
        painter.setPen(mPlayed);
        layout.draw(&painter, QPointF());
    }
    // Trim font fallback ascent/descent padding once, keeping the rendered ink intact.
    int inkTop = mBase.height(), inkBottom = -1;
    for (int row = 0; row < mBase.height(); ++row) {
        const auto* pixels = reinterpret_cast<const QRgb*>(mBase.constScanLine(row));
        for (int col = 0; col < mBase.width(); ++col) {
            if (qAlpha(pixels[col]) > 0) {
                inkTop    = qMin(inkTop, row);
                inkBottom = row;
                break;
            }
        }
    }
    if (inkBottom >= inkTop) {
        const int top    = qMax(0, inkTop - 1);
        const int bottom = qMin(mBase.height(), inkBottom + 2);
        mBase            = mBase.copy(0, top, pixels, bottom - top);
        mBright          = mBright.copy(0, top, pixels, bottom - top);
        for (auto& fragment : mFragments) fragment.rect.translate(0, -top);
        setImplicitHeight(bottom - top);
    } else setImplicitHeight(0);
    ++mGeneration;
    updateFocus();
    update();
}
void LyricLineItem::updateFocus() {
    qreal focus = 0;
    for (const auto& fragment : mFragments) {
        const auto time = mTimes[fragment.cue];
        if (time.second <= time.first || mPosition < time.first) continue;
        const qreal fraction = qBound<qreal>(0, qreal(mPosition - time.first) / (time.second - time.first), 1);
        if (fragment.offset <= fraction * fragment.total) focus = qMax(focus, fragment.rect.top());
    }
    if (qFuzzyCompare(focus + 1, mFocusY + 1)) return;
    mFocusY = focus;
    emit focusYChanged();
}
QVector<QRectF> LyricLineItem::highlightRects() const {
    QVector<QRectF> result;
    if (!mActive) return result;
    for (const auto& fragment : mFragments) {
        const auto  time     = mTimes[fragment.cue];
        const qreal fraction = time.second > time.first
                                 ? qBound<qreal>(0, qreal(mPosition - time.first) / (time.second - time.first), 1)
                                 : (mPosition >= time.first ? 1 : 0);
        const qreal advance  = qAbs(fragment.rect.width());
        const qreal amount   = qBound<qreal>(0, fragment.total * fraction - fragment.offset, advance);
        if (amount < .001) continue;
        const auto& rect = fragment.rect;
        result.append(QRectF(rect.width() < 0 ? rect.x() - amount : rect.x(), rect.y(), amount, rect.height())
                          .intersected(QRectF(QPointF(), mBase.size())));
    }
    std::sort(result.begin(), result.end(), [](const QRectF& a, const QRectF& b) {
        return a.y() == b.y() ? a.x() < b.x() : a.y() < b.y();
    });
    QVector<QRectF> merged;
    for (const auto& rect : result) {
        if (!merged.isEmpty() && merged.last().y() == rect.y() && rect.left() <= merged.last().right() + .05)
            merged.last() = merged.last().united(rect);
        else merged.append(rect);
    }
    return merged;
}
QSGNode* LyricLineItem::updatePaintNode(QSGNode* old, UpdatePaintNodeData*) {
    auto* node = static_cast<LineNode*>(old);
    if (mBase.isNull() || width() <= 0) {
        delete node;
        return nullptr;
    }
    if (!node || node->generation != mGeneration) {
        delete node;
        node             = new LineNode;
        node->generation = mGeneration;
        node->base       = window()->createTextureFromImage(mBase);
        node->bright     = window()->createTextureFromImage(mBright);
        if (!node->base || !node->bright) {
            delete node;
            return nullptr;
        }
        auto* base = new QSGSimpleTextureNode;
        base->setTexture(node->base);
        base->setRect(QRectF(QPointF(), mBase.size()));
        node->appendChildNode(base);
    }
    const auto bands = highlightRects();
    while (node->bands.size() > bands.size()) {
        auto* band = node->bands.takeLast();
        node->removeChildNode(band);
        delete band;
    }
    while (node->bands.size() < bands.size()) {
        auto* band = new QSGSimpleTextureNode;
        band->setTexture(node->bright);
        node->bands.append(band);
        node->appendChildNode(band);
    }
    for (int i = 0; i < bands.size(); ++i) {
        node->bands[i]->setRect(bands[i]);
        node->bands[i]->setSourceRect(bands[i]);
    }
    return node;
}
} // namespace mod::filemanager
