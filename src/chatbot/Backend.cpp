// SPDX-License-Identifier: GPL-3.0-only
/*
 * Copyright (C) 2022-present, PenUniverse.
 * This file is part of the PenMods open source project.
 */

#include "chatbot/Backend.h"

#include "chatbot/AttachmentManager.h"

#include "chatbot/MarkdownParser.h"
#include "common/Event.h"
#include "common/Utils.h"
#include "common/service/Logger.h"
#include "common/util/System.h"
#include "mod/Config.h"
#include "mod/Mod.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QHttpMultiPart>
#include <QHttpPart>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutexLocker>
#include <QNetworkReply>
#include <QPointer>
#include <QProcess>
#include <QQmlContext>
#include <QRegularExpression>
#include <QRunnable>
#include <QTextDocument>
#include <QTextStream>
#include <QTimer>
#include <QUrl>
#include <QUuid>

#include <type_traits>
#include <utility>

namespace mod::chatbot {
namespace {

template <typename Function>
class MarkdownTask final : public QRunnable {
public:
    explicit MarkdownTask(Function function) : m_function(std::move(function)) {}

    void run() override { m_function(); }

private:
    Function m_function;
};

template <typename Function>
QRunnable* makeMarkdownTask(Function&& function) {
    using Task = MarkdownTask<std::decay_t<Function>>;
    return new Task(std::forward<Function>(function));
}

} // namespace

// -----------------------------------------------------------------------
// Markdown → HTML
// -----------------------------------------------------------------------

QString ChatBot::markdownToHtml(const QString& markdown) {
    QTextDocument doc;
    QFont         defaultFont;
    defaultFont.setPixelSize(12);
    defaultFont.setFamily("OPPOSans, OPPOSans M, Microsoft YaHei, SimSun, Arial");
    doc.setDefaultFont(defaultFont);
    doc.setDefaultStyleSheet("body { line-height: 1; }");
    doc.setMarkdown(markdown);
    return doc.toHtml();
}

QVariantMap ChatBot::getApiCacheStats() const {
    const double totalRate = m_totalInputTokens > 0 ? 100.0 * static_cast<double>(m_totalCachedTokens)
                                                          / static_cast<double>(m_totalInputTokens)
                                                    : 0.0;
    const double lastRate = m_lastInputTokens > 0
                              ? 100.0 * static_cast<double>(m_lastCachedTokens) / static_cast<double>(m_lastInputTokens)
                              : 0.0;
    return {
        {"sampleCount",       QVariant::fromValue<qulonglong>(m_cacheSampleCount) },
        {"totalInputTokens",  QVariant::fromValue<qulonglong>(m_totalInputTokens) },
        {"totalCachedTokens", QVariant::fromValue<qulonglong>(m_totalCachedTokens)},
        {"totalRate",         totalRate                                           },
        {"lastInputTokens",   QVariant::fromValue<qulonglong>(m_lastInputTokens)  },
        {"lastCachedTokens",  QVariant::fromValue<qulonglong>(m_lastCachedTokens) },
        {"lastRate",          lastRate                                            }
    };
}

void ChatBot::resetApiCacheStats() {
    m_cacheSampleCount  = 0;
    m_totalInputTokens  = 0;
    m_totalCachedTokens = 0;
    m_lastInputTokens   = 0;
    m_lastCachedTokens  = 0;
    emit apiCacheStatsChanged();
}

void ChatBot::recordApiUsage(const QJsonObject& usage) {
    quint64 input = static_cast<quint64>(usage["prompt_tokens"].toVariant().toULongLong());
    if (input == 0) input = static_cast<quint64>(usage["input_tokens"].toVariant().toULongLong());

    quint64           cached        = 0;
    const QJsonObject promptDetails = usage["prompt_tokens_details"].toObject();
    const QJsonObject inputDetails  = usage["input_tokens_details"].toObject();
    cached                          = static_cast<quint64>(promptDetails["cached_tokens"].toVariant().toULongLong());
    if (cached == 0) cached = static_cast<quint64>(inputDetails["cached_tokens"].toVariant().toULongLong());

    const quint64 cacheHit  = static_cast<quint64>(usage["prompt_cache_hit_tokens"].toVariant().toULongLong());
    const quint64 cacheMiss = static_cast<quint64>(usage["prompt_cache_miss_tokens"].toVariant().toULongLong());
    if (cacheHit > 0 || cacheMiss > 0) {
        cached = cacheHit;
        input  = cacheHit + cacheMiss;
    }

    const quint64 cacheRead     = static_cast<quint64>(usage["cache_read_input_tokens"].toVariant().toULongLong());
    const quint64 cacheCreation = static_cast<quint64>(usage["cache_creation_input_tokens"].toVariant().toULongLong());
    if (cacheRead > 0 || cacheCreation > 0) {
        cached  = cacheRead;
        input  += cacheRead + cacheCreation;
    }

    if (input == 0 && cached == 0) return;
    if (input < cached) input = cached;

    m_lastInputTokens    = input;
    m_lastCachedTokens   = cached;
    m_totalInputTokens  += input;
    m_totalCachedTokens += cached;
    ++m_cacheSampleCount;

    const double lastRate  = input > 0 ? 100.0 * static_cast<double>(cached) / static_cast<double>(input) : 0.0;
    const double totalRate = m_totalInputTokens > 0 ? 100.0 * static_cast<double>(m_totalCachedTokens)
                                                          / static_cast<double>(m_totalInputTokens)
                                                    : 0.0;
    info(
        "API 缓存: 本次 {}/{} tokens ({:.1f}%), 累计 {}/{} ({:.1f}%), 样本 {}",
        cached,
        input,
        lastRate,
        m_totalCachedTokens,
        m_totalInputTokens,
        totalRate,
        m_cacheSampleCount
    );
    emit apiCacheStatsChanged();
}

void ChatBot::parseMarkdownAsync(const QString& markdown, const QString& requestId) {
    if (requestId.isEmpty()) return;
    {
        QMutexLocker lock(&m_markdownRequestsMutex);
        m_markdownRequests.insert(requestId);
    }

    QPointer<ChatBot> guard(this);
    m_markdownPool.start(
        makeMarkdownTask([guard, markdown, requestId]() {
            if (!guard) return;
            {
                QMutexLocker lock(&guard->m_markdownRequestsMutex);
                if (!guard->m_markdownRequests.contains(requestId)) return;
            }

            const QString blocksJson =
                QString::fromUtf8(QJsonDocument(MarkdownParser::parse(markdown)).toJson(QJsonDocument::Compact));
            if (!guard) return;
            {
                QMutexLocker lock(&guard->m_markdownRequestsMutex);
                if (!guard->m_markdownRequests.remove(requestId)) return;
            }
            QMetaObject::invokeMethod(
                guard.data(),
                [guard, requestId, blocksJson]() {
                    if (guard) emit guard->markdownParsed(requestId, blocksJson);
                },
                Qt::QueuedConnection
            );
        }),
        1
    );
}

void ChatBot::cancelMarkdownParse(const QString& requestId) {
    if (requestId.isEmpty()) return;
    QMutexLocker lock(&m_markdownRequestsMutex);
    m_markdownRequests.remove(requestId);
}

// -----------------------------------------------------------------------
// 构造函数
// -----------------------------------------------------------------------

ChatBot::ChatBot()
: Logger("ChatBot"),
  m_networkManager(new QNetworkAccessManager(this)),
  m_apiKey(""),
  m_apiEndpoint("https://api.deepseek.com/v1/chat/completions"),
  m_model("deepseek-v4-flash"),
  m_temperature(0.7),
  m_defaultPrompt("你是一个有用的助手，使用中文回复用户的问题。"),
  m_isStreaming(true),
  m_extraParams(json::object()),
  m_currentStreamBuffer(""),
  m_responseBuffer("") {
    m_markdownPool.setMaxThreadCount(1);
    m_markdownPool.setExpiryTimeout(10000);
    info("ChatBot 初始化完成");

    auto& config = mod::Config::getInstance();
    json  aiCfg  = config.read("ai");
    if (!aiCfg.is_null()) {
        m_isStreaming            = aiCfg.value("streaming", true);
        const QString renderMode = QString::fromStdString(aiCfg.value("bubble_render_mode", std::string("full")));
        if (renderMode == "full" || renderMode == "basic" || renderMode == "plain") m_bubbleRenderMode = renderMode;
    }

    connect(&Event::getInstance(), &Event::beforeUiInitialization, [this](QQuickView& view, QQmlContext* context) {
        context->setContextProperty("chatbot", this);
    });

    initModels();
    initPrompts();
    initTavily();
    initShellTool();
    initMathRender();
    initSessions();
}

ChatBot::~ChatBot() {
    // 清理所有正在执行的 shell 命令
    for (auto it = m_activeShellExecs.constBegin(); it != m_activeShellExecs.constEnd(); ++it) {
        auto* e = it.value();
        if (e->timer) {
            e->timer->stop();
            e->timer->deleteLater();
        }
        if (e->process) {
            e->process->disconnect(this);
            if (e->process->state() != QProcess::NotRunning) {
                e->process->kill();
                e->process->waitForFinished(200);
            }
            e->process->deleteLater();
        }
        delete e;
    }
    m_activeShellExecs.clear();
}

// -----------------------------------------------------------------------
// 配置重载
// -----------------------------------------------------------------------

void ChatBot::reloadConfig() {
    auto& config = mod::Config::getInstance();
    config.reload();

    json aiCfg = config.read("ai");
    if (!aiCfg.is_null()) {
        bool old      = m_isStreaming;
        m_isStreaming = aiCfg.value("streaming", true);
        if (old != m_isStreaming) emit isStreamingChanged();

        const QString renderMode = QString::fromStdString(aiCfg.value("bubble_render_mode", std::string("full")));
        const QString normalizedMode =
            (renderMode == "full" || renderMode == "basic" || renderMode == "plain") ? renderMode : "full";
        if (m_bubbleRenderMode != normalizedMode) {
            m_bubbleRenderMode = normalizedMode;
            emit bubbleRenderModeChanged();
        }
    }

    initModels();
    initPrompts();
    initTavily();
    initShellTool();
    initMathRender();

    emit apiKeyChanged();
    emit apiEndpointChanged();
    emit modelChanged();
    emit temperatureChanged();
    emit defaultPromptChanged();
    emit modelsChanged();
    emit promptsChanged();
    info("配置已重载");
    showToast("配置已重载");
}

void ChatBot::sanitizeConfig() {
    auto& config = mod::Config::getInstance();
    config.sanitize();
    config.reload();

    json aiCfg = config.read("ai");
    if (!aiCfg.is_null()) {
        bool old      = m_isStreaming;
        m_isStreaming = aiCfg.value("streaming", true);
        if (old != m_isStreaming) emit isStreamingChanged();
    }

    initModels();
    initPrompts();
    initTavily();
    initShellTool();
    initMathRender();

    emit apiKeyChanged();
    emit apiEndpointChanged();
    emit modelChanged();
    emit temperatureChanged();
    emit defaultPromptChanged();
    emit modelsChanged();
    emit promptsChanged();
    info("配置已清洗");
    showToast("配置已清洗");
}

// -----------------------------------------------------------------------
// 消息序列化（OpenAI 格式）
// -----------------------------------------------------------------------

QJsonObject ChatBot::messageToJson(const MessageData& msg) const {
    QJsonObject obj;
    obj["role"] = msg.role;

    if (msg.role == "tool") {
        obj["tool_call_id"] = msg.toolCallId;
        obj["content"]      = msg.content;
        return obj;
    }

    if (!msg.toolCallsJson.isEmpty()) {
        QJsonDocument tcDoc = QJsonDocument::fromJson(msg.toolCallsJson.toUtf8());
        if (tcDoc.isArray()) obj["tool_calls"] = tcDoc.array();
    }

    if (msg.isMultimodal() && msg.role == "user") {
        QJsonArray contentArr;
        for (const auto& part : msg.parts) {
            if (part.type == "file") continue;
            QJsonObject p;
            p["type"] = part.type;
            if (part.type == "text") {
                p["text"] = part.text;
            } else if (part.type == "image_url") {
                QJsonObject imgUrl;
                imgUrl["url"]  = AttachmentManager::requestUrl(part);
                p["image_url"] = imgUrl;
            } else if (part.type == "input_audio") {
                QJsonObject audio;
                audio["data"]    = part.data;
                audio["format"]  = part.format;
                p["input_audio"] = audio;
            }
            contentArr.append(p);
        }
        obj["content"] = contentArr.isEmpty() ? QJsonValue(msg.content) : QJsonValue(contentArr);
    } else {
        obj["content"] = msg.content;
    }

    return obj;
}

// -----------------------------------------------------------------------
// 构建 API messages 数组
// -----------------------------------------------------------------------

namespace {
void normalizeToolMessageSequence(QJsonArray& messages);
} // namespace

QJsonArray ChatBot::buildApiMessages(
    const QVector<MessageData>& history,
    const QString&              userText,
    const QVector<MessagePart>& userParts
) {
    QJsonArray messages;

    // system prompt
    QJsonObject sysMsg;
    sysMsg["role"]    = "system";
    sysMsg["content"] = m_defaultPrompt;
    messages.append(sysMsg);

    // history
    for (const auto& msg : history) {
        messages.append(messageToJson(msg));
    }

    // new user message
    MessageData userMsg;
    userMsg.role  = "user";
    userMsg.parts = userParts;
    if (userParts.isEmpty()) {
        userMsg.content = userText;
    } else {
        // prepend text part if userText is non-empty
        if (!userText.isEmpty()) {
            MessagePart textPart;
            textPart.type = "text";
            textPart.text = userText;
            userMsg.parts.prepend(textPart);
        }
    }
    messages.append(messageToJson(userMsg));

    normalizeToolMessageSequence(messages);

    return messages;
}

QJsonArray ChatBot::buildApiMessagesFromHistory() {
    QJsonArray  apiMessages;
    QJsonObject sysMsg;
    sysMsg["role"]    = "system";
    sysMsg["content"] = m_defaultPrompt;
    apiMessages.append(sysMsg);
    for (const auto& msg : currentMessages()) {
        apiMessages.append(messageToJson(msg));
    }
    normalizeToolMessageSequence(apiMessages);
    return apiMessages;
}

// -----------------------------------------------------------------------
// 会话辅助
// -----------------------------------------------------------------------

QVector<MessageData>&       ChatBot::currentMessages() { return m_sessions[m_currentSessionId].messages; }
const QVector<MessageData>& ChatBot::currentMessages() const {
    auto it = m_sessions.find(m_currentSessionId);
    Q_ASSERT(it != m_sessions.end());
    return it->messages;
}

void ChatBot::ensureCurrentSession() {
    if (m_currentSessionId.isEmpty() || !m_sessions.contains(m_currentSessionId)) {
        if (m_sessions.isEmpty()) {
            createSession("新对话");
        } else {
            m_currentSessionId = m_sessions.firstKey();
        }
    }
}

QString ChatBot::sessionsFilePath() {
    if (m_sessionsPath.isEmpty()) {
        m_sessionsPath =
            QString::fromStdString((mod::util::getModuleFileInfo().absolutePath() + "sessions.json").toStdString());
    }
    return m_sessionsPath;
}

// -----------------------------------------------------------------------
// 序列化 / 反序列化 MessageData ↔ JSON（sessions.json）
// -----------------------------------------------------------------------

static json messageDataToJson(const MessageData& msg) {
    json obj;
    obj["role"]    = msg.role.toStdString();
    obj["content"] = msg.content.toStdString();
    if (!msg.toolCallId.isEmpty()) obj["toolCallId"] = msg.toolCallId.toStdString();
    if (!msg.toolCallsJson.isEmpty()) obj["toolCallsJson"] = msg.toolCallsJson.toStdString();
    if (!msg.reasoning.isEmpty()) obj["reasoning"] = msg.reasoning.toStdString();

    if (msg.isMultimodal()) {
        json partsArr = json::array();
        for (const auto& part : msg.parts) {
            if (part.type == "image_url" && part.url.startsWith("data:", Qt::CaseInsensitive)) continue;

            json p;
            p["type"]      = part.type.toStdString();
            p["text"]      = part.text.toStdString();
            p["url"]       = part.url.toStdString();
            p["data"]      = part.data.toStdString();
            p["format"]    = part.format.toStdString();
            p["localPath"] = part.localPath.toStdString();
            p["name"]      = part.name.toStdString();
            p["mimeType"]  = part.mimeType.toStdString();
            p["language"]  = part.language.toStdString();
            p["size"]      = part.size;
            partsArr.push_back(p);
        }
        if (!partsArr.empty()) obj["parts"] = partsArr;
    }
    return obj;
}

static MessageData messageDataFromJson(const json& obj) {
    MessageData msg;
    msg.role    = QString::fromStdString(obj.value("role", ""));
    msg.content = QString::fromStdString(obj.value("content", ""));
    if (obj.contains("toolCallId")) msg.toolCallId = QString::fromStdString(obj["toolCallId"]);
    if (obj.contains("toolCallsJson")) msg.toolCallsJson = QString::fromStdString(obj["toolCallsJson"]);
    if (obj.contains("reasoning")) msg.reasoning = QString::fromStdString(obj["reasoning"]);

    if (obj.contains("parts") && obj["parts"].is_array()) {
        for (const auto& p : obj["parts"]) {
            MessagePart part;
            part.type      = QString::fromStdString(p.value("type", "text"));
            part.text      = QString::fromStdString(p.value("text", ""));
            part.url       = QString::fromStdString(p.value("url", ""));
            part.data      = QString::fromStdString(p.value("data", ""));
            part.format    = QString::fromStdString(p.value("format", ""));
            part.localPath = QString::fromStdString(p.value("localPath", ""));
            part.name      = QString::fromStdString(p.value("name", ""));
            part.mimeType  = QString::fromStdString(p.value("mimeType", ""));
            part.language  = QString::fromStdString(p.value("language", ""));
            part.size      = p.value("size", 0LL);
            msg.parts.append(part);
        }
        if (msg.content.isEmpty()) {
            for (const auto& part : msg.parts) {
                if (part.type == "text" && !part.text.isEmpty()) {
                    msg.content = part.text;
                    break;
                }
            }
        }
    }
    return msg;
}

// 工具调用被取消或未完成时的占位结果
static QString pendingToolResultText() {
    return QStringLiteral("该工具调用已被用户取消或未完成，请直接根据已有信息继续回答，不要重复调用该工具。");
}

static bool sanitizeMessageHistory(MessageData& msg) {
    bool removedImage = false;
    msg.parts.erase(
        std::remove_if(
            msg.parts.begin(),
            msg.parts.end(),
            [&removedImage](const MessagePart& part) {
                if (part.type != "image_url" || !part.url.startsWith("data:", Qt::CaseInsensitive)) return false;
                removedImage = true;
                return true;
            }
        ),
        msg.parts.end()
    );

    if (!removedImage) return false;
    if (!msg.content.isEmpty()) return true;

    for (const auto& part : msg.parts) {
        if (part.type == "text" && !part.text.isEmpty()) {
            msg.content = part.text;
            return true;
        }
    }
    msg.content = "[图片]";
    return true;
}

// 修复历史里损坏的工具调用组
static bool normalizeHistoryMessages(QVector<MessageData>& messages, int* droppedOrphans = nullptr) {
    QVector<MessageData> result;
    bool                 changed = false;

    for (int index = 0; index < messages.size(); ++index) {
        const MessageData& message = messages.at(index);

        if (message.role == "tool") {
            if (droppedOrphans) ++(*droppedOrphans);
            changed = true;
            continue;
        }

        if (message.role != "assistant" || message.toolCallsJson.isEmpty()) {
            result.append(message);
            continue;
        }

        QVector<MessageData> following;
        int                  next = index + 1;
        while (next < messages.size() && messages.at(next).role == "tool") {
            following.append(messages.at(next));
            ++next;
        }

        const QJsonArray     calls = QJsonDocument::fromJson(message.toolCallsJson.toUtf8()).array();
        QJsonArray           validCalls;
        QSet<QString>        seenIds;
        QVector<MessageData> resolved;

        for (const QJsonValue& callValue : calls) {
            const QJsonObject call = callValue.toObject();
            const QString     id   = call.value(QLatin1String("id")).toString().trimmed();
            if (id.isEmpty() || seenIds.contains(id)) {
                changed = true;
                continue;
            }
            seenIds.insert(id);
            validCalls.append(call);

            bool answered = false;
            for (const auto& candidate : following) {
                if (candidate.toolCallId != id) continue;
                resolved.append(candidate);
                answered = true;
                break;
            }
            if (answered) continue;

            MessageData placeholder;
            placeholder.role       = "tool";
            placeholder.toolCallId = id;
            placeholder.content    = pendingToolResultText();
            resolved.append(placeholder);
            changed = true;
        }
        if (resolved.size() != following.size()) changed = true;

        MessageData assistant = message;
        if (validCalls.isEmpty()) {
            assistant.toolCallsJson.clear();
            changed = true;
            if (assistant.content.isEmpty() && assistant.parts.isEmpty()) {
                index = next - 1;
                continue;
            }
        } else {
            const QString normalized = QString::fromUtf8(QJsonDocument(validCalls).toJson(QJsonDocument::Compact));
            if (normalized != assistant.toolCallsJson) changed = true;
            assistant.toolCallsJson = normalized;
        }

        result.append(assistant);
        result += resolved;
        index = next - 1;
    }

    messages = result;
    return changed;
}

// -----------------------------------------------------------------------
// 保存 sessions.json
// -----------------------------------------------------------------------

void ChatBot::saveSessions() {
    json root;
    root["activeSessionId"] = m_currentSessionId.toStdString();
    json sessionsArr        = json::array();

    for (auto it = m_sessions.constBegin(); it != m_sessions.constEnd(); ++it) {
        const SessionData& session = it.value();
        json               sessionObj;
        sessionObj["id"]        = session.id.toStdString();
        sessionObj["title"]     = session.title.toStdString();
        sessionObj["createdAt"] = session.createdAt.toStdString();
        sessionObj["updatedAt"] = session.updatedAt.toStdString();

        json messagesArr = json::array();
        for (const auto& msg : session.messages) {
            messagesArr.push_back(messageDataToJson(msg));
        }
        sessionObj["messages"] = messagesArr;
        sessionsArr.push_back(sessionObj);
    }
    root["sessions"] = sessionsArr;

    QFile file(sessionsFilePath());
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        const std::string payload = root.dump();
        file.write(payload.c_str(), static_cast<qint64>(payload.size()));
        file.close();
    } else {
        error("无法写入 sessions.json: {}", sessionsFilePath().toStdString());
    }
}

