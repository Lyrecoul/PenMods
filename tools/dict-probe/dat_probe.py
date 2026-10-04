#!/usr/bin/env python3
"""Probe / writer prototype for the Youdao dictpen localdict ".dat" container.

The format was recovered from libDictManager.so (QYdDictManager::readHeader /
open / loadIndex2 / loadData / decode) plus the QyDictManager users in the main
binary.  See tools/dict-probe/README.md and doc/DICT_FORMAT_ANALYSIS.md.

Container layout (little/ big endianness is called out per field):

    header:
        u64  version            0x1004 = "V2" dict, 0x1044 = "V1" (点读包)
        u64  dict_id            6000..6040 for the built-in dictionaries
        u8   name_len ; name    UTF-8 display name
        u64  0x4000000000000060 constant in every V2 file
        u64  word_count         total number of words in the dictionary
        u32  0x0000C800         (51200, initial uncompress buffer hint)
        u32  config_len ; config  ASCII "index1Size=<n>[&configInfo=<key>]"
        if version == 0x1044:     u32 magic_len ; magic == b"3575AA5DDA9D1DBF"
    u32  bucket_count        (little endian)
    bucket_count x {
        word bytes, 0x09 (TAB),
        u32 LE regionA_off      offset of this bucket's index2 blob
        u32 LE bucket_words     number of words in the bucket
        u32 LE regionA_len      compressed length of the index2 blob
        u32 LE regionB_off      offset of this bucket's record blob
        u32 LE regionB_len      compressed length of the record blob
    }
    bucket_count x blob       regionA = zlib(XOR(index2))
    bucket_count x blob       regionB = zlib(XOR(records))

    blob on disk   = XOR(zlibstream, key)          key[i] = (7*i) % 34967
    index2 payload = word bytes, 0x09, u32 BE offset_into_record_blob
    record payload = base-64 varint length, JSON[len]
"""
from __future__ import annotations

import json
import struct
import sys
import zlib

MAGIC = b"3575AA5DDA9D1DBF"
XOR_MOD = 34967
XOR_STEP = 7
TYPE_V2 = 0x1004
TYPE_V1 = 0x1044


class Raw:
    """A record that is copied verbatim, without the varint length prefix.

    Used for the special first record of dictionaries whose config carries
    "&configInfo=...", which the vendor writes with a different framing.
    """

    def __init__(self, packet: bytes):
        self.packet = packet


def xor(buf: bytes) -> bytes:
    return bytes(b ^ ((XOR_STEP * i) % XOR_MOD) & 0xFF for i, b in enumerate(buf))


def varint(n: int) -> bytes:
    """Encode a record length the way QYdDictManager::readData decodes it.

    <= 0x3F      1 byte
    <= 0x3FFF    2 bytes, mark 0x40
    <= 0x3FFFFF  3 bytes, mark 0x80
    else         4 bytes, mark 0xC0
    """
    if n <= 0x3F:
        return bytes([n])
    if n <= 0x3FFF:
        return struct.pack(">H", 0x4000 | n)
    if n <= 0x3FFFFF:
        return bytes([0x80 | (n >> 16), (n >> 8) & 0xFF, n & 0xFF])
    return bytes([0xC0 | ((n >> 24) & 0x3F), (n >> 16) & 0xFF, (n >> 8) & 0xFF, n & 0xFF])


def read_varint(buf: bytes, pos: int):
    """Return (length, header_size) of the varint at *pos*."""
    b0 = buf[pos]
    if b0 <= 0x3F:
        return b0, 1
    if b0 <= 0x7F:
        return ((b0 - 0x40) << 8) | buf[pos + 1], 2
    if b0 <= 0xBF:
        return ((b0 - 0x80) << 16) | (buf[pos + 1] << 8) | buf[pos + 2], 3
    return ((b0 - 0xC0) << 24) | (buf[pos + 1] << 16) | (buf[pos + 2] << 8) | buf[pos + 3], 4


class Reader:
    def __init__(self, data: bytes):
        self.d = data
        self.p = 0

    def take(self, n: int) -> bytes:
        v = self.d[self.p:self.p + n]
        if len(v) != n:
            raise EOFError("short read at 0x%x (want %d)" % (self.p, n))
        self.p += n
        return v

    def u8(self) -> int:
        return self.take(1)[0]

    def u32(self) -> int:
        return struct.unpack("<I", self.take(4))[0]

    def u64(self) -> int:
        return struct.unpack("<Q", self.take(8))[0]


