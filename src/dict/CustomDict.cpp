// SPDX-License-Identifier: GPL-3.0-only
/*
 * Copyright (C) 2022-present, PenUniverse.
 * This file is part of the PenMods open source project.
 */

#include "dict/CustomDict.h"

#include "common/Event.h"

#include "base/Hook.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQmlContext>

#include <dlfcn.h>

namespace mod {

namespace {

/// 用户放自造词典的地方（和 plugins 一样放在 /userdisk/PenMods 下）
constexpr char const* DICT_DIR = "/userdisk/PenMods/dicts";

// 容器头里的两个版本号，见 doc/DICT_FORMAT_ANALYSIS.md
constexpr quint64 TYPE_V2 = 0x1004;
constexpr quint64 TYPE_V1 = 0x1044;

/// 读容器头：只需要版本、显示名和词头数（列表用），不认识格式就返回 false
bool readHeader(const QString& file, bool& isV2, QString& name, int& words) {
    QFile handle(file);
    if (!handle.open(QIODevice::ReadOnly)) {
        return false;
    }
    const QByteArray head = handle.read(512);
    if (head.size() < 26) {
        return false;
    }
    const auto* p   = reinterpret_cast<const unsigned char*>(head.constData());
    auto        u64 = [&p](size_t offset) -> quint64 {
        quint64 v = 0;
        for (int i = 7; i >= 0; --i) {
            v = (v << 8) | p[offset + i];
        }
        return v;
    };
    const quint64 version = u64(0);
    if (version != TYPE_V2 && version != TYPE_V1) {
        return false;
    }
    const int nameLen = p[16];
    if (nameLen <= 0 || 25 + nameLen + 8 > head.size()) {
        return false;
    }
    isV2  = version == TYPE_V2;
    name  = QString::fromUtf8(head.constData() + 17, nameLen);
    words = static_cast<int>(u64(25 + nameLen));
    return !name.isEmpty();
}

} // namespace

/**
 * @brief `libDictManager.so` 里 `YQueryDictManager` 的最小包装
 *
 * - 只 dlsym QString 接口：libPenMods.so 是 libc++，厂商类是 libstdc++，
 *   双方唯一共同的 ABI 是 Qt 的 QString。
 * - 对象本体 16 字节（构造函数只做 `stp xzr,xzr,[x0]`，把两个指针清零）。
 */
struct CustomDict::Vendor {
    alignas(16) unsigned char storage[16]    = {};
    void* handle                             = nullptr;
    bool  live                               = false;
    void* (*ctor)(void*)                     = nullptr;
    bool (*open)(void*, const QString&)      = nullptr;
    QString (*lookUp)(void*, const QString&) = nullptr;
    void (*dtor)(void*)                      = nullptr;

    ~Vendor() {
        if (live && dtor) {
            dtor(storage);
        }
        if (handle) {
            dlclose(handle);
        }
    }