// -----------------------------------------------------------------------
// 加载 sessions.json
// -----------------------------------------------------------------------

void ChatBot::initSessions() {
    bool  historySanitized = false;
    auto  path             = sessionsFilePath();
    QFile file(path);
    if (file.exists() && file.open(QIODevice::ReadOnly)) {
        try {
            json root = json::parse(file.readAll().toStdString());
            file.close();

            if (root.contains("sessions") && root["sessions"].is_array()) {
                for (const auto& sessionObj : root["sessions"]) {
                    SessionData session;
                    session.id        = QString::fromStdString(sessionObj.value("id", ""));
                    session.title     = QString::fromStdString(sessionObj.value("title", "未命名对话"));
                    session.createdAt = QString::fromStdString(sessionObj.value("createdAt", ""));
                    session.updatedAt = QString::fromStdString(sessionObj.value("updatedAt", ""));

                    if (sessionObj.contains("messages") && sessionObj["messages"].is_array()) {
                        for (const auto& msgObj : sessionObj["messages"]) {
                            // 兼容旧格式 {role, content}
                            if (msgObj.contains("role") && !msgObj.contains("parts")) {
                                MessageData msg;
                                msg.role    = QString::fromStdString(msgObj.value("role", ""));
                                msg.content = QString::fromStdString(msgObj.value("content", ""));
                                if (msgObj.contains("toolCallId"))
                                    msg.toolCallId = QString::fromStdString(msgObj["toolCallId"]);
                                if (msgObj.contains("toolCallsJson"))
                                    msg.toolCallsJson = QString::fromStdString(msgObj["toolCallsJson"]);
                                if (msgObj.contains("reasoning"))
                                    msg.reasoning = QString::fromStdString(msgObj["reasoning"]);
                                session.messages.append(msg);
                            } else {
                                MessageData msg   = messageDataFromJson(msgObj);
                                historySanitized |= sanitizeMessageHistory(msg);
                                session.messages.append(msg);
                            }
                        }
                    }

                    const auto oldSize = session.messages.size();
                    session.messages.erase(
                        std::remove_if(
                            session.messages.begin(),
                            session.messages.end(),
                            [](const MessageData& message) {
                                return message.role == "system"
                                    && (message.content.startsWith("用户引用了以下文件作为上下文：")
                                        || message.content.startsWith("用户引用了以下文件作为代码审查/分析的上下文："));
                            }
                        ),
                        session.messages.end()
                    );
                    historySanitized |= session.messages.size() != oldSize;
                    // 修复历史里损坏的工具调用组
                    int droppedOrphans = 0;
                    historySanitized |= normalizeHistoryMessages(session.messages, &droppedOrphans);
                    if (droppedOrphans > 0)
                        warn("会话 {} 丢弃了 {} 条找不到宿主的工具结果", session.id.toStdString(), droppedOrphans);
                    if (!session.id.isEmpty()) m_sessions.insert(session.id, session);
                }
            }

            if (root.contains("activeSessionId")) {
                QString activeId = QString::fromStdString(root["activeSessionId"]);
                if (m_sessions.contains(activeId)) m_currentSessionId = activeId;
            }

            info("已加载 {} 个会话", m_sessions.size());
        } catch (const std::exception& e) {
            warn("sessions.json 解析失败: {}, 使用默认配置", e.what());
        }
    }

    ensureCurrentSession();
    if (!m_sessions.contains(m_currentSessionId)) {
        m_currentSessionId = createSession("新对话");
    }
    if (historySanitized) saveSessions();
}

bool ChatBot::isAvailable() { return !m_apiKey.isEmpty(); }

namespace {

constexpr qint64 MAX_FILE_CONTEXT_BYTES = 1024 * 1024;

QString buildFileContext(const QVector<MessagePart>& parts, QStringList* failedFiles = nullptr) {
    QString context;
    qint64  totalBytes = 0;
    for (const auto& part : parts) {
        if (part.type != "file") continue;

        const QFileInfo info(part.localPath);
        if (!info.isAbsolute() || !info.isFile() || !info.isReadable() || info.size() > MAX_FILE_CONTEXT_BYTES
            || totalBytes + info.size() > MAX_FILE_CONTEXT_BYTES) {
            if (failedFiles) failedFiles->append(part.name.isEmpty() ? info.fileName() : part.name);
            continue;
        }

        QFile file(info.absoluteFilePath());
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            if (failedFiles) failedFiles->append(part.name.isEmpty() ? info.fileName() : part.name);
            continue;
        }
        const QByteArray data = file.read(MAX_FILE_CONTEXT_BYTES + 1);
        if (data.size() > MAX_FILE_CONTEXT_BYTES || totalBytes + data.size() > MAX_FILE_CONTEXT_BYTES) {
            if (failedFiles) failedFiles->append(part.name.isEmpty() ? info.fileName() : part.name);
            continue;
        }
        totalBytes += data.size();
        context    += QString("---\n## 文件: %1\n```%2\n%3\n```\n\n")
                          .arg(info.absoluteFilePath(), part.language, QString::fromUtf8(data));
    }
    return context.isEmpty() ? QString() : "用户引用了以下文件作为上下文：\n\n" + context;
}

} // namespace

// -----------------------------------------------------------------------
// sendMessage（纯文本，兼容旧接口）
// -----------------------------------------------------------------------

void ChatBot::sendMessage(const QString& message, const QString& fileRefs) {
    QVector<MessagePart> extraParts;
    QString              requestFileContext;

    // 文件引用转文本 part（保持原有行为）
    if (!fileRefs.isEmpty()) {
        QJsonDocument doc = QJsonDocument::fromJson(fileRefs.toUtf8());
        if (doc.isArray() && !doc.array().isEmpty()) {
            for (const auto& fileVal : doc.array()) {
                QJsonObject file = fileVal.toObject();
                QString     path = file["path"].toString();
                QString     lang = file["language"].toString();

                MessagePart part;
                part.type      = "file";
                part.localPath = path;
                part.name      = file["name"].toString(QFileInfo(path).fileName());
                part.mimeType  = "text/plain";
                part.language  = lang;
                part.size      = QFileInfo(path).size();
                extraParts.append(part);
            }
            QStringList failedFiles;
            requestFileContext = buildFileContext(extraParts, &failedFiles);
            if (!failedFiles.isEmpty()) {
                warn("附件未发送给模型: {}", failedFiles.join("、").toStdString());
                showToast(QString("%1 个附件未发送，请检查权限或大小").arg(failedFiles.size()).toStdString());
            }
            debug("已附加 {} 个文件，{} 个读取失败", doc.array().size(), failedFiles.size());
        }
    }

    QVector<MessageData> requestHistory = currentMessages();
    if (!requestFileContext.isEmpty()) {
        MessageData contextMessage;
        contextMessage.role    = "system";
        contextMessage.content = requestFileContext;
        requestHistory.append(contextMessage);
    }
    QJsonArray apiMessages = buildApiMessages(requestHistory, message);

    // 记录用户消息
    MessageData userMsg;
    userMsg.role    = "user";
    userMsg.content = message;
    userMsg.parts   = extraParts;
    if (!message.isEmpty() && !extraParts.isEmpty()) {
        MessagePart textPart;
        textPart.type = "text";
        textPart.text = message;
        userMsg.parts.prepend(textPart);
    }
    currentMessages().append(userMsg);
    if (currentMessages().size() > MAX_HISTORY_SIZE) currentMessages().removeFirst();

    m_sessions[m_currentSessionId].updatedAt = QDateTime::currentDateTime().toString(Qt::ISODate);
    saveSessions();
    emit messagesChanged();

    makeApiRequest(apiMessages);
}

// -----------------------------------------------------------------------
// sendMessageWithMedia（多模态：图片 / 音频）
// mediaParts: 图片 {type,url} / 音频 {type,data,format}
// -----------------------------------------------------------------------

void ChatBot::sendMessageWithMedia(const QString& message, const QString& mediaParts) {
    m_cancelled = false;

    static constexpr int MAX_MEDIA_JSON_SIZE = 12 * 1024 * 1024;
    if (mediaParts.size() > MAX_MEDIA_JSON_SIZE) {
        emit errorOccurred("图片数据过大，请裁剪或减少拼接范围后重试");
        return;
    }

    QVector<MessagePart> parts;

    QJsonDocument doc = QJsonDocument::fromJson(mediaParts.toUtf8());
    if (doc.isArray()) {
        for (const auto& val : doc.array()) {
            QJsonObject obj = val.toObject();
            MessagePart part;
            part.type = obj["type"].toString();
            if (part.type == "image_url") {
                part.url = obj["url"].toString();
                if (part.url.isEmpty()) continue;
            } else if (part.type == "input_audio") {
                part.data   = obj["data"].toString();
                part.format = obj["format"].toString("mp3");
                if (part.data.isEmpty()) continue;
            } else if (part.type == "text") {
                part.text = obj["text"].toString();
                if (part.text.isEmpty()) continue;
            } else if (part.type == "file") {
                part.localPath = obj["localPath"].toString();
                part.name      = obj["name"].toString(QFileInfo(part.localPath).fileName());
                part.mimeType  = obj["mimeType"].toString("text/plain");
                part.language  = obj["language"].toString();
                part.size      = static_cast<qint64>(obj["size"].toDouble(QFileInfo(part.localPath).size()));
                if (part.localPath.isEmpty()) continue;
            } else {
                continue;
            }
            parts.append(part);
        }
    }

    QStringList   failedFiles;
    const QString fileContext = buildFileContext(parts, &failedFiles);
    if (!fileContext.isEmpty()) {
        MessagePart contextPart;
        contextPart.type = "text";
        contextPart.text = fileContext;
        parts.prepend(contextPart);
    }
    if (!failedFiles.isEmpty()) {
        warn("附件未发送给模型: {}", failedFiles.join("、").toStdString());
        showToast(QString("%1 个附件未发送，请检查权限或大小").arg(failedFiles.size()).toStdString());
    }

    bool hasImageParts = false;
    for (const auto& p : parts) {
        if (p.type == "image_url") {
            hasImageParts = true;
            break;
        }
    }

    if (!m_capVision && hasImageParts) {
        if (!m_proxyVisionModelId.isEmpty()) {
            const int     requestSeq = ++m_requestSeq;
            const QString sessionId  = m_currentSessionId;

            abortActiveReplies();
            m_cancelled = false;
            emit proxyVisionStarted();
            callVisionProxy(message, parts, sessionId, requestSeq);
            return;
        } else {
            showToast("当前模型不支持视觉，已仅发送文字");
            parts.erase(
                std::remove_if(parts.begin(), parts.end(), [](const MessagePart& p) { return p.type == "image_url"; }),
                parts.end()
            );
        }
    }

    finishMediaMessage(message, parts, message, m_currentSessionId);
}

void ChatBot::finishMediaMessage(
    const QString&              message,
    const QVector<MessagePart>& parts,
    const QString&              effectiveMessage,
    const QString&              sessionId
) {
    if (m_cancelled || sessionId != m_currentSessionId || !m_sessions.contains(sessionId)) {
        warn("多模态消息已失效，会话已切换或请求已取消");
        return;
    }
    if (effectiveMessage.trimmed().isEmpty() && parts.isEmpty()) {
        emit errorOccurred("没有可发送的文字或媒体内容");
        return;
    }

    QJsonArray apiMessages = buildApiMessages(m_sessions[sessionId].messages, effectiveMessage, parts);

    // 内嵌图片先落盘为附件，不写入历史
    const bool hadImage =
        std::any_of(parts.cbegin(), parts.cend(), [](const MessagePart& part) { return part.type == "image_url"; });
    QVector<MessagePart> historyParts   = parts;
    const QString        attachmentRoot = QFileInfo(sessionsFilePath()).absolutePath();
    for (auto& part : historyParts) {
        if (part.type != "image_url" || !part.url.startsWith("data:", Qt::CaseInsensitive)) continue;
        QString attachmentError;
        if (!AttachmentManager::persistImage(attachmentRoot, sessionId, part, &attachmentError)) {
            warn("保存图片附件失败: {}", attachmentError.toStdString());
            part.localPath.clear();
        }
        part.url.clear();
    }
    historyParts.erase(
        std::remove_if(
            historyParts.begin(),
            historyParts.end(),
            [](const MessagePart& part) {
                return part.type == "image_url" && part.url.isEmpty() && part.localPath.isEmpty();
            }
        ),
        historyParts.end()
    );

    MessageData userMsg;
    userMsg.role    = "user";
    userMsg.content = message.isEmpty() && hadImage ? "[图片]" : message;
    userMsg.parts   = historyParts;
    if (!effectiveMessage.isEmpty()) {
        if (historyParts.isEmpty()) {
            userMsg.content = effectiveMessage;
        } else {
            MessagePart textPart;
            textPart.type = "text";
            textPart.text = effectiveMessage;
            userMsg.parts.prepend(textPart);
        }
    }
    auto& messages = m_sessions[sessionId].messages;
    messages.append(userMsg);
    if (messages.size() > MAX_HISTORY_SIZE) messages.removeFirst();

    m_sessions[sessionId].updatedAt = QDateTime::currentDateTime().toString(Qt::ISODate);
    saveSessions();
    emit messagesChanged();

    makeApiRequest(apiMessages);
}

// -----------------------------------------------------------------------
// callVisionProxy: 用视觉代理模型异步提取图片文字描述
// -----------------------------------------------------------------------

