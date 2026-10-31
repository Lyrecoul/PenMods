#!/usr/bin/env python3
"""Throwaway lyrics renderer comparison. Default: build and benchmark offscreen (no device UI interaction)."""
import argparse
import json
import pathlib
import shutil
import subprocess

ROOT = pathlib.Path(__file__).resolve().parents[2]
SOURCE = pathlib.Path(__file__).resolve().parent
BUILD = ROOT / 'build/lyrics-render-prototype'
REMOTE = '/tmp/penmods-lyrics-render-prototype'


def run(args, **kwargs):
    return subprocess.run(args, check=True, **kwargs)


def adb(*args, **kwargs):
    return run(['adb', *args], **kwargs)


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--build-only', action='store_true')
parser.add_argument('--launch', action='store_true', help='User-operated: open the comparison window on the pen')
args = parser.parse_args()
BUILD.mkdir(parents=True, exist_ok=True)
qt = ROOT / 'aarch64-linux-qt-5.15.2'
moc = shutil.which('moc') or '/usr/lib/qt5/bin/moc'
run([moc, str(SOURCE / 'main.cpp'), '-o', str(BUILD / 'prototype.moc')])
cmd = ['zig', 'c++', '-target', 'aarch64-linux-gnu.2.27', '-std=c++17', '-O2', '-fPIC',
       '-I' + str(BUILD), '-I' + str(qt / 'include')]
for module in ['Core', 'Gui', 'Qml', 'Quick', 'Network']:
    cmd.append('-I' + str(qt / ('include/Qt' + module)))
cmd += [str(SOURCE / 'main.cpp'), '-L' + str(qt / 'lib'), '-Wl,--allow-shlib-undefined',
        '-lQt5Quick', '-lQt5Qml', '-lQt5Gui', '-lQt5Core', '-o', str(BUILD / 'prototype')]
run(cmd)
shutil.copy2(SOURCE / 'Prototype.qml', BUILD / 'Prototype.qml')
if args.build_only:
    print(BUILD / 'prototype')
    raise SystemExit(0)
adb('shell', 'mkdir -p ' + REMOTE)
for name in ['prototype', 'Prototype.qml']:
    adb('push', str(BUILD / name), REMOTE + '/' + name)
adb('shell', 'chmod +x ' + REMOTE + '/prototype')
if args.launch:
    # Deliberately opt-in: no touch injection and no host application restart.
    adb('shell', 'XDG_RUNTIME_DIR=/var/run WAYLAND_DISPLAY=wayland-0 '
        'QT_WAYLAND_SHELL_INTEGRATION=xdg-shell-v6 ' + REMOTE + '/prototype')
else:
    results = {}
    for variant in ['qml', 'native']:
        output = adb('shell', 'QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software '
                     + REMOTE + '/prototype --benchmark' + (' --native' if variant == 'native' else ''),
                     capture_output=True, text=True, timeout=25)
        log = output.stdout + output.stderr
        (BUILD / (variant + '.log')).write_text(log)
        print(log)
        if 'RESULT ' + variant not in log or 'frames= 0' in log:
            raise RuntimeError('Benchmark produced no rendered frame samples')
        if any(error in log for error in ['ReferenceError', 'TypeError', 'Unable to assign']):
            raise RuntimeError('QML runtime errors; see ' + str(BUILD))
        for name in [variant + '.png'] + [variant + '-' + str(i) + '.png' for i in range(5)]:
            adb('pull', REMOTE + '/' + name, str(BUILD / name))
        results[variant] = log
    (BUILD / 'results.json').write_text(json.dumps(results, ensure_ascii=False, indent=2))
    print('Offscreen SOFTWARE measurements only; visible Wayland/GPU validation remains manual.')
