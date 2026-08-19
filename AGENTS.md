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
- `src/common/Config` — nlohmann_json backed by `/userdata/PenMods/config.json`; macros `WRITE_CFG` / `UPDATE_CFG`
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

Package `com.github.penuniverse` (1.0). Context properties registered in Engine.cpp or constructors: `mod`, `musicPlayer`, `videoPlayer`, `textReader`, `fileManager`, `imageViewer`, `workBookTweaks`, `queryTweaks`, `columnDb`, `batteryInfo`, `locker`, `wallpaperManager`. `PageIndex` is an uncreatable enum type. **All QML must fit 320×170 touchscreen** — prefer Youdao custom components over stock QML.

### QML Resource Workflow

- Edit the source QML files under `resource/models/YDP02X/`.
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

Plugins live in `/userdisk/PenMods/plugins/<id>/` with `metadata.json` and optional `.so`. The `.so` must export `init_plugin()` and optionally `init_plugin_with_hook_api(PluginHookAPI*)`. `PluginSDK.h` defines the public C ABI. Disabled via `.disabled` marker file.

## Deployment Paths (on-device)

| Path | Content |
|---|---|
| `/userdata/PenMods/libPenMods.so` | Main mod library |
| `/userdata/PenMods/libPenModsResources.so` | Optional Qt resource overrides |
| `/userdisk/PenMods/plugins/<id>/` | Plugin directory |
| `/userdisk/PenMods/config.json` | User config |

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
