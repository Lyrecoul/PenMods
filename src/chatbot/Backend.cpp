// SPDX-License-Identifier: GPL-3.0-only
/*
 * Copyright (C) 2022-present, PenUniverse.
 * This file is part of the PenMods open source project.
 */

#include "chatbot/Backend.h"

#include "common/Event.h"
#include "common/Utils.h"
#include "common/service/Logger.h"
#include "common/util/System.h"
#include "mod/Config.h"
#include "mod/Mod.h"

#include <QDateTime>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QHttpMultiPart>
#include <QHttpPart>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QProcess>
#include <QQmlContext>
#include <QRegularExpression>
#include <QTextDocument>
#include <QTextStream>
#include <QTimer>
#include <QUrl>
#include <QUuid>

namespace mod::chatbot {

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
    info("ChatBot 初始化完成");

    auto& config = mod::Config::getInstance();
    json  aiCfg  = config.read("ai");
    if (!aiCfg.is_null()) m_isStreaming = aiCfg.value("streaming", true);

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

    if (msg.isMultimodal()) {
        QJsonArray contentArr;
        for (const auto& part : msg.parts) {
            QJsonObject p;
            p["type"] = part.type;
            if (part.type == "text") {
                p["text"] = part.text;
            } else if (part.type == "image_url") {
                QJsonObject imgUrl;
                imgUrl["url"]  = part.url;
                p["image_url"] = imgUrl;
            } else if (part.type == "input_audio") {
                QJsonObject audio;
                audio["data"]    = part.data;
                audio["format"]  = part.format;
                p["input_audio"] = audio;
            }
            contentArr.append(p);
        }
        obj["content"] = contentArr;
    } else {
        obj["content"] = msg.content;
    }

    return obj;
}

// -----------------------------------------------------------------------
// 构建 API messages 数组
// -----------------------------------------------------------------------

QJsonArray ChatBot::buildApiMessages(const QVector<MessageData>& history,
                                     const QString&              userText,
                                     const QVector<MessagePart>& userParts) {
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

    return messages;
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
    obj["role"] = msg.role.toStdString();
    if (!msg.toolCallId.isEmpty()) obj["toolCallId"] = msg.toolCallId.toStdString();
    if (!msg.toolCallsJson.isEmpty()) obj["toolCallsJson"] = msg.toolCallsJson.toStdString();

    if (msg.isMultimodal()) {
        json partsArr = json::array();
        for (const auto& part : msg.parts) {
            json p;
            p["type"]   = part.type.toStdString();
            p["text"]   = part.text.toStdString();
            p["url"]    = part.url.toStdString();
            p["data"]   = part.data.toStdString();
            p["format"] = part.format.toStdString();
            partsArr.push_back(p);
        }
        obj["parts"] = partsArr;
    } else {
        obj["content"] = msg.content.toStdString();
    }
    return obj;
}

static MessageData messageDataFromJson(const json& obj) {
    MessageData msg;
    msg.role = QString::fromStdString(obj.value("role", ""));
    if (obj.contains("toolCallId")) msg.toolCallId = QString::fromStdString(obj["toolCallId"]);
    if (obj.contains("toolCallsJson")) msg.toolCallsJson = QString::fromStdString(obj["toolCallsJson"]);

    if (obj.contains("parts") && obj["parts"].is_array()) {
        for (const auto& p : obj["parts"]) {
            MessagePart part;
            part.type   = QString::fromStdString(p.value("type", "text"));
            part.text   = QString::fromStdString(p.value("text", ""));
            part.url    = QString::fromStdString(p.value("url", ""));
            part.data   = QString::fromStdString(p.value("data", ""));
            part.format = QString::fromStdString(p.value("format", ""));
            msg.parts.append(part);
        }
    } else {
        msg.content = QString::fromStdString(obj.value("content", ""));
    }
    return msg;
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
        file.write(root.dump(4).c_str());
        file.close();
    } else {
        error("无法写入 sessions.json: {}", sessionsFilePath().toStdString());
    }
}

// -----------------------------------------------------------------------
// 加载 sessions.json
// -----------------------------------------------------------------------

