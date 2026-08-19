---
name: penmods-device-workflow
description: Implements, builds, deploys, verifies, and commits PenMods C++ and YDP02X QML changes. Use when modifying PenMods runtime hooks, QML UI, configuration, or device behavior, especially when changes must be tested on an ADB-connected dictionary pen or emulator.
compatibility: Requires xmake, the configured ARM64 Zig/Qt toolchain, and ADB for device deployment.
---

# PenMods Device Workflow

Use this workflow for PenMods changes that affect C++, QML, configuration, hooks, or on-device behavior.

Read `AGENTS.md` before starting. Follow its build, generated-resource, deployment, and worktree constraints.

## 1. Establish Scope

Inspect the current worktree before editing:

```sh
git status --short
git diff --check
git log -5 --oneline
```

Do not revert or include unrelated user changes. Search with `rg` and read the relevant C++, headers, QML, configuration defaults, and existing hooks before choosing an implementation.

Classify the change:

- **C++ only:** runtime code, hooks, QObject properties/signals, configuration behavior.
- **QML/resource only:** files under `resource/models/YDP02X/` with no C++ contract change.
- **Mixed:** QML uses a new or changed C++ property, signal, invokable, enum, or behavior.

Treat a QML change that references a new C++ API as mixed. Deploying only the resource library in that case leaves QML and the loaded C++ ABI out of sync.

## 2. Implement Safely

For QML changes, edit source files under:

```text
resource/models/YDP02X/qml/
```

Never edit `resource/models/YDP02X/qrc_qml.h` manually. It is generated output. The QML source directory may be excluded by local `.git/info/exclude`; check tracking explicitly instead of force-adding files:

```sh
git check-ignore -v resource/models/YDP02X/qml/<file>.qml || true
git ls-files resource/models/YDP02X/qml/<file>.qml
```

For QObject/QML contracts:

- Add `Q_PROPERTY`, `Q_INVOKABLE`, and notify signals deliberately.
- Emit notify signals after the underlying state changes.
- Separate configuration switches from runtime state. Do not use an "enabled" setting as if it means "currently active."
- Put security-sensitive validation in C++, not only in QML.
- Add new configuration keys to defaults so `_fill_missing_defaults` repairs existing installations.

For hooks:

- Reuse an existing hook/event boundary when possible.
- Keep the hook lightweight and forward behavior through `Event` or the owning service.
- Verify mangled symbol names against existing calls, hooks, docs, or the target binary.

## 3. Check Before Building

Run focused static checks that can catch the requested regression, then check whitespace outside generated output:

```sh
git diff --check -- ':!resource/models/YDP02X/qrc_qml.h'
rg -n '<new property|signal|config key>' src resource/models/YDP02X/qml --glob '!qrc_qml.h'
```

Use `clang-format` only on touched C++ files. Review the diff afterward and restore unrelated formatting churn.

## 4. Generate Resources

After every QML or bundled-resource change:

```sh
cd scripts
./gen_qt_res.sh YDP02X
cd ..
```

Do not hand-fix whitespace emitted by `rcc` in `qrc_qml.h`.

## 5. Build the Required Targets

Build targets separately; this xmake version accepts one target at a time.

For QML/resource changes:

```sh
xmake build PenModsResources
```

For C++ changes:

```sh
xmake build PenMods
```

For mixed changes, run both commands. Never use `xmake run`.

The Zig-backed xmake build must run on the host because it writes to `~/.cache/zig`. ADB commands also require host USB/socket access.

Warnings that predate the change may be reported, but new warnings or errors must be investigated.

## 6. Deploy

First verify the connected target:

```sh
adb devices -l
```

Deploy according to scope.

QML/resource only:

```sh
adb push build/linux/arm64-v8a/release/libPenModsResources.so /userdata/PenMods/libPenModsResources.so
```

C++ only:

```sh
adb push build/linux/arm64-v8a/release/libPenMods.so /userdata/PenMods/libPenMods.so
```

Mixed changes must push both libraries:

```sh
adb push build/linux/arm64-v8a/release/libPenMods.so /userdata/PenMods/libPenMods.so
adb push build/linux/arm64-v8a/release/libPenModsResources.so /userdata/PenMods/libPenModsResources.so
```

Restart the process after all pushes complete:

```sh
adb shell 'sync; killall YoudaoDictPen'
sleep 4
adb shell pidof YoudaoDictPen
```

## 7. Verify the Loaded Artifacts

Compare local and device hashes. For mixed changes, verify both libraries:

```sh
sha256sum \
  build/linux/arm64-v8a/release/libPenMods.so \
  build/linux/arm64-v8a/release/libPenModsResources.so

adb shell 'sha256sum \
  /userdata/PenMods/libPenMods.so \
  /userdata/PenMods/libPenModsResources.so'
```

A matching resource hash with a stale main-library hash is a failed mixed deployment.

Check startup/runtime errors relevant to the change:

```sh
adb logcat -d -t 700 2>/dev/null | \
  rg -i 'ReferenceError|TypeError|Unable to assign|Cannot read property|symbol|PenMods'
```

Then execute the smallest real device interaction that proves behavior. Examples include scanning while music plays, toggling a runtime mode, opening a changed settings page, or restarting to verify persistence. If hardware interaction cannot be automated, state the exact remaining manual check.

## 8. Commit on Request

Only commit when requested. Stage an explicit allowlist of files:

```sh
git add <generated-resource-header> <touched-cpp-files>
git diff --cached --check -- ':!resource/models/YDP02X/qrc_qml.h'
git diff --cached --stat
git status --short
```

Do not include unrelated binaries, IDA databases, documentation, or other untracked files. Use a conventional commit message matching the behavior, then report the full hash:

```sh
git commit -m 'feat(scope): concise behavior'
git rev-parse HEAD
git show --stat --oneline HEAD
```

## Completion Checklist

- The implementation uses the correct runtime state and existing ownership boundaries.
- QML resources were regenerated when required.
- Each required xmake target built successfully.
- Every changed runtime library was pushed.
- Device and local hashes match.
- `YoudaoDictPen` restarted and remained running.
- Relevant logs contain no new QML binding or symbol errors.
- The real device behavior was tested, or the remaining hardware check was stated.
- A requested commit contains only the intended tracked files.