class Bucket:
    __slots__ = ("word", "a_off", "words", "a_len", "b_off", "b_len", "index2", "records")

    def __init__(self, word, a_off, nwords, a_len, b_off, b_len):
        self.word = word
        self.a_off = a_off
        self.words = nwords
        self.a_len = a_len
        self.b_off = b_off
        self.b_len = b_len
        self.index2 = None      # list[(word, offset)] once loaded
        self.records = None     # decompressed record blob

    def dump(self) -> dict:
        return {"word": self.word.decode("utf-8", "replace"), "words": self.words,
                "a_off": self.a_off, "a_len": self.a_len,
                "b_off": self.b_off, "b_len": self.b_len}


class DatFile:
    def __init__(self, path: str):
        self.path = path
        self.data = open(path, "rb").read()
        r = Reader(self.data)
        self.version = r.u64()
        self.dict_id = r.u64()
        n = r.u8()
        self.name = r.take(n)
        self.const18 = r.u64()
        self.word_count = r.u64()
        self.buf_hint = r.u32()
        cl = r.u32()
        self.config = r.take(cl)
        self.magic = None
        if self.version == TYPE_V1:
            ml = r.u32()
            self.magic = r.take(ml)
            if self.magic != MAGIC:
                raise ValueError("V1 header without magic: %r" % self.magic)
        self.header_size = r.p
        self.bucket_count = r.u32()
        self.buckets = []
        for _ in range(self.bucket_count):
            word = bytearray()
            while True:
                b = r.u8()
                if b == 0x09:
                    break
                word += bytes([b])
                if len(word) > 4096:
                    raise ValueError("runaway bucket word at 0x%x" % r.p)
            self.buckets.append(Bucket(bytes(word), r.u32(), r.u32(), r.u32(), r.u32(), r.u32()))
        self.index1_end = r.p
        self.index1_bytes = self.index1_end - (self.header_size + 4)

    # ---- blobs -----------------------------------------------------------
    def bucket_index2(self, i: int):
        b = self.buckets[i]
        if b.index2 is None:
            A = zlib.decompress(xor(self.data[b.a_off:b.a_off + b.a_len]))
            out, p = [], 0
            while p < len(A):
                t = A.index(b"\t", p)
                out.append((A[p:t], int.from_bytes(A[t + 1:t + 5], "big")))
                p = t + 5
            if len(out) != b.words:
                raise ValueError("bucket %d: index2 says %d words, index1 says %d"
                                 % (i, len(out), b.words))
            b.index2 = out
        return b.index2

    def bucket_records(self, i: int) -> bytes:
        b = self.buckets[i]
        if b.records is None:
            b.records = zlib.decompress(xor(self.data[b.b_off:b.b_off + b.b_len]))
        return b.records

    def record(self, i: int, off: int, end: int | None = None):
        """Return (length_tag, payload) of the record at *off* in bucket i.

        The payload excludes the varint length prefix; for the configInfo marker
        record of some dictionaries the tag means something else entirely.
        """
        B = self.bucket_records(i)
        if end is None:
            idx = self.bucket_index2(i)
            pos = [o for _, o in idx].index(off)
            end = idx[pos + 1][1] if pos + 1 < len(idx) else len(B)
        length, hdr = read_varint(B, off)
        return length, B[off + hdr:end]

    # ---- queries ---------------------------------------------------------
    def find(self, word: bytes):
        """Return (bucket_index, record_offset) or None.

        Like the vendor parser, the search key is the ASCII-lowercased word; an
        exact byte match wins over a case-insensitive one.
        """
        key = word.lower()
        for i, b in enumerate(self.buckets):
            idx = self.bucket_index2(i)
            lo, hi = 0, len(idx)
            while lo < hi:
                mid = (lo + hi) // 2
                if idx[mid][0].lower() < key:
                    lo = mid + 1
                else:
                    hi = mid
            for k in range(lo, len(idx)):
                if idx[k][0].lower() != key:
                    break
                if idx[k][0] == word:
                    return i, idx[k][1]
            if lo < len(idx) and idx[lo][0].lower() == key:
                return i, idx[lo][1]
        return None

    def lookup(self, word: str):
        hit = self.find(word.encode("utf-8"))
        if hit is None:
            return None
        i, off = hit
        return self.record(i, off)[1]

    # ---- validation ------------------------------------------------------
    def validate(self, verbose=False) -> list:
        problems = []
        exp1 = 4 + self.index1_bytes
        if self.config.startswith(b"index1Size="):
            n = int(self.config.split(b"=")[1].split(b"&")[0])
            if n != exp1:
                problems.append("config index1Size=%d but measured %d" % (n, exp1))
        if self.bucket_count == 0:
            problems.append("no buckets")
        total = 0
        prev_end_a = self.index1_end
        prev_end_b = None
        for i, b in enumerate(self.buckets):
            if b.a_off != prev_end_a:
                problems.append("bucket %d: region A not contiguous (%d != %d)" % (i, b.a_off, prev_end_a))
            prev_end_a = b.a_off + b.a_len
            if prev_end_b is not None and b.b_off != prev_end_b:
                problems.append("bucket %d: region B not contiguous (%d != %d)" % (i, b.b_off, prev_end_b))
            prev_end_b = b.b_off + b.b_len
            try:
                idx = self.bucket_index2(i)
                B = self.bucket_records(i)
            except Exception as e:
                problems.append("bucket %d: %s" % (i, e))
                continue
            total += len(idx)
            keys = [w.lower() for w, _ in idx]
            if keys != sorted(keys):
                problems.append("bucket %d: words are not sorted by the lowercased key" % i)
            for k, (w, off) in enumerate(idx):
                nxt = idx[k + 1][1] if k + 1 < len(idx) else len(B)
                if off + 2 > len(B) or nxt > len(B):
                    problems.append("bucket %d: record offset out of range (%d)" % (i, off))
                    continue
                span = nxt - off
                length, hdr = read_varint(B, off)
                if self.version == TYPE_V2:
                    if hdr + length != span:
                        if k or i:  # bucket 0 / record 0 may be the configInfo marker
                            problems.append("bucket %d rec %d (%r): declared %d+%d != %d"
                                            % (i, k, w[:20], hdr, length, span))
                    payload = B[off + hdr:off + hdr + length]
                    if not payload.endswith(b"}"):
                        problems.append("bucket %d rec %d (%r): JSON does not end with '}'"
                                        % (i, k, w[:20]))
                if nxt <= off:
                    problems.append("bucket %d rec %d (%r): offsets not ascending" % (i, k, w[:20]))
                if verbose and k < 2:
                    print("     %-24s off=%-6d len=%-5d %s"
                          % (w.decode("utf-8", "replace")[:24], off, jl, B[off + 2:off + 42]))
        if self.word_count != total:
            problems.append("header word_count=%d but parsed %d words" % (self.word_count, total))
        if self.version == TYPE_V1 and len(self.buckets) and self.buckets[-1].b_off + self.buckets[-1].b_len != len(self.data):
            problems.append("last record blob does not end at EOF (%d != %d)"
                            % (self.buckets[-1].b_off + self.buckets[-1].b_len, len(self.data)))
        return problems


