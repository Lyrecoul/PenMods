// SPDX-License-Identifier: GPL-3.0-only
/*
 * Copyright (C) 2022-present, PenUniverse.
 * This file is part of the PenMods open source project.
 */

#pragma once

#include "common/service/Logger.h"
#include "mod/Config.h"

#include <QByteArray>
#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>

class QQmlContext;

namespace mod {

class AsrCapture;
class DeepSeekAsr;

/// 需求 10（DeepSeek 语音识别接管）。
///
/// 接管范围 —— 只有键盘上的语音按钮，这是真机与用户共同确认的唯一范围：
///   • 键盘语音键（`mod::KeyBoard::startVoiceInput` / `stopVoiceInput` 分流）：自己采集 →
///     上传 DeepSeek → 通过 `YSpeechManager::setAsrResult` 注入文本，输入页照旧收下。
///   • 长按语音 / 语音助手页：**完全交还原厂**。这条链路一行都没被动过 —— 不 hook
///     `YRecordCenter::startAsr`、不 hook `YSpeechManager::startAsrRecord` / `stopAsrRecord`、
///     不拦 `YGlobal::showSpeechPage`、不弹气泡、不打开任何页面。原厂的识别、页面跳转、
///     语音助手文案全部由厂商自己负责。
///   • 跟读评分（`startFollowSpeech`）同样完全不动。
///
/// 设计约束（都是硬性的）：
///   • **默认关闭**。关闭时不碰麦克风、不建网络管理器 —— 行为与没有这个类时完全一致；
///     `ai.asr.enabled` 是唯一的开关。
///   • 只做"启动/结果级"接管：厂商的语音识别协议是未知的，我们不解析它的音频，也不重写
///     它的状态机，只在自己录音 → DeepSeek 识别 → 注入文本。
///   • 任何一步失败都必须能回到用户熟悉的行为。启动阶段（还没开始录音）失败 → 直接调用
///     `origin(...)` 让厂商自己识别；录制之后失败（超时/HTTP/解析/空音频）没法重放用户的
///     这句话，所以只弹一次气泡说明原因并复位 UI，让用户重来 —— 绝不把界面留在"聆听中"。
///   • 网络全程异步（QNetworkAccessManager + 超时定时器），UI 线程不和网络同步。
///   • 日志不写音频、不写 API Key。
class AsrService : public QObject, public Singleton<AsrService>, private Logger {
    Q_OBJECT

    // ───────────── 需求 10：语音识别服务设定 ─────────────
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY settingsChanged)
    /// 当前选择的语音服务："system"（默认有道语音）/ "deepseek"（映射到 enabled）
    Q_PROPERTY(QString provider READ provider NOTIFY settingsChanged)
    Q_PROPERTY(bool hasApiKey READ hasApiKey NOTIFY settingsChanged)
    Q_PROPERTY(QString apiKeyHint READ apiKeyHint NOTIFY settingsChanged)
    Q_PROPERTY(QString apiKeySource READ apiKeySource NOTIFY settingsChanged)
    Q_PROPERTY(int timeoutMs READ timeoutMs WRITE setTimeoutMs NOTIFY settingsChanged)
    /// 我们自己的麦克风采集/识别是否在进行（键盘语音键的会话状态）。
    /// QML 用它和厂商的 `speechManager.recognizing` 一起判断"是否正在聆听"，因为接管期间
    /// 厂商的状态机根本不会前进（我们从来没有调用它的 startAsrRecord）。
    Q_PROPERTY(bool voiceActive READ voiceActive NOTIFY voiceChanged)
    Q_PROPERTY(QString voiceStatus READ voiceStatus NOTIFY voiceChanged)

public:
    // ───────────── 需求 10：设置页 ─────────────
    [[nodiscard]] bool    enabled() const { return mEnabled; }
    void                  setEnabled(bool value);
    [[nodiscard]] QString provider() const { return mEnabled ? QStringLiteral("deepseek") : QStringLiteral("system"); }
    [[nodiscard]] bool    hasApiKey() const { return !_apiKey().isEmpty(); }
    [[nodiscard]] QString apiKeyHint() const;
    [[nodiscard]] QString apiKeySource() const;
    [[nodiscard]] int     timeoutMs() const { return mTimeoutMs; }
    void                  setTimeoutMs(int value);
    [[nodiscard]] bool    voiceActive() const { return mVoiceState != VoiceState::Idle; }
    [[nodiscard]] QString voiceStatus() const { return mVoiceStatus; }

