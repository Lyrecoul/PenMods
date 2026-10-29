// SPDX-License-Identifier: GPL-3.0-only
#include "system/sound/VolumeGuard.h"

#include "base/Hook.h"
#include "base/YPointer.h"
#include "common/Event.h"
#include "common/Utils.h"
#include "mod/Config.h"
#include <QFile>
#include <QProcess>
#include <QQmlContext>
#include <QRegularExpression>
#include <QTimer>
#include <algorithm>
#include <chrono>

namespace mod {
namespace {
constexpr const char* kKeys[]       = {"speaker", "analog_headphone", "usb_headphone", "bluetooth"};
constexpr const char* kVolumeKeys[] = {"spk-volume", "hp-volume", "dig_hp_volume", "bt-volume"};
// Mirror playback_dev_ctrl's priority without its unconditional save_sys_cfg + sync
// on every `show`. Polling that script would rewrite flash once per second.
constexpr const char* kProbe =
    "if [ -d /sys/class/sound/card1 ]; then echo dig_hp; "
    "elif [ \"$(awk '/gpio-63 / {print $6}' /sys/kernel/debug/gpio 2>/dev/null)\" = lo ]; then echo hp; "
    "elif grep -q '^ble=client$' /data/cfg/sys_config.conf "
    "&& hcitool con 2>/dev/null | grep -q 'ACL'; then echo bt; "
    "elif [ -r /sys/kernel/debug/gpio ] && [ -r /data/cfg/sys_config.conf ]; then echo spk; fi";
qint64 monotonicMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// Read the same mixer controls the vendor writes. BlueALSA exposes raw 0..127
// values; USB and analog controls report their own min/max rather than a shared range.
QString stateProbe() {
    return QStringLiteral(
               "route=$(%1); echo \"$route\"; case \"$route\" in "
               "bt) amixer -Dbluealsa scontents;; "
               "hp) amixer cget name='MasterAHP Volume';; "
               "spk) amixer cget name='Master Playback Volume';; "
               "dig_hp) control=$(amixer -c1 controls | awk -F, '/Playback Volume/ {print $1; exit}'); "
               "[ -n \"$control\" ] && amixer -c1 cget \"$control\";; esac"
    )
        .arg(kProbe);
}

int mixerPercent(int output, const QString& text) {
    int minimum = 0;
    int maximum = 127;
    if (output == 3) {
        const QRegularExpression channel(QStringLiteral("(?:Front (?:Left|Right)|Mono): Playback (\\d+)"));
        auto                     matches = channel.globalMatch(text);
        int                      loudest = -1;
        while (matches.hasNext()) loudest = std::max(loudest, matches.next().captured(1).toInt());
        if (loudest < 0 || loudest > maximum) return -1;
        return (loudest * 100 + maximum / 2) / maximum;
    }
    const auto range    = QRegularExpression(QStringLiteral("min=(-?\\d+),max=(-?\\d+)")).match(text);
    const auto channels = QRegularExpression(QStringLiteral(": values=([0-9, -]+)")).match(text);
    if (!range.hasMatch() || !channels.hasMatch()) return -1;
    minimum = range.captured(1).toInt();
    maximum = range.captured(2).toInt();
    if (maximum <= minimum) return -1;
    int loudest = minimum;
    for (const auto& channel : channels.captured(1).split(',')) {
        bool      ok    = false;
        const int value = channel.trimmed().toInt(&ok);
        if (!ok || value < minimum || value > maximum) return -1;
        loudest = std::max(loudest, value);
    }
    return static_cast<int>((qint64(loudest - minimum) * 100 + (maximum - minimum) / 2) / (maximum - minimum));
}
} // namespace

VolumeGuard::VolumeGuard() {
    const auto cfg = Config::getInstance().read("volume_guard");
    if (cfg.is_object()) {
        if (cfg.contains("enabled") && cfg["enabled"].is_boolean()) mEnabled = cfg["enabled"].get<bool>();
        for (int i = 0; i < 4; ++i) {
            if (cfg.contains(kKeys[i]) && cfg[kKeys[i]].is_number_integer())
                mLimits[i] = std::clamp(cfg[kKeys[i]].get<int>(), 0, 100);
        }
    }
    connect(&Event::getInstance(), &Event::beforeUiInitialization, this, [this](QQuickView&, QQmlContext* context) {
        context->setContextProperty("volumeGuard", this);
    });
    connect(&Event::getInstance(), &Event::uiCompleted, this, [this]() {
        mReady = true;
        refresh();
    });
}

int VolumeGuard::parseOutput(const QString& output) {
    const auto name = output.trimmed();
    if (name == "spk") return 0;
    if (name == "hp") return 1;
    if (name == "dig_hp") return 2;
    if (name == "bt") return 3;
    return -1;
}

int VolumeGuard::limit(int output) const {
    if (output >= 0 && output < 4) return mLimits[output].load();
    // Unknown routes use the strictest configured limit until detection succeeds.
    int result = 100;
    for (const auto& value : mLimits) result = std::min(result, value.load());
    return result;
}
int VolumeGuard::currentLimit() const { return enabled() ? limit(currentOutput()) : 100; }
int VolumeGuard::clamp(int value, int output) const { return std::clamp(value, 0, enabled() ? limit(output) : 100); }
QString VolumeGuard::outputName() const {
    const QStringList names{"内置扬声器", "Type-C 模拟耳机", "USB / Type-C 数字耳机", "蓝牙音频"};
    const int         output = currentOutput();
    return output >= 0 && output < 4 ? names[output] : QStringLiteral("检测中 / 未知输出");
}
void VolumeGuard::save() {
    json cfg = {
        {"enabled", enabled()}
    };
    for (int i = 0; i < 4; ++i) cfg[kKeys[i]] = limit(i);
    Config::getInstance().write("volume_guard", cfg);
    emit settingsChanged();
    emit outputChanged();
    enforce();
}
void VolumeGuard::setEnabled(bool value) {
    if (mEnabled.exchange(value) != value) save();
}
void VolumeGuard::setLimit(int output, int value) {
    if (output < 0 || output >= 4) return;
    value = std::clamp(value, 0, 100);
    if (mLimits[output].exchange(value) != value) save();
}

void VolumeGuard::refresh() {
    // The vendor probe runs shell tools; never wait for it on the UI thread.
    const qint64 started = monotonicMs();
    auto*        process = new QProcess(this);
    auto*        timeout = new QTimer(process);
    timeout->setSingleShot(true);
    connect(timeout, &QTimer::timeout, process, &QProcess::kill);
    const auto finish = [this, process]() {
        process->deleteLater();
        QTimer::singleShot(1000, this, &VolumeGuard::refresh);
    };
    connect(
        process,
        QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
        this,
        [this, process, finish, started](int code, QProcess::ExitStatus status) {
            const auto state = QString::fromUtf8(process->readAllStandardOutput());
            const int  output =
                code == 0 && status == QProcess::NormalExit ? parseOutput(state.section('\n', 0, 0)) : -1;
            if (mOutput.exchange(output) != output) {
                mMeasured = -1;
                emit outputChanged();
                spdlog::info("VolumeGuard: output={}, limit={}", output, currentLimit());
            }
            // A request or worker write during this read invalidates the sample.
            // Allow queued hardware writes to settle before reconciling the UI.
            if (output >= 0 && started > mLastWrite.load() && monotonicMs() - mLastWrite.load() > 700) {
                synchronize(mixerPercent(output, state));
                enforce();
            }
            finish();
        }
    );
    connect(process, &QProcess::errorOccurred, this, [this, finish](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            mOutput = -1;
            emit outputChanged();
            finish();
        }
    });
    process->start("/bin/sh", {"-c", stateProbe()});
    timeout->start(2000);
}

