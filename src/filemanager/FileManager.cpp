// SPDX-License-Identifier: GPL-3.0-only
/*
 * Copyright (C) 2022-present, PenUniverse.
 * This file is part of the PenMods open source project.
 */

#include "filemanager/FileManager.h"
#include "filemanager/player/MusicPlayer.h"

#include "common/Event.h"
#include "common/Utils.h"
#include "common/service/Logger.h"

#include "mod/Mod.h"

#include "tweaker/ColumnDBLimiter.h"

#include <QDateTime>
#include <QFile>
#include <QHash>
#include <QMutexLocker>
#include <QProcess>
#include <QQmlContext>
#include <QSet>
#include <QThread>
#include <QTimer>
#include <QUrl>

#include <errno.h>
#include <sys/inotify.h>
#include <unistd.h>

namespace mod::filemanager {

static const char* HIDDEN_FLAG = ".HIDDEN_DIR";
static size_t      MAX_FILES   = 65535;

// Natural language sort flag (using a value that doesn't conflict with QDir constants)
static const int NATURAL_SORT = 0x10000; // 65536 - using a value far outside standard QDir sort flags range

FileManager::FileManager() : QAbstractListModel(), Logger("FileManager") {

    mCfg = Config::getInstance().read(mClassName);

    mOrder            = mCfg["order"]["basic"];
    mOrderReversed    = mCfg["order"]["reversed"];
    mHidePairedLyrics = mCfg["hide_paired_lyrics"];
    mShowHiddenFiles  = mCfg["show_hidden_files"];

    // 初始化路径历史，设置初始路径为根目录
    mPathHistory.push_back(mRoot);

    mRefreshTimer = new QTimer(this);
    mRefreshTimer->setSingleShot(true);
    mRefreshTimer->setInterval(350);
    connect(this, &FileManager::directoryChanged, mRefreshTimer, qOverload<>(&QTimer::start));
    connect(mRefreshTimer, &QTimer::timeout, this, [this]() {
        reload();
        if (mCurrentPath == mCurrentPlayingPath) refreshPlayList();
    });

    setupInotify();
    connect(&Event::getInstance(), &Event::uiCompleted, [this]() {
        if (shouldHiddenAll()) {
            QTimer::singleShot(15000, this, [&]() { setMtpOnoff(false); });
        }
    });
    connect(&Event::getInstance(), &Event::beforeUiInitialization, [this](QQuickView& view, QQmlContext* context) {
        context->setContextProperty("fileManager", this);
    });
}

int FileManager::rowCount(const QModelIndex& parent) const { return mProxyCount; };

QVariant FileManager::data(const QModelIndex& index, int role) const {
    if (!index.isValid()) {
        return {};
    }

    const size_t row = index.row();
    if (row < 0 || row > (size_t)mProxyCount - 1) {
        return {};
    }

    auto& entity = mEntities.at(row);

    auto getSizeString = [&]() -> QString {
        auto size = entity->size();
        if (size <= 0) {
            return "0B";
        }
        QList<QString> units = {"B", "KB", "MB", "GB"};
        for (int i = 1; i < units.size() + 1; i++) {
            auto step = pow(1024, i);
            if ((double)size <= step) {
                return QString::number((double)size / step * 1024.0, 'f', 0) + units.at(i - 1);
            }
        } // dict pen's largest storage size is 32GB, lol.
        const_cast<FileManager*>(this)->error("Abnormal file size({}) detected!", size);
        return "-1B";
    };

    auto getExtIcon = [&]() -> QString {
        if (entity->isSymLink()) {
            return "qrc:/images/format/symlink.png";
        }

        if (entity->isDir()) {
            return "qrc:/images/folder-empty.png";
        }
        auto    ext = entity->suffix().toLower();
        QString name;
        switch (H(ext.toUtf8())) {
        case H("mp3"):
        case H("flac"):
        case H("m4a"):
            name = "mp3";
            break;
        case H("md"):
            name = "md";
            break;
        case H("txt"):
        case H("lrc"):
            name = "txt";
            break;
        case H("json"):
            name = "json";
            break;
        case H("yml"):
        case H("yaml"):
        case H("xml"):
            name = "xml";
            break;
        case H("avi"):
        case H("mp4"):
        case H("mov"):
        case H("flv"):
        case H("mkv"):
        case H("webm"):
            name = "mp4";
            break;
        case H("png"):
        case H("jpg"):
        case H("jpeg"):
        case H("gif"):
        case H("bmp"):
        case H("svg"):
        case H("ico"):
        case H("webp"):
            name = "image";
            break;
        default:
            return "qrc:/images/file-empty.png";
        }
        return QString("qrc:/images/format/suffix-%1.png").arg(name);
    };

    switch ((UserRoles)role) {
    case UserRoles::FileName:
        return entity->fileName();
    case UserRoles::IsDirectory:
        return entity->isDir();
    case UserRoles::SizeString:
        return getSizeString();
    case UserRoles::ExtensionName:
        return entity->suffix().toLower();
    case UserRoles::ExtensionIcon:
        return getExtIcon();
    case UserRoles::IsExecutable:
        return entity->isExecutable();
    case UserRoles::IsSymLink:
        return entity->isSymLink();
    default:
        return {};
    }
};

QHash<int, QByteArray> FileManager::roleNames() const {
    return QHash<int, QByteArray>{
        {(int)UserRoles::FileName,      "fileName"    },
        {(int)UserRoles::IsDirectory,   "isDir"       },
        {(int)UserRoles::SizeString,    "sizeStr"     },
        {(int)UserRoles::ExtensionName, "extName"     },
        {(int)UserRoles::ExtensionIcon, "extIcon"     },
        {(int)UserRoles::IsExecutable,  "isExecutable"},
        {(int)UserRoles::IsSymLink,     "isSymLink"}
    };
};

void FileManager::onDirectoryChanged(const QString& path) {
    debug("dir-changed: {}", path.toStdString());
    auto url = QUrl(path);
    debug("url.path() {}", url.path().toStdString());
    debug("cur.path() {}", mCurrentPath.path().toStdString());
    if (url.path() == mCurrentPath.path()) {
        emit directoryChanged();
    }
    if (path == mCurrentPlayingPath.path()) {
        refreshPlayList();
    }
}

QDir const& FileManager::getCurrentPath() const { return mCurrentPath; }

QString FileManager::getCurrentPathString() const { return mCurrentPath.path(); }

bool FileManager::changeDir(const QString& dir) {
    if (dir.isEmpty()) {
        emit directoryAboutToChange(true);
        // 返回根目录
        mPathHistory.clear();
        mPathHistory.push_back(mRoot);
        mCurrentPath.setPath(mRoot);
    } else if (dir == "..") {
        // 返回上级目录
        if (mPathHistory.size() > 1) {
            emit directoryAboutToChange(true);
            mPathHistory.pop_back(); // 弹出当前路径
            QString prevPath = mPathHistory.back(); // 获取上一级路径
            mCurrentPath.setPath(prevPath);
        } else {
            // 如果已经是根目录或路径历史为空，不做任何操作
            return false;
        }
    } else {
        debug("Move to dir -> {}", dir.toStdString());

        // 检查是否是软链接目录
        QString targetPath = mCurrentPath.absoluteFilePath(dir);
        QFileInfo fileInfo(targetPath);

        QString nextPath;
        if (fileInfo.isSymLink()) {
            // 如果是软链接，获取其指向的实际路径
            QString linkTarget = fileInfo.symLinkTarget();
            if (!linkTarget.isEmpty() && QDir(linkTarget).exists()) {
                nextPath = linkTarget;
            } else {
                // 如果软链接无效，尝试直接使用原路径
                nextPath = mCurrentPath.absoluteFilePath(dir);
            }
        } else {
            nextPath = mCurrentPath.absoluteFilePath(dir);
        }

        // 验证路径是否存在
        if (!QDir(nextPath).exists()) {
            return false;
        }

        emit directoryAboutToChange(false);
        // 更新路径历史
        mCurrentPath.setPath(nextPath);
        mPathHistory.push_back(nextPath);
    }

    finishDirectoryChange();
    return true;
}

void FileManager::finishDirectoryChange() {
    mRefreshTimer->stop();
    QMutexLocker locker(&mInotifyMutex);
    addInotifyWatch(mCurrentPath.path());
    reset();
    _initCurrentDir();
    loadMore();
    emit currentTitleChanged();
    emit directoryLoaded();
}

bool FileManager::navigateToAncestor(int depth) {
    if (depth < 0 || depth >= static_cast<int>(mPathHistory.size()) - 1) return false;
    if (!QDir(mPathHistory[depth]).exists()) return false;
    emit directoryAboutToChange(true);
    mPathHistory.resize(depth + 1);
    mCurrentPath.setPath(mPathHistory.back());
    finishDirectoryChange();
    return true;
}

QStringList FileManager::getBreadcrumbs() const {
    QStringList result;
    for (size_t i = 0; i < mPathHistory.size(); ++i)
        result.append(i == 0 ? QStringLiteral("根目录") : QFileInfo(mPathHistory[i]).fileName());
    return result;
}

QString FileManager::fileNameAt(int row) const {
    return row >= 0 && row < mProxyCount ? mEntities[row]->fileName() : QString();
}

int FileManager::indexOfFile(const QString& name) const {
    for (int i = 0; i < mProxyCount; ++i) {
        if (mEntities[i]->fileName() == name) return i;
    }
    return -1;
}

bool FileManager::canCdUp() const {
    // 检查路径历史栈，如果有多个路径则可以返回上级
    return mPathHistory.size() > 1;
}

void FileManager::loadMore() { loadMore(1500); }

void FileManager::loadMore(int amount) {
    // Do not use 'isHasMore()' here!
    auto finalCount = std::min(mProxyCount + amount, (int)mEntities.size());
    if (finalCount <= mProxyCount) return;
    beginInsertRows(QModelIndex(), mProxyCount, finalCount - 1);
    mProxyCount = finalCount; // safe: limited by MAX_FILES.
    endInsertRows();
    emit hasMoreChanged();
}

void FileManager::reload() {
    emit refreshing();
    const int loadedCount = std::max(mProxyCount, 1500);
    reset();
    mCurrentPath.refresh();
    _initCurrentDir();
    loadMore(loadedCount);
    emit refreshed();
}

void FileManager::reset() {
    beginResetModel();
    mEntities.clear();
    mProxyCount = 0;
    endResetModel();
    emit hasMoreChanged();
}

void FileManager::remove(const QString& fileName) {
    if (!mCurrentPath.exists(fileName)) {
        return;
    }
    int idx = 0;
    for (const auto& i : mEntities) {
        if (i->fileName() != fileName) {
            idx++;
            continue;
        }

        QString   filePath = mCurrentPath.absoluteFilePath(fileName);
        QFileInfo fileInfo(filePath);

        if (fileInfo.isSymLink()) {
            // 如果是软链接，只需删除链接本身，而不删除目标
            mCurrentPath.remove(fileName);
        } else if (i->isDir()) {
            exec(QString("rm -rf \"%1\"").arg(filePath));
        } else {
            mCurrentPath.remove(fileName);
        }
        const bool visibleRow = idx < mProxyCount;
        if (visibleRow) beginRemoveRows(QModelIndex(), idx, idx);
        mEntities.erase(mEntities.begin() + idx);
        if (visibleRow) {
            mProxyCount--;
            endRemoveRows();
        }
        markSuspendDirChangedNotifier();
        emit hasMoreChanged();
        break;
    }
}

void FileManager::rename(const QString& fileName, const QString& newFileName) {
    if (!judgeIsLegalFileName(newFileName)) {
        showToast("文件名不能包含特殊字符", "#E9900C");
        return;
    }
    auto newFilePath = mCurrentPath.absoluteFilePath(newFileName);
    markSuspendDirChangedNotifier();
    if (!mCurrentPath.rename(fileName, newFileName)) {
        showToast("修改失败", "#E9900C");
        return;
    }
    auto idx = 0;
    for (auto& i : mEntities) {
        if (i->fileName() == fileName) {
            i->setFile(newFilePath);
            auto midx = index(idx);
            emit dataChanged(midx, midx);
            break;
        }
        idx++;
    }
}

bool FileManager::shouldHiddenAll() const { return QFile(QString("%1/%2").arg(mRoot, HIDDEN_FLAG)).exists(); }

void FileManager::negateHiddenAll() {
    QFile file(QString("%1/%2").arg(mRoot, HIDDEN_FLAG));
    if (file.exists()) {
        file.remove();
        setMtpOnoff(true);
    } else {
        file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate);
        file.write("This directory has been hidden.");
        file.close();
        setMtpOnoff(false);
    }
}

