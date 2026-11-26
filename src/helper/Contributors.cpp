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

#include <algorithm>
#include <utility>

namespace mod {

namespace {

constexpr auto kApiUrl = "https://api.github.com/repos/Lyrecoul/PenMods/contributors?per_page=%1&page=%2";
constexpr auto kUserAgent = "PenMods-Contributors (+https://github.com/Lyrecoul/PenMods)";
constexpr auto kAccept = "application/vnd.github+json";
constexpr int kPerPage = 100;
constexpr int kMaxPages = 3;
constexpr int kTimeoutMs = 15000;
constexpr qint64 kMaxBodySize = 2 * 1024 * 1024;

} // namespace

Contributors::Contributors() : Logger("Contributors") {
    connect(&Event::getInstance(), &Event::beforeUiInitialization, this, [this](QQuickView&, QQmlContext* context) {
        context->setContextProperty("contributors", this);
    });
}

void Contributors::fetch() {
    if (mBusy) return;

    mBusy = true;
    mFailed = false;
    mContributors.clear();
    emit changed();

    _requestPage(1);
}

void Contributors::_requestPage(int page) {
    // Lazily create the manager after the application's CA setup has run.
    if (!mNetwork) mNetwork = new QNetworkAccessManager(this);

    QNetworkRequest request{QUrl(QString::fromLatin1(kApiUrl).arg(kPerPage).arg(page))};
    request.setRawHeader("User-Agent", kUserAgent);
    request.setRawHeader("Accept", kAccept);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);

    auto* reply = mNetwork->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, page] { _handlePage(reply, page); });
    QTimer::singleShot(kTimeoutMs, reply, &QNetworkReply::abort);
}

void Contributors::_handlePage(QNetworkReply* reply, int page) {
    reply->deleteLater();

    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status && status != 200) {
        _fail(QStringLiteral("HTTP %1").arg(status));
        return;
    }
    if (reply->error() != QNetworkReply::NoError) {
        const bool timeout = reply->error() == QNetworkReply::OperationCanceledError;
        _fail(timeout ? QStringLiteral("Request timed out") : reply->errorString());
        return;
    }
    if (status != 200) {
        _fail(QStringLiteral("Missing HTTP status"));
        return;
    }
    if (reply->bytesAvailable() > kMaxBodySize) {
        _fail(QStringLiteral("Response too large"));
        return;
    }

    const auto document = QJsonDocument::fromJson(reply->readAll());
    if (!document.isArray()) {
        _fail(QStringLiteral("Invalid contributors JSON"));
        return;
    }

    const auto array = document.array();
    for (const QJsonValue& value : array) {
        if (!value.isObject()) continue;

        const auto object = value.toObject();
        auto login = object.value(QLatin1String("login")).toString();
        if (login.isEmpty()) continue;

        const int contributions = object.value(QLatin1String("contributions")).toInt();
        mPending.push_back({std::move(login), contributions});
    }

    if (array.size() >= kPerPage && page < kMaxPages) {
        _requestPage(page + 1);
        return;
    }
    if (mPending.empty()) {
        _fail(QStringLiteral("Empty contributors list"));
        return;
    }

    _finish();
}

void Contributors::_finish() {
    std::sort(mPending.begin(), mPending.end(), [](const Entry& left, const Entry& right) {
        if (left.contributions != right.contributions) return left.contributions > right.contributions;
        return QString::compare(left.login, right.login, Qt::CaseInsensitive) < 0;
    });

    mContributors.reserve(static_cast<int>(mPending.size()));
    for (auto& entry : mPending) mContributors.append(std::move(entry.login));
    mPending.clear();
    mBusy = false;
    debug("Loaded {} GitHub contributors", mContributors.size());
    emit changed();
}

void Contributors::_fail(const QString& reason) {
    mPending.clear();
    mBusy = false;
    mFailed = true;
    warn("Failed to load GitHub contributors: {}", reason.toStdString());
    emit changed();
}

} // namespace mod
