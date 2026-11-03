# Native update launcher

This helper is a separate native executable. It has no Qt dependency, does not
load PenMods, and stays outside candidate generations. The Lead builds target
PenModsUpdate as penmods-update with static libc++, nlohmann_json, Zig's
 aarch64-linux-gnu.2.27 target, and PENMODS_VERSION/PENMODS_CHANNEL string defines.
The only external program used during staging is a fixed-path unzip -p child
(and fixed-path md5sum for legacy core packages); no package script executes.

## Commands

Global options precede the command:

    penmods-update [--root STORE] [--prefix ROOTFS] [--core-version X.Y.Z] COMMAND

The default store is /userdisk/PenMods/update-system. --prefix maps device paths
for host tests; --root independently selects the store. Production uses neither.

- status prints JSON, without initializing or changing active aliases.
- init --snapshot-only imports baseline files only when no state exists and records a
  confirmed generation without creating the active pointer or changing legacy aliases.
- init --reinstall-snapshot requires the disabled marker and an exclusive supervisor lock.
  It snapshots current fixed libraries/extensions into a fresh confirmed generation,
  resets stale pending/trial state, and preserves active pointer and aliases for the
  still-running legacy process. It does not clear disabled.
- init additionally repairs the active pointer and migrates compatibility aliases.
  Use only without a running app; a running managed supervisor is rejected.
- stage DESCRIPTOR_JSON ARCHIVE_ZIP verifies and merges a pending generation.
  It automatically performs snapshot-only baseline import if needed. It never
  changes the active pointer or compatibility aliases.
- enable requires disabled plus a valid pending core with health_protocol:1, checks
  current/pending payloads and dependencies, then durably removes disabled. It never
  changes the active pointer or aliases. A failed bootstrap remains bypassed.
- cancel clears pending state. A trial cannot be cancelled.
- run -- APP ARGS... starts and supervises the application with inherited working
  directory, environment, stdin, stdout and stderr. The child remains named by its
  actual executable, so killall YoudaoDictPen targets the app, not its supervisor.

status returns schema, installed, active, pending, trial, components,
pending_components and last_error. Component maps contain version and path.
Imported optional extensions have unknown version 0.0.0. Baseline core version
comes from PENMODS_VERSION or --core-version. Components are core, player,
mathjax and ppocr. Custom MathJax paths are not imported automatically.

New core ZIPs contain both libPenMods.so and libPenModsResources.so at the root.
The ignored legacy _do_update.sh member is allowed only for core; it is never
extracted or executed. Imported baseline core can omit external resources and
continue using its embedded QML. Player ZIPs contain
FFmpegPlayer/{libffmpegplayerplugin.so,qmldir}; MathJax contains mathjax-server;
PP-OCR contains libyocr.so and models/PP_OCRv5_mobile_rec.ncnn.{param,bin}.
Extra OCR model pairs may use ASCII letters, digits, underscore, dot and hyphen.

Descriptors require id, version (X.Y.Z), model (YDP02X), arch (aarch64), channel
matching PENMODS_CHANNEL, exact compressed size and unpacked_size, and sha256.
Legacy core can use md5 instead. Core also requires health_protocol:1. A requires
object maps component IDs to min and/or max_exclusive version bounds. The complete
resulting combination is checked, including dependencies of unchanged components.

## Bootstrap and launcher hook

The authored bootstrap shell installs the helper using a scratch file plus
rename, then runs:

    /userdata/PenMods/penmods-update init --reinstall-snapshot   # when disabled marker exists
    /userdata/PenMods/penmods-update init --snapshot-only         # first install otherwise
    /userdata/PenMods/penmods-update stage core-descriptor.json core-package.zip
    sh install-launcher.sh
    /userdata/PenMods/penmods-update enable

Only report installer success after all steps succeed. For a disabled reinstall,
keep the marker until staging and exact launcher-hook installation both succeed;
`enable` validates pending protocol and durability before allowing the next run.
Installing the hook never kills the app or initializes the store itself. It backs up /usr/bin/runDictPen
once as runDictPen.pre-penmods-update and replaces exactly this full line:

    stdbuf -o0 ./YoudaoDictPen -platform wayland > $LOG_FILE 2>&1

