#!/usr/bin/env python3
"""Execute the installed hook with cold-start failures; never touch device paths."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

INSTALLER = Path(__file__).with_name("install-launcher.sh")


class LauncherTests(unittest.TestCase):
    def run_hook(self, helper_body="exit 1", broken_log=False, missing_helper=False, successful=False, watchdog_mode="ok"):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            mod = root / "userdata/PenMods"
            mod.mkdir(parents=True)
            helper = mod / "penmods-update"
            helper.write_text("#!/bin/sh\n" + helper_body + "\n")
            helper.chmod(0o755)
            launcher = root / "usr/bin/runDictPen"
            launcher.parent.mkdir(parents=True)
            launcher.write_text('#!/bin/sh\nset -eu\nstdbuf -o0 ./YoudaoDictPen -platform wayland > $LOG_FILE 2>&1\n')
            subprocess.run(["sh", str(INSTALLER), "--rootfs-prefix", str(root)], check=True, capture_output=True)
            if missing_helper:
                helper.unlink()
            # Only map absolute device paths for the host fixture.
            watchdog = root / 'watchdog-control'
            if watchdog_mode == 'failed':
                watchdog.symlink_to('/dev/full')
            elif watchdog_mode == 'ok':
                watchdog.write_text('running\n')
            launcher.write_text(launcher.read_text().replace('/userdata/PenMods', str(mod)).replace('/tmp/penmods-launch-', str(root / 'penmods-launch-')).replace('/tmp/penmods-safe-mode', str(root / 'penmods-safe-mode')).replace('/tmp/penmods-launch.log', str(root / 'launch.log')).replace('/sys/devices/virtual/misc/watchdog/control', str(watchdog)))
            app = root / "YoudaoDictPen"
            app.write_text('#!/bin/sh\nprintf "app:%s:%s\\n" "$*" "$PRESERVED" >> app-runs\nexit 0\n')
            app.chmod(0o755)
            bins = root / "bin"
            bins.mkdir()
            # Fixed clock and captured sleeps make the guardian timing assertion deterministic.
            for name, body in {"date": "echo 100", "sleep": 'echo "$1" >> sleeps'}.items():
                path = bins / name
                path.write_text('#!/bin/sh\n' + body + '\n')
                path.chmod(0o755)
            env = dict(os.environ, PATH=str(bins) + ':' + os.environ['PATH'], PRESERVED='yes',
                       LOG_FILE=str(root / ('missing/log' if broken_log else 'app.log')))
            result = subprocess.run(['sh', str(launcher)], cwd=root, env=env, capture_output=True, text=True, timeout=5)
            runs = (root / 'app-runs').read_text() if (root / 'app-runs').exists() else ''
            sleeps = (root / 'sleeps').read_text() if (root / 'sleeps').exists() else ''
            if watchdog_mode == 'ok':
                self.assertEqual(watchdog.read_text().strip(), 'stop', 'vendor boot watchdog remains armed while helper validates components')
            if watchdog_mode == 'failed':
                self.assertIn('bypassing helper', (root / 'app.log').read_text())
            self.assertEqual(runs.count('app:-platform wayland:yes'), 1, result.stderr)
            if successful:
                self.assertNotIn('helper failed', (root / 'app.log').read_text())
            self.assertTrue(sleeps and int(sleeps.strip()) >= 16, 'fast return reaches guardian without cooldown')

    def test_three_failures_enter_stock_until_next_boot(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            mod = root / 'userdata/PenMods'
            mod.mkdir(parents=True)
            helper = mod / 'penmods-update'
            helper.write_text('#!/bin/sh\necho "$1" >> calls\nif [ "$1" = stock ]; then exit 0; fi\nexit "$(cat result)"\n')
            helper.chmod(0o755)
            launcher = root / 'usr/bin/runDictPen'
            launcher.parent.mkdir(parents=True)
            launcher.write_text('#!/bin/sh\nset -eu\nstdbuf -o0 ./YoudaoDictPen -platform wayland > $LOG_FILE 2>&1\n')
            subprocess.run(['sh', str(INSTALLER), '--rootfs-prefix', str(root)], check=True, capture_output=True)
            clock = root / 'uptime'
            clock.write_text('10.0 1.0\n')
            launcher.write_text(launcher.read_text().replace('/userdata/PenMods', str(mod))
                .replace('/tmp/penmods-', str(root / 'penmods-'))
                .replace('/proc/uptime', str(clock))
                .replace('/sys/devices/virtual/misc/watchdog/control', str(root / 'no-watchdog')))
            app = root / 'YoudaoDictPen'
            app.write_text('#!/bin/sh\nexit 42\n'); app.chmod(0o755)
            bins = root / 'bin'; bins.mkdir()
            sleep = bins / 'sleep'; sleep.write_text('#!/bin/sh\nexit 0\n'); sleep.chmod(0o755)
            env = dict(os.environ, PATH=str(bins) + ':' + os.environ['PATH'], LOG_FILE=str(root / 'log'))
            def run(code):
                (root / 'result').write_text(str(code))
                return subprocess.run(['sh', str(launcher)], cwd=root, env=env, timeout=5).returncode
            count = root / 'penmods-launch-failures'
            safe = root / 'penmods-safe-mode'
            self.assertEqual(run(42), 42)
            self.assertEqual(count.read_text().strip(), '1')
            run(143)  # Intentional restart clears the sequence.
            self.assertEqual(count.read_text().strip(), '0')
            run(42); run(42)
            self.assertFalse(safe.exists())
            self.assertEqual(run(42), 0)
            self.assertTrue(safe.exists())
            before = (root / 'calls').read_text().count('run\n')
            run(42)
            self.assertEqual((root / 'calls').read_text().count('run\n'), before)
            # Emulate reboot by clearing only boot-scoped state.
            safe.unlink(); count.unlink()
            run(42)
            self.assertEqual(count.read_text().strip(), '1')
            self.assertFalse(safe.exists())
            # A long-lived process breaks the failure sequence.
            helper.write_text(helper.read_text().replace('exit "$(cat result)"',
                'echo "100.0 1.0" > uptime\nexit 42'))
            run(42)
            self.assertEqual(count.read_text().strip(), '0')

    def test_watchdog_stop_failure_bypasses_helper(self):
        self.run_hook(helper_body='touch helper-invoked; exit 0', watchdog_mode='failed')

    def test_without_vendor_watchdog(self):
        self.run_hook(watchdog_mode='missing')

    def test_success_does_not_launch_twice(self):
        self.run_hook(helper_body='shift 2; exec "$@"', successful=True)

    def test_helper_crash(self):
        self.run_hook(helper_body='kill -KILL $$')

    def test_helper_failure(self):
        self.run_hook()

    def test_missing_helper(self):
        self.run_hook(missing_helper=True)

    def test_log_open_failure(self):
        self.run_hook(broken_log=True)


if __name__ == '__main__':
    unittest.main()
