#!/bin/sh
# Assemble the one-file installer: installer/penmods-ppocr.sh.in + a gzip payload
# (shim + model) appended after a byte-exact 4096-byte header, so the installer can
# find the payload with `tail -c +4097 "$0"` no matter where the file is stored.
#
#   scripts/fetch-deps.sh && scripts/build.sh && scripts/make-bundle.sh
#   -> dist/penmods-ppocr.sh
#
# Ship that single file; on the device it is:
#   adb push dist/penmods-ppocr.sh /userdisk/
#   adb shell 'sh /userdisk/penmods-ppocr.sh'

set -e

here=$(cd "$(dirname "$0")/.." && pwd)
CACHE=${CACHE:-$here/.cache}
BUILD=$here/build
DIST=$here/dist
HEADER=$here/installer/penmods-ppocr.sh.in
OUT=$DIST/penmods-ppocr.sh
OFFSET=8192

[ -s "$BUILD/libyocr.so" ] || { echo "error: run scripts/build.sh first"; exit 1; }
[ -s "$CACHE/models/PP_OCRv5_mobile_rec.ncnn.bin" ] || { echo "error: run scripts/fetch-deps.sh first"; exit 1; }

mkdir -p "$DIST" "$BUILD/.payload"
cp -f "$BUILD/libyocr.so" "$BUILD/.payload/"
cp -f "$CACHE/models/PP_OCRv5_mobile_rec.ncnn.param" "$CACHE/models/PP_OCRv5_mobile_rec.ncnn.bin" "$BUILD/.payload/"
tar czf "$BUILD/.payload.tar.gz" -C "$BUILD/.payload" libyocr.so PP_OCRv5_mobile_rec.ncnn.param PP_OCRv5_mobile_rec.ncnn.bin

OFFSET="$OFFSET" python3 - "$HEADER" "$BUILD/.payload.tar.gz" "$OUT" <<'PY'
import os, sys
header, payload, out = sys.argv[1], sys.argv[2], sys.argv[3]
off = int(os.environ['OFFSET'])
head = open(header, 'rb').read()
assert b'PAYLOAD_OFFSET=' + str(off).encode() in head, 'installer payload offset mismatch'
if len(head) > off - 2:
    sys.exit('error: installer header is %d bytes, does not fit in %d' % (len(head), off))
pad = off - len(head) - 1                      # keep room for the newline
head = head + b'#' * pad + b'\n'
assert len(head) == off, len(head)
with open(out, 'wb') as f:
    f.write(head)
    f.write(open(payload, 'rb').read())
os.chmod(out, 0o755)
print('header %d bytes, payload %d bytes, total %d bytes' %
      (off, os.path.getsize(payload), os.path.getsize(out)))
PY

# The appended payload is binary, so only the header can be parsed; check that.
python3 - "$OUT" "$OFFSET" <<'PY2'
import sys
data = open(sys.argv[1], 'rb').read(int(sys.argv[2]))
open('/tmp/.ppocr-header-check.sh', 'wb').write(data)
PY2
sh -n /tmp/.ppocr-header-check.sh || { echo "error: installer header is not valid shell"; exit 1; }
rm -f /tmp/.ppocr-header-check.sh

echo "==> $OUT"
ls -la "$OUT"
echo "    sha256 $(sha256sum "$OUT" | cut -d' ' -f1)"
echo
echo "Install on the device with:"
echo "  adb push $OUT /userdisk/ && adb shell 'sh /userdisk/$(basename "$OUT")'"
