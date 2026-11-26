// SPDX-License-Identifier: GPL-3.0-only
/*
 * Copyright (C) 2022-present, PenUniverse.
 * This file is part of the PenMods open source project.
 */

#include "tweaker/AsrCapture.h"

#include "base/YPointer.h"

#include "common/Utils.h"

#include "system/sound/AudioDaemon.h"

#include "recorder/AudioRecorder.h"

#include "tweaker/AsrWav.h"

#include <QAudioDeviceInfo>
#include <QAudioInput>

namespace mod {

namespace {

constexpr int kSampleRate = 16000;
constexpr int kSampleSize = 16;
/// 接口上限 120 秒；留 5 秒余量给重传/编码，避免"刚好 120.0 秒"被服务端判超限。
constexpr int  kMaxCaptureMs  = 115 * 1000;
constexpr auto kMicOnCommand  = "amixer cset numid=2 1";
constexpr auto kMicOffCommand = "amixer cset numid=2 0";

/// `mod::exec()` 用 popen，理论上可能抛异常；这些命令必须在任何路径上都"只看结果不看异常"，
/// 否则一个 popen 失败会从 Qt 的定时器/信号回调里穿出去把进程带走。
void runCommand(const char* command) {
    try {
        exec(command);
    } catch (...) {
        spdlog::warn("执行命令失败：{}", command);
    }
}

/// 设备/格式探测的缓存。
/// `QAudioDeviceInfo::defaultInputDevice()` 会枚举 ALSA 设备，`isFormatSupported()` 会真的打开
/// PCM 设备做一次格式协商 —— 两步加起来在设备上是几百毫秒。以前每次按语音键都要付一遍，
/// 表现就是"键盘语音按钮点击非常卡顿"。设备热插拔在这个笔上不会发生，所以缓存住；
/// QAudioInput 真起不来时清掉缓存，下一次重新探测。
struct AudioProbe {
    bool             valid = false;
    QAudioDeviceInfo device;
    QAudioFormat     format;
};

AudioProbe& audioProbeCache() {
    static AudioProbe cache;
    return cache;
}

void invalidateAudioProbeCache() { audioProbeCache().valid = false; }

} // namespace

int AsrCapture::maxCaptureMs() { return kMaxCaptureMs; }

AsrCapture::AsrCapture(QObject* parent) : QObject(parent) { mError = QStringLiteral("麦克风未启动"); }

AsrCapture::~AsrCapture() { abort(); }

/// 把"设备 + 可用格式"探测出来（结果缓存，见 AudioProbe 的注释）。
/// 返回 false 时 reason 里是可以直接弹给用户的原因。
static bool probeCapture(const QString& microphoneMissingReason, QAudioDeviceInfo& device, QAudioFormat& format, QString& reason) {
    auto& cache = audioProbeCache();
    if (cache.valid) {
        device = cache.device;
        format = cache.format;
        return true;
    }

    device = QAudioDeviceInfo::defaultInputDevice();
    if (device.isNull() || device.deviceName().isEmpty()) {
        reason = microphoneMissingReason;
        return false;
    }

    format.setSampleRate(kSampleRate);
    format.setSampleSize(kSampleSize);
    format.setSampleType(QAudioFormat::SignedInt);
    format.setByteOrder(QAudioFormat::LittleEndian);
    format.setCodec(QStringLiteral("audio/pcm"));
#if PL_BUILD_YDP02X
    // YDP02X 的默认采集格式是双声道（见 AudioRecorder）；这里照抄，取出后再取左声道。
    format.setChannelCount(2);
#else
    format.setChannelCount(1);
#endif

    if (!device.isFormatSupported(format)) {
        const auto nearest = device.nearestFormat(format);
        // 只接受"16-bit 有符号小端"，其它位深（24/32-bit、浮点）不做重采样：宁可清楚失败
        if (nearest.sampleSize() != kSampleSize || nearest.sampleType() != QAudioFormat::SignedInt
            || nearest.byteOrder() != QAudioFormat::LittleEndian) {
            reason = QStringLiteral("麦克风不支持 16-bit PCM 采集");
            return false;
        }
        format = nearest;
    }

    cache.device = device;
    cache.format = format;
    cache.valid  = true;
    return true;
}

void AsrCapture::warmUp() {
    QString         reason;
    QAudioDeviceInfo device;
    QAudioFormat     format;
    if (!probeCapture(QStringLiteral("没有可用的麦克风"), device, format, reason))
        spdlog::warn("语音识别设备预热失败：{}", reason.toStdString());
    else
        spdlog::debug("语音识别设备预热完成：{}", device.deviceName().toStdString());
}

bool AsrCapture::start(QString& reason) {
    if (mInputAudio) {
        reason = QStringLiteral("语音识别正在录音");
        return false;
    }

    // 与 PenMods 录音机互斥：同一时刻只能有一个进程内采集器打开 ALSA 捕获设备。
    if (AudioRecorder::getInstance().isWorking()) {
        reason = QStringLiteral("录音机正在录音，语音识别暂不可用");
        return false;
    }

    QAudioDeviceInfo device;
    QAudioFormat     format;
    if (!probeCapture(QStringLiteral("没有可用的麦克风"), device, format, reason)) return false;

    mSampleRate = format.sampleRate();
    mChannels   = qMax(1, format.channelCount());
    mPcm.clear();
    mLimitReached = false;
    mError.clear();

    // 麦克风竞争：厂商唯一已知可行的顺序（forceStop → amixer on → AudioDaemon 引用）
    mDaemonHeld = true;
    AudioDaemon::getInstance().acquire(AudioSource::SYSTEM);
    if (auto* soundCenter = YPointer<YSoundCenter>::getInstance())
        PEN_CALL(void, "_ZN12YSoundCenter9forceStopEv", void*)(soundCenter);
    runCommand(kMicOnCommand);
    mMixerEnabled = true;

    mInputAudio = new QAudioInput(device, format, this);
    // 单次请求最多 120 秒的 16-bit 数据；按实际采集格式再留一倍余量，超出的部分直接丢弃。
    mMaxBytes = static_cast<int>(static_cast<qint64>(kMaxCaptureMs) * mSampleRate / 1000 * mChannels * 2);
    mInputAudio->setBufferSize(1024 * 1024);
    mInputAudio->setVolume(1.0);
    mInputDevice = mInputAudio->start();
    if (!mInputDevice) {
        mError = QStringLiteral("无法打开麦克风");
        reason = mError;
        invalidateAudioProbeCache(); // 设备布局可能变了：下次重新探测
        _release();
        return false;
    }

    if (mInputAudio->error() != QAudio::NoError) {
        mError = QStringLiteral("打开输入设备时出错");
        reason = mError;
        _release();
        return false;
    }

    connect(mInputDevice, &QIODevice::readyRead, this, [this]() {
        if (!mInputDevice) return;
        const auto chunk = mInputDevice->readAll();
        if (chunk.isEmpty()) return;
        const int room = mMaxBytes - mPcm.size();
        if (room <= 0) {
            _applyLimit();
            return;
        }
        mPcm.append(chunk.left(room));
        if (mPcm.size() >= mMaxBytes) _applyLimit();
    });

    return true;
}

void AsrCapture::_applyLimit() {
    if (mLimitReached) return;
    mLimitReached = true;
    emit limitReached();
}

QByteArray AsrCapture::stop() {
    if (!mInputAudio) return {};
    // 先读干缓冲，避免丢掉最后几百毫秒
    if (mInputDevice) {
        const auto tail = mInputDevice->readAll();
        if (!tail.isEmpty()) {
            const int room = mMaxBytes - mPcm.size();
            if (room > 0) mPcm.append(tail.left(room));
        }
    }
    const QByteArray raw      = mPcm;
    const int        channels = mChannels;
    _release();
    // ASR 只识别左声道；双声道源取 ch0 并重写为单声道 WAV。
    return asr::extractChannel(raw, channels, 0);
}

void AsrCapture::abort() {
    if (!mInputAudio) {
        mPcm.clear();
        return;
    }
    _release();
}

void AsrCapture::_release() {
    if (mInputAudio) {
        mInputAudio->stop();
        mInputAudio->disconnect(this);
        mInputAudio->deleteLater();
        mInputAudio = nullptr;
    }
    mInputDevice = nullptr;
    mPcm.clear();

    if (mMixerEnabled) {
        runCommand(kMicOffCommand);
        mMixerEnabled = false;
    }
    if (mDaemonHeld) {
        AudioDaemon::getInstance().release(AudioSource::SYSTEM);
        mDaemonHeld = false;
    }
}

int AsrCapture::recordedMs() const {
    // 仅用于日志/兜底判断，用当前累积的数据量估算，不依赖 QElapsedTimer。
    const qint64 bytesPerSecond = static_cast<qint64>(mSampleRate) * mChannels * 2;
    if (bytesPerSecond <= 0) return 0;
    return static_cast<int>(mPcm.size() * 1000 / bytesPerSecond);
}

} // namespace mod
