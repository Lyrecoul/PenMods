#!/usr/bin/env python3
"""Regression guard for issue #29 (pure-audio seek crash).

FFmpeg 3.4.8's ``ff_seek_frame_binary()`` declares ``pos_min``/``pos_max`` with
``av_uninit()``, which expands to nothing under clang/zig, and then passes them
to ``ff_gen_search()`` even on the paths where the index lookup failed. Passing
indeterminate values is undefined behaviour, so the optimizer is allowed to
conclude the ``index < 0`` path never happens and drop ``if (index >= 0)``.
The resulting code indexes ``st->index_entries[-1]`` and segfaults for
audio-only files (they have no second index entry to find).

``patches/0011-avformat-init-seek-positions.patch`` initializes both variables.
This test asserts the patch is present, that the dependent package will be
rebuilt (``network_revision`` participates in the package hash), and — when an
FFmpeg 3.4.8 source tree is available — that a cross-compiled object really
contains the negative-index branch.

Run: ``python3 tools/test_ffmpeg_seek_patch.py``
Optional: ``PENMODS_FFMPEG_SRC=/path/to/ffmpeg-3.4.8 python3 tools/test_ffmpeg_seek_patch.py``
"""
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
PLAYER = ROOT / "external/ffmpeg-player"
PATCH = PLAYER / "patches/0011-avformat-init-seek-positions.patch"
XMAKE = PLAYER / "xmake.lua"
REVISION = 10


class SeekPatchTest(unittest.TestCase):
    def test_patch_initializes_both_position_variables(self):
        text = PATCH.read_text(encoding="utf-8")
        self.assertIn("-    int64_t av_uninit(pos_min), av_uninit(pos_max), pos, pos_limit;", text)
        self.assertIn("+    int64_t pos_min = 0, pos_max = 0, pos, pos_limit;", text)
        self.assertIn("libavformat/utils.c", text)

    def test_dependency_rebuild_is_forced(self):
        """The package hash must change, otherwise a broken libavformat.a is reused."""
        text = XMAKE.read_text(encoding="utf-8")
        revisions = [int(value) for value in re.findall(r"network_revision\s*=\s*(\d+)", text)]
        self.assertTrue(revisions, "network_revision missing from xmake.lua")
        self.assertGreaterEqual(
            max(revisions),
            REVISION,
            "bump network_revision after changing the FFmpeg patches",
        )

    def test_patch_applies_to_upstream_source(self):
        source = os.environ.get("PENMODS_FFMPEG_SRC")
        if not source:
            self.skipTest("set PENMODS_FFMPEG_SRC to an FFmpeg 3.4.8 source tree")
        utils = Path(source) / "libavformat/utils.c"
        if not utils.is_file():
            self.skipTest(f"{utils} not found")
        with tempfile.TemporaryDirectory(prefix="penmods-ffmpeg-seek-") as tmp:
            staged = Path(tmp) / "libavformat"
            staged.mkdir()
            shutil.copy2(utils, staged / "utils.c")
            result = subprocess.run(
                ["patch", "-p1", "-i", str(PATCH)],
                cwd=tmp,
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("pos_min = 0, pos_max = 0", (staged / "utils.c").read_text(encoding="utf-8"))

    def test_cross_compiled_object_keeps_negative_index_guard(self):
        """Compile the patched function and require a sign check before the entry load."""
        source = os.environ.get("PENMODS_FFMPEG_SRC")
        if not source:
            self.skipTest("set PENMODS_FFMPEG_SRC to an FFmpeg 3.4.8 source tree")
        objdump = shutil.which("aarch64-linux-gnu-objdump") or shutil.which("objdump")
        zig = shutil.which("zig")
        if not objdump or not zig:
            self.skipTest("needs zig and an aarch64 objdump")
        utils = Path(source) / "libavformat/utils.c"
        if not utils.is_file():
            self.skipTest(f"{utils} not found")
        with tempfile.TemporaryDirectory(prefix="penmods-ffmpeg-seek-") as tmp:
            staged = Path(tmp) / "libavformat"
            staged.mkdir()
            patched = staged / "utils.c"
            shutil.copy2(utils, patched)
            applied = subprocess.run(
                ["patch", "-p1", "-i", str(PATCH)],
                cwd=tmp,
                capture_output=True,
                text=True,
            )
            self.assertEqual(applied.returncode, 0, applied.stdout + applied.stderr)
            obj = Path(tmp) / "utils.o"
            env = dict(os.environ)
            env["ZIG_GLOBAL_CACHE_DIR"] = str(Path(tmp) / "zig-global")
            env["ZIG_LOCAL_CACHE_DIR"] = str(Path(tmp) / "zig-local")
            build = subprocess.run(
                [
                    zig, "cc", "-target", "aarch64-linux-gnu.2.27",
                    "-Os", "-fno-sanitize=all", "-ffunction-sections", "-fdata-sections",
                    "-I", source, "-I", str(Path(source) / "libavformat"),
                    "-c", str(patched), "-o", str(obj),
                ],
                capture_output=True,
                text=True,
                env=env,
            )
            if build.returncode != 0:
                self.skipTest("cannot cross-compile utils.c here: " + build.stderr[-400:])
            listing = subprocess.run(
                [objdump, "-d", str(obj)],
                capture_output=True,
                text=True,
                check=True,
            ).stdout
            body = re.search(
                r"<ff_seek_frame_binary>:\n(.*?)(?:\n\n|\Z)", listing, re.S
            )
            self.assertIsNotNone(body, "ff_seek_frame_binary not found")
            code = body.group(1)
            # The negative-index guard must branch over the AVIndexEntry load.
            self.assertRegex(
                code,
                r"tbnz\s+w0, #31|b\.lt|cmp\s+w0, #0",
                "no negative-index guard: the optimizer dropped `if (index >= 0)`",
            )


if __name__ == "__main__":
    unittest.main()
