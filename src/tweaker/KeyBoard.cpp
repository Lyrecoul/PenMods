// SPDX-License-Identifier: GPL-3.0-only
/*
 * Copyright (C) 2022-present, PenUniverse.
 * This file is part of the PenMods open source project.
 */

#include "tweaker/KeyBoard.h"

#include "common/Event.h"
#include "mod/Config.h"
#include "system/input/ScreenManager.h"

#include "base/YPointer.h"

#include <QDateTime>
#include <QFile>
#include <QQmlContext>

#include <atomic>
#include <cstring>

namespace {

// ─────────────────────────────────────────────────────────────────────────────
// 扫描诊断日志（[ScanDiag]）
//
// 目的：多行扫描「漏句子」时，判断文本是在哪一层丢的。这里只读、不改行为，
// 每次扫描按下面顺序把每一层都记下来（日志里用同一个 session 序号关联）：
//
//   onScanStart                     手势开始；清空原厂显示结果、记录开关状态
//   onScanning            (debug)   多帧拼接过程中原厂给出的实时文本（每帧变化记一次）
//   onOcrComplete         (PRE)     原厂拼接/切分后的原始识别文本（逐行/整段），进 onScanFinish 之前
//   onContinueScan                  原厂连续扫描事件（多行追加）
//   onScanFinish(RAW)               交给 YSystemBase 的文本（单行）
//   onScanFinish(POST-MERGE)        原厂合并后、QML 真正会显示/查询的 ocrCompletedResult
//   onStopContinueScan              原厂清空「连续扫描拼接基串」
//   ocrCompletedResultChanged       显示文本变更（被 PenMods 拦截时也会记一条）
//   showDictPage          (DISPLAY) 词典页实际拿到的文本（最终显示/查询长度）
//
// 换行数 = '\n' 的个数；lines = 非空时的 换行数+1。文本预览把换行转义成 \n，
// 超过 2*160 字符时只保留头尾，避免把长段落整段塞进日志。
// ─────────────────────────────────────────────────────────────────────────────

std::atomic<uint64_t> g_scanSession{0};
std::atomic<int64_t>  g_scanStartMs{0};
std::atomic<int64_t>  g_scanFinishMs{0};
std::atomic<int>      g_stitchUpdates{0};
std::atomic<int>      g_stitchMaxLen{0};
std::atomic<int>      g_continueEvents{0};
std::atomic<uint64_t> g_lastStitchHash{0};
std::atomic<int>      g_lastStitchLen{-1};
std::atomic<int>      g_fullResultLen{-1};
std::atomic<int>      g_lastShownLen{-1};

int64_t scanNowMs() {
    static const int64_t origin = QDateTime::currentMSecsSinceEpoch();
    return QDateTime::currentMSecsSinceEpoch() - origin;
}

// 诊断日志默认按 debug 输出（只有 --force-debug-log 构建才可见）；设备上存在
// /userdisk/PenMods_scan_diag 或环境变量 PENMODS_SCAN_DIAG 时提升为 info，
// 便于让普通用户/复现者提日志而不需要专门构建。
//
// 开关文件每 3 秒最多 stat 一次：既能在 app 运行中创建（无需重启）后 3 秒内生效，
// 也不会在每次扫描事件上做文件系统调用。release 构建 info 级打印默认开启时才会走到这里。
bool scanDiagInfoRequested() {
    static const bool envSet = qEnvironmentVariableIsSet("PENMODS_SCAN_DIAG");
    if (envSet) return true;
    static std::atomic<int64_t> nextCheckMs{0};
    static std::atomic<bool>    fileSeen{false};
    const int64_t               now = scanNowMs();
    if (now < nextCheckMs.load(std::memory_order_relaxed)) return fileSeen.load(std::memory_order_relaxed);
    const bool seen = QFile::exists(QStringLiteral("/userdisk/PenMods_scan_diag"));
    fileSeen.store(seen, std::memory_order_relaxed);
    nextCheckMs.store(now + 3000, std::memory_order_relaxed);
    return seen;
}

// 「零成本关闭」判定：release 构建既没有 info 提升、也没有开启 debug 级别时，
// 所有诊断 hook 在构造任何 QString、调用任何原厂 getter 之前直接透传，
// 不给 PP-OCR 扫描线程增加任何可测开销。should_log 只是一次原子级别比较，不分配内存。
bool scanDiagActive() {
    auto logger = spdlog::default_logger();
    // debug 级别（--force-debug-log 构建）直接输出，不动文件系统。
    if (logger != nullptr && logger->should_log(spdlog::level::debug)) return true;
    // 否则只有在 info 会被打印时才检查提升开关（release 构建默认 info 级）。
    if (logger == nullptr || !logger->should_log(spdlog::level::info)) return false;
    return scanDiagInfoRequested();
}

uint64_t currentSession() { return g_scanSession.load(); }

const char* scanTypeName(int scanType) {
    switch (scanType) {
    case 0:
        return "POINT(0)";
    case 1:
        return "SCAN(1)";
    default:
        return "OTHER";
    }
}

QString escapedPreview(const QString& text, int edge = 160) {
    QString out = text;
    out.replace(QLatin1String("\\"), QLatin1String("\\\\"));
    out.replace(QLatin1Char('\n'), QLatin1String("\\n"));
    out.replace(QLatin1Char('\r'), QLatin1String("\\r"));
    out.replace(QLatin1Char('\t'), QLatin1String("\\t"));
    if (out.size() <= edge * 2) return out;
    return out.left(edge) + QLatin1String("…<truncated>…") + out.right(edge);
}

// "len=/utf8=/lines=/newlines=/blanks=" —— 覆盖需求里的「文本长度、换行数」。
QString describeText(const QString& text) {
    int  newlines   = 0;
    int  blankLines = 0;
    bool lineEmpty  = true;
    for (const QChar c : text) {
        if (c == QLatin1Char('\n')) {
            ++newlines;
            if (lineEmpty) ++blankLines;
            lineEmpty = true;
        } else if (!c.isSpace()) {
            lineEmpty = false;
        }
    }
    const int lines = text.isEmpty() ? 0 : newlines + 1;
    return QStringLiteral("len=%1 utf8=%2 lines=%3 newlines=%4 blanks=%5 preview=\"%6\"")
        .arg(text.size())
        .arg(text.toUtf8().size())
        .arg(lines)
        .arg(newlines)
        .arg(blankLines)
        .arg(escapedPreview(text));
}

// 读取 libstdc++ 的 std::string（32 字节：ptr/len/union{cap,SSO[16]}）。
// PenMods 是 libc++，不能直接解引用 vendor 的 std::string；这里只在**只读**前提下
// 按布局取数据（SSO 时 ptr 指向自身 +16），与 CameraCapture 里解析 ydstitch 返回值的做法一致。
QString fromLibStdString(const void* address) {
    if (address == nullptr) return {};
    const auto* bytes   = static_cast<const unsigned char*>(address);
    uint64      dataPtr = 0;
    uint64      size    = 0;
    std::memcpy(&dataPtr, bytes, sizeof(dataPtr));
    std::memcpy(&size, bytes + sizeof(dataPtr), sizeof(size));
    const uint64 selfAddr = reinterpret_cast<uint64>(address);
    const char*  data     = reinterpret_cast<const char*>(dataPtr);
    if (dataPtr >= selfAddr && dataPtr < selfAddr + 32) {
        data = reinterpret_cast<const char*>(bytes + 16);
    }
    if (data == nullptr || size == 0 || size > (1u << 20)) return {};
    return QString::fromUtf8(data, static_cast<int>(size));
}

struct ScanEnv {
    bool continueScan     = false; // 原厂「多行扫描翻译」开关
    bool inputPageShowing = false; // 原厂输入页是否显示（PenMods 用它拦截扫描）
    bool autoSendScan     = false; // PenMods「扫描结果快速发送」
    int  vendorScanType   = -1;    // 原厂 YGlobal.scanType（0=点查 1=整行）
};

ScanEnv readScanEnv() {
    ScanEnv env;
    if (auto* setting = mod::YPointer<YSettingManager>::getInstance()) {
        env.continueScan =
            PEN_CALL(bool, "_ZNK15YSettingManager14isContinueScanEv", uint64)(reinterpret_cast<uint64>(setting));
    }
    if (auto* global = mod::YPointer<YGlobal>::getInstance()) {
        env.inputPageShowing = PEN_CALL(bool, "_ZNK7YGlobal16inputPageShowingEv", void*)(global);
        env.vendorScanType   = PEN_CALL(int, "_ZNK7YGlobal8scanTypeEv", void*)(global);
    }
    env.autoSendScan = mod::KeyBoard::getInstance().autoSendScan();
    return env;
}

void logScanEvent(const char* event, const QString& detail) {
    if (scanDiagInfoRequested()) {
        spdlog::info("[ScanDiag] #{} t=+{}ms {} {}", currentSession(), scanNowMs(), event, detail.toStdString());
    } else {
        spdlog::debug("[ScanDiag] #{} t=+{}ms {} {}", currentSession(), scanNowMs(), event, detail.toStdString());
    }
}

QString envSummary(const ScanEnv& env) {
    return QStringLiteral(
               "continueScan=%1 inputPageShowing=%2 autoSendScan=%3 vendorScanType=%4 stitchUpdates=%5 "
               "continueEvents=%6"
    )
        .arg(env.continueScan)
        .arg(env.inputPageShowing)
        .arg(env.autoSendScan)
        .arg(env.vendorScanType)
        .arg(g_stitchUpdates.load())
        .arg(g_continueEvents.load());
}

} // namespace

