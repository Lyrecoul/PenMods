#!/usr/bin/env python3
"""Build/run real MathJax lifecycle fixtures without xmake or ADB.

Needs a host C++17 compiler, pkg-config and Qt Core+Network development files.
Default auto prefers Qt5 (the device API); --qt all verifies both installed majors.
Examples: python3 -B tools/test_math_server_runtime.py --qt all
          python3 -B tools/test_math_server_runtime.py --qt Qt5
"""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--qt", choices=("auto", "all", "Qt5", "Qt6"), default="auto")
args = parser.parse_args()
installed = [qt for qt in ("Qt5", "Qt6") if subprocess.run(
    ["pkg-config", "--exists", qt + "Core", qt + "Network"], check=False).returncode == 0]
if args.qt in ("auto", "all"):
    selected = installed[:1] if args.qt == "auto" else installed
else:
    selected = [args.qt] if args.qt in installed else []
if not selected:
    parser.error("requested host Qt Core+Network development packages unavailable (pkg-config)")

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="penmods-math-runtime-") as temporary:
    for qt in selected:
        print("Testing " + qt, flush=True)
        output = Path(temporary) / ("runtime-test-" + qt)
        flags = shlex.split(subprocess.check_output(
            ["pkg-config", "--cflags", "--libs", qt + "Core", qt + "Network"], text=True))
        subprocess.run([*shlex.split(os.environ.get("CXX", "g++")), "-std=c++17", "-Wall", "-Wextra", "-Werror", "-fPIC",
                        "-I", str(root / "src"), str(root / "src/chatbot/MathServerRuntime.cpp"),
                        str(root / "tools/math_server_runtime_test.cpp"), "-o", str(output), *flags], check=True)
        subprocess.run([str(output)], check=True, timeout=30)
