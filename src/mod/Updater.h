// SPDX-License-Identifier: GPL-3.0-only
/*
 * Copyright (C) 2022-present, PenUniverse.
 * This file is part of the PenMods open source project.
 */

#pragma once

#include "common/service/Logger.h"

#include "Version.h"

#include <QNetworkReply>
#include <QPointer>
#include <nlohmann/json.hpp>

#include "base/YEnum.h"

using json = nlohmann::json;

namespace mod {

struct Version {
    bool                  operator==(Version) const;
    bool                  operator>(Version) const;
    bool                  operator<(Version) const;
    [[nodiscard]] QString toString() const;
    [[nodiscard]] uint32  toNumber() const;
    uint32                mMajor;
    uint32                mMinor;
    uint32                mRevision;
};

class Updater : public QObject, public Singleton<Updater>, private Logger {
    Q_OBJECT
    Q_PROPERTY(QString manifestUrl READ manifestUrl NOTIFY settingsChanged)
    Q_PROPERTY(QString githubProxy READ githubProxy NOTIFY settingsChanged)
    Q_PROPERTY(QString sourceStatus READ sourceStatus NOTIFY sourceStatusChanged)
    Q_PROPERTY(bool sourceConnected READ sourceConnected NOTIFY sourceStatusChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY sourceStatusChanged)
    Q_PROPERTY(bool checking READ checking NOTIFY sourceStatusChanged)
    Q_PROPERTY(bool downloadFailed READ downloadFailed NOTIFY sourceStatusChanged)
    Q_PROPERTY(bool hasManifest READ hasManifest NOTIFY sourceStatusChanged)
    Q_PROPERTY(bool checkCancelled READ checkCancelled NOTIFY sourceStatusChanged)
    Q_PROPERTY(QString cacheTimeText READ cacheTimeText NOTIFY sourceStatusChanged)

    struct UpdateInfo;

public:
    Q_INVOKABLE void check();
    Q_INVOKABLE void ensureChecked();
    Q_INVOKABLE void cancelCheck();

    Q_INVOKABLE void download();

    Q_INVOKABLE void install();

    void onUiCompleted();

    UpdateInfo& getInfo();

    QString          manifestUrl() const;
    QString          githubProxy() const;
    QString          sourceStatus() const { return mSourceStatus; }
    bool             sourceConnected() const { return mSourceConnected; }
    bool             busy() const { return mBusy; }
    bool             checking() const { return mBusy && !mDownloading; }
    bool             downloadFailed() const { return mInfo.mOtaStatus == DOWNLOAD_FAILED; }
    bool             hasManifest() const { return !mCachedManifest.isEmpty(); }
    bool             checkCancelled() const { return mCheckCancelled; }
    QString          cacheTimeText() const { return mCacheTime; }
    Q_INVOKABLE bool saveSources(QString manifest, QString proxy);

signals:
    void settingsChanged();
    void sourceStatusChanged();

private:
    QByteArray              mCachedManifest;
    QString                 mCacheTime;
    bool                    mCacheAttempted = false;
    bool                    mCheckCancelled = false;
    quint64                 mRequestSeq     = 0;
    QPointer<QNetworkReply> mReply;
    QString                 _cachePath() const;
    bool                    _loadManifestCache();
    void                    _saveManifestCache(const QByteArray& data);
    bool                    _restoreManifest();

    QStringList mCandidates;
    QString     mSourceStatus;
    QString     mPreferredProxy;
    bool        mSourceConnected = false;
    bool        mBusy            = false;
    bool        mDownloading     = false;
    void        _requestNext();
    bool        _parseManifest(const QByteArray& data);
    QStringList _sources(const QString& original) const;
    void        _finishRequest(UpdateStatus status);

    friend Singleton<Updater>;
    explicit Updater();

    std::string mClassName = "updater";

    // latest.
    json mLatestObject;

    const Version mSelfVersion = {VERSION_MAJOR, VERSION_MINOR, VERSION_ALTER};

    struct UpdateInfo {
        QString      mCurrentStr       = "";
        QString      mLatestStr        = "";
        UpdateStatus mOtaStatus        = UpdateStatus::LATEST_VERSION;
        QString      mUpdateNote       = "";
        double       mPackageSize      = 0.0;
        uint32       mDownloadProgress = 0;
        uint32       mInstallProgress  = 0;
    } mInfo;

    std::string _calcFileMd5(const QString&);

    void _unzip(QString, QString);

    void _cleanupTemp();

    void _setCurrentVersion(QString);

    void _setLatestVersion(QString);

    void _setUpdateNote(QString);

    void _setUpdatePackSize(double);

    void _setDownloadProgress(int);

    void _setOtaStatus(UpdateStatus);

    void _setInstallProgress(int);
};

} // namespace mod
