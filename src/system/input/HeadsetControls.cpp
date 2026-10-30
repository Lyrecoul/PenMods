// SPDX-License-Identifier: GPL-3.0-only
/*
 * Copyright (C) 2022-present, PenUniverse.
 * This file is part of the PenMods open source project.
 */

#include "system/input/HeadsetControls.h"

#include "base/YEnum.h"
#include "base/YPointer.h"

#include "common/Event.h"
#include "common/Utils.h"

#include "media/MediaSession.h"

#include "mod/Config.h"

#include <QDir>
#include <QMetaObject>
#include <QPointer>
#include <QRunnable>
#include <QSocketNotifier>
#include <QStringList>
#include <QThreadPool>

#include <fcntl.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstring>
#include <functional>

namespace mod {

namespace {

// 不用 QRunnable::create(std::function)：系统 Qt 使用 libstdc++，mod 使用 libc++，
// 该导出函数的 std::function 参数 ABI 不兼容。回调仅在 mod 内部构造和执行。
class ScanTask final : public QRunnable {
public:
    explicit ScanTask(std::function<void()> callback) : mCallback(std::move(callback)) {}

    void run() override { mCallback(); }

private:
    std::function<void()> mCallback;
};

constexpr int kScanIntervalMs = 2000;
// 单击要等窗口结束才能确定不是双击/三击；380ms 是 Lead 指定的 350–400ms 区间。
constexpr int kClickWindowMs = 380;
// 每次按键的 0-100 音量步长（厂商数字耳机脚本内部用 20，偏粗；这里取 5 便于细调）。
constexpr int kVolumeStep = 5;

template <size_t N>
bool testBit(const std::array<unsigned long, N>& bits, int code) {
    const size_t word = static_cast<size_t>(code) / (8 * sizeof(unsigned long));
    if (word >= N) {
        return false;
    }
    return (bits[word] >> (static_cast<size_t>(code) % (8 * sizeof(unsigned long)))) & 1UL;
}


} // namespace

struct HeadsetControls::OpenDevice {
    DeviceInfo       info;
    int              fd       = -1;
    QSocketNotifier* notifier = nullptr;
};

HeadsetControls::HeadsetControls() : Logger("HeadsetControls") {
    mClickTimer.setSingleShot(true);
    connect(&mClickTimer, &QTimer::timeout, this, &HeadsetControls::_flushClickWindow);
    connect(&Event::getInstance(), &Event::uiCompleted, this, &HeadsetControls::onUiCompleted);
}

void HeadsetControls::onUiCompleted() {
    // uiCompleted 是 Qt 槽：任何异常都必须就地吞掉，否则会 std::terminate(SIGABRT)。
    try {
        _loadConfig();
        if (!mEnabled) {
            info("耳机线控已关闭（headset.enabled=false），不监听任何输入设备");
            return;
        }
        if (!mTimerStarted) {
            mTimerStarted = true;
            mScanTimer.setInterval(kScanIntervalMs);
            mScanTimer.setSingleShot(false);
            connect(&mScanTimer, &QTimer::timeout, this, [this]() { _scan("poll"); });
            mScanTimer.start();
        }
        _scan("ui-completed");
    } catch (const std::exception& e) {
        error("耳机线控初始化异常，已按关闭处理（{}）", e.what());
        mEnabled = false;
        mScanTimer.stop();
        mTimerStarted = false;
    } catch (...) {
        error("耳机线控初始化未知异常，已按关闭处理");
        mEnabled = false;
        mScanTimer.stop();
        mTimerStarted = false;
    }
}

void HeadsetControls::refresh(const char* reason) { _scan(reason); }

void HeadsetControls::_loadConfig() {
    // Config::read() 对不存在的节返回 `{}`（nlohmann null，不是空 object！），
    // 对 null 调 value()/at()/[] 会抛 type_error；默认启用（真机已确认有 PLAYPAUSE 线控），
    // 但必须可配置关闭。
    json cfg = json::object();
    try {
        const json section = Config::getInstance().read("headset");
        if (section.is_object()) {
            cfg = section;
        }
        mEnabled      = cfg.value("enabled", true);
        mHandleVolume = cfg.value("handle_volume", true);
    } catch (const std::exception& e) {
        warn("耳机线控读取配置异常（{}），按默认启用处理", e.what());
        mEnabled      = true;
        mHandleVolume = true;
    } catch (...) {
        warn("耳机线控读取配置未知异常，按默认启用处理");
        mEnabled      = true;
        mHandleVolume = true;
    }
    info(
        "耳机线控配置：enabled={} handle_volume={}（默认启用；headset.enabled=false / handle_volume=false 可关闭）",
        mEnabled,
        mHandleVolume
    );
}

bool HeadsetControls::_probeDevice(const std::string& path, DeviceInfo* out) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }
    char name[256] = {0};
    if (::ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name) < 0) {
        ::close(fd);
        return false;
    }
    char phys[256] = {0};
    if (::ioctl(fd, EVIOCGPHYS(sizeof(phys) - 1), phys) < 0) {
        phys[0] = '\0';
    }
    struct input_id id{};
    if (::ioctl(fd, EVIOCGID, &id) < 0) {
        ::close(fd);
        return false;
    }

    std::array<unsigned long, (EV_CNT + 8 * sizeof(unsigned long) - 1) / (8 * sizeof(unsigned long))>  evBits{};
    std::array<unsigned long, (KEY_CNT + 8 * sizeof(unsigned long) - 1) / (8 * sizeof(unsigned long))> keyBits{};
    std::array<unsigned long, (SW_CNT + 8 * sizeof(unsigned long) - 1) / (8 * sizeof(unsigned long))>  swBits{};
    const bool evOk  = ::ioctl(fd, EVIOCGBIT(0, sizeof(evBits)), evBits.data()) >= 0;
    const bool keyOk = ::ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keyBits)), keyBits.data()) >= 0;
    const bool swOk  = ::ioctl(fd, EVIOCGBIT(EV_SW, sizeof(swBits)), swBits.data()) >= 0;
    ::close(fd);

    if (!evOk) {
        return false;
    }

    out->path             = path;
    out->name             = name;
    out->phys             = phys;
    out->bustype          = id.bustype;
    out->vendor           = id.vendor;
    out->product          = id.product;
    out->headphoneSwitch  = swOk && testBit(swBits, SW_HEADPHONE_INSERT);
    out->playPause        = keyOk && testBit(keyBits, KEY_PLAYPAUSE);
    out->play             = keyOk && (testBit(keyBits, KEY_PLAY) || testBit(keyBits, KEY_PLAYCD));
    out->pause            = keyOk && (testBit(keyBits, KEY_PAUSE) || testBit(keyBits, KEY_PAUSECD));
    out->next             = keyOk && testBit(keyBits, KEY_NEXTSONG);
    out->previous         = keyOk && testBit(keyBits, KEY_PREVIOUSSONG);
    out->vendorVolumeKeys = keyOk && (testBit(keyBits, KEY_VOLUMEUP) || testBit(keyBits, KEY_VOLUMEDOWN));
    return true;
}

