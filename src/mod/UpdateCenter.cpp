// SPDX-License-Identifier: GPL-3.0-only
#include "UpdateCenter.h"
#include "UpdateLaunchStatus.h"

#include "Version.h"
#include "common/Event.h"
#include "helper/NetworkSettings.h"
#include "mod/Config.h"
#include "mod/Mod.h"
#include "mod/PpOcrPayload.h"
#include "mod/Updater.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QQmlContext>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStorageInfo>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>
#include <QVersionNumber>
#include <cmath>
#include <unistd.h>

namespace mod {
namespace {
constexpr auto   Helper      = "/userdata/PenMods/penmods-update";
constexpr qint64 MaxArchive  = 256LL * 1024 * 1024;
constexpr qint64 MaxUnpacked = 512LL * 1024 * 1024;

QString versionString(const QJsonValue& value) {
    if (value.isString()) {
        const auto text = value.toString();
        return QRegularExpression("^[0-9]{1,3}\\.[0-9]{1,3}\\.[0-9]{1,3}$").match(text).hasMatch() ? text : QString();
    }
    const auto parts = value.toArray();
    if (parts.size() != 3) return {};
    QStringList result;
    for (const auto& part : parts) {
        const double n = part.toDouble(-1);
        if (n < 0 || n > 999 || std::floor(n) != n) return {};
        result.append(QString::number(int(n)));
    }
    return result.join('.');
}

bool newer(const QString& a, const QString& b) {
    return !a.isEmpty() && (b.isEmpty() || QVersionNumber::fromString(a) > QVersionNumber::fromString(b));
}

QString buildModel() {
#if PL_BUILD_YDPG3
    return "YDPG3";
#elif PL_BUILD_YDP03X
    return "YDP03X";
#else
    return "YDP02X";
#endif
}

bool validUrl(const QString& text) {
    const QUrl url(text, QUrl::StrictMode);
    return text.size() <= 2048 && url.isValid() && url.scheme() == "https" && !url.host().isEmpty()
        && url.userInfo().isEmpty() && !url.hasFragment();
}

// Legacy core metadata is upgraded to a descriptor, but its installer is never executed.
QJsonObject descriptorFor(const QJsonObject& entry, const QString& id, bool legacy) {
    auto          result   = entry;
    const auto    version  = versionString(entry.value("version"));
    const double  size     = entry.value("size").toDouble(-1);
    const double  unpacked = entry.value("unpacked_size").toDouble(legacy ? MaxUnpacked : -1);
    const QString sha      = entry.value("sha256").toString().toLower();
    const QString md5      = entry.value("md5").toString().toLower();
    const bool    hashOk   = QRegularExpression("^[0-9a-f]{64}$").match(sha).hasMatch()
                          || (legacy && QRegularExpression("^[0-9a-f]{32}$").match(md5).hasMatch());
    if (version.isEmpty() || !hashOk || size <= 0 || size > MaxArchive || std::floor(size) != size || unpacked <= 0
        || unpacked > MaxUnpacked || std::floor(unpacked) != unpacked || !validUrl(entry.value("download").toString()))
        return {};
    if ((!legacy || entry.contains("model") || entry.contains("arch") || entry.contains("channel"))
        && (entry.value("model").toString() != buildModel() || entry.value("arch").toString() != "aarch64"
            || entry.value("channel").toString().toLower() != QStringLiteral(BUILD_CHANNEL).toLower()))
        return {};
    if (!legacy && entry.contains("requires") && !entry.value("requires").isObject()) return {};
    result.insert("id", id);
    result.insert("version", version);
    result.insert("size", size);
    result.insert("unpacked_size", unpacked);
    result.insert("model", buildModel());
    result.insert("arch", "aarch64");
    result.insert("channel", QStringLiteral(BUILD_CHANNEL).toLower());
    if (!sha.isEmpty()) result.insert("sha256", sha);
    if (!md5.isEmpty()) result.insert("md5", md5);
    result.insert("legacy", legacy);
    return result;
}

QJsonObject readObject(const QByteArray& bytes) {
    QJsonParseError error;
    const auto      doc = QJsonDocument::fromJson(bytes, &error);
    return error.error == QJsonParseError::NoError && doc.isObject() ? doc.object() : QJsonObject();
}

json updaterSettings() {
    const auto value = Config::getInstance().read("updater");
    return value.is_object() ? value : json::object();
}

bool booleanSetting(const json& object, const std::string& key, bool fallback) {
    const auto value = object.find(key);
    return value != object.end() && value->is_boolean() ? value->get<bool>() : fallback;
}

QString componentStateText(const QString& state) {
    if (state == "queued") return "已排队";
    if (state == "downloading") return "下载中";
    if (state == "staging") return "正在校验和暂存";
    if (state == "error") return "更新失败，可重试";
    return {};
}
} // namespace

UpdateCenter::UpdateCenter() : Logger("UpdateCenter") {
    for (const auto& item : QList<QPair<QString, QString>>{
             {"core",    "PenMods"      },
             {"player",  "视频播放器"   },
             {"mathjax", "数学公式渲染" },
             {"ppocr",   "PP-OCRv5 识别"}
    }) {
        Component c;
        c.id   = item.first;
        c.name = item.second;
        if (c.id == "core") {
            c.installed        = true;
            c.installedVersion = VERSION_STRING;
        }
        mComponents.append(c);
    }
    mManifestSource = Updater::getInstance().manifestUrl();
    connect(&Updater::getInstance(), &Updater::manifestReady, this, &UpdateCenter::ingestManifest);
    connect(&Updater::getInstance(), &Updater::sourceStatusChanged, this, &UpdateCenter::observeManifestCheck);
    connect(&Updater::getInstance(), &Updater::settingsChanged, this, [this]() {
        const QString source = Updater::getInstance().manifestUrl();
        if (source == mManifestSource) return;
        mManifestSource = source;
        mCatalog.clear();
        mNextAutoCheck     = QDateTime::currentDateTime().addSecs(30);
        mAutoCheckFailures = 0;
        updateCatalog();
        notify();
    });
    connect(&Event::getInstance(), &Event::beforeUiInitialization, this, [this](QQuickView&, QQmlContext* context) {
        context->setContextProperty("updateCenter", this);
    });
    connect(&Event::getInstance(), &Event::uiCompleted, this, [this]() {
        mUiReady = true;
        refresh();
        if (!qEnvironmentVariableIsEmpty("PENMODS_UPDATE_TOKEN")) {
            mHeartbeatTimer = new QTimer(this);
            mHeartbeatTimer->setInterval(2000);
            connect(mHeartbeatTimer, &QTimer::timeout, this, &UpdateCenter::writeHeartbeat);
            mHeartbeatTimer->start();
            writeHeartbeat();
        }
        mNextAutoCheck  = QDateTime::currentDateTime().addSecs(30);
        auto* scheduler = new QTimer(this);
        scheduler->setInterval(60000);
        connect(scheduler, &QTimer::timeout, this, &UpdateCenter::runAutomaticCheck);
        scheduler->start();
        QTimer::singleShot(30000, this, &UpdateCenter::runAutomaticCheck);
        connect(
            &NetworkSettings::getInstance(),
            &NetworkSettings::networkChanged,
            this,
            &UpdateCenter::runAutomaticCheck
        );
    });
}

QString UpdateCenter::rootPath() const {
    const QString root = qEnvironmentVariable("PENMODS_UPDATE_ROOT");
    return root.isEmpty() ? QStringLiteral("/userdisk/PenMods/update-system") : QDir::cleanPath(root);
}

UpdateCenter::Component* UpdateCenter::component(const QString& id) {
    for (auto& c : mComponents)
        if (c.id == id) return &c;
    return nullptr;
}

bool UpdateCenter::autoCheck() const { return booleanSetting(updaterSettings(), "auto_check", true); }
bool UpdateCenter::autoDownload() const { return booleanSetting(updaterSettings(), "auto_download", false); }

bool UpdateCenter::componentAutoUpdate(const QString& id) const {
    const auto cfg   = updaterSettings();
    const auto flags = cfg.find("component_auto");
    return flags != cfg.end() && flags->is_object() ? booleanSetting(*flags, id.toStdString(), id == "core")
                                                    : id == "core";
}

void UpdateCenter::setAutoCheck(bool enabled) {
    auto cfg          = updaterSettings();
    cfg["auto_check"] = enabled;
    if (!Config::getInstance().write("updater", cfg)) return;
    emit settingsChanged();
}

void UpdateCenter::setAutoDownload(bool enabled) {
    auto cfg             = updaterSettings();
    cfg["auto_download"] = enabled;
    if (!Config::getInstance().write("updater", cfg)) return;
    emit settingsChanged();
    if (enabled) scheduleAutomaticDownloads();
}

void UpdateCenter::setComponentAutoUpdate(QString id, bool enabled) {
    if (!component(id)) return;
    auto cfg = updaterSettings();
    if (!cfg.contains("component_auto") || !cfg["component_auto"].is_object()) cfg["component_auto"] = json::object();
    cfg["component_auto"][id.toStdString()] = enabled;
    if (!Config::getInstance().write("updater", cfg)) return;
    emit settingsChanged();
    emit componentsChanged();
    if (enabled) scheduleAutomaticDownloads();
}

QString UpdateCenter::dependencyProblem(const Component& candidate) const {
    const auto requirements = candidate.descriptor.value("requires");
    if (requirements.isUndefined()) return {};
    if (!requirements.isObject()) return "更新目录的组件依赖无效";
    const auto object = requirements.toObject();
    for (auto it = object.begin(); it != object.end(); ++it) {
        const Component* dependency = nullptr;
        for (const auto& c : mComponents)
            if (c.id == it.key()) {
                dependency = &c;
                break;
            }
        if (!dependency || !it.value().isObject()) return "更新目录包含不支持的组件依赖：" + it.key();
        const QString actual = dependency->id == candidate.id        ? candidate.descriptor.value("version").toString()
                             : !dependency->pendingVersion.isEmpty() ? dependency->pendingVersion
                                                                     : dependency->installedVersion;
        const auto    bounds = it.value().toObject();
        for (auto bound = bounds.begin(); bound != bounds.end(); ++bound) {
            const QString required = versionString(bound.value());
            if (required.isEmpty()) return "更新目录的依赖版本无效";
            if (bound.key() != "min" && bound.key() != "max_exclusive") return "更新目录包含未知依赖范围";
            const bool satisfied =
                !actual.isEmpty()
                && (bound.key() == "min" ? QVersionNumber::fromString(actual) >= QVersionNumber::fromString(required)
                                         : QVersionNumber::fromString(actual) < QVersionNumber::fromString(required));
            if (!satisfied)
                return "需要 " + dependency->name + (bound.key() == "min" ? " ≥ " : " < ") + required + "（当前 "
                     + (actual.isEmpty()    ? "未安装"
                        : actual == "0.0.0" ? "版本未知"
                                            : actual)
                     + "）";
        }
    }
    return {};
}

QVariantList UpdateCenter::components() const {
    QVariantList rows;
    for (const auto& c : mComponents) {
        const QString latest      = c.descriptor.value("version").toString();
        const bool    available   = !c.descriptor.isEmpty();
        const bool    pending     = !c.pendingVersion.isEmpty() && (!c.installed || c.pendingVersion != c.installedVersion);
        const bool    rowBusy     = c.state == "queued" || c.state == "downloading" || c.state == "staging";
        const bool    needsUpdate = available && (!c.installed || newer(latest, c.installedVersion));
        const bool    healthyCore = c.id != "core" || c.descriptor.value("health_protocol").toInt() == 1;
        const QString dependency  = dependencyProblem(c);
        QString text = c.state == "error" && !c.error.isEmpty() ? "更新失败：" + c.error : componentStateText(c.state);
        if (text.isEmpty()) {
            if (pending) text = "已暂存，重启应用后生效";
            else if (c.id == "ppocr" && !c.installed && !c.installedVersion.isEmpty())
                text = "组件文件缺失，请重新安装";
            else if (!available) text = c.installed ? "已安装，暂无更新目录" : "暂无可用版本";
            else if (needsUpdate && !healthyCore) text = "此旧版更新包不支持启动健康验证";
            else if (needsUpdate && !dependency.isEmpty()) text = dependency;
            else if (!c.installed) text = "可选扩展，尚未安装";
            else if (c.installedVersion == "0.0.0") text = "已安装，版本未知";
            else text = needsUpdate ? "有可用更新" : "已是最新版本";
        }
        const double size = c.descriptor.value("size").toDouble();
        QVariantMap  row{
            {"id", c.id},
            {"name", c.name},
            {"installedVersion", c.installedVersion},
            {"availableVersion", latest},
            {"note", c.descriptor.value("note").toString()},
            {"sizeText", size > 0 ? QString::number(size / 1024.0 / 1024.0, 'f', 1) + " MB" : QString()},
            {"stateText", text},
            {"state", c.state},
            {"error", c.error},
            {"progress", c.progress},
            {"available", available},
            {"installed", c.installed},
            {"pending", pending},
            {"busy", rowBusy},
            {"autoUpdate", componentAutoUpdate(c.id)},
            {"canDownload",
             mLauncherReady && !mTrialActive && !mCancellingPending && needsUpdate && healthyCore
                 && dependency.isEmpty() && !rowBusy && (c.pendingVersion.isEmpty() || newer(latest, c.pendingVersion))}
        };
        rows.append(row);
    }
    return rows;
}

void UpdateCenter::notify() {
    emit componentsChanged();
    emit stateChanged();
}

void UpdateCenter::ingestManifest(const QByteArray& manifest) {
    if (manifest.size() > 1024 * 1024 || readObject(manifest).isEmpty()) return;
    mCatalog = manifest;
    updateCatalog();
    notify();
    scheduleAutomaticDownloads();
}

void UpdateCenter::updateCatalog() {
    const auto manifest = readObject(mCatalog);
    for (auto& c : mComponents) c.descriptor = {};
    if (manifest.isEmpty()) return;
    // Preserve Updater's accepted target, including an explicitly required intermediate version.
    const QString target = Updater::getInstance().getInfo().mLatestStr;
    for (const auto& value : manifest.value("versions").toArray()) {
        const auto entry = value.toObject();
        if (versionString(entry.value("version")) == target) {
            component("core")->descriptor = descriptorFor(entry, "core", true);
            break;
        }
    }
    for (const auto& value : manifest.value("components").toArray()) {
        const auto item = value.toObject();
        auto*      c    = component(item.value("id").toString());
        if (!c) continue;
        for (const auto& version : item.value("versions").toArray()) {
            const auto candidate = descriptorFor(version.toObject(), c->id, false);
            if (candidate.isEmpty()) continue;
            const QString number = candidate.value("version").toString();
            if (c->id == "core" && number != target) continue;
            if (c->descriptor.isEmpty() || newer(number, c->descriptor.value("version").toString())
                || (c->id == "core" && number == target))
                c->descriptor = candidate;
        }
    }
}

void UpdateCenter::refresh() {
    if (!mUiReady || busy() || mStatusProcess || mStageProcess || mCancellingPending) return;
    if (!QFileInfo(Helper).isExecutable()) {
        mLauncherReady = false;
        mStatusText    = "独立更新启动器未安装，暂不能安装更新";
        notify();
        return;
    }
    auto* process  = new QProcess(this);
    mStatusProcess = process;
    auto done      = std::make_shared<bool>(false);
    auto finish    = [this, process, done](bool ok) {
        if (*done) return;
        *done = true;
        mStatusProcess.clear();
        const auto status = readObject(process->readAllStandardOutput());
        const bool storeReady = ok && !status.isEmpty() && status.value("installed").toBool(false)
                             && !status.value("disabled").toBool(false)
                             && !QFile::exists(QDir(rootPath()).filePath("disabled"));
        mLauncherReady = storeReady && updateLaunchHookReady()
                      && (!status.contains("launcher_ready") || status.value("launcher_ready").toBool(false));
        mTrialActive = status.value("trial").isObject();
        if (storeReady) {
            const auto installed = status.value("components").toObject();
            const auto pending   = status.value("pending_components").toObject();
            mPendingRestart      = !status.value("pending").toString().isEmpty();
            for (auto& c : mComponents) {
                const auto item    = installed.value(c.id).toObject();
                c.installed        = !item.isEmpty();
                if (c.id == "ppocr") {
                    const QString shim = item.value("path").toString();
                    c.installed = ppocr::managedPayloadReady(shim, "/userdisk/ppocr_models");
                }
                c.installedVersion = item.value("version").toString();
                c.pendingVersion   = pending.value(c.id).toObject().value("version").toString();
                // The running core reports its real version even when an imported baseline was unknown.
                if (c.id == "core") {
                    c.installed        = true;
                    c.installedVersion = VERSION_STRING;
                }
            }
            QString localError;
            for (const auto& c : mComponents)
                if (c.state == "error" && !c.error.isEmpty()) {
                    localError = c.name + "：" + c.error;
                    break;
                }
            const QString lastError = status.value("last_error").toString();
            mStatusText             = !mLauncherReady       ? "启动入口未挂接，暂存更新无法在重启时激活；请重新安装启动器"
                                    : mTrialActive          ? "正在验证新版本启动状态，稍后可继续更新"
                                    : mPendingRestart       ? "更新已暂存，下次启动应用时激活"
                                    : !localError.isEmpty() ? localError
                                    : !lastError.isEmpty()  ? "上次更新已回滚：" + lastError
                                                            : "独立更新启动器已就绪";
        } else {
            mStatusText = "独立更新启动器尚未接管应用启动，暂不能安装更新";
            if (!ok) warn("Update helper status failed: {}", process->readAllStandardError().left(2048).toStdString());
        }
        process->deleteLater();
        updateCatalog();
        notify();
        if (mLauncherReady) scheduleAutomaticDownloads();
        startNextDownload();
    };
    connect(
        process,
        QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
        this,
        [finish](int code, QProcess::ExitStatus status) { finish(code == 0 && status == QProcess::NormalExit); }
    );
    connect(process, &QProcess::errorOccurred, this, [finish](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) finish(false);
    });
    QTimer::singleShot(15000, process, [process]() {
        if (process->state() != QProcess::NotRunning) process->kill();
    });
    process->start(Helper, {"--root", rootPath(), "status"});
}