void FileManager::markSuspendDirChangedNotifier() {
    mShouldNotifyDirChanged = false;
    QTimer::singleShot(5000, this, [&]() { mShouldNotifyDirChanged = true; });
}

void FileManager::setMtpOnoff(bool onoff) {
    if (onoff && shouldHiddenAll()) {
        return;
    }
    if (onoff) {
        exec("grep usb_mtp_en /tmp/.usb_config || echo usb_mtp_en >> /tmp/.usb_config");
    } else {
        exec("sed -i '/usb_mtp_en/d' /tmp/.usb_config");
    }
    exec("/etc/init.d/S98usbdevice restart");
}

int FileManager::getOrder() const { return mOrder; }

bool FileManager::getOrderReversed() const { return mOrderReversed; }

void FileManager::setOrder(int order) {
    if (mOrder != order) {
        mOrder                 = order;
        mCfg["order"]["basic"] = order;
        WRITE_CFG;
        emit orderChanged();
    }
}

void FileManager::setOrderReversed(bool val) {
    if (mOrderReversed != val) {
        mOrderReversed            = val;
        mCfg["order"]["reversed"] = val;
        WRITE_CFG;
        emit orderReversedChanged();
    }
}

QString FileManager::getCurrentTitle() const {
    // 使用路径历史来构建标题，这样可以正确反映用户导航路径
    if (mPathHistory.empty()) {
        return "(根目录)";
    }

    // 获取当前路径相对于根目录的部分
    auto    path = mCurrentPath.path().remove(0, mRoot.size());
    QString ret  = "(根目录)";
    auto    list = path.split("/", Qt::SkipEmptyParts);
    for (auto i = 0; i < list.size(); i++) {
        auto part = " ﹥ " + list[i];
        if (i == list.size() - 1) { // final
            part = "<font color=\"white\">" + part + "</font>";
        }
        ret += part;
    }
    return ret;
}