bool HeadsetControls::_isHeadphoneMediaDevice(const DeviceInfo& device) const {
    // USB 保持原有门槛；蓝牙也接受只暴露独立播放/暂停或切歌键的设备。
    return (device.bustype == BUS_USB && device.playPause)
        || (device.bustype == BUS_BLUETOOTH
            && (device.playPause || device.play || device.pause || device.next || device.previous));
}

void HeadsetControls::_scan(const char* reason) {
    if (!mEnabled || mScanInProgress) {
        return;
    }
    mScanInProgress                      = true;
    const std::string         scanReason = reason ? reason : "unknown";
    QPointer<HeadsetControls> self(this);
    QThreadPool::globalInstance()->start(new ScanTask([self, scanReason]() {
        std::vector<DeviceInfo> devices;
        try {
            devices = _enumerateDevices();
        } catch (...) {
            if (self) {
                QMetaObject::invokeMethod(
                    self,
                    [self]() {
                        if (self) {
                            self->mScanInProgress = false;
                        }
                    },
                    Qt::QueuedConnection
                );
            }
            return;
        }
        if (!self) {
            return;
        }
        QMetaObject::invokeMethod(
            self,
            [self, devices = std::move(devices), scanReason]() mutable {
                if (!self) {
                    return;
                }
                self->mScanInProgress = false;
                self->_applyScan(std::move(devices), scanReason);
            },
            Qt::QueuedConnection
        );
    }));
}