    bool load() {
        if (handle) {
            return live;
        }
        handle = dlopen("libDictManager.so.1", RTLD_LAZY);
        if (!handle) {
            return false;
        }
        ctor   = reinterpret_cast<decltype(ctor)>(dlsym(handle, "_ZN17YQueryDictManagerC1Ev"));
        dtor   = reinterpret_cast<decltype(dtor)>(dlsym(handle, "_ZN17YQueryDictManagerD1Ev"));
        open   = reinterpret_cast<decltype(open)>(dlsym(handle, "_ZN17YQueryDictManager4openERK7QString"));
        lookUp = reinterpret_cast<decltype(lookUp)>(dlsym(handle, "_ZN17YQueryDictManager6lookUpERK7QString"));
        if (!ctor || !open || !lookUp) {
            dlclose(handle);
            handle = nullptr;
            return false;
        }
        ctor(storage);
        live = true;
        return true;
    }
};

CustomDict::CustomDict() : Logger("CustomDict") {
    connect(&Event::getInstance(), &Event::beforeUiInitialization, [this](QQuickView& view, QQmlContext* context) {
        context->setContextProperty("customDict", this);
    });
    QDir().mkpath(directory()); // 目录不存在时先建好，用户直接往里丢文件即可
    setStatus(QStringLiteral("把自造词典放到 %1").arg(directory()));
    refresh(); // 启动时扫一次，设置页里那行才能显示真实数量
}

CustomDict::~CustomDict() = default;

/// 结果页里的词典类型：宿主 YEnumWrapper::DictType 用到的值是 0..9 与 407，
/// 这里取一个宿主不会派发的值，QML 用 `customDict.dictTypeId` 匹配并载入自己的渲染器。
int CustomDict::dictTypeId() { return 900; }

/// 厂商的 lookUp 返回的是 "词\tJSON"（结果页那侧会自己切掉词头）
QString CustomDict::stripWordPrefix(const QString& raw) {
    const int tab = raw.indexOf(QChar('\t'));
    return tab >= 0 ? raw.mid(tab + 1) : raw.trimmed();
}

QString CustomDict::directory() { return QString::fromLatin1(DICT_DIR); }

QVariantList CustomDict::dictionaries() const {
    QVariantList list;
    for (const auto& entry : mEntries) {
        list.append(
            QVariantMap{
                {QStringLiteral("name"),  entry.name },
                {QStringLiteral("file"),  entry.file },
                {QStringLiteral("words"), entry.words},
        }
        );
    }
    return list;
}

QString CustomDict::name() const {
    return (mIndex >= 0 && mIndex < static_cast<int>(mEntries.size())) ? mEntries[mIndex].name : QString();
}

QString CustomDict::path() const {
    return (mIndex >= 0 && mIndex < static_cast<int>(mEntries.size())) ? mEntries[mIndex].file : QString();
}

int CustomDict::wordCount() const {
    return (mIndex >= 0 && mIndex < static_cast<int>(mEntries.size())) ? mEntries[mIndex].words : 0;
}

void CustomDict::setStatus(const QString& status) {
    if (mStatus == status) {
        return;
    }
    mStatus = status;
    emit statusChanged();
}

void CustomDict::setRecord(const QString& record) {
    if (mRecord == record) {
        return;
    }
    mRecord = record;
    emit recordChanged();
}

void CustomDict::setWord(const QString& word) {
    if (mWord == word) {
        return;
    }
    mWord = word;
    emit wordChanged();
}

bool CustomDict::scanEntries() {
    const QFileInfoList files = QDir(directory()).entryInfoList({QStringLiteral("*.dat")}, QDir::Files, QDir::Name);

    QString            stamp;
    std::vector<Entry> entries;
    for (const QFileInfo& fileInfo : files) {
        bool    isV2  = false;
        QString name  = fileInfo.completeBaseName();
        int     words = 0;
        if (!readHeader(fileInfo.absoluteFilePath(), isV2, name, words)) {
            debug("skip {}: not a localdict container", fileInfo.fileName().toStdString());
            continue;
        }

        // 引擎（YQueryDictManager::open）先在 ".dat" 前插 "V2" 找 xxxV2.dat，找不到才退回 xxx.dat；
        // 而且**一旦用了 xxxV2.dat 就会把同目录的 xxx.dat 删掉**（厂商的旧文件清理，实测 13MB 的
        // webster.dat 就被它删了）。所以这里只按引擎的约定把基名交给它，并且保证目录里不会
        // 出现 xxx.dat 与 xxxV2.dat 并存——否则它会删掉用户的原文件。
        const QString dir  = fileInfo.absolutePath();
        const QString stem = fileInfo.completeBaseName();
        QString       file = fileInfo.absoluteFilePath();

        if (isV2 && !stem.endsWith(QStringLiteral("V2"))) {
            // V2 容器用了非约定命名：改名为 xxxV2.dat（引擎自己也是这么命名的）
            const QString v2Name = dir + QChar('/') + stem + QStringLiteral("V2.dat");
            if (QFile::exists(v2Name)) {
                warn("skip {}: {} already exists", file.toStdString(), v2Name.toStdString());
                continue;
            }
            if (!QFile::rename(file, v2Name)) {
                warn("cannot rename {} to {}", file.toStdString(), v2Name.toStdString());
                continue;
            }
            info("renamed {} to {} (engine looks for the V2 name)", file.toStdString(), v2Name.toStdString());
            file = v2Name;
        }

        const bool    v2Named = file.endsWith(QStringLiteral("V2.dat"));
        const QString base    = v2Named ? file.left(file.size() - 6) + QStringLiteral(".dat") : file;
        if (QFile::exists(base) && base != file) {
            warn("skip {}: engine would delete {} when opening the V2 file", file.toStdString(), base.toStdString());
            continue;
        }
        if (!v2Named && QFile::exists(file.left(file.size() - 4) + QStringLiteral("V2.dat"))) {
            warn("skip {}: a V2 sibling exists and the engine would delete this file", file.toStdString());
            continue;
        }

        const QFileInfo current(file);
        const QString   entryStamp =
            QStringLiteral("%1:%2").arg(current.size()).arg(current.lastModified().toMSecsSinceEpoch());
        stamp += current.fileName() + QLatin1Char('=') + entryStamp + QLatin1Char(';');
        entries.push_back(Entry{current.fileName(), base, name, entryStamp, words, isV2});
    }

    if (stamp == mScanStamp) {
        return false;
    }
    mScanStamp = stamp;
    mEntries   = std::move(entries);
    return true;
}

void CustomDict::rescanIfChanged() {
    if (!scanEntries()) {
        return;
    }
    const int keep = mIndex;
    if (mEntries.empty()) {
        mIndex = -1;
    } else if (mIndex < 0 || mIndex >= static_cast<int>(mEntries.size())) {
        mIndex = 0;
    }
    mLoaded.clear(); // 缓存的读取器由 stamp 校验，这里只清掉"当前打开的是谁"
    if (mIndex != keep) {
        setRecord(QString());
        emit selectionChanged();
    }
    emit dictionariesChanged();
}

void CustomDict::refresh() {
    scanEntries();
    if (mEntries.empty()) {
        mIndex = -1;
    } else if (mIndex < 0 || mIndex >= static_cast<int>(mEntries.size())) {
        mIndex = 0;
    }
    mLoaded.clear();
    setRecord(QString());
    if (mEntries.empty()) {
        setStatus(QStringLiteral("没有找到词典，把 .dat 放进 %1").arg(directory()));
    } else {
        setStatus(QStringLiteral("%1 本词典 · %2 词").arg(mEntries.size()).arg(wordCount()));
    }
    emit dictionariesChanged();
    emit selectionChanged();
}

void CustomDict::setIndex(int index) {
    if (index < 0 || index >= static_cast<int>(mEntries.size()) || index == mIndex) {
        return;
    }
    mIndex = index;
    mLoaded.clear();
    setRecord(QString());
    setStatus(QStringLiteral("%1 · %2 词").arg(name()).arg(wordCount()));
    emit selectionChanged();
}

CustomDict::Vendor* CustomDict::readerFor(const Entry& entry) {
    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    auto it = mReaders.find(entry.openPath);
    if (it != mReaders.end() && it->second.stamp == entry.stamp) {
        it->second.lastUsed = now;
        return it->second.vendor.get();
    }
    if (it != mReaders.end()) {
        debug("{} changed on disk, reopening", entry.file.toStdString());
        mReaders.erase(it); // 文件被换掉了，旧索引作废
    }

    // 每个读取器会把词典索引读进内存，所以只留最近用过的几本
    constexpr int kMaxReaders = 4;
    while (static_cast<int>(mReaders.size()) >= kMaxReaders) {
        auto oldest = mReaders.begin();
        for (auto scan = mReaders.begin(); scan != mReaders.end(); ++scan) {
            if (scan->second.lastUsed < oldest->second.lastUsed) {
                oldest = scan;
            }
        }
        mReaders.erase(oldest);
    }

    auto vendor = std::make_unique<Vendor>();
    if (!vendor->load() || !vendor->open(vendor->storage, entry.openPath)) {
        warn("cannot open {}", entry.openPath.toStdString());
        return nullptr;
    }
    auto* raw =
        &*mReaders.emplace(entry.openPath, CachedReader{std::move(vendor), entry.stamp, now}).first->second.vendor;
    return raw;
}

bool CustomDict::ensureOpen() {
    if (mIndex < 0 || mIndex >= static_cast<int>(mEntries.size())) {
        setStatus(QStringLiteral("没有选中词典"));
        return false;
    }
    const Entry& entry = mEntries[mIndex];
    if (!readerFor(entry)) {
        setStatus(QStringLiteral("打不开 %1").arg(entry.file));
        return false;
    }
    mLoaded = entry.openPath;
    return true;
}

bool CustomDict::rawRecordFor(const QString& word, Entry const** entry, QString* record) {
    rescanIfChanged(); // 用户刚丢进来的词典，下一次查询就能用，不必重启
    for (const Entry& candidate : mEntries) {
        Vendor* vendor = readerFor(candidate);
        if (!vendor) {
            continue;
        }
        const QString raw = vendor->lookUp(vendor->storage, word);
        if (raw.isEmpty()) {
            continue;
        }
        *entry  = &candidate;
        *record = stripWordPrefix(raw);
        return true;
    }
    return false;
}

/// 主查询（结果页）里的一段自造词典内容
///
/// 结果页按 dictType 选择渲染器，宿主没有"自定义词典"这个类型，所以这里用一个宿主不会派发的值，
/// 由 PenMods 的 QML（dicts/YDictTypeDtPenMods.qml）渲染。内容是一个薄包装：
/// {"name": 词典名, "word": 词, "record": 原始记录}（记录不是 JSON 时用 "raw": 文本）。
QString CustomDict::payloadFor(const QString& word) {
    const QString key = word.trimmed();
    if (key.isEmpty() || mEntries.empty()) {
        return {};
    }
    Entry const* entry = nullptr;
    QString      record;
    if (!rawRecordFor(key, &entry, &record) || !entry) {
        debug("payloadFor: no custom dict has {}", key.toStdString());
        return {};
    }
    debug("payloadFor: hit in {} ({} bytes)", entry->name.toStdString(), record.size());

    QJsonObject wrapper{
        {QStringLiteral("name"), entry->name},
        {QStringLiteral("word"), key        },
    };
    QJsonParseError     error{};
    const QJsonDocument document = QJsonDocument::fromJson(record.toUtf8(), &error);
    if (error.error == QJsonParseError::NoError && document.isObject()) {
        wrapper.insert(QStringLiteral("record"), document.object());
    } else {
        wrapper.insert(QStringLiteral("raw"), record);
    }

    return QString::fromUtf8(QJsonDocument(wrapper).toJson(QJsonDocument::Compact));
}

void CustomDict::lookup() {
    setRecord(QString());
    if (mWord.trimmed().isEmpty()) {
        setStatus(QStringLiteral("先输入一个词"));
        return;
    }
    if (!ensureOpen()) {
        return;
    }
    Vendor*       vendor = readerFor(mEntries[mIndex]);
    const QString raw    = vendor->lookUp(vendor->storage, mWord.trimmed());
    if (raw.isEmpty()) {
        setStatus(QStringLiteral("「%1」不在《%2》里").arg(mWord).arg(name()));
        return;
    }
    setRecord(stripWordPrefix(raw));
    setStatus(QStringLiteral("命中「%1」· %2 字符").arg(mWord).arg(mRecord.size()));
}

void CustomDict::lookupWord(const QString& word) {
    setWord(word);
    lookup();
}

/**
 * 把自造词典接进主查询流程：扫描/手输一个词之后，结果页里会多出一段自造词典的内容。
 *
 * 挂在这里而不是 queryAndShowEnglish/Chinese 上，原因有两个：
 *  - 那些函数只覆盖部分入口（扫描/手输/翻译各有各的路径），addResult 是所有路径的必经之地；
 *  - 宿主在查询开始时会调用 wipeData()，随后只有"查询进行中"这个状态才接受 addResult，
 *    所以注入必须发生在宿主自己 addResult 的那段时间里。
 * 做法：wipeData 时清标志，本次查询的**第一次** addResult 之后追加我们那一条（排在宿主的
 * 第一段之后，既醒目又不会顶掉第一段词典的发音/单词本那一套首段 UI）。
 */
namespace {
bool g_injected       = false; ///< 本次查询已经注入过
bool g_injectedCustom = false; ///< 本次查询注入了自造词典内容
} // namespace

PEN_HOOK(void, _ZN14YResultManager8wipeDataEv, void* self) {
    g_injected       = false;
    g_injectedCustom = false;
    origin(self);
}

PEN_HOOK(
    void,
    _ZN14YResultManager9addResultERK7QStringS2_RKN12YEnumWrapper8DictTypeE,
    void*          self,
    const QString& key,
    const QString& content,
    void*          type
) {
    // 先插自造词典那一段，让它排在结果页最上面（第一段会带上发音/单词本那一套按钮）
    if (!g_injected) {
        g_injected            = true;
        const QString payload = CustomDict::getInstance().payloadFor(key);
        if (!payload.isEmpty()) {
            int custom = CustomDict::dictTypeId();
            origin(self, key, payload, &custom);
            g_injectedCustom = true;
            spdlog::debug("[CustomDict] injected custom section ({} bytes)", payload.size());
        }
    }
    origin(self, key, content, type);
    // 宿主每加一段都可能插到最前面，所以每次都在最后把自造词典那段置顶一次（宿主自己的置顶接口）
    if (g_injectedCustom) {
        int custom = CustomDict::dictTypeId();
        PEN_CALL(void, "_ZN14YResultManager6setTopERKN12YEnumWrapper8DictTypeE", void*, void*)(self, &custom);
    }
}

} // namespace mod
