#include "common/Resource.h"
#include "common/Event.h"

#include <QFileInfo>
#include <QQmlContext>

namespace mod {

Resource::Resource() {
    connect(&Event::getInstance(), &Event::beforeUiInitialization, [this](QQuickView& view, QQmlContext* context) {
        context->setContextProperty("res", this);
    });
}

QString Resource::get(const QString& name) { return QString("qrc:/images/%1").arg(name); }

QString Resource::getDisk(const QString& name) { return QString("image://icons/%1").arg(name); }

} // namespace mod
