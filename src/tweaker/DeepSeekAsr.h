// SPDX-License-Identifier: GPL-3.0-only
/*
 * Copyright (C) 2022-present, PenUniverse.
 * This file is part of the PenMods open source project.
 */

#pragma once

#include <QByteArray>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>

class QNetworkReply;

namespace mod {

/// DeepSeek 语音识别（`POST https://api.deepseek.com/v1/tools/asr`）的一次性异步客户端。
///
/// 接口事实（官方文档）：multipart/form-data 字段名固定为 `file`；只接受 16-bit PCM WAV 或
/// Ogg Opus；单次 ≤120 秒、≤8 MiB、同步返回、无流式、无时间戳，成功时响应体是
/// `{request_id, text, duration_ms}`。错误体字段没有文档化，所以这里只按 HTTP 状态码给出
/// 人话，然后尽力从 `error.message` / `message` / `msg` / `error` 里捞一句原文。
///
/// 约定（与 AiTranslation 一致）：
///   • 完全异步 —— 绝不在 UI 线程同步等 QNetworkReply；
///   • 同一实例同时只允许一个在飞请求，`cancel()` 先摘指针再 abort，避免 abort 同步回调重入；
///   • 日志只写状态码与字节数，绝不写音频内容或 API Key。
class DeepSeekAsr : public QObject {
    Q_OBJECT

public:
    explicit DeepSeekAsr(QObject* parent = nullptr);

    /// 上传一段音频。`suffix` 取 "wav" / "ogg"，只影响 multipart 的文件名和 Content-Type。
    /// 返回 false 表示参数不合格（原因已经通过 failed() 发出），调用方不需要再处理。
    bool upload(const QByteArray& audio, const QString& suffix, const QString& apiKey, int timeoutMs);

    /// 取消在飞请求（没有在飞请求时是空操作）。
    void cancel();

    [[nodiscard]] bool busy() const { return !mReply.isNull(); }

    /// 把"HTTP 状态码 + 响应体"翻译成一句给气泡用的话（≤200 字）。
    static QString describeFailure(int status, const QByteArray& body);

signals:
    void succeeded(const QString& text, int durationMs);
    void failed(const QString& reason);

private:
    void _finish(QNetworkReply* reply);
    void _fail(const QString& reason);
    void _drop(const QString& reason);

    QNetworkAccessManager*  mNetwork{nullptr};
    QPointer<QNetworkReply> mReply;
    QTimer                  mDeadline;
    bool                    mFinished{false};
};

} // namespace mod
