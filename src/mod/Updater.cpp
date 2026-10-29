// SPDX-License-Identifier: GPL-3.0-only
/*
 * Copyright (C) 2022-present, PenUniverse.
 * This file is part of the PenMods open source project.
 */

#include "Updater.h"

#include "base/YPointer.h"

#include "common/Event.h"
#include "common/Utils.h"
#include "common/util/System.h"
#include "mod/Config.h"
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QTimer>
#include <QUrl>

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QQmlContext>
#include <QSaveFile>
#include <QStorageInfo>

#define UH_TEMP_PATH    "/tmp/modupdate/"
#define UH_TEMP_DIRNAME "modupdate"

#if PL_BUILD_YDP02X
#define UH_MAIN_URL "https://cdn.jsdelivr.net/gh/lyrecoul/penmods-ota@main/updates.json"
#endif

namespace mod {
namespace {
bool validSource(const QString& text);
}

Updater::Updater() : Logger("Updater") {
    connect(&Event::getInstance(), &Event::uiCompleted, this, &Updater::onUiCompleted);
    connect(&Event::getInstance(), &Event::beforeUiInitialization, [this](QQuickView& view, QQmlContext* context) {
        context->setContextProperty("updater", this);
    });
}

QString Version::toString() const {
    spdlog::warn("1.1");
    return QString("%1.%2.%3").arg(QString::number(mMajor), QString::number(mMinor), QString::number(mRevision));
}

uint32 Version::toNumber() const { return mMajor * 100 + mMinor * 10 + mRevision; }

bool Version::operator==(const Version b) const {
    return mMajor == b.mMajor && mMinor == b.mMinor && mRevision == b.mRevision;
}

bool Version::operator>(const Version b) const { return toNumber() > b.toNumber(); }

bool Version::operator<(const Version b) const { return toNumber() < b.toNumber(); }

QString Updater::_cachePath() const { return util::getModuleFileInfo().absoluteDir().filePath("ota-manifest.json"); }

bool Updater::_restoreManifest() { return !mCachedManifest.isEmpty() && _parseManifest(mCachedManifest); }

bool Updater::_loadManifestCache() {
    QFile file(_cachePath());
    if (!file.open(QIODevice::ReadOnly) || file.size() > 2 * 1024 * 1024) return false;
    try {
        const auto cache = json::parse(file.readAll().toStdString());
        if (cache.at("source").get<std::string>() != manifestUrl().toStdString()) return false;
        const auto data = QByteArray::fromStdString(cache.at("manifest").dump());
        const auto time = QDateTime::fromSecsSinceEpoch(cache.at("saved_at").get<qint64>()).toLocalTime();
        if (data.size() > 1024 * 1024 || !_parseManifest(data)) return false;
        mCachedManifest  = data;
        mCacheTime       = time.toString("yyyy-MM-dd HH:mm");
        mSourceConnected = false;
        mSourceStatus    = "使用已缓存的更新清单";
        emit sourceStatusChanged();
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

void Updater::_saveManifestCache(const QByteArray& data) {
    mCachedManifest  = data;
    const auto now   = QDateTime::currentDateTime();
    mCacheTime       = now.toString("yyyy-MM-dd HH:mm");
    const json cache = {
        {"source",   manifestUrl().toStdString()    },
        {"saved_at", now.toSecsSinceEpoch()         },
        {"manifest", json::parse(data.toStdString())}
    };
    const auto bytes = QByteArray::fromStdString(cache.dump());
    QSaveFile  file(_cachePath());
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
        warn("Cannot persist OTA manifest cache; keeping the in-memory copy");
}

void Updater::ensureChecked() {
    if (mBusy || hasManifest() || mCheckCancelled) return;
    if (!mCacheAttempted) {
        mCacheAttempted = true;
        if (_loadManifestCache()) return;
    }
    check();
}

void Updater::cancelCheck() {
    if (!mBusy || mDownloading) return;
    ++mRequestSeq;
    mCandidates.clear();
    if (mReply) {
        auto* reply = mReply.data();
        mReply.clear();
        disconnect(reply, nullptr, this, nullptr);
        reply->abort();
        reply->manager()->deleteLater();
    }
    const bool restored = _restoreManifest();
    mCheckCancelled     = !restored;
    mSourceConnected    = false;
    mSourceStatus       = restored ? "检测已取消，保留缓存清单" : "检测已取消";
    _finishRequest(restored ? mInfo.mOtaStatus : LATEST_VERSION);
}

void Updater::check() {
    if (mBusy) return;
    auto power = PEN_CALL(uint32, "_ZN15YBatteryManager5powerEv", void*)(YPointer<YBatteryManager>::getInstance());
    if (power < 10) {
        _setOtaStatus(ERROR_LOW_BATTERY);
        return;
    }
    _cleanupTemp();
    ++mRequestSeq;
    mCheckCancelled = false;
    mDownloading    = false;
    mBusy           = true;
    mPreferredProxy.clear();
    mCandidates = _sources(QStringLiteral(UH_MAIN_URL));
    if (!manifestUrl().isEmpty()) mCandidates.prepend(manifestUrl());
    mCandidates.removeDuplicates();
    _setOtaStatus(CHECKING);
    _requestNext();
}

bool Updater::_parseManifest(const QByteArray& data) {
    try {
        auto j = json::parse(data.toStdString());
        if (j["status"] != 200) {
            throw std::runtime_error("status != 200");
        }
        if (!j["versions"].is_array() || j["versions"].empty()) throw std::runtime_error("Empty version list");
        for (const auto& entry : j["versions"]) {
            for (const auto* key : {"version", "next"}) {
                if (!entry.contains(key) && QString(key) == "next") continue;
                const auto& v = entry.at(key);
                if (!v.is_array() || v.size() != 3) throw std::runtime_error("Invalid version");
                for (const auto& n : v)
                    if (!n.is_number_unsigned() || n.get<quint64>() > 999)
                        throw std::runtime_error("Invalid version component");
            }
            if (!entry.at("size").is_number_unsigned() || entry.at("size").get<quint64>() == 0
                || entry.at("size").get<quint64>() > 256 * 1024 * 1024 || !entry.at("note").is_string()
                || !entry.at("download").is_string()
                || !validSource(QString::fromStdString(entry.at("download").get<std::string>()))
                || !entry.at("md5").is_string()
                || !QRegularExpression("^[0-9a-fA-F]{32}$")
                        .match(QString::fromStdString(entry.at("md5").get<std::string>()))
                        .hasMatch())
                throw std::runtime_error("Invalid update metadata");
        }
        mLatestObject = json();
        // matchSelfVersion -> needGoNextVersion? -> latestDirectly / nextVersion
        bool selfVersionDetected = false;
        auto topVer              = mSelfVersion;
        for (auto i : j["versions"]) {
            auto that = Version{i["version"][0], i["version"][1], i["version"][2]};
            if (that > topVer) {
                topVer = that;
            } else if (that == mSelfVersion) {
                selfVersionDetected = true;
                if (i.find("next") != i.end()) {
                    topVer = Version{i["next"][0], i["next"][1], i["next"][2]};
                    break;
                }
            }
        }
        if (!selfVersionDetected) {
            throw std::runtime_error("Cannot get self version from mod_versions.");
        }
        for (auto i : j["versions"]) {
            auto that = Version{i["version"][0], i["version"][1], i["version"][2]};
            if (that == topVer) {
                mLatestObject = i;
                break;
            }
        }
        if (mLatestObject.is_null()) throw std::runtime_error("Missing target version");
        _setLatestVersion(topVer.toString());
        if (topVer > mSelfVersion) {
            auto   avail = QStorageInfo(UH_TEMP_PATH).bytesAvailable();
            qint64 bytes = mLatestObject["size"];
            if (avail == -1 || avail < bytes) {
                error("Checking for updates stalled: No enough space(2).");
                _setOtaStatus(ERROR_NO_ENOUGH_MEMORY);
                return true;
            }
            _setUpdateNote(QString::fromStdString(mLatestObject["note"]));
            _setUpdatePackSize((double)bytes / 1024.0 / 1024.0);
            info("New version({}) founded!", topVer.toString().toStdString());
            _setOtaStatus(HAS_NEW_VERSION);
        } else {
            info("Already the latest version.");
            _setOtaStatus(LATEST_VERSION);
        }
    } catch (const std::exception& e) {
        error("An error occurred while parsing the version information.");
        error(e.what());
        return false;
    }
    return true;
}

Updater::UpdateInfo& Updater::getInfo() { return mInfo; }

void Updater::download() {
    if (mBusy || (mInfo.mOtaStatus != HAS_NEW_VERSION && mInfo.mOtaStatus != DOWNLOAD_FAILED)) return;
    mDownloading = true;
    ++mRequestSeq;
    mBusy = true;
    _setDownloadProgress(0);
    mCandidates = _sources(QString::fromStdString(mLatestObject["download"]));
    _setOtaStatus(DOWNLOADING);
    _requestNext();
}

namespace {
const QStringList& otaProxies() {
    static const QStringList proxies = {"https://gh-proxy.com/", "https://ghproxy.net/", "https://gh-proxy.org/"};
    return proxies;
}
bool validSource(const QString& text) {
    const QUrl url(text, QUrl::StrictMode);
    return text.size() <= 2048 && url.isValid() && url.scheme() == "https" && !url.host().isEmpty()
        && url.userInfo().isEmpty() && !url.hasFragment();
}
} // namespace

QString Updater::manifestUrl() const {
    return QString::fromStdString(Config::getInstance().read("updater").value("manifest_url", ""));
}

QString Updater::githubProxy() const {
    return QString::fromStdString(Config::getInstance().read("updater").value("github_proxy", ""));
}

bool Updater::saveSources(QString manifest, QString proxy) {
    if (mBusy) return false;
    manifest = manifest.trimmed();
    proxy    = proxy.trimmed();
    if ((!manifest.isEmpty() && !validSource(manifest))
        || (!proxy.isEmpty() && (!validSource(proxy) || QUrl(proxy).hasQuery())))
        return false;
    if (!proxy.isEmpty() && !proxy.endsWith('/')) proxy += '/';
    const bool sourceChanged = manifest != manifestUrl();
    Config::getInstance().write(
        "updater",
        {
            {"manifest_url", manifest.toStdString()},
            {"github_proxy", proxy.toStdString()   }
    }
    );
    mPreferredProxy.clear();
    if (sourceChanged) {
        mCachedManifest.clear();
        mCacheTime.clear();
        mCacheAttempted = false;
        mLatestObject   = json();
    }
    mCheckCancelled  = false;
    mSourceConnected = false;
    mSourceStatus.clear();
    emit settingsChanged();
    emit sourceStatusChanged();
    return true;
}

QStringList Updater::_sources(const QString& original) const {
    QString       github = original;
    const QString cdn    = "https://cdn.jsdelivr.net/gh/";
    if (original.startsWith(cdn)) {
        const auto path  = original.mid(cdn.size());
        const int  at    = path.indexOf('@');
        const int  slash = path.indexOf('/', at);
        if (at > 0 && slash > at)
            github = "https://raw.githubusercontent.com/" + path.left(at) + "/" + path.mid(at + 1, slash - at - 1)
                   + path.mid(slash);
    }
    QStringList result;
    const QUrl  url(github);
    if (url.host() == "raw.githubusercontent.com" || url.host() == "github.com") {
        QStringList proxies = otaProxies();
        if (!githubProxy().isEmpty()) proxies.prepend(githubProxy());
        if (!mPreferredProxy.isEmpty()) proxies.prepend(mPreferredProxy);
        for (const auto& proxy : proxies) result.append(proxy + github);
        result.append(github);
    }
    result.append(original);
    result.removeDuplicates();
    return result;
}

void Updater::_finishRequest(UpdateStatus status) {
    mBusy = false;
    _setOtaStatus(status);
    emit sourceStatusChanged();
}

void Updater::_requestNext() {
    if (!mBusy) return;
    const auto sequence = mRequestSeq;
    mSourceConnected    = false;
    if (mCandidates.isEmpty()) {
        mSourceStatus       = "所有更新源均失败，请检查网络或设置自定义源";
        const bool restored = !mDownloading && _restoreManifest();
        _finishRequest(restored ? mInfo.mOtaStatus : (mDownloading ? DOWNLOAD_FAILED : ERROR_NO_CONNECTION));
        return;
    }
    const QString address = mCandidates.takeFirst();
    mSourceStatus         = QString("正在%1：%2（剩余 %3）")
                                .arg(mDownloading ? "下载" : "检查", QUrl(address).host())
                                .arg(mCandidates.size());
    emit sourceStatusChanged();
    info("OTA trying host {}", QUrl(address).host().toStdString());
    // Construct the network backend only after QCoreApplication exists.
    auto*           manager = new QNetworkAccessManager(this);
    QNetworkRequest request{QUrl(address)};
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    auto* reply = manager->get(request);
    mReply      = reply;
    reply->setReadBufferSize(64 * 1024);
    auto* idle = new QTimer(reply);
    idle->setSingleShot(true);
    idle->setInterval(15000);
    connect(idle, &QTimer::timeout, reply, &QNetworkReply::abort);
    idle->start();
    QTimer::singleShot(mDownloading ? 600000 : 20000, reply, &QNetworkReply::abort);
    auto file = std::make_shared<QFile>(mDownloading ? UH_TEMP_PATH "download.temp" : UH_TEMP_PATH "version_list.temp");
    const qint64 limit      = mDownloading ? mLatestObject["size"].get<qint64>() : 1024 * 1024;
    auto         localError = std::make_shared<bool>(!file->open(QIODevice::WriteOnly | QIODevice::Truncate));
    auto         consume    = [reply, file, limit, localError, idle]() {
        const auto bytes = reply->readAll();
        if (!bytes.isEmpty()) idle->start();
        if (file->size() + bytes.size() > limit) {
            reply->abort();
            return;
        }
        if (*localError || file->write(bytes) != bytes.size()) {
            *localError = true;
            reply->abort();
        }
    };
    connect(reply, &QNetworkReply::readyRead, this, consume);
    connect(reply, &QNetworkReply::downloadProgress, this, [this](qint64 received, qint64 total) {
        if (mDownloading && total > 0) _setDownloadProgress(int(received * 100 / total));
    });
    connect(
        reply,
        &QNetworkReply::finished,
        this,
        [this, reply, manager, file, localError, consume, address, limit, sequence]() {
            if (sequence != mRequestSeq || !mBusy) return;
            mReply.clear();
            consume();
            const bool flushed = file->flush();
            if (!flushed) *localError = true;
            const qint64 size = file->size();
            file->close();
            bool ok = !*localError && flushed && reply->error() == QNetworkReply::NoError
                   && reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200;
            reply->deleteLater();
            manager->deleteLater();
            if (ok && mDownloading) {
                QFile              input(file->fileName());
                QCryptographicHash hash(QCryptographicHash::Md5);
                ok = size == limit && input.open(QIODevice::ReadOnly) && hash.addData(&input)
                  && hash.result().toHex() == QByteArray::fromStdString(mLatestObject["md5"]).toLower();
            } else if (ok) {
                QFile      input(file->fileName());
                const auto data = input.open(QIODevice::ReadOnly) ? input.readAll() : QByteArray();
                ok              = _parseManifest(data);
                if (ok) _saveManifestCache(data);
            }
            if (ok) {
                for (const auto& proxy : otaProxies())
                    if (address.startsWith(proxy)) mPreferredProxy = proxy;
                mSourceConnected = true;
                mSourceStatus    = QUrl(address).host();
                _finishRequest(mDownloading ? DOWNLOAD_FINISHED : mInfo.mOtaStatus);
            } else if (*localError && size <= limit) {
                mSourceStatus = "无法写入更新文件，请检查可用空间";
                _finishRequest(ERROR_NO_ENOUGH_MEMORY);
            } else {
                _setDownloadProgress(0);
                QTimer::singleShot(0, this, [this, sequence]() {
                    if (sequence == mRequestSeq && mBusy) _requestNext();
                });
            }
        }
    );
    if (*localError) reply->abort();
}

void Updater::install() {
    if (mBusy
        || (mInfo.mOtaStatus != DOWNLOAD_FINISHED && mInfo.mOtaStatus != INSTALL_FAILED
            && mInfo.mOtaStatus != ERROR_MD5_CHECK))
        return;

    _setOtaStatus(MD5_CHECKING);
    if (_calcFileMd5(UH_TEMP_PATH "download.temp")
        != QString::fromStdString(mLatestObject["md5"]).toLower().toStdString()) {
        _setOtaStatus(ERROR_MD5_CHECK);
    } else {
        _unzip(UH_TEMP_PATH "download.temp", UH_TEMP_PATH);
        if (!QFile(UH_TEMP_PATH "_do_update.sh").exists()) {
            _setOtaStatus(ERROR_MD5_CHECK);
        } else {
            _setOtaStatus(MD5_CHECK_SUCCESSFULLY);
            _setOtaStatus(INSTALLING);
            exec(QString("chmod +x \"%1\"").arg(UH_TEMP_PATH "_do_update.sh"));
            exec(QString("cd \"%1\" && bash _do_update.sh").arg(UH_TEMP_PATH));
            _setInstallProgress(100);
            if (QFile(UH_TEMP_PATH "INSTALL_SUCCESSFULLY").exists()) {
                _setOtaStatus(INSTALL_SUCCESSFULLY);
            } else {
                _setOtaStatus(INSTALL_FAILED);
            }
        }
    }
}

void Updater::onUiCompleted() {

    _cleanupTemp();

    _setCurrentVersion(mSelfVersion.toString());
}

std::string Updater::_calcFileMd5(const QString& path) {
    return exec(QString(R"(md5sum -b "%1" | cut -d" " -f1)").arg(path));
}

void Updater::_unzip(QString zipPath, QString toWhere) {
    exec(QString(R"(unzip -q -o "%1" -d "%2")").arg(zipPath, toWhere));
}

void Updater::_cleanupTemp() {
    util::getModuleFileInfo().absoluteDir().rmdir(UH_TEMP_DIRNAME);
    QDir(UH_TEMP_PATH).mkpath(".");
}

void Updater::_setCurrentVersion(QString verstr) {
    mInfo.mCurrentStr = std::move(verstr);
    PEN_CALL(void*, "_ZN14YUpdateManager23onCurrentVersionChangedEv", void*)(YPointer<YUpdateManager>::getInstance());
}

void Updater::_setLatestVersion(QString latest) {
    mInfo.mLatestStr = std::move(latest);
    PEN_CALL(void*, "_ZN14YUpdateManager22onTargetVersionChangedEv", void*)(YPointer<YUpdateManager>::getInstance());
}

void Updater::_setUpdateNote(QString note) {
    mInfo.mUpdateNote = std::move(note);
    PEN_CALL(void*, "_ZN14YUpdateManager19onUpdateNoteChangedEv", void*)(YPointer<YUpdateManager>::getInstance());
}

void Updater::_setUpdatePackSize(double size) {
    mInfo.mPackageSize                                                         = size;
    *(double*)(*((uint64*)YPointer<YUpdateManager>::getInstance() + 2) + 64LL) = dec(size, 1);
    PEN_CALL(void*, "_ZN14YUpdateManager20updateImgSizeChangedEv", void*)(YPointer<YUpdateManager>::getInstance());
}

void Updater::_setOtaStatus(UpdateStatus stat) {
    mInfo.mOtaStatus = stat;
    PEN_CALL(void*, "_ZN14YUpdateManager18onOtaStatusChangedEv", void*)(YPointer<YUpdateManager>::getInstance());
}

void Updater::_setDownloadProgress(int prog) {
    mInfo.mDownloadProgress = qBound(0, prog, 100);
    // The native getters are adjacent 12-byte functions; inline hooks would overlap.
    auto* state = *reinterpret_cast<char**>(reinterpret_cast<char*>(YPointer<YUpdateManager>::getInstance()) + 16);
    *reinterpret_cast<int*>(state + 104) = static_cast<int>(mInfo.mDownloadProgress);
    PEN_CALL(void*, "_ZN14YUpdateManager25onDownloadProgressChangedEv", void*)(YPointer<YUpdateManager>::getInstance());
}

void Updater::_setInstallProgress(int prog) {
    mInfo.mInstallProgress = prog;
    auto* state = *reinterpret_cast<char**>(reinterpret_cast<char*>(YPointer<YUpdateManager>::getInstance()) + 16);
    *reinterpret_cast<int*>(state + 100) = prog;
    PEN_CALL(void*, "_ZN14YUpdateManager24onInstallProgressChangedEv", void*)(YPointer<YUpdateManager>::getInstance());
}

} // namespace mod

PEN_HOOK(QString, _ZN14YUpdateManager14currentVersionEv, uint64 self, void* a2) {
    return mod::Updater::getInstance().getInfo().mCurrentStr;
}

PEN_HOOK(QString, _ZN14YUpdateManager13targetVersionEv, uint64 self, void* a2) {
    return mod::Updater::getInstance().getInfo().mLatestStr;
}

PEN_HOOK(UpdateStatus, _ZN14YUpdateManager9otaStatusEv, uint64) {
    return mod::Updater::getInstance().getInfo().mOtaStatus;
}

PEN_HOOK(QString, _ZN14YUpdateManager10updateNoteEv, uint64 self, void* a2, void* a3) {
    return mod::Updater::getInstance().getInfo().mUpdateNote;
}

// Disabled Functions;

PEN_HOOK(void, _ZN14YUpdateManager7installEv, uint64 self) {}

PEN_HOOK(void, _ZN14YUpdateManager13downloadStartEv, uint64 self) {}

PEN_HOOK(void, _ZN14YUpdateManager13downloadPauseEv, uint64 self) {}

PEN_HOOK(void, _ZN14YUpdateManager14downloadResumeEv, uint64 self) {}

PEN_HOOK(bool, _ZN21YUpdateManagerPrivate25checkOtaOperateThreadFuncEv, uint64 self) { return false; }

PEN_HOOK(bool, _ZN21YUpdateManagerPrivate32checkInstallOtaOperateThreadFuncEv, uint64 self) { return false; }

PEN_HOOK(uint64, _ZN21YUpdateManagerPrivate15checkOtaInstallEv, uint64 self) { return 0; }

PEN_HOOK(void, _ZN21YUpdateManagerPrivate14checkOtaUpdateEv, uint64 self) {}

PEN_HOOK(void, _ZN21YUpdateManagerPrivate19onPowerStatusChangeEiRK7QString, void* self, void* a2, void* a3) {}
