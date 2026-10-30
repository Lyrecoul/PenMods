// SPDX-License-Identifier: GPL-3.0-only
/*
 * Copyright (C) 2022-present, PenUniverse.
 * This file is part of the PenMods open source project.
 */

#pragma once

#include "common/service/Logger.h"

#include <QTimer>

#include <vector>

namespace mod {

/**
 * @brief 耳机线控支持（Issue #21 item 5 / task-9）。
 *
 * 真机采样（build/diag/headset/probe-run-<timestamp>）：插 Type-C 耳机后出现一个 USB 输入设备
 *   bus=0x0003(USB) vendor=0x3302 product=0x12cb name="TTGK Technology T02"
 *   KEY: KEY_VOLUMEDOWN(114) / KEY_VOLUMEUP(115) / KEY_PLAYPAUSE(164) / KEY_VOICECOMMAND(582)
 * 用户单击/双击/三击都只发 KEY_PLAYPAUSE，因此按“释放沿 + 380ms 窗口”区分单/双/三击：
 *   单击 → 播放/暂停；双击 → 下一曲；三击 → 上一曲。
 * 该设备没有 NEXT/PREV 键；模拟耳机只有 SW_HEADPHONE_INSERT，没有按键源。
 *
 * 设备识别：**不硬编码 event 编号、不依赖名称** —— 用 EVIOCGBIT 判断
 * “含媒体键”且 “bus == BUS_USB 或 BUS_BLUETOOTH”。实测 `/dev/input/event6` 还会被其它虚拟
 * 设备复用（penmods-injector），所以只认 capabilities + bus。
 * USB 仍要求 KEY_PLAYPAUSE；蓝牙接受 KEY_PLAYPAUSE、KEY_PLAY/PLAYCD、KEY_PAUSE/PAUSECD、
 * KEY_NEXTSONG、KEY_PREVIOUSSONG。蓝牙耳机自行解析手势，每条命令立即执行，不套 USB 连击窗口。
 * YDP02X + REDMI Buds 8 Pro 已确认创建 BUS_BLUETOOTH 输入设备，提供独立播放/暂停和切歌键。
 * 这覆盖固件映射到 Linux input 的蓝牙控制；不提供 LE Audio 或另行注册 D-Bus AVRCP 播放器。
 *
 * 音量键 114/115：厂商 /usr/bin/dig_headset_sevice 虽然会在插入时启动（probe 快照证实），
 * 但它用 `evtest > FIFO` 且没有 stdbuf，stdout 是块缓冲 —— 父 shell 的 `while read` 在
 * evtest 退出/缓冲满之前收不到任何行，所以短按几次音量在实测中不生效（dig_hp_volume 保持
 * 32 不变）。因此 PenMods 接管 114/115，调 YSettingManager::spkVolume()/setSpkVolume()：
 * 其内部 set_spk_volume 会按 `playback_dev_ctrl show` 的当前输出路径（spk/hp/dig_hp/bt）
 * 选正确的卡/控件，数字耳机（card1）也被覆盖。可用 `headset.handle_volume=false` 关闭；
 * 若未来厂商修复脚本，可再切回由厂商处理。
 *
 * 设备热插拔用 2s 轮询（不监听 event0 也能发现新设备/拔出），拔出即关闭 notifier。
 * 默认启用，可通过配置 `headset.enabled=false` 关闭。
 * 配置缺失时保留 is_object() 兜底，避免对 null 调 value() 导致启动 SIGABRT。
 *
 * 所有入口（uiCompleted 槽、QSocketNotifier、QTimer）都做异常兜底，异常不得逃出 Qt 事件派发。
 */
class HeadsetControls : public QObject, public Singleton<HeadsetControls>, private Logger {
    Q_OBJECT

public:
    struct DeviceInfo {
        std::string    path;
        std::string    name;
        std::string    phys;
        unsigned short bustype          = 0;
        unsigned short vendor           = 0;
        unsigned short product          = 0;
        bool           headphoneSwitch  = false; ///< EV_SW 含 SW_HEADPHONE_INSERT
        bool           playPause        = false; ///< EV_KEY 含 KEY_PLAYPAUSE
        bool           vendorVolumeKeys = false; ///< KEY_VOLUMEUP/KEY_VOLUMEDOWN
        bool           play             = false;
        bool           pause            = false;
        bool           next             = false;
        bool           previous         = false;
    };

    void onUiCompleted();

    bool enabled() const { return mEnabled; }

    /// 重新扫描 /dev/input（插拔、配置变化、诊断时调用）。
    void refresh(const char* reason);

    const std::vector<DeviceInfo>& devices() const { return mDevices; }

private:
    friend Singleton<HeadsetControls>;
    explicit HeadsetControls();

    struct OpenDevice;

    void _loadConfig();

    void _scan(const char* reason);

    void _applyScan(std::vector<DeviceInfo> devices, const std::string& reason);

    static std::vector<DeviceInfo> _enumerateDevices();

    static bool _probeDevice(const std::string& path, DeviceInfo* out);

    /// USB/蓝牙媒体键设备（不看名称/event 号）。
    bool _isHeadphoneMediaDevice(const DeviceInfo& device) const;

    void _openDevices();

    void _closeDevices();

    void _onReadable(OpenDevice* device);

    // 释放沿计数 → 窗口结束后按 1/2/3 击派发。
    void _onPlayPauseClick();

    // 音量键：按下沿立即执行（不做双击语义），步长 kVolumeStep。
    void _onVolumeKey(bool up);

    void _flushClickWindow();

    void _dispatchTogglePlayPause();

    void _dispatchPlay();

    void _dispatchPause();

    void _dispatchNext();

    void _dispatchPrevious();

    bool mEnabled        = false;
    bool mHandleVolume   = true;
    bool mTimerStarted   = false;
    bool mScanInProgress = false;
    bool mVolumeLogOnce  = false;

    QTimer mScanTimer;
    QTimer mClickTimer;
    int    mClickCount = 0;

    std::vector<DeviceInfo>  mDevices;
    std::string              mLastSignature;
    std::vector<OpenDevice*> mOpenDevices;
};

} // namespace mod