namespace mod {

KeyBoard::KeyBoard() {
    auto& config         = mod::Config::getInstance();
    json  aiCfg          = config.read("ai");
    m_autoSendScanConfig = aiCfg.contains("auto_send_scan") ? aiCfg["auto_send_scan"].get<bool>() : true;

    mCfg             = config.read(mClassName);
    m_keyboardLayout = QString::fromStdString(mCfg.value("layout", std::string("native")));
    if (m_keyboardLayout != "compact") m_keyboardLayout = "native";

    connect(&Event::getInstance(), &Event::beforeUiInitialization, [this](QQuickView& view, QQmlContext* context) {
        context->setContextProperty("keyBoard", this);
    });
}

void KeyBoard::setKeyboardLayout(const QString& value) {
    const QString normalized = value == "compact" ? "compact" : "native";
    if (m_keyboardLayout == normalized) return;
    m_keyboardLayout = normalized;
    mCfg["layout"]   = normalized.toStdString();
    WRITE_CFG;
    emit keyboardLayoutChanged();
}

void KeyBoard::setAutoSendScan(bool value) {
    if (value && m_inputPageShowing) return;
    if (m_autoSendScan != value) {
        m_autoSendScan = value;
        emit autoSendScanChanged();
    }
}

void KeyBoard::setInputPageShowing(bool value) {
    if (m_inputPageShowing != value) {
        m_inputPageShowing = value;
        emit inputPageShowingChanged();
    }
}

void KeyBoard::setAutoSendScanConfig(bool value) {
    if (m_autoSendScanConfig != value) {
        m_autoSendScanConfig = value;
        auto& config         = mod::Config::getInstance();
        json  aiCfg          = config.read("ai");
        if (aiCfg.is_null()) aiCfg = json::object();
        aiCfg["auto_send_scan"] = value;
        config.write("ai", aiCfg, true);
        emit autoSendScanConfigChanged();
    }
}

bool KeyBoard::startVoiceInput(QObject* speechManager) {
    auto* startAsrRecord = PEN_SYM("_ZN14YSpeechManager14startAsrRecordEv");
    auto* setAsrResult   = PEN_SYM("_ZN14YSpeechManager12setAsrResultERK7QString");
    if (speechManager == nullptr || startAsrRecord == nullptr || setAsrResult == nullptr) return false;

    const QString emptyResult;
    reinterpret_cast<void (*)(void*, const QString&)>(setAsrResult)(speechManager, emptyResult);

    m_startingVoiceInput = true;
    reinterpret_cast<void (*)(void*)>(startAsrRecord)(speechManager);
    m_startingVoiceInput = false;
    return true;
}

bool KeyBoard::stopVoiceInput(QObject* speechManager) {
    auto* stopAsrRecord = PEN_SYM("_ZN14YSpeechManager13stopAsrRecordEv");
    if (speechManager == nullptr || stopAsrRecord == nullptr) return false;

    reinterpret_cast<void (*)(void*)>(stopAsrRecord)(speechManager);
    return true;
}

} // namespace mod

