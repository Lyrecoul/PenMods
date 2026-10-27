// SPDX-License-Identifier: GPL-3.0-only
/*
 * Copyright (C) 2022-present, PenUniverse.
 * This file is part of the PenMods open source project.
 */

#pragma once

#include <QHash>
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
#include <QSet>
#include <QThreadPool>
#include <QTimer>
#include <QVector>
#include <QUuid>

#include <nlohmann/json.hpp>
using json = nlohmann::json;

#include "common/service/Logger.h"
#include "shell/ShellCommandPolicy.h"

namespace mod::chatbot {

// 消息内容部分（多模态）
struct MessagePart {
    QString type;      // "text" | "image_url" | "input_audio"
    QString text;      // type=text
    QString url;       // type=image_url：HTTP URL 或 data:image/...;base64,...
    QString data;      // type=input_audio：base64 音频数据
    QString format;    // type=input_audio：格式 (mp3/wav/ogg 等)
    QString localPath; // 持久化附件的绝对路径
    QString name;      // 附件显示名称
    QString mimeType;  // 附件 MIME 类型
    QString language;  // 文本附件的语法语言
    qint64  size = 0;  // 附件字节数
};

// 单条消息（纯文本或多模态）
struct MessageData {
    QString              messageId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    // Local display metadata. messageToJson deliberately excludes these fields.
    QVariantList         toolStates;
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
    Q_PROPERTY(QString responseMessageId READ responseMessageId NOTIFY streamStart)
    Q_PROPERTY(QVariantList toolStates READ getToolStates NOTIFY toolStatesChanged)
    Q_PROPERTY(QString currentSessionId READ getCurrentSessionId NOTIFY sessionSwitched)
    Q_PROPERTY(QVariantMap apiCacheStats READ getApiCacheStats NOTIFY apiCacheStatsChanged)

    // 当前活动模型的能力标志（只读，随模型切换更新）
    Q_PROPERTY(bool capText READ getCapText NOTIFY activeModelCapabilitiesChanged)
    Q_PROPERTY(bool capVision READ getCapVision NOTIFY activeModelCapabilitiesChanged)
    Q_PROPERTY(bool capAudio READ getCapAudio NOTIFY activeModelCapabilitiesChanged)
    Q_PROPERTY(bool capToolCall READ getCapToolCall NOTIFY activeModelCapabilitiesChanged)
    Q_PROPERTY(bool capReasoning READ getCapReasoning NOTIFY activeModelCapabilitiesChanged)
    Q_PROPERTY(bool capImageGeneration READ getCapImageGeneration NOTIFY activeModelCapabilitiesChanged)

    Q_PROPERTY(
        QString proxyVisionModelId READ getProxyVisionModelId WRITE setProxyVisionModelId NOTIFY
            proxyVisionSettingsChanged
    )
    Q_PROPERTY(
        QString proxyVisionPrompt READ getProxyVisionPrompt WRITE setProxyVisionPrompt NOTIFY proxyVisionSettingsChanged
    )

    // 网络搜索（provider: off | tavily | deepseek）
    Q_PROPERTY(QString searchProvider READ getSearchProvider WRITE setSearchProvider NOTIFY searchConfigChanged)
    Q_PROPERTY(bool searchEnabled READ getSearchEnabled WRITE setSearchEnabled NOTIFY searchConfigChanged)
    Q_PROPERTY(bool searchConfigured READ getSearchConfigured NOTIFY searchConfigChanged)
    Q_PROPERTY(int searchCount READ getSearchCount WRITE setSearchCount NOTIFY searchConfigChanged)
    // 兼容旧版外部资源库（libPenModsResources.so 可能先于主库被替换）
    Q_PROPERTY(bool tavilyEnabled READ getTavilyEnabled WRITE setTavilyEnabled NOTIFY tavilyConfigChanged)
    Q_PROPERTY(bool tavilyConfigured READ getTavilyConfigured NOTIFY tavilyConfigChanged)

    Q_PROPERTY(bool shellToolEnabled READ getShellToolEnabled WRITE setShellToolEnabled NOTIFY shellToolConfigChanged)
    // off | confirm | automatic；旧 shellToolEnabled 仍兼容（true 映射为 confirm，绝不自动开启 automatic）
    Q_PROPERTY(QString shellMode READ getShellMode WRITE setShellMode NOTIFY shellToolConfigChanged)

