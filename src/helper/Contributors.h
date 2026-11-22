// SPDX-License-Identifier: GPL-3.0-only
/*
 * Copyright (C) 2022-present, PenUniverse.
 * This file is part of the PenMods open source project.
 */

#pragma once

#include "common/service/Logger.h"
#include "common/service/Singleton.h"

#include <QObject>
#include <QPointer>
#include <QVariantList>

class QNetworkAccessManager;
class QNetworkReply;

namespace mod {

/*
 * GitHub 贡献者列表（设置 → 关于 → 关于 PenMods → 特别鸣谢 →「在线获取所有贡献者」）。
 *
 * 只有用户点击才发请求（懒加载，不占启动时间）：拉取 Lyrecoul/PenMods 的 contributors，按 contributions
 * 降序交给 QML。请求全程异步，busy 期间重复点击直接忽略；超时、非 200、JSON 解析失败、空数组以及翻页
 * 异常都走 failed()，不会把半个列表当成成功结果显示出去。
 *
 * 两个容易踩的点：
 *   1. GitHub REST API 强制要求 User-Agent，缺省直接 403；Accept 固定 application/vnd.github+json。
 *   2. 证书链依赖 helper/CaCertificates 在 beforeUiInitialization 装好的默认信任锚，这里不自己管 TLS。
 *
 * 翻页用 page 参数（而不是解析 Link: rel="next"），最多 kMaxPages 页，上限防止接口/代理返回异常时
 * 无限请求。contributors 单项为 { login, avatarUrl, contributions }。
 */
class Contributors : public QObject, public Singleton<Contributors>, private Logger {
    Q_OBJECT

    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)
    Q_PROPERTY(QVariantList contributors READ contributors NOTIFY changed)

public:
    /// 是否有请求在途（QML 据此显示加载态并禁用按钮）
    [[nodiscard]] bool busy() const { return mBusy; }

    /// 最近一次失败的内部原因，只用于日志/排查；UI 统一提示 fixed 文案（见 failed()）
    [[nodiscard]] QString error() const { return mError; }

    /// 贡献者列表，每项 { login, avatarUrl, contributions }，按 contributions 降序
    [[nodiscard]] QVariantList contributors() const { return mContributors; }

    /// 发起 https://api.github.com/repos/Lyrecoul/PenMods/contributors 的拉取
    Q_INVOKABLE void fetch();

signals:

    /// busy / error / contributors 任一变化时发出（属性变更通知统一用它）
    void changed();

    /// 获取失败，参数是给用户看的固定文案
    void failed(QString message);

private:
    friend Singleton<Contributors>;
    explicit Contributors();

    void _requestPage(int page);
    void _handlePage(QNetworkReply* reply, int page);
    void _finish();
    void _fail(const QString& reason);

    static constexpr int kPerPage   = 100;
    static constexpr int kMaxPages  = 3;
    static constexpr int kTimeoutMs = 15000;

    QNetworkAccessManager*  mNetwork = nullptr;
    QPointer<QNetworkReply> mReply;
    QVariantList            mContributors;
    QString                 mError;
    bool                    mBusy = false;
};

} // namespace mod