PEN_HOOK(uint64, _ZN7YGlobal14showSpeechPageEv, uint64 self) {
    if (mod::KeyBoard::getInstance().isStartingVoiceInput()) return 0;
    return origin(self);
}

static bool shouldBlockScan() {
    bool inputPageShowing =
        PEN_CALL(bool, "_ZNK7YGlobal16inputPageShowingEv", void*)(mod::YPointer<YGlobal>::getInstance());
    return inputPageShowing || mod::KeyBoard::getInstance().autoSendScan();
}

// ── 扫描诊断：手势开始 ───────────────────────────────────────────────────────
PEN_HOOK(void, _ZN11YSystemBase11onScanStartEv, uint64 self) {
    if (!scanDiagActive()) {
        origin(self);
        return;
    }
    ++g_scanSession;
    g_scanStartMs    = scanNowMs();
    g_scanFinishMs   = 0;
    g_stitchUpdates  = 0;
    g_stitchMaxLen   = 0;
    g_continueEvents = 0;
    g_lastStitchHash = 0;
    g_lastStitchLen  = -1;
    g_fullResultLen  = -1;
    g_lastShownLen   = -1;

    const ScanEnv env = readScanEnv();
    logScanEvent(
        "onScanStart",
        QStringLiteral("%1 blocked=%2").arg(envSummary(env)).arg(env.inputPageShowing || env.autoSendScan)
    );
    origin(self);
}