void ChatBot::callVisionProxy(
    const QString&              message,
    const QVector<MessagePart>& parts,
    const QString&              sessionId,
    int                         requestSeq
) {
    std::string proxyId = m_proxyVisionModelId.toStdString();

    json proxyModel;
    if (m_modelsData.contains("models") && m_modelsData["models"].is_array()) {
        for (const auto& model : m_modelsData["models"]) {
            if (model.is_object() && model.value("id", "") == proxyId) {
                proxyModel = model;
                break;
            }
        }
    }
    if (proxyModel.empty() || !proxyModel.contains("endpoint") || !proxyModel["endpoint"].is_string()
        || !proxyModel.contains("modelId") || !proxyModel["modelId"].is_string()
        || (proxyModel.contains("apiKey") && !proxyModel["apiKey"].is_string())) {
        warn("代理视觉模型未找到: {}", proxyId);
        QVector<MessagePart> fallbackParts = parts;
        fallbackParts.erase(
            std::remove_if(
                fallbackParts.begin(),
                fallbackParts.end(),
                [](const MessagePart& p) { return p.type == "image_url"; }
            ),
            fallbackParts.end()
        );
        showToast("视觉代理模型配置无效，已仅发送文字");
        emit proxyVisionCompleted(QString());
        finishMediaMessage(message, fallbackParts, "[图片分析失败]\n" + message, sessionId);
        return;
    }

    QString proxyEndpoint = QString::fromStdString(proxyModel.value("endpoint", ""));
    QString proxyApiKey   = QString::fromStdString(proxyModel.value("apiKey", ""));
    QString proxyModelId  = QString::fromStdString(proxyModel.value("modelId", ""));

    bool isAnthropic = proxyEndpoint.contains("/messages");

    // 构造请求体
    QJsonArray  messages;
    QJsonObject userMsg;
    userMsg["role"] = "user";

    if (isAnthropic) {
        QJsonArray contentArray;
        for (const auto& p : parts) {
            if (p.type == "image_url") {
                QString base64Data = p.url;
                QString mediaType  = "image/jpeg";
                if (base64Data.startsWith("data:image/jpeg;base64,")) base64Data = base64Data.mid(23);
                else if (base64Data.startsWith("data:image/png;base64,")) {
                    base64Data = base64Data.mid(22);
                    mediaType  = "image/png";
                } else {
                    warn("Anthropic 视觉代理仅支持 data URL 图片");
                    continue;
                }

                QJsonObject imgObj;
                imgObj["type"] = "image";
                QJsonObject source;
                source["type"]       = "base64";
                source["media_type"] = mediaType;
                source["data"]       = base64Data;
                imgObj["source"]     = source;
                contentArray.append(imgObj);
            }
        }
        QJsonObject textObj;
        textObj["type"] = "text";
        textObj["text"] = m_proxyVisionPrompt;
        contentArray.append(textObj);
        userMsg["content"] = contentArray;
    } else {
        QJsonArray contentArray;
        for (const auto& p : parts) {
            if (p.type == "image_url") {
                QJsonObject imgObj;
                imgObj["type"] = "image_url";
                QJsonObject imgUrl;
                imgUrl["url"]       = p.url;
                imgObj["image_url"] = imgUrl;
                contentArray.append(imgObj);
            }
        }
        QJsonObject textObj;
        textObj["type"] = "text";
        textObj["text"] = m_proxyVisionPrompt;
        contentArray.append(textObj);
        userMsg["content"] = contentArray;
    }
    messages.append(userMsg);

    QJsonObject requestBody;
    requestBody["model"]    = proxyModelId;
    requestBody["messages"] = messages;
    requestBody["stream"]   = false;
    if (isAnthropic) requestBody["max_tokens"] = 4096;
    if (proxyModel.contains("temperature") && proxyModel["temperature"].is_number())
        requestBody["temperature"] = proxyModel["temperature"].get<double>();

    QJsonDocument jsonDoc(requestBody);
    QByteArray    data = jsonDoc.toJson();

    QUrl            proxyUrl(proxyEndpoint);
    QNetworkRequest request(proxyUrl);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setTransferTimeout(30000);
    if (isAnthropic) {
        request.setRawHeader("x-api-key", proxyApiKey.toUtf8());
        request.setRawHeader("anthropic-version", "2023-06-01");
    } else if (!proxyApiKey.isEmpty()) {
        request.setRawHeader("Authorization", ("Bearer " + proxyApiKey).toUtf8());
    }

    info(
        "调用视觉代理模型 {} ({} {}): {}",
        proxyId,
        isAnthropic ? "Anthropic" : "OpenAI",
        proxyEndpoint.toStdString(),
        m_proxyVisionPrompt.left(30).toStdString()
    );

    QNetworkReply* reply = m_networkManager->post(request, data);
    m_activeReplies.append(reply);

    connect(reply, &QNetworkReply::finished, this, [this, reply, requestSeq, sessionId, message, parts, isAnthropic]() {
        m_activeReplies.removeAll(reply);

        if (requestSeq != m_requestSeq || m_cancelled) {
            reply->deleteLater();
            return;
        }

        QByteArray responseData = reply->readAll();
        QString    description;
        QString    failureReason;

        if (reply->error() != QNetworkReply::NoError) {
            failureReason = reply->errorString();
        } else {
            QJsonParseError parseError;
            QJsonDocument   responseDoc = QJsonDocument::fromJson(responseData, &parseError);
            if (parseError.error != QJsonParseError::NoError || !responseDoc.isObject()) {
                failureReason = "响应不是有效的 JSON";
            } else {
                QJsonObject responseJson = responseDoc.object();
                if (responseJson["error"].isObject()) {
                    failureReason = responseJson["error"].toObject()["message"].toString("服务返回错误");
                } else if (isAnthropic) {
                    QJsonArray content = responseJson["content"].toArray();
                    for (const auto& item : content) {
                        QJsonObject block = item.toObject();
                        if (block["type"].toString() == "text") description += block["text"].toString();
                    }
                    if (description.isEmpty()) failureReason = "响应中没有文本内容";
                } else {
                    QJsonArray choices = responseJson["choices"].toArray();
                    if (choices.isEmpty()) {
                        failureReason = "响应中没有 choices";
                    } else {
                        QJsonValue content = choices.first().toObject()["message"].toObject()["content"];
                        if (content.isString()) description = content.toString();
                        else if (content.isArray()) {
                            for (const auto& item : content.toArray()) {
                                QJsonObject block = item.toObject();
                                if (block["type"].toString() == "text") description += block["text"].toString();
                            }
                        }
                        if (description.isEmpty()) failureReason = "响应中没有文本内容";
                    }
                }
            }
        }

        QVector<MessagePart> nextParts = parts;
        nextParts.erase(
            std::remove_if(
                nextParts.begin(),
                nextParts.end(),
                [](const MessagePart& p) { return p.type == "image_url"; }
            ),
            nextParts.end()
        );

        QString effectiveMessage;
        if (failureReason.isEmpty()) {
            description      = description.trimmed();
            effectiveMessage = "[图片描述]\n" + description + "\n\n[用户消息]\n" + message;
            info("视觉代理模型返回 {} 个字符", description.length());
            emit proxyVisionCompleted(description);
        } else {
            warn("视觉代理模型调用失败: {}", failureReason.toStdString());
            showToast("视觉代理模型调用失败，已仅发送文字");
            effectiveMessage = "[图片分析失败]\n" + message;
            emit proxyVisionCompleted(QString());
        }

        reply->deleteLater();
        finishMediaMessage(message, nextParts, effectiveMessage, sessionId);
    });
}

// -----------------------------------------------------------------------
// submitToolResult：将工具调用结果提交给模型
// -----------------------------------------------------------------------

void ChatBot::submitToolResult(const QString& toolCallId, const QString& toolName, const QString& result) {
    MessageData toolMsg;
    toolMsg.role       = "tool";
    toolMsg.toolCallId = toolCallId;
    toolMsg.content    = result;
    currentMessages().append(toolMsg);
    if (currentMessages().size() > MAX_HISTORY_SIZE) currentMessages().removeFirst();

    // 以当前历史重新发起请求
    QJsonArray apiMessages = buildApiMessagesFromHistory();

    m_sessions[m_currentSessionId].updatedAt = QDateTime::currentDateTime().toString(Qt::ISODate);
    saveSessions();
    emit messagesChanged();

    makeApiRequest(apiMessages);
}

void ChatBot::submitToolResultBatched(const QString& toolCallId, const QString& toolName, const QString& result) {
    for (auto& entry : m_toolCallBatch) {
        if (entry.id == toolCallId) {
            entry.resolved = true;
            entry.result   = result;
            break;
        }
    }
    tryFlushToolBatch();
}

void ChatBot::tryFlushToolBatch() {
    if (m_cancelled) return;
    if (m_toolCallBatch.isEmpty()) return;

    for (const auto& entry : m_toolCallBatch) {
        if (!entry.resolved) return;
    }

    // 所有 tool call 都已完成，按顺序提交结果
    for (const auto& entry : m_toolCallBatch) {
        MessageData toolMsg;
        toolMsg.role       = "tool";
        toolMsg.toolCallId = entry.id;
        toolMsg.content    = entry.result;
        currentMessages().append(toolMsg);
        if (currentMessages().size() > MAX_HISTORY_SIZE) currentMessages().removeFirst();
    }
    m_toolCallBatch.clear();

    QJsonArray apiMessages = buildApiMessagesFromHistory();

    m_sessions[m_currentSessionId].updatedAt = QDateTime::currentDateTime().toString(Qt::ISODate);
    saveSessions();
    emit messagesChanged();
    emit toolBatchFlushed();

    makeApiRequest(apiMessages);
}

// 把尚未落库的工具调用结果补进历史
void ChatBot::resolvePendingToolCalls(const QString& reason) {
    if (m_toolCallBatch.isEmpty()) return;

    bool appended = false;
    for (const auto& entry : m_toolCallBatch) {
        if (entry.id.isEmpty()) continue;
        MessageData toolMsg;
        toolMsg.role       = "tool";
        toolMsg.toolCallId = entry.id;
        toolMsg.content    = entry.resolved ? entry.result : reason;
        currentMessages().append(toolMsg);
        appended = true;
    }
    m_toolCallBatch.clear();
    if (!appended) return;

    if (currentMessages().size() > MAX_HISTORY_SIZE) currentMessages().removeFirst();
    m_sessions[m_currentSessionId].updatedAt = QDateTime::currentDateTime().toString(Qt::ISODate);
    saveSessions();
    emit messagesChanged();
}

// -----------------------------------------------------------------------
// Responses API conversion
// -----------------------------------------------------------------------

bool ChatBot::usesResponsesApi() const { return m_apiProtocol == "responses"; }

QJsonArray ChatBot::messagesToResponsesInput(const QJsonArray& messages) const {
    QJsonArray input;
    for (const QJsonValue& value : messages) {
        const QJsonObject message = value.toObject();
        const QString     role    = message["role"].toString();

        if (role == "tool") {
            input.append(
                QJsonObject{
                    {"type",    "function_call_output"            },
                    {"call_id", message["tool_call_id"].toString()},
                    {"output",  message["content"].toString()     }
            }
            );
            continue;
        }

        QJsonValue       content;
        const QJsonValue contentValue = message["content"];
        if (contentValue.isString()) {
            content = contentValue;
        } else if (contentValue.isArray()) {
            QJsonArray responsesContent;
            bool       hasUnsupportedMedia = false;
            for (const QJsonValue& partValue : contentValue.toArray()) {
                const QJsonObject part     = partValue.toObject();
                const QString     partType = part["type"].toString();
                if (partType == "text") {
                    const QString text = part["text"].toString();
                    if (!text.isEmpty())
                        responsesContent.append(
                            QJsonObject{
                                {"type", "input_text"},
                                {"text", text        }
                        }
                        );
                } else if (partType == "image_url") {
                    const QString imageUrl = part["image_url"].toObject()["url"].toString();
                    if (!imageUrl.isEmpty())
                        responsesContent.append(
                            QJsonObject{
                                {"type",      "input_image"},
                                {"image_url", imageUrl     }
                        }
                        );
                } else {
                    hasUnsupportedMedia = true;
                }
            }
            if (responsesContent.isEmpty() && hasUnsupportedMedia)
                responsesContent.append(
                    QJsonObject{
                        {"type", "input_text"        },
                        {"text", "[不支持的媒体输入]"}
                }
                );
            content = responsesContent;
        }

        if ((content.isString() && !content.toString().isEmpty())
            || (content.isArray() && !content.toArray().isEmpty()))
            input.append(
                QJsonObject{
                    {"type",    "message"},
                    {"role",    role     },
                    {"content", content  }
            }
            );

        if (message["tool_calls"].isArray()) {
            for (const QJsonValue& toolValue : message["tool_calls"].toArray()) {
                const QJsonObject tool     = toolValue.toObject();
                const QJsonObject function = tool["function"].toObject();
                input.append(
                    QJsonObject{
                        {"type",      "function_call"                 },
                        {"call_id",   tool["id"].toString()           },
                        {"name",      function["name"].toString()     },
                        {"arguments", function["arguments"].toString()}
                }
                );
            }
        }
    }
    return input;
}

namespace {

QString responseOutputText(const QJsonArray& output) {
    QString text;
    for (const QJsonValue& itemValue : output) {
        const QJsonObject item = itemValue.toObject();
        if (item["type"].toString() != "message") continue;
        for (const QJsonValue& contentValue : item["content"].toArray()) {
            const QJsonObject content = contentValue.toObject();
            if (content["type"].toString() == "output_text") text += content["text"].toString();
        }
    }
    return text;
}

QString responseOutputReasoning(const QJsonArray& output) {
    QString reasoning;
    for (const QJsonValue& itemValue : output) {
        const QJsonObject item = itemValue.toObject();
        if (item["type"].toString() != "reasoning") continue;

        // summary[] 为摘要，content[] 为原始思维链
        QString summary;
        for (const QJsonValue& summaryValue : item["summary"].toArray()) {
            const QJsonObject summaryObject = summaryValue.toObject();
            if (summaryObject["type"].toString() == "summary_text") summary += summaryObject["text"].toString();
        }

        QString content;
        for (const QJsonValue& contentValue : item["content"].toArray()) {
            const QJsonObject contentObject = contentValue.toObject();
            const QString     contentType   = contentObject["type"].toString();
            if (contentType == "reasoning_text" || contentType == "text" || contentType == "summary_text")
                content += contentObject["text"].toString();
        }

        // 两者都有时优先原始思维链
        reasoning += content.isEmpty() ? summary : content;
    }
    return reasoning;
}

QString responseOutputToolCalls(const QJsonArray& output) {
    QJsonArray toolCalls;
    for (const QJsonValue& itemValue : output) {
        const QJsonObject item = itemValue.toObject();
        if (item["type"].toString() != "function_call") continue;
        toolCalls.append(
            QJsonObject{
                {"id",       item["call_id"].toString()                                                     },
                {"type",     "function"                                                                     },
                {"function",
                 QJsonObject{{"name", item["name"].toString()}, {"arguments", item["arguments"].toString()}}}
        }
        );
    }
    return QString::fromUtf8(QJsonDocument(toolCalls).toJson(QJsonDocument::Compact));
}

} // namespace

QVector<MessagePart> ChatBot::persistGeneratedImages(const QJsonArray& output) {
    QVector<MessagePart> parts;
    const QString        rootPath = QFileInfo(sessionsFilePath()).absolutePath();
    for (const QJsonValue& itemValue : output) {
        const QJsonObject item = itemValue.toObject();
        if (item["type"].toString() != "image_generation_call") continue;

        const QString result = item["result"].toString();
        if (result.isEmpty()) continue;

        MessagePart part;
        part.type = "image_url";
        part.url  = "data:image/png;base64," + result;
        part.name = "生成图片.png";
        QString attachmentError;
        if (!AttachmentManager::persistImage(rootPath, m_currentSessionId, part, &attachmentError)) {
            warn("保存生成图片失败: {}", attachmentError.toStdString());
            continue;
        }
        part.url.clear();
        parts.append(part);
    }
    return parts;
}

QVariantList ChatBot::attachmentVariants(const QVector<MessagePart>& parts) const {
    QVariantList variants;
    for (const auto& part : parts) {
        QVariantMap attachment;
        attachment["type"]      = part.type;
        attachment["localPath"] = part.localPath;
        attachment["source"]    = QUrl::fromLocalFile(part.localPath).toString();
        attachment["name"]      = part.name;
        attachment["mimeType"]  = part.mimeType;
        attachment["size"]      = part.size;
        variants.append(attachment);
    }
    return variants;
}

// -----------------------------------------------------------------------
// makeApiRequest：构建请求体并发送
// -----------------------------------------------------------------------

void ChatBot::abortActiveReplies() {
    const QList<QNetworkReply*> replies = m_activeReplies;
    m_activeReplies.clear();

    for (QNetworkReply* reply : replies) {
        if (!reply) continue;
        reply->disconnect(this);
        if (!reply->isFinished()) reply->abort();
        reply->deleteLater();
    }
}