std::vector<HeadsetControls::DeviceInfo> HeadsetControls::_enumerateDevices() {
    const QDir              dir("/dev/input");
    const QStringList       entries = dir.entryList(QStringList() << "event*", QDir::System | QDir::Files, QDir::Name);
    std::vector<DeviceInfo> devices;
    for (const QString& entry : entries) {
        DeviceInfo info;
        if (_probeDevice(dir.absoluteFilePath(entry).toStdString(), &info)) {
            devices.push_back(std::move(info));
        }
    }
    return devices;
}

void HeadsetControls::_applyScan(std::vector<DeviceInfo> devices, const std::string& reason) {
    try {
        std::string signature;
        for (const auto& d : devices) {
            signature += d.path + "|" + d.name + "|" + std::to_string(d.bustype) + "|" + std::to_string(d.vendor) + "|"
                       + std::to_string(d.product) + "|" + (d.playPause ? "P" : "-") + (d.play ? "p" : "-")
                       + (d.pause ? "A" : "-") + (d.next ? "N" : "-") + (d.previous ? "B" : "-")
                       + (d.headphoneSwitch ? "S" : "-") + (d.vendorVolumeKeys ? "V" : "-") + ";";
        }
        if (signature == mLastSignature) {
            mDevices = std::move(devices);
            return;
        }
        mLastSignature = signature;
        mDevices       = std::move(devices);

        bool found = false;
        for (const auto& d : mDevices) {
            if (_isHeadphoneMediaDevice(d)) {
                found = true;
                info(
                    "耳机线控设备：{} name=\"{}\" phys=\"{}\" bus=0x{:04x} vendor=0x{:04x} product=0x{:04x} "
                    "play_pause={} play={} pause={} next={} previous={} vendor_volume_keys={}",
                    d.path,
                    d.name,
                    d.phys,
                    d.bustype,
                    d.vendor,
                    d.product,
                    d.playPause,
                    d.play,
                    d.pause,
                    d.next,
                    d.previous,
                    d.vendorVolumeKeys
                );
            }
        }
        if (!found) {
            debug("耳机线控扫描（{}）：当前没有 USB/蓝牙媒体键设备", reason);
        }
        _openDevices();
    } catch (const std::exception& e) {
        warn("耳机线控设备扫描异常（{}），稍后重试", e.what());
    } catch (...) {
        warn("耳机线控设备扫描未知异常，稍后重试");
    }
}

void HeadsetControls::_closeDevices() {
    // 不让已拔出的 USB 耳机留下待执行的连击。
    mClickTimer.stop();
    mClickCount = 0;
    for (auto* device : mOpenDevices) {
        if (device->notifier != nullptr) {
            device->notifier->setEnabled(false);
            delete device->notifier;
            device->notifier = nullptr;
        }
        if (device->fd >= 0) {
            ::close(device->fd);
            device->fd = -1;
        }
        delete device;
    }
    mOpenDevices.clear();
}

void HeadsetControls::_openDevices() {
    _closeDevices();
    for (const auto& d : mDevices) {
        if (!_isHeadphoneMediaDevice(d)) {
            continue;
        }
        const int fd = ::open(d.path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) {
            warn("耳机线控：无法打开 {}（{}），跳过", d.path, std::strerror(errno));
            continue;
        }
        auto* device     = new OpenDevice;
        device->info     = d;
        device->fd       = fd;
        device->notifier = new QSocketNotifier(fd, QSocketNotifier::Read, this);
        connect(device->notifier, &QSocketNotifier::activated, this, [this, device]() { _onReadable(device); });
        mOpenDevices.push_back(device);
        info("耳机线控：开始监听 {}（{}）", d.path, d.name);
    }
    if (mOpenDevices.empty()) {
        mClickCount = 0;
        mClickTimer.stop();
        debug("耳机线控：没有可监听的耳机媒体设备");
    }
}

