#pragma once

#include "common/service/Logger.h"

#include <QQuickImageProvider>
#include <QQuickPaintedItem>
#include <QImage>
#include <QTimer>

// 前向声明 libwebp 类型，避免优化编译的执行
typedef struct WebPAnimDecoder WebPAnimDecoder;

namespace mod::filemanager {

// WebP 图像提供者 - 用于 Image 组件的静态预览
class WebPImageProvider : public QQuickImageProvider, private Logger {
public:
    WebPImageProvider();
    QImage requestImage(const QString &id, QSize *size, const QSize &requestedSize) override;
    
    void setCurrentWebPPath(const QString &path);
    
private:
    QString m_currentWebPPath;
};

// WebP 动画渲染组件
class WebPAnimatedImage : public QQuickPaintedItem, private Logger {
    Q_OBJECT
    Q_PROPERTY(QString source READ source WRITE setSource NOTIFY sourceChanged)
    Q_PROPERTY(bool running READ running WRITE setRunning NOTIFY runningChanged)

public:
    explicit WebPAnimatedImage(QQuickItem *parent = nullptr);
    ~WebPAnimatedImage() override;

    void paint(QPainter *painter) override;

    QString source() const;
    void setSource(const QString &source);

    bool running() const;
    void setRunning(bool running);

signals:
    void sourceChanged();
    void runningChanged();

private:
    void load();
    void clear();
    void onTimeout();

    QString m_source;
    bool m_running = true;
    
    // WebP 数据与解码器
    QByteArray m_webpData;      // 保位 WebP 数据在内存中
    WebPAnimDecoder* m_decoder = nullptr;
    
    // 渲染状态
    QImage m_currentFrame;
    int m_prevTimestamp = 0;
    QTimer m_timer;
};

// 图片查看器 - 整合图片和 WebP 动画处理
class ImageViewer : public QObject, public Singleton<ImageViewer>, private Logger {
    Q_OBJECT
    Q_PROPERTY(QString source READ source NOTIFY sourceChanged)          // 图片蹴迹
    Q_PROPERTY(QString fullPath READ fullPath NOTIFY sourceChanged)      // 绝对路径
    Q_PROPERTY(bool isWebP READ isWebP NOTIFY sourceChanged)             // 是否为 WebP
    Q_PROPERTY(bool isAnimatedWebP READ isAnimatedWebP NOTIFY sourceChanged) // 是否为动画 WebP

public:
    QString source() const;
    QString fullPath() const;
    bool isWebP() const;
    bool isAnimatedWebP() const;

    Q_INVOKABLE void open(const QString &path);
    Q_INVOKABLE bool openAbsolute(const QString &path);

signals:
    void sourceChanged();

private:
    friend class Singleton<ImageViewer>;
    explicit ImageViewer(QObject *parent = nullptr);

    QString m_openingFileName;
    QString m_absolutePath;
    WebPImageProvider* m_webpProvider;
};

} // namespace mod::filemanager
