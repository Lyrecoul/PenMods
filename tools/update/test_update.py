#!/usr/bin/env python3
"""Host integration/fault tests; no ADB or device paths are touched.

Usage: python3 tools/update/test_update.py tools/update/penmods-update-test
Build with PENMODS_UPDATE_TESTING, TRIAL_SECONDS=1, TIMEOUT_SECONDS=4.
"""
import hashlib
import json
import os
from pathlib import Path
import signal
import stat
import struct
import subprocess
import sys
import tempfile
import time
import unittest
import zipfile

HELPER = Path(sys.argv[1]).resolve()
sys.argv[1:] = []


def elf(tag=b"candidate"):
    data = bytearray(64)
    data[:6] = b"\x7fELF\x02\x01"
    data[16:20] = b"\x03\x00\xb7\x00"
    return bytes(data) + tag


class UpdateTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="penmods-update-")
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.prefix = self.base / "device"
        self.root = self.prefix / "userdata/PenMods/update-system"
        self.mod = self.prefix / "userdata/PenMods"
        self.mod.mkdir(parents=True)
        (self.mod / "libPenMods.so").write_bytes(elf(b"baseline-main"))
        (self.mod / "libPenModsResources.so").write_bytes(elf(b"baseline-qml"))
        self.call("init")
        self.baseline = self.status()["active"]

    def call(self, *args, ok=True, env=None):
        merged = os.environ.copy()
        if env:
            merged.update(env)
        result = subprocess.run([str(HELPER), "--root", str(self.root), "--prefix", str(self.prefix),
                                 "--core-version", "2.6.3", *map(str, args)], capture_output=True, text=True,
                                env=merged, timeout=12)
        if ok:
            self.assertEqual(result.returncode, 0, result.stderr)
        else:
            self.assertNotEqual(result.returncode, 0, result.stdout)
        return result

    def status(self):
        return json.loads(self.call("status").stdout)

    def package(self, component="core", entries=None, descriptor=None):
        if entries is None:
            entries = [("libPenMods.so", elf()), ("libPenModsResources.so", elf(b"candidate-qml")),
                       ("_do_update.sh", b"exit 99\n")]
        archive = self.base / (component + ".zip")
        with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as out:
            for name, data in entries:
                out.writestr(name, data)
        raw = archive.read_bytes()
        desc = dict(id=component, version="2.7.0", model="YDP02X", arch="aarch64", channel="dev",
                    size=len(raw), unpacked_size=sum(len(data) for _, data in entries),
                    sha256=hashlib.sha256(raw).hexdigest(), requires={}, health_protocol=1)
        if descriptor:
            desc.update(descriptor)
        path = self.base / (component + ".json")
        path.write_text(json.dumps(desc))
        return path, archive

    def app(self, mode):
        script = self.base / "app.py"
        script.write_text('''import json, os, pathlib, sys, time
root=pathlib.Path(os.environ["PENMODS_UPDATE_ROOT"])
token=os.environ.get("PENMODS_UPDATE_TOKEN")
generation=os.environ["PENMODS_UPDATE_GENERATION"]
print("child",os.getpid(),generation,os.getcwd(),os.environ.get("PRESERVED"),flush=True)
mode=sys.argv[1]
if not token: sys.exit(0)
if mode=="crash": sys.exit(42)
if mode=="hang": time.sleep(8);sys.exit(1)
for seq in range(20):
 data=dict(generation=generation,token=token,pid=os.getpid(),sequence=seq+1)
 if mode=="wrongpid": data["pid"]+=1
 if mode=="stale": data["sequence"]=1
 path=root/"health"/(token+".json")
 temp=path.with_suffix(".tmp")
 temp.write_text(json.dumps(data));temp.replace(path)
 time.sleep(.15)
''')
        return [sys.executable, str(script), mode]

    def test_stage_merge_cancel_and_reuse(self):
        desc, archive = self.package()
        before = (self.mod / "libPenMods.so").read_bytes()
        self.call("stage", desc, archive)
        status = self.status()
        self.assertEqual(status["active"], self.baseline)
        self.assertEqual((self.mod / "libPenMods.so").read_bytes(), before)
        self.assertEqual(status["pending_components"]["core"]["version"], "2.7.0")
        desc, archive = self.package("player", [("FFmpegPlayer/libffmpegplayerplugin.so", elf()),
                                                ("FFmpegPlayer/qmldir", b"module FFmpegPlayer\n")])
        self.call("stage", desc, archive)
        status = self.status()
        self.assertIn("player", status["pending_components"])
        self.assertEqual(status["pending_components"]["core"]["version"], "2.7.0")
        self.call("cancel")
        self.assertIsNone(self.status()["pending"])
        self.assertEqual(self.status()["active"], self.baseline)

    def test_hash_size_model_dependencies_and_pair_validation(self):
        cases = [{"sha256": "0" * 64}, {"size": 1}, {"unpacked_size": 1}, {"arch": "x86_64"},
                 {"model": "YDP03X"}, {"health_protocol": 0},
                 {"requires": {"core": {"min": "9.0.0"}}}]
        for change in cases:
            with self.subTest(change=change):
                self.call("stage", *self.package(descriptor=change), ok=False)
                self.assertIsNone(self.status()["pending"])
        self.call("stage", *self.package(entries=[("libPenMods.so", elf())]), ok=False)
        self.call("stage", *self.package("ppocr", [("libyocr.so", elf()), ("models/PP_OCRv5_mobile_rec.ncnn.param", b"param")]), ok=False)

    def test_reject_unsafe_zip_and_local_header_mismatch(self):
        bad = [[("../escape", b"x")], [("/absolute", b"x")], [("a\\b", b"x")],
               [("libPenMods.so", elf()), ("libPenMods.so", elf())], [("hook.sh", b"exit 0")]]
        for entries in bad:
            self.call("stage", *self.package(entries=entries), ok=False)
        desc, archive = self.package()
        raw = bytearray(archive.read_bytes())
        raw[30] ^= 1  # central/local filename mismatch, but valid outer digest
        archive.write_bytes(raw)
        value = json.loads(desc.read_text()); value["sha256"] = hashlib.sha256(raw).hexdigest()
        desc.write_text(json.dumps(value))
        self.call("stage", desc, archive, ok=False)
        # Unix symlink attributes are rejected before extraction.
        with zipfile.ZipFile(archive, "w") as out:
            entry = zipfile.ZipInfo("libPenMods.so")
            entry.create_system = 3; entry.external_attr = (stat.S_IFLNK | 0o777) << 16
            out.writestr(entry, b"/etc/passwd")
        raw = archive.read_bytes();value.update(size=len(raw),sha256=hashlib.sha256(raw).hexdigest(),unpacked_size=11)
        desc.write_text(json.dumps(value));self.call("stage", desc, archive, ok=False)
        self.assertFalse((self.base / "escape").exists())

    def test_trial_confirms_child_heartbeat_and_preserves_io_environment(self):
        self.call("stage", *self.package())
        pending = self.status()["pending"]
        result = self.call("run", "--", *self.app("healthy"), env={"PRESERVED": "yes"})
        self.assertIn("yes", result.stdout)
        status = self.status()
        self.assertEqual(status["active"], pending)
        self.assertEqual(status["components"]["core"]["version"], "2.7.0")
        self.assertIsNone(status["trial"])
        self.assertEqual(json.loads((self.root / "state.json").read_text())["previous"], self.baseline)

    def test_failed_hung_stale_wrong_pid_trial_rolls_back_in_parent(self):
        for mode in ["crash", "hang", "stale", "wrongpid"]:
            with self.subTest(mode=mode):
                self.call("stage", *self.package())
                result = self.call("run", "--", *self.app(mode))
                self.assertIn("starting confirmed", result.stderr)
                self.assertEqual(result.stdout.count("child"), 2)
                self.assertEqual(self.status()["active"], self.baseline)
                self.assertIsNone(self.status()["trial"])

    def test_stage_fault_does_not_change_active(self):
        for point in ["object-published", "generation-published", "pending-committed"]:
            with self.subTest(point=point):
                self.call("stage", *self.package(), env={"PENMODS_UPDATE_FAILPOINT": point}, ok=False)
                self.assertEqual(self.status()["active"], self.baseline)
                self.assertIn(b"baseline-main", (self.mod / "libPenMods.so").read_bytes())
                self.call("cancel")

    def test_interrupted_trial_recovers_without_retry(self):
        for point in ["trial-armed", "active-published", "trial-child-recorded"]:
            with self.subTest(point=point):
                self.call("stage", *self.package())
                self.call("run", "--", *self.app("healthy"), env={"PENMODS_UPDATE_FAILPOINT": point}, ok=False)
                self.call("run", "--", *self.app("crash"))
                self.assertEqual(self.status()["active"], self.baseline)
                self.assertIsNone(self.status()["trial"])

    def test_ocr_install_does_not_enable_and_new_alias_rollback(self):
        entries = [("libyocr.so", elf()), ("models/PP_OCRv5_mobile_rec.ncnn.param", b"param"), ("models/PP_OCRv5_mobile_rec.ncnn.bin", b"bin")]
        self.call("stage", *self.package("ppocr", entries))
        self.call("run", "--", *self.app("crash"))
        self.assertFalse((self.prefix / "userdisk/Qtlib/libyocr.so").exists())
        self.assertFalse((self.prefix / "userdisk/ppocr_backend/libyocr.so").is_symlink())
        self.call("stage", *self.package("ppocr", entries))
        self.call("run", "--", *self.app("healthy"))
        self.assertTrue((self.prefix / "userdisk/ppocr_backend/libyocr.so").is_symlink())
        self.assertFalse((self.prefix / "userdisk/Qtlib/libyocr.so").exists())

    def test_snapshot_only_baseline_optional_resources_and_gc(self):
        prefix = self.base / "legacy"
        mod = prefix / "userdata/PenMods"
        mod.mkdir(parents=True)
        main = mod / "libPenMods.so"
        main.write_bytes(elf(b"embedded-qml"))
        root = mod / "update-system"
        args = [str(HELPER), "--root", str(root), "--prefix", str(prefix), "--core-version", "2.6.3"]
        result = subprocess.run(args + ["init", "--snapshot-only"], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse(main.is_symlink())
        self.assertFalse((root / "active").exists())
        subprocess.run(args + ["run", "--", "/bin/true"], check=True)
        self.assertTrue(main.is_symlink())
        self.assertFalse((mod / "libPenModsResources.so").is_symlink())
        # Repeated invalid transactions cannot fill staging indefinitely.
        for _ in range(4):
            self.call("stage", *self.package(descriptor={"sha256": "0" * 64}), ok=False)
        self.assertEqual(list((self.root / "staging").iterdir()), [])
        self.call("stage", *self.package(), env={"PENMODS_UPDATE_FAILPOINT": "object-published"}, ok=False)
        self.call("cancel")
        self.assertEqual(len(list((self.root / "objects").iterdir())), 1)

    def test_corrupt_candidate_and_confirmed_content_recovery(self):
        self.call("stage", *self.package())
        pending = self.status()["pending"]
        candidate = self.root / "generations" / pending / "core/libPenMods.so"
        candidate.unlink()  # private object loses a payload; baseline remains intact
        self.call("run", "--", *self.app("crash"))
        self.assertEqual(self.status()["active"], self.baseline)
        self.call("stage", *self.package())
        self.call("run", "--", *self.app("healthy"))
        active = self.status()["active"]
        (self.root / "generations" / active / "core/libPenMods.so").write_bytes(b"bitrot")
        self.call("run", "--", "/bin/true")
        self.assertEqual(self.status()["active"], self.baseline)
        # Invalid JSON recovers the last durable backup, including armed trials.
        (self.root / "state.json").write_text("{")
        self.call("run", "--", "/bin/true")
        self.assertEqual(self.status()["active"], self.baseline)

    def test_launcher_installer_exact_backup_idempotence_and_rejection(self):
        launcher = self.prefix / "usr/bin/runDictPen"
        launcher.parent.mkdir(parents=True)
        helper = self.mod / "penmods-update"
        helper.write_text("#!/bin/sh\nexit 0\n"); helper.chmod(0o755)
        original = "#!/bin/sh\n# untouched\nstdbuf -o0 ./YoudaoDictPen -platform wayland > $LOG_FILE 2>&1\n# after\n"
        launcher.write_text(original); launcher.chmod(0o751)
        installer = Path(__file__).with_name("install-launcher.sh")
        command = ["sh", str(installer), "--rootfs-prefix", str(self.prefix)]
        subprocess.run(command, check=True, capture_output=True)
        backup = launcher.with_name("runDictPen.pre-penmods-update")
        self.assertEqual(backup.read_text(), original)
        self.assertEqual(launcher.stat().st_mode & 0o777, 0o751)
        self.assertIn("/userdata/PenMods/penmods-update run -- stdbuf -o0 ./YoudaoDictPen", launcher.read_text())
        self.assertTrue(self.status()["launcher_ready"])
        snapshot = launcher.read_bytes()
        subprocess.run(command, check=True, capture_output=True)
        self.assertEqual(launcher.read_bytes(), snapshot)
        # Upgrade the exact v1 hook without changing the independent stock backup.
        launcher.write_text(original.replace('stdbuf -o0 ./YoudaoDictPen',
            'stdbuf -o0 /userdata/PenMods/penmods-update run -- ./YoudaoDictPen'))
        subprocess.run(command, check=True, capture_output=True)
        self.assertEqual(launcher.read_bytes(), snapshot)
        self.assertEqual(backup.read_text(), original)
        launcher.write_text(original.replace("wayland", "linuxfb"))
        result = subprocess.run(command, capture_output=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(backup.read_text(), original)

    def test_status_requires_exact_launcher_hook_and_executable_helper(self):
        launcher=self.prefix/"usr/bin/runDictPen"
        launcher.parent.mkdir(parents=True)
        helper=self.mod/"penmods-update"
        helper.write_text("helper");helper.chmod(0o755)
        stock="#!/bin/sh\nstdbuf -o0 ./YoudaoDictPen -platform wayland > $LOG_FILE 2>&1\n"
        managed="#!/bin/sh\nstdbuf -o0 /userdata/PenMods/penmods-update run -- ./YoudaoDictPen -platform wayland > $LOG_FILE 2>&1\n"
        launcher.write_text(managed)
        ready=self.status()
        self.assertTrue(ready["launcher_ready"])
        self.assertEqual(ready["launcher_error"],"")
        launcher.write_text(stock)
        missing_hook=self.status()
        self.assertFalse(missing_hook["launcher_ready"])
        self.assertIn("hook missing",missing_hook["launcher_error"])
        launcher.write_text(managed)
        helper.chmod(0o644)
        missing_helper=self.status()
        self.assertFalse(missing_helper["launcher_ready"])
        self.assertIn("not executable",missing_helper["launcher_error"])
        helper.chmod(0o755)
        launcher.write_text(managed+managed.splitlines(keepends=True)[1])
        ambiguous=self.status()
        self.assertFalse(ambiguous["launcher_ready"])
        self.assertIn("ambiguous",ambiguous["launcher_error"])
        (self.root/"disabled").touch()
        disabled=self.status()
        self.assertFalse(disabled["launcher_ready"])
        self.assertIn("disabled",disabled["launcher_error"])

    def test_low_battery_defers_without_consuming_pending(self):
        battery = self.prefix / "sys/class/power_supply/battery"
        battery.mkdir(parents=True)
        (battery / "capacity").write_text("10\n")
        self.call("stage", *self.package())
        pending = self.status()["pending"]
        self.call("run", "--", *self.app("healthy"))
        self.assertEqual(self.status()["pending"], pending)
        self.assertEqual(self.status()["active"], self.baseline)
        self.assertIn("deferred", self.status()["last_error"])
        usb = battery.parent / "usb"
        usb.mkdir(); (usb / "online").write_text("1\n")
        self.call("run", "--", *self.app("healthy"))
        self.assertEqual(self.status()["active"], pending)
        self.assertIsNone(self.status()["pending"])

    def test_disabled_probe_error_still_uses_emergency_backup(self):
        # ELOOP models an inaccessible disabled marker before prepare() starts.
        (self.root / "disabled").symlink_to("disabled")
        result = self.call("run", "--", "/bin/true")
        self.assertIn("independent userdata backup", result.stderr)

    def test_unavailable_store_uses_independent_userdata_backup(self):
        # Both journals are unreadable; no candidate can establish recovery.
        (self.root / "state.json").write_text("{")
        (self.root / "state.backup.json").write_text("{")
        result = self.call("run", "--", "/bin/sh", "-c", 'printf "recovered:%s" "$PENMODS_UPDATE_TOKEN"')
        self.assertEqual(result.stdout, "recovered:")
        self.assertIn("independent userdata backup", result.stderr)
        self.assertEqual((self.mod / "libPenMods.so").readlink(), self.mod / "libPenMods.so.pre-update-system")
        self.assertIn(b"baseline-main", (self.mod / "libPenMods.so").read_bytes())
        # Stage errors must not rewrite those now-restored aliases.
        before = (self.mod / "libPenMods.so").readlink()
        self.call("stage", *self.package(), ok=False)
        self.assertEqual((self.mod / "libPenMods.so").readlink(), before)

    def test_first_start_store_failure_keeps_valid_legacy_files(self):
        prefix = self.base / "first-start"
        mod = prefix / "userdata/PenMods"
        mod.mkdir(parents=True)
        before = elf(b"legacy-never-migrated")
        (mod / "libPenMods.so").write_bytes(before)
        (mod / "libPenModsResources.so").write_bytes(elf(b"legacy-resource"))
        invalid_root = prefix / "not-a-directory"
        invalid_root.write_text("not a volume")
        result = subprocess.run([str(HELPER), "--root", str(invalid_root), "--prefix", str(prefix),
                                 "run", "--", "/bin/true"], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("independent userdata backup", result.stderr)
        self.assertFalse((mod / "libPenMods.so").is_symlink())
        self.assertEqual((mod / "libPenMods.so").read_bytes(), before)

    def test_disabled_reinstall_snapshot_requires_marker_and_resets_stale_state(self):
        self.call("stage", *self.package())
        old = self.status()
        self.assertIsNotNone(old["pending"])
        (self.root / "disabled").touch()
        fixed = self.mod / "libPenMods.so"
        replacement = elf(b"current-uninstalled-core")
        self.call("init", "--snapshot-only")
        self.assertTrue((self.root / "disabled").exists())
        (self.root / "disabled").unlink()
        self.call("init", "--reinstall-snapshot", ok=False)
        (self.root / "disabled").touch()
        # The marker can be set while an already-running helper still owns the lock.
        (self.root / "disabled").unlink()
        running = subprocess.Popen([str(HELPER), "--root", str(self.root), "--prefix", str(self.prefix),
                                    "run", "--", "/bin/sleep", "20"])
        time.sleep(.2)
        (self.root / "disabled").touch()
        self.call("init", "--reinstall-snapshot", ok=False)
        running.send_signal(signal.SIGTERM); running.wait(timeout=5)
        fixed.unlink()
        fixed.write_bytes(replacement)
        before_active = (self.root / "active").readlink()
        self.call("init", "--reinstall-snapshot")
        state = self.status()
        self.assertTrue(state["disabled"])
        self.assertIsNone(state["pending"])
        self.assertIsNone(state["trial"])
        self.assertEqual(state["components"]["core"]["version"], "2.6.3")
        self.assertEqual((self.root / "active").readlink(), before_active)
        old_active=self.root/before_active
        old_manifest=json.loads((old_active/"manifest.json").read_text())
        old_object=old_manifest["components"]["core"]["object"]
        self.assertIn(b"baseline-main",(self.root/"objects"/old_object/"libPenMods.so").read_bytes())
        generation = state["active"]
        manifest = json.loads((self.root / "generations" / generation / "manifest.json").read_text())
        object_id = manifest["components"]["core"]["object"]
        self.assertEqual((self.root / "objects" / object_id / "libPenMods.so").read_bytes(), replacement)
        self.assertEqual(fixed.read_bytes(), replacement)
        self.assertFalse(fixed.is_symlink())
        # Failed stages and normal initialization never clear disabled.
        self.call("stage", *self.package(descriptor={"sha256": "0" * 64}), ok=False)
        self.call("init", "--snapshot-only")
        self.assertTrue((self.root / "disabled").exists())
        self.call("stage", *self.package())
        self.assertTrue((old_active/"manifest.json").is_file())
        self.assertIn(b"baseline-main",(self.root/"objects"/old_object/"libPenMods.so").read_bytes())
        self.call("enable")
        self.assertFalse((self.root / "disabled").exists())
        self.call("run", "--", *self.app("healthy"))
        self.assertEqual(self.status()["components"]["core"]["version"], "2.7.0")

    def test_disabled_reinstall_requires_protocol_one_pending_core(self):
        (self.root / "disabled").touch()
        self.call("init", "--reinstall-snapshot")
        self.call("enable", ok=False)
        self.assertTrue((self.root / "disabled").exists())
        self.call("stage", *self.package(descriptor={"health_protocol": 0}), ok=False)
        self.call("enable", ok=False)
        self.assertTrue((self.root / "disabled").exists())

    def test_malformed_active_pointer_keeps_all_generation_objects(self):
        orphan_id="a"*32
        orphan_generation=self.root/"generations"/orphan_id
        orphan_generation.mkdir()
        (orphan_generation/"manifest.json").write_text(json.dumps({"components":{}}))
        orphan_object=self.root/"objects"/("b"*32)
        orphan_object.mkdir()
        (orphan_object/"payload").write_bytes(b"active recovery payload")
        active=self.root/"active"
        active.unlink()
        active.symlink_to("not-a-generation")
        self.call("cancel")
        self.assertTrue((orphan_generation/"manifest.json").is_file())
        self.assertEqual((orphan_object/"payload").read_bytes(),b"active recovery payload")

    def test_disabled_direct_exec_and_signal_forwarding(self):
        (self.root / "disabled").touch()
        result = self.call("run", "--", "/bin/sh", "-c", "printf bypass")
        self.assertEqual(result.stdout, "bypass")
        (self.root / "disabled").unlink()
        process = subprocess.Popen([str(HELPER), "--root", str(self.root), "--prefix", str(self.prefix),
                                    "run", "--", "/bin/sleep", "30"])
        time.sleep(.2); process.send_signal(signal.SIGTERM)
        self.assertEqual(process.wait(timeout=5), 128 + signal.SIGTERM)


if __name__ == "__main__":
    unittest.main(verbosity=2)
