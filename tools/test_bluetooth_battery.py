#!/usr/bin/env python3
"""Compile the real battery class with host Qt5 and exercise it on a private D-Bus.

Requires g++, moc-qt5 (or moc), pkg-config, Qt5Core/Qt5DBus and dbus-run-session.
No Bluetooth hardware, production daemon, project build or generated QML is used.
"""
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    moc = shutil.which("moc-qt5") or shutil.which("moc")
    if not moc:
        raise SystemExit("Qt5 moc is required")
    flags = shlex.split(subprocess.check_output(
        ["pkg-config", "--cflags", "--libs", "Qt5Core", "Qt5DBus"], text=True
    ))
    with tempfile.TemporaryDirectory(prefix="penmods-bluetooth-battery-") as temporary:
        work = Path(temporary)
        generated = work / "moc_BluetoothBattery.cpp"
        executable = work / "bluetooth-battery-test"
        subprocess.run([moc, str(ROOT / "src/helper/BluetoothBattery.h"), "-o", str(generated)], check=True)
        hfp_moc = work / "moc_HfpBattery.cpp"
        subprocess.run([moc, str(ROOT / "src/helper/HfpBattery.h"), "-o", str(hfp_moc)], check=True)
        subprocess.run([
            "g++", "-std=c++17", "-fPIC", "-Wall", "-Wextra", "-I" + str(ROOT / "src"),
            str(ROOT / "src/helper/BluetoothBattery.cpp"), str(generated),
            str(ROOT / "src/helper/HfpBattery.cpp"), str(hfp_moc),
            str(ROOT / "tools/bluetooth_battery_test.cpp"), "-o", str(executable), *flags,
        ], check=True)
        subprocess.run(["dbus-run-session", "--", str(executable)], check=True)


if __name__ == "__main__":
    main()
