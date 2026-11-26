// SPDX-License-Identifier: GPL-3.0-only
/*
 * Copyright (C) 2022-present, PenUniverse.
 * This file is part of the PenMods open source project.
 */

#pragma once

#include <QAudioFormat>
#include <QByteArray>
#include <QObject>
#include <QString>

class QAudioInput;
class QIODevice;

namespace mod {

/// 16 kHz / 16-bit PCM 采集，供 DeepSeek 语音识别接管使用。
///
/// 尽量少做事：打开麦克风、把 PCM 攒进内存、停止时把"左声道单声道 16-bit"交出去。
/// 麦克风竞争的处理完全照抄 `src/recorder/AudioRecorder.cpp`（厂商唯一已知可行的路径）：
/// 采集前 `YSoundCenter::forceStop()` + `amixer cset numid=2 1` + AudioDaemon 引用，
/// 结束后 `amixer cset numid=2 0` + 释放引用。不这么做时设备上录到的是静音。
class AsrCapture : public QObject {
    Q_OBJECT

public:
    explicit AsrCapture(QObject* parent = nullptr);
    ~AsrCapture() override;

    /// 开始采集。失败时返回 false 并通过 reason 给出一句可以直接弹给用户的原因。
    bool start(QString& reason);

    /// 预热设备/格式探测（麦克风枚举 + ALSA 格式协商，几百毫秒量级）。
    /// 开启接管且 UI 已就绪时调一次，让第一次按语音键也不用等这一步。
    static void warmUp();

    /// 停止采集并返回单声道 16-bit PCM（长度对齐到采样帧；可能是空的）。
    QByteArray stop();

    /// 停止采集并丢弃数据。
    void abort();

    [[nodiscard]] bool    capturing() const { return mInputAudio != nullptr; }
    [[nodiscard]] int     recordedMs() const;
    [[nodiscard]] int     sampleRate() const { return mSampleRate; }
    [[nodiscard]] int     channels() const { return mChannels; }
    [[nodiscard]] QString errorString() const { return mError; }

    /// 超出单次请求的 120 秒上限时截断并发一次这个信号，宿主据此自动收尾（而不是一直占着麦克风）。
    static int maxCaptureMs();

signals:
    void limitReached();

private:
    void _release();
    void _applyLimit();

    QAudioInput* mInputAudio{nullptr};
    QIODevice*   mInputDevice{nullptr};

    QByteArray mPcm;
    QString    mError;
    int        mSampleRate{16000};
    int        mChannels{1};
    int        mMaxBytes{0};
    bool       mLimitReached{false};
    bool       mDaemonHeld{false};
    bool       mMixerEnabled{false};
};

} // namespace mod
