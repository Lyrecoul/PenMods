// SPDX-License-Identifier: GPL-3.0-only
#include "filemanager/player/LyricClock.h"
#include <QCoreApplication>
#include <QThread>
#include <cstdlib>
#include <iostream>
using mod::filemanager::LyricClock;
static void check(bool ok, const char* why) {
    if (!ok) {
        std::cerr << why << '\n';
        std::exit(1);
    }
}
static void tick(unsigned long ms) {
    QThread::msleep(ms);
    QCoreApplication::processEvents();
}
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    LyricClock       clock;
    clock.setSourcePosition(1000);
    clock.setPlaying(true);
    clock.setRunning(true);
    tick(65);
    check(clock.position() >= 1040 && clock.position() < 1250, "interpolate between host samples");
    clock.setPlaying(false);
    const auto paused = clock.position();
    tick(65);
    check(clock.position() == paused, "pause freezes progress");
    clock.setSourcePosition(3000);
    check(clock.position() == 3000, "paused seek lands immediately");
    clock.setPlaybackRate(2);
    clock.setPlaying(true);
    tick(65);
    check(clock.position() >= 3080 && clock.position() < 3400, "rate applies to interpolation");
    clock.setSourcePosition(100);
    check(clock.position() < 150, "backwards seek discards old progress");
    clock.setRunning(false);
    const auto hidden = clock.position();
    tick(65);
    check(clock.position() == hidden, "hidden clock stops advancing");
    clock.setSourcePosition(8000);
    clock.setRunning(true);
    tick(1600);
    check(clock.position() == 11000, "stale host samples have a bounded extrapolation horizon");
    clock.setRunning(false);
    std::cout << "lyric clock checks passed\n";
}
