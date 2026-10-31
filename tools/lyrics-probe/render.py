#!/usr/bin/env python3
"""Cross-build and check the production lyric renderer in a device offscreen window."""
import argparse
import pathlib
import shutil
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--gestures', action='store_true', help='Check fixed title and real offscreen mouse gestures')
parser.add_argument('--handoff', action='store_true', help='Check production bindings during completed-line handoff')
args = parser.parse_args()
root = pathlib.Path(__file__).resolve().parents[2]
target = 'handoff-check' if args.handoff else ('gesture-check' if args.gestures else 'render-check')
build = root / ('build/lyrics-' + target)
build.mkdir(parents=True, exist_ok=True)
qt = root / 'aarch64-linux-qt-5.15.2'
sources = [root / ('tools/lyrics-probe/handoff.cpp' if args.handoff else ('tools/lyrics-probe/gestures.cpp' if args.gestures else 'tools/lyrics-probe/render.cpp')), root / 'src/filemanager/player/LyricLineItem.cpp',
           root / 'src/filemanager/player/LyricTimeline.cpp', root / 'src/filemanager/player/LyricFormats.cpp']
for name in ['LyricLineItem']:
    generated = build / ('moc_' + name + '.cpp')
    subprocess.run(['moc', str(root / ('src/filemanager/player/' + name + '.h')), '-o', str(generated)], check=True)
    sources.append(generated)
command = ['zig', 'c++', '-target', 'aarch64-linux-gnu.2.27', '-std=c++17', '-O2', '-fPIC',
           '-I' + str(root / 'src'), '-I' + str(qt / 'include')]
for module in ['Core', 'Gui', 'Quick', 'Qml', 'Network']:
    command.append('-I' + str(qt / ('include/Qt' + module)))
command += list(map(str, sources)) + ['-L' + str(qt / 'lib'), '-Wl,--allow-shlib-undefined',
           '-lQt5Quick', '-lQt5Qml', '-lQt5Gui', '-lQt5Core', '-o', str(build / target)]
subprocess.run(command, check=True)
remote = '/tmp/penmods-lyrics-' + target
subprocess.run(['adb', 'shell', 'mkdir -p ' + remote], check=True)
if args.handoff:
    source = (root / 'resource/models/YDP02X/qml/audioplayer/YAudioPlayerLrcContent.qml').read_text()
    start = source.index('LyricLineItem {')
    end, depth = start + source[start:].index('{') + 1, 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    line = source[start:end]
    fixture = '''import QtQuick 2.15
import RenderCheck 1.0
Rectangle {
    id: id_delegate_item
    width: 320; height: 170; color: "black"
    property int index: 0
    property int activeLine: 0
    property bool isCurrentLrc: activeLine === 0
    property bool customLyrics: true
    property bool isShowing: true
    property bool lyricsAvailable: true
    property var lrcStateList: [0]
    property int lrcStateIndex: 0
    property string currentFontFamily: "Noto Sans SC"
    property var modelData: ({mainLrc: "Finish", time: 0, cues: [{time: 0, until: 1000, end: 6}]})
    QtObject { id: lyricClock; property int position: 1000 }
    LINE
}'''.replace('LINE', line)
    (build / 'Handoff.qml').write_text(fixture)
    subprocess.run(['adb', 'push', str(build / 'Handoff.qml'), remote + '/Handoff.qml'], check=True)
if args.gestures:
    name = 'YAudioPlayerLyricView.qml'
    shutil.copy2(root / ('resource/models/YDP02X/qml/audioplayer/' + name), build / name)
    subprocess.run(['adb', 'push', str(build / name), remote + '/' + name], check=True)
subprocess.run(['adb', 'push', str(build / target), remote + '/' + target + '.tmp'], check=True)
subprocess.run(['adb', 'shell', 'mv ' + remote + '/' + target + '.tmp ' + remote + '/' + target + '; chmod +x '
                + remote + '/' + target], check=True)
result = subprocess.run(['adb', 'shell', 'QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software '
                         + remote + '/' + target], capture_output=True, text=True, timeout=15)
log = result.stdout + result.stderr
(build / 'render.log').write_text(log)
print(log)
expected = 'HANDOFF CHECK PASS' if args.handoff else ('GESTURE CHECK PASS' if args.gestures else 'RENDER CHECK PASS')
if result.returncode or expected not in log or any(error in log for error in ['TypeError', 'ReferenceError', 'Unable to assign']):
    raise RuntimeError('Production lyric check failed')
names = ['handoff-complete.png', 'handoff-transition.png', 'handoff-settled.png', 'handoff-rewound.png'] if args.handoff else (['before.png', 'after.png', 'empty.png', 'fade.png'] if args.gestures else ['bilingual.png'] + ['render-' + str(time) + '.png' for time in [0, 500, 1500, 2500, 4000]])
for name in names:
    subprocess.run(['adb', 'pull', remote + '/' + name, str(build / name)], check=True)
