#!/usr/bin/env python3
"""Compile production RimeWrapper against QtCore and a recording librime API.

Requires native Qt5Core development tools, spdlog and librime headers.
RIME_INCLUDE_DIR can point to an xmake cross package: no librime binary is linked.
"""
from pathlib import Path
import os
import shlex
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
include = os.environ.get('RIME_INCLUDE_DIR')
if not include:
    candidates = list((Path.home() / '.xmake/packages/l/librime').glob('1.15.0/*/include/rime_api.h'))
    candidates += [Path('/usr/include/rime_api.h')]
    candidates += list((Path.home() / '.xmake/packages/l/librime').glob('*/*/include/rime_api.h'))
    include = next((str(p.parent) for p in candidates if p.is_file()), None)
if not include:
    raise SystemExit('Set RIME_INCLUDE_DIR to the directory containing rime_api.h')
flags = shlex.split(subprocess.check_output(['pkg-config', '--cflags', '--libs', 'Qt5Core', 'spdlog'], text=True))
moc = os.environ.get('MOC') or shutil.which('moc-qt5') or shutil.which('moc')
if not moc:
    raise SystemExit('Qt5 moc is required')
with tempfile.TemporaryDirectory(prefix='penmods-rime-test-') as directory:
    build = Path(directory)
    generated = build / 'moc_RimeWrapper.cpp'
    subprocess.run([moc, '-I' + include, str(root / 'src/rime/RimeWrapper.h'), '-o', str(generated)], check=True)
    command = [os.environ.get('CXX', 'c++'), '-std=c++17', '-fPIC', '-pthread',
               '-include', str(root / 'tests/rime/prelude.h'), '-I' + str(root / 'src'), '-I' + include,
               str(root / 'src/rime/RimeWrapper.cpp'), str(root / 'src/rime/RimeRuntime.cpp'), str(generated), str(root / 'tests/rime/wrapper_test.cpp'),
               '-o', str(build / 'wrapper_test'), *flags]
    subprocess.run(command, check=True)
    subprocess.run([str(build / 'wrapper_test')], check=True)

    try:
        backend_flags = shlex.split(subprocess.check_output(
            ['pkg-config', '--cflags', '--libs', 'Qt5Core', 'Qt5Qml', 'Qt5Quick', 'spdlog'],
            text=True, stderr=subprocess.DEVNULL))
        backend_moc = moc
    except subprocess.CalledProcessError:
        backend_flags = shlex.split(subprocess.check_output(
            ['pkg-config', '--cflags', '--libs', 'Qt6Core', 'Qt6Qml', 'Qt6Quick', 'spdlog'], text=True))
        libexec = subprocess.check_output(['pkg-config', '--variable=libexecdir', 'Qt6Core'], text=True).strip()
        backend_moc = str(Path(libexec) / 'moc')
    generated_backend = []
    for name in ('rime/RimeWrapper', 'rime/Backend', 'common/Event'):
        output = build / ('moc_backend_' + Path(name).name + '.cpp')
        subprocess.run([backend_moc, '-I' + include, str(root / 'src' / (name + '.h')),
                        '-o', str(output)], check=True)
        generated_backend.append(str(output))
    json_headers = list((Path.home() / '.xmake/packages/n/nlohmann_json').glob('*/*/include/nlohmann/json.hpp'))
    json_flags = ['-I' + str(json_headers[0].parents[1])] if json_headers else []
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-fPIC', '-pthread',
                    '-include', str(root / 'tests/rime/backend_prelude.h'),
                    '-I' + str(root / 'src'), '-I' + include, *json_flags,
                    str(root / 'src/rime/RimeWrapper.cpp'), str(root / 'src/rime/RimeRuntime.cpp'),
                    str(root / 'src/rime/Backend.cpp'), *generated_backend,
                    str(root / 'tests/rime/backend_test.cpp'),
                    '-o', str(build / 'backend_test'), *backend_flags], check=True)
    subprocess.run([str(build / 'backend_test')], check=True)
