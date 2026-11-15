// SPDX-License-Identifier: GPL-3.0-only
// Exercise the real daemon installer and screen manager; only closed-source
// services, hook registration and shell process discovery are replaced.
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QThread>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <sys/stat.h>

static void* testSymbol(const char* name);
#define PEN_CALL(ret_t, sym, args_t...) reinterpret_cast<ret_t (*)(args_t)>(testSymbol(sym))
#define PEN_HOOK(ret_t, sym, args_t...)                                                                                \
    struct HookRegistrar_##sym {                                                                                       \
        static ret_t (*origin)(args_t);                                                                                \
        static ret_t detour(args_t);                                                                                   \
    };                                                                                                                 \
    ret_t (*HookRegistrar_##sym::origin)(args_t) = nullptr;                                                            \
    ret_t HookRegistrar_##sym::detour(args_t)

#include "system/input/InputDaemon.cpp"
#include "system/input/ScreenManager.cpp"

static QString model         = "Exam_V0";
static bool    daemonRunning = true;
static void    check(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
static void put(const QString& path, const QByteArray& data) {
    QFile file(path);
    check(file.open(QIODevice::WriteOnly) && file.write(data) == data.size(), "fixture write failed");
}
static QByteArray get(const QString& path) {
    QFile file(path);
    check(file.open(QIODevice::ReadOnly), "expected file missing");
    return file.readAll();
}
static void settle() {
    for (int i = 0; i < 50; ++i) {
        QCoreApplication::processEvents();
        QThread::msleep(5);
    }
}

namespace mod {
void Logger::_applyLevelFromConfig(const std::string&) {}
Event::Event() = default;
HeadsetControls::HeadsetControls() : Logger("HeadsetControls") {}
BatteryInfo::BatteryInfo() = default;
bool    BatteryInfo::isCharging() const { return false; }
bool    BatteryInfo::isOnExternalPower() const { return false; }
uint32  BatteryInfo::getAutoSuspendDuration() const { return 300; }
void    BatteryInfo::update() {}
QString BatteryInfo::getStatus() const { return {}; }
QString BatteryInfo::getVoltage() const { return {}; }
QString BatteryInfo::getTemperature() const { return {}; }
QString BatteryInfo::getHealth() const { return {}; }
QString BatteryInfo::getPrediction() const { return {}; }
QString BatteryInfo::getCurrent() const { return {}; }
QString BatteryInfo::getAutoSuspendDurationStr() const { return {}; }
void    BatteryInfo::setAutoSuspendDurationStr(const QString&) {}
AudioDaemon::AudioDaemon() : Logger("AudioDaemon") {}
ReadingLoop::ReadingLoop() = default;
bool   ReadingLoop::active() const { return false; }
double ReadingLoop::interruptionEpoch() const { return 0; }
void   ReadingLoop::claim(unsigned int) {}
void   ReadingLoop::release(unsigned int) {}
void   ReadingLoop::beginScreenOff() {}
void   ReadingLoop::endScreenOff() {}
Config::Config() : Logger("Config") {}
json Config::read(const std::string&) {
    return {
        {"sleep_duration",         180 },
        {"shutdown_duration",      0   },
        {"intel_sleep",            true},
        {"intel_sleep_audio_lock", true}
    };
}
bool        Config::write(const std::string&, json, bool) { return true; }
std::string readFile(const char* path) {
    QFile file(QString::fromUtf8(path));
    return file.open(QIODevice::ReadOnly) ? file.readAll().toStdString() : std::string{};
}
std::string exec(const char* command) {
    if (std::strcmp(command, "get_pcba_version") == 0) return model.toStdString();
    if (std::strcmp(command, "pidof input-event-daemon") == 0) return daemonRunning ? "1234" : "";
    if (std::strcmp(command, "killall input-event-daemon") == 0) daemonRunning = false;
    else if (std::strcmp(command, "input-event-daemon") == 0) daemonRunning = true;
    else throw std::runtime_error(std::string("unexpected shell command: ") + command);
    return {};
}
} // namespace mod
static void* testSymbol(const char* name) {
    if (!std::strcmp(name, "start_auto_screen_off") || !std::strcmp(name, "stop_auto_screen_off"))
        return reinterpret_cast<void*>(+[]() -> void* { return nullptr; });
    throw std::runtime_error(std::string("unexpected vendor call: ") + name);
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (argc > 1) model = QString::fromLocal8Bit(argv[1]);
    int  failures = 0;
    auto run      = [&](const char* name, auto test) {
        try {
            test();
            std::printf("PASS: %s\n", name);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "FAIL: %s: %s\n", name, e.what());
            ++failures;
        }
    };
    auto&         daemon = mod::InputDaemon::getInstance();
    auto&         screen = mod::ScreenManager::getInstance();
    const QString source = model == "Exam_V0" ? "/etc/input-event-daemon_Exam.conf" : "/etc/input-event-daemon_V4.conf";
    const QString effective  = "/etc/input-event-daemon.conf";
    const QByteArray factory = "factory configuration\n";
    put(source, factory);
    check(::link(qPrintable(source), qPrintable(effective)) == 0, "shared inode fixture failed");
    run("configuration install preserves the shared-inode baseline exactly once", [&] {
        daemon.reset();
        check(get(source + ".factory-bak") == factory, "baseline was overwritten without backup");
        daemon.setScreenOff(120);
        check(get(source + ".factory-bak") == factory, "later settings replaced baseline backup");
        check(get(source) == get(effective), "effective config diverged");
    });
    run("physical input routing excludes fake-keys across event renumbering", [&] {
        const auto config = get(effective);
        QDir().mkpath("/tmp/input/by-path");
        for (int fake : {4, 5}) {
            const int adc   = fake == 4 ? 2 : 3;
            const int led   = fake == 4 ? 3 : 4;
            const int touch = fake == 4 ? 0 : 2;
            for (int event = 0; event <= 5; ++event) put(QString("/tmp/input/event%1").arg(event), {});
            const auto link = [](const char* name, int event) {
                const QString alias = QString("/tmp/input/by-path/") + name;
                QFile::remove(alias);
                check(QFile::link(QString("../event%1").arg(event), alias), "input alias creation failed");
            };
            link("platform-adc-keys-event", adc);
            link("platform-led_control-event", led);
            link("platform-ff190000.i2c-event", touch);
            QStringList listened;
            for (const auto& line : config.split('\n')) {
                if (!line.startsWith("listen = ")) continue;
                const QString path = QString::fromUtf8(line.mid(9)).replace("/dev/input/", "/tmp/input/");
                listened.append(QFileInfo(path).canonicalFilePath());
            }
            check(
                !listened.contains(QString("/tmp/input/event%1").arg(fake)),
                "screen_off press is routed back to idle reset"
            );
            check(
                listened.contains(QString("/tmp/input/event%1").arg(adc))
                    && listened.contains(QString("/tmp/input/event%1").arg(touch)),
                "physical touch or key activity no longer wakes the daemon"
            );
        }
    });
    run("failed baseline backup leaves both live configurations unchanged", [&] {
        const auto before = get(source);
        QFile::remove(source + ".factory-bak");
        check(QFile::link("/nonexistent/baseline", source + ".factory-bak"), "backup failure fixture failed");
        daemon.setScreenOff(240);
        check(get(source) == before && get(effective) == before, "backup failure destroyed live config");
    });
    run("physical wake clears the vendor screen-off latch asynchronously", [&] {
        QDir().mkpath("/tmp/bin");
        put("/tmp/bin/screen_onoff",
            "#!/bin/sh\n[ \"$1\" = on ] || exit 1\n[ -f /tmp/screen_status ] || exit 0\n"
            "echo release >> /tmp/releases\nrm /tmp/screen_status\n");
        QFile::setPermissions("/tmp/bin/screen_onoff", QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        qputenv("PATH", "/tmp/bin:/usr/bin:/bin");
        put("/tmp/screen_status", "off\n");
        screen.onScreenOff();
        settle();
        check(QFile::exists("/tmp/screen_status") && !QFile::exists("/tmp/releases"), "screen-off woke itself");
        screen.onScreenOn();
        settle();
        check(!QFile::exists("/tmp/screen_status"), "power-key wake left vendor power saving armed");
        check(get("/tmp/releases") == "release\n", "missing screen_off release");
        screen.onScreenOn();
        settle();
        check(get("/tmp/releases") == "release\n", "duplicate on notification injected another release");
    });
    run("normal vendor wake needs no additional release", [&] {
        QFile::remove("/tmp/releases");
        screen.onScreenOff();
        screen.onScreenOn();
        settle();
        check(!QFile::exists("/tmp/releases"), "already completed wake emitted another release");
    });
    run("queued wake cannot turn a newly dark screen back on", [&] {
        QFile::remove("/tmp/releases");
        screen.onScreenOff();
        screen.onScreenOn();
        screen.onScreenOff();
        put("/tmp/screen_status", "off\n");
        settle();
        check(QFile::exists("/tmp/screen_status") && !QFile::exists("/tmp/releases"), "stale wake re-lit the screen");
    });
    return failures ? 1 : 0;
}