void UpdateCenter::runAutomaticCheck() {
    auto&      updater = Updater::getInstance();
    const auto now     = QDateTime::currentDateTime();
    if (!mUiReady || !autoCheck() || updater.busy() || now < mNextAutoCheck) return;
    if (!updater.hasManifest()) updater.ensureChecked();
    if (updater.busy()) return;
    const auto fetched = updater.manifestFetchedAt();
    if (fetched.isValid() && fetched <= now && fetched.secsTo(now) < 24 * 60 * 60) {
        mNextAutoCheck = fetched.addSecs(24 * 60 * 60);
        return;
    }
    updater.check();
    // A preflight rejection (for example low battery) has no request completion signal.
    if (!updater.busy()) mNextAutoCheck = now.addSecs(5 * 60);
}

void UpdateCenter::observeManifestCheck() {
    auto& updater = Updater::getInstance();
    if (updater.checking()) {
        mObservedCheck = true;
        return;
    }
    if (!mObservedCheck || updater.busy()) return;
    mObservedCheck = false;
    if (updater.sourceConnected() && updater.manifestFetchedAt().isValid()) {
        mAutoCheckFailures = 0;
        mNextAutoCheck     = updater.manifestFetchedAt().addSecs(24 * 60 * 60);
    } else {
        const int delay = qMin(3600, 300 * (1 << qMin(mAutoCheckFailures, 4)));
        ++mAutoCheckFailures;
        mNextAutoCheck = QDateTime::currentDateTime().addSecs(delay);
    }
}

