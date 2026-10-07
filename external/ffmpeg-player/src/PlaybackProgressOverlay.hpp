#pragma once

#include <QColor>
#include <QQuickItem>
#include <QSGSimpleRectNode>
#include <algorithm>

namespace ffmpeg_player {

// 独立场景图图层，无需复制视频帧，软件和 OpenGL 后端均可绘制。
class PlaybackProgressOverlay final : public QQuickItem {
public:
  explicit PlaybackProgressOverlay(QQuickItem* parent) : QQuickItem(parent) {
    setFlag(ItemHasContents, true);
    setAcceptedMouseButtons(Qt::NoButton);
    setZ(2);
    setVisible(false);
  }

  void setProgress(qreal fraction, const QColor& color, const QColor& background) {
    fraction = std::clamp(fraction, qreal(0), qreal(1));
    if (fraction_ == fraction && color_ == color && background_ == background)
      return;
    fraction_ = fraction;
    color_ = color;
    background_ = background;
    update();
  }

protected:
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
  void geometryChange(const QRectF& current, const QRectF& previous) override {
    QQuickItem::geometryChange(current, previous);
    update();
  }
#else
  void geometryChanged(const QRectF& current, const QRectF& previous) override {
    QQuickItem::geometryChanged(current, previous);
    update();
  }
#endif

  QSGNode* updatePaintNode(QSGNode* old, UpdatePaintNodeData*) override {
    auto* background = static_cast<QSGSimpleRectNode*>(old);
    if (width() <= 0 || height() <= 0) {
      delete background;
      return nullptr;
    }
    if (!background) {
      background = new QSGSimpleRectNode(boundingRect(), background_);
      background->appendChildNode(new QSGSimpleRectNode(
          QRectF(0, 0, width() * fraction_, height()), color_));
    }
    background->setRect(boundingRect());
    background->setColor(background_);
    auto* progress = static_cast<QSGSimpleRectNode*>(background->firstChild());
    progress->setRect(0, 0, width() * fraction_, height());
    progress->setColor(color_);
    return background;
  }

private:
  qreal fraction_ = 0;
  QColor color_{QStringLiteral("#80cfff")};
  QColor background_{Qt::transparent};
};

} // namespace ffmpeg_player