namespace {

struct EmbeddedContent {
    QString answer;
    QString reasoning;
};

struct ThinkTag {
    const char* open;
    const char* close;
};

const ThinkTag kThinkTags[] = {
    { "<thinking>",  "</thinking>" },
    { "<think>",     "</think>"    },
    { "<reasoning>", "</reasoning>"},
    { "<reasoning_scratchpad>", "</reasoning_scratchpad>" },
    { "<thought>",   "</thought>"  },
    { "<analysis>",  "</analysis>" },
};

struct ThinkAlias {
    const char* from;
    const char* to;
};

const ThinkAlias kThinkAliases[] = {
    { "<|thinking|>",  "<thinking>"  },
    { "<|/thinking|>", "</thinking>" },
    { "<|think|>",     "<think>"     },
    { "<|/think|>",    "</think>"    },
    { "<think >",      "<think>"     },
    { "<thinking >",   "<thinking>"  },
    { "\xe2\x97\x81think\xe2\x96\xb7",   "<think>"  },
    { "\xe2\x97\x81/think\xe2\x96\xb7",  "</think>" },
    { "\xef\xbd\x9cthink\xef\xbd\x9c",   "<think>"  },
    { "\xef\xbd\x9c/think\xef\xbd\x9c",  "</think>" },
};

QString normalizeThinkTags(const QString& text) {
    if (!text.contains(QLatin1Char('<')) && !text.contains(QChar(0x25C1)) && !text.contains(QChar(0xFF5C)))
        return text;

    QString normalized = text;
    for (const auto& alias : kThinkAliases) {
        const QString from = QString::fromUtf8(alias.from);
        if (!normalized.contains(from, Qt::CaseInsensitive)) continue;
        normalized.replace(from, QString::fromUtf8(alias.to), Qt::CaseInsensitive);
    }
    return normalized;
}

// 尾部保留字符数，不小于最长标签长度
constexpr int kTagRetainChars = 32;
static_assert(
    kTagRetainChars >= 24,
    "kTagRetainChars must stay >= the longest think tag/alias, otherwise tag text leaks into the answer"
);

struct TagMatch {
    int pos    = -1;
    int length = 0;
};

TagMatch findEarliestThinkTag(const QString& buffer, bool closing) {
    TagMatch best;
    for (const auto& tag : kThinkTags) {
        const QString needle = QString::fromLatin1(closing ? tag.close : tag.open);
        const int     pos    = buffer.indexOf(needle, 0, Qt::CaseInsensitive);
        if (pos < 0) continue;
        // 同一位置取更长的标签
        if (best.pos < 0 || pos < best.pos || (pos == best.pos && needle.size() > best.length)) {
            best.pos    = pos;
            best.length = needle.size();
        }
    }
    return best;
}

int retainForPossibleTag(const QString& buffer) {
    const int keep = qMin(buffer.size(), kTagRetainChars);
    if (keep <= 0) return 0;
    const QString tail = buffer.right(keep);
    if (!tail.contains(QLatin1Char('<')) && !tail.contains(QChar(0x25C1)) && !tail.contains(QChar(0xFF5C)))
        return 0;
    return keep;
}

bool couldExtendThinkTag(const QString& tag) {
    if (tag.isEmpty()) return false;
    const QString stem = tag.left(tag.size() - 1);
    for (const auto& candidate : kThinkTags) {
        const QString open = QString::fromLatin1(candidate.open);
        if (open.size() > tag.size() && open.startsWith(stem, Qt::CaseInsensitive)) return true;
    }
    return false;
}

bool isTruncatedThinkTag(const QString& text) {
    if (text.isEmpty()) return false;
    for (const auto& tag : kThinkTags) {
        const QString open = QString::fromLatin1(tag.open);
        if (text.size() < open.size() && open.startsWith(text, Qt::CaseInsensitive)) return true;
    }
    return false;
}

// 从各兼容字段中提取思维链文本
QString extractReasoningField(const QJsonObject& obj) {
    static const char* kStringKeys[] = {
        "reasoning_content",
        "reasoning",
        "reasoning_text",
        "thinking",
        "analysis",
        "thought",
    };
    for (const char* key : kStringKeys) {
        const QJsonValue value = obj.value(QLatin1String(key));
        if (!value.isString()) continue;
        const QString text = value.toString();
        if (!text.isEmpty()) return text;
    }

    // reasoning 为对象时取其中的 text/content
    for (const char* key : { "reasoning", "thinking" }) {
        const QJsonValue value = obj.value(QLatin1String(key));
        if (!value.isObject()) continue;
        const QJsonObject nested = value.toObject();
        for (const char* textKey : { "text", "content", "summary", "thinking" }) {
            const QJsonValue textValue = nested.value(QLatin1String(textKey));
            if (textValue.isString() && !textValue.toString().isEmpty()) return textValue.toString();
        }
    }

    // reasoning_details 数组，取其中的 text
    const QJsonValue details = obj.value(QLatin1String("reasoning_details"));
    if (details.isArray()) {
        QString text;
        for (const QJsonValue& itemValue : details.toArray()) {
            const QJsonObject item = itemValue.toObject();
            const QString     type = item.value(QLatin1String("type")).toString();
            if (type.startsWith("reasoning") || type.isEmpty()) text += item.value(QLatin1String("text")).toString();
        }
        if (!text.isEmpty()) return text;
    }

    return QString();
}

EmbeddedContent splitEmbeddedContent(const QString& rawContent) {
    EmbeddedContent result;
    const QString content = normalizeThinkTags(rawContent);
    result.answer         = content;

    const TagMatch open = findEarliestThinkTag(content, false);
    if (open.pos < 0) {
        // 只有结束标签时，之前的整段都算思维链
        const TagMatch strayClose = findEarliestThinkTag(content, true);
        if (strayClose.pos >= 0) {
            result.reasoning = content.left(strayClose.pos);
            result.answer    = content.mid(strayClose.pos + strayClose.length);
        }
        return result;
    }

    const TagMatch close = findEarliestThinkTag(content.mid(open.pos + open.length), true);
    const int      reasoningStart = open.pos + open.length;
    if (close.pos < 0) {
        // 没有结束标签：整段都算思维链
        result.reasoning = content.mid(reasoningStart);
        result.answer    = content.left(open.pos);
        return result;
    }

    const int closePos = reasoningStart + close.pos;
    result.reasoning   = content.mid(reasoningStart, closePos - reasoningStart);
    result.answer      = content.left(open.pos) + content.mid(closePos + close.length);
    return result;
}

// 补齐缺失的 tool 结果，丢弃孤立的 tool 消息
void normalizeToolMessageSequence(QJsonArray& messages) {
    QJsonArray normalized;
    for (int index = 0; index < messages.size(); ++index) {
        const QJsonObject message = messages.at(index).toObject();
        const QString     role    = message.value(QLatin1String("role")).toString();

        // tool 消息统一在下面重建
        if (role == QLatin1String("tool")) continue;

        normalized.append(message);

        if (role != QLatin1String("assistant")) continue;
        const QJsonValue toolCallsValue = message.value(QLatin1String("tool_calls"));
        if (!toolCallsValue.isArray()) continue;

        QStringList    pendingIds;
        QSet<QString>  seenIds;
        QJsonArray     validCalls;
        for (const QJsonValue& callValue : toolCallsValue.toArray()) {
            const QJsonObject call = callValue.toObject();
            const QString     id   = call.value(QLatin1String("id")).toString().trimmed();
            if (id.isEmpty() || seenIds.contains(id)) continue;
            seenIds.insert(id);
            validCalls.append(call);
            pendingIds.append(id);
        }

        if (validCalls.isEmpty()) {
            // 无有效 tool_calls 时去掉该字段
            QJsonObject cleaned = normalized.last().toObject();
            cleaned.remove(QLatin1String("tool_calls"));
            normalized.replace(normalized.size() - 1, cleaned);
            continue;
        }
        if (validCalls.size() != toolCallsValue.toArray().size()) {
            QJsonObject cleaned = normalized.last().toObject();
            cleaned["tool_calls"] = validCalls;
            normalized.replace(normalized.size() - 1, cleaned);
        }

        QHash<QString, QString> results;
        while (index + 1 < messages.size()) {
            const QJsonObject next = messages.at(index + 1).toObject();
            if (next.value(QLatin1String("role")).toString() != QLatin1String("tool")) break;
            ++index;
            const QString id = next.value(QLatin1String("tool_call_id")).toString();
            if (id.isEmpty() || results.contains(id)) continue;
            results.insert(id, next.value(QLatin1String("content")).toString());
        }

        for (const QString& id : pendingIds) {
            QJsonObject toolMessage;
            toolMessage["role"]         = QStringLiteral("tool");
            toolMessage["tool_call_id"] = id;
            toolMessage["content"] =
                results.contains(id) ? results.value(id) : pendingToolResultText();
            normalized.append(toolMessage);
        }
    }
    messages = normalized;
}

} // namespace

void ChatBot::finishStream() {
    if (m_streamEndEmitted) return;
    flushEmbeddedContent();
    m_streamEndEmitted = true;
    emit streamEnd();
}

void ChatBot::emitContentChunk(const QString& content) {
    if (content.isEmpty()) return;

    // Some providers embed reasoning inside content.
    m_embeddedContentBuffer += content;
    while (!m_embeddedContentBuffer.isEmpty()) {
        m_embeddedContentBuffer = normalizeThinkTags(m_embeddedContentBuffer);

        if (m_embeddedReasoningActive) {
            const TagMatch close = findEarliestThinkTag(m_embeddedContentBuffer, true);
            if (close.pos < 0) {
                // 尾部可能是半个标签，留给下一个分片
                const int keep     = retainForPossibleTag(m_embeddedContentBuffer);
                const int emitSize = m_embeddedContentBuffer.size() - keep;
                if (emitSize <= 0) return;
                const QString reasoning = m_embeddedContentBuffer.left(emitSize);
                m_embeddedContentBuffer.remove(0, emitSize);
                appendReasoningChunk(reasoning);
                return;
            }
            if (close.pos > 0) appendReasoningChunk(m_embeddedContentBuffer.left(close.pos));
            m_embeddedContentBuffer.remove(0, close.pos + close.length);
            m_embeddedReasoningActive = false;
            continue;
        }

        const TagMatch open = findEarliestThinkTag(m_embeddedContentBuffer, false);
        if (open.pos < 0) {
            // Retain a possible partial tag for the next chunk.
            const int keep     = retainForPossibleTag(m_embeddedContentBuffer);
            const int emitSize = m_embeddedContentBuffer.size() - keep;
            if (emitSize <= 0) return;
            const QString answer = m_embeddedContentBuffer.left(emitSize);
            m_embeddedContentBuffer.remove(0, emitSize);
            appendAnswerChunk(answer);
            return;
        }

        if (open.pos > 0) {
            appendAnswerChunk(m_embeddedContentBuffer.left(open.pos));
            m_embeddedContentBuffer.remove(0, open.pos);
        }

        // 可能是更长标签的前缀，先等下一个分片
        const QString candidate = m_embeddedContentBuffer.left(open.length);
        if (candidate.size() == m_embeddedContentBuffer.size() && couldExtendThinkTag(candidate)) return;

        m_embeddedContentBuffer.remove(0, open.length);
        m_embeddedReasoningActive = true;
    }
}

void ChatBot::appendReasoningChunk(const QString& text) {
    if (text.isEmpty()) return;
    m_currentReasoningBuffer += text;
    emit reasoningChunk(text);
}

void ChatBot::appendAnswerChunk(const QString& text) {
    if (text.isEmpty()) return;
    m_currentStreamBuffer += text;
    emit streamChunk(text);
}

// 用完整正文重新拆分思维链与正文
void ChatBot::reclassifyFinalAnswer() {
    if (m_currentStreamBuffer.isEmpty()) return;
    // 已有独立思维链字段时不再归并
    if (!m_currentReasoningBuffer.isEmpty()) return;

    const EmbeddedContent finalSplit = splitEmbeddedContent(m_currentStreamBuffer);
    if (finalSplit.reasoning.isEmpty()) return;

    info("收尾时从正文中拆出 {} 字的思维链", finalSplit.reasoning.size());
    m_currentReasoningBuffer = finalSplit.reasoning;
    m_currentStreamBuffer    = finalSplit.answer;
}

void ChatBot::flushEmbeddedContent() {
    if (m_embeddedContentBuffer.isEmpty()) return;
    QString remaining = normalizeThinkTags(m_embeddedContentBuffer);
    m_embeddedContentBuffer.clear();

    // 流停在完整开标签后时按思维链收尾
    if (!m_embeddedReasoningActive) {
        const TagMatch open = findEarliestThinkTag(remaining, false);
        if (open.pos >= 0 && remaining.size() == open.pos + open.length) {
            if (open.pos > 0) appendAnswerChunk(remaining.left(open.pos));
            remaining.clear();
            m_embeddedReasoningActive = true;
        }
    }

    if (m_embeddedReasoningActive) {
        appendReasoningChunk(remaining);
        return;
    }
    if (isTruncatedThinkTag(remaining)) return;
    appendAnswerChunk(remaining);
}

void ChatBot::makeApiRequest(const QJsonArray& messages) {
    // 递增序列号使旧请求的回调自动失效
    int seq = ++m_requestSeq;

    abortActiveReplies();

    m_cancelled        = false;
    m_streamEndEmitted = false;

    if (m_apiKey.isEmpty()) {
        emit errorOccurred("API 密钥未设置\n请进入「设置」页面配置有效的 API 密钥后重试");
        return;
    }

    // 发送前统一规整 tool_calls 序列
    QJsonArray safeMessages = messages;
    normalizeToolMessageSequence(safeMessages);

    QJsonObject requestBody;
    requestBody["model"] = m_model;
    requestBody[usesResponsesApi() ? "input" : "messages"] =
        usesResponsesApi() ? messagesToResponsesInput(safeMessages) : safeMessages;
    requestBody["temperature"] = m_temperature;
    requestBody["stream"]      = m_isStreaming;
    if (m_isStreaming && !usesResponsesApi())
        requestBody["stream_options"] = QJsonObject{
            {"include_usage", true}
        };

    if (m_capReasoning) {
        if (usesResponsesApi()) {
            QJsonObject reasoning{
                {"summary", "auto"}
            };
            if (!m_reasoningEffort.isEmpty()) reasoning["effort"] = m_reasoningEffort;
            requestBody["reasoning"] = reasoning;
        } else if (!m_reasoningEffort.isEmpty()) {
            requestBody["reasoning_effort"] = m_reasoningEffort;
        }
    }

    // 合并 extraParams（模型自定义参数，可覆盖默认字段）
    if (!m_extraParams.empty()) {
        QJsonDocument epDoc = QJsonDocument::fromJson(QByteArray::fromStdString(m_extraParams.dump()));
        if (epDoc.isObject()) {
            const auto epObj = epDoc.object();
            for (auto it = epObj.constBegin(); it != epObj.constEnd(); ++it) requestBody[it.key()] = it.value();
        }
    }

    // 注入本地函数工具和 Responses API 图片生成工具。
    if ((m_capToolCall && ((m_tavilyEnabled && !m_tavilyApiKey.isEmpty()) || m_shellToolEnabled))
        || (usesResponsesApi() && (m_capImageGeneration || m_nativeWebSearchEnabled))) {
        injectToolDefinitions(requestBody);
    }

    QJsonDocument requestDoc(requestBody);
    QByteArray    requestData = requestDoc.toJson(QJsonDocument::Compact);
    debug(
        "发送 API 请求: model={}, messages={}, payload={} bytes",
        m_model.toStdString(),
        safeMessages.size(),
        requestData.size()
    );

    QNetworkRequest request;
    request.setUrl(QUrl(m_apiEndpoint));
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setRawHeader("Authorization", QString("Bearer %1").arg(m_apiKey).toUtf8());
    request.setRawHeader("Content-Type", "application/json");
    request.setRawHeader("User-Agent", "PenMods ChatBot/1.0");

    QNetworkReply* reply = m_networkManager->post(request, requestData);
    m_activeReplies.append(reply);

    m_currentStreamBuffer.clear();
    m_currentReasoningBuffer.clear();
    m_embeddedContentBuffer.clear();
    m_embeddedReasoningActive = false;
    m_responseBuffer.clear();
    m_toolCallsBuffer.clear();
    m_responseToolItemIndexes.clear();
    m_serverToolCallActive = false;
    m_serverToolCallName.clear();

    // 流式与非流式都要发 streamStart
    emit streamStart();

    connect(reply, &QNetworkReply::finished, this, [this, reply, seq]() {
        if (seq != m_requestSeq) {
            reply->deleteLater();
            return;
        }
        m_activeReplies.removeAll(reply);
        handleNetworkReply(reply, m_isStreaming);
    });

    if (m_isStreaming) {
        connect(reply, &QNetworkReply::readyRead, this, [this, reply, seq]() {
            if (seq != m_requestSeq) return;
            QByteArray data = reply->readAll();
            if (data.isEmpty()) return;

            m_responseBuffer                     += QString::fromUtf8(data);
            const bool        hasTrailingNewline  = m_responseBuffer.endsWith('\n');
            const QStringList lines               = m_responseBuffer.split("\n", Qt::KeepEmptyParts);
            m_responseBuffer.clear();

            for (int lineIndex = 0; lineIndex < lines.size(); ++lineIndex) {
                const QString& line          = lines.at(lineIndex);
                QString        trimmedLine   = line.trimmed();
                const bool     isPartialLine = !hasTrailingNewline && lineIndex == lines.size() - 1;
                if (isPartialLine) {
                    if (!trimmedLine.startsWith("data: ")) {
                        m_responseBuffer = line;
                        break;
                    }
                    const QString candidate = trimmedLine.mid(6);
                    if (candidate.trimmed() != "[DONE]") {
                        QJsonParseError partialError;
                        QJsonDocument::fromJson(candidate.toUtf8(), &partialError);
                        if (partialError.error != QJsonParseError::NoError) {
                            m_responseBuffer = line;
                            break;
                        }
                    }
                }
                if (!trimmedLine.startsWith("data: ")) continue;

                QString jsonData = trimmedLine.mid(6);
                if (jsonData.trimmed() == "[DONE]") {
                    flushEmbeddedContent();
                    if (!m_cancelled) reclassifyFinalAnswer();
                    if (m_serverToolCallActive) {
                        emit toolCallProgress(
                            QString("已完成：%1")
                                .arg(m_serverToolCallName.isEmpty() ? "服务器工具" : m_serverToolCallName),
                            true
                        );
                        m_serverToolCallActive = false;
                        m_serverToolCallName.clear();
                    }
                    if (!m_cancelled) {
                        if (!m_toolCallsBuffer.isEmpty()) {
                            json tcArr = json::array();
                            for (auto it = m_toolCallsBuffer.constBegin(); it != m_toolCallsBuffer.constEnd(); ++it)
                                tcArr.push_back(it.value());
                            QString     toolCallsJson = QString::fromStdString(tcArr.dump());
                            MessageData assistantMsg;
                            assistantMsg.role          = "assistant";
                            assistantMsg.content       = m_currentStreamBuffer;
                            assistantMsg.reasoning     = m_currentReasoningBuffer;
                            assistantMsg.toolCallsJson = toolCallsJson;
                            currentMessages().append(assistantMsg);
                            if (currentMessages().size() > MAX_HISTORY_SIZE) currentMessages().removeFirst();
                            m_sessions[m_currentSessionId].updatedAt =
                                QDateTime::currentDateTime().toString(Qt::ISODate);
                            saveSessions();
                            emit messagesChanged();
                            dispatchToolCalls(toolCallsJson);
                        } else if (!m_currentStreamBuffer.isEmpty() || !m_currentReasoningBuffer.isEmpty()) {
                            MessageData assistantMsg;
                            assistantMsg.role      = "assistant";
                            assistantMsg.content   = m_currentStreamBuffer;
                            assistantMsg.reasoning = m_currentReasoningBuffer;
                            currentMessages().append(assistantMsg);
                            if (currentMessages().size() > MAX_HISTORY_SIZE) currentMessages().removeFirst();
                            m_sessions[m_currentSessionId].updatedAt =
                                QDateTime::currentDateTime().toString(Qt::ISODate);
                            saveSessions();
                            emit messagesChanged();
                        }
                    }
                    m_currentStreamBuffer.clear();
                    m_currentReasoningBuffer.clear();
                    m_toolCallsBuffer.clear();
                    finishStream();
                    continue;
                }

                QJsonDocument doc = QJsonDocument::fromJson(jsonData.toUtf8());
                if (!doc.isObject()) continue;

                QJsonObject obj = doc.object();
                if (obj["usage"].isObject()) recordApiUsage(obj["usage"].toObject());
                if (usesResponsesApi()) {
                    const QString eventType = obj["type"].toString();
                    if (eventType == "response.output_text.delta") {
                        emitContentChunk(obj["delta"].toString());
                    } else if (
                        eventType == "response.reasoning_summary_text.delta"
                        || eventType == "response.reasoning_text.delta"
                    ) {
                        const QString content = obj["delta"].toString();
                        if (!content.isEmpty()) appendReasoningChunk(content);
                    } else if (
                        eventType == "response.reasoning_summary_text.done"
                        || eventType == "response.reasoning_text.done"
                    ) {
                        // 服务端只推 done 时用最终文本兜底
                        const QString content = obj["text"].toString();
                        if (m_currentReasoningBuffer.isEmpty() && !content.isEmpty())
                            appendReasoningChunk(content);
                    } else if (eventType == "response.function_call_arguments.delta") {
                        const QString callId  = obj["item_id"].toString(obj["call_id"].toString());
                        const QString content = obj["delta"].toString();
                        if (!callId.isEmpty()) {
                            int index = m_responseToolItemIndexes.value(callId, m_toolCallsBuffer.size());
                            for (auto it = m_toolCallsBuffer.constBegin(); it != m_toolCallsBuffer.constEnd(); ++it)
                                if (it.value().value("id", "") == callId.toStdString()) index = it.key();
                            if (!m_toolCallsBuffer.contains(index)) {
                                m_toolCallsBuffer[index] = json{
                                    {"id",       callId.toStdString()             },
                                    {"type",     "function"                       },
                                    {"function", {{"name", ""}, {"arguments", ""}}}
                                };
                            }
                            m_toolCallsBuffer[index]["function"]["arguments"] =
                                m_toolCallsBuffer[index]["function"]["arguments"].get<std::string>()
                                + content.toStdString();
                        }
                    } else if (eventType == "response.output_item.added") {
                        const QJsonObject item     = obj["item"].toObject();
                        const QString     itemType = item["type"].toString();
                        if (itemType == "function_call") {
                            // 工具卡由 dispatchToolCalls 创建
                            const int index                                  = m_toolCallsBuffer.size();
                            m_responseToolItemIndexes[item["id"].toString()] = index;
                            m_toolCallsBuffer[index]                         = json{
                                {"id",       item["call_id"].toString().toStdString()       },
                                {"type",     "function"                                     },
                                {"function",
                                 {{"name", item["name"].toString().toStdString()},
                                  {"arguments", item["arguments"].toString().toStdString()}}}
                            };
                        } else if (itemType.endsWith("_call")) {
                            m_serverToolCallActive = true;
                            m_serverToolCallName   = itemType;
                            emit toolCallProgress(QString("正在执行：%1").arg(itemType), false);
                        }
                    } else if (eventType == "response.output_item.done") {
                        const QString itemType = obj["item"].toObject()["type"].toString();
                        if (itemType.endsWith("_call") && itemType != "function_call") {
                            m_serverToolCallActive = false;
                            m_serverToolCallName.clear();
                            emit toolCallProgress(QString("已完成：%1").arg(itemType), true);
                        }
                    } else if (eventType.startsWith("response.") && eventType.contains("_call.")) {
                        const QString toolName = eventType.section('.', 1, 1);
                        if (eventType.endsWith(".in_progress")) {
                            m_serverToolCallActive = true;
                            m_serverToolCallName   = toolName;
                            emit toolCallProgress(QString("工具处理中：%1").arg(toolName), false);
                        } else if (eventType.endsWith(".completed") || eventType.endsWith(".done")) {
                            m_serverToolCallActive = false;
                            m_serverToolCallName.clear();
                            emit toolCallProgress(QString("已完成：%1").arg(toolName), true);
                        }
                    } else if (eventType == "response.completed") {
                        const QJsonObject response = obj["response"].toObject();
                        if (!obj["usage"].isObject() && response["usage"].isObject())
                            recordApiUsage(response["usage"].toObject());
                        if (response["status"].toString() != "completed") {
                            if (m_serverToolCallActive) {
                                emit toolCallProgress("服务器工具执行未完成", true);
                                m_serverToolCallActive = false;
                                m_serverToolCallName.clear();
                            }
                            const QString detail = response["error"].toObject()["message"].toString(
                                response["incomplete_details"].toObject()["reason"].toString("响应未完成")
                            );
                            emit errorOccurred("API 请求失败\n" + detail);
                            m_currentStreamBuffer.clear();
                            m_toolCallsBuffer.clear();
                            finishStream();
                            continue;
                        }
                        if (m_serverToolCallActive) {
                            emit toolCallProgress(
                                QString("已完成：%1")
                                    .arg(m_serverToolCallName.isEmpty() ? "服务器工具" : m_serverToolCallName),
                                true
                            );
                            m_serverToolCallActive = false;
                            m_serverToolCallName.clear();
                        }
                        const QJsonArray           output          = response["output"].toArray();
                        const QVector<MessagePart> generatedImages = persistGeneratedImages(output);
                        if (m_currentReasoningBuffer.isEmpty()) {
                            const QString reasoning = responseOutputReasoning(output);
                            if (!reasoning.isEmpty()) appendReasoningChunk(reasoning);
                        }
                        flushEmbeddedContent();
                        if (m_currentStreamBuffer.isEmpty()) {
                            const QString content = responseOutputText(output);
                            if (!content.isEmpty()) emitContentChunk(content);
                            flushEmbeddedContent();
                        }

                        if (m_currentStreamBuffer.isEmpty() && !generatedImages.isEmpty()) {
                            emitContentChunk("已生成图片");
                            flushEmbeddedContent();
                        }

                        QString toolCallsJson = responseOutputToolCalls(output);
                        if ((toolCallsJson.isEmpty() || toolCallsJson == "[]") && !m_toolCallsBuffer.isEmpty()) {
                            json calls = json::array();
                            for (auto it = m_toolCallsBuffer.constBegin(); it != m_toolCallsBuffer.constEnd(); ++it)
                                calls.push_back(it.value());
                            toolCallsJson = QString::fromStdString(calls.dump());
                        }
                        if (!m_cancelled && !toolCallsJson.isEmpty() && toolCallsJson != "[]") {
                            MessageData assistantMsg;
                            assistantMsg.role          = "assistant";
                            assistantMsg.content       = m_currentStreamBuffer;
                            assistantMsg.reasoning     = m_currentReasoningBuffer;
                            assistantMsg.toolCallsJson = toolCallsJson;
                            assistantMsg.parts         = generatedImages;
                            currentMessages().append(assistantMsg);
                            if (currentMessages().size() > MAX_HISTORY_SIZE) currentMessages().removeFirst();
                            m_sessions[m_currentSessionId].updatedAt =
                                QDateTime::currentDateTime().toString(Qt::ISODate);
                            saveSessions();
                            emit messagesChanged();
                            dispatchToolCalls(toolCallsJson);
                        } else if (
                            !m_cancelled && (!m_currentStreamBuffer.isEmpty() || !m_currentReasoningBuffer.isEmpty())
                        ) {
                            MessageData assistantMsg;
                            assistantMsg.role      = "assistant";
                            assistantMsg.content   = m_currentStreamBuffer;
                            assistantMsg.reasoning = m_currentReasoningBuffer;
                            assistantMsg.parts     = generatedImages;
                            currentMessages().append(assistantMsg);
                            if (currentMessages().size() > MAX_HISTORY_SIZE) currentMessages().removeFirst();
                            m_sessions[m_currentSessionId].updatedAt =
                                QDateTime::currentDateTime().toString(Qt::ISODate);
                            saveSessions();
                            emit messagesChanged();
                        }
                        if (!generatedImages.isEmpty())
                            emit imageAttachmentsReceived(attachmentVariants(generatedImages));
                        m_currentStreamBuffer.clear();
                        m_currentReasoningBuffer.clear();
                        m_toolCallsBuffer.clear();
                        m_responseToolItemIndexes.clear();
                        finishStream();
                    }
                    continue;
                }
                if (!obj.contains("choices") || !obj["choices"].isArray()) continue;

                QJsonArray choices = obj["choices"].toArray();
                if (choices.isEmpty()) continue;
                QJsonObject choice = choices[0].toObject();
                if (!choice.contains("delta")) continue;

                QJsonObject delta = choice["delta"].toObject();

                const QString reasoning = extractReasoningField(delta);
                if (!reasoning.isEmpty()) {
                    appendReasoningChunk(reasoning);
                }

                if (delta.contains("content") && delta["content"].isString()) {
                    emitContentChunk(delta["content"].toString());
                }

                if (delta.contains("tool_calls") && delta["tool_calls"].isArray()) {
                    for (const auto& tcVal : delta["tool_calls"].toArray()) {
                        QJsonObject tc    = tcVal.toObject();
                        int         index = tc["index"].toInt(0);
                        if (!m_toolCallsBuffer.contains(index)) {
                            json buf;
                            buf["id"]       = "";
                            buf["type"]     = "function";
                            buf["function"] = {
                                {"name",      ""},
                                {"arguments", ""}
                            };
                            m_toolCallsBuffer[index] = buf;
                        }
                        auto& buf = m_toolCallsBuffer[index];
                        if (tc.contains("id") && !tc["id"].toString().isEmpty())
                            buf["id"] = tc["id"].toString().toStdString();
                        if (tc.contains("function") && tc["function"].isObject()) {
                            QJsonObject fn = tc["function"].toObject();
                            if (fn.contains("name") && !fn["name"].toString().isEmpty())
                                buf["function"]["name"] = fn["name"].toString().toStdString();
                            if (fn.contains("arguments"))
                                buf["function"]["arguments"] = buf["function"]["arguments"].get<std::string>()
                                                             + fn["arguments"].toString().toStdString();
                        }
                    }
                }
            }
        });
    }
}

