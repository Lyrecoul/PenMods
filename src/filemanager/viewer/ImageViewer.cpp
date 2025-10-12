#include "filemanager/viewer/ImageViewer.h"
#include "filemanager/Config.h"
#include "filemanager/FileManager.h"

#include "common/Event.h"

#include <QQmlContext>

FILEMANAGER_BEGIN

ImageViewer::ImageViewer(QObject *parent) : QObject(parent) {
    connect(&Event::getInstance(), &Event::beforeUiInitialization, [this](QQuickView &view, QQmlContext *context) {
        context->setContextProperty("imageViewer", this);
    });
}

QString ImageViewer::source() const {
    if (m_openingFileName.isEmpty())
        return "";
    return QString("file://%1").arg(FileManager::getInstance().getCurrentPath().filePath(m_openingFileName));
}

void ImageViewer::open(const QString &path) {
    m_openingFileName = path;
    emit sourceChanged();
}

FILEMANAGER_END