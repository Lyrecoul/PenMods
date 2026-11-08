// SPDX-License-Identifier: GPL-3.0-only
// Host seam: real ReadingLoop.cpp + real YReadingLoopControl.qml. Only the
// closed-source sound player and hook registration are replaced at the boundary.
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlExpression>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTest>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <thread>

static void* testSymbol(const char* name);
#define PEN_CALL(ret_t, sym, args_t...) reinterpret_cast<ret_t (*)(args_t)>(testSymbol(sym))
#define PEN_HOOK(ret_t, sym, args_t...)                                                                                \
    struct HookRegistrar_##sym {                                                                                       \
        static ret_t (*origin)(args_t);                                                                                \
        static ret_t detour(args_t);                                                                                   \
    };                                                                                                                 \
    ret_t (*HookRegistrar_##sym::origin)(args_t) = nullptr;                                                            \
    ret_t HookRegistrar_##sym::detour(args_t)

#include "tweaker/ReadingLoop.cpp"

// Keep the actual Event interface; no UI or device hook setup is needed here.
mod::Event::Event() = default;

class SoundCenter final : public QObject {
    Q_OBJECT
    Q_PROPERTY(unsigned int currentPlayId READ currentPlayId NOTIFY currentPlayIdChanged)
    Q_PROPERTY(int starts READ starts NOTIFY currentPlayIdChanged)
public:
    unsigned int             currentPlayId() const { return mId; }
    int                      starts() const { return mStarts; }
    int                      stops = 0;
    Q_INVOKABLE unsigned int play() {
        // The real host play()/playFile() stops the previous audio first.
        stop();
        ++mStarts;
        ++mId;
        emit currentPlayIdChanged();
        return mId;
    }
    Q_INVOKABLE void stop() { mod::HookRegistrar__ZN12YSoundCenter4stopEv::detour(this); }
    void             finish() { emit soundEnd(mId); }
    void             takeOver() {
        ++mId;
        emit currentPlayIdChanged();
    }
    void originalStop() {
        ++stops;
        emit soundEnd(mId);
    }
signals:
    void currentPlayIdChanged();
    void soundEnd(unsigned int seq);
    void soundSuspend(unsigned int seq);

private:
    unsigned int mId     = 0;
    int          mStarts = 0;
};

static void* testSymbol(const char* name) {
    if (std::strcmp(name, "_ZNK12YSoundCenter13currentPlayIdEv") == 0) {
        return reinterpret_cast<void*>(+[](void* self) { return static_cast<SoundCenter*>(self)->currentPlayId(); });
    }
    qFatal("Unexpected vendor symbol: %s", name);
}

static void check(bool condition, const char* message) {
    if (!condition) qFatal("FAIL: %s", message);
}

static QVariant evaluate(QQmlEngine& engine, QObject* object, const QString& source) {
    QQmlExpression expression(engine.rootContext(), object, source);
    QVariant       result = expression.evaluate();
    if (expression.hasError()) qFatal("%s", qPrintable(expression.error().toString()));
    return result;
}

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    check(argc == 2, "expected QML source directory");
    auto&       loop = mod::ReadingLoop::getInstance();
    SoundCenter sound;
    mod::HookRegistrar__ZN12YSoundCenter4stopEv::origin =
        +[](void* self) { static_cast<SoundCenter*>(self)->originalStop(); };
    mod::HookRegistrar__ZN12YSoundCenter9forceStopEv::origin =
        +[](void* self) { static_cast<SoundCenter*>(self)->originalStop(); };
    mod::HookRegistrar__ZN12YSoundCenter7suspendEv::origin =
        +[](void* self) { static_cast<SoundCenter*>(self)->originalStop(); };
    mod::HookRegistrar__ZN12YSoundCenter16killSoundProcessEb::origin =
        +[](void* self, bool) { static_cast<SoundCenter*>(self)->originalStop(); };
    QQmlEngine engine;
    engine.rootContext()->setContextProperty("readingLoop", &loop);
    engine.rootContext()->setContextProperty("soundCenter", &sound);
    engine.rootContext()->setContextProperty(
        "qmlGlobal",
        QVariantMap{
            {"fontFamily", "sans-serif"}
    }
    );
    QQmlComponent mediaComponent(&engine);
    mediaComponent.setData("import QtQuick 2.12; QtObject { property bool active: false }", QUrl());
    std::unique_ptr<QObject> media(mediaComponent.create());
    check(media != nullptr, "media fixture created");
    engine.rootContext()->setContextProperty("mediaSession", media.get());
    QQmlComponent component(&engine);
    component.setData(
        R"(
import QtQuick 2.12
Item {
    width: 320; height: 100
    property bool failNext: false
    YReadingLoopControl {
        id: loopControl
        objectName: "readingLoopControl"
        contentKey: "poem-1"
        playOnce: function() { return parent.failNext ? 0 : soundCenter.play() }
    }
})",
        QUrl::fromLocalFile(QString::fromLocal8Bit(argv[1]) + "/dicts/ReadingLoopHarness.qml")
    );
    if (component.isError()) qFatal("%s", qPrintable(component.errorString()));
    std::unique_ptr<QObject> root(component.create());
    check(root != nullptr, "real QML component created");
    auto* control = root->findChild<QObject*>("readingLoopControl");
    check(control != nullptr, "real control found");
    QQuickWindow window;
    static_cast<QQuickItem*>(root.get())->setParentItem(window.contentItem());
    window.resize(320, 100);
    window.show();
    QTest::qWait(30);
    auto start = [&] {
        evaluate(engine, control, "looping = true; startOne()");
        check(control->property("looping").toBool() && loop.active(), "loop owns active session");
    };
    auto cancel = [&] {
        evaluate(engine, control, "cancel(true)");
        check(!control->property("looping").toBool() && !loop.active(), "cancel releases session");
    };
    auto finish = [&] {
        sound.finish();
        QTest::qWait(450);
    };
    auto screenOff = [&] {
        const int    before = sound.stops;
        const double epoch  = loop.interruptionEpoch();
        loop.beginScreenOff();
        sound.stop();
        sound.stop(); // Both onScreenOff stop sites, including non-poem reading.
        loop.endScreenOff();
        window.hide();
        check(sound.stops == before && loop.interruptionEpoch() == epoch, "screen-off implicit stops suppressed");
    };
    start();
    screenOff();
    int before = sound.starts();
    finish();
    check(sound.starts() == before + 1 && loop.active(), "screen-off playback replays");
    puts("PASS: real stop hook suppresses only screen-off stops; QML replays with window hidden");

    QSignalSpy activeChanges(&loop, &mod::ReadingLoop::activeChanged);
    sound.finish();
    QTest::qWait(100);
    check(loop.active() && activeChanges.isEmpty(), "session held through replay gap");
    screenOff();
    QTest::qWait(350);
    check(loop.active() && activeChanges.isEmpty(), "session held through internal stop/play");
    puts("PASS: active stays armed through 350ms gap and play internal stop/start");

    before = sound.starts();
    sound.stop();
    QTest::qWait(450);
    check(
        !loop.active() && !control->property("looping").toBool() && sound.starts() == before,
        "explicit stop while screen off cancels replay"
    );
    puts("PASS: explicit stop while screen off terminates loop");

    window.show();
    start();
    sound.finish();
    cancel();
    before = sound.starts();
    QTest::qWait(450);
    check(sound.starts() == before, "cancelled pending replay does not run");
    puts("PASS: loop toggle stop cancels queued completion");

    start();
    root->setProperty("visible", false);
    check(!loop.active(), "page exit releases session");
    root->setProperty("visible", true);
    start();
    control->setProperty("contentKey", "poem-2");
    check(!loop.active(), "content change releases session");
    start();
    evaluate(engine, control, "cancel(false)"); // Same call as the non-loop poem button.
    sound.play();
    before = sound.starts();
    finish();
    check(!loop.active() && sound.starts() == before, "one-shot is never adopted");
    puts("PASS: page exit/content change/one-shot playback retain cancellation semantics");

    start();
    sound.takeOver();
    check(!loop.active(), "other audio sequence releases session");
    start();
    media->setProperty("active", true);
    check(!loop.active(), "plugin takeover releases session");
    media->setProperty("active", false);
    start();
    root->setProperty("failNext", true);
    finish();
    check(!loop.active() && !control->property("looping").toBool(), "failed replay releases previous session");
    root->setProperty("failNext", false);
    puts("PASS: audio takeover/plugin takeover/failed replay release session");

    start();
    const auto oldId = sound.currentPlayId();
    loop.claim(oldId + 1);
    loop.release(oldId);
    loop.beginScreenOff();
    check(!loop.shouldSuppressScreenOffStop(oldId), "mismatched native sequence is not suppressed");
    before = sound.stops;
    sound.stop();
    check(sound.stops == before + 1, "mismatched sequence stop reaches original player");
    loop.endScreenOff();
    check(loop.active(), "stale owner cannot release a newer session");
    loop.release(oldId + 1);
    evaluate(engine, control, "cancel(false)");
    start();
    sound.finish();
    QTest::qWait(100);
    check(loop.active(), "replay gap still owns a session before cancel");
    before = sound.starts();
    cancel();
    QTest::qWait(350);
    check(sound.starts() == before, "cancel during replay gap stops next iteration");
    start();
    loop.beginScreenOff();
    bool        suppressedOffThread = true;
    std::thread other([&] { suppressedOffThread = loop.shouldSuppressScreenOffStop(sound.currentPlayId()); });
    other.join();
    loop.endScreenOff();
    check(!suppressedOffThread, "screen-off exception is thread-local");
    cancel();
    loop.beginScreenOff();
    before = sound.stops;
    sound.stop();
    loop.endScreenOff();
    check(sound.stops == before + 1, "non-loop screen-off stop remains intact");
    puts("PASS: stale release/other thread/non-loop stop cannot inherit screen-off exception");

    for (int operation = 0; operation < 3; ++operation) {
        start();
        if (operation == 0) mod::HookRegistrar__ZN12YSoundCenter9forceStopEv::detour(&sound);
        if (operation == 1) mod::HookRegistrar__ZN12YSoundCenter7suspendEv::detour(&sound);
        if (operation == 2) mod::HookRegistrar__ZN12YSoundCenter16killSoundProcessEb::detour(&sound, false);
        QTest::qWait(20);
        check(!loop.active() && !control->property("looping").toBool(), "explicit interruption hook cancels loop");
    }
    start();
    root.reset();
    check(!loop.active(), "component destruction releases session");
    puts("PASS: forceStop/suspend/kill/destruction cancel loop");
    return 0;
}

#include "reading_loop_runtime_test.moc"