    Q_PROPERTY(
        bool mathRenderEnabled READ getMathRenderEnabled WRITE setMathRenderEnabled NOTIFY mathRenderConfigChanged
    )
    Q_PROPERTY(QString mathServerPath READ getMathServerPath WRITE setMathServerPath NOTIFY mathRenderConfigChanged)
    Q_PROPERTY(
        QString bubbleRenderMode READ getBubbleRenderMode WRITE setBubbleRenderMode NOTIFY bubbleRenderModeChanged
    )

public:
    // 基础接口
    Q_INVOKABLE void    sendMessage(const QString& message, const QString& fileRefs = QString());
    Q_INVOKABLE void    sendMessageWithMedia(const QString& message, const QString& mediaParts);
    // #21-3 附件边界常量：QML 预检与 C++ 保持同一来源，避免两处硬编码漂移。
    Q_INVOKABLE int     maxImageBytes() const;
    Q_INVOKABLE int     maxMediaJsonBytes() const;
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
    // 带媒体的编辑：mediaParts 是编辑后仍在的图片（[{type,url,localPath,name,mimeType}]，
    // url 可为空、只给 localPath）。独立函数而非默认参数：QML 按实参个数分派会解析失败。
    Q_INVOKABLE void editMessageWithMedia(int index, const QString& newContent, const QString& mediaParts);
    // #21-3：按完整附件列表全量替换（图片 + 文件 + 音频，可增删）；新 QML 优先用它。
    // parts JSON 字段与 sendMessageWithMedia 一致；空数组 = 清空全部附件。
    Q_INVOKABLE void editMessageWithParts(int index, const QString& newContent, const QString& parts);
    Q_INVOKABLE void deleteMessage(int index);
    Q_INVOKABLE void regenerateMessage(int index);
    Q_INVOKABLE void cancelRequest();
    // 重置 AI 服务（进程内 ChatBot，不新建单例）：取消在途网络请求/Shell/pending tool calls，
    // 清空流式临时状态，重置 requestSeq/取消标志，并重读 AI 配置；保留会话历史与已保存消息。
    // 返回 false 表示配置重读失败（其余状态仍已清理）；主线程同步但只做内存/本地文件操作，不阻塞 UI。
    Q_INVOKABLE bool resetService();
    Q_INVOKABLE void resetApiCacheStats();

    // Tool Call 接口
    Q_INVOKABLE void submitToolResult(const QString& toolCallId, const QString& toolName, const QString& result);

    // Shell Tool 接口
    Q_INVOKABLE void    approveShellCommand(const QString& toolCallId);
    Q_INVOKABLE void    denyShellCommand(const QString& toolCallId);
    Q_INVOKABLE QString getShellToolConfig();
    Q_INVOKABLE void    setShellToolConfig(const QString& configJson);
    // 命令结构审计（confirm 对话框用，不执行）。返回 JSON：
    // {ok,error,summary,commands[],warnings[],features[],mode,allowed,needsConfirm,decisionCode,reason,policyVersion}
    Q_INVOKABLE QString auditShellCommand(const QString& command);

    // 多会话管理
    Q_INVOKABLE QString      getSessions();
    Q_INVOKABLE QString      getCurrentSessionId();
    Q_INVOKABLE bool         switchSession(const QString& sessionId);
    Q_INVOKABLE QString      createSession(const QString& title = QString());
    Q_INVOKABLE bool         deleteSession(const QString& sessionId);
    Q_INVOKABLE bool         renameSession(const QString& sessionId, const QString& newTitle);
    Q_INVOKABLE QVariantList getSessionMessages(const QString& sessionId);

    // 多模型管理
    // getModels()/getActiveModel() 保留给旧版外部资源库（libPenModsResources.so 可能先于主库被替换）：
    // 返回归一化后的数据，但**不含完整 apiKey**（只给 apiKeySet 标志），旧 QML 保存时未提交 apiKey
    // 不会清空已存密钥。新 QML 用 getModelSummaries()/getModelDetail()。
    Q_INVOKABLE QString getModels();
    Q_INVOKABLE QString getModelSummaries();
    Q_INVOKABLE QString getModelDetail(const QString& modelId);
    Q_INVOKABLE bool    addModel(const QString& modelJson);
    Q_INVOKABLE bool    removeModel(const QString& modelId);
    Q_INVOKABLE bool    setActiveModel(const QString& modelId);
    Q_INVOKABLE QString getActiveModel();
    Q_INVOKABLE QString getActiveModelId();
    Q_INVOKABLE bool    loadModelsFile();
    // 最近一次 addModel/removeModel/setActiveModel 的失败原因（可直接展示，不含密钥）
    Q_INVOKABLE QString getLastModelError();
    // 自定义请求体参数（extraParams）允许的最大字节数，供 QML 预校验/提示
    Q_INVOKABLE int extraParamsMaxBytes();