// -----------------------------------------------------------------------
// handleNetworkReply
// -----------------------------------------------------------------------

void ChatBot::handleNetworkReply(QNetworkReply* reply, bool isStream) {
    if (reply->error() == QNetworkReply::NoError) {
        if (isStream) {
            flushEmbeddedContent();
            if (!m_cancelled) reclassifyFinalAnswer();
            if (!m_toolCallsBuffer.isEmpty()) {
                json tcArr = json::array();
                for (auto it = m_toolCallsBuffer.constBegin(); it != m_toolCallsBuffer.constEnd(); ++it)
                    tcArr.push_back(it.value());
                QString     toolCallsJson = QString::fromStdString(tcArr.dump());
                MessageData assistantMsg;
                assistantMsg.role = "assistant";
                if (!m_cancelled) {
                    assistantMsg.content   = m_currentStreamBuffer;
                    assistantMsg.reasoning = m_currentReasoningBuffer;
                }
                assistantMsg.toolCallsJson = toolCallsJson;
                if (!m_cancelled) {
                    currentMessages().append(assistantMsg);
                    if (currentMessages().size() > MAX_HISTORY_SIZE) currentMessages().removeFirst();
                    m_sessions[m_currentSessionId].updatedAt = QDateTime::currentDateTime().toString(Qt::ISODate);
                    saveSessions();
                    emit messagesChanged();
                }
                dispatchToolCalls(toolCallsJson);
            } else if (!m_currentStreamBuffer.isEmpty() || !m_currentReasoningBuffer.isEmpty()) {
                if (!m_cancelled) {
                    MessageData assistantMsg;
                    assistantMsg.role      = "assistant";
                    assistantMsg.content   = m_currentStreamBuffer;
                    assistantMsg.reasoning = m_currentReasoningBuffer;
                    currentMessages().append(assistantMsg);
                    if (currentMessages().size() > MAX_HISTORY_SIZE) currentMessages().removeFirst();
                    m_sessions[m_currentSessionId].updatedAt = QDateTime::currentDateTime().toString(Qt::ISODate);
                    saveSessions();
                    emit messagesChanged();
                }
            }
            if (m_serverToolCallActive) {
                emit toolCallProgress("服务器工具执行未完成", true);
                m_serverToolCallActive = false;
                m_serverToolCallName.clear();
            }
            finishStream();
        } else {
            QByteArray    response = reply->readAll();
            QJsonDocument doc      = QJsonDocument::fromJson(response);
            if (!doc.isObject()) {
                emit errorOccurred("API 响应格式错误：不是有效的 JSON 对象");
                reply->deleteLater();
                return;
            }

            QJsonObject obj = doc.object();
            if (obj["usage"].isObject()) recordApiUsage(obj["usage"].toObject());
            if (usesResponsesApi()) {
                if (obj["status"].toString() != "completed") {
                    const QString detail = obj["error"].toObject()["message"].toString(
                        obj["incomplete_details"].toObject()["reason"].toString("响应未完成")
                    );
                    emit errorOccurred("API 请求失败\n" + detail);
                    m_activeReplies.removeAll(reply);
                    reply->deleteLater();
                    return;
                }
                const QJsonArray           output          = obj["output"].toArray();
                const QVector<MessagePart> generatedImages = persistGeneratedImages(output);
                const QString              toolCallsJson   = responseOutputToolCalls(output);
                QString                    content         = responseOutputText(output);
                QString                    reasoning       = responseOutputReasoning(output);
                const EmbeddedContent      embedded        = splitEmbeddedContent(content);
                content                                    = embedded.answer;
                if (!embedded.reasoning.isEmpty()) {
                    if (!reasoning.isEmpty()) reasoning += "\n";
                    reasoning += embedded.reasoning;
                }
                if (content.isEmpty() && !generatedImages.isEmpty()) content = "已生成图片";
                if (!m_cancelled && !reasoning.isEmpty()) emit reasoningChunk(reasoning);
                if (!m_cancelled && !toolCallsJson.isEmpty() && toolCallsJson != "[]") {
                    MessageData assistantMsg;
                    assistantMsg.role          = "assistant";
                    assistantMsg.content       = content;
                    assistantMsg.reasoning     = reasoning;
                    assistantMsg.toolCallsJson = toolCallsJson;
                    assistantMsg.parts         = generatedImages;
                    currentMessages().append(assistantMsg);
                    if (currentMessages().size() > MAX_HISTORY_SIZE) currentMessages().removeFirst();
                    m_sessions[m_currentSessionId].updatedAt = QDateTime::currentDateTime().toString(Qt::ISODate);
                    saveSessions();
                    emit messagesChanged();
                    dispatchToolCalls(toolCallsJson);
                } else if (!m_cancelled && (!content.isEmpty() || !reasoning.isEmpty())) {
                    MessageData assistantMsg;
                    assistantMsg.role      = "assistant";
                    assistantMsg.content   = content;
                    assistantMsg.reasoning = reasoning;
                    assistantMsg.parts     = generatedImages;
                    currentMessages().append(assistantMsg);
                    if (currentMessages().size() > MAX_HISTORY_SIZE) currentMessages().removeFirst();
                    m_sessions[m_currentSessionId].updatedAt = QDateTime::currentDateTime().toString(Qt::ISODate);
                    saveSessions();
                    emit messageReceived(content, true);
                    if (!generatedImages.isEmpty()) emit imageAttachmentsReceived(attachmentVariants(generatedImages));
                    emit messagesChanged();
                } else if (!m_cancelled) {
                    emit errorOccurred("API 响应格式错误：Responses API 未返回文本、图片或工具调用");
                }
                m_activeReplies.removeAll(reply);
                reply->deleteLater();
                return;
            }
            if (!obj.contains("choices") || !obj["choices"].isArray()) {
                emit errorOccurred("API 响应格式错误：缺少 choices");
                reply->deleteLater();
                return;
            }

            QJsonArray choices = obj["choices"].toArray();
            if (choices.isEmpty()) {
                emit errorOccurred("API 响应格式错误：choices 为空");
                reply->deleteLater();
                return;
            }

            QJsonObject           choice    = choices.first().toObject();
            QJsonObject           message   = choice["message"].toObject();
            QString               reasoning = extractReasoningField(message);
            const EmbeddedContent embedded  = splitEmbeddedContent(message["content"].toString());
            if (!embedded.reasoning.isEmpty()) {
                if (!reasoning.isEmpty()) reasoning += "\n";
                reasoning += embedded.reasoning;
            }
            if (!m_cancelled && !reasoning.isEmpty()) emit reasoningChunk(reasoning);

            if (message.contains("tool_calls") && message["tool_calls"].isArray()) {
                QJsonDocument tcDoc(message["tool_calls"].toArray());
                QString       toolCallsJson = QString(tcDoc.toJson(QJsonDocument::Compact));
                MessageData   assistantMsg;
                assistantMsg.role = "assistant";
                if (!m_cancelled) {
                    assistantMsg.content   = embedded.answer;
                    assistantMsg.reasoning = reasoning;
                }
                assistantMsg.toolCallsJson = toolCallsJson;
                if (!m_cancelled) {
                    currentMessages().append(assistantMsg);
                    if (currentMessages().size() > MAX_HISTORY_SIZE) currentMessages().removeFirst();
                    m_sessions[m_currentSessionId].updatedAt = QDateTime::currentDateTime().toString(Qt::ISODate);
                    saveSessions();
                    emit messagesChanged();
                }
                dispatchToolCalls(toolCallsJson);
            } else if (message.contains("content") && message["content"].isString()) {
                const QString content = embedded.answer;
                if (!m_cancelled) {
                    MessageData assistantMsg;
                    assistantMsg.role      = "assistant";
                    assistantMsg.content   = content;
                    assistantMsg.reasoning = reasoning;
                    currentMessages().append(assistantMsg);
                    if (currentMessages().size() > MAX_HISTORY_SIZE) currentMessages().removeFirst();
                    m_sessions[m_currentSessionId].updatedAt = QDateTime::currentDateTime().toString(Qt::ISODate);
                    saveSessions();
                    emit messageReceived(content, true);
                    emit messagesChanged();
                }
            }
        }
    } else {
        if (reply->error() == QNetworkReply::OperationCanceledError && m_cancelled) {
            m_activeReplies.removeAll(reply);
            reply->deleteLater();
            return;
        }

        if (isStream) finishStream();

        int        httpStatus   = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        QByteArray responseBody = reply->readAll();
        QString    detailMsg;
        if (!responseBody.isEmpty()) {
            QJsonDocument errDoc = QJsonDocument::fromJson(responseBody);
            if (errDoc.isObject()) {
                QJsonObject errObj = errDoc.object();
                if (errObj.contains("error") && errObj["error"].isObject())
                    detailMsg = errObj["error"].toObject()["message"].toString();
            }
            if (detailMsg.isEmpty()) detailMsg = QString::fromUtf8(responseBody).left(200);
        }

        QString suggestion;
        if (httpStatus == 401 || httpStatus == 403) suggestion = "API 密钥无效或已过期";
        else if (httpStatus == 429) suggestion = "请求频率过高，请稍后再试";
        else if (httpStatus >= 500) suggestion = "AI 服务端异常，请稍后重试";
        else if (
            reply->error() == QNetworkReply::ConnectionRefusedError
            || reply->error() == QNetworkReply::HostNotFoundError || reply->error() == QNetworkReply::TimeoutError
        )
            suggestion = "无法连接到 AI 服务，请检查网络连接";
        else suggestion = reply->errorString();

        QStringList parts;
        if (httpStatus > 0) parts << QString("状态码: %1").arg(httpStatus);
        if (!detailMsg.isEmpty()) parts << detailMsg;
        parts << suggestion;
        emit errorOccurred("API 请求失败\n" + parts.join("\n"));
    }

    m_activeReplies.removeAll(reply);
    reply->deleteLater();
}

// -----------------------------------------------------------------------
// editMessage / truncateHistory / deleteMessage / regenerateMessage
// -----------------------------------------------------------------------

