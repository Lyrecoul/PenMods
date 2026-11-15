#!/usr/bin/env python3
"""Run real input configuration and screen-wake paths in an isolated mount namespace.

Requires host Qt Core/Quick, spdlog, nlohmann_json, a C++17 compiler and bubblewrap.
No ADB, host /etc writes, input injection or system power commands are used.
"""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    qt = "Qt5" if subprocess.run(["pkg-config", "--exists", "Qt5Quick"]).returncode == 0 else "Qt6"
    moc = "moc-qt5" if qt == "Qt5" else str(Path(subprocess.check_output(
        ["pkg-config", "--variable=libexecdir", "Qt6Core"], text=True).strip()) / "moc")
    flags = shlex.split(subprocess.check_output(
        ["pkg-config", "--cflags", "--libs", qt + "Quick", "spdlog"], text=True))
    json_headers = Path("/usr/include/nlohmann/json.hpp")
    if not json_headers.exists():
        json_headers = next((Path.home() / ".xmake/packages/n/nlohmann_json").glob("*/*/include/nlohmann/json.hpp"))
    with tempfile.TemporaryDirectory(prefix="penmods-input-") as directory:
        work = Path(directory)
        prelude = work / "prelude.h"
        prelude.write_text(
            '#include <memory>\n#include <string>\n#include <QObject>\n#include <QString>\n'
            '#include <spdlog/spdlog.h>\n#include <spdlog/sinks/stdout_color_sinks.h>\n'
            '#include "base/StdInt.h"\n#include "common/service/Singleton.h"\n')
        mocs = []
        for header in ["system/input/InputDaemon.h", "system/input/ScreenManager.h", "common/Event.h",
                       "system/input/HeadsetControls.h", "system/battery/BatteryInfo.h",
                       "system/sound/AudioDaemon.h", "tweaker/ReadingLoop.h"]:
            target = work / ("moc_" + Path(header).stem + ".cpp")
            subprocess.run([moc, str(ROOT / "src" / header), "-o", str(target)], check=True)
            mocs.append(str(target))
        executable = work / "input-test"
        subprocess.run([*shlex.split(os.environ.get("CXX", "c++")), "-std=c++17", "-fPIC", "-pthread",
                        "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                        "-include", str(prelude), "-I", str(ROOT / "src"),
                        "-I", str(json_headers.parent.parent),
                        str(ROOT / "tools/input_daemon_runtime_test.cpp"), *mocs,
                        "-o", str(executable), *flags], check=True)
        for model in ("Exam_V0", "Dictpen2.0_V4"):
            print("Testing " + model, flush=True)
            subprocess.run(["bwrap", "--unshare-user", "--unshare-pid", "--die-with-parent",
                            "--ro-bind", "/", "/", "--proc", "/proc", "--tmpfs", "/tmp",
                            "--tmpfs", "/etc", "--ro-bind", str(work), "/tmp/fixture",
                            "/tmp/fixture/input-test", model], check=True, timeout=30)


if __name__ == "__main__":
    main()
