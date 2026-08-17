// SPDX-License-Identifier: GPL-3.0-only
/*
 * Copyright (C) 2022-present, PenUniverse.
 * This file is part of the PenMods open source project.
 */

#pragma once

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QMutex>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QObject>
#include <QProcess>
#include <QQueue>
#include <QThreadPool>
#include <QSet>
#include <QTimer>
#include <QVector>

#include <nlohmann/json.hpp>
using json = nlohmann::json;

#include "common/service/Logger.h"

namespace mod::chatbot {

// 消息内容部分（多模态）
struct MessagePart {
    QString type;   // "text" | "image_url" | "input_audio"
    QString text;   // type=text
    QString url;    // type=image_url：HTTP URL 或 data:image/...;base64,...
    QString data;   // type=input_audio：base64 音频数据
    QString format; // type=input_audio：格式 (mp3/wav/ogg 等)
};

// 单条消息（纯文本或多模态）
struct MessageData {
    QString              role;          // "user" | "assistant" | "system" | "tool"
    QString              content;       // 纯文本（无多模态时使用）
    QString              reasoning;     // 模型推理摘要（不发送回 API）
    QVector<MessagePart> parts;         // 多模态内容（非空时优先于 content）
    QString              toolCallId;    // role=tool 时
    QString              toolCallsJson; // role=assistant 且有 tool_calls 时，原始 JSON 字符串

    bool isMultimodal() const { return !parts.isEmpty(); }
};

// 单个会话的完整数据
struct SessionData {
    QString              id;
    QString              title;
    QString              createdAt;
    QString              updatedAt;
    QVector<MessageData> messages;
};

class ChatBot : public QObject, public Singleton<ChatBot>, private Logger {
    Q_OBJECT

    Q_PROPERTY(QString apiKey READ getApiKey WRITE setApiKey NOTIFY apiKeyChanged)
    Q_PROPERTY(QString apiEndpoint READ getApiEndpoint WRITE setApiEndpoint NOTIFY apiEndpointChanged)
    Q_PROPERTY(QString model READ getModel WRITE setModel NOTIFY modelChanged)
    Q_PROPERTY(QString apiProtocol READ getApiProtocol NOTIFY apiProtocolChanged)
    Q_PROPERTY(qreal temperature READ getTemperature WRITE setTemperature NOTIFY temperatureChanged)
    Q_PROPERTY(QString defaultPrompt READ getDefaultPrompt WRITE setDefaultPrompt NOTIFY defaultPromptChanged)
    Q_PROPERTY(bool isStreaming READ getIsStreaming WRITE setIsStreaming NOTIFY isStreamingChanged)
    Q_PROPERTY(bool isAvailable READ isAvailable CONSTANT)
    Q_PROPERTY(QVariantList messages READ getMessages NOTIFY messagesChanged)
    Q_PROPERTY(QString currentSessionId READ getCurrentSessionId NOTIFY sessionSwitched)
    Q_PROPERTY(QVariantMap apiCacheStats READ getApiCacheStats NOTIFY apiCacheStatsChanged)

    // 当前活动模型的能力标志（只读，随模型切换更新）
    Q_PROPERTY(bool capText READ getCapText NOTIFY activeModelCapabilitiesChanged)
    Q_PROPERTY(bool capVision READ getCapVision NOTIFY activeModelCapabilitiesChanged)
    Q_PROPERTY(bool capAudio READ getCapAudio NOTIFY activeModelCapabilitiesChanged)
    Q_PROPERTY(bool capToolCall READ getCapToolCall NOTIFY activeModelCapabilitiesChanged)
    Q_PROPERTY(bool capReasoning READ getCapReasoning NOTIFY activeModelCapabilitiesChanged)

    Q_PROPERTY(QString proxyVisionModelId READ getProxyVisionModelId WRITE setProxyVisionModelId NOTIFY proxyVisionSettingsChanged)
    Q_PROPERTY(QString proxyVisionPrompt READ getProxyVisionPrompt WRITE setProxyVisionPrompt NOTIFY proxyVisionSettingsChanged)

    Q_PROPERTY(bool tavilyEnabled READ getTavilyEnabled WRITE setTavilyEnabled NOTIFY tavilyConfigChanged)
    Q_PROPERTY(bool tavilyConfigured READ getTavilyConfigured NOTIFY tavilyConfigChanged)

