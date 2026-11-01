#!/usr/bin/env python3
"""Regression checks for issue #28's embedded-player QML failure path.

These checks intentionally do not load the production QML: the device-only
``com.youdao.pen`` imports and FFmpeg plugin are unavailable on the host. They
assert the source-level contract that keeps an old colliding module usable and
makes Loader errors recoverable.
"""
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
AUDIO_PAGES = ROOT / "resource/models/YDP02X/qml/audiopages"
EXTERNAL_PLAYER = AUDIO_PAGES / "ExternalPlayer.qml"
EMBEDDED_PLAYER = AUDIO_PAGES / "ExternalPlayerEmbedded.qml"


class ExternalPlayerIssue28Test(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.external = EXTERNAL_PLAYER.read_text(encoding="utf-8")
        cls.embedded = EMBEDDED_PLAYER.read_text(encoding="utf-8")

    def test_optional_property_is_not_bound_declaratively(self):
        """Keep the optional property out of QML's compile-time assignments."""
        self.assertNotIn("downsamplingEnabled:", self.embedded)

    def test_loader_error_uses_the_mpv_fallback(self):
        self.assertIn("status === Loader.Error", self.external)
        self.assertIn("id_external_player.useMpvFallback();", self.external)
        self.assertIn("id_external_player.fallbackAttempted", self.external)
        self.assertIn("fallbackToMpv()", self.external)

    def test_loader_is_removed_after_failure_and_back_bar_is_visible(self):
        self.assertIn("embeddedLoadFailed = true", self.external)
        self.assertIn("embedded = false", self.external)
        self.assertIn('visible: !id_external_player.embedded', self.external)
        self.assertIn('source: id_external_player.embedded && !id_external_player.embeddedLoadFailed', self.external)

    def test_media_change_resets_per_media_failure_state(self):
        self.assertIn("embeddedLoadFailed = false", self.external)
        self.assertIn("fallbackAttempted = false", self.external)
        self.assertIn("fallbackFailed = false", self.external)


if __name__ == "__main__":
    unittest.main()