bool FileManager::isHasMore() const {
    return !mEntities.empty() && (int)mEntities.size() > mProxyCount; // safe.
}

void FileManager::_initCurrentDir() {
    // 1. 安全检查与重置
    if (Mod::getInstance().isTrustedDevice() && shouldHiddenAll()) {
        emit error("空文件夹");
        reset();
        return;
    }

    // 确保清空旧数据，防止重复追加
    mEntities.clear();

    // 2. 准备标志位和排序参数
    auto order = getOrder();
    auto flags = QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot;
    if (getShowHiddenFiles()) {
        flags |= QDir::Hidden;
    }

    bool useNaturalSort = (order & NATURAL_SORT) != 0;
    bool isReversed     = getOrderReversed() || (order & QDir::Reversed);

    // 如果不使用自然排序，直接让 QDir 处理排序，效率更高
    QDir::SortFlags sortFlags = QDir::NoSort;
    if (!useNaturalSort) {
        sortFlags = static_cast<QDir::SortFlags>(order | QDir::DirsFirst | QDir::IgnoreCase);
        if (isReversed) {
            sortFlags |= QDir::Reversed;
        }
    }

    // 3. 获取文件列表
    QFileInfoList list = mCurrentPath.entryInfoList(flags, sortFlags);

    // 4. 基础校验
    if (list.empty()) {
        emit error("空文件夹");
        reset();
        return;
    }

    // 防止内存滥用
    if (static_cast<size_t>(list.size()) > MAX_FILES) {
        emit error("该目录下文件太多");
        reset();
        return;
    }

    // 5. 执行自然排序 (如果需要)
    if (useNaturalSort) {
        // 显式提取排序位
        bool sortByTime = (order & QDir::Time);
        bool sortBySize = (order & QDir::Size);

        std::sort(
            list.begin(),
            list.end(),
            [isReversed, sortByTime, sortBySize](const QFileInfo& a, const QFileInfo& b) {
                // 始终将文件夹排在文件前面（不受反向排序影响）
                // 如果 a 是文件夹而 b 不是，a 应该排在前面
                if (a.isDir() && !b.isDir()) {
                    return true;
                }
                // 如果 b 是文件夹而 a 不是，b 应该排在前面
                if (!a.isDir() && b.isDir()) {
                    return false;
                }

                // 如果两者都是软链接或都不是软链接，继续下面的排序逻辑
                // 如果其中一个为软链接，另一个不是，优先显示非软链接
                if (a.isSymLink() && !b.isSymLink()) {
                    return false; // 软链接排在后面
                }
                if (!a.isSymLink() && b.isSymLink()) {
                    return true; // 非软链接排在前面
                }

                // 如果两者都是文件夹或都是文件，继续下面的排序逻辑
                // 内部比较 lambda
                auto compareLogic = [&](const QFileInfo& fa, const QFileInfo& fb) -> bool {
                    QString nameA = fa.fileName();
                    QString nameB = fb.fileName();

                    // 极速路径优化
                    if (!nameA.isEmpty() && !nameB.isEmpty()) {
                        QChar cA = nameA.front();
                        QChar cB = nameB.front();
                        if (!cA.isDigit() && !cB.isDigit() && cA.toLower() != cB.toLower()) {
                            return cA.toLower() < cB.toLower();
                        }
                    }

                    if (nameA == nameB) {
                        if (sortByTime) return fa.lastModified() < fb.lastModified();
                        if (sortBySize) return fa.size() < fb.size();
                        return fa.absoluteFilePath() < fb.absoluteFilePath();
                    }

                    return FileManager::naturalCompare(nameA, nameB);
                };

                // 根据 isReversed 决定顺序
                if (isReversed) {
                    return compareLogic(b, a);
                } else {
                    return compareLogic(a, b);
                }
            }
        );
    }

    // 6. 处理同名歌词文件隐藏（先精确匹配，再模糊匹配）
    QSet<QString> filesToHide;
    if (getHidePairedLyrics()) {
        // 收集所有支持的侧挂歌词文件。
        QVector<QFileInfo> lrcFiles;
        for (const auto& i : list) {
            if (i.isFile() && isLyricSuffix(i.suffix())) lrcFiles.append(i);
        }

        const QStringList audioExts = {"mp3", "flac", "m4a", "wav", "ogg", "aac"};
        for (const auto& i : list) {
            const QString ext = i.suffix().toLower();
            if (!audioExts.contains(ext))
                continue;

            const QString baseName = i.completeBaseName();
            double        bestScore  = 0.0;
            QString       bestMatch;

            for (const auto& lrc : lrcFiles) {
                const QString lrcBase = lrc.completeBaseName();
                if (lrcBase == baseName) {
                    bestScore  = 1.0;
                    bestMatch = lrc.absoluteFilePath();
                    break;
                }
                double score = fuzzyLrcMatch(baseName, lrcBase);
                if (score > bestScore) {
                    bestScore = score;
                    bestMatch = lrc.absoluteFilePath();
                }
            }
            if (bestScore >= 0.35 && !bestMatch.isEmpty()) {
                filesToHide.insert(bestMatch);
                // 同一音频可能同时附带原版 LRC 和逐字歌词，均属于侧挂文件。
                for (const auto& lyric : lrcFiles)
                    if (lyric.completeBaseName() == baseName) filesToHide.insert(lyric.absoluteFilePath());
            }
        }
    }

    // 7. 填充 mEntities
    mEntities.reserve(list.size());

    for (const auto& i : list) {
        if (!filesToHide.empty() && filesToHide.contains(i.absoluteFilePath())) {
            continue;
        }
        if (!getShowHiddenFiles() && i.fileName().startsWith('.')) {
            continue;
        }
        mEntities.emplace_back(std::make_shared<QFileInfo>(i));
    }
}

