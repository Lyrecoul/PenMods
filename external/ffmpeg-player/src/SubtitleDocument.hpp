#pragma once

#include <QByteArray>
#include <QFile>
#include <QFileInfo>
#include <QMetaObject>
#include <QObject>
#include <QRegularExpression>
#include <QStringList>
#include <QTextCodec>
#include <QUrl>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace ffmpeg_player::subtitles {

constexpr int MaxInputBytes = 4 * 1024 * 1024;
struct Document { QByteArray ass; };
using DocumentPtr = std::shared_ptr<const Document>;
struct LoadResult { DocumentPtr document; QString error; };

inline QString defaultFont() {
    const QStringList paths{
        qEnvironmentVariable("FFPLAYER_SUBTITLE_FONT"),
        "/usr/lib/fonts/NotoSansSC-Regular.otf",
        "/usr/lib/fonts/msyh.ttc",
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"
    };
    for (const auto& path : paths)
        if (!path.isEmpty() && QFileInfo(path).isFile() && QFileInfo(path).isReadable())
            return path;
    return {};
}

inline QString assTime(qint64 ms) {
    const qint64 cs = std::max<qint64>(0, ms) / 10;
    return QString("%1:%2:%3.%4").arg(cs / 360000)
        .arg((cs / 6000) % 60, 2, 10, QChar('0'))
        .arg((cs / 100) % 60, 2, 10, QChar('0'))
        .arg(cs % 100, 2, 10, QChar('0'));
}

inline QString escapeText(QString text) {
    // WORD JOINER 阻止用户的 \N 等字样成为 ASS 指令，但不改变可见文本。
    text.replace("\\", QString("\\") + QChar(0x2060));
    text.replace("{", "\\{");
    text.replace("}", "\\}");
    text.replace('\n', "\\N");
    return text;
}

inline LoadResult parse(QString text, QString format) {
    if (text.contains(QChar(0))) return {{}, "Subtitle contains binary data"};
    if (text.toUtf8().size() > MaxInputBytes)
        return {{}, "Subtitle exceeds 4 MiB"};
    if (text.startsWith(QChar(0xfeff))) text.remove(0, 1);
    text.replace("\r\n", "\n");
    text.replace('\r', '\n');
    format = format.toLower();
    if (format == "ass" || format == "ssa") {
        if (!text.contains("[Events]", Qt::CaseInsensitive))
            return {{}, "Invalid ASS subtitle: missing [Events]"};
        return {std::make_shared<Document>(Document{text.toUtf8()}), {}};
    }
    if (format != "lrc") return {{}, "Subtitle format must be ASS or LRC"};

    struct Cue { qint64 time; QString text; };
    QList<Cue> cues;
    const QRegularExpression timestamp(R"(\[(\d{1,4}):([0-5]\d)(?:[.:](\d{1,3}))?\])");
    const QRegularExpression offsetTag(R"(\[offset:\s*([+-]?\d+)\s*\])", QRegularExpression::CaseInsensitiveOption);
    const QRegularExpression wordTag(R"(<\d{1,4}:[0-5]\d(?:[.:]\d{1,3})?>)");
    qint64 offset = 0;
    qint64 expandedBytes = 0;
    auto offsets = offsetTag.globalMatch(text);
    while (offsets.hasNext()) {
        bool ok = false;
        offset = offsets.next().captured(1).toLongLong(&ok);
        if (!ok || offset < -86400000 || offset > 86400000)
            return {{}, "LRC offset is out of range"};
    }
    for (const QString& line : text.split('\n')) {
        QList<qint64> times;
        int cursor = 0;
        while (cursor < line.size()) {
            while (cursor < line.size() && line[cursor].isSpace()) ++cursor;
            const auto match = timestamp.match(line, cursor);
            if (!match.hasMatch() || match.capturedStart() != cursor) break;
            if (times.size() + cues.size() >= 10000) return {{}, "Too many LRC timestamps"};
            const qint64 minutes = match.captured(1).toLongLong();
            if (minutes > 1440) return {{}, "LRC timestamp is out of range"};
            const QString fraction = match.captured(3).leftJustified(3, '0');
            times.append(minutes * 60000 + match.captured(2).toInt() * 1000 + fraction.toInt() - offset);
            cursor = match.capturedEnd();
        }
        QString lyric = line.mid(cursor);
        lyric.remove(wordTag); // 基础 LRC 以整行为单位，不显示增强 LRC 的逐字时间标签。
        if (times.isEmpty()) continue;
        const QString escaped = escapeText(lyric);
        expandedBytes += (qint64(escaped.toUtf8().size()) + 128) * times.size();
        if (expandedBytes > 2 * MaxInputBytes)
            return {{}, "Expanded LRC exceeds 8 MiB"};
        for (qint64 time : times) {
            if (cues.size() >= 10000) return {{}, "Too many LRC timestamps"};
            cues.append({time, escaped});
        }
    }
    if (cues.isEmpty()) return {{}, "No timestamps found in LRC subtitle"};
    std::stable_sort(cues.begin(), cues.end(), [](const Cue& a, const Cue& b) { return a.time < b.time; });
    QString ass = "[Script Info]\nScriptType: v4.00+\nPlayResX: 1280\nPlayResY: 720\nWrapStyle: 0\n"
        "[V4+ Styles]\nFormat: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding\n"
        "Style: Default,sans-serif,64,&H00FFFFFF,&H00FFFFFF,&H00000000,&H80000000,0,0,0,0,100,100,0,0,1,2,1,2,40,40,30,1\n"
        "[Events]\nFormat: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n";
    for (int i = 0; i < cues.size();) {
        const qint64 begin = cues[i].time;
        QStringList lines;
        do {
            if (!cues[i].text.isEmpty()) lines.append(cues[i].text);
            ++i;
        } while (i < cues.size() && cues[i].time == begin);
        // 空行也是时间边界；最后一行保留到媒体结束（最长额外 24 小时）。
        const qint64 end = i < cues.size() ? cues[i].time : begin + 86400000;
        if (end <= 0 || lines.isEmpty()) continue;
        ass += QString("Dialogue: 0,%1,%2,Default,,0,0,0,,%3\n")
            .arg(assTime(begin), assTime(end), lines.join("\\N"));
    }
    return {std::make_shared<Document>(Document{ass.toUtf8()}), {}};
}

