// SPDX-License-Identifier: GPL-3.0-only
/*
 * Copyright (C) 2022-present, PenUniverse.
 * This file is part of the PenMods open source project.
 */

#include "system/input/HeadsetControls.h"

#include "base/YEnum.h"
#include "base/YPointer.h"

#include "common/Event.h"

#include "media/MediaSession.h"

#include "mod/Config.h"

#include <QDir>
#include <QSocketNotifier>
#include <QStringList>

#include <fcntl.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstring>

namespace mod {

namespace {

constexpr int kScanIntervalMs = 2000;
// 单击要等窗口结束才能确定不是双击/三击；380ms 是 Lead 指定的 350–400ms 区间。
constexpr int kClickWindowMs = 380;

template <size_t N>
bool testBit(const std::array<unsigned long, N>& bits, int code) {
    const size_t word = static_cast<size_t>(code) / (8 * sizeof(unsigned long));
    if (word >= N) {
        return false;
    }
    return (bits[word] >> (static_cast<size_t>(code) % (8 * sizeof(unsigned long)))) & 1UL;
}

// 数字耳机（USB 输入设备）才会带 KEY_PLAYPAUSE；模拟耳机只有 SW_HEADPHONE_INSERT。
bool isUsbBus(unsigned short bustype) { return bustype == BUS_USB; }

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
        mEnabled = cfg.value("enabled", true);
    } catch (const std::exception& e) {
        warn("耳机线控读取配置异常（{}），按默认启用处理", e.what());
        mEnabled = true;
    } catch (...) {
        warn("耳机线控读取配置未知异常，按默认启用处理");
        mEnabled = true;
    }
    info("耳机线控配置：enabled={}（默认启用；headset.enabled=false 可关闭）", mEnabled);
}

bool HeadsetControls::_probeDevice(const std::string& path, DeviceInfo* out) const {
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
    out->vendorVolumeKeys = keyOk && (testBit(keyBits, KEY_VOLUMEUP) || testBit(keyBits, KEY_VOLUMEDOWN));
    return true;
}

bool HeadsetControls::_isHeadphoneMediaDevice(const DeviceInfo& device) const {
    // 只认“USB 总线 + 含 KEY_PLAYPAUSE”的设备：不硬编码 event 编号，也不依赖名称
    // （event6 会被 penmods-injector 等虚拟设备复用）。
    return isUsbBus(device.bustype) && device.playPause;
}

void HeadsetControls::_scan(const char* reason) {
    if (!mEnabled) {
        return;
    }
    try {
        const QDir        dir("/dev/input");
        const QStringList entries = dir.entryList(QStringList() << "event*", QDir::System | QDir::Files, QDir::Name);
        std::vector<DeviceInfo> devices;
        for (const QString& entry : entries) {
            DeviceInfo info;
            if (_probeDevice(dir.absoluteFilePath(entry).toStdString(), &info)) {
                devices.push_back(info);
            }
        }

        std::string signature;
        for (const auto& d : devices) {
            signature += d.path + "|" + d.name + "|" + std::to_string(d.bustype) + "|" + std::to_string(d.vendor) + "|"
                       + std::to_string(d.product) + "|" + (d.playPause ? "P" : "-") + (d.headphoneSwitch ? "S" : "-")
                       + (d.vendorVolumeKeys ? "V" : "-") + ";";
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
                    "play_pause={} vendor_volume_keys={}",
                    d.path,
                    d.name,
                    d.phys,
                    d.bustype,
                    d.vendor,
                    d.product,
                    d.playPause,
                    d.vendorVolumeKeys
                );
            }
        }
        if (!found) {
            debug("耳机线控扫描（{}）：当前没有 USB+PLAYPAUSE 设备", reason);
        }
        _openDevices();
    } catch (const std::exception& e) {
        warn("耳机线控设备扫描异常（{}），稍后重试", e.what());
    } catch (...) {
        warn("耳机线控设备扫描未知异常，稍后重试");
    }
}

void HeadsetControls::_closeDevices() {
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
                QTimer::singleShot(0, this, [this]() { refresh("device-read-error"); });
                return;
            }
            if (n == 0) {
                return;
            }
            const size_t count = static_cast<size_t>(n) / sizeof(struct input_event);
            for (size_t i = 0; i < count; ++i) {
                const struct input_event& ev = events[i];
                if (ev.type == EV_KEY) {
                    if (ev.code == KEY_PLAYPAUSE) {
                        // 采样显示单击/双击/三击都只发 KEY_PLAYPAUSE，用释放沿计数。
                        if (ev.value == 0) {
                            _onPlayPauseClick();
                        }
                    } else if (ev.code == KEY_VOLUMEUP || ev.code == KEY_VOLUMEDOWN) {
                        // 厂商 /usr/bin/dig_headset_sevice 已在消费 114/115（dig_hp_volume），
                        // PenMods 再处理会一次跳两步；只记录。
                        debug("耳机线控：收到厂商已消费的音量键 code={} value={}（PenMods 不处理）", ev.code, ev.value);
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
        info("耳机线控：单击 → 播放/暂停（MediaSession 插件会话 {}）", session.ownerPluginId().toStdString());
        session.requestTogglePlayPause();
        return;
    }
    auto*      manager = YPointer<YMediaPlayerManager>::getInstance();
    const auto state   = PEN_CALL(PlayState, "_ZNK19YMediaPlayerManager9playStateEv", void*)(manager);
    if (state == PlayState::PLAYING) {
        info("耳机线控：单击 → 暂停（宿主播放器）");
        PEN_CALL(void, "_ZN19YMediaPlayerManager14onClickedPauseEv", void*)(manager);
    } else {
        info("耳机线控：单击 → 播放（宿主播放器）");
        PEN_CALL(void, "_ZN19YMediaPlayerManager13onClickedPlayEv", void*)(manager);
    }
}

void HeadsetControls::_dispatchNext() {
    auto& session = MediaSession::getInstance();
    if (session.active()) {
        info("耳机线控：双击 → 下一曲（MediaSession 插件会话 {}）", session.ownerPluginId().toStdString());
        session.requestNext();
        return;
    }
    info("耳机线控：双击 → 下一曲（宿主播放器）");
    PEN_CALL(uint64, "_ZN19YMediaPlayerManager13onClickedNextEb", void*, bool)
    (YPointer<YMediaPlayerManager>::getInstance(), true);
}

void HeadsetControls::_dispatchPrevious() {
    auto& session = MediaSession::getInstance();
    if (session.active()) {
        info("耳机线控：三击 → 上一曲（MediaSession 插件会话 {}）", session.ownerPluginId().toStdString());
        session.requestPrevious();
        return;
    }
    info("耳机线控：三击 → 上一曲（宿主播放器）");
    PEN_CALL(uint64, "_ZN19YMediaPlayerManager13onClickedPrevEb", void*, bool)
    (YPointer<YMediaPlayerManager>::getInstance(), true);
}

} // namespace mod