void FileManager::forEachLoadedEntities(const std::function<void(std::shared_ptr<QFileInfo>)>& callback) {
    for (const auto& i : mEntities) {
        callback(i);
    }
}

// MusicPlayer

bool FileManager::getHidePairedLyrics() const { return mHidePairedLyrics; }

void FileManager::setHidePairedLyrics(bool val) {
    if (mHidePairedLyrics != val) {
        mHidePairedLyrics          = val;
        mCfg["hide_paired_lyrics"] = val;
        WRITE_CFG;
        emit hidePairedLyricsChanged();
    }
}

bool FileManager::getShowHiddenFiles() const { return mShowHiddenFiles; }

void FileManager::setShowHiddenFiles(bool val) {
    if (mShowHiddenFiles != val) {
        mShowHiddenFiles          = val;
        mCfg["show_hidden_files"] = val;
        WRITE_CFG;
        emit showHiddenFilesChanged();
    }
}

void FileManager::playFromView(const QString& fileName) {
    if (mCurrentPlayingPath != mCurrentPath) {
        refreshPlayList();
        mCurrentPlayingPath = mCurrentPath;

        // 更新 inotify 监视
        QMutexLocker locker(&mInotifyMutex);
        addInotifyWatch(mCurrentPlayingPath.path());
    }
    size_t idx   = 0;
    bool   valid = false;
    for (auto& file : MusicPlayer::getInstance().getPlayListRef()) {
        if (file->fileName() == fileName) {
            valid = true;
            break;
        }
        idx++;
    }
    if (valid) MusicPlayer::getInstance().play(idx);
}

