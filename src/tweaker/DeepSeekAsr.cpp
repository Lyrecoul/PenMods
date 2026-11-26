// SPDX-License-Identifier: GPL-3.0-only
/*
 * Copyright (C) 2022-present, PenUniverse.
 * This file is part of the PenMods open source project.
 */

#include "tweaker/DeepSeekAsr.h"

#include <QHttpMultiPart>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>
#include <QVariant>

namespace mod {

namespace {

constexpr int  kMaxAudioBytes    = 8 * 1024 * 1024; ///< 接口硬上限
constexpr int  kMaxResponseBytes = 256 * 1024;
constexpr int  kMinTimeoutMs     = 3000;
constexpr int  kMaxTimeoutMs     = 120000;
constexpr auto kAsrEndpoint      = "https://api.deepseek.com/v1/tools/asr";

/// 从错误响应体里捞一句可读的说明。字段没有文档化，按常见形式依次尝试。
QString errorDetail(const QByteArray& body) {
    if (body.isEmpty()) return {};
    QString    detail;
    const auto document = QJsonDocument::fromJson(body);
    if (document.isObject()) {
        const auto root = document.object();
        if (root.value(QStringLiteral("error")).isObject())
            detail = root.value(QStringLiteral("error")).toObject().value(QStringLiteral("message")).toString();
        if (detail.isEmpty()) detail = root.value(QStringLiteral("error")).toString();
        if (detail.isEmpty()) detail = root.value(QStringLiteral("message")).toString();
        if (detail.isEmpty()) detail = root.value(QStringLiteral("msg")).toString();
        if (detail.isEmpty()) detail = root.value(QStringLiteral("detail")).toString();
    }
    if (detail.isEmpty()) detail = QString::fromUtf8(body); // 网关的纯文本/HTML 错误页
    return detail.simplified().left(200);
}

} // namespace

DeepSeekAsr::DeepSeekAsr(QObject* parent) : QObject(parent) {
    mDeadline.setSingleShot(true);
    connect(&mDeadline, &QTimer::timeout, this, [this]() { _fail(QStringLiteral("语音识别超时，请检查网络后重试")); });
}

void DeepSeekAsr::cancel() {
    mDeadline.stop();
    auto* reply = mReply.data();
    mReply.clear();
    mFinished = true;
    if (!reply) return;
    // 先 disconnect 再 abort：abort 可能同步触发 finished()，不能在回调里重入状态机
    reply->disconnect(this);
    reply->abort();
    reply->deleteLater();
}

void DeepSeekAsr::_fail(const QString& reason) {
    mDeadline.stop();
    auto* reply = mReply.data();
    mReply.clear();
    mFinished = true;
    if (reply) {
        reply->disconnect(this);
        reply->abort();
        reply->deleteLater();
    }
    emit failed(reason.left(200));
}

bool DeepSeekAsr::upload(const QByteArray& audio, const QString& suffix, const QString& apiKey, int timeoutMs) {
    cancel();
    if (audio.isEmpty()) {
        emit failed(QStringLiteral("没有录到声音"));
        return false;
    }
    if (audio.size() > kMaxAudioBytes) {
        emit failed(QStringLiteral("音频超过 8 MiB 上限"));
        return false;
    }
    const QString key = apiKey.trimmed();
    if (key.isEmpty()) {
        emit failed(QStringLiteral("未配置 DeepSeek API Key"));
        return false;
    }

    const bool    ogg  = suffix.compare(QStringLiteral("ogg"), Qt::CaseInsensitive) == 0;
    const QString name = ogg ? QStringLiteral("asr.ogg") : QStringLiteral("asr.wav");
    const QString mime = ogg ? QStringLiteral("audio/ogg") : QStringLiteral("audio/wav");

    QNetworkRequest request(QUrl(QString::fromLatin1(kAsrEndpoint)));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setRawHeader("Authorization", ("Bearer " + key).toUtf8());
    request.setRawHeader("Accept", "application/json");

    // multipart/form-data，字段名固定为 file（接口只认这个字段）
    auto*     multiPart = new QHttpMultiPart(QHttpMultiPart::FormDataType);
    QHttpPart filePart;
    filePart.setHeader(
        QNetworkRequest::ContentDispositionHeader,
        QVariant("form-data; name=\"file\"; filename=\"" + name + "\"")
    );
    filePart.setHeader(QNetworkRequest::ContentTypeHeader, QVariant(mime));
    filePart.setBody(audio);
    multiPart->append(filePart);

    // QNetworkAccessManager 必须在 QCoreApplication 之后构造，所以延迟到第一次真正发请求时创建。
    if (!mNetwork) mNetwork = new QNetworkAccessManager(this);
    auto* reply = mNetwork->post(request, multiPart);
    if (!reply) {
        delete multiPart;
        emit failed(QStringLiteral("无法创建语音识别请求"));
        return false;
    }
    multiPart->setParent(reply); // Qt 也会接管，这里显式一遍更稳

    mReply    = reply;
    mFinished = false;
    connect(reply, &QNetworkReply::finished, this, [this, reply]() { _finish(reply); });
    mDeadline.start(qBound(kMinTimeoutMs, timeoutMs, kMaxTimeoutMs));
    return true;
}

void DeepSeekAsr::_finish(QNetworkReply* reply) {
    if (mFinished || mReply != reply) {
        reply->deleteLater();
        return;
    }
    mFinished = true;
    mDeadline.stop();
    mReply.clear();

    const int        status    = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray body      = reply->readAll().left(kMaxResponseBytes);
    const auto       error     = reply->error();
    const QString    errorText = reply->errorString();
    reply->deleteLater();

    if (status < 200 || status >= 300) {
        QString reason;
        if (status == 0) {
            // 没有拿到状态码：DNS / TLS / 连接失败，或本地 abort
            reason = error == QNetworkReply::OperationCanceledError ? QStringLiteral("语音识别已取消")
                                                                    : QStringLiteral("网络错误：") + errorText;
        } else {
            reason = describeFailure(status, body);
        }
        emit failed(reason.left(200));
        return;
    }

    const auto document = QJsonDocument::fromJson(body);
    if (!document.isObject()) {
        emit failed(QStringLiteral("识别结果解析失败"));
        return;
    }
    const auto root = document.object();
    if (!root.contains(QStringLiteral("text"))) {
        emit failed(QStringLiteral("识别结果解析失败"));
        return;
    }
    const QString text = root.value(QStringLiteral("text")).toString();
    if (text.trimmed().isEmpty()) {
        emit failed(QStringLiteral("没有识别出文字"));
        return;
    }
    emit succeeded(text, root.value(QStringLiteral("duration_ms")).toInt());
}

QString DeepSeekAsr::describeFailure(int status, const QByteArray& body) {
    QString reason;
    switch (status) {
    case 401:
    case 403:
        reason = QStringLiteral("DeepSeek API Key 无效或无权限");
        break;
    case 408:
    case 504:
        reason = QStringLiteral("语音识别服务超时");
        break;
    case 413:
        reason = QStringLiteral("音频超过 8 MiB 上限");
        break;
    case 429:
        reason = QStringLiteral("语音识别请求过于频繁，请稍后再试");
        break;
    case 503:
        reason = QStringLiteral("语音识别服务暂不可用");
        break;
    default:
        reason = QStringLiteral("语音识别接口返回 HTTP %1").arg(status);
        break;
    }
    const QString detail = errorDetail(body);
    if (!detail.isEmpty()) reason += QStringLiteral("：") + detail;
    return reason.left(200);
}

} // namespace mod
