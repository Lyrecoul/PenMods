// SPDX-License-Identifier: GPL-3.0-only
#include "tweaker/ReadingLoop.h"
#include "common/Event.h"
#include <QQmlContext>

namespace mod {
namespace {
thread_local unsigned int screenOffDepth = 0;
}

ReadingLoop::ReadingLoop() {
    connect(&Event::getInstance(), &Event::beforeUiInitialization, this, [this](QQuickView&, QQmlContext* context) {
        context->setContextProperty("readingLoop", this);
    });
}
double ReadingLoop::interruptionEpoch() const { return static_cast<double>(mEpoch.load()); }
bool   ReadingLoop::active() const { return mPlayId.load() != 0; }

void ReadingLoop::claim(unsigned int playId) {
    if (playId == 0) return;
    mClaimEpoch.store(mEpoch.load());
    if (mPlayId.exchange(playId) == 0) emit activeChanged();
}

void ReadingLoop::release(unsigned int playId) {
    if (playId != 0 && mPlayId.compare_exchange_strong(playId, 0)) emit activeChanged();
}

void ReadingLoop::beginScreenOff() { ++screenOffDepth; }
void ReadingLoop::endScreenOff() { --screenOffDepth; }

bool ReadingLoop::shouldSuppressScreenOffStop(unsigned int currentPlayId) const {
    // onScreenOff calls stop unconditionally, even for the poem-reading mode.
    // Do not adopt other audio or suppress an explicit stop after this stack unwinds.
    return screenOffDepth != 0 && currentPlayId != 0 && mPlayId.load() == currentPlayId
        && mClaimEpoch.load() == mEpoch.load();
}

void ReadingLoop::interrupt() {
    mEpoch.fetch_add(1);
    // Hooks may be called off the UI thread. The epoch itself changes synchronously,
    // so a completion already queued to QML cannot race an explicit stop.
    // QML releases the claim when it observes this epoch. Keeping it until then
    // also avoids dropping the suspend lock during play()'s internal stop/start.
    QMetaObject::invokeMethod(this, [this] { emit interruptionEpochChanged(); }, Qt::QueuedConnection);
}

PEN_HOOK(void, _ZN12YSoundCenter4stopEv, void* self) {
    auto& loop = ReadingLoop::getInstance();
    if (loop.active()
        && loop.shouldSuppressScreenOffStop(
            PEN_CALL(unsigned int, "_ZNK12YSoundCenter13currentPlayIdEv", void*)(self)
        )) {
        return;
    }
    loop.interrupt();
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