void ChatBot::editMessage(int index, const QString& newContent) {
    auto& msgs = currentMessages();
    if (index < 0 || index >= msgs.size()) return;

    if (msgs[index].role != "user") {
        warn("拒绝编辑非用户消息，索引 {} 的 role={}", index, msgs[index].role.toStdString());
        return;
    }

    QString currentRole = msgs[index].role;
    msgs[index].content = newContent;
    msgs[index].parts.clear();
    msgs.resize(index + 1);

    m_sessions[m_currentSessionId].updatedAt = QDateTime::currentDateTime().toString(Qt::ISODate);
    saveSessions();
    emit messagesChanged();

    if (currentRole == "user" && index == msgs.size() - 1) {
        QJsonArray  apiMessages;
        QJsonObject sysMsg;
        sysMsg["role"]    = "system";
        sysMsg["content"] = m_defaultPrompt;
        apiMessages.append(sysMsg);
        for (const auto& msg : msgs) apiMessages.append(messageToJson(msg));
        makeApiRequest(apiMessages);
    }
}

void ChatBot::truncateHistory(int index) {
    auto& msgs = currentMessages();
    if (index < 0 || index >= msgs.size()) return;
    msgs.resize(index);
    m_sessions[m_currentSessionId].updatedAt = QDateTime::currentDateTime().toString(Qt::ISODate);
    saveSessions();
    emit messagesChanged();
}

void ChatBot::deleteMessage(int index) {
    auto& msgs = currentMessages();
    if (index < 0 || index >= msgs.size()) return;
    msgs.remove(index);
    m_sessions[m_currentSessionId].updatedAt = QDateTime::currentDateTime().toString(Qt::ISODate);
    saveSessions();
    emit messagesChanged();
}

void ChatBot::regenerateMessage(int index) {
    auto& msgs = currentMessages();
    if (index < 0 || index >= msgs.size()) return;
    if (msgs[index].role != "assistant") {
        error("只能重新生成 AI 消息，索引 {} 处的消息不是 AI 消息", index);
        return;
    }

    int userMessageIndex = -1;
    for (int i = index - 1; i >= 0; i--) {
        if (msgs[i].role == "user") {
            userMessageIndex = i;
            break;
        }
    }
    if (userMessageIndex == -1) {
        error("找不到对应于 AI 消息的用户消息，索引: {}", index);
        return;
    }

    msgs.resize(userMessageIndex + 1);
    m_sessions[m_currentSessionId].updatedAt = QDateTime::currentDateTime().toString(Qt::ISODate);
    saveSessions();
    emit messagesChanged();

    QJsonArray  apiMessages;
    QJsonObject sysMsg;
    sysMsg["role"]    = "system";
    sysMsg["content"] = m_defaultPrompt;
    apiMessages.append(sysMsg);
    for (const auto& msg : msgs) apiMessages.append(messageToJson(msg));
    makeApiRequest(apiMessages);
}

// -----------------------------------------------------------------------
// clearHistory / saveMessages
// -----------------------------------------------------------------------

void ChatBot::clearHistory() {
    cancelRequest();

    currentMessages().clear();
    m_sessions[m_currentSessionId].updatedAt = QDateTime::currentDateTime().toString(Qt::ISODate);
    saveSessions();
    emit messagesChanged();
}

// 中止 shell 调用，只发信号不写历史
void ChatBot::abortPendingShellExecs(const QString& reason) {
    QStringList shellKeys;
    for (auto it = m_activeShellExecs.constBegin(); it != m_activeShellExecs.constEnd(); ++it)
        shellKeys.append(it.key());
    for (const auto& key : shellKeys) cleanupShellExec(key);

    while (!m_pendingShellExecs.isEmpty()) {
        auto pending = m_pendingShellExecs.takeFirst();
        emit shellCommandFinished(pending.toolCallId, false, reason, pending.command);
    }
}

void ChatBot::cancelRequest() {
    m_cancelled = true;
    ++m_requestSeq;
    abortActiveReplies();
    abortPendingShellExecs("用户取消了请求");

    m_currentStreamBuffer.clear();
    m_currentReasoningBuffer.clear();
    m_embeddedContentBuffer.clear();
    m_embeddedReasoningActive = false;
    m_responseBuffer.clear();
    m_toolCallsBuffer.clear();
    m_responseToolItemIndexes.clear();
    m_serverToolCallActive = false;
    m_serverToolCallName.clear();

    // 补齐本轮未完成的工具调用结果
    resolvePendingToolCalls("本次请求已中断，该工具调用已取消。");

    emit requestCancelled();
}

void ChatBot::saveMessages() {
    QString saveDir = "/userdisk/Music/AI/Saved";
    QDir    dir(saveDir);
    if (!dir.exists()) dir.mkpath(saveDir);

    QString fileName = "chat_" + QDateTime::currentDateTime().toString("yyyyMMdd_hhmmss") + ".md";
    QString savePath = saveDir + "/" + fileName;

    QFile file(savePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        error("无法打开文件进行写入: {}", savePath.toStdString());
        emit errorOccurred("保存聊天记录失败\n路径: " + savePath + "\n请检查磁盘空间或目录权限");
        return;
    }

    QTextStream out(&file);
    out.setCodec("UTF-8");
    out << tr("# AI 聊天记录\n");
    out << QDateTime::currentDateTime().toString("保存时间: yyyy-MM-dd hh:mm:ss") << tr("\n\n");

    for (const auto& msg : currentMessages()) {
        if (msg.role == "user")
            out << tr("我：\n") << (msg.isMultimodal() ? "[多模态消息]" : msg.content) << tr("\n\n");
        else if (msg.role == "assistant") out << tr("AI：\n") << msg.content << tr("\n\n");
    }

    file.close();
    showToast("保存成功");
}

// -----------------------------------------------------------------------
// getMessages（QML property getter）
// -----------------------------------------------------------------------

QVariantList ChatBot::getMessages() const {
    QVariantList messageList;
    if (!m_sessions.contains(m_currentSessionId)) return messageList;

    for (const auto& msg : m_sessions[m_currentSessionId].messages) {
        QVariantMap m;
        m["role"]          = msg.role;
        m["content"]       = msg.content;
        m["toolCallsJson"] = msg.toolCallsJson;
        m["toolCallId"]    = msg.toolCallId;
        m["reasoning"]     = msg.reasoning;
        if (msg.isMultimodal()) {
            QVariantList partsList;
            for (const auto& part : msg.parts) {
                QVariantMap pm;
                pm["type"]      = part.type;
                pm["text"]      = part.text;
                pm["url"]       = part.url;
                pm["format"]    = part.format;
                pm["localPath"] = part.localPath;
                pm["source"]    = part.localPath.isEmpty() ? part.url : QUrl::fromLocalFile(part.localPath).toString();
                pm["name"]      = part.name;
                pm["mimeType"]  = part.mimeType;
                pm["language"]  = part.language;
                pm["size"]      = part.size;
                partsList.append(pm);
            }
            m["parts"] = partsList;
        }
        messageList.append(m);
    }
    return messageList;
}

// -----------------------------------------------------------------------
// Getters / Setters
// -----------------------------------------------------------------------

QString ChatBot::getApiKey() const { return m_apiKey; }

void ChatBot::setApiKey(const QString& key) {
    if (m_apiKey == key) return;
    m_apiKey = key;
    emit apiKeyChanged();
}

QString ChatBot::getApiEndpoint() const { return m_apiEndpoint; }

void ChatBot::setApiEndpoint(const QString& endpoint) {
    if (m_apiEndpoint == endpoint) return;
    m_apiEndpoint = endpoint;
    emit apiEndpointChanged();
}

QString ChatBot::getModel() const { return m_model; }

QString ChatBot::getApiProtocol() const { return m_apiProtocol; }

void ChatBot::setModel(const QString& model) {
    if (m_model == model) return;
    m_model = model;
    emit modelChanged();
}

qreal ChatBot::getTemperature() const { return m_temperature; }

void ChatBot::setTemperature(qreal temp) {
    if (m_temperature == temp) return;
    m_temperature = temp;
    emit temperatureChanged();
}

QString ChatBot::getDefaultPrompt() const { return m_defaultPrompt; }

void ChatBot::setDefaultPrompt(const QString& prompt) {
    if (m_defaultPrompt == prompt) return;
    m_defaultPrompt = prompt;
    emit defaultPromptChanged();
}

bool ChatBot::getIsStreaming() const { return m_isStreaming; }

void ChatBot::setIsStreaming(bool streaming) {
    if (m_isStreaming == streaming) return;
    m_isStreaming = streaming;
    auto& config  = mod::Config::getInstance();
    json  aiCfg   = config.read("ai");
    if (aiCfg.is_null()) aiCfg = json::object();
    // 顶层与 chatbot 下都写一份
    aiCfg["streaming"] = streaming;
    if (!aiCfg.contains("chatbot") || !aiCfg["chatbot"].is_object()) aiCfg["chatbot"] = json::object();
    aiCfg["chatbot"]["streaming"] = streaming;
    config.write("ai", aiCfg, true);
    emit isStreamingChanged();
}

void ChatBot::setProxyVisionModelId(const QString& v) {
    if (m_proxyVisionModelId == v) return;
    m_proxyVisionModelId = v;
    emit proxyVisionSettingsChanged();
}

void ChatBot::setProxyVisionPrompt(const QString& v) {
    if (m_proxyVisionPrompt == v) return;
    m_proxyVisionPrompt = v;
    emit proxyVisionSettingsChanged();
}

// -----------------------------------------------------------------------
// 多模型管理
// -----------------------------------------------------------------------

void ChatBot::initModels() {
    json aiCfg = mod::Config::getInstance().read("ai");

    if (aiCfg.contains("models") && aiCfg["models"].is_array() && !aiCfg["models"].empty()) {
        m_modelsData["models"]        = aiCfg["models"];
        m_modelsData["activeModelId"] = aiCfg.value("activeModelId", "");
    } else {
        info("创建默认模型配置");
        json defaultModel;
        defaultModel["id"]                 = m_model.toStdString();
        defaultModel["name"]               = "DeepSeek Chat";
        defaultModel["provider"]           = "DeepSeek";
        defaultModel["endpoint"]           = m_apiEndpoint.toStdString();
        defaultModel["apiKey"]             = m_apiKey.toStdString();
        defaultModel["modelId"]            = m_model.toStdString();
        defaultModel["apiProtocol"]        = "chat_completions";
        defaultModel["temperature"]        = m_temperature;
        defaultModel["reasoningEffort"]    = "";
        defaultModel["extraParams"]        = json::object();
        defaultModel["proxyVisionModelId"] = "";
        defaultModel["proxyVisionPrompt"]  = "请详细描述这张图片的内容。如果图片中有文字，请完整转录。";
        m_modelsData["models"]             = json::array({defaultModel});
        m_modelsData["activeModelId"]      = m_model.toStdString();
        saveModels();
    }

    if (m_modelsData.contains("activeModelId")) {
        std::string activeId = m_modelsData["activeModelId"];
        for (const auto& model : m_modelsData["models"]) {
            if (model.contains("id") && model["id"] == activeId) {
                applyModelConfig(model);
                break;
            }
        }
    }
}

void ChatBot::saveModels() {
    json aiCfg = mod::Config::getInstance().read("ai");
    if (aiCfg.is_null()) aiCfg = json::object();
    aiCfg["models"]        = m_modelsData["models"];
    aiCfg["activeModelId"] = m_modelsData.value("activeModelId", "");
    mod::Config::getInstance().write("ai", aiCfg);
}

void ChatBot::applyModelConfig(const json& modelObj) {
    if (modelObj.contains("endpoint")) m_apiEndpoint = QString::fromStdString(modelObj["endpoint"]);
    if (modelObj.contains("apiKey")) m_apiKey = QString::fromStdString(modelObj["apiKey"]);
    if (modelObj.contains("modelId")) m_model = QString::fromStdString(modelObj["modelId"]);
    m_apiProtocol = QString::fromStdString(modelObj.value("apiProtocol", "chat_completions"));
    if (m_apiProtocol != "responses") m_apiProtocol = "chat_completions";
    if (modelObj.contains("temperature") && modelObj["temperature"].is_number())
        m_temperature = modelObj["temperature"];
    if (modelObj.contains("extraParams") && modelObj["extraParams"].is_object())
        m_extraParams = modelObj["extraParams"];
    else m_extraParams = json::object();

    m_maxContextSize                          = modelObj.value("maxContextSize", 0);
    m_reasoningEffort                         = QString::fromStdString(modelObj.value("reasoningEffort", ""));
    static const QStringList reasoningEfforts = {"none", "minimal", "low", "medium", "high", "xhigh"};
    if (!reasoningEfforts.contains(m_reasoningEffort)) m_reasoningEffort.clear();

    m_nativeWebSearchEnabled  = modelObj.value("nativeWebSearchEnabled", false);
    m_nativeWebSearchProvider = QString::fromStdString(modelObj.value("nativeWebSearchProvider", "auto"));
    static const QStringList nativeWebSearchProviders = {"auto", "openai", "xai", "compatible"};
    if (!nativeWebSearchProviders.contains(m_nativeWebSearchProvider)) m_nativeWebSearchProvider = "auto";

    m_proxyVisionModelId = QString::fromStdString(modelObj.value("proxyVisionModelId", ""));
    m_proxyVisionPrompt  = QString::fromStdString(
        modelObj.value("proxyVisionPrompt", "请详细描述这张图片的内容。如果图片中有文字，请完整转录。")
    );

    m_capText            = true;
    m_capVision          = false;
    m_capAudio           = false;
    m_capToolCall        = false;
    m_capReasoning       = false;
    m_capImageGeneration = false;
    if (modelObj.contains("capabilities") && modelObj["capabilities"].is_object()) {
        const auto& cap      = modelObj["capabilities"];
        m_capText            = cap.value("text", true);
        m_capVision          = cap.value("vision", false);
        m_capAudio           = cap.value("audio", false);
        m_capToolCall        = cap.value("toolCall", false);
        m_capReasoning       = cap.value("reasoning", false);
        m_capImageGeneration = cap.value("imageGeneration", false);
    }
    emit activeModelCapabilitiesChanged();
}

QString ChatBot::getModels() { return QString::fromStdString(m_modelsData.dump(2)); }

bool ChatBot::addModel(const QString& modelJson) {
    QJsonDocument doc = QJsonDocument::fromJson(modelJson.toUtf8());
    if (!doc.isObject()) return false;

    QJsonObject input    = doc.object();
    std::string id       = input["id"].toString().toStdString();
    std::string modelId  = input["modelId"].toString().toStdString();
    std::string endpoint = input["endpoint"].toString().toStdString();

    if (id.empty() || modelId.empty() || endpoint.empty()) {
        warn("添加模型失败：缺少必填字段 (id, modelId, endpoint)");
        return false;
    }

    json newModel;
    newModel["id"]                            = id;
    newModel["name"]                          = input["name"].toString().toStdString();
    newModel["provider"]                      = input["provider"].toString().toStdString();
    newModel["endpoint"]                      = endpoint;
    newModel["apiKey"]                        = input["apiKey"].toString().toStdString();
    newModel["modelId"]                       = modelId;
    QString apiProtocol                       = input["apiProtocol"].toString();
    newModel["apiProtocol"]                   = apiProtocol == "responses" ? "responses" : "chat_completions";
    newModel["temperature"]                   = input.contains("temperature") ? input["temperature"].toDouble() : 0.7;
    newModel["maxContextSize"]                = input.contains("maxContextSize") ? input["maxContextSize"].toInt() : 0;
    QString                  reasoningEffort  = input["reasoningEffort"].toString();
    static const QStringList reasoningEfforts = {"none", "minimal", "low", "medium", "high", "xhigh"};
    newModel["reasoningEffort"] =
        reasoningEfforts.contains(reasoningEffort) ? reasoningEffort.toStdString() : std::string();
    newModel["nativeWebSearchEnabled"]                = input["nativeWebSearchEnabled"].toBool(false);
    QString                  nativeWebSearchProvider  = input["nativeWebSearchProvider"].toString("auto");
    static const QStringList nativeWebSearchProviders = {"auto", "openai", "xai", "compatible"};
    newModel["nativeWebSearchProvider"] =
        nativeWebSearchProviders.contains(nativeWebSearchProvider) ? nativeWebSearchProvider.toStdString() : "auto";

    // capabilities
    {
        json cap;
        cap["text"]            = true;
        cap["vision"]          = false;
        cap["audio"]           = false;
        cap["toolCall"]        = false;
        cap["reasoning"]       = false;
        cap["imageGeneration"] = false;
        if (input.contains("capabilities") && input["capabilities"].isObject()) {
            const auto qcap = input["capabilities"].toObject();
            if (qcap.contains("text")) cap["text"] = qcap["text"].toBool(true);
            if (qcap.contains("vision")) cap["vision"] = qcap["vision"].toBool(false);
            if (qcap.contains("audio")) cap["audio"] = qcap["audio"].toBool(false);
            if (qcap.contains("toolCall")) cap["toolCall"] = qcap["toolCall"].toBool(false);
            if (qcap.contains("reasoning")) cap["reasoning"] = qcap["reasoning"].toBool(false);
            if (qcap.contains("imageGeneration")) cap["imageGeneration"] = qcap["imageGeneration"].toBool(false);
        }
        newModel["capabilities"] = cap;
    }

    // extraParams：接受字符串化的 JSON object
    if (input.contains("extraParams") && !input["extraParams"].toString().isEmpty()) {
        try {
            newModel["extraParams"] = json::parse(input["extraParams"].toString().toStdString());
        } catch (...) {
            newModel["extraParams"] = json::object();
        }
    } else {
        newModel["extraParams"] = json::object();
    }

    // proxyVision
    newModel["proxyVisionModelId"] = input["proxyVisionModelId"].toString().toStdString();
    newModel["proxyVisionPrompt"]  = input["proxyVisionPrompt"].toString().toStdString();

    bool updated = false;
    for (auto& model : m_modelsData["models"]) {
        if (model["id"] == id) {
            model   = newModel;
            updated = true;
            break;
        }
    }
    if (!updated) m_modelsData["models"].push_back(newModel);

    // 若更新的是当前活动模型，立即重新应用配置
    if (updated && m_modelsData.value("activeModelId", "") == id) {
        applyModelConfig(newModel);
        emit apiEndpointChanged();
        emit apiKeyChanged();
        emit modelChanged();
        emit apiProtocolChanged();
        emit temperatureChanged();
    }

    saveModels();
    emit modelsChanged();
    info("模型已{}: {}", updated ? "更新" : "添加", id);
    return true;
}

bool ChatBot::removeModel(const QString& modelId) {
    std::string id     = modelId.toStdString();
    auto&       models = m_modelsData["models"];
    for (auto it = models.begin(); it != models.end(); ++it) {
        if ((*it)["id"] == id) {
            models.erase(it);
            if (m_modelsData["activeModelId"] == id && !models.empty()) {
                m_modelsData["activeModelId"] = models[0]["id"];
                applyModelConfig(models[0]);
                emit apiEndpointChanged();
                emit apiKeyChanged();
                emit modelChanged();
                emit apiProtocolChanged();
                emit temperatureChanged();
            }
            // 清理其他模型对被删除模型的视觉代理引用
            for (auto& model : models) {
                if (model.value("proxyVisionModelId", "") == id) model["proxyVisionModelId"] = "";
            }
            // 若当前活动模型的代理引用恰好是被删除的模型，通知 QML
            if (m_proxyVisionModelId == modelId) {
                m_proxyVisionModelId.clear();
                emit proxyVisionSettingsChanged();
            }
            saveModels();
            emit modelsChanged();
            info("模型已删除: {}", id);
            return true;
        }
    }
    warn("删除模型失败：未找到 id={}", id);
    return false;
}

