// SPDX-License-Identifier: GPL-3.0-only
#include "filemanager/player/LyricTimeline.h"
#include <QCoreApplication>
#include <QDebug>
#include <QFile>
#include <cstdio>
#include <cstdlib>
using namespace mod::filemanager;
void check(bool condition, const char* message) {
    if (!condition) {
        qCritical() << message;
        std::exit(1);
    }
}
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const auto       yrc = parseLyrics("[1000,2000](1000,500,0)Hello (1800,1200,0)world");
    check(yrc.size() == 1 && yrc[0].text == "Hello world" && yrc[0].cues.size() == 2, "YRC text/words");
    check(
        yrc[0].cues[0].until == 1500 && yrc[0].cues[1].time == 1800 && yrc[0].cues[1].until == 3000,
        "YRC gaps/duration"
    );
    const auto saved = parseLyrics(serializeLyrics(yrc));
    check(
        saved[0].cues.size() == 2 && saved[0].cues[0].until == 1500 && saved[0].cues[1].until == 3000,
        "Enhanced LRC round trip with silence"
    );
    const auto qrc = parseLyrics("[1000,2000]Hello (1000,500)world(1800,1200)");
    check(
        qrc.size() == 1 && qrc[0].text == yrc[0].text && qrc[0].cues.size() == 2 && qrc[0].cues[1].until == 3000,
        "Plain QRC"
    );
    const auto wrapped = parseLyrics(
        R"xml(<QrcInfos><LyricInfo><Lyric_1 LyricContent="[1000,2000]Hello (1000,500)world(1800,1200)"/></LyricInfo></QrcInfos>)xml"
    );
    check(wrapped.size() == 1 && wrapped[0].cues.size() == 2, "XML-wrapped plain QRC");
    const auto bad = parseLyrics("[1000,2000](1800,1200,0)Hello (1000,500,0)world");
    check(
        bad.size() == 1 && bad[0].text == "Hello world" && bad[0].cues.isEmpty(),
        "Invalid timing degrades to sentence"
    );
    const QString xml = QString::fromUtf8(
        R"(<tt xmlns="http://www.w3.org/ns/ttml" xmlns:ttm="http://www.w3.org/ns/ttml#metadata"><body><div><p begin="00:01.000" end="00:03.000"><span begin="1s" end="1.5s">Hello </span><span begin="1.8s" end="3s">world</span><span ttm:role="x-translation">你好世界</span></p></div></body></tt>)"
    );
    const auto ttml = parseLyrics(xml);
    check(
        ttml.size() == 1 && ttml[0].text == "Hello world" && ttml[0].translation == QString::fromUtf8("你好世界")
            && ttml[0].cues.size() == 2 && ttml[0].cues.last().until == 3000,
        "TTML words/translation"
    );
    check(parseLyrics(xml.left(xml.size() - 5)).isEmpty(), "Malformed XML rejected");
    check(
        parseLyrics("<!DOCTYPE tt [<!ENTITY test SYSTEM 'file:///etc/passwd'>]><tt>&test;</tt>").isEmpty(),
        "DTD/entities rejected"
    );
    QString deep;
    for (int i = 0; i < 34; ++i) deep += "<div>";
    for (int i = 0; i < 34; ++i) deep += "</div>";
    check(parseLyrics(deep).isEmpty(), "Depth bounded");
    const auto online = lyricsFromOnlineResponse(
        R"({"code":200,"yrc":{"lyric":"[1000,2000](1000,500,0)Hello (1800,1200,0)world"},"lrc":{"lyric":"[00:01]Hello world"},"tlyric":{"lyric":"[00:01.1]Translation"}})"
    );
    check(
        online.size() == 1 && online[0].cues.size() == 2 && online[0].translation == "Translation",
        "Online prefers words and aligns translation"
    );
    const auto fallback =
        lyricsFromOnlineResponse(R"({"code":200,"yrc":{"lyric":"unavailable"},"lrc":{"lyric":"[00:01]Hello"}})");
    check(fallback.size() == 1 && fallback[0].text == "Hello", "Online LRC fallback");
    check(lyricsFromOnlineResponse("{bad json").isEmpty(), "Invalid API JSON rejected");
    if (argc > 1) {
        QFile file(QString::fromLocal8Bit(argv[1]));
        check(file.open(QIODevice::ReadOnly), "Open API sample");
        const auto rows = lyricsFromOnlineResponse(file.readAll());
        check(!rows.isEmpty(), "Live API provides parsed rows");
        int words = 0;
        for (const auto& row : rows) words += row.cues.size();
        const auto roundtrip = parseLyrics(serializeLyrics(rows));
        int        restored  = 0;
        for (const auto& row : roundtrip) restored += row.cues.size();
        check(
            words > 0 && words == restored && rows.size() == roundtrip.size(),
            "Live API word timings survive persistence"
        );
        for (int i = 0; i < rows.size(); ++i) {
            check(
                rows[i].time == roundtrip[i].time && rows[i].text == roundtrip[i].text
                    && rows[i].translation == roundtrip[i].translation
                    && rows[i].cues.size() == roundtrip[i].cues.size(),
                "Live API line content survives persistence"
            );
            for (int j = 0; j < rows[i].cues.size(); ++j) {
                const auto a = rows[i].cues[j], b = roundtrip[i].cues[j];
                check(
                    a.time == b.time && a.until == b.until && a.end == b.end,
                    "Live API exact word intervals survive persistence"
                );
            }
        }
        std::printf("Live API rows=%d words=%d; round trip preserved\n", rows.size(), words);
    }
    std::puts("FORMAT CHECK PASS");
}