void ChatBot::initSessions() {
    auto  path = sessionsFilePath();
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
                                session.messages.append(msg);
                            } else {
                                session.messages.append(messageDataFromJson(msgObj));
                            }
                        }
                    }

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
}

bool ChatBot::isAvailable() { return !m_apiKey.isEmpty(); }

// -----------------------------------------------------------------------
// sendMessage（纯文本，兼容旧接口）
// -----------------------------------------------------------------------

void ChatBot::sendMessage(const QString& message, const QString& fileRefs) {
    QVector<MessagePart> extraParts;

    // 文件引用转文本 part（保持原有行为）
    if (!fileRefs.isEmpty()) {
        QJsonDocument doc = QJsonDocument::fromJson(fileRefs.toUtf8());
        if (doc.isArray() && !doc.array().isEmpty()) {
            QString contextText = "用户引用了以下文件作为代码审查/分析的上下文：\n\n";
            for (const auto& fileVal : doc.array()) {
                QJsonObject file     = fileVal.toObject();
                QString     path     = file["path"].toString();
                QString     content  = file["content"].toString();
                QString     lang     = file["language"].toString();
                contextText         += QString("---\n## 文件: %1\n```%2\n%3\n```\n\n").arg(path, lang, content);
            }
            // 在 system 消息之后插入文件上下文（通过在历史前追加一条 system 消息实现）
            MessageData ctxMsg;
            ctxMsg.role    = "system";
            ctxMsg.content = contextText;
            currentMessages().append(ctxMsg);
            debug("已附加 {} 个文件作为上下文", doc.array().size());
        }
    }

    QJsonArray apiMessages = buildApiMessages(currentMessages(), message);

    // 记录用户消息
    MessageData userMsg;
    userMsg.role    = "user";
    userMsg.content = message;
    currentMessages().append(userMsg);
    if (currentMessages().size() > MAX_HISTORY_SIZE) currentMessages().removeFirst();

    m_sessions[m_currentSessionId].updatedAt = QDateTime::currentDateTime().toString(Qt::ISODate);
    saveSessions();
    emit messagesChanged();

    makeApiRequest(apiMessages);
}

// -----------------------------------------------------------------------
// sendMessageWithMedia（多模态：图片 / 音频）
// mediaParts JSON 格式（数组）：
//   [{"type":"image_url","url":"https://..."},
//    {"type":"input_audio","data":"<base64>","format":"mp3"}]
// -----------------------------------------------------------------------