void HeadsetControls::_onReadable(OpenDevice* device) {
    try {
        struct input_event events[64];
        for (;;) {
            const ssize_t n = ::read(device->fd, events, sizeof(events));
            if (n < 0) {
                if (errno == EAGAIN || errno == EINTR) {
                    return;
                }
                warn("耳机线控：读取 {} 失败（{}），关闭监听并在稍后重扫", device->info.path, std::strerror(errno));
                device->notifier->setEnabled(false);
                mLastSignature.clear();
                QTimer::singleShot(0, this, [this]() { refresh("device-read-error"); });
                return;
            }
            if (n == 0) {
                device->notifier->setEnabled(false);
                mLastSignature.clear();
                QTimer::singleShot(0, this, [this]() { refresh("device-eof"); });
                return;
            }
            const size_t count = static_cast<size_t>(n) / sizeof(struct input_event);
            for (size_t i = 0; i < count; ++i) {
                const struct input_event& ev = events[i];
                if (ev.type == EV_KEY) {
                    debug("耳机输入：device={} code={} value={}", device->info.path, ev.code, ev.value);
                    if (ev.code == KEY_PLAYPAUSE) {
                        // 蓝牙手势由耳机解析，不能把两条播放暂停命令合并成“下一曲”。
                        // USB 保持原有释放沿连击语义。
                        if (ev.value == 0) {
                            if (device->info.bustype == BUS_BLUETOOTH) {
                                _dispatchTogglePlayPause();
                            } else {
                                _onPlayPauseClick();
                            }
                        }
                    } else if (
                        ev.code == KEY_PLAY || ev.code == KEY_PLAYCD || ev.code == KEY_PAUSE || ev.code == KEY_PAUSECD
                    ) {
                        if (ev.value == 1) {
                            (ev.code == KEY_PLAY || ev.code == KEY_PLAYCD) ? _dispatchPlay() : _dispatchPause();
                        }
                    } else if (ev.code == KEY_NEXTSONG || ev.code == KEY_PREVIOUSSONG) {
                        if (ev.value == 1) {
                            ev.code == KEY_NEXTSONG ? _dispatchNext() : _dispatchPrevious();
                        }
                    } else if (ev.code == KEY_VOLUMEUP || ev.code == KEY_VOLUMEDOWN) {
                        // 厂商 dig_headset_sevice 的 evtest 输出是块缓冲、短按不生效（见头文件注释），
                        // 由 PenMods 接管；按“按下沿”动作一次（value=2 的自动重复忽略）。
                        if (mHandleVolume && ev.value == 1) {
                            _onVolumeKey(ev.code == KEY_VOLUMEUP);
                        } else {
                            debug(
                                "耳机线控：音量键 code={} value={}（handle_volume={}）",
                                ev.code,
                                ev.value,
                                mHandleVolume
                            );
                        }
                    } else {
                        // 未知键（例如 KEY_VOICECOMMAND=582）只记录，不动作。
                        debug("耳机线控：未知按键 code={} value={}，忽略", ev.code, ev.value);
                    }
                } else if (ev.type == EV_SW && ev.code == SW_HEADPHONE_INSERT) {
                    info("耳机线控：耳机{}，稍后重扫输入设备", ev.value ? "已插入" : "已拔出");
                    QTimer::singleShot(300, this, [this]() { refresh("headphone-switch"); });
                }
            }
        }
    } catch (const std::exception& e) {
        error("耳机线控读取异常（{}），关闭监听", e.what());
        QTimer::singleShot(0, this, [this]() { refresh("device-read-exception"); });
    } catch (...) {
        error("耳机线控读取未知异常，关闭监听");
        QTimer::singleShot(0, this, [this]() { refresh("device-read-exception"); });
    }
}

void HeadsetControls::_onPlayPauseClick() {
    mClickCount += 1;
    if (mClickCount > 3) {
        mClickCount = 3;
    }
    mClickTimer.start(kClickWindowMs);
    debug("耳机线控：PLAYPAUSE 释放，窗口内累计 {} 击", mClickCount);
}

void HeadsetControls::_onVolumeKey(bool up) {
    auto* settingManager = YPointer<YSettingManager>::getInstance();
    if (settingManager == nullptr) {
        warn("耳机线控：YSettingManager 尚未就绪，忽略音量键");
        return;
    }
    if (!mVolumeLogOnce) {
        mVolumeLogOnce = true;
        // 记录厂商脚本是否在跑，供真机日志判断“是否双调”：厂商脚本的 evtest 输出块缓冲，
        // 正常短按不生效；若未来厂商修复，可把 headset.handle_volume 置 false。
        // 注意：厂商脚本以 `sh /usr/bin/dig_headset_sevice` 运行，`pidof dig_headset_sevice`
        // 匹配不到脚本名，必须用 ps 看完整命令行。
        const std::string vendor = exec("ps w | grep -F 'dig_headset_sevice' | grep -v grep | head -n 1");
        info(
            "耳机线控：接管音量键（step={}）；厂商 dig_headset_sevice={}",
            kVolumeStep,
            vendor.empty() ? std::string("not running") : vendor
        );
    }
    const int current = PEN_CALL(int, "_ZN15YSettingManager9spkVolumeEv", void*)(settingManager);
    int       next    = current + (up ? kVolumeStep : -kVolumeStep);
    if (next < 0) {
        next = 0;
    } else if (next > 100) {
        next = 100;
    }
    info(
        "耳机线控：音量{} {} → {}（YSettingManager::setSpkVolume，按当前输出路径选卡）",
        up ? "+" : "-",
        current,
        next
    );
    PEN_CALL(void, "_ZN15YSettingManager12setSpkVolumeEi", void*, int)(settingManager, next);
}

