#!/usr/bin/env python3
"""Test the actual online lyrics panel and common controls in an isolated device offscreen window."""
import pathlib
import shutil
import subprocess
root = pathlib.Path(__file__).resolve().parents[2]
build = root / 'build/lyrics-preview-check'
qt = root / 'aarch64-linux-qt-5.15.2'
(build / 'audioplayer').mkdir(parents=True, exist_ok=True)
(build / 'commons').mkdir(exist_ok=True)
source = root / 'resource/models/YDP02X/qml'
shutil.copy2(source / 'audioplayer/YOnlineLyricsPanel.qml', build / 'audioplayer')
for name in ['YColors', 'YMouseArea', 'YLoader', 'YButton', 'YButtonBase', 'YButtonBaseMouseArea',
             'YTimer', 'YText', 'YTextMedium', 'YTextBase', 'YVerticalTitleBar', 'YVerticalTitleBarBase',
             'YBackButton', 'YBackButtonBase', 'YIconButton', 'YImage', 'YImageBase', 'YBaseListView', 'YPagePopHelper']:
    shutil.copy2(source / ('commons/' + name + '.qml'), build / 'commons')
shutil.copy2(source / 'commons/qmldir', build / 'commons')
subprocess.run(['moc', str(root / 'tools/lyrics-probe/preview.cpp'), '-o', str(build / 'moc_preview.cpp')], check=True)
command = ['zig', 'c++', '-target', 'aarch64-linux-gnu.2.27', '-std=c++17', '-O2', '-fPIC',
           '-I' + str(build), '-I' + str(qt / 'include')]
for module in ['Core', 'Gui', 'Quick', 'Qml', 'Network']:
    command.append('-I' + str(qt / ('include/Qt' + module)))
command += [str(root / 'tools/lyrics-probe/preview.cpp'), '-L' + str(qt / 'lib'),
            '-Wl,--allow-shlib-undefined', '-lQt5Quick', '-lQt5Qml', '-lQt5Gui', '-lQt5Core', '-o', str(build / 'preview-check')]
subprocess.run(command, check=True)
remote = '/tmp/penmods-lyrics-preview-check'
subprocess.run(['adb', 'shell', 'mkdir -p ' + remote], check=True)
for name in ['audioplayer', 'commons']:
    subprocess.run(['adb', 'push', str(build / name), remote + '/'], check=True)
subprocess.run(['adb', 'push', str(build / 'preview-check'), remote + '/preview.tmp'], check=True)
subprocess.run(['adb', 'shell', 'mv ' + remote + '/preview.tmp ' + remote + '/preview-check; chmod +x ' + remote + '/preview-check'], check=True)
result = subprocess.run(['adb', 'shell', 'QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software ' + remote + '/preview-check'], capture_output=True, text=True, timeout=10)
log = result.stdout + result.stderr
(build / 'run.log').write_text(log)
print(log)
if result.returncode or 'PREVIEW CHECK PASS' not in log or any(word in log for word in ['ReferenceError', 'TypeError', 'Unable to assign']):
    raise RuntimeError('Online lyrics preview check failed')
