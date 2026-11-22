// SPDX-License-Identifier: GPL-3.0-only
/*
 * Copyright (C) 2022-present, PenUniverse.
 * This file is part of the PenMods open source project.
 */

#include "helper/Contributors.h"

#include "common/Event.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QQmlContext>
#include <QTimer>
#include <QUrl>
#include <QVariantMap>

#include <algorithm>

namespace mod {

namespace {

// %1 是页码。GitHub 文档要求所有请求都带 User-Agent（否则 403）与固定的 Accept。
constexpr auto kApiUrl      = "https://api.github.com/repos/Lyrecoul/PenMods/contributors?per_page=100&page=%1";
constexpr auto kUserAgent   = "PenMods-Contributors (+https://github.com/Lyrecoul/PenMods)";
constexpr auto kAccept      = "application/vnd.github+json";
constexpr auto kFailedText  = "网络异常，数据获取失败";
constexpr int  kMaxBodySize = 2 * 1024 * 1024;

} // namespace

Contributors::Contributors() : Logger("Contributors") {

    connect(&Event::getInstance(), &Event::beforeUiInitialization, [this](QQuickView& view, QQmlContext* context) {
        context->setContextProperty("contributors", this);
    });
}

void Contributors::fetch() {

    // QML 侧按钮已经有 enabled 判断，这里兜住脚本重复调用，避免同一份数据重复拉取/串号。
    if (mBusy) {
        debug("已有请求在途，忽略重复的获取请求");
        return;
    }

    mBusy = true;
    mError.clear();
    mContributors.clear();
    emit changed();

    _requestPage(1);
}

void Contributors::_requestPage(int page) {

    if (!mNetwork) mNetwork = new QNetworkAccessManager(this);

    QNetworkRequest request{QUrl(QString::fromLatin1(kApiUrl).arg(page))};
    request.setRawHeader("User-Agent", kUserAgent);
    request.setRawHeader("Accept", kAccept);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);

    auto* reply = mNetwork->get(request);
    mReply      = reply;

    // 单次请求的绝对期限：超时 abort 后 finished 照常触发，统一走失败路径。
    // 定时器以 reply 为上下文，reply 被回收时定时器一起销毁，不会打到已释放的对象上。
    QTimer::singleShot(kTimeoutMs, reply, &QNetworkReply::abort);
    connect(reply, &QNetworkReply::finished, this, [this, reply, page] { _handlePage(reply, page); });
}

void Contributors::_handlePage(QNetworkReply* reply, int page) {

    if (mReply != reply) {
        // 已被替换的旧请求，只负责回收。
        reply->deleteLater();
        return;
    }
    mReply.clear();
    reply->deleteLater();

    if (reply->error() != QNetworkReply::NoError) {
        const bool timeout = reply->error() == QNetworkReply::OperationCanceledError;
        _fail(timeout ? QStringLiteral("请求超时") : reply->errorString());
        return;
    }

    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status != 200) {
        _fail(QStringLiteral("HTTP %1").arg(status));
        return;
    }

    const auto body = reply->readAll();
    if (body.size() > kMaxBodySize) {
        _fail(QStringLiteral("响应内容过大"));
        return;
    }

    QJsonParseError error{};
    const auto      document = QJsonDocument::fromJson(body, &error);
    if (error.error != QJsonParseError::NoError || !document.isArray()) {
        _fail(QStringLiteral("JSON 解析失败"));
        return;
    }

    const auto array = document.array();
    for (const QJsonValue& value : array) {
        if (!value.isObject()) continue;

        const auto object = value.toObject();
        const auto login  = object.value("login").toString();
        if (login.isEmpty()) continue;

        QVariantMap contributor;
        contributor.insert(QStringLiteral("login"), login);
        contributor.insert(QStringLiteral("avatarUrl"), object.value("avatar_url").toString());
        contributor.insert(QStringLiteral("contributions"), object.value("contributions").toInt());
        mContributors.append(contributor);
    }

    // 满一页才继续翻页，超过 kMaxPages 就收手，避免异常分页把请求拖成死循环。
    if (array.size() >= kPerPage && page < kMaxPages) {
        _requestPage(page + 1);
        return;
    }

    // 接口/镜像用 200 + []（或每一项都缺 login）表示异常时，不能当成"成功但没人"。
    if (mContributors.isEmpty()) {
        _fail(QStringLiteral("贡献者列表为空"));
        return;
    }

    _finish();
}

void Contributors::_finish() {

    std::sort(mContributors.begin(), mContributors.end(), [](const QVariant& lhs, const QVariant& rhs) {
        const auto left  = lhs.toMap();
        const auto right = rhs.toMap();
        const int  lc    = left.value(QStringLiteral("contributions")).toInt();
        const int  rc    = right.value(QStringLiteral("contributions")).toInt();
        if (lc != rc) return lc > rc;
        return left.value(QStringLiteral("login")).toString().toLower()
             < right.value(QStringLiteral("login")).toString().toLower();
    });

    mBusy  = false;
    mError = QString();
    debug("获取到 {} 位贡献者", static_cast<int>(mContributors.size()));
    emit changed();
}

void Contributors::_fail(const QString& reason) {

    mBusy  = false;
    mError = reason;
    mContributors.clear();
    warn("获取贡献者失败：{}", reason.toStdString());

    emit changed();
    emit failed(QString::fromUtf8(kFailedText));
}

} // namespace mod