void HeadsetControls::_flushClickWindow() {
    const int clicks = mClickCount;
    mClickCount      = 0;
    if (clicks <= 0) {
        return;
    }
    try {
        if (clicks == 1) {
            _dispatchTogglePlayPause();
        } else if (clicks == 2) {
            _dispatchNext();
        } else {
            _dispatchPrevious();
        }
    } catch (const std::exception& e) {
        error("耳机线控：{} 击动作异常（{}）", clicks, e.what());
    } catch (...) {
        error("耳机线控：{} 击动作未知异常", clicks);
    }
}

void HeadsetControls::_dispatchTogglePlayPause() {
    auto& session = MediaSession::getInstance();
    if (session.active()) {
        info("耳机线控：播放/暂停（MediaSession 插件会话 {}）", session.ownerPluginId().toStdString());
        session.requestTogglePlayPause();
        return;
    }
    auto*      manager = YPointer<YMediaPlayerManager>::getInstance();
    const auto state   = PEN_CALL(PlayState, "_ZNK19YMediaPlayerManager9playStateEv", void*)(manager);
    if (state == PlayState::PLAYING) {
        info("耳机线控：暂停（宿主播放器）");
        PEN_CALL(void, "_ZN19YMediaPlayerManager14onClickedPauseEv", void*)(manager);
    } else {
        info("耳机线控：播放（宿主播放器）");
        PEN_CALL(void, "_ZN19YMediaPlayerManager13onClickedPlayEv", void*)(manager);
    }
}

void HeadsetControls::_dispatchPlay() {
    auto& session = MediaSession::getInstance();
    if (session.active()) {
        info("耳机线控：播放（MediaSession 插件会话 {}）", session.ownerPluginId().toStdString());
        session.requestPlay();
        return;
    }
    info("耳机线控：播放（宿主播放器）");
    PEN_CALL(void, "_ZN19YMediaPlayerManager13onClickedPlayEv", void*)(YPointer<YMediaPlayerManager>::getInstance());
}

void HeadsetControls::_dispatchPause() {
    auto& session = MediaSession::getInstance();
    if (session.active()) {
        info("耳机线控：暂停（MediaSession 插件会话 {}）", session.ownerPluginId().toStdString());
        session.requestPause();
        return;
    }
    info("耳机线控：暂停（宿主播放器）");
    PEN_CALL(void, "_ZN19YMediaPlayerManager14onClickedPauseEv", void*)(YPointer<YMediaPlayerManager>::getInstance());
}

void HeadsetControls::_dispatchNext() {
    auto& session = MediaSession::getInstance();
    if (session.active()) {
        info("耳机线控：下一曲（MediaSession 插件会话 {}）", session.ownerPluginId().toStdString());
        session.requestNext();
        return;
    }
    info("耳机线控：下一曲（宿主播放器）");
    PEN_CALL(uint64, "_ZN19YMediaPlayerManager13onClickedNextEb", void*, bool)
    (YPointer<YMediaPlayerManager>::getInstance(), true);
}

void HeadsetControls::_dispatchPrevious() {
    auto& session = MediaSession::getInstance();
    if (session.active()) {
        info("耳机线控：上一曲（MediaSession 插件会话 {}）", session.ownerPluginId().toStdString());
        session.requestPrevious();
        return;
    }
    info("耳机线控：上一曲（宿主播放器）");
    PEN_CALL(uint64, "_ZN19YMediaPlayerManager13onClickedPrevEb", void*, bool)
    (YPointer<YMediaPlayerManager>::getInstance(), true);
}

} // namespace mod