    Q_PROPERTY(bool shellToolEnabled READ getShellToolEnabled WRITE setShellToolEnabled NOTIFY shellToolConfigChanged)

    Q_PROPERTY(
        bool mathRenderEnabled READ getMathRenderEnabled WRITE setMathRenderEnabled NOTIFY mathRenderConfigChanged)
    Q_PROPERTY(QString mathServerPath READ getMathServerPath WRITE setMathServerPath NOTIFY mathRenderConfigChanged)

public:
    // 基础接口
    Q_INVOKABLE void    sendMessage(const QString& message, const QString& fileRefs = QString());
    Q_INVOKABLE void    sendMessageWithMedia(const QString& message, const QString& mediaParts);
    Q_INVOKABLE bool    isAvailable();
    Q_INVOKABLE void    reloadConfig();
    Q_INVOKABLE void    sanitizeConfig();
    Q_INVOKABLE void    clearHistory();
    Q_INVOKABLE void    saveMessages();
    Q_INVOKABLE QString markdownToHtml(const QString& markdown);
    Q_INVOKABLE void    parseMarkdownAsync(const QString& markdown, const QString& requestId);
    Q_INVOKABLE void    cancelMarkdownParse(const QString& requestId);
    Q_INVOKABLE void    truncateHistory(int index);
    Q_INVOKABLE void    editMessage(int index, const QString& newContent);
    Q_INVOKABLE void    deleteMessage(int index);
    Q_INVOKABLE void    regenerateMessage(int index);
    Q_INVOKABLE void    cancelRequest();
    Q_INVOKABLE void    resetApiCacheStats();

    // Tool Call 接口
    Q_INVOKABLE void submitToolResult(const QString& toolCallId, const QString& toolName, const QString& result);

    // Shell Tool 接口
    Q_INVOKABLE void    approveShellCommand(const QString& toolCallId);
    Q_INVOKABLE void    denyShellCommand(const QString& toolCallId);
    Q_INVOKABLE QString getShellToolConfig();
    Q_INVOKABLE void    setShellToolConfig(const QString& configJson);

    // 多会话管理
    Q_INVOKABLE QString      getSessions();
    Q_INVOKABLE QString      getCurrentSessionId();
    Q_INVOKABLE bool         switchSession(const QString& sessionId);
    Q_INVOKABLE QString      createSession(const QString& title = QString());
    Q_INVOKABLE bool         deleteSession(const QString& sessionId);
    Q_INVOKABLE bool         renameSession(const QString& sessionId, const QString& newTitle);
    Q_INVOKABLE QVariantList getSessionMessages(const QString& sessionId);

    // 多模型管理
    Q_INVOKABLE QString getModels();
    Q_INVOKABLE bool    addModel(const QString& modelJson);
    Q_INVOKABLE bool    removeModel(const QString& modelId);
    Q_INVOKABLE bool    setActiveModel(const QString& modelId);
    Q_INVOKABLE QString getActiveModel();
    Q_INVOKABLE bool    loadModelsFile();

    // 多提示词管理
    Q_INVOKABLE QString getPrompts();
    Q_INVOKABLE bool    addPrompt(const QString& promptJson);
    Q_INVOKABLE bool    removePrompt(const QString& promptId);
    Q_INVOKABLE bool    setActivePrompt(const QString& promptId);
    Q_INVOKABLE QString getActivePrompt();

    // Tavily 网络搜索
    Q_INVOKABLE QString getTavilyConfig();
    Q_INVOKABLE void    setTavilyConfig(const QString& configJson);

    // 数学公式渲染
    Q_INVOKABLE QString getMathRenderConfig();
    Q_INVOKABLE void    setMathRenderConfig(const QString& configJson);

    // Getter/Setter
    QString      getApiKey() const;
    QString      getApiEndpoint() const;
    QString      getModel() const;
    QString      getApiProtocol() const;
    qreal        getTemperature() const;
    QString      getDefaultPrompt() const;
    bool         getIsStreaming() const;
    QVariantList getMessages() const;
    QVariantMap  getApiCacheStats() const;