inline LoadResult decode(const QByteArray& bytes, const QString& format) {
    if (bytes.size() > MaxInputBytes) return {{}, "Subtitle exceeds 4 MiB"};
    auto* codec = QTextCodec::codecForUtfText(bytes, QTextCodec::codecForName("UTF-8"));
    QTextCodec::ConverterState state;
    QString text = codec->toUnicode(bytes.constData(), bytes.size(), &state);
    if (state.invalidChars || state.remainingChars) {
        auto* gb = QTextCodec::codecForName("GB18030");
        if (!gb) return {{}, "Invalid subtitle text encoding"};
        QTextCodec::ConverterState fallback;
        text = gb->toUnicode(bytes.constData(), bytes.size(), &fallback);
        if (fallback.invalidChars || fallback.remainingChars) return {{}, "Invalid subtitle text encoding"};
    }
    if (text.contains(QChar(0))) return {{}, "Subtitle contains binary data"};
    return parse(text, format);
}

// 只处理本地/qrc 字幕。网络资源由宿主下载后通过 setSubtitleText() 提交。
class Loader {
public:
    ~Loader() { stop(); }
    void request(QObject* receiver, QUrl source, std::function<void(LoadResult)> callback) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) return;
        pending_ = Request{++generation_, receiver, std::move(source), std::move(callback), {}, {}};
        if (!worker_.joinable()) worker_ = std::thread([this] { run(); });
        changed_.notify_one();
    }
    void requestText(QObject* receiver, QString text, QString format, std::function<void(LoadResult)> callback) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) return;
        pending_ = Request{++generation_, receiver, {}, std::move(callback), std::move(text), std::move(format)};
        if (!worker_.joinable()) worker_ = std::thread([this] { run(); });
        changed_.notify_one();
    }
    void cancel() {
        std::lock_guard<std::mutex> lock(mutex_);
        ++generation_;
        pending_.reset();
    }
    void stop() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
            ++generation_;
            pending_.reset();
        }
        changed_.notify_one();
        if (worker_.joinable()) worker_.join();
    }
private:
    struct Request {
        quint64 id;
        QObject* receiver;
        QUrl source;
        std::function<void(LoadResult)> callback;
        QString text, format;
    };
    static LoadResult read(const QUrl& source) {
        QString path;
        if (source.isLocalFile()) path = source.toLocalFile();
        else if (source.scheme().isEmpty()) path = source.path();
        else if (source.scheme() == "qrc") path = ':' + source.path();
        else return {{}, "Use a local/qrc subtitle URL, or setSubtitleText() for downloaded text"};
        QFile file(path);
        if (path.startsWith(':')) {
            if (!file.open(QIODevice::ReadOnly)) return {{}, "Cannot open subtitle resource"};
        } else {
            const int fd = ::open(QFile::encodeName(path).constData(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
            if (fd < 0) return {{}, "Cannot open subtitle file"};
            struct stat st{};
            if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
                ::close(fd);
                return {{}, "Subtitle must be a regular file"};
            }
            if (st.st_size > MaxInputBytes) { ::close(fd); return {{}, "Subtitle exceeds 4 MiB"}; }
            if (!file.open(fd, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle)) {
                ::close(fd);
                return {{}, "Cannot read subtitle file"};
            }
        }
        const QByteArray bytes = file.read(MaxInputBytes + 1);
        if (file.error() != QFileDevice::NoError) return {{}, "Subtitle read failed"};
        return decode(bytes, QFileInfo(path).suffix());
    }
    void run() {
        for (;;) {
            Request job;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                changed_.wait(lock, [this] { return stopping_ || pending_.has_value(); });
                if (stopping_) return;
                job = std::move(*pending_);
                pending_.reset();
            }
            auto result = job.format.isEmpty() ? read(job.source) : parse(job.text, job.format);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (stopping_ || job.id != generation_) continue;
            }
            QMetaObject::invokeMethod(job.receiver, [callback = job.callback, result = std::move(result)]() mutable {
                callback(std::move(result));
            }, Qt::QueuedConnection);
        }
    }
    std::mutex mutex_;
    std::condition_variable changed_;
    bool stopping_ = false;
    quint64 generation_ = 0;
    std::optional<Request> pending_;
    std::thread worker_;
};
}