// ── 扫描诊断：拼接过程中的实时文本（每帧变化记一次，debug 级） ──────────────
PEN_HOOK(void, _ZN11YSystemBase10onScanningERK7QString, uint64 self, QString const& text) {
    if (!scanDiagActive()) {
        origin(self, text);
        return;
    }
    const int      len  = text.size();
    const uint64_t hash = static_cast<uint64_t>(qHash(text));
    ++g_stitchUpdates;
    if (len > g_stitchMaxLen.load()) g_stitchMaxLen = len;

    const bool changed = hash != g_lastStitchHash.load() || len != g_lastStitchLen.load();
    if (changed) {
        g_lastStitchHash = hash;
        g_lastStitchLen  = len;
        spdlog::debug(
            "[ScanDiag] #{} t=+{}ms onScanning stitchUpdate={} {}",
            currentSession(),
            scanNowMs(),
            g_stitchUpdates.load(),
            describeText(text).toStdString()
        );
    }
    origin(self, text);
}

// ── 扫描诊断：原厂 OCR 原始文本（进 onScanFinish 之前的最后一层） ────────────
// 用 void* 接 libstdc++ std::string 引用，避免跨 ABI 构造/析构。
PEN_HOOK(
    uint64,
    _ZN11YSystemBase13onOcrCompleteERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEi,
    uint64 self,
    void*  rawText,
    int    scanType
) {
    if (!scanDiagActive()) return origin(self, rawText, scanType);
    const ScanEnv env = readScanEnv();
    logScanEvent(
        "onOcrComplete(PRE-JOIN)",
        QStringLiteral("scanType=%1 %2 %3")
            .arg(scanTypeName(scanType))
            .arg(describeText(fromLibStdString(rawText)))
            .arg(envSummary(env))
    );
    return origin(self, rawText, scanType);
}

