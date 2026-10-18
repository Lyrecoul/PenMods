#!/bin/sh
# Refresh the CA material embedded in libPenMods.so:
#
#   resource/certs/ca-bundle.pem     the Mozilla bundle itself (PEM, certificate blocks only)
#   resource/certs/ca-bundle.hashes  one "<X509_NAME hash>" per certificate, in the same order — the
#                                    CApath file names (<hash>.0) that /etc/ssl/certs requires
#
# The stock rootfs ships an empty /etc/ssl/certs (see GitHub issue #15), so the mod has to carry its own
# trust anchors: src/helper/CaCertificates publishes them (tmpfs + bind mount over /etc/ssl/certs) and falls
# back to Qt's in-process default configuration when the mount is not possible.
#
# Usage: scripts/update_ca_bundle.sh [source.pem]
#   Debian/Ubuntu: /etc/ssl/certs/ca-certificates.crt   (default)
#   Fedora/Arch:   /etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem
#
# Afterwards re-run `xmake f ...` so the generated build/config/CaBundleData.h is regenerated.
set -eu

src=${1:-/etc/ssl/certs/ca-certificates.crt}
root=$(cd "$(dirname "$0")/.." && pwd)
dst=$root/resource/certs/ca-bundle.pem
hashfile=$root/resource/certs/ca-bundle.hashes

[ -r "$src" ] || { echo "cannot read $src" >&2; exit 1; }
command -v openssl >/dev/null 2>&1 || { echo "openssl is required to compute the CApath names" >&2; exit 1; }
command -v awk >/dev/null 2>&1 || { echo "awk is required" >&2; exit 1; }

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

# Debian's bundle interleaves "# <label>" comment lines; drop them (and blank lines) so the shipped file is
# nothing but certificate blocks — exactly what QSslCertificate::fromData(QSsl::Pem) parses.
awk '!/^#/ && NF' "$src" > "$work/bundle.pem"

count=$(grep -c -- '-----BEGIN CERTIFICATE-----' "$work/bundle.pem" || true)
[ "$count" -gt 0 ] || { echo "no certificate found in $src" >&2; exit 1; }

# Split into single-certificate files and ask openssl for each subject hash. The device has no openssl /
# c_rehash, so these names have to be precomputed here — a bundle dumped into the CApath directory under any
# other name is silently ignored by OpenSSL's by_dir lookup.
mkdir -p "$work/split"
awk -v out="$work/split" '/-----BEGIN CERTIFICATE-----/ { n++ } n > 0 { print > (out "/cert" n ".pem") }' \
    "$work/bundle.pem"
split_count=$(find "$work/split" -type f | wc -l)
[ "$split_count" -eq "$count" ] || { echo "failed to split the bundle ($split_count != $count)" >&2; exit 1; }

: > "$work/hashes"
i=1
while [ "$i" -le "$count" ]; do
    hash=$(openssl x509 -in "$work/split/cert$i.pem" -noout -subject_hash)
    # OpenSSL formats CApath names as %08x; subject_hash already prints 8 lowercase hex digits.
    printf '%s\n' "$hash" >> "$work/hashes"
    i=$((i + 1))
done

mkdir -p "$(dirname "$dst")"
cp "$work/bundle.pem" "$dst"
cp "$work/hashes" "$hashfile"
chmod 644 "$dst" "$hashfile"

echo "wrote $count certificate(s), $(wc -c < "$dst") bytes -> $dst"
echo "wrote $(wc -l < "$hashfile") subject hash(es) -> $hashfile"
