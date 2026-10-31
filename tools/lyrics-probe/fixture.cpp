// SPDX-License-Identifier: GPL-3.0-only
// Validate that our generated continuous-render demo actually supplies continuous timing for every segment.
#include "filemanager/player/LyricTimeline.h"
#include <QCoreApplication>
#include <QFile>
#include <iostream>
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (argc != 2) return 2;
    QFile file(QString::fromLocal8Bit(argv[1]));
    if (!file.open(QIODevice::ReadOnly)) return 2;
    const auto lines = mod::filemanager::parseLyrics(QString::fromUtf8(file.readAll()));
    if (lines.isEmpty()) return 2;
    for (const auto& line : lines) {
        if (line.cues.isEmpty()) return 2;
        for (const auto& cue : line.cues) {
            if (cue.until <= cue.time) {
                std::cerr << "FAIL: demo segment starting at " << cue.time
                          << " ms has no end; it will flash fully highlighted\n";
                return 1;
            }
        }
    }
    std::cout << "PASS: every demo segment has an interval for continuous highlighting\n";
}