# ---------------------------------------------------------------------------
# writer prototype
# ---------------------------------------------------------------------------
def make_header(name: bytes, dict_id: int, word_count: int, config: bytes,
                version: int = TYPE_V2) -> bytes:
    head = bytearray()
    head += struct.pack("<QQ", version, dict_id)
    head += bytes([len(name)]) + name
    head += struct.pack("<Q", 0x4000000000000060)
    head += struct.pack("<Q", word_count)
    head += struct.pack("<I", 51200)
    head += struct.pack("<I", len(config)) + config
    if version == TYPE_V1:
        head += struct.pack("<I", len(MAGIC)) + MAGIC
    return bytes(head)


def build(entries, path=None, name="PenMods 测试词典", dict_id=6099, words_per_bucket=512,
          version=TYPE_V2, config_extra=None) -> bytes:
    """entries: iterable of (word, record_object_or_bytes); words are sorted for you.

    Words are chunked into buckets of *words_per_bucket*; the engine binary-searches
    the bucket first-words (index1) and then the bucket's index2, so the global word
    order has to be ascending, which sorting guarantees.
    """
    entries = [(w.encode("utf-8") if isinstance(w, str) else w,
                r.packet if isinstance(r, Raw) else
                (json.dumps(r, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
                 if not isinstance(r, bytes) else r)) for w, r in entries]
    # The vendor sorts (and the parser searches) by the ASCII-lowercased key:
    # eckidV2 orders 'a' before 'A' and only this key makes its binary search work.
    entries.sort(key=lambda e: e[0].lower())
    chunks = [entries[i:i + words_per_bucket] for i in range(0, len(entries), words_per_bucket)] or [[]]

    index, payload_a, payload_b = [], [], []
    for chunk in chunks:
        index2, records = bytearray(), bytearray()
        for w, r in chunk:
            index2 += w + b"\t" + struct.pack(">I", len(records))
            if isinstance(r, Raw):
                records += r.packet
            else:
                if len(r) > 0x3FFFFF:
                    raise ValueError("record for %r too long (%s bytes)" % (w, len(r)))
                records += varint(len(r)) + r
        index.append((chunk[0][0] if chunk else b"", len(chunk)))
        payload_a.append(xor(zlib.compress(bytes(index2), 9)))
        payload_b.append(xor(zlib.compress(bytes(records), 9)))

    index1_bytes_only = sum(len(w) + 1 + 20 for w, _ in index)
    config = b"index1Size=%d" % (4 + index1_bytes_only)
    if config_extra:
        config += b"&configInfo=" + config_extra
    header = make_header(name if isinstance(name, bytes) else name.encode("utf-8"),
                         dict_id, len(entries), config, version)

    base = len(header) + 4 + index1_bytes_only
    size_a = sum(len(p) for p in payload_a)
    index1, a_cursor, b_cursor = bytearray(), 0, 0
    for i, (word, nwords) in enumerate(index):
        index1 += word + b"\t" + struct.pack("<IIIII", base + a_cursor, nwords, len(payload_a[i]),
                                             base + size_a + b_cursor, len(payload_b[i]))
        a_cursor += len(payload_a[i])
        b_cursor += len(payload_b[i])

    out = header + struct.pack("<I", len(index)) + bytes(index1) \
        + b"".join(payload_a) + b"".join(payload_b)
    if path:
        open(path, "wb").write(out)
    return out


# ---------------------------------------------------------------------------
def cmd_info(path):
    d = DatFile(path)
    print("%s: %d bytes" % (path, len(d.data)))
    print("  version      = 0x%04x" % d.version)
    print("  dict_id      = %d" % d.dict_id)
    print("  name         = %s" % d.name.decode("utf-8", "replace"))
    print("  const_18     = 0x%016x" % d.const18)
    print("  word_count   = %d" % d.word_count)
    print("  buf_hint     = %d" % d.buf_hint)
    print("  config       = %r" % d.config)
    print("  magic        = %r" % d.magic)
    print("  header_size  = 0x%x" % d.header_size)
    print("  buckets      = %d (index1 = %d bytes)" % (d.bucket_count, d.index1_bytes))
    for b in d.buckets[:12]:
        print("     %-20s words=%-6d A=[%d,+%d) B=[%d,+%d)"
              % (b.word.decode("utf-8", "replace")[:20], b.words, b.a_off, b.a_len, b.b_off, b.b_len))
    if d.bucket_count > 12:
        print("     ... %d more buckets" % (d.bucket_count - 12))


def cmd_list(path, limit=None):
    d = DatFile(path)
    n = 0
    for i in range(d.bucket_count):
        for w, off in d.bucket_index2(i):
            pre, rec = d.record(i, off)
            print("%-32s %s" % (w.decode("utf-8", "replace"), rec[:120].decode("utf-8", "replace")))
            n += 1
            if limit and n >= limit:
                return


def cmd_lookup(path, word):
    d = DatFile(path)
    rec = d.lookup(word)
    if rec is None:
        print("not found: %s" % word)
    else:
        print(rec.decode("utf-8", "replace"))


def cmd_validate(paths, verbose=False):
    for p in paths:
        d = DatFile(p)
        problems = d.validate(verbose=verbose)
        print("%-40s %s" % (p, "OK" if not problems else "%d problems:" % len(problems)))
        for x in problems[:20]:
            print("   -", x)


def cmd_build(out, src, opts):
    rows = [json.loads(l) for l in open(src, encoding="utf-8") if l.strip()]
    build([(r["word"], r["record"]) for r in rows], path=out,
          name=opts.get("name", "PenMods 测试词典"), dict_id=int(opts.get("id", 6099)),
          words_per_bucket=int(opts.get("bucket", 512)))
    print("wrote %s (%d words)" % (out, len(rows)))


def cmd_tsv(out, src, opts):
    """Build from a plain TSV: word, phonetic, pos, tran, example_en, example_zh.

    Several rows may share a word (one per part of speech / example); they are
    merged into a single record in the recommended schema.
    """
    entries = {}
    with open(src, encoding="utf-8") as handle:
        for lineno, line in enumerate(handle, 1):
            line = line.strip("\n")
            if not line.strip() or line.lstrip().startswith("#"):
                continue
            cols = (line.split("\t") + [""] * 6)[:6]
            word = cols[0].strip()
            if not word:
                print("line %d: empty word, skipped" % lineno)
                continue
            record = entries.setdefault(word, {"word": word})
            if cols[1].strip():
                record.setdefault("phonetic", cols[1].strip())
            if cols[2].strip() or cols[3].strip():
                record.setdefault("defs", []).append(
                    {"pos": cols[2].strip(), "tran": cols[3].strip()})
            if cols[4].strip() or cols[5].strip():
                record.setdefault("examples", []).append(
                    {"en": cols[4].strip(), "zh": cols[5].strip()})
    build(list(entries.items()), path=out, name=opts.get("name", "PenMods 测试词典"),
          dict_id=int(opts.get("id", 6099)), words_per_bucket=int(opts.get("bucket", 512)))
    print("wrote %s (%d words from %s)" % (out, len(entries), src))


def cmd_rebuild(src, out):
    """Read every record out of *src* and write an equivalent container to *out*.

    The first record of a dictionary with "configInfo" in its header config is
    carried over verbatim, because the vendor frames it differently.
    """
    d = DatFile(src)
    rows = []
    for i in range(d.bucket_count):
        idx = d.bucket_index2(i)
        for k, (w, off) in enumerate(idx):
            B = d.bucket_records(i)
            if i == 0 and k == 0 and b"&configInfo=" in d.config:
                nxt = idx[1][1] if len(idx) > 1 else len(B)
                rows.append((w, Raw(B[off:nxt])))
                continue
            nxt = idx[k + 1][1] if k + 1 < len(idx) else len(B)
            length, hdr = read_varint(B, off)
            rows.append((w, B[off + hdr:off + hdr + length]))
    extra = d.config.split(b"&configInfo=", 1)[1] if b"&configInfo=" in d.config else None
    build(rows, path=out, name=d.name, dict_id=d.dict_id, version=d.version, config_extra=extra)
    print("rebuilt %s -> %s (%d words)" % (src, out, len(rows)))


def parse_options(args):
    """Split "--name xxx --id 6099" style options out of the positional args."""
    opts, rest = {}, []
    i = 0
    while i < len(args):
        token = args[i]
        if token.startswith("--"):
            key = token[2:].split("=")[0]
            if "=" in token:
                opts[key] = token.split("=", 1)[1]
            elif i + 1 < len(args):
                opts[key] = args[i + 1]
                i += 1
            else:
                raise SystemExit("option %s needs a value" % token)
        else:
            rest.append(token)
        i += 1
    # 也接受位置参数形式: dat_probe.py build out.dat entries.jsonl "词典名" 6099
    return opts, rest


def main():
    args = sys.argv[1:]
    if len(args) < 2:
        print(__doc__)
        print("usage: dat_probe.py info|list|lookup|validate|build|tsv|rebuild ...")
        return 1
    cmd, opts, rest = args[0], {}, args[1:]
    opts, rest = parse_options(rest)
    if len(rest) >= 3:
        # 位置参数: <out> <in> [name] [id]
        if "name" not in opts:
            opts["name"] = rest[2]
        if len(rest) >= 4 and "id" not in opts:
            opts["id"] = rest[3]
    if cmd == "info":
        cmd_info(rest[0])
    elif cmd == "list":
        cmd_list(rest[0], int(rest[1]) if len(rest) > 1 else None)
    elif cmd == "lookup":
        cmd_lookup(rest[0], rest[1])
    elif cmd == "validate":
        cmd_validate(rest)
    elif cmd == "build":
        cmd_build(rest[0], rest[1], opts)
    elif cmd == "tsv":
        cmd_tsv(rest[0], rest[1], opts)
    elif cmd == "rebuild":
        cmd_rebuild(rest[0], rest[1])
    else:
        print("unknown command", cmd)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
