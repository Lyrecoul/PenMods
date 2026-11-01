// SPDX-License-Identifier: GPL-3.0-only
#include "MathServerRuntime.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QProcess>
#include <QTcpServer>
#include <signal.h>
#include <sys/prctl.h>
#include <unistd.h>

namespace mod::chatbot {
namespace {
QString canonical(const QString& path) { return QFileInfo(path).canonicalFilePath(); }

class OwnedMathProcess final : public QProcess {
public:
    explicit OwnedMathProcess(QObject* parent) : QProcess(parent) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        setChildProcessModifier([] { bindParent(); });
#endif
    }

private:
    static void bindParent() {
        const auto parent = getppid();
        if (prctl(PR_SET_PDEATHSIG, SIGKILL) != 0 || parent == 1 || getppid() != parent) _exit(127);
    }
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    void setupChildProcess() override { bindParent(); }
#endif
};
} // namespace

MathServerRuntime::MathServerRuntime(QString managedPath, Save save, QObject* parent, quint16 port)
: QObject(parent),
  mManagedPath(std::move(managedPath)),
  mSave(std::move(save)),
  mPort(port) {
    QDir root(QFileInfo(mManagedPath).absolutePath());
    root.cdUp();
    root.cdUp();
    mStoreRoot = root.absolutePath();
    mTransition.setInterval(50);
    connect(&mTransition, &QTimer::timeout, this, [this] { drive(); });
    if (QCoreApplication::instance())
        connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, this, [this] { stop(); });
}

MathServerRuntime::~MathServerRuntime() {
    mWanted = false;
    mTransition.stop();
    if (mProcess) {
        disconnect(mProcess, nullptr, this, nullptr);
        mProcess->kill();
        delete mProcess.data();
    }
}

bool MathServerRuntime::installed() const {
    const QFileInfo file(mManagedPath);
    return file.isFile() && file.isExecutable();
}
bool    MathServerRuntime::enabled() const { return mConfig.value("enabled").toBool(false); }
QString MathServerRuntime::path() const { return installed() ? mManagedPath : QString(); }
bool   MathServerRuntime::running() const { return mProcess && mProcess->state() == QProcess::Running && !mStopping; }
qint64 MathServerRuntime::processId() const { return mProcess ? mProcess->processId() : 0; }

void MathServerRuntime::state(const QString& text) {
    if (mStatus == text) return;
    mStatus = text;
    if (changed) changed();
}
void MathServerRuntime::error(const QString& text) {
    mWanted = false;
    if (!mProcess) mTransition.stop();
    state(text);
    if (failed) failed(text);
}

bool MathServerRuntime::recognized(const QString& path) {
    if (QFileInfo(path).fileName() != "mathjax-server") return false;
    QFile file(path);
    return file.open(QIODevice::ReadOnly) && file.read(4) == QByteArray("\177ELF", 4);
}

void MathServerRuntime::load(const QJsonObject& config) {
    const QString previous = path();
    mConfig                = config;
    mConfig.remove("server_path");
    mConfig.remove("server_source");
    if (mConfig != config && mSave && !mSave(mConfig))
        state("旧公式服务路径已停用，但配置清理未能保存");
    if (!previous.isEmpty() && canonical(previous) != canonical(path()) && recognized(previous))
        mRetiredPaths.append(canonical(previous));
    mRetiredPaths.removeDuplicates();
    if (canonical(previous) != canonical(path())) mRetirementStarted = false;
    if (selectionChanged) selectionChanged();
    if (!enabled()) stop();
    else if (mWanted || mProcess) start();
}

bool MathServerRuntime::commit(const QJsonObject& config) {
    if (config == mConfig) return true;
    if (!mSave || !mSave(config)) {
        state("无法保存公式服务设置，原选择保持不变");
        if (failed) failed(mStatus);
        return false;
    }
    load(config);
    return true;
}

bool MathServerRuntime::update(const QJsonObject& patch) {
    auto next = mConfig;
    if (patch.contains("enabled")) {
        if (!patch.value("enabled").isBool()) return false;
        next.insert("enabled", patch.value("enabled"));
    }
    return commit(next);
}

bool MathServerRuntime::setEnabled(bool value) {
    return update({
        {"enabled", value}
    });
}

MathServerRuntime::Identity MathServerRuntime::identity(qint64 pid) {
    Identity result;
    if (pid <= 1 || pid == getpid()) return result;
    QFile file(QString("/proc/%1/stat").arg(pid));
    if (!file.open(QIODevice::ReadOnly)) return result;
    const auto data   = file.readAll();
    const auto fields = data.mid(data.lastIndexOf(')') + 2).split(' ');
    if (fields.size() <= 19 || fields[0] == "Z" || fields[0] == "X") return result;
    const QString executable = QFileInfo(QString("/proc/%1/exe").arg(pid)).symLinkTarget();
    if (executable.isEmpty()) return result;
    result.pid        = pid;
    result.startTime  = fields[19];
    result.executable = executable;
    return result;
}
bool MathServerRuntime::sameProcess(const Identity& expected) {
    const auto actual = identity(expected.pid);
    return actual.pid && actual.startTime == expected.startTime && actual.executable == expected.executable;
}

