# AGENTS.md

Guidance for AI coding agents working in this repository.

## What This Project Is

PenMods is a runtime mod framework for **NetEase Youdao Dictionary Pen** devices (YDP02X, YDPG3, YDP03X). It produces `libPenMods.so`, injected via `LD_PRELOAD` into the closed-source `YoudaoDictPen` binary (ARM64 Linux, rk3326, Qt 5.15.2, glibc 2.27). Hooking is done via [Dobby](https://github.com/jmpews/Dobby).

## Build System

Uses **xmake** (not CMake/Make). Do not run `xmake run` — its default `on_run` calls `scripts/install.sh` which references developer-machine paths.

Configure (adapt paths as needed):
```
xmake f --qt="..." --arch=arm64-v8a --build-platform=YDP02X --target-channel=dev --toolchain=zig -m release -vD --cross=aarch64-linux-gnu.2.27
```

On this dev machine (Qt at `$HOME/PenMods/aarch64-linux-qt-5.15.2`):
```
xmake f \
  --qt="$HOME/PenMods/aarch64-linux-qt-5.15.2" \
  --arch=arm64-v8a \
  --build-platform=YDP02X \
  --target-channel=dev \
  --toolchain=zig \
  -m release \
  -vD \
  --cross=aarch64-linux-gnu.2.27 \
  --force-debug-log=true -c
```
- `--build-platform`: `YDP02X`, `YDPG3`, `YDP03X`
- `--target-channel`: `dev`, `canary`, `beta`, `stable`
- `--qemu=y` for emulator testing
- `--cross=aarch64-linux-gnu.2.27` is **required** (glibc 2.27 compat on device)
- Build defines: `PL_BUILD_YDP02X`, `PL_DEV_CHANNEL`, etc. (uppercased from config values)

Targets: `xmake build PenMods` (`libPenMods.so`), `PenModsResources` (Qt resource overrides), `QrcExporter`.

PCH: `src/base/Base.h` (STL, Qt, spdlog, Hook.h). All `.cpp` files use it automatically.

## Commands That Must Run Outside the Sandbox

> Environment note: this applies to sandboxed agents such as Codex. On an unsandboxed host (the maintainer's
> `pi` setup) these commands run directly — `xmake f` / `xmake build` with the Zig toolchain and all `adb`
> commands are verified working with no special handling.

The following commands require local/host execution (or explicit elevated execution outside the agent sandbox):

- All xmake configure and build commands that use the Zig toolchain, including `xmake f ...` and
  `xmake build ...`. Zig writes temporary data under `~/.cache/zig`; the sandbox exposes that location as
  read-only and fails with `ReadOnlyFileSystem`.
- All ADB commands, including `adb devices`, `adb push ...`, and `adb shell ...`. The ADB daemon must bind its
  local smart-socket listener (normally TCP port 5037) and access the USB device; sandbox execution fails with
  `Operation not permitted`.
- The QEMU/VNC deployment workflow started by `scripts/install.sh`. It launches host processes and accesses
  resources outside the workspace, so run it locally. Continue to avoid `xmake run`, which invokes this script
  with developer-machine-specific paths.

Resource generation with `scripts/gen_qt_res.sh` only writes inside the repository and may run in the sandbox.
For the QML workflow below, run the generation step in the sandbox if desired, then run the xmake and ADB steps
outside it.

If an xmake build fails with `cannot runv(/tmp/.xmake*/zigcc/c++, ...), No such file or directory`, the cached
Zig compiler wrapper is stale (its `/tmp` path was cleaned, e.g. after a reboot). Re-run `xmake f ...` to
regenerate it; do not patch the wrapper by hand.

## Architecture

### Entry Point

`src/mod/Mod.cpp` — `__attribute__((constructor)) BeforeMain()` runs before `main()`, initializes all singletons in dependency order, installs bypass hooks (`isVerified = true`, `license_verify = true`).

`src/mod/Engine.cpp` hooks `YGuiApplicationPrivate::initUi` to inject QML context properties and optionally load `libPenModsResources.so`.

### Hooking

- `PEN_HOOK(ret_t, sym, args_t...)` — static registrar calls `DobbyHook` at `.so` load time (via `src/base/Hook.h`)
- `PEN_HOOK_ADDR(ret_t, name, addr, args_t...)` — hook by raw address
- `PEN_SYM(sym)` / `PEN_CALL(ret_t, sym, args_t...)` — symbol lookup / call original
- `src/base/SymDB.cpp` parses `.symtab` via ELFIO at startup; `DobbySymbolResolver` as fallback

### Common Services

- `src/common/Event.h` — Qt signal/slot event bus: `beforeUiInitialization`, `uiCompleted`, `homeButtonPressed`, etc.
- `src/mod/Config` — nlohmann_json backed by `/userdata/PenModsconfig.json` (module dir + `config.json`, no slash); macros `WRITE_CFG` / `UPDATE_CFG`. New keys added to defaults are auto-filled on load.
- `src/common/Utils.h` — `exec()` (shell), `H()` (DJB2 hash for string dispatch), `showToast()`, `fuzzyLrcMatch()`
- `src/common/service/Singleton.h` — CRTP base template for all major classes

### Module Organization

| Directory | Purpose |
|---|---|
| `src/base/` | Hook macros, SymDB, YPointer, reverse-engineered types |
| `src/common/` | Event bus, Config, Utils, Downloader, Singleton base |
| `src/mod/` | Entry point (Mod.cpp), Engine, version info, OTA updater |
| `src/tweaker/` | Feature flags, DB limit patches, wordbook tweaks, keyboard |
| `src/filemanager/` | File browser, MusicPlayer, VideoPlayer, TextReader, ImageViewer |
| `src/helper/` | AntiEmbs, NetworkSettings, DeveloperSettings, ServiceManager |
| `src/system/` | BatteryInfo, InputDaemon, ScreenManager, AudioDaemon |
| `src/plugin/` | PluginManager, PluginSDK.h (public C ABI), QmlPluginWrapper |
| `src/locker/` | Password-protected page feature |
| `src/recorder/` | Audio recorder |
| `src/rime/` | librime input method |
| `src/tts/` | TTS wrapper (exposed to QML) |
| `src/shell/` | ShellExecutor (sync/async) |
| `src/chatbot/` | AI chatbot backends |
| `src/hitokoto/` | Hitokoto one-liner quotes |
| `src/torch/` | Flashlight control |
| `src/wallpaper/` | Wallpaper manager |
| `src/capture/` | (empty, not yet implemented) |

### QML Integration

Package `com.github.penuniverse` (1.0). Context properties registered in Engine.cpp or constructors: `mod`, `musicPlayer`, `videoPlayer`, `textReader`, `fileManager`, `imageViewer`, `workBookTweaks`, `queryTweaks`, `columnDb`, `batteryInfo`, `locker`, `wallpaperManager`, `mediaSession`. `PageIndex` and `MediaSession` are uncreatable enum types. **All QML must fit 320×170 touchscreen** — prefer Youdao custom components over stock QML.

### QML Resource Workflow

- Edit the source QML files under `resource/models/YDP02X/`. The whole `qml/` tree is listed in
  `.git/info/exclude`, so it is untracked/ignored by git — only the regenerated `qrc_qml.h` is committed. Stage
  that generated header together with the C++ changes.
- `resource/models/YDP02X/qrc_qml.h` is generated output. Never edit or format it manually, including whitespace-only fixes; any manual change will be overwritten by the next resource generation.
- After any QML or bundled resource change, regenerate, build, and deploy with:
  ```sh
  cd scripts
  ./gen_qt_res.sh YDP02X
  cd ..
  xmake build PenModsResources
  adb push ./build/linux/arm64-v8a/release/libPenModsResources.so /userdata/PenMods
  ```
- Pure QML changes do not require rebuilding `PenMods` when the external `libPenModsResources.so` is deployed. `Engine.cpp` prefers the external resource library when it exists.
- Restart the `YoudaoDictPen` process after pushing the resource library so the new `.so` is loaded.
- The generated header may contain formatting artifacts from `rcc`; do not hand-edit the generated file to satisfy formatting or whitespace checks.

### Chatbot Vision Safety

- `src/chatbot/Backend.cpp` uses an asynchronous two-stage flow when a non-vision model delegates image analysis to a vision proxy. Do not reintroduce a nested `QEventLoop` or synchronously wait for `QNetworkReply` on the UI thread.
- All chatbot network replies, including vision-proxy replies, must be tracked in `m_activeReplies`. Cancellation must first remove replies from the active list and disconnect callbacks, then abort and schedule deletion; aborting while iterating the live list can re-enter `finished` handlers and invalidate the iteration.
- Inline `data:` image URLs are request-scoped. Do not persist their Base64 payloads in session history or log complete request bodies. HTTP image URLs may remain in history. Existing sessions are sanitized when loaded.
- Keep a bounded media payload before JSON parsing/serialization (currently 12 MiB), and validate empty/malformed media and API responses before indexing arrays such as `choices`.
- The vision-proxy completion callback is tied to both `m_requestSeq` and the originating session. A cancelled, superseded, or session-switched request must not append messages or launch the second-stage model request.

### Plugin System

Plugins live in `/userdisk/PenMods/plugins/<id>/` with `metadata.json` and optional `.so`. The `.so` must export `init_plugin()` and optionally `init_plugin_with_hook_api(PluginHookAPI*)` or `init_plugin_with_media_api(PluginMediaAPI*)`. `PluginSDK.h` defines the public C ABI. Disabled via `.disabled` marker file. The `mediaSession` context property (src/media/MediaSession) lets a plugin that plays its own audio surface its track in the quick-settings panel; `YQuickMusicPlayer.qml`/`YQuickSettingLayer.qml` prefer an active plugin session over `mediaPlayerManager`.

### Keyboard & Rime

- `mod::KeyBoard` is the `keyBoard` context property and owns `keyboardLayout` (`"native"` / `"compact"`), persisted
  under the `keyboard` section of the config. The QML character keyboards render from row models:
  `YInputTextCharsModelBase` is a `Column` of `Row`s, and the `YInputText{Lower,Upper,Number,Symbol}Chars` pages
  provide the `rows`. Compact mode uses QWERTY (10/9/7) for letters and 7 columns for digits/symbols; native mode
  reproduces the original 5-per-row `Flow` left-aligned layout.
- `rime::Backend` is the `rime` context property and wraps librime maintenance/schema APIs: `schemaListJson()`,
  `selectSchema()`, `redeploy()`, `syncUserData()`. `select_schema` persists the choice to `user.yaml`. Long-running
  maintenance/sync runs on a `QThreadPool` worker and reports back through queued signals — never block the UI
  thread. Each `YInputPage` creates its own `RimeWrapper` session, so a changed layout or schema takes effect on the
  next input-page open.
- Rime data dir is `/userdisk/Music/Rime` (schema + user data + build staging).

## Deployment Paths (on-device)

| Path | Content |
|---|---|
| `/userdata/PenMods/libPenMods.so` | Main mod library |
| `/userdata/PenMods/libPenModsResources.so` | Optional Qt resource overrides |
| `/userdisk/PenMods/plugins/<id>/` | Plugin directory |
| `/userdata/PenModsconfig.json` | User config (note: no slash before `config.json`) |
| `/userdata/applog/DictPen_<timestamp>.log` | `YoudaoDictPen` stdout: spdlog + QML `console.log` |
| `/tmp/rime/` | librime logs |
| `/userdisk/Music/Rime/` | Rime user/shared data dir |

## On-Device UI Verification

The device has no Android `logcat`; `YoudaoDictPen` writes stdout to `/userdata/applog/DictPen_<YYYYMMDD_HHMMSS>.log`.
QML `console.log(...)` lines appear there prefixed with `[qml]`. After restarting the app, grep that file for
`ReferenceError|TypeError|Unable to assign|Cannot read|is not a function` to catch broken QML bindings.

### Screen orientation

- The physical panel is 170×320 (portrait) and Weston runs with `transform=270` (`/etc/xdg/weston/weston.ini`), so
  the logical Qt UI is 320×170 landscape. `weston-screenshooter`, `evtest`, `/dev/uinput`, and `/dev/fb0` exist on
  the device, but `/dev/fb0` is blank and the screenshooter debug protocol is disabled by default.

### Screenshots

`weston-screenshooter` needs Weston's debug protocol. Temporarily:

1. Back up `/etc/init.d/S50launcher` and add `--debug` to the `weston ...` command (~line 96).
2. Reboot (or restart Weston).
3. `adb shell 'cd /tmp && XDG_RUNTIME_DIR=/var/run WAYLAND_DISPLAY=wayland-0 weston-screenshooter'`, then pull the
   resulting `wayland-screenshot-*.png`. It is portrait 170×320; rotate 270° (`PIL: img.rotate(270, expand=True)`)
   to get the 320×170 UI.
4. Restore the original `/etc/init.d/S50launcher` and reboot when done.

### Touch injection (for UI navigation)

The touchscreen `hyn_ts` reports `ABS_MT_POSITION_X 0..170`, `ABS_MT_POSITION_Y 0..320`, `INPUT_PROP_DIRECT`. Create a
`/dev/uinput` device with the same geometry and emit MT events. Map a landscape UI point `(lx, ly)` to raw device
coordinates: `raw_x = ly`, `raw_y = 319 - lx`. Cross-compile the injector for the device:

```sh
zig cc -target aarch64-linux-gnu.2.27 -O2 -o injector injector.c   # glibc target cannot be -static
```

Navigation facts: the home page (`YIndexPage`) is a horizontal scroll list (items 112×102) whose last entry is
`更多设置` → `YSettingPage`; the settings grid is a single vertical column of 58px cells.

### Quick layout checks without touch

`keyBoard.keyboardLayout` is persisted in `/userdata/PenModsconfig.json`. For a no-touch check, edit that JSON
(`"layout": "compact"` / `"native"`) and restart the app; the preloaded `YInputPage` (`main.qml`
`preloadMainKeyboard`) instantiates all four character pages at startup, so the layout is exercised immediately.

## Code Style

- `.clang-format`: LLVM base, 4-space indent, 120 column limit, `PointerAlignment: Left`, `SortIncludes: CaseSensitive`. Run before committing.
- `.clang-tidy`: bugprone, cert, modernize, performance, readability checks.

## Testing

No test suite. Testing via QEMU emulator: build with `--qemu=y`, then `scripts/install.sh` copies the `.so` and launches QEMU + VNC. `EmulatorTweaks.cpp` (compiled only under `PL_QEMU`) stubs `exec()`/`popen()`, redirects DB paths, and blocks audio/recording.

## Key Reference Docs

- `doc/REVERSE_ENGINEERING.md` — hook details, memory layouts, deployment flow
- `doc/HOOK_SYSTEM_ANALYSIS.md` — internal hooks vs PluginHookAPI
- `doc/PLUGIN_HOOK_DEV_GUIDE.md` — guide for external plugins
- `doc/YSOUNDCENTER_ANALYSIS.md` — IDA Pro RE of `YSoundCenter`
- `binary/YoudaoDictPen.i64` — IDA Pro database for the target binary