bool ChatBot::setActiveModel(const QString& modelId) {
    std::string id = modelId.toStdString();
    for (const auto& model : m_modelsData["models"]) {
        if (model["id"] == id) {
            m_modelsData["activeModelId"] = id;
            applyModelConfig(model);
            saveModels();
            emit modelsChanged();
            emit apiEndpointChanged();
            emit apiKeyChanged();
            emit modelChanged();
            emit apiProtocolChanged();
            emit temperatureChanged();
            info("活动模型已切换为: {}", id);
            return true;
        }
    }
    warn("切换模型失败：未找到 id={}", id);
    return false;
}

QString ChatBot::getActiveModel() {
    std::string activeId = m_modelsData.value("activeModelId", "");
    for (const auto& model : m_modelsData["models"]) {
        if (model["id"] == activeId) {
            json result        = model;
            result["isActive"] = true;
            return QString::fromStdString(result.dump(2));
        }
    }
    return "{}";
}

bool ChatBot::loadModelsFile() {
    initModels();
    emit modelsChanged();
    return true;
}

// -----------------------------------------------------------------------
// 多提示词管理
// -----------------------------------------------------------------------

void ChatBot::initPrompts() {
    json aiCfg = mod::Config::getInstance().read("ai");

    if (aiCfg.contains("prompts") && aiCfg["prompts"].is_array() && !aiCfg["prompts"].empty()) {
        m_promptsData["prompts"]        = aiCfg["prompts"];
        m_promptsData["activePromptId"] = aiCfg.value("activePromptId", "");
    } else {
        info("创建默认提示词配置");
        json defaultPrompt;
        defaultPrompt["id"]             = "default";
        defaultPrompt["name"]           = "通用助手";
        defaultPrompt["content"]        = m_defaultPrompt.toStdString();
        m_promptsData["prompts"]        = json::array({defaultPrompt});
        m_promptsData["activePromptId"] = "default";
        savePrompts();
    }

    if (m_promptsData.contains("activePromptId")) {
        std::string activeId = m_promptsData["activePromptId"];
        for (const auto& prompt : m_promptsData["prompts"]) {
            if (prompt.contains("id") && prompt["id"] == activeId) {
                QString old     = m_defaultPrompt;
                m_defaultPrompt = QString::fromStdString(prompt.value("content", ""));
                if (old != m_defaultPrompt) emit defaultPromptChanged();
                break;
            }
        }
    }
}

void ChatBot::savePrompts() {
    json aiCfg = mod::Config::getInstance().read("ai");
    if (aiCfg.is_null()) aiCfg = json::object();
    aiCfg["prompts"]        = m_promptsData["prompts"];
    aiCfg["activePromptId"] = m_promptsData.value("activePromptId", "");
    mod::Config::getInstance().write("ai", aiCfg);
}

QString ChatBot::getPrompts() { return QString::fromStdString(m_promptsData.dump(2)); }

bool ChatBot::addPrompt(const QString& promptJson) {
    QJsonDocument doc = QJsonDocument::fromJson(promptJson.toUtf8());
    if (!doc.isObject()) return false;

    QJsonObject input   = doc.object();
    std::string id      = input["id"].toString().toStdString();
    std::string name    = input["name"].toString().toStdString();
    std::string content = input["content"].toString().toStdString();

    if (id.empty() || name.empty()) {
        warn("添加提示词失败：缺少必填字段 (id, name)");
        return false;
    }

    json newPrompt;
    newPrompt["id"]      = id;
    newPrompt["name"]    = name;
    newPrompt["content"] = content;

    bool updated = false;
    for (auto& p : m_promptsData["prompts"]) {
        if (p["id"] == id) {
            p       = newPrompt;
            updated = true;
            break;
        }
    }
    if (!updated) m_promptsData["prompts"].push_back(newPrompt);

    savePrompts();
    emit promptsChanged();
    info("提示词已{}: {}", updated ? "更新" : "添加", id);
    return true;
}

bool ChatBot::removePrompt(const QString& promptId) {
    std::string id      = promptId.toStdString();
    auto&       prompts = m_promptsData["prompts"];
    for (auto it = prompts.begin(); it != prompts.end(); ++it) {
        if ((*it)["id"] == id) {
            prompts.erase(it);
            if (m_promptsData["activePromptId"] == id && !prompts.empty()) {
                m_promptsData["activePromptId"] = prompts[0]["id"];
                QString old                     = m_defaultPrompt;
                m_defaultPrompt                 = QString::fromStdString(prompts[0].value("content", ""));
                if (old != m_defaultPrompt) emit defaultPromptChanged();
            }
            savePrompts();
            emit promptsChanged();
            info("提示词已删除: {}", id);
            return true;
        }
    }
    warn("删除提示词失败：未找到 id={}", id);
    return false;
}

bool ChatBot::setActivePrompt(const QString& promptId) {
    std::string id = promptId.toStdString();
    for (const auto& p : m_promptsData["prompts"]) {
        if (p["id"] == id) {
            m_promptsData["activePromptId"] = id;
            QString old                     = m_defaultPrompt;
            m_defaultPrompt                 = QString::fromStdString(p.value("content", ""));
            if (old != m_defaultPrompt) emit defaultPromptChanged();
            savePrompts();
            emit promptsChanged();
            info("活动提示词已切换为: {}", id);
            return true;
        }
    }
    warn("切换提示词失败：未找到 id={}", id);
    return false;
}

QString ChatBot::getActivePrompt() {
    std::string activeId = m_promptsData.value("activePromptId", "");
    for (const auto& p : m_promptsData["prompts"]) {
        if (p["id"] == activeId) {
            json result        = p;
            result["isActive"] = true;
            return QString::fromStdString(result.dump(2));
        }
    }
    return "{}";
}

// -----------------------------------------------------------------------
// 多会话管理
// -----------------------------------------------------------------------

QString ChatBot::getCurrentSessionId() { return m_currentSessionId; }

QString ChatBot::getSessions() {
    json result;
    result["activeSessionId"] = m_currentSessionId.toStdString();
    json sessionsArr          = json::array();

    for (auto it = m_sessions.constBegin(); it != m_sessions.constEnd(); ++it) {
        const SessionData& session = it.value();
        json               sessionObj;
        sessionObj["id"]           = session.id.toStdString();
        sessionObj["title"]        = session.title.toStdString();
        sessionObj["createdAt"]    = session.createdAt.toStdString();
        sessionObj["updatedAt"]    = session.updatedAt.toStdString();
        sessionObj["messageCount"] = session.messages.size();
        sessionsArr.push_back(sessionObj);
    }
    result["sessions"] = sessionsArr;
    return QString::fromStdString(result.dump(2));
}

bool ChatBot::switchSession(const QString& sessionId) {
    if (!m_sessions.contains(sessionId)) {
        warn("切换会话失败：未找到 id={}", sessionId.toStdString());
        return false;
    }
    if (m_currentSessionId == sessionId) return true;

    // 先收尾旧会话进行中的请求与工具调用
    if (!m_activeReplies.isEmpty() || !m_toolCallBatch.isEmpty() || !m_pendingShellExecs.isEmpty()
        || !m_activeShellExecs.isEmpty()) {
        cancelRequest();
    }

    m_currentSessionId = sessionId;
    saveSessions();
    emit messagesChanged();
    emit sessionSwitched(sessionId);
    info("已切换到会话: {}", sessionId.toStdString());
    return true;
}

QString ChatBot::createSession(const QString& title) {
    SessionData session;
    session.id        = QUuid::createUuid().toString(QUuid::WithoutBraces);
    session.title     = title.isEmpty() ? "新对话" : title;
    session.createdAt = QDateTime::currentDateTime().toString(Qt::ISODate);
    session.updatedAt = session.createdAt;

    if (!m_activeReplies.isEmpty() || !m_toolCallBatch.isEmpty() || !m_pendingShellExecs.isEmpty()
        || !m_activeShellExecs.isEmpty()) {
        cancelRequest();
    }

    m_sessions.insert(session.id, session);
    m_currentSessionId = session.id;
    saveSessions();
    emit messagesChanged();
    emit sessionsChanged();
    emit sessionSwitched(session.id);
    info("已创建新会话: {} ({})", session.title.toStdString(), session.id.toStdString());
    return session.id;
}

bool ChatBot::deleteSession(const QString& sessionId) {
    // 删除当前会话前先收尾进行中的请求
    if (sessionId == m_currentSessionId
        && (!m_activeReplies.isEmpty() || !m_toolCallBatch.isEmpty() || !m_pendingShellExecs.isEmpty()
            || !m_activeShellExecs.isEmpty())) {
        cancelRequest();
    }

    if (!m_sessions.contains(sessionId)) {
        warn("删除会话失败：未找到 id={}", sessionId.toStdString());
        return false;
    }
    if (m_sessions.size() <= 1) {
        warn("不能删除唯一的会话");
        return false;
    }

    m_sessions.remove(sessionId);
    AttachmentManager::removeSession(QFileInfo(sessionsFilePath()).absolutePath(), sessionId);
    if (m_currentSessionId == sessionId) {
        m_currentSessionId = m_sessions.firstKey();
        emit messagesChanged();
        emit sessionSwitched(m_currentSessionId);
    }
    saveSessions();
    emit sessionsChanged();
    info("已删除会话: {}", sessionId.toStdString());
    return true;
}

bool ChatBot::renameSession(const QString& sessionId, const QString& newTitle) {
    if (!m_sessions.contains(sessionId)) {
        warn("重命名会话失败：未找到 id={}", sessionId.toStdString());
        return false;
    }
    m_sessions[sessionId].title     = newTitle;
    m_sessions[sessionId].updatedAt = QDateTime::currentDateTime().toString(Qt::ISODate);
    saveSessions();
    emit sessionsChanged();
    info("会话已重命名: {} -> {}", sessionId.toStdString(), newTitle.toStdString());
    return true;
}

QVariantList ChatBot::getSessionMessages(const QString& sessionId) {
    QVariantList messageList;
    if (!m_sessions.contains(sessionId)) return messageList;
    for (const auto& msg : m_sessions[sessionId].messages) {
        QVariantMap m;
        m["role"]    = msg.role;
        m["content"] = msg.content;
        messageList.append(m);
    }
    return messageList;
}

// -----------------------------------------------------------------------
// Tavily 网络搜索
// -----------------------------------------------------------------------

void ChatBot::initTavily() {
    json aiCfg = mod::Config::getInstance().read("ai");
    if (!aiCfg.contains("tavily") || !aiCfg["tavily"].is_object()) return;

    const auto& t       = aiCfg["tavily"];
    m_tavilyApiKey      = QString::fromStdString(t.value("api_key", ""));
    m_tavilySearchDepth = QString::fromStdString(t.value("search_depth", "advanced"));
    m_tavilyMaxResults  = t.value("max_results", 5);
    m_tavilyEnabled     = t.value("enabled", false);
    emit tavilyConfigChanged();
    info("Tavily 配置已加载, enabled={}, configured={}", m_tavilyEnabled, !m_tavilyApiKey.isEmpty());
}

void ChatBot::setTavilyEnabled(bool v) {
    if (m_tavilyEnabled == v) return;
    m_tavilyEnabled = v;

    json aiCfg = mod::Config::getInstance().read("ai");
    if (aiCfg.is_null()) aiCfg = json::object();
    if (!aiCfg.contains("tavily") || !aiCfg["tavily"].is_object()) aiCfg["tavily"] = json::object();
    aiCfg["tavily"]["enabled"] = v;
    mod::Config::getInstance().write("ai", aiCfg, true);
    emit tavilyConfigChanged();
}

QString ChatBot::getTavilyConfig() {
    json obj;
    obj["apiKey"]      = m_tavilyApiKey.toStdString();
    obj["searchDepth"] = m_tavilySearchDepth.toStdString();
    obj["maxResults"]  = m_tavilyMaxResults;
    obj["enabled"]     = m_tavilyEnabled;
    return QString::fromStdString(obj.dump(2));
}

void ChatBot::setTavilyConfig(const QString& configJson) {
    QJsonDocument doc = QJsonDocument::fromJson(configJson.toUtf8());
    if (!doc.isObject()) return;

    QJsonObject obj = doc.object();
    if (obj.contains("apiKey")) m_tavilyApiKey = obj["apiKey"].toString();
    if (obj.contains("searchDepth")) m_tavilySearchDepth = obj["searchDepth"].toString();
    if (obj.contains("maxResults")) m_tavilyMaxResults = obj["maxResults"].toInt(5);
    if (obj.contains("enabled")) m_tavilyEnabled = obj["enabled"].toBool();

    json aiCfg = mod::Config::getInstance().read("ai");
    if (aiCfg.is_null()) aiCfg = json::object();
    if (!aiCfg.contains("tavily") || !aiCfg["tavily"].is_object()) aiCfg["tavily"] = json::object();
    aiCfg["tavily"]["api_key"]      = m_tavilyApiKey.toStdString();
    aiCfg["tavily"]["search_depth"] = m_tavilySearchDepth.toStdString();
    aiCfg["tavily"]["max_results"]  = m_tavilyMaxResults;
    aiCfg["tavily"]["enabled"]      = m_tavilyEnabled;
    mod::Config::getInstance().write("ai", aiCfg, true);
    emit tavilyConfigChanged();
    info("Tavily 配置已保存");
}

void ChatBot::injectToolDefinitions(QJsonObject& requestBody) {
    QJsonArray tools;

    if (usesResponsesApi() && m_capImageGeneration)
        tools.append(
            QJsonObject{
                {"type", "image_generation"}
        }
        );

    if (usesResponsesApi() && m_nativeWebSearchEnabled)
        tools.append(
            QJsonObject{
                {"type", "web_search"}
        }
        );

    if (m_capToolCall && m_tavilyEnabled && !m_tavilyApiKey.isEmpty()) {
        QJsonObject funcParam;
        funcParam["type"] = "object";

        QJsonObject queryProp;
        queryProp["type"]        = "string";
        queryProp["description"] = "The search query in the user's language";

        QJsonObject properties;
        properties["query"] = queryProp;

        funcParam["properties"] = properties;
        funcParam["required"]   = QJsonArray({"query"});

        QJsonObject func;
        func["name"]        = "tavily_search";
        func["description"] = "Search the web for up-to-date information. Use when the user asks about current events, "
                              "recent facts, or anything outside your training data.";
        func["parameters"]  = funcParam;

        if (usesResponsesApi()) {
            func["type"] = "function";
            tools.append(func);
        } else {
            QJsonObject tool;
            tool["type"]     = "function";
            tool["function"] = func;
            tools.append(tool);
        }
    }

    if (m_capToolCall && m_shellToolEnabled) {
        QJsonObject funcParam;
        funcParam["type"] = "object";

        QJsonObject commandProp;
        commandProp["type"]        = "string";
        commandProp["description"] = "The shell command to execute (runs via /bin/sh -c)";

        QJsonObject properties;
        properties["command"] = commandProp;

        funcParam["properties"] = properties;
        funcParam["required"]   = QJsonArray({"command"});

        QJsonObject func;
        func["name"]        = "shell_exec";
        func["description"] = "Execute a shell command on the user's Linux device. Requires user approval. "
                              "Use for file operations, system queries, diagnostics, package management, etc.";
        func["parameters"]  = funcParam;

        if (usesResponsesApi()) {
            func["type"] = "function";
            tools.append(func);
        } else {
            QJsonObject tool;
            tool["type"]     = "function";
            tool["function"] = func;
            tools.append(tool);
        }
    }

    if (!tools.isEmpty()) {
        requestBody["tools"]       = tools;
        requestBody["tool_choice"] = "auto";
    }
}

void ChatBot::dispatchToolCalls(const QString& toolCallsJson) {
    if (m_cancelled) return;

    QJsonDocument doc = QJsonDocument::fromJson(toolCallsJson.toUtf8());
    if (!doc.isArray()) {
        emit toolCallReceived(toolCallsJson);
        return;
    }

    QJsonArray arr = doc.array();
    m_toolCallBatch.clear();

    const bool tavilyAvailable = m_capToolCall && m_tavilyEnabled && !m_tavilyApiKey.isEmpty();
    const bool shellAvailable  = m_capToolCall && m_shellToolEnabled;

    // 登记所有 tool_call，不可用的写占位结果
    for (const auto& val : arr) {
        QJsonObject tc   = val.toObject();
        QString     id   = tc["id"].toString();
        QString     name = tc["function"].toObject()["name"].toString();
        if (id.isEmpty()) continue;
        if (name == "tavily_search" && tavilyAvailable) {
            m_toolCallBatch.append({id, name, false, ""});
        } else if (name == "shell_exec" && shellAvailable) {
            m_toolCallBatch.append({id, name, false, ""});
        } else {
            // 明确告知模型该工具未执行
            m_toolCallBatch.append(
                {id,
                 name,
                 true,
                 QString("工具 %1 当前不可用或未启用，本次没有执行。请直接根据已有信息回答，不要重复调用该工具。")
                     .arg(name)}
            );
        }
    }

    if (m_toolCallBatch.isEmpty()) {
        emit toolCallReceived(toolCallsJson);
        return;
    }

    // 分发执行
    for (const auto& val : arr) {
        if (m_cancelled) break;
        QJsonObject tc   = val.toObject();
        QString     name = tc["function"].toObject()["name"].toString();
        QString     id   = tc["id"].toString();
        QString     args = tc["function"].toObject()["arguments"].toString();

        if (name == "tavily_search" && tavilyAvailable) {
            QString       query;
            QJsonDocument argsDoc = QJsonDocument::fromJson(args.toUtf8());
            if (argsDoc.isObject()) query = argsDoc.object()["query"].toString();
            if (query.isEmpty()) query = args;
            executeTavilySearch(id, query);
        } else if (name == "shell_exec" && shellAvailable) {
            QString       command;
            QJsonDocument argsDoc = QJsonDocument::fromJson(args.toUtf8());
            if (argsDoc.isObject()) command = argsDoc.object()["command"].toString();
            if (command.isEmpty()) command = args;

            if (isCommandBlocked(command)) {
                submitToolResultBatched(id, "shell_exec", "该命令被安全策略拦截，无法执行。请换一种方式。");
                emit shellCommandFinished(id, false, "命令被安全策略拦截", command);
            } else {
                m_pendingShellExecs.append({id, command});
                emit shellCommandPending(id, command);
            }
        }
    }

    // 分发中被取消时补齐未完成的调用
    if (m_cancelled) {
        resolvePendingToolCalls("本次请求已中断，该工具调用已取消。");
        m_toolCallBatch.clear();
        return;
    }

    // 全是占位结果时立即写回历史
    tryFlushToolBatch();
}