with a guarded shell block (`BEGIN/END PenMods guarded launcher v3`). The block
runs the helper without stdbuf and passes `stdbuf -o0 ./YoudaoDictPen -platform
wayland` as the child command instead, keeping stdbuf's preload out of the helper.
Before verification it stops the vendor boot watchdog through
`/sys/devices/virtual/misc/watchdog/control`, using the same `stop` operation the
stock app performs during initialization. The helper's full component verification
can postpone that app operation beyond the boot watchdog deadline; guardian
cooldown alone cannot prevent this independent hardware reset. If the node exists
but stopping it fails, the hook bypasses the helper and starts the app immediately.
Devices without that vendor node retain the normal helper path.
It opens the quoted LOG_FILE inside the guard; an unavailable log path falls back
to /tmp/penmods-launch.log. Loader errors, native exceptions escaping emergency
recovery, and abnormal helper exits attempt the original app command with update
heartbeat variables cleared. Successful exits and TERM/INT/HUP/QUIT exit statuses
do not launch a second app. The fallback uses the current library aliases; it is
not proof that a damaged generation or missing filesystem can be recovered.

After the helper/fallback returns, the shell always waits 16 seconds before
returning to the vendor guardian. This includes normal app restarts, which now
have an additional 16-second delay. A relative sleep avoids cold-boot wall-clock
adjustments defeating the guardian's <15-second crash counter. Even if both helper
and original app fail to load, they cannot cause the stock six-fast-exits reboot
escalation through this entry. The shell guard is necessary because native helper
exception handling cannot cover its own dynamic loader or an outer redirection.

Unknown/ambiguous layouts fail before writing. Re-running an exact guarded hook
succeeds. The exact v1 hook and the released v2 guarded hook are upgraded while
retaining the original stock backup. Modified v2 blocks are rejected by hash.
Host tests use sh install-launcher.sh --rootfs-prefix DIR. Issue #32 was reproduced
on-device with repeated cold-boot resets and slot fallback. With the watchdog
handoff moved before verification, both slots booted successfully: the hook stopped
the watchdog at about 10.9 seconds, while the stock app reached its own stop at
about 30.1 seconds. Loader, log-open and early recovery failures remain separate
fault-injection cases covered by the shell guard. Native `run` also registers its original
app command before probing `disabled`, so a filesystem exception during that early
probe enters independent-backup recovery rather than returning 1 immediately.
A deterministic ELOOP regression test covers this previously unprotected path.

## Boot-scoped stock safe mode

The v3 shell hook counts consecutive abnormal helper exits whose elapsed runtime
is less than 60 seconds (including verification and app runtime). On the third
failure it immediately starts the stock backup; subsequent guardian relaunches
stay in stock mode for this boot. Exit 0 and HUP/INT/QUIT/TERM statuses, or a run
lasting at least 60 seconds, clear the sequence. Candidate failures recovered
inside the supervisor do not count unless the recovered run also exits quickly.
The count is independent of the vendor guardian counter: the 16-second cooldown
continues to protect against firmware escalation.

State lives in `/tmp/penmods-launch-failures` and `/tmp/penmods-safe-mode`, so a
full reboot retries PenMods. This detects fast exits, not a permanently hung app
or a hardware/power reset. It does not modify the application, generation pointers,
partitions, or the persistent update-system disabled marker.

`penmods-update stock -- APP ARGS...` works without accessing the version store.
It searches `APP.original_bak` then `APP.bak`, accepting executable regular files
(no symlinks), 64 bytes to 32 MiB, with a little-endian AArch64 executable ELF
header and no `libPenMods` string anywhere in the file. This is a conservative
check for a mod-patched backup, not cryptographic firmware authenticity validation.
Arguments, cwd and vendor environment are preserved, except that LD_PRELOAD,
LD_AUDIT and update heartbeat variables are cleared. LD_LIBRARY_PATH entries for
`/userdisk/Qtlib`, `/userdata/PenMods`, and managed objects are removed to bypass
optional OCR/library overrides as well. No managed library aliases are rewritten.

A missing/rejected backup or failed stock exec is logged; the hook stays in safe
mode and retains its cooldown rather than retrying the mod. Recovery requires a
usable helper and a stock backup. This cannot rescue a helper whose own loader is
broken; its loader failure still receives the shell cooldown protection.

## Recovery and durability