void ChatBot::sendMessageWithMedia(const QString& message, const QString& mediaParts) {
    QVector<MessagePart> parts;

    QJsonDocument doc = QJsonDocument::fromJson(mediaParts.toUtf8());
    if (doc.isArray()) {
        for (const auto& val : doc.array()) {
            QJsonObject obj = val.toObject();
            MessagePart part;
            part.type = obj["type"].toString();
            if (part.type == "image_url") {
                part.url = obj["url"].toString();
            } else if (part.type == "input_audio") {
                part.data   = obj["data"].toString();
                part.format = obj["format"].toString("mp3");
            } else if (part.type == "text") {
                part.text = obj["text"].toString();
            }
            if (!part.type.isEmpty()) parts.append(part);
        }
    }

    bool hasImageParts = false;
    for (const auto& p : parts) {
        if (p.type == "image_url") {
            hasImageParts = true;
            break;
        }
    }

    QString effectiveMessage = message;

    if (!m_capVision && hasImageParts) {
        if (!m_proxyVisionModelId.isEmpty()) {
            emit proxyVisionStarted();
            QString desc = callVisionProxy(parts);
            if (!desc.isEmpty()) {
                effectiveMessage = "[图片描述]\n" + desc + "\n\n[用户消息]\n" + message;
                // 保留非 image 的 parts（如 text、audio）
                parts.erase(std::remove_if(parts.begin(), parts.end(), [](const MessagePart& p) {
                    return p.type == "image_url";
                }), parts.end());
            } else {
                emit errorOccurred("视觉代理模型调用失败，已移除图片仅发送文字");
                parts.erase(std::remove_if(parts.begin(), parts.end(), [](const MessagePart& p) {
                    return p.type == "image_url";
                }), parts.end());
                effectiveMessage = "[图片分析失败]\n" + message;
            }
        } else {
            emit errorOccurred("当前模型不支持视觉，请先在模型设置中配置「视觉代理模型」");
            parts.erase(std::remove_if(parts.begin(), parts.end(), [](const MessagePart& p) {
                return p.type == "image_url";
            }), parts.end());
        }
        emit proxyVisionCompleted(effectiveMessage);
    }

    QJsonArray apiMessages = buildApiMessages(currentMessages(), effectiveMessage, parts);

    // 记录用户消息（含多模态）
    MessageData userMsg;
    userMsg.role  = "user";
    userMsg.parts = parts;
    if (!effectiveMessage.isEmpty()) {
        MessagePart textPart;
        textPart.type = "text";
        textPart.text = effectiveMessage;
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
// callVisionProxy: 用视觉代理模型提取图片文字描述（同步）
// 自动检测 OpenAI / Anthropic 格式
// -----------------------------------------------------------------------

QString ChatBot::callVisionProxy(const QVector<MessagePart>& parts) {
    std::string proxyId = m_proxyVisionModelId.toStdString();

    json proxyModel;
    for (const auto& model : m_modelsData["models"]) {
        if (model["id"] == proxyId) {
            proxyModel = model;
            break;
        }
    }
    if (proxyModel.empty()) {
        warn("代理视觉模型未找到: {}", proxyId);
        return {};
    }

    QString proxyEndpoint = QString::fromStdString(proxyModel["endpoint"]);
    QString proxyApiKey   = QString::fromStdString(proxyModel["apiKey"]);
    QString proxyModelId  = QString::fromStdString(proxyModel["modelId"]);

    bool isAnthropic = proxyEndpoint.contains("/messages");

    // 构造请求体
    QJsonArray messages;
    QJsonObject userMsg;
    userMsg["role"] = "user";

    if (isAnthropic) {
        // Anthropic 格式
        QJsonArray contentArray;
        for (const auto& p : parts) {
            if (p.type == "image_url") {
                QString base64Data = p.url;
                if (base64Data.startsWith("data:image/jpeg;base64,"))
                    base64Data = base64Data.mid(23);
                else                 if (base64Data.startsWith("data:image/png;base64,"))
                    base64Data = base64Data.mid(22);

                QJsonObject imgObj;
                imgObj["type"] = "image";
                QJsonObject source;
                source["type"]       = "base64";
                source["media_type"] = "image/jpeg";
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
        // OpenAI 格式
        QJsonArray contentArray;
        for (const auto& p : parts) {
            if (p.type == "image_url") {
                QJsonObject imgObj;
                imgObj["type"] = "image_url";
                QJsonObject imgUrl;
                imgUrl["url"] = p.url;
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
    if (isAnthropic)
        requestBody["max_tokens"] = 4096;
    if (proxyModel.contains("temperature") && proxyModel["temperature"].is_number())
        requestBody["temperature"] = proxyModel["temperature"].get<double>();

    QJsonDocument jsonDoc(requestBody);
    QByteArray    data = jsonDoc.toJson();

    QUrl            proxyUrl(proxyEndpoint);
    QNetworkRequest request(proxyUrl);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    if (isAnthropic) {
        request.setRawHeader("x-api-key", proxyApiKey.toUtf8());
        request.setRawHeader("anthropic-version", "2023-06-01");
    } else if (!proxyApiKey.isEmpty()) {
        request.setRawHeader("Authorization", ("Bearer " + proxyApiKey).toUtf8());
    }

    info("调用视觉代理模型 {} ({} {}): {}",
         proxyId, isAnthropic ? "Anthropic" : "OpenAI", proxyEndpoint.toStdString(),
         m_proxyVisionPrompt.left(30).toStdString());

    QNetworkReply* reply = m_networkManager->post(request, data);
    QObject::connect(reply, &QNetworkReply::finished, reply, &QNetworkReply::deleteLater);

    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();

    if (reply->error() != QNetworkReply::NoError) {
        warn("视觉代理模型调用失败: {}", reply->errorString().toStdString());
        return {};
    }

    QByteArray  responseData = reply->readAll();
    QJsonObject responseJson = QJsonDocument::fromJson(responseData).object();

    if (responseJson.contains("error")) {
        warn("视觉代理模型返回错误: {}", responseJson["error"].toObject()["message"].toString().toStdString());
        return {};
    }

    QString desc;
    if (isAnthropic) {
        QJsonArray content = responseJson["content"].toArray();
        if (content.isEmpty()) {
            warn("视觉代理模型返回空 content，完整响应: {}", responseData.constData());
            return {};
        }
        desc = content[0].toObject()["text"].toString();
    } else {
        QJsonArray choices = responseJson["choices"].toArray();
        if (choices.isEmpty()) {
            warn("视觉代理模型返回空 choices，完整响应: {}", responseData.constData());
            return {};
        }
        desc = choices[0].toObject()["message"].toObject()["content"].toString();
    }

    if (desc.isEmpty()) {
        warn("视觉代理模型返回空文本");
        return {};
    }

    info("视觉代理模型返回 {} 个字符", desc.length());
    return desc.trimmed();
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

    // 以当前历史重新发起请求（工具结果作为最后一条 history 消息，不再追加新 user 消息）
    QJsonArray  apiMessages;
    QJsonObject sysMsg;
    sysMsg["role"]    = "system";
    sysMsg["content"] = m_defaultPrompt;
    apiMessages.append(sysMsg);
    for (const auto& msg : currentMessages()) {
        apiMessages.append(messageToJson(msg));
    }

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

    QJsonArray  apiMessages;
    QJsonObject sysMsg;
    sysMsg["role"]    = "system";
    sysMsg["content"] = m_defaultPrompt;
    apiMessages.append(sysMsg);
    for (const auto& msg : currentMessages()) {
        apiMessages.append(messageToJson(msg));
    }

    m_sessions[m_currentSessionId].updatedAt = QDateTime::currentDateTime().toString(Qt::ISODate);
    saveSessions();
    emit messagesChanged();
    emit toolBatchFlushed();

    makeApiRequest(apiMessages);
}

// -----------------------------------------------------------------------
// makeApiRequest：构建请求体并发送
// -----------------------------------------------------------------------

void ChatBot::makeApiRequest(const QJsonArray& messages) {
    // 递增序列号使旧请求的回调自动失效
    int seq = ++m_requestSeq;

    // 清理旧 reply，安全地断开连接以释放底层网络资源
    for (QNetworkReply* reply : m_activeReplies) {
        if (reply) {
            reply->disconnect(this);
            if (!reply->isFinished()) reply->abort();
            reply->deleteLater();
        }
    }
    m_activeReplies.clear();

    m_cancelled = false;

    if (m_apiKey.isEmpty()) {
        emit errorOccurred("API 密钥未设置\n请进入「设置」页面配置有效的 API 密钥后重试");
        return;
    }

    QJsonObject requestBody;
    requestBody["model"]       = m_model;
    requestBody["messages"]    = messages;
    requestBody["temperature"] = m_temperature;
    requestBody["stream"]      = m_isStreaming;

    // 合并 extraParams（模型自定义参数，可覆盖默认字段）
    if (!m_extraParams.empty()) {
        QJsonDocument epDoc = QJsonDocument::fromJson(QByteArray::fromStdString(m_extraParams.dump()));
        if (epDoc.isObject()) {
            const auto epObj = epDoc.object();
            for (auto it = epObj.constBegin(); it != epObj.constEnd(); ++it) requestBody[it.key()] = it.value();
        }
    }

    // 注入工具定义
    if (m_capToolCall && ((m_tavilyEnabled && !m_tavilyApiKey.isEmpty()) || m_shellToolEnabled)) {
        injectToolDefinitions(requestBody);
    }

    QJsonDocument requestDoc(requestBody);
    QByteArray    requestData = requestDoc.toJson(QJsonDocument::Compact);
    debug("请求数据: {}", QString(requestData).toStdString());

    QNetworkRequest request;
    request.setUrl(QUrl(m_apiEndpoint));
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setRawHeader("Authorization", QString("Bearer %1").arg(m_apiKey).toUtf8());
    request.setRawHeader("Content-Type", "application/json");
    request.setRawHeader("User-Agent", "PenMods ChatBot/1.0");

    QNetworkReply* reply = m_networkManager->post(request, requestData);
    m_activeReplies.append(reply);

    if (m_isStreaming) {
        m_currentStreamBuffer.clear();
        m_responseBuffer.clear();
        m_toolCallsBuffer.clear();
        emit streamStart();
    }

    connect(reply, &QNetworkReply::finished, this, [this, reply, seq]() {
        if (seq != m_requestSeq) { reply->deleteLater(); return; }
        m_activeReplies.removeAll(reply);
        handleNetworkReply(reply, m_isStreaming);
    });

    if (m_isStreaming) {
        connect(reply, &QNetworkReply::readyRead, this, [this, reply, seq]() {
            if (seq != m_requestSeq) return;
            QByteArray data = reply->readAll();
            if (data.isEmpty()) return;

            m_responseBuffer  += QString::fromUtf8(data);
            QStringList lines  = m_responseBuffer.split("\n", Qt::SkipEmptyParts);

            for (const QString& line : lines) {
                QString trimmedLine = line.trimmed();
                if (!trimmedLine.startsWith("data: ")) continue;

                QString jsonData = trimmedLine.mid(6);
                if (jsonData.trimmed() == "[DONE]") {
                    // 在 finished 信号之前就把响应写入历史，
                    // 防止 regenerateMessage 在 streamEnd 后 finished 前被调用时因索引越界空转
                    if (!m_cancelled) {
                        if (!m_toolCallsBuffer.isEmpty()) {
                            json tcArr = json::array();
                            for (auto it = m_toolCallsBuffer.constBegin(); it != m_toolCallsBuffer.constEnd(); ++it)
                                tcArr.push_back(it.value());
                            QString     toolCallsJson = QString::fromStdString(tcArr.dump());
                            MessageData assistantMsg;
                            assistantMsg.role          = "assistant";
                            assistantMsg.content       = m_currentStreamBuffer;
                            assistantMsg.toolCallsJson = toolCallsJson;
                            currentMessages().append(assistantMsg);
                            if (currentMessages().size() > MAX_HISTORY_SIZE) currentMessages().removeFirst();
                            m_sessions[m_currentSessionId].updatedAt = QDateTime::currentDateTime().toString(Qt::ISODate);
                            saveSessions();
                            emit messagesChanged();
                            dispatchToolCalls(toolCallsJson);
                        } else if (!m_currentStreamBuffer.isEmpty()) {
                            MessageData assistantMsg;
                            assistantMsg.role    = "assistant";
                            assistantMsg.content = m_currentStreamBuffer;
                            currentMessages().append(assistantMsg);
                            if (currentMessages().size() > MAX_HISTORY_SIZE) currentMessages().removeFirst();
                            m_sessions[m_currentSessionId].updatedAt = QDateTime::currentDateTime().toString(Qt::ISODate);
                            saveSessions();
                            emit messagesChanged();
                        }
                    }
                    // 清空 buffer 防止 handleNetworkReply 重复保存
                    m_currentStreamBuffer.clear();
                    m_toolCallsBuffer.clear();
                    emit streamEnd();
                    continue;
                }

                QJsonDocument doc = QJsonDocument::fromJson(jsonData.toUtf8());
                if (!doc.isObject()) continue;

                QJsonObject obj = doc.object();
                if (!obj.contains("choices") || !obj["choices"].isArray()) continue;

                QJsonArray choices = obj["choices"].toArray();
                if (choices.isEmpty()) continue;
                QJsonObject choice = choices[0].toObject();
                if (!choice.contains("delta")) continue;

                QJsonObject delta = choice["delta"].toObject();

                if (delta.contains("content") && delta["content"].isString()) {
                    QString content = delta["content"].toString();
                    if (!content.isEmpty()) {
                        emit streamChunk(content);
                        m_currentStreamBuffer += content;
                    }
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

            if (m_responseBuffer.endsWith("\n")) {
                m_responseBuffer.clear();
            } else {
                int lastNewline = m_responseBuffer.lastIndexOf("\n");
                if (lastNewline != -1 && lastNewline < m_responseBuffer.length() - 1)
                    m_responseBuffer = m_responseBuffer.mid(lastNewline + 1);
                else m_responseBuffer.clear();
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
            if (!m_toolCallsBuffer.isEmpty()) {
                json tcArr = json::array();
                for (auto it = m_toolCallsBuffer.constBegin(); it != m_toolCallsBuffer.constEnd(); ++it)
                    tcArr.push_back(it.value());
                QString     toolCallsJson = QString::fromStdString(tcArr.dump());
                MessageData assistantMsg;
                assistantMsg.role          = "assistant";
                if (!m_cancelled) assistantMsg.content = m_currentStreamBuffer;
                assistantMsg.toolCallsJson = toolCallsJson;
                if (!m_cancelled) {
                    currentMessages().append(assistantMsg);
                    if (currentMessages().size() > MAX_HISTORY_SIZE) currentMessages().removeFirst();
                    m_sessions[m_currentSessionId].updatedAt = QDateTime::currentDateTime().toString(Qt::ISODate);
                    saveSessions();
                    emit messagesChanged();
                }
                dispatchToolCalls(toolCallsJson);
            } else if (!m_currentStreamBuffer.isEmpty()) {
                if (!m_cancelled) {
                    MessageData assistantMsg;
                    assistantMsg.role    = "assistant";
                    assistantMsg.content = m_currentStreamBuffer;
                    currentMessages().append(assistantMsg);
                    if (currentMessages().size() > MAX_HISTORY_SIZE) currentMessages().removeFirst();
                    m_sessions[m_currentSessionId].updatedAt = QDateTime::currentDateTime().toString(Qt::ISODate);
                    saveSessions();
                    emit messagesChanged();
                }
            }
        } else {
            QByteArray    response = reply->readAll();
            QJsonDocument doc      = QJsonDocument::fromJson(response);
            if (!doc.isObject()) {
                reply->deleteLater();
                return;
            }

            QJsonObject obj = doc.object();
            if (!obj.contains("choices") || !obj["choices"].isArray()) {
                reply->deleteLater();
                return;
            }

            QJsonObject choice  = obj["choices"].toArray()[0].toObject();
            QJsonObject message = choice["message"].toObject();

            if (message.contains("tool_calls") && message["tool_calls"].isArray()) {
                QJsonDocument tcDoc(message["tool_calls"].toArray());
                QString       toolCallsJson = QString(tcDoc.toJson(QJsonDocument::Compact));
                MessageData   assistantMsg;
                assistantMsg.role          = "assistant";
                if (!m_cancelled) assistantMsg.content = message["content"].toString();
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
                QString     content = message["content"].toString();
                if (!m_cancelled) {
                    MessageData assistantMsg;
                    assistantMsg.role    = "assistant";
                    assistantMsg.content = content;
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

        if (isStream) emit streamEnd();

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
        else if (reply->error() == QNetworkReply::ConnectionRefusedError
                 || reply->error() == QNetworkReply::HostNotFoundError || reply->error() == QNetworkReply::TimeoutError)
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
    for (QNetworkReply* reply : m_activeReplies) {
        if (reply && !reply->isFinished()) reply->abort();
    }
    m_activeReplies.clear();

    currentMessages().clear();
    m_sessions[m_currentSessionId].updatedAt = QDateTime::currentDateTime().toString(Qt::ISODate);
    saveSessions();
    emit messagesChanged();
}

void ChatBot::cancelRequest() {
    m_cancelled = true;

    for (QNetworkReply* reply : m_activeReplies) {
        if (reply && !reply->isFinished()) reply->abort();
    }
    m_activeReplies.clear();

    QStringList shellKeys;
    for (auto it = m_activeShellExecs.constBegin(); it != m_activeShellExecs.constEnd(); ++it)
        shellKeys.append(it.key());
    for (const auto& key : shellKeys)
        cleanupShellExec(key);

    while (!m_pendingShellExecs.isEmpty()) {
        auto pending = m_pendingShellExecs.takeFirst();
        emit shellCommandFinished(pending.toolCallId, false, "用户取消了请求", pending.command);
    }

    m_currentStreamBuffer.clear();
    m_responseBuffer.clear();
    m_toolCallsBuffer.clear();
    m_toolCallBatch.clear();

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
        if (msg.isMultimodal()) {
            QVariantList partsList;
            for (const auto& part : msg.parts) {
                QVariantMap pm;
                pm["type"]   = part.type;
                pm["text"]   = part.text;
                pm["url"]    = part.url;
                pm["format"] = part.format;
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
    if (!aiCfg.contains("chatbot")) aiCfg["chatbot"] = json::object();
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
        defaultModel["id"]            = m_model.toStdString();
        defaultModel["name"]          = "DeepSeek Chat";
        defaultModel["provider"]      = "DeepSeek";
        defaultModel["endpoint"]      = m_apiEndpoint.toStdString();
        defaultModel["apiKey"]        = m_apiKey.toStdString();
        defaultModel["modelId"]       = m_model.toStdString();
        defaultModel["temperature"]   = m_temperature;
        defaultModel["extraParams"]   = json::object();
        defaultModel["proxyVisionModelId"] = "";
        defaultModel["proxyVisionPrompt"]  = "请详细描述这张图片的内容。如果图片中有文字，请完整转录。";
        m_modelsData["models"]        = json::array({defaultModel});
        m_modelsData["activeModelId"] = m_model.toStdString();
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
    if (modelObj.contains("temperature") && modelObj["temperature"].is_number())
        m_temperature = modelObj["temperature"];
    if (modelObj.contains("extraParams") && modelObj["extraParams"].is_object())
        m_extraParams = modelObj["extraParams"];
    else m_extraParams = json::object();

    m_maxContextSize = modelObj.value("maxContextSize", 0);

    m_proxyVisionModelId = QString::fromStdString(modelObj.value("proxyVisionModelId", ""));
    m_proxyVisionPrompt  = QString::fromStdString(
        modelObj.value("proxyVisionPrompt", "请详细描述这张图片的内容。如果图片中有文字，请完整转录。")
    );

    m_capText      = true;
    m_capVision    = false;
    m_capAudio     = false;
    m_capToolCall  = false;
    m_capReasoning = false;
    if (modelObj.contains("capabilities") && modelObj["capabilities"].is_object()) {
        const auto& cap = modelObj["capabilities"];
        m_capText       = cap.value("text", true);
        m_capVision     = cap.value("vision", false);
        m_capAudio      = cap.value("audio", false);
        m_capToolCall   = cap.value("toolCall", false);
        m_capReasoning  = cap.value("reasoning", false);
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
    newModel["id"]             = id;
    newModel["name"]           = input["name"].toString().toStdString();
    newModel["provider"]       = input["provider"].toString().toStdString();
    newModel["endpoint"]       = endpoint;
    newModel["apiKey"]         = input["apiKey"].toString().toStdString();
    newModel["modelId"]        = modelId;
    newModel["temperature"]    = input.contains("temperature") ? input["temperature"].toDouble() : 0.7;
    newModel["maxContextSize"] = input.contains("maxContextSize") ? input["maxContextSize"].toInt() : 0;

    // capabilities
    {
        json cap;
        cap["text"]      = true;
        cap["vision"]    = false;
        cap["audio"]     = false;
        cap["toolCall"]  = false;
        cap["reasoning"] = false;
        if (input.contains("capabilities") && input["capabilities"].isObject()) {
            const auto qcap = input["capabilities"].toObject();
            if (qcap.contains("text")) cap["text"] = qcap["text"].toBool(true);
            if (qcap.contains("vision")) cap["vision"] = qcap["vision"].toBool(false);
            if (qcap.contains("audio")) cap["audio"] = qcap["audio"].toBool(false);
            if (qcap.contains("toolCall")) cap["toolCall"] = qcap["toolCall"].toBool(false);
            if (qcap.contains("reasoning")) cap["reasoning"] = qcap["reasoning"].toBool(false);
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
    newModel["proxyVisionPrompt"]  = input["proxyVisionPrompt"]
                                         .toString()
                                         .toStdString();

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
                emit temperatureChanged();
            }
            // 清理其他模型对被删除模型的视觉代理引用
            for (auto& model : models) {
                if (model.value("proxyVisionModelId", "") == id)
                    model["proxyVisionModelId"] = "";
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
    if (!m_sessions.contains(sessionId)) {
        warn("删除会话失败：未找到 id={}", sessionId.toStdString());
        return false;
    }
    if (m_sessions.size() <= 1) {
        warn("不能删除唯一的会话");
        return false;
    }

    m_sessions.remove(sessionId);
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

    if (m_tavilyEnabled && !m_tavilyApiKey.isEmpty()) {
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

        QJsonObject tool;
        tool["type"]     = "function";
        tool["function"] = func;
        tools.append(tool);
    }

    if (m_shellToolEnabled) {
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

        QJsonObject tool;
        tool["type"]     = "function";
        tool["function"] = func;
        tools.append(tool);
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

    // 注册所有已知工具调用到 batch 中
    for (const auto& val : arr) {
        QJsonObject tc   = val.toObject();
        QString     id   = tc["id"].toString();
        QString     name = tc["function"].toObject()["name"].toString();
        if (name == "tavily_search" || name == "shell_exec") {
            m_toolCallBatch.append({id, name, false, ""});
        }
    }

    if (m_toolCallBatch.isEmpty()) {
        emit toolCallReceived(toolCallsJson);
        return;
    }

    // 分发执行
    for (const auto& val : arr) {
        if (m_cancelled) return;
        QJsonObject tc   = val.toObject();
        QString     name = tc["function"].toObject()["name"].toString();
        QString     id   = tc["id"].toString();
        QString     args = tc["function"].toObject()["arguments"].toString();

        if (name == "tavily_search") {
            QString       query;
            QJsonDocument argsDoc = QJsonDocument::fromJson(args.toUtf8());
            if (argsDoc.isObject()) query = argsDoc.object()["query"].toString();
            if (query.isEmpty()) query = args;
            executeTavilySearch(id, query);
        } else if (name == "shell_exec") {
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
        m_shellToolBlocklist = QStringList{"rm -rf /",
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
                                           "mount -o remount"};
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

            // 将所有未完成的 batch 条目标记为已拒绝，并写入 history，
            // 避免下次 sendMessage/editMessage 时 API 看到 tool_calls 却无对应 tool_result 而报 Bad request
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

    auto* exec         = new ActiveShellExec;
    exec->toolCallId   = toolCallId;
    exec->command      = command;
    exec->process      = new QProcess(this);
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
    connect(exec->process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this, toolCallId](int exitCode, QProcess::ExitStatus status) {
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
    });

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

            // 杀掉进程。kill() 会在同线程同步触发 finished 信号，
            // finished 处理器会调用 cleanupShellExec 删除 e，所以 kill 后 e 可能已失效。
            if (e->process->state() != QProcess::NotRunning) {
                e->process->kill();
            }

            // 检查 entry 是否还在（可能在 kill() 同步触发的 finished 信号中被清理了）
            e = m_activeShellExecs.value(toolCallId);
            if (!e) return;

            // finished 信号未处理（进程已自行结束但信号未传递），直接处理
            QString resultText = QString("命令超时（%1ms）。\n").arg(m_shellToolTimeoutMs);
            resultText += "stdout:\n" + e->stdoutBuf + "\n";
            resultText += "stderr:\n" + e->stderrBuf + "\n";

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
            // 异步场景下不使用 waitForFinished，避免阻塞
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
