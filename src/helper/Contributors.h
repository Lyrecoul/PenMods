// SPDX-License-Identifier: GPL-3.0-only
/*
 * Copyright (C) 2022-present, PenUniverse.
 * This file is part of the PenMods open source project.
 */

#pragma once

#include "common/service/Logger.h"
#include "common/service/Singleton.h"

#include <QObject>
#include <QStringList>

#include <vector>

class QNetworkAccessManager;
class QNetworkReply;

namespace mod {

class Contributors : public QObject, public Singleton<Contributors>, private Logger {
    Q_OBJECT

    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(bool failed READ failed NOTIFY changed)
    Q_PROPERTY(QStringList contributors READ contributors NOTIFY changed)

public:
    [[nodiscard]] bool busy() const { return mBusy; }
    [[nodiscard]] bool failed() const { return mFailed; }
    [[nodiscard]] QStringList contributors() const { return mContributors; }

    Q_INVOKABLE void fetch();

signals:
    void changed();

private:
    friend Singleton<Contributors>;
    explicit Contributors();

    void _requestPage(int page);
    void _handlePage(QNetworkReply* reply, int page);
    void _finish();
    void _fail(const QString& reason);

    struct Entry {
        QString login;
        int contributions;
    };

    QNetworkAccessManager* mNetwork = nullptr;
    std::vector<Entry>     mPending;
    QStringList            mContributors;
    bool                   mBusy   = false;
    bool                   mFailed = false;
};

} // namespace mod