// ── 扫描诊断：原厂连续扫描事件 ──────────────────────────────────────────────
PEN_HOOK(uint64, _ZN11YSystemBase14onContinueScanERK7QStringb, uint64 self, QString const& text, bool a2) {
    if (!scanDiagActive()) return origin(self, text, a2);
    ++g_continueEvents;
    const ScanEnv env = readScanEnv();
    logScanEvent(
        "onContinueScan",
        QStringLiteral("flag=%1 %2 %3").arg(a2).arg(describeText(text)).arg(envSummary(env))
    );
    return origin(self, text, a2);
}

// ── 扫描诊断：原厂清空「连续扫描拼接基串」 ─────────────────────────────────
PEN_HOOK(uint64, _ZN11YSystemBase18onStopContinueScanEv, uint64 self) {
    if (!scanDiagActive()) return origin(self);
    logScanEvent("onStopContinueScan", QStringLiteral("(clears join base) %1").arg(envSummary(readScanEnv())));
    return origin(self);
}

// ── 原厂显示结果被显式赋值（QML/Qt 属性路径） ──────────────────────────────
PEN_HOOK(uint64, _ZN11YSystemBase21setOcrCompletedResultE7QString, uint64 self, QString value) {
    if (!scanDiagActive()) return origin(self, value);
    logScanEvent("setOcrCompletedResult", describeText(value));
    return origin(self, value);
}

// ── 显示文本变更（QML 的 systemBase.ocrCompletedResult 就是它） ─────────────
PEN_HOOK(
    uint64,
    _ZN11YSystemBase25ocrCompletedResultChangedEv,
    uint64 self,
    uint64 a2,
    uint64 a3,
    uint64 a4,
    uint64 a5
) {
    const bool active = scanDiagActive();
    // 拦截行为与诊断开关无关：保持原有判定。
    if (shouldBlockScan()) {
        if (active) {
            const ScanEnv env = readScanEnv();
            logScanEvent("ocrCompletedResultChanged SUPPRESSED", envSummary(env));
        }
        return false;
    }
    // origin 的实参按 ABI 顺序 a2,a3,a4,a5 传，调换会让宿主读到别的字段。
    const uint64 ret = origin(self, a2, a3, a4, a5);
    if (active) {
        const QString displayed = PEN_CALL(QString, "_ZN11YSystemBase18ocrCompletedResultEv", uint64)(self);
        g_fullResultLen         = displayed.size();
        const ScanEnv env       = readScanEnv();
        logScanEvent(
            "ocrCompletedResultChanged(DISPLAY)",
            QStringLiteral("%1 %2").arg(describeText(displayed)).arg(envSummary(env))
        );
    }
    return ret;
}

// ── 词典页拿到的内容（最终显示/查询长度） ─────────────────────────────────
PEN_HOOK(void, _ZN7YGlobal12showDictPageEiRK7QString, uint64 self, int pageIndex, QString const& content) {
    if (!scanDiagActive()) {
        origin(self, pageIndex, content);
        return;
    }
    // QML 的逐字打字动画会每 12ms 调一次 showDictPage(substring)；这里只记「打到完整长度」
    // 的那一次，避免一次扫描刷几百行日志。
    const int64_t sinceFinish = scanNowMs() - g_scanFinishMs.load();
    const int     len         = content.size();
    const int     fullLen     = g_fullResultLen.load();
    if (g_scanFinishMs.load() > 0 && sinceFinish >= 0 && sinceFinish <= 30000 && fullLen > 0 && len >= fullLen
        && len != g_lastShownLen.load()) {
        g_lastShownLen = len;
        logScanEvent(
            "showDictPage(DISPLAY)",
            QStringLiteral("page=%1 sinceScanFinish=%2ms %3").arg(pageIndex).arg(sinceFinish).arg(describeText(content))
        );
    }
    origin(self, pageIndex, content);
}

