#!/bin/sh
# Restore the editable QML source tree that belongs to a generated resource
# header.  The reverse of gen_qt_res.sh:
#
#   ./gen_qt_res.sh YDP02X      # tree  -> resource/models/YDP02X/qrc_qml.h
#   ./unpack_qt_res.sh YDP02X   # qrc_qml.h -> tree
#
# The tree is untracked (see .git/info/exclude), so a fresh clone has only the
# header and needs this to get readable/writable files back.

set -e

# An optional first argument is the model directory (like gen_qt_res.sh), but
# options may be passed on their own: `./unpack_qt_res.sh --check`.
platform=YDP02X
case ${1:-} in
    '' | -*) ;;
    *)
        platform=$1
        shift
        ;;
esac

script_path=$(cd "$(dirname "$0")" && pwd)
exec python3 "$script_path/unpack_qrc.py" --platform "$platform" "$@"