void VolumeGuard::noteVolumeWrite() {
    mLastWrite = monotonicMs();
    mMeasured  = -1;
}

int VolumeGuard::measuredVolume() const {
    return monotonicMs() - mLastMeasurement.load() < 2500 ? mMeasured.load() : -1;
}

void VolumeGuard::synchronize(int measured) {
    if (measured < 0) {
        mMeasured = -1;
        return;
    }
    mMeasured        = measured;
    mLastMeasurement = monotonicMs();
    if (measured > currentLimit()) return; // enforce() must lower the mixer first.
    auto* setting = YPointer<YSettingManager>::getInstance();
    if (!setting) return;
    const int displayed = PEN_CALL(int, "_ZN15YSettingManager9spkVolumeEv", void*)(setting);
    // Refresh through the host's read/notify API: no setter, mixer write, or
    // flash write. The get_spk_volume hook supplies this measured value.
    if (displayed != measured) PEN_CALL(void, "_ZN15YSettingManager18updateVolumeAndLcdEv", void*)(setting);
}

void VolumeGuard::enforce() {
    const int output = currentOutput();
    if (!mReady || !enabled() || output < 0) return;
    int volume = measuredVolume();
    // Fall back to the vendor's saved value only when the mixer is unavailable.
    if (volume < 0) {
        QFile file("/data/cfg/sys_config.conf");
        if (!file.open(QIODevice::ReadOnly)) return;
        for (const auto& line : file.readAll().split('\n')) {
            const auto parts = line.trimmed().split('=');
            if (parts.size() != 2 || parts[0] != kVolumeKeys[output]) continue;
            bool      ok    = false;
            const int saved = parts[1].toInt(&ok);
            if (ok && saved >= 0 && saved <= 100) volume = saved;
            break;
        }
    }
    const int cap = currentLimit();
    if (volume <= cap) return;
    auto* setting = YPointer<YSettingManager>::getInstance();
    if (!setting) return;
    PEN_CALL(void, "_ZN15YSettingManager12setSpkVolumeEi", void*, int)(setting, cap);
    // The setting setter returns early if its cached value already equals cap.
    PEN_CALL(void, "_ZN14YDeviceManager12setSpkVolumeEi", void*, int)
    (PEN_CALL(void*, "_ZN10YSingletonI14YDeviceManagerE8instanceEv")(), cap);
    spdlog::info("VolumeGuard: reduced output={} volume={} to {}", output, volume, cap);
}
} // namespace mod

PEN_HOOK(void, _ZN15YSettingManager12setSpkVolumeEi, void* self, int value) {
    auto& guard = mod::VolumeGuard::getInstance();
    guard.noteVolumeWrite();
    origin(self, guard.clamp(value, guard.currentOutput()));
}

// YDeviceManager applies volume on a worker. Detect again at the final write so a
// queued request cannot carry a speaker limit into headphones after a hotplug.
PEN_HOOK(int, set_spk_volume, int value) {
    auto&     guard  = mod::VolumeGuard::getInstance();
    const int output = mod::VolumeGuard::parseOutput(QString::fromStdString(mod::exec(mod::kProbe)));
    guard.noteVolumeWrite();
    const int result = origin(guard.clamp(value, output));
    guard.noteVolumeWrite();
    return result;
}

PEN_HOOK(int, get_spk_volume, int* value) {
    const int measured = mod::VolumeGuard::getInstance().measuredVolume();
    if (value && measured >= 0) {
        *value = measured;
        return 0;
    }
    return origin(value);
}
