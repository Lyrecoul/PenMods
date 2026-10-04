// SPDX-License-Identifier: GPL-3.0-only
/*
 * Copyright (C) 2022-present, PenUniverse.
 * This file is part of the PenMods open source project.
 */

#pragma once

#include "common/service/Logger.h"

#include <QVariantList>

#include <map>
#include <memory>

namespace mod {

/**
 * @brief 用户自造离线词典的浏览与查询服务
 *
 * 容器格式、生成工具和逆向结论见 `doc/DICT_FORMAT_ANALYSIS.md` 与 `tools/dict-probe/`。
 * 查询本身交给厂商自己的读取器（`libDictManager.so` 导出的 `YQueryDictManager`），
 * 只使用它的 QString 接口：libPenMods.so 是 libc++ 构建的，而厂商类是 libstdc++，
 * 绝不能把我们的 std::string 递进去（`YQueryDictManager::open/lookUp` 正好只收 QString）。
 *
 * 这样可以查询任意自造容器，而不用在 Mod 里再实现一遍 XOR + zlib + 两级索引。
 */
class CustomDict : public QObject, public Singleton<CustomDict>, private Logger {
    Q_OBJECT

    /// 目录里找到的词典：{name, file, words, v2}
    Q_PROPERTY(QVariantList dictionaries READ dictionaries NOTIFY dictionariesChanged)

    /// 当前选中的词典（索引、名字、大小端无关的词头数）
    Q_PROPERTY(int index READ index WRITE setIndex NOTIFY selectionChanged)
    Q_PROPERTY(QString name READ name NOTIFY selectionChanged)
    Q_PROPERTY(QString path READ path NOTIFY selectionChanged)
    Q_PROPERTY(int wordCount READ wordCount NOTIFY selectionChanged)

    Q_PROPERTY(QString directory READ directory CONSTANT)

    /// 结果页里这个词典用的 DictType（宿主枚举里不存在的值，QML 用自己的渲染器接管）
    Q_PROPERTY(int dictTypeId READ dictTypeId CONSTANT)

    /// 查询词、命中的原始记录（排版交给 QML 的 YDictPenModsRender.js）和状态提示
    Q_PROPERTY(QString word READ word WRITE setWord NOTIFY wordChanged)
    Q_PROPERTY(QString record READ record NOTIFY recordChanged)
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)

public:
    /// 析构必须在这里只声明、在 .cpp 里定义：Vendor 是不完整类型，moc 生成的 TU 需要完整的析构
    ~CustomDict() override;

    Q_SIGNAL void dictionariesChanged();

    Q_SIGNAL void selectionChanged();

    Q_SIGNAL void wordChanged();

    Q_SIGNAL void recordChanged();

    Q_SIGNAL void statusChanged();

    void setIndex(int index);

    void setWord(const QString& word);

    /// 重新扫描词典目录（页面打开时调用）
    Q_INVOKABLE void refresh();

    /// 目录变了才重扫（查询路径上调用，用户丢进新词典后无需重启）
    void rescanIfChanged();

    /// 用当前 word 查询当前词典
    Q_INVOKABLE void lookup();

    /// 设置查询词并立即查询
    Q_INVOKABLE void lookupWord(const QString& word);


    /// 结果页挂钩用：在任意一本自造词典里查词，命中就返回给结果页渲染的包装 JSON（空 = 未命中）
    [[nodiscard]] QString payloadFor(const QString& word);

    [[nodiscard]] static int dictTypeId();

    [[nodiscard]] QVariantList dictionaries() const;

    [[nodiscard]] int index() const { return mIndex; }

    [[nodiscard]] QString name() const;

    [[nodiscard]] QString path() const;

    [[nodiscard]] int wordCount() const;

    [[nodiscard]] static QString directory();

    [[nodiscard]] QString word() const { return mWord; }

    [[nodiscard]] QString record() const { return mRecord; }

    [[nodiscard]] QString status() const { return mStatus; }

private:
    friend Singleton<CustomDict>;
    explicit CustomDict();

    /// 容器头（我们自己解析，只为了列表里能显示名字和词头数）
    struct Entry {
        QString file;     ///< 目录里的真实文件名
        QString openPath; ///< 交给厂商标记读取器的路径（V2 容器需要 `xxxV2.dat`）
        QString name;     ///< 容器里记录的显示名
        QString stamp;    ///< 大小 + mtime，文件被换掉时用来丢弃缓存的读取器
        int     words = 0;
        bool    v2    = false;
    };

    struct Vendor; ///< libDictManager 的 YQueryDictManager 包装，定义在 .cpp

    /// 读取器缓存项
    struct CachedReader {
        std::unique_ptr<Vendor> vendor;
        QString                 stamp; ///< 打开时的 Entry::stamp
        qint64                  lastUsed = 0;
    };

    /// 取（或打开）某本词典的读取器；失败返回 nullptr
    Vendor* readerFor(const Entry& entry);

    /// 在 word 命中的第一本词典里取原始记录（空 = 未命中）
    bool rawRecordFor(const QString& word, Entry const** entry, QString* record);

    /// 拆掉结果页返回的 "词\tJSON" 前缀
    static QString stripWordPrefix(const QString& raw);

    void setStatus(const QString& status);

    void setRecord(const QString& record);

    /// 确保当前选中的词典已经打开，失败返回 false 并写好 status
    bool ensureOpen();

    /// 扫描目录；内容变化返回 true，同时更新 mEntries / mScanStamp
    bool scanEntries();

    std::vector<Entry>              mEntries;
    std::map<QString, CachedReader> mReaders; ///< 按 openPath 缓存（主查询也要用）
    QString                         mScanStamp;
    int                             mIndex = -1;
    QString                         mWord;
    QString                         mRecord;
    QString                         mStatus;
    QString                         mLoaded;
};

} // namespace mod