    // 多提示词管理
    Q_INVOKABLE QString getPrompts();
    Q_INVOKABLE bool    addPrompt(const QString& promptJson);
    Q_INVOKABLE bool    removePrompt(const QString& promptId);
    Q_INVOKABLE bool    setActivePrompt(const QString& promptId);
    Q_INVOKABLE QString getActivePrompt();

    // 网络搜索
    Q_INVOKABLE QString getSearchConfig();
    Q_INVOKABLE void    setSearchConfig(const QString& configJson);
    // 旧接口别名，等价于 getSearchConfig/setSearchConfig 的 tavily 子集
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
    QString responseMessageId() const { return m_responseMessageId; }
    QVariantList getToolStates() const;
    QVariantMap  getApiCacheStats() const;

    bool getCapText() const { return m_capText; }
    bool getCapVision() const { return m_capVision; }
    bool getCapAudio() const { return m_capAudio; }
    bool getCapToolCall() const { return m_capToolCall; }
    bool getCapReasoning() const { return m_capReasoning; }
    bool getCapImageGeneration() const { return m_capImageGeneration; }

    QString getProxyVisionModelId() const { return m_proxyVisionModelId; }
    void    setProxyVisionModelId(const QString& v);
    QString getProxyVisionPrompt() const { return m_proxyVisionPrompt; }
    void    setProxyVisionPrompt(const QString& v);

    QString getSearchProvider() const { return m_searchProvider; }
    bool    getSearchEnabled() const { return m_searchEnabled; }
    bool    getSearchConfigured() const { return searchConfigured(); }
    int     getSearchCount() const { return m_searchCount; }
    void    setSearchProvider(const QString& provider);
    void    setSearchEnabled(bool v);
    void    setSearchCount(int count);

    bool getTavilyEnabled() const { return m_searchProvider == "tavily" && m_searchEnabled; }
    bool getTavilyConfigured() const { return !m_searchTavilyKey.isEmpty(); }
    void setTavilyEnabled(bool v);

    bool    getShellToolEnabled() const { return m_shellToolEnabled; }
    QString getShellMode() const { return QString::fromLatin1(mod::shell::toolModeName(m_shellMode)); }
    void    setShellToolEnabled(bool v);
    void    setShellMode(const QString& mode);

    bool    getMathRenderEnabled() const { return m_mathRenderEnabled; }
    QString getMathServerPath() const { return m_mathServerPath; }
    QString getBubbleRenderMode() const { return m_bubbleRenderMode; }
    void    setMathRenderEnabled(bool v);
    void    setMathServerPath(const QString& path);
    void    setBubbleRenderMode(const QString& mode);

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
    void toolStateChanged(const QVariantMap& state);
    void toolStatesChanged();
    void imageAttachmentsReceived(const QVariantList& attachments);
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
    void searchConfigChanged();
    void webSearchStarted(const QString& toolCallId, const QString& query);
    void webSearchFinished(const QString& toolCallId, bool success, const QString& summary, const QString& resultText);
    // 旧信号：新资源库已改用 webSearch*，这里仍照常发出，旧版资源库可继续工作
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
    void bubbleRenderModeChanged();

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
    bool                   m_cancelled        = false;
    bool                   m_resetting        = false; // resetService 防重入
    bool                   m_streamEndEmitted = false;
    int                    m_requestSeq       = 0;
    QString                m_responseMessageId;
    QString                m_responseSessionId;
    QVariantList           m_responseToolStates;
    bool                   m_toolFlushScheduled = false;
    void prepareTool(int index, const json& call);
    void updateTool(const QString& callId, const QString& state, const QString& text,
                    const QString& result = QString());
    void publishTool(QVariantMap state);
    void finishOutstandingTools(const QString& state, const QString& reason);
    void updateServerTool(const QJsonObject& item, const QString& state);
    void attachResponseMetadata(MessageData& message) const;
    void persistPartialResponse();

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
    bool    m_capText                 = true;
    bool    m_capVision               = false;
    bool    m_capAudio                = false;
    bool    m_capToolCall             = false;
    bool    m_capReasoning            = false;
    bool    m_capImageGeneration      = false;
    bool    m_nativeWebSearchEnabled  = false;
    QString m_nativeWebSearchProvider = "auto";
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
    QJsonArray buildApiMessages(
        const QVector<MessageData>& history,
        const QString&              userText,
        const QVector<MessagePart>& userParts = {}
    );

