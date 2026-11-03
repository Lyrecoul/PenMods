// SPDX-License-Identifier: GPL-3.0-only
#include "tweaker/ReadingLoop.h"
#include "common/Event.h"
#include <QQmlContext>

namespace mod {
ReadingLoop::ReadingLoop() {
    connect(&Event::getInstance(), &Event::beforeUiInitialization, this, [this](QQuickView&, QQmlContext* context) {
        context->setContextProperty("readingLoop", this);
    });
}
double ReadingLoop::interruptionEpoch() const { return static_cast<double>(mEpoch.load()); }
void   ReadingLoop::interrupt() {
    mEpoch.fetch_add(1);
    // Hooks may be called off the UI thread. The epoch itself changes synchronously,
    // so a completion already queued to QML cannot race an explicit stop.
    QMetaObject::invokeMethod(this, [this] { emit interruptionEpochChanged(); }, Qt::QueuedConnection);
}

PEN_HOOK(void, _ZN12YSoundCenter4stopEv, void* self) {
    ReadingLoop::getInstance().interrupt();
    origin(self);
}
PEN_HOOK(void, _ZN12YSoundCenter9forceStopEv, void* self) {
    ReadingLoop::getInstance().interrupt();
    origin(self);
}
PEN_HOOK(void, _ZN12YSoundCenter7suspendEv, void* self) {
    ReadingLoop::getInstance().interrupt();
    origin(self);
}
PEN_HOOK(void, _ZN12YSoundCenter16killSoundProcessEb, void* self, bool restart) {
    ReadingLoop::getInstance().interrupt();
    origin(self, restart);
}
} // namespace mod
