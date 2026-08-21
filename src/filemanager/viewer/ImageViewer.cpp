#include "filemanager/viewer/ImageViewer.h"

#include "filemanager/FileManager.h"

#include "common/Event.h"
#include "common/service/Logger.h"

#include <QQmlContext>
#include <QImage>
#include <QBuffer>
#include <QMimeDatabase>
#include <QQuickView>
#include <QPainter>

#include <webp/decode.h>
#include <webp/demux.h> 

namespace mod::filemanager {

// WebP 图像提供者 - 用于 Image 组件的静态预览

WebPImageProvider::WebPImageProvider() : QQuickImageProvider(QQuickImageProvider::Image), Logger("WebPImageProvider") {
}

QImage WebPImageProvider::requestImage(const QString &id, QSize *size, const QSize &requestedSize) {
    Q_UNUSED(id);
    Q_UNUSED(requestedSize);
    
    if (m_currentWebPPath.isEmpty()) {
        return QImage();
    }
    
    const QString fullPath = QFileInfo(m_currentWebPPath).isAbsolute()
                                 ? m_currentWebPPath
                                 : FileManager::getInstance().getCurrentPath().filePath(m_currentWebPPath);
    
    QFile webpFile(fullPath);
    if (!webpFile.open(QIODevice::ReadOnly)) {
        error("Could not open WebP file: {}", fullPath.toStdString());
        return QImage();
    }
    
    QByteArray webpData = webpFile.readAll();
    webpFile.close();

    // 准备 WebP 数据结构
    WebPData webp_data;
    webp_data.bytes = reinterpret_cast<const uint8_t*>(webpData.constData());
    webp_data.size = webpData.size();

    // 使用 WebPDemux 分析文件头，判断是否为动画
    WebPDemuxer* demux = WebPDemux(&webp_data);
    if (!demux) {
        error("Failed to parse WebP headers (Demux failed): {}", fullPath.toStdString());
        return QImage();
    }

    uint32_t flags = WebPDemuxGetI(demux, WEBP_FF_FORMAT_FLAGS);
    bool isAnimation = (flags & ANIMATION_FLAG);
    int width = WebPDemuxGetI(demux, WEBP_FF_CANVAS_WIDTH);
    int height = WebPDemuxGetI(demux, WEBP_FF_CANVAS_HEIGHT);
    WebPDemuxDelete(demux);

    QImage resultImage;

    if (isAnimation) {
        // 使用 WebPAnimDecoder 提取第一帧
        WebPAnimDecoderOptions dec_options;
        if (WebPAnimDecoderOptionsInit(&dec_options)) {
            dec_options.color_mode = MODE_RGBA;
            dec_options.use_threads = 0; // 仅提取一帧，不需要多线程开销

            WebPAnimDecoder* decoder = WebPAnimDecoderNew(&webp_data, &dec_options);
            if (decoder) {
                uint8_t* buf = nullptr;
                int timestamp = 0;
                
                // 获取第一帧
                if (WebPAnimDecoderGetNext(decoder, &buf, &timestamp)) {
                    // 深拷贝创建 QImage
                    QImage frame(buf, width, height, width * 4, QImage::Format_RGBA8888);
                    resultImage = frame.copy();
                    
                    // debug("Decoded first frame of animation: {}", fullPath.toStdString());
                } else {
                    error("Failed to get first frame from animation: {}", fullPath.toStdString());
                }
                WebPAnimDecoderDelete(decoder);
            } else {
                error("Failed to create AnimDecoder for animation file: {}", fullPath.toStdString());
            }
        }
    } else {
        // 非动画图片，直接解码
        uint8_t* decoded = WebPDecodeRGBA(webp_data.bytes, webp_data.size, &width, &height);
        
        if (decoded) {
            QImage image(decoded, width, height, width * 4, QImage::Format_RGBA8888);
            resultImage = image.copy();
            WebPFree(decoded);
            // debug("Decoded static WebP: {}", fullPath.toStdString());
        } else {
            error("Failed to decode static WebP image via WebPDecodeRGBA: {}", fullPath.toStdString());
        }
    }

    if (resultImage.isNull()) {
        return QImage();
    }

    if (size) {
        *size = resultImage.size();
    }
    
    return resultImage;
}

void WebPImageProvider::setCurrentWebPPath(const QString &path) {
    m_currentWebPPath = path;
}

// ================= WebPAnimatedImage =================

WebPAnimatedImage::WebPAnimatedImage(QQuickItem *parent) 
    : QQuickPaintedItem(parent), Logger("WebPAnimatedImage") 
{
    setOpaquePainting(false);
    // 使用高精度定时器保证动画平滑
    m_timer.setTimerType(Qt::PreciseTimer);
    connect(&m_timer, &QTimer::timeout, this, &WebPAnimatedImage::onTimeout);
    m_timer.setSingleShot(true); 
}

WebPAnimatedImage::~WebPAnimatedImage() {
    clear();
}

void WebPAnimatedImage::paint(QPainter *painter) {
    if (!m_currentFrame.isNull()) {
        QRectF targetRect(0, 0, width(), height());
        // 高质量缩放渲染
        painter->setRenderHint(QPainter::SmoothPixmapTransform, true);
        painter->setRenderHint(QPainter::Antialiasing, true);
        painter->drawImage(targetRect, m_currentFrame);
    }
}

QString WebPAnimatedImage::source() const {
    return m_source;
}

void WebPAnimatedImage::setSource(const QString &source) {
    if (m_source == source) return;
    m_source = source;
    load();
    emit sourceChanged();
}

bool WebPAnimatedImage::running() const {
    return m_running;
}

void WebPAnimatedImage::setRunning(bool running) {
    if (m_running == running) return;
    m_running = running;
    
    if (m_running) {
        if (m_decoder && !m_timer.isActive()) {
            m_timer.start(0);
        }
    } else {
        m_timer.stop();
    }
    emit runningChanged();
}

void WebPAnimatedImage::clear() {
    m_timer.stop();
    if (m_decoder) {
        WebPAnimDecoderDelete(m_decoder);
        m_decoder = nullptr;
    }
    m_webpData.clear();
    m_currentFrame = QImage();
    m_prevTimestamp = 0;
    update();
}

void WebPAnimatedImage::load() {
    clear();
    if (m_source.isEmpty()) return;

    // 移除 file:// 协议前缀
    QString path = m_source;
    if (path.startsWith("file://")) {
#ifdef Q_OS_WIN
        path = path.mid(8);
#else
        path = path.mid(7);
#endif
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        error("Failed to open WebP file: {}", path.toStdString());
        return;
    }

    m_webpData = file.readAll();
    file.close();

    WebPData webp_data;
    webp_data.bytes = reinterpret_cast<const uint8_t*>(m_webpData.constData());
    webp_data.size = m_webpData.size();

    // 初始化解码器
    WebPAnimDecoderOptions dec_options;
    if (!WebPAnimDecoderOptionsInit(&dec_options)) {
        error("Version mismatch for WebPAnimDecoderOptionsInit");
        return;
    }
    dec_options.color_mode = MODE_RGBA;
    dec_options.use_threads = 1;

    m_decoder = WebPAnimDecoderNew(&webp_data, &dec_options);
    if (!m_decoder) {
        error("Failed to create WebPAnimDecoder");
        return;
    }

    // 设置隐式大小供 QML 使用
    WebPAnimInfo anim_info;
    if (WebPAnimDecoderGetInfo(m_decoder, &anim_info)) {
        setImplicitWidth(anim_info.canvas_width);
        setImplicitHeight(anim_info.canvas_height);
        info("Loaded animated WebP: {}x{}, {} frames, {} loop count", 
             anim_info.canvas_width, anim_info.canvas_height, 
             anim_info.frame_count, anim_info.loop_count);
    }

    if (m_running) {
        m_timer.start(0);
    }
}

void WebPAnimatedImage::onTimeout() {
    if (!m_decoder || !m_running) return;

    uint8_t* buf = nullptr;
    int timestamp = 0;

    if (WebPAnimDecoderGetNext(m_decoder, &buf, &timestamp)) {
        WebPAnimInfo anim_info;
        WebPAnimDecoderGetInfo(m_decoder, &anim_info);

        QImage frame(buf, anim_info.canvas_width, anim_info.canvas_height, 
                     anim_info.canvas_width * 4, QImage::Format_RGBA8888);
        m_currentFrame = frame.copy();
        update();

        // 计算帧间隔并限制最小值以降低 CPU 占用
        int duration = timestamp - m_prevTimestamp;
        if (duration < 0) duration = 0;
        if (duration < 20) duration = 20;
        m_prevTimestamp = timestamp;
        m_timer.start(duration);
    } else {
        // 循环播放：重置解码器重新开始
        WebPAnimDecoderReset(m_decoder);
        m_prevTimestamp = 0;
        m_timer.start(0);
    }
}

// ================= ImageViewer =================

ImageViewer::ImageViewer(QObject *parent) : QObject(parent), Logger("ImageViewer") {
    m_webpProvider = new WebPImageProvider();
    
    connect(&Event::getInstance(), &Event::beforeUiInitialization, [this](QQuickView &view, QQmlContext *context) {
        context->setContextProperty("imageViewer", this);
        view.engine()->addImageProvider("webp", m_webpProvider);
        // 在 QML 中注册 WebPAnimatedImage 组件
        qmlRegisterType<WebPAnimatedImage>("Mod.FileManager", 1, 0, "WebPAnimatedImage");
    });
}

QString ImageViewer::source() const {
    if (m_openingFileName.isEmpty())
        return "";
    
    // WebP 图片返回 provider 路径供静态预览
    if (isWebP()) {
        const_cast<WebPImageProvider*>(m_webpProvider)->setCurrentWebPPath(fullPath());
        return "image://webp/current";
    }
    
    return QString("file://%1").arg(fullPath());
}

QString ImageViewer::fullPath() const {
    if (!m_absolutePath.isEmpty()) return m_absolutePath;
    if (m_openingFileName.isEmpty()) return "";
    return FileManager::getInstance().getCurrentPath().filePath(m_openingFileName);
}

bool ImageViewer::isWebP() const {
    return m_openingFileName.toLower().endsWith(".webp");
}

bool ImageViewer::isAnimatedWebP() const {
    if (!isWebP()) return false;

    QString fullPath = this->fullPath();
    QFile webpFile(fullPath);
    if (!webpFile.open(QIODevice::ReadOnly)) {
        const_cast<ImageViewer*>(this)->error("Could not open WebP file for animation check: {}", fullPath.toStdString());
        return false;
    }

    QByteArray webpData = webpFile.readAll();
    webpFile.close();

    // 使用 WebPDemux 分析文件头判断是否为动画
    WebPData webp_data;
    webp_data.bytes = reinterpret_cast<const uint8_t*>(webpData.constData());
    webp_data.size = webpData.size();

    WebPDemuxer* demux = WebPDemux(&webp_data);
    if (!demux) {
        const_cast<ImageViewer*>(this)->error("Failed to parse WebP headers for animation check: {}", fullPath.toStdString());
        return false;
    }

    uint32_t flags = WebPDemuxGetI(demux, WEBP_FF_FORMAT_FLAGS);
    bool isAnimation = (flags & ANIMATION_FLAG);
    WebPDemuxDelete(demux);
    return isAnimation;
}

void ImageViewer::open(const QString &path) {
    m_absolutePath.clear();
    m_openingFileName = path;
    emit sourceChanged();
}

bool ImageViewer::openAbsolute(const QString &path) {
    const QFileInfo file(path);
    if (!file.isAbsolute() || !file.isFile() || !file.isReadable()) {
        warn("拒绝打开无效的绝对图片路径: {}", path.toStdString());
        return false;
    }

    m_absolutePath = file.canonicalFilePath();
    m_openingFileName = file.fileName();
    emit sourceChanged();
    return true;
}

} // namespace mod::filemanager