void ChatBot::executeTavilySearch(const QString& toolCallId, const QString& query) {
    info("Tavily 搜索: {}", query.toStdString());
    emit tavilySearchStarted(toolCallId, query);

    QJsonObject body;
    body["query"]        = query;
    body["search_depth"] = m_tavilySearchDepth;
    body["max_results"]  = m_tavilyMaxResults;

    QNetworkRequest request;
    request.setUrl(QUrl("https://api.tavily.com/search"));
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setRawHeader("Authorization", QString("Bearer %1").arg(m_tavilyApiKey).toUtf8());
    request.setRawHeader("User-Agent", "PenMods ChatBot/1.0");

    QByteArray     data  = QJsonDocument(body).toJson(QJsonDocument::Compact);
    QNetworkReply* reply = m_networkManager->post(request, data);
    m_activeReplies.append(reply);

    connect(reply, &QNetworkReply::finished, this, [this, reply, toolCallId, query]() {
        if (!m_activeReplies.contains(reply)) {
            reply->deleteLater();
            return;
        }
        m_activeReplies.removeAll(reply);
        reply->deleteLater();

        if (m_cancelled) return;

        if (reply->error() != QNetworkReply::NoError) {
            QString errMsg = reply->errorString();
            warn("Tavily 搜索失败: {}", errMsg.toStdString());
            submitToolResultBatched(toolCallId, "tavily_search", "搜索失败: " + errMsg);
            emit tavilySearchFinished(toolCallId, false, errMsg, errMsg);
            return;
        }

        QByteArray    raw = reply->readAll();
        QJsonDocument doc = QJsonDocument::fromJson(raw);
        if (!doc.isObject()) {
            submitToolResultBatched(toolCallId, "tavily_search", "搜索失败: 响应格式错误");
            emit tavilySearchFinished(toolCallId, false, "响应格式错误", "响应格式错误");
            return;
        }

        QJsonArray results = doc.object()["results"].toArray();
        if (results.isEmpty()) {
            submitToolResultBatched(toolCallId, "tavily_search", "未找到相关结果");
            emit tavilySearchFinished(toolCallId, true, "未找到结果", "未找到相关结果");
            return;
        }

        QString formatted = QString("网络搜索结果（查询: \"%1\"）：\n\n").arg(query);
        int     count     = 0;
        for (const auto& val : results) {
            QJsonObject r  = val.toObject();
            formatted     += QString("%1. %2 (%3)\n   %4\n\n")
                                 .arg(++count)
                                 .arg(r["title"].toString())
                                 .arg(r["url"].toString())
                                 .arg(r["content"].toString());
        }

        submitToolResultBatched(toolCallId, "tavily_search", formatted);
        emit tavilySearchFinished(toolCallId, true, QString("找到 %1 条结果").arg(count), formatted);
        info("Tavily 搜索完成，找到 {} 条结果", count);
    });
}

// -----------------------------------------------------------------------
// Shell Tool
// -----------------------------------------------------------------------

void ChatBot::initShellTool() {
    json aiCfg = mod::Config::getInstance().read("ai");
    if (!aiCfg.contains("shell_tool") || !aiCfg["shell_tool"].is_object()) return;

    const auto& st       = aiCfg["shell_tool"];
    m_shellToolEnabled   = st.value("enabled", false);
    m_shellToolTimeoutMs = st.value("timeout_ms", 10000);
    m_shellToolMaxOutput = st.value("max_output_bytes", 4096);

    m_shellToolBlocklist.clear();
    if (st.contains("blocklist") && st["blocklist"].is_array()) {
        for (const auto& item : st["blocklist"]) {
            if (item.is_string()) m_shellToolBlocklist.append(QString::fromStdString(item.get<std::string>()));
        }
    } else {
        m_shellToolBlocklist = QStringList{
            "rm -rf /",
            "rm -rf /*",
            "mkfs",
            "dd if=",
            ":(){ :|:&",
            "> /dev/sd",
            "chmod -R 777 /",
            "shutdown",
            "reboot",
            "init 0",
            "init 6",
            "halt",
            "fdisk",
            "mount -o remount"
        };
    }
    emit shellToolConfigChanged();
    info("Shell tool 配置已加载, enabled={}", m_shellToolEnabled);
}

void ChatBot::setShellToolEnabled(bool v) {
    if (m_shellToolEnabled == v) return;
    m_shellToolEnabled = v;

    json aiCfg = mod::Config::getInstance().read("ai");
    if (aiCfg.is_null()) aiCfg = json::object();
    if (!aiCfg.contains("shell_tool") || !aiCfg["shell_tool"].is_object()) aiCfg["shell_tool"] = json::object();
    aiCfg["shell_tool"]["enabled"] = v;
    mod::Config::getInstance().write("ai", aiCfg, true);
    emit shellToolConfigChanged();
}

QString ChatBot::getShellToolConfig() {
    json obj;
    obj["enabled"]          = m_shellToolEnabled;
    obj["timeout_ms"]       = m_shellToolTimeoutMs;
    obj["max_output_bytes"] = m_shellToolMaxOutput;
    json bl                 = json::array();
    for (const auto& s : m_shellToolBlocklist) bl.push_back(s.toStdString());
    obj["blocklist"] = bl;
    return QString::fromStdString(obj.dump(2));
}

void ChatBot::setShellToolConfig(const QString& configJson) {
    QJsonDocument doc = QJsonDocument::fromJson(configJson.toUtf8());
    if (!doc.isObject()) return;

    QJsonObject obj = doc.object();
    if (obj.contains("enabled")) m_shellToolEnabled = obj["enabled"].toBool();
    if (obj.contains("timeout_ms")) m_shellToolTimeoutMs = obj["timeout_ms"].toInt(10000);
    if (obj.contains("max_output_bytes")) m_shellToolMaxOutput = obj["max_output_bytes"].toInt(4096);
    if (obj.contains("blocklist") && obj["blocklist"].isArray()) {
        m_shellToolBlocklist.clear();
        for (const auto& v : obj["blocklist"].toArray()) {
            m_shellToolBlocklist.append(v.toString());
        }
    }

    json aiCfg = mod::Config::getInstance().read("ai");
    if (aiCfg.is_null()) aiCfg = json::object();
    if (!aiCfg.contains("shell_tool") || !aiCfg["shell_tool"].is_object()) aiCfg["shell_tool"] = json::object();
    aiCfg["shell_tool"]["enabled"]          = m_shellToolEnabled;
    aiCfg["shell_tool"]["timeout_ms"]       = m_shellToolTimeoutMs;
    aiCfg["shell_tool"]["max_output_bytes"] = m_shellToolMaxOutput;
    json bl                                 = json::array();
    for (const auto& s : m_shellToolBlocklist) bl.push_back(s.toStdString());
    aiCfg["shell_tool"]["blocklist"] = bl;
    mod::Config::getInstance().write("ai", aiCfg, true);
    emit shellToolConfigChanged();
    info("Shell tool 配置已保存");
}

bool ChatBot::isCommandBlocked(const QString& command) {
    QString trimmed = command.trimmed();
    for (const auto& pattern : m_shellToolBlocklist) {
        if (trimmed.contains(pattern, Qt::CaseInsensitive)) return true;
    }
    return false;
}

QString ChatBot::truncateOutput(const QString& output, int maxBytes) {
    if (output.toUtf8().size() <= maxBytes) return output;
    QByteArray truncated = output.toUtf8().left(maxBytes);
    return QString::fromUtf8(truncated) + "\n... [输出已截断]";
}

void ChatBot::approveShellCommand(const QString& toolCallId) {
    for (int i = 0; i < m_pendingShellExecs.size(); ++i) {
        if (m_pendingShellExecs[i].toolCallId == toolCallId) {
            auto pending = m_pendingShellExecs.takeAt(i);
            executeShellCommand(pending.toolCallId, pending.command);
            return;
        }
    }
    warn("approveShellCommand: toolCallId 未找到: {}", toolCallId.toStdString());
}

void ChatBot::denyShellCommand(const QString& toolCallId) {
    for (int i = 0; i < m_pendingShellExecs.size(); ++i) {
        if (m_pendingShellExecs[i].toolCallId == toolCallId) {
            auto pending = m_pendingShellExecs.takeAt(i);

            // 将未完成的 batch 条目标记为已拒绝
            for (auto& entry : m_toolCallBatch) {
                if (!entry.resolved) {
                    entry.resolved = true;
                    entry.result   = "用户拒绝了该操作";
                }
            }
            for (const auto& entry : m_toolCallBatch) {
                MessageData toolMsg;
                toolMsg.role       = "tool";
                toolMsg.toolCallId = entry.id;
                toolMsg.content    = entry.result;
                currentMessages().append(toolMsg);
                if (currentMessages().size() > MAX_HISTORY_SIZE) currentMessages().removeFirst();
            }
            m_toolCallBatch.clear();
            m_sessions[m_currentSessionId].updatedAt = QDateTime::currentDateTime().toString(Qt::ISODate);
            saveSessions();
            emit messagesChanged();

            emit shellCommandFinished(toolCallId, false, "用户拒绝执行", pending.command);
            cancelRequest();
            return;
        }
    }
    warn("denyShellCommand: toolCallId 未找到: {}", toolCallId.toStdString());
}

void ChatBot::executeShellCommand(const QString& toolCallId, const QString& command) {
    // 如果该 toolCallId 已有活跃执行，先清理
    if (m_activeShellExecs.contains(toolCallId)) {
        warn("executeShellCommand: toolCallId {} 已有正在执行的命令，将被替换", toolCallId.toStdString());
        cleanupShellExec(toolCallId);
    }

    info("启动异步 shell 命令: {}", command.toStdString());
    emit shellCommandStarted(toolCallId, command);

    auto* exec       = new ActiveShellExec;
    exec->toolCallId = toolCallId;
    exec->command    = command;
    exec->process    = new QProcess(this);
    exec->process->setProgram("/bin/sh");
    exec->process->setArguments({"-c", command});
    exec->process->setProcessChannelMode(QProcess::SeparateChannels);

    // 收集 stdout
    connect(exec->process, &QProcess::readyReadStandardOutput, this, [this, toolCallId]() {
        auto* e = m_activeShellExecs.value(toolCallId);
        if (!e) return;
        e->stdoutBuf += QString::fromUtf8(e->process->readAllStandardOutput());
    });

    // 收集 stderr
    connect(exec->process, &QProcess::readyReadStandardError, this, [this, toolCallId]() {
        auto* e = m_activeShellExecs.value(toolCallId);
        if (!e) return;
        e->stderrBuf += QString::fromUtf8(e->process->readAllStandardError());
    });

    // 进程正常结束
    connect(
        exec->process,
        QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
        this,
        [this, toolCallId](int exitCode, QProcess::ExitStatus status) {
            auto* e = m_activeShellExecs.value(toolCallId);
            if (!e) return;

            // 停掉超时定时器
            if (e->timer) {
                e->timer->stop();
                e->timer->deleteLater();
                e->timer = nullptr;
            }

            QString stdoutStr = truncateOutput(e->stdoutBuf, m_shellToolMaxOutput);
            QString stderrStr = truncateOutput(e->stderrBuf, m_shellToolMaxOutput / 2);

            bool    crashed = (status != QProcess::NormalExit);
            QString resultText;
            resultText += QString("Exit code: %1\n").arg(exitCode);
            if (crashed) resultText += "（进程崩溃）\n";
            if (!stdoutStr.isEmpty()) resultText += "stdout:\n" + stdoutStr + "\n";
            if (!stderrStr.isEmpty()) resultText += "stderr:\n" + stderrStr + "\n";

            bool    success = (exitCode == 0 && !crashed);
            QString summary = success ? "执行成功" : crashed ? "进程崩溃" : QString("退出码: %1").arg(exitCode);

            QString cmd = e->command;
            cleanupShellExec(toolCallId);

            submitToolResultBatched(toolCallId, "shell_exec", resultText);
            emit shellCommandFinished(toolCallId, success, summary, resultText);
            info("Shell 命令完成 [{}], exitCode={}, crashed={}", cmd.toStdString(), exitCode, crashed);
        }
    );

    // 进程启动失败
    connect(exec->process, &QProcess::errorOccurred, this, [this, toolCallId](QProcess::ProcessError err) {
        if (err != QProcess::FailedToStart) return;
        auto* e = m_activeShellExecs.value(toolCallId);
        if (!e) return;

        if (e->timer) {
            e->timer->stop();
            e->timer->deleteLater();
            e->timer = nullptr;
        }

        QString cmd = e->command;
        cleanupShellExec(toolCallId);

        submitToolResultBatched(toolCallId, "shell_exec", "命令启动失败，请检查命令是否正确。");
        emit shellCommandFinished(toolCallId, false, "启动失败", cmd);
        warn("Shell 命令启动失败 [{}]", cmd.toStdString());
    });

    // 超时定时器
    if (m_shellToolTimeoutMs > 0) {
        exec->timer = new QTimer(this);
        exec->timer->setSingleShot(true);
        connect(exec->timer, &QTimer::timeout, this, [this, toolCallId]() {
            auto* e = m_activeShellExecs.value(toolCallId);
            if (!e) return;

            warn("Shell 命令超时 [{}]: {}", toolCallId.toStdString(), e->command.toStdString());

            // kill() 会同步触发 finished，e 可能已失效
            if (e->process->state() != QProcess::NotRunning) {
                e->process->kill();
            }

            // 重新取一次 entry（可能已被清理）
            e = m_activeShellExecs.value(toolCallId);
            if (!e) return;

            // finished 信号未处理，直接处理
            QString resultText  = QString("命令超时（%1ms）。\n").arg(m_shellToolTimeoutMs);
            resultText         += "stdout:\n" + e->stdoutBuf + "\n";
            resultText         += "stderr:\n" + e->stderrBuf + "\n";

            QString cmd = e->command;
            cleanupShellExec(toolCallId);

            submitToolResultBatched(toolCallId, "shell_exec", resultText);
            emit shellCommandFinished(toolCallId, false, "执行超时", resultText);
            warn("Shell 命令超时（进程已结束） [{}]", cmd.toStdString());
        });
        exec->timer->start(m_shellToolTimeoutMs);
    }

    m_activeShellExecs.insert(toolCallId, exec);
    exec->process->start();
    info("Shell 命令已在后台启动 [toolCallId={}]: {}", toolCallId.toStdString(), command.toStdString());
}

void ChatBot::cleanupShellExec(const QString& toolCallId) {
    auto* e = m_activeShellExecs.take(toolCallId);
    if (!e) return;

    if (e->timer) {
        e->timer->stop();
        e->timer->deleteLater();
    }

    if (e->process) {
        e->process->disconnect(this);
        if (e->process->state() != QProcess::NotRunning) {
            e->process->kill();
            e->process->waitForFinished(200);
        }
        e->process->deleteLater();
    }

    delete e;
}

// -----------------------------------------------------------------------
// 数学公式渲染
// -----------------------------------------------------------------------

void ChatBot::initMathRender() {
    json aiCfg = mod::Config::getInstance().read("ai");
    if (!aiCfg.contains("math_render") || !aiCfg["math_render"].is_object()) return;

    const auto& mr      = aiCfg["math_render"];
    m_mathRenderEnabled = mr.value("enabled", false);
    m_mathServerPath    = QString::fromStdString(mr.value("server_path", std::string()));
    emit mathRenderConfigChanged();
    info("Math render 配置已加载, enabled={}, path={}", m_mathRenderEnabled, m_mathServerPath.toStdString());
}

void ChatBot::setMathRenderEnabled(bool v) {
    if (m_mathRenderEnabled == v) return;
    m_mathRenderEnabled = v;

    json aiCfg = mod::Config::getInstance().read("ai");
    if (aiCfg.is_null()) aiCfg = json::object();
    if (!aiCfg.contains("math_render") || !aiCfg["math_render"].is_object()) aiCfg["math_render"] = json::object();
    aiCfg["math_render"]["enabled"] = v;
    mod::Config::getInstance().write("ai", aiCfg, true);
    emit mathRenderConfigChanged();
}

void ChatBot::setMathServerPath(const QString& path) {
    if (m_mathServerPath == path) return;
    m_mathServerPath = path;

    json aiCfg = mod::Config::getInstance().read("ai");
    if (aiCfg.is_null()) aiCfg = json::object();
    if (!aiCfg.contains("math_render") || !aiCfg["math_render"].is_object()) aiCfg["math_render"] = json::object();
    aiCfg["math_render"]["server_path"] = path.toStdString();
    mod::Config::getInstance().write("ai", aiCfg, true);
    emit mathRenderConfigChanged();
}

void ChatBot::setBubbleRenderMode(const QString& mode) {
    if (mode != "full" && mode != "basic" && mode != "plain") return;
    if (m_bubbleRenderMode == mode) return;

    m_bubbleRenderMode = mode;
    json aiCfg         = mod::Config::getInstance().read("ai");
    if (aiCfg.is_null()) aiCfg = json::object();
    aiCfg["bubble_render_mode"] = mode.toStdString();
    mod::Config::getInstance().write("ai", aiCfg, true);
    emit bubbleRenderModeChanged();
}

QString ChatBot::getMathRenderConfig() {
    json obj;
    obj["enabled"]     = m_mathRenderEnabled;
    obj["server_path"] = m_mathServerPath.toStdString();
    return QString::fromStdString(obj.dump(2));
}

void ChatBot::setMathRenderConfig(const QString& configJson) {
    QJsonDocument doc = QJsonDocument::fromJson(configJson.toUtf8());
    if (!doc.isObject()) return;

    QJsonObject obj = doc.object();
    if (obj.contains("enabled")) m_mathRenderEnabled = obj["enabled"].toBool();
    if (obj.contains("server_path")) m_mathServerPath = obj["server_path"].toString();

    json aiCfg = mod::Config::getInstance().read("ai");
    if (aiCfg.is_null()) aiCfg = json::object();
    if (!aiCfg.contains("math_render") || !aiCfg["math_render"].is_object()) aiCfg["math_render"] = json::object();
    aiCfg["math_render"]["enabled"]     = m_mathRenderEnabled;
    aiCfg["math_render"]["server_path"] = m_mathServerPath.toStdString();
    mod::Config::getInstance().write("ai", aiCfg, true);
    emit mathRenderConfigChanged();
    info("Math render 配置已保存");
}

} // namespace mod::chatbot