void UpdateCenter::scheduleAutomaticDownloads() {
    if (!mUiReady || !mLauncherReady || !autoDownload() || mAutoScheduled) return;
    mAutoScheduled = true;
    QTimer::singleShot(0, this, [this]() {
        mAutoScheduled = false;
        if (!autoDownload()) return;
        for (const auto& row : components()) {
            const auto    map = row.toMap();
            const QString id  = map.value("id").toString();
            const auto*   c   = component(id);
            // Automatic downloads never opt an absent extension into installation.
            // Legacy MD5 core packages remain available for explicit manual installation.
            const QString attempt = id + ":" + c->descriptor.value("sha256").toString();
            if (map.value("installed").toBool() && map.value("canDownload").toBool() && componentAutoUpdate(id)
                && !c->descriptor.value("sha256").toString().isEmpty() && !mAutomaticAttempts.contains(attempt)) {
                mAutomaticAttempts.insert(attempt);
                downloadComponent(id);
            }
        }
    });
}

void UpdateCenter::downloadComponent(QString id) {
    for (const auto& value : components()) {
        const auto row = value.toMap();
        if (row.value("id").toString() != id || !row.value("canDownload").toBool()) continue;
        auto* c = component(id);
        mQueue.append({id, c->descriptor});
        c->state = "queued";
        c->error.clear();
        c->progress = 0;
        notify();
        startNextDownload();
        return;
    }
}

