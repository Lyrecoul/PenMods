// SPDX-License-Identifier: GPL-3.0-only
/*
 * Copyright (C) 2022-present, PenUniverse.
 * This file is part of the PenMods open source project.
 */

#include "tweaker/AsrService.h"

#include "common/Event.h"
#include "common/Utils.h"

#include "tweaker/AsrCapture.h"
#include "tweaker/AsrWav.h"
#include "tweaker/DeepSeekAsr.h"

#include <QQmlContext>
#include <QQuickView>
#include <QThread>

namespace mod {

namespace {

// 厂商符号 —— 全部来自 `KeyBoard.cpp` 里已经在用的那几个。
constexpr auto kSetAsrResult  = "_ZN14YSpeechManager12setAsrResultERK7QString";
constexpr auto kStopAsrRecord = "_ZN14YSpeechManager13stopAsrRecordEv";

/// 注入文本之后，厂商 `recognizing` 状态如果还没回到 idle，等这么久再兜底收尾。
/// 为什么要兜底：接管期间厂商从来没有真正开始录音，它的状态机完全没前进过；如果哪个版本里
/// `setAsrResult` 自己也不推进状态，键盘麦克风图标和语音页的"正在聆听"就会一直亮着。
/// 兜底只做"能确认还卡着"时才动手（读不到状态就什么都不做，避免误调厂商 stop 把正常流程打断）。
constexpr int kStateRecoveryDelayMs = 1200;

constexpr int kDefaultTimeoutMs   = 20000;
constexpr int kDefaultMaxRecordMs = 45000;

/// 打开麦克风推迟这么久（约两帧）：QAudioInput 要枚举 ALSA 设备、申请音频引用并执行 amixer，
/// 这一段几百毫秒压在按键的槽函数里就是"键盘语音按钮点击很卡"。先让 QML 把麦克风图标/聆听态
/// 画出来，再干活；录音本身是"听到人声才开始计"的，晚 30 ms 不会丢内容。
constexpr int kCaptureStartDelayMs = 30;

} // namespace

AsrService::AsrService() : Logger("AsrService") {
    _load();

    mCapture     = new AsrCapture(this);
    mVoiceClient = new DeepSeekAsr(this);

    mSessionDeadline.setSingleShot(true);
    connect(&mSessionDeadline, &QTimer::timeout, this, [this]() {
        if (mVoiceState == VoiceState::Idle) return;
        info("语音会话达到 {} ms 上限，自动收尾", mMaxRecordMs);
        _finishSession();
    });

    // 采集长度已达接口上限：必须收尾，否则麦克风一直被占着（120 秒是 DeepSeek 的硬上限）
    connect(mCapture, &AsrCapture::limitReached, this, [this]() {
        if (mVoiceState != VoiceState::Listening) return;
        info("录音达到单次请求上限，自动收尾");
        _finishSession();
    });

    connect(mVoiceClient, &DeepSeekAsr::succeeded, this, [this](const QString& text, int durationMs) {
        QObject* manager = mPendingManager.data();
        mPendingManager.clear();
        info("DeepSeek 语音识别成功：{} 字 / {} ms", text.size(), durationMs);
        if (!manager) {
            warn("识别成功但注入目标已失效，结果没地方放");
            _setVoiceState(VoiceState::Idle, QStringLiteral("识别完成"));
            _toast(QStringLiteral("识别完成，但输入框已经关闭"));
            return;
        }
        // 先注入再复位状态：QML 的 acceptVoiceResult() 要求"语音输入仍在进行"才收下文本。
        _injectResult(manager, text);
        _setVoiceState(VoiceState::Idle, QStringLiteral("识别完成"));
    });

    connect(mVoiceClient, &DeepSeekAsr::failed, this, [this](const QString& reason) {
        mPendingManager.clear();
        warn("DeepSeek 语音识别失败：{}", reason.toStdString());
        _setVoiceState(VoiceState::Idle, reason);
        // 录制之后失败：用户那句话已经说完了，没法重放给厂商，只能讲清原因让他重来。
        _toast(QStringLiteral("DeepSeek 语音识别失败：") + reason);
    });

    connect(&Event::getInstance(), &Event::beforeUiInitialization, this, [this](QQuickView&, QQmlContext* context) {
        mRootContext = context;
        context->setContextProperty("asrService", this);
        // 设备/格式探测（ALSA 枚举 + 格式协商）预先做掉，别等用户按语音键时才付这笔开销。
        // 必须等事件循环转起来：Qt 的音频插件在这之前还不可用。
        if (mEnabled) QTimer::singleShot(1500, this, []() { AsrCapture::warmUp(); });
    });

    // 长按语音 / 语音助手页完全交还原厂：这里不安装任何 hook，也不拦 showSpeechPage。
}

// ───────────────────────── 配置 ─────────────────────────

json AsrService::_readSection() const {
    const auto ai = Config::getInstance().read("ai");
    if (!ai.is_object()) return json::object();
    const auto it = ai.find("asr");
    return it != ai.end() && it->is_object() ? *it : json::object();
}

void AsrService::_saveSection() {
    auto ai = Config::getInstance().read("ai");
    if (!ai.is_object()) ai = json::object();
    ai["asr"] = mCfg;
    Config::getInstance().write("ai", ai, true);
}

void AsrService::_load() {
    mCfg = _readSection();

    const auto enabled = mCfg.find("enabled");
    mEnabled           = enabled != mCfg.end() && enabled->is_boolean() && enabled->get<bool>();

    const auto timeout = mCfg.find("timeout_ms");
    mTimeoutMs = timeout != mCfg.end() && timeout->is_number_integer() ? std::clamp(timeout->get<int>(), 3000, 120000)
                                                                       : kDefaultTimeoutMs;

    const auto maxRecord = mCfg.find("max_record_ms");
    mMaxRecordMs         = maxRecord != mCfg.end() && maxRecord->is_number_integer()
                             ? std::clamp(maxRecord->get<int>(), 5000, AsrCapture::maxCaptureMs())
                             : kDefaultMaxRecordMs;

    // 保证这一段始终是完整形状，设置页与后续写入都不用再判空
    mCfg["enabled"]       = mEnabled;
    mCfg["timeout_ms"]    = mTimeoutMs;
    mCfg["max_record_ms"] = mMaxRecordMs;
    if (!mCfg.contains("model")) mCfg["model"] = "deepseek-asr";
    if (!mCfg.contains("deepseek_api_key")) mCfg["deepseek_api_key"] = "";
}

void AsrService::setEnabled(bool value) {
    if (mEnabled == value) return;
    mEnabled        = value;
    mCfg["enabled"] = value;
    _saveSection();

    if (mEnabled) {
        // 刚打开：UI 已经起来的话立刻预热（没起来的路径由构造函数里的延迟预热兜住）。
        if (mRootContext) QTimer::singleShot(0, this, []() { AsrCapture::warmUp(); });
    } else if (mVoiceState != VoiceState::Idle) {
        _abortSession(QStringLiteral("已关闭 DeepSeek 语音识别"));
    }
    info("DeepSeek 语音识别接管：{}", value ? "开启" : "关闭");
    emit settingsChanged();
}

void AsrService::setTimeoutMs(int value) {
    value = std::clamp(value, 3000, 120000);
    if (mTimeoutMs == value) return;
    mTimeoutMs         = value;
    mCfg["timeout_ms"] = value;
    _saveSection();
    emit settingsChanged();
}

void AsrService::selectProvider(const QString& provider) {
    setEnabled(provider.compare(QStringLiteral("deepseek"), Qt::CaseInsensitive) == 0);
}

void AsrService::setApiKey(const QString& key) {
    const auto trimmed = key.trimmed();
    if (_trimmed(mCfg, "deepseek_api_key") == trimmed) return;
    mCfg["deepseek_api_key"] = trimmed.toStdString();
    _saveSection();
    info("DeepSeek API Key 已更新（{} 字符）", trimmed.size()); // 只记长度，不记内容
    emit settingsChanged();
}

void AsrService::clearApiKey() { setApiKey(QString()); }

// ───────────────────────── API Key 解析链 ─────────────────────────

QString AsrService::_trimmed(const json& object, const char* key) {
    if (!object.is_object()) return {};
    const auto it = object.find(key);
    if (it == object.end() || !it->is_string()) return {};
    return QString::fromStdString(it->get<std::string>()).trimmed();
}

QString AsrService::apiKeySource() const {
    if (!_trimmed(mCfg, "deepseek_api_key").isEmpty()) return QStringLiteral("asr");

    const auto ai = Config::getInstance().read("ai");
    if (!ai.is_object()) return {};

    const auto search = ai.find("search");
    if (search != ai.end() && search->is_object() && !_trimmed(*search, "deepseek_api_key").isEmpty())
        return QStringLiteral("search");

    const auto activeId = _trimmed(ai, "activeModelId");
    const auto models   = ai.find("models");
    if (activeId.isEmpty() || models == ai.end() || !models->is_array()) return {};
    for (const auto& model : *models) {
        if (!model.is_object() || _trimmed(model, "id") != activeId) continue;
        // 只有明确指向 DeepSeek 官方端点的模型，它的 key 才能拿来调 ASR
        if (!_trimmed(model, "endpoint").contains(QStringLiteral("api.deepseek.com"))) return {};
        if (_trimmed(model, "apiKey").isEmpty()) return {};
        return QStringLiteral("model");
    }
    return {};
}

QString AsrService::_apiKeyFrom(const QString& source) const {
    if (source == QLatin1String("asr")) return _trimmed(mCfg, "deepseek_api_key");

    const auto ai = Config::getInstance().read("ai");
    if (!ai.is_object()) return {};

    if (source == QLatin1String("search")) {
        const auto it = ai.find("search");
        return it != ai.end() && it->is_object() ? _trimmed(*it, "deepseek_api_key") : QString();
    }
    if (source == QLatin1String("model")) {
        const auto activeId = _trimmed(ai, "activeModelId");
        const auto models   = ai.find("models");
        if (activeId.isEmpty() || models == ai.end() || !models->is_array()) return {};
        for (const auto& model : *models) {
            if (!model.is_object() || _trimmed(model, "id") != activeId) continue;
            if (!_trimmed(model, "endpoint").contains(QStringLiteral("api.deepseek.com"))) return {};
            return _trimmed(model, "apiKey");
        }
    }
    return {};
}

QString AsrService::_apiKey() const {
    const auto source = apiKeySource();
    return source.isEmpty() ? QString() : _apiKeyFrom(source);
}

QString AsrService::apiKeyHint() const {
    const auto source = apiKeySource();
    if (source.isEmpty()) return QStringLiteral("未配置");
    const auto where = source == QLatin1String("asr")    ? QStringLiteral("本页")
                     : source == QLatin1String("search") ? QStringLiteral("联网搜索")
                                                         : QStringLiteral("当前模型");
    // 只说到"已配置 + 来自哪里"，不回显 Key 的任何片段：设置页是公共可见的界面，
    // 后 4 位对排错没有帮助，反而让肩窥就能拿到一半以上的信息（真机反馈要求去掉）。
    return QStringLiteral("已配置（来自%1）").arg(where);
}

// ───────────────────────── 接管：会话 ─────────────────────────

bool AsrService::_mayTakeOver(QString& reason) const {
    reason.clear();
    // QAudioInput / QTimer 只能在有事件循环的线程里用。厂商如果从别的线程调进来，一律不接管。
    if (QThread::currentThread() != thread()) {
        reason = QStringLiteral("语音识别调用不在主线程，已使用有道语音");
        return false;
    }
    if (!mEnabled) return false;                                                // 关闭：静默走厂商路径，不打扰用户
    if (mVoiceState != VoiceState::Idle || mCaptureStarting || mCapture->capturing())
        return false; // 已在会话中（或正在打开麦克风）：忽略重复启动
    if (!hasApiKey()) {
        reason = QStringLiteral("未配置 DeepSeek API Key，已使用有道语音");
        return false;
    }
    return true;
}

QObject* AsrService::_contextSpeechManager() const {
    if (!mRootContext) return nullptr;
    // 宿主把 YSpeechManager 作为 `speechManager` 注入根上下文。键盘语音键的调用方正常都会把它
    // 传进来，这条路径只是兜底：拿不到管理器时至少还有一根可注入的引用可用。
    return mRootContext->contextProperty(QStringLiteral("speechManager")).value<QObject*>();
}

bool AsrService::_startSession(QObject* speechManager, QString& reason) {
    if (!_mayTakeOver(reason)) return false;

    QObject* manager = speechManager ? speechManager : _contextSpeechManager();
    if (!manager) {
        reason = QStringLiteral("无法定位系统语音管理器，已使用有道语音");
        return false;
    }

    // 状态先发出去、麦克风下一拍再开（原因见 kCaptureStartDelayMs）：QML 立刻把语音图标
    // 切成"聆听中"，按键反馈不再被 ALSA 初始化卡住。
    mSessionManager = manager;
    mPendingManager.clear();
    mSessionDeadline.start(mMaxRecordMs);
    mCaptureStarting = true;
    const int startSeq = ++mCaptureStartSeq;
    _setVoiceState(VoiceState::Listening, QStringLiteral("正在聆听…"));
    QTimer::singleShot(kCaptureStartDelayMs, this, [this, startSeq]() { _openCapture(startSeq); });

    info("DeepSeek 语音接管开始（键盘语音键，上限 {} ms）", mMaxRecordMs);
    return true;
}

void AsrService::_openCapture(int startSeq) {
    if (startSeq != mCaptureStartSeq || !mCaptureStarting) return;
    mCaptureStarting = false;
    if (mVoiceState == VoiceState::Idle) return; // 期间被取消/关闭

    QString captureError;
    if (!mCapture->start(captureError)) {
        // 这里是"设备真的没有可用的麦克风 / 格式不支持"这类失败（录音机占用、无 Key 等
        // 前置条件在 _mayTakeOver 里已经同步判掉并回落厂商了）。麦克风推迟打开意味着这一刻
        // 已经错过了那条同步回落路径，所以只弹原因、把界面复位，让用户重按 —— 设备层面
        // 打不开时厂商路径同样打不开。
        warn("语音接管：麦克风打开失败（{}）", captureError.toStdString());
        _toast(captureError);
        mSessionManager.clear();
        mSessionDeadline.stop();
        _setVoiceState(VoiceState::Idle, captureError);
        return;
    }
}

bool AsrService::beginVoiceCapture(QObject* speechManager) {
    try {
        QString reason;
        if (!_startSession(speechManager, reason)) {
            if (!reason.isEmpty()) _toast(reason);
            return false;
        }
        return true;
    } catch (const std::exception& e) {
        warn("语音接管异常：{}，回落有道语音", e.what());
        _abortSession(QStringLiteral("语音接管异常"));
        return false;
    } catch (...) {
        warn("语音接管出现未知异常，回落有道语音");
        _abortSession(QStringLiteral("语音接管异常"));
        return false;
    }
}

bool AsrService::endVoiceCapture(QObject* speechManager) {
    try {
        if (mVoiceState == VoiceState::Idle) return false;
        if (speechManager && mSessionManager && speechManager != mSessionManager.data()) {
            // 不是本次会话的管理器，交给厂商自己的收尾逻辑
            return false;
        }
        if (mVoiceState == VoiceState::Recognizing) {
            // 已经在等结果，再按一次 = 取消
            _abortSession(QStringLiteral("已取消识别"));
            return true;
        }
        _finishSession();
        return true;
    } catch (...) {
        warn("结束语音接管时出现异常，回落有道语音");
        return false;
    }
}

void AsrService::cancelVoiceCapture() {
    if (mVoiceState == VoiceState::Idle) return;
    info("语音接管：会话被取消");
    _abortSession(QStringLiteral("已取消"));
}

void AsrService::_finishSession() {
    if (mVoiceState != VoiceState::Listening) return;
    // 麦克风还没真正打开（用户点得太快）：这次只是取消，不算"没有录到声音"。
    if (mCaptureStarting) {
        mCaptureStartSeq++; // 作废 _openCapture 的那次调度
        mCaptureStarting = false;
        info("语音接管：麦克风尚未打开就被取消");
        mSessionManager.clear();
        mSessionDeadline.stop();
        _setVoiceState(VoiceState::Idle, QStringLiteral("已取消"));
        return;
    }
    mSessionDeadline.stop();

    const int        rate   = mCapture->sampleRate();
    const QByteArray pcm    = mCapture->stop(); // 已取左声道、单声道
    QObject*         target = mSessionManager.data();
    mSessionManager.clear();

    if (pcm.isEmpty()) {
        warn("语音接管结束：没有录到声音");
        _setVoiceState(VoiceState::Idle, QStringLiteral("没有录到声音"));
        _toast(QStringLiteral("没有录到声音，请重试"));
        return;
    }

    if (!target) {
        warn("语音接管结束：语音管理器已失效，结果无处注入");
        _setVoiceState(VoiceState::Idle, QStringLiteral("语音管理器已失效"));
        _toast(QStringLiteral("语音管理器已失效，请重试"));
        return;
    }

    mPendingManager = target;
    _setVoiceState(VoiceState::Recognizing, QStringLiteral("正在识别…"));
    const auto wav = asr::buildWav(pcm, rate, 1);
    info("语音接管结束：上传 {} 字节 WAV（PCM {} 字节）", wav.size(), pcm.size());
    // 失败会在 mVoiceClient 的 failed 回调里统一处理（气泡 + 复位状态）
    mVoiceClient->upload(wav, QStringLiteral("wav"), _apiKey(), mTimeoutMs);
}

void AsrService::_abortSession(const QString& reason) {
    if (mCaptureStarting) {
        mCaptureStartSeq++;
        mCaptureStarting = false;
    }
    mSessionDeadline.stop();
    mVoiceClient->cancel();
    mCapture->abort();
    // 接管从不推进厂商的 recognizing（我们从来没调用它的 startAsrRecord），只清自己的会话目标。
    mSessionManager.clear();
    mPendingManager.clear();
    _setVoiceState(VoiceState::Idle, reason);
}

void AsrService::_setVoiceState(VoiceState state, const QString& status) {
    const bool activeChanged = (mVoiceState == VoiceState::Idle) != (state == VoiceState::Idle);
    const bool statusChanged = mVoiceStatus != status;
    mVoiceState              = state;
    mVoiceStatus             = status;
    if (activeChanged || statusChanged) emit voiceChanged();
}

// ───────────────────────── 接管：结果注入与状态机兜底 ─────────────────────────

void AsrService::_injectResult(QObject* manager, const QString& text) {
    auto* setAsrResult = PEN_SYM(kSetAsrResult);
    if (!manager || !setAsrResult) {
        warn("找不到 YSpeechManager::setAsrResult，无法注入识别结果");
        _toast(QStringLiteral("无法把识别结果交给系统"));
        return;
    }
    using Setter      = void (*)(void*, const QString&);
    const auto setter = reinterpret_cast<Setter>(setAsrResult);
    // 先清空再写入：QML 的 onAsrResultChanged 按"文本是否变化"去重，同一句话连说两次时
    // 中间清空过一次才会再次触发。KeyBoard::startVoiceInput 进来前也是这么清的。
    const QString empty;
    setter(manager, empty);
    setter(manager, text);
    info("语音接管：已注入 {} 字", text.size());
    _scheduleStateRecovery(manager);
}

int AsrService::_readRecognizing(void* manager) {
    if (!mRecognizingProbed) {
        mRecognizingProbed = true;
        // Q_PROPERTY 的 getter 会作为符号留在 .symtab 里；名字对不上就当作"无法观测"。
        static const char* candidates[] = {"_ZNK14YSpeechManager11recognizingEv", "_ZN14YSpeechManager11recognizingEv"};
        for (const auto* symbol : candidates) {
            mRecognizingGetter = PEN_SYM(symbol);
            if (mRecognizingGetter) break;
        }
        if (!mRecognizingGetter) warn("找不到 YSpeechManager::recognizing，无法观测厂商语音状态");
    }
    if (!mRecognizingGetter || !manager) return -1;
    // 返回类型在符号里看不出来（int 或 ASRState），ARM64 上都走 w0，按 int 读
    return reinterpret_cast<int (*)(void*)>(mRecognizingGetter)(manager);
}

bool AsrService::_pushRecognizing(void* manager, int state) {
    if (!manager) return false;
    if (!mSetterProbed) {
        mSetterProbed                   = true;
        static const char* candidates[] = {
            "_ZN14YSpeechManager15setRecognizingE9ASRState",
            "_ZN14YSpeechManager15setRecognizingEi"
        };
        for (const auto* symbol : candidates) {
            mRecognizingSetter = PEN_SYM(symbol);
            if (mRecognizingSetter) break;
        }
    }
    if (!mRecognizingSetter) return false;
    reinterpret_cast<void (*)(void*, int)>(mRecognizingSetter)(manager, state);
    return true;
}

void AsrService::_scheduleStateRecovery(QObject* manager) {
    if (!manager) return;
    QPointer<QObject> guard(manager);
    QTimer::singleShot(kStateRecoveryDelayMs, this, [this, guard]() {
        if (!guard) return;
        const int state = _readRecognizing(guard.data());
        // -1 = 观测不到，>0 = 确认还卡在识别态
        if (state <= 0) return;
        warn("注入文本 {} ms 后厂商语音状态仍为 {}，尝试收尾", kStateRecoveryDelayMs, state);
        if (_pushRecognizing(guard.data(), 0)) return;
        // 没有 setter：退回厂商自己的 stop 接口收尾（就是 KeyBoard::stopVoiceInput 用的那个）
        auto* stopAsrRecord = PEN_SYM(kStopAsrRecord);
        if (!stopAsrRecord) {
            warn("既没有 setRecognizing 也没有 stopAsrRecord，无法收尾厂商语音状态");
            return;
        }
        warn("改用 YSpeechManager::stopAsrRecord 收尾");
        reinterpret_cast<void (*)(void*)>(stopAsrRecord)(guard.data());
    });
}

void AsrService::_toast(const QString& text) {
    if (text.isEmpty()) return;
    info("提示：{}", text.toStdString());
    // UI 还没起来（或已销毁）时不要往宿主里打：showToast 依赖 YGlobal 单例，早了会打空指针
    if (!mRootContext) return;
    // 同一条提示 2 秒内只弹一次：键盘语音键的失败与回落可能在同一毫秒里各说一次。
    if (text == mLastToastText && mLastToastClock.isValid() && mLastToastClock.elapsed() < 2000) return;
    mLastToastText = text;
    mLastToastClock.start();
    try {
        showToast(text.toStdString());
    } catch (...) {
        warn("气泡提示失败：{}", text.toStdString());
    }
}

} // namespace mod