void FileManager::refreshPlayList() {
    auto& list = MusicPlayer::getInstance().getPlayListRef();
    list.clear();
    forEachLoadedEntities([&](const std::shared_ptr<QFileInfo>& file) {
        const QString ext = file->suffix().toLower();
        if (ext == "mp3" || ext == "flac" || ext == "m4a" || ext == "wav" || ext == "ogg" || ext == "aac") {
            list.emplace_back(file);
        }
    });
}

void FileManager::executeFile(const QString& fileName) {
    QString   filePath = mCurrentPath.absoluteFilePath(fileName);
    QFileInfo fileInfo(filePath);

    // 如果是软链接，获取目标文件路径
    if (fileInfo.isSymLink()) {
        QString targetPath = fileInfo.symLinkTarget();
        if (!targetPath.isEmpty()) {
            filePath = targetPath;
        }
    }

    if (!QFileInfo(filePath).isExecutable()) {
        emit exception("文件不可执行");
        return;
    }

    QProcess* process = new QProcess(this);
    process->start(filePath);
    if (!process->waitForStarted()) {
        emit exception("启动失败");
        delete process;
        return;
    }
}

bool FileManager::naturalCompare(const QString& a, const QString& b) {
    const QChar* itA  = a.unicode();
    const QChar* itB  = b.unicode();
    const QChar* endA = itA + a.length();
    const QChar* endB = itB + b.length();

    while (itA != endA && itB != endB) {
        // 检查当前字符是否都是数字
        if (itA->isDigit() && itB->isDigit()) {
            // 1. 跳过前导零
            const QChar* startA = itA;
            while (itA != endA && *itA == '0') ++itA;

            const QChar* startB = itB;
            while (itB != endB && *itB == '0') ++itB;

            // 2. 计算有效数字部分的长度 (即去掉前导零后的长度)
            const QChar* significantA = itA;
            while (itA != endA && itA->isDigit()) ++itA;
            qint64 lenA = itA - significantA;

            const QChar* significantB = itB;
            while (itB != endB && itB->isDigit()) ++itB;
            qint64 lenB = itB - significantB;

            // 3. 比较逻辑
            // 规则 A: 有效数字越长，数值越大 (例如 100 > 50)
            if (lenA != lenB) {
                return lenA < lenB;
            }

            // 规则 B: 长度相同，逐位比较 (例如 123 < 124)
            // 此时不用担心溢出，因为只是字符比较
            for (qint64 i = 0; i < lenA; ++i) {
                if (significantA[i] != significantB[i]) {
                    return significantA[i] < significantB[i];
                }
            }

            // 规则 C: 数值完全相等 (例如 "01" 和 "1")
            // 此时通常比较前导零的个数，或者认为它们“相等”
            // 为了排序稳定性，通常认为较长的原始字符串（即前导零多的）排在后面或前面
            // 这里采用：原数字串较短的排在前面 (即 1 < 01)
            qint64 originLenA = itA - startA; // 包含前导零的长度
            qint64 originLenB = itB - startB;
            if (originLenA != originLenB) {
                return originLenA < originLenB;
            }

            // 如果连前导零都一样，继续循环比较后面的字符
        } else {
            // 普通字符比较：忽略大小写
            // QChar::toLower() 是内联的且很快
            QChar cA = itA->toLower();
            QChar cB = itB->toLower();

            if (cA != cB) {
                return cA < cB;
            }

            ++itA;
            ++itB;
        }
    }

    // 如果一个字符串结束了，较短的排在前面
    return (itA == endA) && (itB != endB);
}