    void       makeApiRequest(const QJsonArray& messages);
    void       handleNetworkReply(QNetworkReply* reply, bool isStream);
    void       finishStream();
    void       emitContentChunk(const QString& content);
    void       appendAnswerChunk(const QString& text);
    void       appendReasoningChunk(const QString& text);
    void       flushEmbeddedContent();
    void       reclassifyFinalAnswer();
    QJsonArray buildApiMessagesFromHistory();
    void       recordApiUsage(const QJsonObject& usage);
    bool       usesResponsesApi() const;
    QJsonArray messagesToResponsesInput(const QJsonArray& messages) const;
    void       abortActiveReplies();
    // 重读 ai 配置（供 reloadConfig/resetService 复用；不新增 connect，不构造第二个 ChatBot）
    void applyAiConfigReload();

    QString m_currentStreamBuffer;
    QString m_currentReasoningBuffer;
    QString m_embeddedContentBuffer;
    bool    m_embeddedReasoningActive = false;
    QString m_responseBuffer;
    // 流式 tool_calls 累积缓冲（按 index 存储各工具调用的片段）
    QMap<int, json>    m_toolCallsBuffer;
    QMap<QString, int> m_responseToolItemIndexes;
    bool               m_serverToolCallActive = false;
    QString            m_serverToolCallName;
    quint64            m_cacheSampleCount  = 0;
    quint64            m_totalInputTokens  = 0;
    quint64            m_totalCachedTokens = 0;
    quint64            m_lastInputTokens   = 0;
    quint64            m_lastCachedTokens  = 0;

    // 多模型管理
    json m_modelsData;

    // 归一化后每个模型的提示（extraParams 非法/超长等），不写入配置文件，只在编辑页提示用
    QHash<QString, QString> m_modelWarnings;
    QString                 m_lastModelError;

    static constexpr int kModelExtraParamsMaxBytes = 4096;      // extraParams 序列化后上限
    static constexpr int kModelJsonMaxBytes        = 32 * 1024; // addModel 入参 JSON 上限

    void initModels();
    void saveModels();
    void normalizeModelsData();
    json normalizeModel(const json& raw, const std::string& fallbackId, std::string& warning) const;
    // extraParams 统一入口：只接受 JSON object；兼容旧版本存成字符串（含二次 stringify 的嵌套字符串）
    static bool parseExtraParams(const json& raw, json& out, std::string& error);
    void        applyModelConfig(const json& modelObj);

    // 多提示词管理
    json m_promptsData;

    void initPrompts();
    void savePrompts();

    // 网络搜索
    QString m_searchProvider    = "off"; // off | tavily | deepseek
    bool    m_searchEnabled     = false;
    int     m_searchCount       = 5;
    bool    m_searchNeedContent = false;
    int     m_searchMaxChars    = 6000;
    QString m_searchTavilyKey;
    QString m_searchTavilyDepth = "advanced";
    QString m_searchDeepseekKey;
    QString m_searchDeepseekBaseUrl = "https://api.deepseek.com";

    void    initSearch();
    void    saveSearchConfig();
    bool    searchConfigured() const;
    bool    searchToolAvailable() const;
    QString effectiveSearchKey() const;
    QString formatSearchResults(const QString& query, const QJsonArray& results, bool includeContent) const;
    void    injectToolDefinitions(QJsonObject& requestBody);
    QVector<MessagePart> persistGeneratedImages(const QJsonArray& output);
    // 视觉代理路径专用：原图落盘成会话附件（仅本地查看，不发给主模型）
    QVector<MessagePart> persistUserImages(const QVector<MessagePart>& parts, const QString& sessionId);
    QVariantList         attachmentVariants(const QVector<MessagePart>& parts) const;