Immutable objects are referenced by immutable generations; the single active
symlink selects a full combination. Staging fsyncs files and directories before
publishing a generation and pending journal entry. On launch the helper durably
arms one trial before changing the pointer. An interrupted armed trial is rolled
back rather than retried. A valid state backup recovers a corrupt JSON journal.
Every referenced payload is rehashed before launching; a damaged confirmed
generation can fall back to the retained previous generation. If the userdisk
volume or both journals are unavailable, run alone validates and restores the
independent userdata .pre-update-system core/resource backups, then supervises
the original app without update environment variables. Before first migration,
valid regular non-symlink legacy files may be used unchanged. A current managed
symlink is never accepted as recovery evidence. If no retained or independent
known-good payload remains, an operator must restore an external backup.

A candidate child receives PENMODS_UPDATE_ROOT, PENMODS_UPDATE_GENERATION and a
random PENMODS_UPDATE_TOKEN. After uiCompleted the app atomically writes
health/TOKEN.json every two seconds containing generation, token, pid and an
increasing sequence. The helper checks the actual child PID and requires at least
20 seconds of progressing heartbeats, with no gap exceeding six seconds. No
healthy trial within 120 seconds, child exit, or interrupted helper means rollback.
The parent restores confirmed aliases and relaunches known-good internally, so a
failed candidate does not immediately reach the stock guardian crash counter.
The child receives a parent-death signal to prevent an orphaned candidate.

Pending activation defers when a readable battery capacity is below 20% without
external USB/mains power; missing sensors do not block activation. The pending
generation remains available for the next launch. root/disabled bypasses all store
and alias work and directly execs the original app (used by uninstall).

Stable aliases are changed only before the app starts. Existing legacy files or
directories are preserved once as .pre-update-system backups; same-filesystem
baseline imports use hardlinks to avoid duplicating large payloads. Never write
in place through an alias or its preserved inode. OCR staging does not enable
OCR: an absent /userdisk/Qtlib/libyocr.so stays absent. Existing enabled OCR follows
the same active generation as its models. User config, plugin state, Rime data and
service data remain outside generations.

Staging reserves 2*archive_size + unpacked_size + 8 MiB of available storage and
checks again during extraction. Store-locked garbage collection removes abandoned
staging data and generations/objects not referenced by current or backup state,
retaining confirmed, previous, pending and armed trial generations. Failed stages
also trigger cleanup. The temporary ZIP is privately copied before parsing to
prevent a downloader replacing the validated input during extraction.

## Host checks

Provide the nlohmann_json include directory, then build the test variant:

    c++ -std=c++17 -O1 -g -Wall -Wextra -Werror \
      -DPENMODS_UPDATE_TESTING -DPENMODS_UPDATE_TRIAL_SECONDS=1 \
      -DPENMODS_UPDATE_TIMEOUT_SECONDS=4 -DPENMODS_VERSION='"2.6.3"' \
      -I /path/to/nlohmann-json/include \
      tools/update/main.cpp tools/update/Archive.cpp -o tools/update/penmods-update-test
    python3 tools/update/test_update.py tools/update/penmods-update-test
    python3 tools/update/test_launcher.py

The shell-hook tests install the real hook into a temporary root and execute it
with an unavailable helper, helper error/crash, and failed log open. They verify
original-app fallback, preserved arguments/environment, the vendor boot watchdog
handoff (including absent/failed control), and a 16-second cooldown request (sleep
is captured to keep tests fast), including with shell errexit set.
A successful helper exec is also checked to avoid launching the app twice.

The update-center readiness regression test compiles against host Qt Core:

```sh
c++ -std=c++17 -fPIC -Isrc tools/update_launch_status_test.cpp \
  $(pkg-config --cflags --libs Qt5Core) -o /tmp/penmods-update-launch-status-test
/tmp/penmods-update-launch-status-test tools/update/install-launcher.sh
```

Modern helpers own hook recognition through `launcher_ready`; the UI uses its
legacy command probe only when that field is absent. This prevents a valid guarded
hook from being rejected by a second, outdated parser in the application.

The test variant supports PENMODS_UPDATE_FAILPOINT for deterministic abrupt exits
at object/generation/pending publication, trial arming, pointer publication, child
recording, confirmation and rollback. Production does not read that variable.
The tests create temporary filesystem roots, fake aarch64 headers and a real child
process, and do not call ADB or mutate any live device.