    bool getCapText() const { return m_capText; }
    bool getCapVision() const { return m_capVision; }
    bool getCapAudio() const { return m_capAudio; }
    bool getCapToolCall() const { return m_capToolCall; }
    bool getCapReasoning() const { return m_capReasoning; }

    QString getProxyVisionModelId() const { return m_proxyVisionModelId; }
    void    setProxyVisionModelId(const QString& v);
    QString getProxyVisionPrompt() const { return m_proxyVisionPrompt; }
    void    setProxyVisionPrompt(const QString& v);

    bool getTavilyEnabled() const { return m_tavilyEnabled; }
    bool getTavilyConfigured() const { return !m_tavilyApiKey.isEmpty(); }
    void setTavilyEnabled(bool v);

    bool getShellToolEnabled() const { return m_shellToolEnabled; }
    void setShellToolEnabled(bool v);

    bool    getMathRenderEnabled() const { return m_mathRenderEnabled; }
    QString getMathServerPath() const { return m_mathServerPath; }
    void    setMathRenderEnabled(bool v);
    void    setMathServerPath(const QString& path);

    void setApiKey(const QString& key);
    void setApiEndpoint(const QString& endpoint);
    void setModel(const QString& model);
    void setTemperature(qreal temp);
    void setDefaultPrompt(const QString& prompt);
    void setIsStreaming(bool streaming);

signals:
    void messageReceived(const QString& content, bool isComplete = true);
    void streamStart();
    void streamChunk(const QString& content);
    void reasoningChunk(const QString& content);
    void markdownParsed(const QString& requestId, const QString& blocksJson);
    void streamEnd();
    void errorOccurred(const QString& error);
    void requestCancelled();
    // Tool Call 信号：toolCallsJson 为完整 tool_calls 数组的 JSON 字符串
    void toolCallReceived(const QString& toolCallsJson);
    void toolCallProgress(const QString& text, bool isComplete);
    void apiKeyChanged();
    void apiEndpointChanged();
    void modelChanged();
    void apiProtocolChanged();
    void temperatureChanged();
    void defaultPromptChanged();
    void isStreamingChanged();
    void messagesChanged();
    void apiCacheStatsChanged();
    void modelsChanged();
    void promptsChanged();
    void sessionsChanged();
    void sessionSwitched(const QString& sessionId);
    void activeModelCapabilitiesChanged();
    void proxyVisionSettingsChanged();
    void proxyVisionStarted();
    void proxyVisionCompleted(const QString& description);
    void tavilyConfigChanged();
    void tavilySearchStarted(const QString& toolCallId, const QString& query);
    void
    tavilySearchFinished(const QString& toolCallId, bool success, const QString& summary, const QString& resultText);
    void shellToolConfigChanged();
    void shellCommandPending(const QString& toolCallId, const QString& command);
    void shellCommandStarted(const QString& toolCallId, const QString& command);
    void
    shellCommandFinished(const QString& toolCallId, bool success, const QString& summary, const QString& resultText);
    void toolBatchFlushed();
    void mathRenderConfigChanged();

private:
    friend Singleton<ChatBot>;
    explicit ChatBot();

public:
    ~ChatBot();

private:
    QNetworkAccessManager* m_networkManager;
    QThreadPool            m_markdownPool;
    QMutex                 m_markdownRequestsMutex;
    QSet<QString>          m_markdownRequests;
    QList<QNetworkReply*>  m_activeReplies;
    bool                   m_cancelled = false;
    int                    m_requestSeq = 0;

    QString m_apiKey;
    QString m_apiEndpoint;
    QString m_model;
    QString m_apiProtocol = "chat_completions";
    qreal   m_temperature;
    QString m_defaultPrompt;
    bool    m_isStreaming;

    // 当前活动模型的额外请求体参数（JSON object 字符串）
    json m_extraParams;

    // 当前活动模型的能力标志
    bool m_capText        = true;
    bool m_capVision      = false;
    bool m_capAudio       = false;
    bool m_capToolCall    = false;
    bool    m_capReasoning   = false;
    QString m_reasoningEffort;
    int     m_maxContextSize = 0;

    QString m_proxyVisionModelId;
    QString m_proxyVisionPrompt = "请详细描述这张图片的内容。如果图片中有文字，请完整转录。";