QList<MathServerRuntime::Identity> MathServerRuntime::oldServers() const {
    QStringList exact = mRetiredPaths;
    if (recognized(path())) exact.append(canonical(path()));
    QString objects = canonical(mStoreRoot + "/objects");
    if (!objects.isEmpty()) objects += '/';
    QList<Identity> result;
    for (const auto& entry : QDir("/proc").entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        bool       ok  = false;
        const auto pid = entry.toLongLong(&ok);
        if (!ok || (mProcess && mProcess->processId() == pid)) continue;
        const auto process = identity(pid);
        if (!process.pid) continue;
        QString executable = process.executable;
        if (executable.endsWith(" (deleted)")) executable.chop(10);
        bool match = exact.contains(executable);
        if (!objects.isEmpty() && executable.startsWith(objects)) {
            const auto suffix  = executable.mid(objects.size()).split('/');
            match             |= suffix.size() == 2 && suffix[1] == "mathjax-server";
        }
        // The name alone is never enough: match an exact selected canonical path or
        // one immutable managed object, then verify ELF via the live proc executable.
        if (match && QFileInfo(executable).fileName() == "mathjax-server") {
            QFile image(QString("/proc/%1/exe").arg(pid));
            if (image.open(QIODevice::ReadOnly) && image.read(4) == QByteArray("\177ELF", 4)) result.append(process);
        }
    }
    return result;
}

bool MathServerRuntime::start() {
    const QString selected = canonical(path());
    if (!enabled() || selected.isEmpty() || !QFileInfo(selected).isExecutable() || !QFileInfo(selected).isFile()) {
        mWanted = false;
        drive();
        if (enabled()) error("所选公式服务不存在或不可执行");
        return false;
    }
    mWanted = true;
    if (mProcess && !mStopping && mRunningPath == selected) return true;
    drive();
    return mWanted;
}

void MathServerRuntime::stop() {
    mWanted = false;
    drive();
}

void MathServerRuntime::drive() {
    const QString selected = enabled() && mWanted ? canonical(path()) : QString();
    if (mProcess) {
        if (!mStopping && !selected.isEmpty() && mRunningPath == selected) return;
        if (!mStopping) {
            mStopping = true;
            mStoppingSince.start();
            mProcess->terminate();
            state("正在切换公式服务");
        } else if (mStoppingSince.elapsed() >= 1500) mProcess->kill();
        mTransition.start();
        return;
    }
    if (selected.isEmpty()) {
        mTransition.stop();
        state("公式服务已停止");
        return;
    }
    if (!mRetirementStarted) {
        mRetiring          = oldServers();
        mRetirementStarted = true;
        mStoppingSince.start();
        for (const auto& process : mRetiring)
            if (sameProcess(process)) ::kill(pid_t(process.pid), SIGTERM);
    }
    bool remaining = false;
    for (const auto& process : mRetiring)
        if (sameProcess(process)) {
            remaining = true;
            if (mStoppingSince.elapsed() >= 1500) ::kill(pid_t(process.pid), SIGKILL);
        }
    if (remaining) {
        if (mStoppingSince.elapsed() > 3000) {
            error("旧公式服务无法退出，请稍后重试");
            return;
        }
        state("正在关闭旧公式服务");
        mTransition.start();
        return;
    }
    mTransition.stop();
    mRetirementStarted = false;
    mRetiring.clear();
    mRetiredPaths.clear();
    QTcpServer portProbe;
    if (!portProbe.listen(QHostAddress::Any, mPort)) {
        error("公式服务端口已被其他程序占用；未终止无关进程");
        return;
    }
    portProbe.close();
    auto* process = new OwnedMathProcess(this);
    mProcess      = process;
    mRunningPath  = selected;
    mStopping     = false;
    process->setProcessChannelMode(QProcess::ForwardedChannels);
    process->setWorkingDirectory(QFileInfo(selected).absolutePath());
    connect(process, &QProcess::started, this, [this, process] {
        if (mProcess == process && !mStopping) state("公式服务进程已启动，等待健康检查");
    });
    connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError reason) {
        if (mProcess != process || mStopping) return;
        if (reason == QProcess::FailedToStart) {
            mProcess.clear();
            process->deleteLater();
            error("公式服务启动失败：" + process->errorString());
        }
    });
    connect(
        process,
        QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
        this,
        [this, process](int, QProcess::ExitStatus) {
            if (mProcess != process) return;
            const bool switching = mStopping;
            mProcess.clear();
            mStopping = false;
            process->deleteLater();
            if (switching) QTimer::singleShot(0, this, [this] { drive(); });
            else error("公式服务已退出，请检查程序或端口后重试");
        }
    );
    state("正在启动公式服务");
    process->start(selected, QStringList{});
}

} // namespace mod::chatbot
