#!/bin/sh
# Refresh (or check) the CA material embedded in libPenMods.so:
#
#   resource/certs/ca-bundle.pem       the Mozilla bundle itself (PEM, certificate blocks only)
#   resource/certs/ca-bundle.hashes    one "<X509_NAME hash>" per certificate, in the same order — the
#                                      CApath file names (<hash>.0) that /etc/ssl/certs requires
#   resource/certs/distrusted.sha256   roots that must never be shipped (blocklist, see GitHub issue #18)
#
# The stock rootfs ships an empty /etc/ssl/certs (see GitHub issue #15), so the mod has to carry its own
# trust anchors: src/helper/CaCertificates publishes them (tmpfs + bind mount over /etc/ssl/certs) and falls
# back to Qt's in-process default configuration when the mount is not possible. That store has no
# revocation checking at all, so a distrusted root is only gone once it is out of this bundle.
#
# Usage:
#   scripts/update_ca_bundle.sh [source.pem]       regenerate from a host bundle
#     Debian/Ubuntu/Arch: /etc/ssl/certs/ca-certificates.crt   (default)
#     Fedora:             /etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem
#   scripts/update_ca_bundle.sh --check [ref.pem]  validate the committed files without touching them:
#     the hashes file must match the PEM line for line, and no blocklisted root may be present. With a
#     reference bundle, roots we ship that the reference no longer has are reported as warnings.
#
# Refresh whenever Mozilla publishes a new NSS root store (and right away after a CA incident), after
# updating the host's ca-certificates package. Never delete PEM blocks by hand: the hashes file is matched to
# the PEM by position, so a hand edit silently files every later certificate under the wrong <hash>.0.
#
# Afterwards re-run `xmake f ...` so the generated build/config/CaBundleData.h is regenerated.
set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
dst=$root/resource/certs/ca-bundle.pem
hashfile=$root/resource/certs/ca-bundle.hashes
blocklist=$root/resource/certs/distrusted.sha256

command -v openssl >/dev/null 2>&1 || { echo "openssl is required to compute the CApath names" >&2; exit 1; }
command -v awk >/dev/null 2>&1 || { echo "awk is required" >&2; exit 1; }

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

# Normalised SHA-256 fingerprint (upper-case hex, no colons) of a single-certificate PEM file.
fingerprint() {
    openssl x509 -in "$1" -noout -fingerprint -sha256 | sed 's/^[^=]*=//; s/://g' | tr 'a-f' 'A-F'
}

# Subject CN (or the full subject when there is no CN), for messages only.
subject_of() {
    s=$(openssl x509 -in "$1" -noout -subject -nameopt utf8,sep_comma_plus,lname 2>/dev/null || true)
    cn=$(printf '%s\n' "$s" | sed -n 's/.*commonName=\([^,]*\).*/\1/p')
    [ -n "$cn" ] && printf '%s\n' "$cn" || printf '%s\n' "${s#subject=}"
}

# split <pem> <dir>: one cert<N>.pem per certificate, prints the count.
split_bundle() {
    mkdir -p "$2"
    awk -v out="$2" '/-----BEGIN CERTIFICATE-----/ { n++ } n > 0 { print > (out "/cert" n ".pem") }' "$1"
    find "$2" -type f -name 'cert*.pem' | wc -l | tr -d ' '
}

# Blocklisted fingerprints, one normalised fingerprint per line.
: > "$work/blocked"
if [ -r "$blocklist" ]; then
    awk '!/^[[:space:]]*#/ && NF { print $1 }' "$blocklist" | tr -d ':' | tr 'a-f' 'A-F' > "$work/blocked"
fi
if grep -qvE '^[0-9A-F]{64}$' "$work/blocked"; then
    echo "malformed SHA-256 fingerprint in $blocklist:" >&2
    grep -vE '^[0-9A-F]{64}$' "$work/blocked" >&2
    exit 1
fi