void UpdateCenter::startNextDownload() {
    if (!mActiveId.isEmpty() || mQueue.isEmpty() || mCancellingPending || mStatusProcess) return;
    const auto task         = mQueue.takeFirst();
    mActiveId               = task.id;
    mActiveDescriptor       = task.descriptor;
    mSources                = Updater::getInstance().sourcesFor(mActiveDescriptor.value("download").toString());
    const QString directory = QDir(rootPath()).filePath("downloads");
    if (!QDir().mkpath(directory)) {
        finishDownload("无法创建下载目录");
        return;
    }
    mDownloadDir = std::make_shared<QTemporaryDir>(directory + "/download-XXXXXX");
    if (!mDownloadDir->isValid()) {
        finishDownload("无法创建下载暂存目录");
        return;
    }
    const auto   available = QStorageInfo(directory).bytesAvailable();
    const qint64 needed    = 3 * qint64(mActiveDescriptor.value("size").toDouble())
                           + qint64(mActiveDescriptor.value("unpacked_size").toDouble()) + 16 * 1024 * 1024;
    if (available < needed) {
        finishDownload("存储空间不足，无法安全暂存更新");
        return;
    }
    requestNextSource();
}

void UpdateCenter::requestNextSource() {
    if (mActiveId.isEmpty()) return;
    if (mSources.isEmpty()) {
        finishDownload("所有下载源均失败，或文件大小与校验值不符");
        return;
    }
    const QString url = mSources.takeFirst();
    if (!validUrl(url)) {
        requestNextSource();
        return;
    }
    auto* c                  = component(mActiveId);
    c->state                 = "downloading";
    c->progress              = 0;
    mStatusText              = "正在下载：" + c->name + "（" + QUrl(url).host() + "）";
    const quint64   sequence = ++mRequestSequence;
    auto*           manager  = new QNetworkAccessManager(this);
    QNetworkRequest request{QUrl(url)};
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    auto* reply = manager->get(request);
    mReply      = reply;
    reply->setReadBufferSize(64 * 1024);
    const auto   dir          = mDownloadDir;
    auto         file         = std::make_shared<QSaveFile>(dir->filePath("archive.zip"));
    auto         failed       = std::make_shared<bool>(!file->open(QIODevice::WriteOnly));
    auto         received     = std::make_shared<qint64>(0);
    const qint64 expected     = qint64(mActiveDescriptor.value("size").toDouble());
    const bool   sha          = !mActiveDescriptor.value("sha256").toString().isEmpty();
    const auto   expectedHash = mActiveDescriptor.value(sha ? "sha256" : "md5").toString().toLatin1();
    auto  hash = std::make_shared<QCryptographicHash>(sha ? QCryptographicHash::Sha256 : QCryptographicHash::Md5);
    auto* idle = new QTimer(reply);
    idle->setInterval(15000);
    idle->setSingleShot(true);
    connect(idle, &QTimer::timeout, reply, &QNetworkReply::abort);
    idle->start();
    QTimer::singleShot(600000, reply, &QNetworkReply::abort);
    auto consume = [reply, file, failed, received, hash, expected, idle]() {
        const auto bytes = reply->readAll();
        if (!bytes.isEmpty()) idle->start();
        if (*failed || *received + bytes.size() > expected) {
            *failed = true;
            reply->abort();
            return;
        }
        if (file->write(bytes) != bytes.size()) {
            *failed = true;
            reply->abort();
            return;
        }
        hash->addData(bytes);
        *received += bytes.size();
    };
    connect(reply, &QNetworkReply::readyRead, this, consume);
    connect(reply, &QNetworkReply::downloadProgress, this, [this, sequence, expected](qint64 bytes, qint64) {
        if (sequence != mRequestSequence || mActiveId.isEmpty()) return;
        component(mActiveId)->progress = int(qBound<qint64>(0, bytes * 100 / expected, 100));
        emit componentsChanged();
    });
    connect(
        reply,
        &QNetworkReply::finished,
        this,
        [this, sequence, reply, manager, file, failed, received, hash, expected, expectedHash, consume, dir]() {
            if (sequence != mRequestSequence) return;
            mReply.clear();
            consume();
            bool ok = !*failed && reply->error() == QNetworkReply::NoError
                   && reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200 && *received == expected
                   && hash->result().toHex() == expectedHash;
            if (ok) ok = file->commit();
            else file->cancelWriting();
            reply->deleteLater();
            manager->deleteLater();
            if (ok) stageDownloadedArchive();
            else
                QTimer::singleShot(0, this, [this, sequence]() {
                    if (sequence == mRequestSequence) requestNextSource();
                });
        }
    );
    notify();
    if (*failed) reply->abort();
}

