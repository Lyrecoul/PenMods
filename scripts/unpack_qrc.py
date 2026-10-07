#!/usr/bin/env python3
"""Restore the Qt resource tree from a generated `qrc_qml.h`.

`resource/models/<platform>/{qml,images,language,...}` are untracked (see
`.git/info/exclude`); the only tracked copy of that tree is the `qrc_qml.h`
produced by `scripts/gen_qt_res.sh`. This tool is the reverse of that script:
it parses the three byte arrays `rcc` emitted (`qt_resource_data`,
`qt_resource_name`, `qt_resource_struct`) and writes the original, editable
files back out, so a fresh clone can be turned into a working tree again.

The `rcc` 5.15 layout parsed here:

    data entry   : <u32 length><payload>
                   payload is raw bytes, or `qCompress()` output when the
                   file node carries the COMPRESSED flag (a 4-byte big-endian
                   uncompressed size followed by a zlib stream)
    name entry   : <u16 char count><u32 hash><utf-16be chars>
    struct entry : <u32 nameOffset><u16 flags><u32 childCount>
                   <u32 childOffset|dataOffset><u64 lastModified>
                   childOffset counts struct entries, not bytes; lastModified
                   is the source file's mtime in milliseconds

Entry size, flag values and the meaning of the merged child/data offset were
verified by round-tripping the committed header against the tree it was
generated from (all 771 files byte-identical).

Extraction cannot reproduce the directory entry order, and `rcc` follows it, so
regenerating a header from a restored tree may order entries differently even
when every file matches. That is why mtimes from the header are restored too:
they keep the diff of a regenerated header limited to that ordering instead of
marking every entry as modified.

`--check` compares file contents only, since an edited file legitimately has a
newer mtime than the header records.
"""

import argparse
import os
import re
import sys
import zlib

HEADER_NAME = "qrc_qml.h"
DEFAULT_PLATFORM = "YDP02X"

STRUCT_ENTRY_SIZE = 22
FLAG_COMPRESSED = 0x01
FLAG_DIRECTORY = 0x02

# `lastModified` is stored in milliseconds since the epoch. Clamp it to a
# plausible range (2000-01-01 .. 2200-01-01) before stamping it onto a file, so
# a corrupt header cannot set a bogus date.
MIN_MTIME_MS = 946_684_800_000
MAX_MTIME_MS = 7_258_118_400_000

# gen_qrc.py feeds everything under the model dir into the .qrc except for
# files with these suffixes; mirror that when comparing the tree to a header.
IGNORED_SUFFIXES = (".qrc", ".h")
IGNORED_DIRS = (".git",)


class FormatError(Exception):
    pass


def parse_array(source, name):
    """Extract one `static const unsigned char <name>[] = {...}` byte array."""
    match = re.search(
        r"static const unsigned char " + name + r"\[\]\s*=\s*\{(.*?)\n\};", source, re.S
    )
    if not match:
        raise FormatError(f"cannot find qt_resource array '{name}' in the header")

    body = "\n".join(
        line for line in match.group(1).splitlines() if not line.strip().startswith("//")
    )
    leftovers = [chunk for chunk in re.split(r"0x[0-9a-fA-F]+", body) if chunk.strip(" \t\r\n,")]
    if leftovers:
        raise FormatError(f"unexpected content in qt_resource array '{name}': {leftovers[0]!r}")

    return bytes(int(value, 16) for value in re.findall(r"0x([0-9a-fA-F]+)", body))


def name_at(names, offset):
    if offset < 0 or offset + 2 > len(names):
        raise FormatError(f"name offset {offset} is outside the name table")
    length = int.from_bytes(names[offset : offset + 2], "big")
    start = offset + 6  # <u16 length><u32 hash>
    if start + 2 * length > len(names):
        raise FormatError(f"name at offset {offset} exceeds the name table")
    return names[start : start + 2 * length].decode("utf-16-be")