void FileManager::setupInotify() {
    mInotifyFd = inotify_init1(IN_NONBLOCK);
    if (mInotifyFd < 0) {
        error("Failed to initialize inotify: {}", strerror(errno));
        return;
    }

    // 监听当前路径
    if (!mCurrentPath.path().isEmpty()) {
        addInotifyWatch(mCurrentPath.path());
    }

    // 同时监听播放路径
    if (!mCurrentPlayingPath.path().isEmpty()) {
        addInotifyWatch(mCurrentPlayingPath.path());
    }

    startInotifyThread();
}

void FileManager::addInotifyWatch(const QString& path) {
    if (mInotifyFd >= 0) {
        // 先移除旧的监视
        if (mWatchFd >= 0) {
            inotify_rm_watch(mInotifyFd, mWatchFd);
        }

        // 添加新的监视
        mWatchFd = inotify_add_watch(mInotifyFd, path.toLocal8Bit().data(),
                                   IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_MODIFY);
        if (mWatchFd < 0) {
            error("Failed to add inotify watch for path {}: {}", path.toStdString(), strerror(errno));
        } else {
            debug("Added inotify watch for path: {}", path.toStdString());
        }
    }
}

void FileManager::cleanupInotify() {
    stopInotifyThread();

    if (mInotifyFd >= 0) {
        if (mWatchFd >= 0) {
            inotify_rm_watch(mInotifyFd, mWatchFd);
            mWatchFd = -1;
        }
        close(mInotifyFd);
        mInotifyFd = -1;
    }
}