    // #21-3 多附件：解析 mediaParts JSON（image_url/file/input_audio），校验类型/大小并按内容去重。
    // 返回空 + error 非空 = 拒绝（调用方 emit errorOccurred 后 return）。
    // hasReplacementAttachments = JSON 里带 file/input_audio 条目（编辑时走“附件全量替换”）。
    QVector<MessagePart>
    parseAttachmentParts(const QString& mediaParts, QString* error, bool* hasReplacementAttachments) const;
    // 收集一个会话所有消息 parts 里引用的附件绝对路径（用于删除消息后的孤儿清理）。
    QSet<QString> referencedAttachmentPaths(const SessionData& session) const;
    // editMessageWithMedia / editMessageWithParts 共用：落盘图片、重建 parts、写历史并重新请求。
    void
    applyEditedMessage(int index, const QString& newContent, const QVector<MessagePart>& editedParts, bool fullReplace);
    // 删除该会话 attachments 目录里不再被引用的附件文件。
    void pruneSessionAttachments(const QString& sessionId);
    // 后台预热历史图片 data URL 缓存（避免构造请求时在 UI 线程读盘 + base64）。
    void                 warmAttachmentCache(const SessionData& session);
    void                 dispatchToolCalls(const QString& toolCallsJson);
    void                 executeWebSearch(const QString& toolCallId, const QString& query);
    void                 executeTavilySearch(const QString& toolCallId, const QString& query);
    void                 executeDeepSeekSearch(const QString& toolCallId, const QString& query);
    void                 requestDeepSeekSearch(const QString& toolCallId, const QString& query, int attempt);

    // Shell tool
    mod::shell::ToolMode m_shellMode              = mod::shell::ToolMode::Off;
    bool                 m_shellToolEnabled       = false; // 派生：mode != off（兼容旧 QML 属性）
    int                  m_shellToolTimeoutMs     = 10000;
    int                  m_shellToolMaxOutput     = 4096;
    int                  m_shellToolMaxConcurrent = 2;
    int                  m_shellToolQueueLimit    = 8;
    QStringList          m_shellToolBlocklist;

    // confirm 模式：等待用户确认（不占用并发槽位）
    struct PendingShellExec {
        QString toolCallId;
        QString command;
    };
    QVector<PendingShellExec> m_pendingShellExecs;

    // 已批准（confirm）或策略放行（automatic）：等待并发槽位/正在执行的请求
    struct QueuedShellExec {
        QString toolCallId;
        QString command;
        QString sessionId;
        QString batchId;
        int     requestSeq = 0;
        bool    automatic  = false;
    };
    QVector<QueuedShellExec> m_queuedShellExecs;

    // 异步 Shell 执行追踪（避免主线程阻塞）
    struct ActiveShellExec {
        QString   toolCallId;
        QString   command;
        QString   sessionId;
        QString   batchId;
        int       requestSeq = 0;
        bool      automatic  = false;
        int       timeoutMs  = 0;
        QProcess* process    = nullptr;
        QTimer*   timer      = nullptr;
        // 边读边截断，避免子进程输出无上限占用内存
        QString stdoutBuf;
        QString stderrBuf;
        int     stdoutBytes     = 0;
        int     stderrBytes     = 0;
        bool    stdoutTruncated = false;
        bool    stderrTruncated = false;
    };
    QMap<QString, ActiveShellExec*> m_activeShellExecs;

    void                     initShellTool();
    void                     persistShellToolConfig();
    mod::shell::PolicyConfig shellPolicyConfig() const;
    void                     enqueueShellExec(const QueuedShellExec& request);
    void                     startShellProcess(const QueuedShellExec& request);
    void                     drainShellQueue();
    bool                     isShellRequestCurrent(const QueuedShellExec& request) const;
    void                     finalizeShellExec(
        const QString& toolCallId,
        const QString& resultText,
        bool           success,
        const QString& summary,
        int            exitCode
    );
    QString shellResultText(const ActiveShellExec& exec) const;
    void    cleanupShellExec(const QString& toolCallId);
    void    abortPendingShellExecs(const QString& reason);
    // 模式切到 off 时，取消所有待确认/排队/执行中的命令，并给模型明确结果
    void abortAllShellExecs(const QString& reason);
    // 模式切走 automatic 时，取消排队/正在执行的 automatic 命令并给模型明确结果
    void abortAutomaticShellExecs(const QString& reason);

    // 数学公式渲染
    bool    m_mathRenderEnabled = false;
    QString m_mathServerPath;
    QString m_bubbleRenderMode = "full";

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
    void resolvePendingToolCalls(const QString& reason);

    void callVisionProxy(
        const QString&              message,
        const QVector<MessagePart>& parts,
        const QString&              sessionId,
        int                         requestSeq
    );
    void finishMediaMessage(
        const QString&              message,
        const QVector<MessagePart>& parts,
        const QString&              effectiveMessage,
        const QString&              sessionId,
        // 仅本地查看的附件（视觉代理路径下的用户原图）：落盘后进历史，但不进这一轮的请求
        const QVector<MessagePart>& localOnlyParts = {}
    );
};

} // namespace mod::chatbot
