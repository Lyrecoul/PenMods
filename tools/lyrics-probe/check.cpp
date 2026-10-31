// SPDX-License-Identifier: GPL-3.0-only
// Host regression probe: compile with LyricTimeline.cpp and pkg-config Qt5Core flags.
#include "filemanager/player/LyricTimeline.h"
#include <QCoreApplication>
#include <cstdlib>
#include <iostream>
using namespace mod::filemanager;
static void check(bool ok, const char* message) {
    if (!ok) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const auto       lines = parseLyrics(
        QString::fromUtf8(
            "[offset:-100]\n[00:03]next\n[00:01.20][00:05.20]<00:01.20>A&<00:01.70>中😀<00:02.20>\n"
            "[00:01.200]译文\n[00:01.200]译文\n[00:01.200]translation two\n[00:06]\n[00:70]bad\n"
        )
    );
    check(lines.size() == 4, "sorted/grouped timestamps and empty instrumental cue");
    check(
        lines[0].time == 1100 && lines[0].translation == QString::fromUtf8("译文\ntranslation two"),
        "offset and duplicate translations"
    );
    check(lines[0].text == QString::fromUtf8("A&中😀"), "word tags removed without losing surrogate pairs");
    check(lyricLineAt(lines, 1099) == -1 && lyricLineAt(lines, 1100) == 0, "prelude and exact boundary");
    check(lyricLineAt(lines, 5900) == 3 && lyricLineAt(lines, 1500) == 0, "seek both directions");
    check(lines[2].cues[1].time == 5600, "repeated line shifts word times");
    check(lyricMarkup(lines[0], 1500, "blue", "white").contains("A&amp;</font>"), "first word and HTML escaping");
    check(
        lyricMarkup(lines[0], 1600, "blue", "white").contains(QString::fromUtf8("A&amp;中😀</font>")),
        "next word boundary"
    );
    check(parseLyrics("[00:02]a\n[00:01]b")[0].text == "b", "unordered file");
    check(parseLyrics("[ar:artist]\nplain text").isEmpty(), "metadata is not lyrics");
    check(parseLyrics("[00:01]<00:02>a<00:01>b")[0].cues.isEmpty(), "invalid word times fall back to sentence");
    check(
        lines[0].cues[0].until == 1600 && lines[0].cues[1].until == 2100,
        "word ends and trailing empty tag survive offset"
    );
    check(lines[2].cues[1].until == 6100, "repeated word end shifts with the line");
    check(
        parseLyrics(QString::fromUtf8("[00:01]<00:01>e<00:02>́"))[0].cues.isEmpty(),
        "time labels cannot divide a grapheme"
    );
    check(
        parseLyrics("[00:01]<00:01>unknown end")[0].cues[0].until == -1,
        "missing end is not given an invented duration"
    );
    std::cout << "lyric timeline checks passed\n";
}