void FileManager::startInotifyThread() {
    if (mInotifyThread == nullptr) {
        mInotifyThread = new QThread(this);
        // 创建一个辅助对象来运行 inotify 循环
        QObject* worker = new QObject();
        worker->moveToThread(mInotifyThread);

        connect(mInotifyThread, &QThread::started, worker, [this, worker]() {
            inotifyLoop();
            worker->deleteLater(); // 确保 worker 对象被删除
        });

        connect(mInotifyThread, &QThread::finished, worker, []() {
            // 线程结束时 worker 会被自动删除
        });

        mInotifyThread->start();
    }
}

void FileManager::stopInotifyThread() {
    if (mInotifyThread != nullptr) {
        mInotifyThread->quit();
        mInotifyThread->wait();
        delete mInotifyThread;
        mInotifyThread = nullptr;
    }
}

void FileManager::inotifyLoop() {
    char buffer[4096];
    fd_set readfds;

    while (true) {
        FD_ZERO(&readfds);
        FD_SET(mInotifyFd, &readfds);

        // 使用 select 等待事件，超时时间为 1 秒，以便能够响应线程退出信号
        struct timeval timeout;
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;

        int ret = select(mInotifyFd + 1, &readfds, nullptr, nullptr, &timeout);

        if (ret < 0) {
            if (errno != EINTR) {
                error("Select error: {}", strerror(errno));
            }
            break;
        } else if (ret > 0 && FD_ISSET(mInotifyFd, &readfds)) {
            ssize_t len = read(mInotifyFd, buffer, sizeof(buffer));

            if (len > 0) {
                int i = 0;
                while (i < len) {
                    struct inotify_event* event = (struct inotify_event*)&buffer[i];

                    if (event->mask & (IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_MODIFY)) {
                        // 发出目录更改信号
                        QMetaObject::invokeMethod(this, [this]() { emit directoryChanged(); }, Qt::QueuedConnection);
                    }

                    i += sizeof(struct inotify_event) + event->len;
                }
            } else if (len < 0 && errno != EAGAIN) {
                error("Read error: {}", strerror(errno));
                break;
            }
        }

        // 检查线程是否被要求退出
        if (QThread::currentThread()->isInterruptionRequested()) {
            break;
        }
    }
}
} // namespace mod::filemanager

PEN_HOOK(void, _ZN13YMediaManager13entryMyImportEv, uint64) {}

PEN_HOOK(void, _ZN13YMediaManager16entryMyImportDirERK7QString, uint64, const QString& a2) {}

PEN_HOOK(void, _ZN13YMediaManager20launchMyImportMediasEv, uint64) {}
