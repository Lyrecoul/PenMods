#!/usr/bin/env python3
"""Compile and run PP-OCR payload readiness fixtures against installed host Qt."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
qt_majors = [qt for qt in ("Qt5", "Qt6") if subprocess.run(
    ["pkg-config", "--exists", qt + "Core"], check=False).returncode == 0]
if not qt_majors:
    raise SystemExit("host Qt Core development package unavailable (pkg-config)")

with tempfile.TemporaryDirectory(prefix="penmods-ppocr-test-") as temporary:
    for qt in qt_majors:
        print("Testing " + qt, flush=True)
        output = Path(temporary) / ("ppocr-test-" + qt)
        flags = shlex.split(subprocess.check_output(
            ["pkg-config", "--cflags", "--libs", qt + "Core"], text=True))
        subprocess.run([*shlex.split(os.environ.get("CXX", "g++")), "-std=c++17", "-Wall", "-Wextra", "-Werror", "-fPIC",
                        "-I", str(root / "src"), str(root / "tools/ppocr_component_test.cpp"),
                        "-o", str(output), *flags], check=True)
        subprocess.run([str(output)], check=True, timeout=10)