def struct_entries(struct):
    if len(struct) % STRUCT_ENTRY_SIZE:
        raise FormatError(f"struct table size {len(struct)} is not a multiple of {STRUCT_ENTRY_SIZE}")
    entries = []
    for index in range(len(struct) // STRUCT_ENTRY_SIZE):
        entry = struct[index * STRUCT_ENTRY_SIZE : (index + 1) * STRUCT_ENTRY_SIZE]
        entries.append(
            {
                "name": int.from_bytes(entry[0:4], "big"),
                "flags": int.from_bytes(entry[4:6], "big"),
                "children": int.from_bytes(entry[6:10], "big"),
                # For directories this is the first child's struct index; for
                # files it is the offset into qt_resource_data.
                "offset": int.from_bytes(entry[10:14], "big"),
                "modified": int.from_bytes(entry[14:22], "big"),
            }
        )
    return entries


def payload_at(data, offset, flags, path):
    if offset + 4 > len(data):
        raise FormatError(f"{path}: data offset {offset} is outside the data table")
    length = int.from_bytes(data[offset : offset + 4], "big")
    if offset + 4 + length > len(data):
        raise FormatError(f"{path}: data at offset {offset} exceeds the data table")
    payload = data[offset + 4 : offset + 4 + length]

    if not flags & FLAG_COMPRESSED:
        return payload

    # rcc stores qCompress() output, i.e. the expected size in front of a zlib
    # stream.
    if len(payload) < 6:
        raise FormatError(f"{path}: compressed payload is too short")
    expected = int.from_bytes(payload[:4], "big")
    try:
        raw = zlib.decompress(payload[4:])
    except zlib.error as exc:
        raise FormatError(f"{path}: cannot decompress payload: {exc}") from exc
    if len(raw) != expected:
        raise FormatError(f"{path}: unpacked {len(raw)} bytes, expected {expected}")
    return raw


def extract(tree):
    """Yield (relative path, bytes, mtime in ms) for every file in the tree."""
    data = parse_array(tree, "qt_resource_data")
    names = parse_array(tree, "qt_resource_name")
    entries = struct_entries(parse_array(tree, "qt_resource_struct"))

    stack = [(0, "")]
    seen = set()
    while stack:
        index, path = stack.pop()
        if index >= len(entries):
            raise FormatError(f"struct index {index} is out of range")
        if index in seen:
            raise FormatError(f"struct entry {index} is referenced twice")
        seen.add(index)

        entry = entries[index]
        if index != 0:  # the root node has no name
            path += name_at(names, entry["name"])

        if entry["flags"] & FLAG_DIRECTORY:
            base = path + "/" if index != 0 else ""
            children = [(entry["offset"] + child, base) for child in range(entry["children"])]
            if entry["offset"] + entry["children"] > len(entries):
                raise FormatError(f"{path or '/'}: children exceed the struct table")
            stack.extend(reversed(children))  # keep the resource order
            continue

        yield path, payload_at(data, entry["offset"], entry["flags"], path), entry["modified"]


def target_path(root, relative):
    parts = [part for part in relative.split("/") if part not in ("", ".")]
    if not parts or relative.startswith("/") or ".." in parts:
        raise FormatError(f"refusing to write outside the output directory: {relative!r}")
    return os.path.join(root, *parts)


def tree_paths(root):
    """Existing resource files under `root`, matching gen_qrc.py's walk."""
    found = set()
    for directory, dirnames, filenames in os.walk(root):
        dirnames[:] = [name for name in dirnames if name not in IGNORED_DIRS]
        for filename in filenames:
            if filename.endswith(IGNORED_SUFFIXES):
                continue
            full = os.path.join(directory, filename)
            found.add(os.path.relpath(full, root).replace(os.sep, "/"))
    return found


def run_check(root, files):
    problems = 0
    listed = set()
    for relative, raw, _ in files:
        listed.add(relative)
        path = target_path(root, relative)
        if not os.path.exists(path):
            print(f"missing:   {relative}")
            problems += 1
            continue
        with open(path, "rb") as handle:
            if handle.read() != raw:
                print(f"different: {relative}")
                problems += 1

    for relative in sorted(tree_paths(root) - listed):
        print(f"untracked: {relative} (on disk but not in the header)")
        problems += 1

    if problems:
        print(f"\n{root} is out of sync with the header ({problems} difference(s)).")
        return 1
    print(f"{root} matches the header ({len(files)} files).")
    return 0


def run_restore(root, files, dry_run, force, keep_mtime):
    created = unchanged = replaced = skipped = 0
    for relative, raw, modified in files:
        path = target_path(root, relative)
        exists = os.path.exists(path)
        if exists:
            with open(path, "rb") as handle:
                identical = handle.read() == raw
            if identical:
                unchanged += 1
                if dry_run:
                    print(f"up-to-date: {relative}")
                continue
            if not force:
                skipped += 1
                print(f"WARNING: {relative} differs, keeping it (use --force to overwrite)", file=sys.stderr)
                continue
            replaced += 1
            action = "replace"
        else:
            created += 1
            action = "create"

        if dry_run:
            print(f"{action}:   {relative}")
            continue
        directory = os.path.dirname(path)
        if directory:
            os.makedirs(directory, exist_ok=True)
        with open(path, "wb") as handle:
            handle.write(raw)
        stamp = restore_time(modified)
        if keep_mtime and stamp is not None:
            os.utime(path, (stamp, stamp))

    print(
        f"{created} created, {replaced} replaced, {unchanged} already up-to-date, "
        f"{skipped} left alone" + (" (dry run)" if dry_run else "")
    )
    return 0


def restore_time(modified):
    """Turn a struct entry's lastModified (ms) into a mtime, if it is plausible."""
    if not MIN_MTIME_MS <= modified <= MAX_MTIME_MS:
        return None
    return modified / 1000.0


def default_header(platform):
    repo = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    return os.path.join(repo, "resource", "models", platform, HEADER_NAME)


def main():
    parser = argparse.ArgumentParser(
        description="Restore the Qt resource tree from a generated qrc_qml.h (reverse of gen_qt_res.sh)."
    )
    parser.add_argument(
        "header",
        nargs="?",
        help=f"generated resource header (default: resource/models/<platform>/{HEADER_NAME})",
    )
    parser.add_argument(
        "-p",
        "--platform",
        default=DEFAULT_PLATFORM,
        help=f"model directory to locate the header in (default: {DEFAULT_PLATFORM})",
    )
    parser.add_argument(
        "-o",
        "--output",
        help="directory to write the tree into (default: the directory holding the header)",
    )
    parser.add_argument(
        "-n",
        "--dry-run",
        action="store_true",
        help="only report what would be written",
    )
    parser.add_argument(
        "-f",
        "--force",
        action="store_true",
        help="overwrite files whose contents differ instead of leaving them alone",
    )
    parser.add_argument(
        "--no-mtime",
        action="store_true",
        help="do not restore file modification times from the header (keep the extraction time); "
        "rcc embeds the mtime of every entry, so restoring them keeps a regenerated header's "
        "timestamps stable",
    )
    parser.add_argument(
        "--check",
        action="store_true",
        help="compare the tree against the header instead of writing it; exit 1 when out of sync",
    )
    args = parser.parse_args()

    header = args.header or default_header(args.platform)
    if not os.path.isfile(header):
        parser.error(f"{header} does not exist")
    root = args.output or os.path.dirname(os.path.abspath(header))

    with open(header, "r", encoding="utf-8", errors="replace") as handle:
        tree = handle.read()

    try:
        files = list(extract(tree))
    except FormatError as exc:
        parser.error(f"{header}: {exc}")

    if not files:
        parser.error(f"{header}: no files found in the resource tree")

    if args.check:
        return run_check(root, files)
    return run_restore(root, files, args.dry_run, args.force, not args.no_mtime)


if __name__ == "__main__":
    sys.exit(main())