void UpdateCenter::stageDownloadedArchive() {
    auto* c     = component(mActiveId);
    c->state    = "staging";
    c->progress = 100;
    mStatusText = "正在安全暂存：" + c->name;
    QSaveFile descriptor(mDownloadDir->filePath("descriptor.json"));
    auto      metadata = mActiveDescriptor;
    metadata.remove("download");
    metadata.remove("note");
    metadata.remove("legacy");
    const auto bytes = QJsonDocument(metadata).toJson(QJsonDocument::Compact);
    if (!descriptor.open(QIODevice::WriteOnly) || descriptor.write(bytes) != bytes.size() || !descriptor.commit()) {
        finishDownload("无法保存更新描述文件");
        return;
    }
    auto* process = new QProcess(this);
    mStageProcess = process;
    auto done     = std::make_shared<bool>(false);
    auto finish   = [this, process, done](bool ok) {
        if (*done) return;
        *done = true;
        mStageProcess.clear();
        const auto result  = readObject(process->readAllStandardOutput());
        const auto message = QString::fromUtf8(process->readAllStandardError().left(4096)).trimmed();
        if (ok && !result.isEmpty()) {
            component(mActiveId)->pendingVersion = mActiveDescriptor.value("version").toString();
            mPendingRestart                      = true;
            finishDownload();
        } else finishDownload(message.isEmpty() ? "原生更新器拒绝了更新包，原版本保持不变" : message);
        process->deleteLater();
        refresh();
    };
    connect(
        process,
        QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
        this,
        [finish](int code, QProcess::ExitStatus status) { finish(code == 0 && status == QProcess::NormalExit); }
    );
    connect(process, &QProcess::errorOccurred, this, [finish](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) finish(false);
    });
    // A killed staging process cannot publish a partially extracted generation; helper owns recovery.
    QTimer::singleShot(600000, process, [process]() {
        if (process->state() != QProcess::NotRunning) process->kill();
    });
    process->start(
        Helper,
        {"--root",
         rootPath(),
         "stage",
         mDownloadDir->filePath("descriptor.json"),
         mDownloadDir->filePath("archive.zip")}
    );
    notify();
}