# reject_blocked <split dir> <count> <label>: exit non-zero when any certificate is blocklisted.
reject_blocked() {
    bad=0
    i=1
    while [ "$i" -le "$2" ]; do
        fp=$(fingerprint "$1/cert$i.pem")
        if grep -qx "$fp" "$work/blocked"; then
            echo "distrusted root in $3 (certificate #$i): $(subject_of "$1/cert$i.pem") SHA256=$fp" >&2
            bad=1
        fi
        i=$((i + 1))
    done
    [ "$bad" -eq 0 ] || {
        echo "refusing: update the source bundle (host ca-certificates) or see $blocklist" >&2
        exit 1
    }
}

if [ "${1:-}" = "--check" ]; then
    ref=${2:-}
    count=$(grep -c -- '-----BEGIN CERTIFICATE-----' "$dst" || true)
    [ "$count" -gt 0 ] || { echo "no certificate found in $dst" >&2; exit 1; }
    split_count=$(split_bundle "$dst" "$work/split")
    [ "$split_count" -eq "$count" ] || { echo "failed to split $dst ($split_count != $count)" >&2; exit 1; }

    awk 'NF { print $1 }' "$hashfile" > "$work/committed"
    nhash=$(wc -l < "$work/committed" | tr -d ' ')
    [ "$nhash" -eq "$count" ] || { echo "$hashfile has $nhash entries, $dst has $count certificates" >&2; exit 1; }

    i=1
    while [ "$i" -le "$count" ]; do
        want=$(openssl x509 -in "$work/split/cert$i.pem" -noout -subject_hash)
        have=$(sed -n "${i}p" "$work/committed")
        [ "$want" = "$have" ] || {
            echo "hash mismatch at certificate #$i ($(subject_of "$work/split/cert$i.pem")):" \
                "file says $have, certificate is $want — re-run scripts/update_ca_bundle.sh" >&2
            exit 1
        }
        i=$((i + 1))
    done
    reject_blocked "$work/split" "$count" "$dst"

    if [ -n "$ref" ]; then
        [ -r "$ref" ] || { echo "cannot read $ref" >&2; exit 1; }
        rcount=$(split_bundle "$ref" "$work/ref")
        : > "$work/reffp"
        i=1
        while [ "$i" -le "$rcount" ]; do
            fingerprint "$work/ref/cert$i.pem" >> "$work/reffp"
            i=$((i + 1))
        done
        stale=0
        i=1
        while [ "$i" -le "$count" ]; do
            fp=$(fingerprint "$work/split/cert$i.pem")
            if ! grep -qx "$fp" "$work/reffp"; then
                echo "warning: shipped root missing from $ref: $(subject_of "$work/split/cert$i.pem") SHA256=$fp" >&2
                stale=$((stale + 1))
            fi
            i=$((i + 1))
        done
        echo "reference $ref: $rcount certificate(s), $stale shipped root(s) not in it"
    fi

    echo "ok: $count certificate(s), hashes match, no distrusted root"
    exit 0
fi

src=${1:-/etc/ssl/certs/ca-certificates.crt}
[ -r "$src" ] || { echo "cannot read $src" >&2; exit 1; }

# Debian's bundle interleaves "# <label>" comment lines; drop them (and blank lines) so the shipped file is
# nothing but certificate blocks — exactly what QSslCertificate::fromData(QSsl::Pem) parses.
awk '!/^#/ && NF' "$src" > "$work/bundle.pem"

count=$(grep -c -- '-----BEGIN CERTIFICATE-----' "$work/bundle.pem" || true)
[ "$count" -gt 0 ] || { echo "no certificate found in $src" >&2; exit 1; }

# Split into single-certificate files and ask openssl for each subject hash. The device has no openssl /
# c_rehash, so these names have to be precomputed here — a bundle dumped into the CApath directory under any
# other name is silently ignored by OpenSSL's by_dir lookup.
split_count=$(split_bundle "$work/bundle.pem" "$work/split")
[ "$split_count" -eq "$count" ] || { echo "failed to split the bundle ($split_count != $count)" >&2; exit 1; }

# A host package that lags behind Mozilla must not reintroduce a root we already know is compromised.
reject_blocked "$work/split" "$count" "$src"

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