    QVector<MessageData>&       currentMessages();
    const QVector<MessageData>& currentMessages() const;
    static const int            MAX_HISTORY_SIZE = 100;

    // 会话管理
    QMap<QString, SessionData> m_sessions;
    QString                    m_currentSessionId;
    QString                    m_sessionsPath;

    void    initSessions();
    void    saveSessions();
    QString sessionsFilePath();
    void    ensureCurrentSession();

    // 将 MessageData 序列化为 OpenAI API 格式的 QJsonObject
    QJsonObject messageToJson(const MessageData& msg) const;
    // 将当前历史（加 system prompt）组装为 API messages 数组
    QJsonArray buildApiMessages(const QVector<MessageData>& history,
                                const QString&              userText,
                                const QVector<MessagePart>& userParts = {});

    void makeApiRequest(const QJsonArray& messages);
    void handleNetworkReply(QNetworkReply* reply, bool isStream);
    void recordApiUsage(const QJsonObject& usage);
    bool       usesResponsesApi() const;
    QJsonArray messagesToResponsesInput(const QJsonArray& messages) const;
    void abortActiveReplies();

    QString m_currentStreamBuffer;
    QString m_currentReasoningBuffer;
    QString m_responseBuffer;
    // 流式 tool_calls 累积缓冲（按 index 存储各工具调用的片段）
    QMap<int, json>    m_toolCallsBuffer;
    QMap<QString, int> m_responseToolItemIndexes;
    bool               m_serverToolCallActive = false;
    QString            m_serverToolCallName;
    quint64            m_cacheSampleCount = 0;
    quint64            m_totalInputTokens = 0;
    quint64            m_totalCachedTokens = 0;
    quint64            m_lastInputTokens = 0;
    quint64            m_lastCachedTokens = 0;

    // 多模型管理
    json m_modelsData;

    void initModels();
    void saveModels();
    void applyModelConfig(const json& modelObj);

    // 多提示词管理
    json m_promptsData;

    void initPrompts();
    void savePrompts();

    // Tavily 网络搜索
    QString m_tavilyApiKey;
    QString m_tavilySearchDepth;
    int     m_tavilyMaxResults = 5;
    bool    m_tavilyEnabled    = false;

    void initTavily();
    void injectToolDefinitions(QJsonObject& requestBody);
    void dispatchToolCalls(const QString& toolCallsJson);
    void executeTavilySearch(const QString& toolCallId, const QString& query);

    // Shell tool
    bool        m_shellToolEnabled   = false;
    int         m_shellToolTimeoutMs = 10000;
    int         m_shellToolMaxOutput = 4096;
    QStringList m_shellToolBlocklist;

    struct PendingShellExec {
        QString toolCallId;
        QString command;
    };
    QVector<PendingShellExec> m_pendingShellExecs;

    // 异步 Shell 执行追踪（避免主线程阻塞）
    struct ActiveShellExec {
        QString   toolCallId;
        QString   command;
        QProcess* process    = nullptr;
        QTimer*   timer      = nullptr;
        QString   stdoutBuf;
        QString   stderrBuf;
    };
    QMap<QString, ActiveShellExec*> m_activeShellExecs;

    void    initShellTool();
    bool    isCommandBlocked(const QString& command);
    void    executeShellCommand(const QString& toolCallId, const QString& command);
    void    cleanupShellExec(const QString& toolCallId);
    QString truncateOutput(const QString& output, int maxBytes);

    // 数学公式渲染
    bool    m_mathRenderEnabled = false;
    QString m_mathServerPath;

    void initMathRender();

    // 批量 tool result 收集（多个 tool_calls 时，等全部完成后统一提交）
    struct ToolCallEntry {
        QString id;
        QString name;
        bool    resolved = false;
        QString result;
    };
    QVector<ToolCallEntry> m_toolCallBatch;

    void submitToolResultBatched(const QString& toolCallId, const QString& toolName, const QString& result);
    void tryFlushToolBatch();

    void callVisionProxy(const QString&              message,
                         const QVector<MessagePart>& parts,
                         const QString&              sessionId,
                         int                         requestSeq);
    void finishMediaMessage(const QString&              message,
                            const QVector<MessagePart>& parts,
                            const QString&              effectiveMessage,
                            const QString&              sessionId);
};

} // namespace mod::chatbot