void UpdateCenter::finishDownload(const QString& failure) {
    if (auto* c = component(mActiveId)) {
        c->state    = failure.isEmpty() ? QString() : QStringLiteral("error");
        c->error    = failure;
        mStatusText = failure.isEmpty() ? "更新已暂存，下次启动应用时激活" : c->name + "：" + failure;
    }
    mActiveId.clear();
    mActiveDescriptor = {};
    mDownloadDir.reset();
    mSources.clear();
    notify();
    QTimer::singleShot(0, this, &UpdateCenter::startNextDownload);
}

void UpdateCenter::cancelDownload() {
    for (const auto& task : mQueue) {
        if (auto* c = component(task.id)) c->state.clear();
    }
    mQueue.clear();
    if (mStageProcess) {
        mStatusText = "正在完成安全暂存，已取消后续下载";
        notify();
        return;
    }
    ++mRequestSequence;
    if (mReply) {
        auto* reply = mReply.data();
        mReply.clear();
        disconnect(reply, nullptr, this, nullptr);
        reply->abort();
        reply->manager()->deleteLater();
    }
    if (auto* c = component(mActiveId)) {
        c->state.clear();
        c->progress = 0;
    }
    mActiveId.clear();
    mActiveDescriptor = {};
    mDownloadDir.reset();
    mSources.clear();
    mStatusText = "下载已取消，已暂存的更新保持不变";
    notify();
}