PEN_HOOK(bool, _ZN11YSystemBase12onScanFinishERK7QStringi, uint64 self, QString const& content, ScanType scanType) {
    const bool active = scanDiagActive();
    const bool block  = shouldBlockScan();
    if (active) {
        const ScanEnv env         = readScanEnv();
        const QString beforeMerge = PEN_CALL(QString, "_ZN11YSystemBase18ocrCompletedResultEv", uint64)(self);
        logScanEvent(
            "onScanFinish(PRE-MERGE)",
            QStringLiteral("scanType=%1 blocked=%2 prevResult=%3 %4")
                .arg(scanTypeName(scanType))
                .arg(block)
                .arg(describeText(beforeMerge))
                .arg(envSummary(env))
        );
        logScanEvent("onScanFinish(RAW)", describeText(content));
    }

    if (block) {
        emit mod::KeyBoard::getInstance().scanFinished(content);
        g_scanFinishMs = scanNowMs();
        if (active) {
            logScanEvent(
                "onScanFinish(BLOCKED)",
                QStringLiteral("emitted keyBoard.scanFinished %1").arg(describeText(content))
            );
        }
        return false;
    }
    const uint64 ret = origin(self, content, scanType);
    g_scanFinishMs   = scanNowMs();
    if (active) {
        const QString afterMerge = PEN_CALL(QString, "_ZN11YSystemBase18ocrCompletedResultEv", uint64)(self);
        logScanEvent(
            "onScanFinish(POST-MERGE)",
            QStringLiteral("%1 stitchUpdatesAtFinish=%2 stitchMaxLen=%3")
                .arg(describeText(afterMerge))
                .arg(g_stitchUpdates.load())
                .arg(g_stitchMaxLen.load())
        );
    }
    return ret;
}

PEN_HOOK(uint64, _ZN11YSystemBase8ocrStartEv, uint64 self, uint64 a2, uint64 a3, uint64 a4, uint64 a5) {
    mod::ScreenManager::getInstance().setSystemBase(reinterpret_cast<YSystemBase*>(self));
    const bool isButtonRelease =
        PEN_CALL(bool, "_ZNK11YSystemBase15isButtonReleaseEv", void*)(reinterpret_cast<void*>(self));
    if (!isButtonRelease) {
        emit mod::Event::getInstance().ocrStarted();
    }
    if (shouldBlockScan()) {
        if (scanDiagActive()) logScanEvent("ocrStart BLOCKED", envSummary(readScanEnv()));
        return false;
    }
    if (scanDiagActive()) logScanEvent("ocrStart", envSummary(readScanEnv()));
    return origin(self, a2, a3, a4, a5);
}

PEN_HOOK(uint64, _ZN11YSystemBase7ocrStopEi, uint64 self, int a2, uint64 a3, uint64 a4, uint64 a5) {
    // 先发事件再判拦截：被 shouldBlockScan 拦下的扫描也必须关窗，否则扫描保护会一直占着 MUSIC 引用。
    emit mod::Event::getInstance().ocrStopped();
    if (shouldBlockScan()) {
        if (scanDiagActive()) {
            const ScanEnv env = readScanEnv();
            logScanEvent(
                "ocrStop BLOCKED",
                QStringLiteral("scanType=%1 %2").arg(scanTypeName(a2)).arg(envSummary(env))
            );
        }
        return false;
    }
    if (scanDiagActive()) {
        const ScanEnv env = readScanEnv();
        logScanEvent("ocrStop", QStringLiteral("scanType=%1 %2").arg(scanTypeName(a2)).arg(envSummary(env)));
    }
    return origin(self, a2, a3, a4, a5);
}
