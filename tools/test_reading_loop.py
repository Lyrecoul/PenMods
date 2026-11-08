#!/usr/bin/env python3
"""Compile/run real ReadingLoop + source QML on host Qt 6, without a device."""
import argparse
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def run(*args, **kwargs):
    subprocess.run([str(arg) for arg in args], check=True, **kwargs)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--resources", action="store_true",
                        help="always test QML extracted from the generated resource header")
    args = parser.parse_args()
    flags = shlex.split(subprocess.check_output(
        ["pkg-config", "--cflags", "--libs", "Qt6Quick", "Qt6Qml", "Qt6Test"], text=True
    ))
    libexec = subprocess.check_output(["pkg-config", "--variable=libexecdir", "Qt6Core"], text=True).strip()
    moc = Path(libexec) / "moc"
    if not moc.is_file():
        moc = Path("/usr/lib/qt6/moc")
    with tempfile.TemporaryDirectory(prefix="penmods-reading-loop-") as directory:
        work = Path(directory)
        qml = ROOT / "resource/models/YDP02X/qml"
        if args.resources or not (qml / "dicts/YReadingLoopControl.qml").is_file():
            resources = work / "resources"
            run("sh", ROOT / "scripts/unpack_qt_res.sh", "YDP02X", "-o", resources)
            qml = resources / "qml"
        source = work / "reading_loop_runtime_test.cpp"
        shutil.copyfile(ROOT / "tools/reading_loop_runtime_test.cpp", source)
        for original, target in [
            (ROOT / "src/tweaker/ReadingLoop.h", work / "moc_ReadingLoop.cpp"),
            (ROOT / "src/common/Event.h", work / "moc_Event.cpp"),
            (source, work / "reading_loop_runtime_test.moc"),
        ]:
            run(moc, original, "-o", target)
        prelude = work / "prelude.h"
        prelude.write_text(
            "#include <memory>\n#include <QObject>\n#include <QString>\n"
            '#include "common/service/Singleton.h"\n'
        )
        executable = work / "reading-loop-test"
        run("c++", "-std=c++17", "-fPIC", "-pthread", "-include", prelude,
            "-I", ROOT / "src", "-I", work, source,
            work / "moc_ReadingLoop.cpp", work / "moc_Event.cpp", *flags, "-o", executable)
        environment = os.environ | {"QT_QPA_PLATFORM": "offscreen", "QT_QUICK_BACKEND": "software"}
        run(executable, qml, env=environment)


if __name__ == "__main__":
    main()