void UpdateCenter::cancelPending() {
    if (busy() || !mPendingRestart || !mLauncherReady || mCancellingPending || mStatusProcess) return;
    mCancellingPending = true;
    auto* process      = new QProcess(this);
    auto  done         = std::make_shared<bool>(false);
    auto  finish       = [this, process, done](bool ok) {
        if (*done) return;
        *done              = true;
        mCancellingPending = false;
        if (ok) {
            mPendingRestart = false;
            for (auto& c : mComponents) c.pendingVersion.clear();
        }
        mStatusText = ok ? "已取消待激活更新" : "取消失败，待激活更新保持不变";
        process->deleteLater();
        notify();
        refresh();
    };
    connect(
        process,
        QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
        this,
        [finish](int code, QProcess::ExitStatus status) { finish(code == 0 && status == QProcess::NormalExit); }
    );
    connect(process, &QProcess::errorOccurred, this, [finish](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) finish(false);
    });
    QTimer::singleShot(15000, process, [process]() {
        if (process->state() != QProcess::NotRunning) process->kill();
    });
    process->start(Helper, {"--root", rootPath(), "cancel"});
    notify();
}

void UpdateCenter::restartNow() {
    if (busy() || mCancellingPending || !mPendingRestart || !mLauncherReady) return;
    Mod::getInstance().restartApplication();
}