    Q_INVOKABLE void selectProvider(const QString& provider);
    Q_INVOKABLE void setApiKey(const QString& key);
    Q_INVOKABLE void clearApiKey();

    /// 键盘语音键（`mod::KeyBoard::startVoiceInput` / `stopVoiceInput`）的接管入口。
    /// 返回 true 表示 AsrService 已经接管这次语音输入，调用方不要再进厂商路径。
    bool beginVoiceCapture(QObject* speechManager);
    bool endVoiceCapture(QObject* speechManager);
    /// 放弃当前会话（离开输入页时用）：不录音、不上传、直接把麦克风还给系统。
    Q_INVOKABLE void cancelVoiceCapture();

signals:
    void settingsChanged();
    void voiceChanged();

private:
    friend Singleton<AsrService>;
    explicit AsrService();

    /// 我们的语音会话状态。Listening = 正在录音；Recognizing = 已停止采集、等 DeepSeek 回结果。
    /// 两段都算 voiceActive，因为 QML 要在这两段里都把"正在聆听/识别中"显示出来，而且要挡掉
    /// 第二次"开始"（第二次点击的含义是停止 / 取消）。
    enum class VoiceState { Idle, Listening, Recognizing };

    // ── 配置（ai.asr 段；Config 只支持顶层键，读写都在 ai 对象里做） ──
    json _readSection() const;
    void _saveSection();
    void _load();

    // ── API Key 解析链 ──
    QString        _apiKey() const;
    QString        _apiKeyFrom(const QString& source) const;
    static QString _trimmed(const json& object, const char* key);

    // ── 接管（需求 10） ──
    bool     _mayTakeOver(QString& reason) const;
    bool     _startSession(QObject* speechManager, QString& reason);
    /// 真正打开麦克风（由 _startSession 推迟一拍调用，见那里的注释）。
    void     _openCapture(int startSeq);
    void     _finishSession();
    void     _abortSession(const QString& reason);
    void     _setVoiceState(VoiceState state, const QString& status);
    QObject* _contextSpeechManager() const;
    void     _injectResult(QObject* manager, const QString& text);
    void     _scheduleStateRecovery(QObject* manager);
    int      _readRecognizing(void* manager);
    bool     _pushRecognizing(void* manager, int state);
    void     _toast(const QString& text);

    // ── 状态 ──
    json mCfg;
    bool mEnabled{false};
    int  mTimeoutMs{20000};
    int  mMaxRecordMs{45000};

    QPointer<QQmlContext> mRootContext;
    AsrCapture*           mCapture{nullptr};
    DeepSeekAsr*          mVoiceClient{nullptr};

    // 语音会话
    QPointer<QObject> mSessionManager; ///< 本次会话要注入文本的 YSpeechManager
    QPointer<QObject> mPendingManager; ///< 已停止采集、等识别结果的注入目标
    QTimer            mSessionDeadline;
    /// 麦克风正在异步打开（_startSession 发出的单拍延迟还没到）。这段时间里状态已经是
    /// Listening，但 mCapture 还没开始采，停止时必须先取消这次打开。
    bool mCaptureStarting{false};
    int  mCaptureStartSeq{0};
    VoiceState        mVoiceState{VoiceState::Idle};
    bool              mRecognizingProbed{false};
    void*             mRecognizingGetter{nullptr};
    bool              mSetterProbed{false};
    void*             mRecognizingSetter{nullptr};
    QString           mVoiceStatus;
    QString           mLastToastText; ///< 同一条气泡在短时间内只弹一次
    QElapsedTimer     mLastToastClock;
};

} // namespace mod
