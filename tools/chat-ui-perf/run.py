#!/usr/bin/env python3
"""Device regression replay: python3 tools/chat-ui-perf/run.py.

Uses qmlscene alongside the app; makes no API calls or session changes.
Budget is for this RK3326/Qt 5.15 fixture, not a screen frame-rate claim.
The original duplicated-layout implementation measured 656 ms worst / 326 ms mean.
"""
import pathlib
import re
import subprocess

root = pathlib.Path(__file__).resolve().parents[2]
remote = "/tmp/penmods-chat-perf"


def adb(*args):
    return subprocess.run(["adb", *args], check=True, capture_output=True, text=True).stdout


try:
    adb("shell", f"mkdir -p {remote}/messages")
    adb("push", str(root / "resource/models/YDP02X/qml/assistant/messages") + "/.", remote + "/messages/")
    adb("push", str(pathlib.Path(__file__).with_name("StreamReplay.qml")), remote + "/")
    output = subprocess.run([
        "adb", "shell", "XDG_RUNTIME_DIR=/var/run WAYLAND_DISPLAY=wayland-0 "
        "QT_WAYLAND_SHELL_INTEGRATION=xdg-shell-v6 "
        "QT_LOGGING_RULES='*.debug=false;qml.debug=true' "
        f"timeout 45 qmlscene {remote}/StreamReplay.qml"
    ], capture_output=True, text=True, timeout=50)
    log = output.stdout + output.stderr
    print(log.strip())
    match = re.search(r"PERF chars=11880 worst_ms=([\d.]+) mean_ms=([\d.]+)", log)
    assert match, "Replay did not finish"
    worst, mean = map(float, match.groups())
    assert worst < 250 and mean < 150, f"Streaming UI stalled: worst={worst}, mean={mean}"
    assert not re.search(r"ReferenceError|TypeError|Unable to assign", log), log
    print("PASS: streaming event-loop latency within device budget")

    adb("push", str(pathlib.Path(__file__).with_name("AnimationReplay.qml")), remote + "/")
    motion = subprocess.run([
        "adb", "shell", "XDG_RUNTIME_DIR=/var/run WAYLAND_DISPLAY=wayland-0 "
        "QT_WAYLAND_SHELL_INTEGRATION=xdg-shell-v6 "
        "QT_LOGGING_RULES='*.debug=false;qml.debug=true' "
        f"timeout 10 qmlscene {remote}/AnimationReplay.qml"
    ], capture_output=True, text=True, timeout=15)
    motion_log = motion.stdout + motion.stderr
    print(motion_log.strip())
    assert "ANIMATION PASS" in motion_log, "Bubble/fade/follow/gesture regression"
    assert not re.search(r"ReferenceError|TypeError|Unable to assign", motion_log), motion_log
finally:
    adb("shell", f"rm -rf {remote}")