void UpdateCenter::writeHeartbeat() {
    const QString token      = qEnvironmentVariable("PENMODS_UPDATE_TOKEN");
    const QString generation = qEnvironmentVariable("PENMODS_UPDATE_GENERATION");
    if (!mUiReady || generation.isEmpty() || !QRegularExpression("^[A-Za-z0-9_-]{1,128}$").match(token).hasMatch())
        return;
    // Confirmation removes the trial journal. Stop writes after observing it, rather than
    // writing an unnecessary heartbeat to flash for the lifetime of a confirmed app.
    if (mHealthSequence > 0 && mHealthSequence % 10 == 0) {
        QFile state(QDir(rootPath()).filePath("state.json"));
        if (state.open(QIODevice::ReadOnly) && state.size() <= 64 * 1024) {
            const auto journal = readObject(state.readAll());
            if (!journal.isEmpty() && journal.value("trial").toObject().value("token").toString() != token) {
                if (mHeartbeatTimer) mHeartbeatTimer->stop();
                QFile::remove(QDir(rootPath()).filePath("health/" + token + ".json"));
                refresh();
                return;
            }
        }
    }
    const QString directory = QDir(rootPath()).filePath("health");
    if (!QDir().mkpath(directory)) return;
    QJsonObject heartbeat{
        {"generation", generation               },
        {"token",      token                    },
        {"pid",        int(getpid())            },
        {"sequence",   double(++mHealthSequence)}
    };
    const auto bytes = QJsonDocument(heartbeat).toJson(QJsonDocument::Compact);
    QSaveFile  file(QDir(directory).filePath(token + ".json"));
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
        warn("Cannot write update launcher heartbeat");
}

} // namespace mod
