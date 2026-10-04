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
- `binary/YoudaoDictPen` is byte-identical to the device binary (`/oem/YoudaoDictPen/output/YoudaoDictPen`, launched by
  `/usr/bin/runDictPen`), so `nm -C binary/YoudaoDictPen` reflects exactly the symbols `SymDB` can resolve on-device —
  including non-dynamic `.symtab` entries such as `_ZNK15YSettingManager15isRightHandModeEv`.

### Common Services

- `src/common/Event.h` — Qt signal/slot event bus: `beforeUiInitialization`, `uiCompleted`, `homeButtonPressed`, etc.
- `src/mod/Config` — nlohmann_json backed by `/userdata/PenModsconfig.json` (module dir + `config.json`, no slash); macros `WRITE_CFG` / `UPDATE_CFG`. New keys added to defaults are auto-filled on load.
- `src/common/Utils.h` — `exec()` (shell), `H()` (DJB2 hash for string dispatch), `showToast()`, `fuzzyLrcMatch()`
- `src/common/service/Singleton.h` — CRTP base template for all major classes
- `src/media/MediaSession` — `mediaSession` context property; a plugin that plays its own audio claims it to
  drive the quick settings panel, otherwise the panel reads the host `mediaPlayerManager`. See `### Plugin System`.

### Host Objects Used from PenMods QML

| QML name | Host class | Notes |
|---|---|---|
| `settingManager` | `YSettingManager` | `isRightHandMode`, `lcdBrightness`, `updateVolumeAndLcd()`, … |
| `mediaPlayerManager` | `YMediaPlayerManager` | Host music player: `title`, `playState`, `progress`, `currentPos`, `duration`, `hasLrc`, `onClickedPlay/Pause/Prev/Next()` |
| `YEnum` | host enums | `PLAYING`, `STOPPED`, `PM_AudioPlayer`, `Screen.Width/Height`, … |

Left/right hand mode is a 180° rotation applied to the main window in QML (`settingManager.isRightHandMode ? 0 : 180`);
the host's `YSettingManager::rotate(int)` is an empty stub, so nothing else in the system learns about it. The value is
persisted as `righthandmode` in `/userdata/DictPenData/NeteaseYoudao/YoudaoDictPen.conf`.

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
| `src/media/` | MediaSession: plugin media interface for the quick settings panel |
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

Plugins live in `/userdisk/PenMods/plugins/<id>/` with `metadata.json` and optional `.so`. The `.so` must export `init_plugin()` and optionally `init_plugin_with_hook_api(PluginHookAPI*)` or `init_plugin_with_media_api(PluginMediaAPI*)`. `PluginSDK.h` defines the public C ABI. Disabled via `.disabled` marker file.

- `.so` files are loaded eagerly at startup (`PluginManager` constructor) and the `init_plugin*` entry points run inside
  `BeforeMain`, i.e. before any `QCoreApplication`/UI exists; a plugin's `attach_engine` runs later at
  `beforeUiInitialization`. Plugin QML (`main_qml`) is only loaded on demand by the plugin manager page.
- Plugin QML shares the app's root context, so all context properties (`mediaSession`, `shell`, `mod`, registrations its
  own `attach_engine` made, …) are visible. Importing `com.github.penuniverse` gives access to PenMods enums such as
  `MediaSession.Playing`.
- The `mediaSession` context property (`src/media/MediaSession`) lets a plugin that plays its own audio claim the
  quick-settings music controls: it reports title/lyrics/state/progress and receives play/pause/next/prev/stop. Only one
  session exists at a time; without one the panel reads the host `mediaPlayerManager`, so the host path is unaffected.
  `YQuickMusicPlayer.qml` / `YQuickSettingLayer.qml` implement that fallback, and `PluginManager::unloadSo()` releases a
  plugin's session (and clears its C ABI callbacks) when it is disabled. See `doc/PLUGIN_DEV_GUIDE.md`.
  A session belongs to the plugin id passed to `begin()`: keep `end(pluginId)` (owner-checked, no-op for a revoked
  owner) as the plugin-facing release, and keep the `mRevoking` guard around the `sessionRevoked` emit — a takeover
  must not let the previous owner's `end()` tear down the incoming session. A takeover also fires
  `PluginMediaCallbacks::onSessionRevoked` (an additive, `structSize`-gated field) for the C ABI, since a C plugin's
  handle is invalidated by the takeover and no later status call reaches the panel. Panel visibility is exactly
  `mediaSession.active`, so a `Stopped` session keeps its card (that is the intended resume path); do not auto-expire it.

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

### External Video Player (mpv)

`externalPlayer.open()` (`src/filemanager/player/ExternalPlayer.cpp`) starts `/userdisk/VideoPlayer`, a shell wrapper
around `/userdisk/mpv/bin/mpv` (0.36.0, `vo=wlshm`, config in `/userdisk/mpv/config`). It is a separate Wayland client,
so it never learns about left/right hand mode; the mod passes `--video-rotate=180` in left-hand mode. `--video-rotate`
rotates the video frame only — subtitles/OSD are composited by the VO afterwards and stay unrotated (add `--vf=sub` to
render them into the frame before the autorotate post-filter if that ever needs to change).

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

## Release & OTA Update

PenMods is distributed via OTA from the separate repo `Lyrecoul/penmods-ota`, whose manifest is fetched by
`src/mod/Updater.cpp` (`UH_MAIN_URL = https://cdn.jsdelivr.net/gh/lyrecoul/penmods-ota@main/updates.json`).
Releasing touches three places: the version in this repo, a release tag here, and a new package + tag in the OTA repo.

### Versioning

- The version is the single `set_version('X.Y.Z')` in `xmake.lua`. Nothing else hard-codes it: `src/mod/Version.h.in`
  is templated at configure time into `build/config/Version.h` (`VERSION_MAJOR/MINOR/ALTER`).
- `VERSION_STRING` becomes `X.Y.Z` (`Mod::getVersionStr()`, and the updater's `mSelfVersion`), while `VERSION_CONFIG`
  is the digits concatenated (e.g. `2.3.0` → `230`) and drives config migration in `src/mod/Config.cpp`.
- `BUILD_INFO_STRING` prints a literal `[BUILD_CHANNEL]`: the template only substitutes `VERSION_*`/`MODE`/`GIT_COMMIT`,
  so `VERSION_TO_STRING(BUILD_CHANNEL)` stringifies the macro name instead of its value. Do not rely on it to tell a
  channel/`--target-channel` build apart.

### 1. Bump and tag this repo

```sh
git add xmake.lua
git commit -m "chore(release): bump version to X.Y.Z"
git tag -a VX.Y.Z -m "VX.Y.Z"        # main repo uses an UPPERCASE V
git push origin main
git push origin VX.Y.Z
```

### 2. Build the release artifact

Build **release with no `--force-debug-log`** (that flag forces spdlog to debug level and is only for on-device
debugging; `.github/workflows/build.yaml` likewise omits it). The build must run outside the sandbox — see
`Commands That Must Run Outside the Sandbox`. A stale `/tmp` Zig wrapper is fixed by re-running `xmake f`.

```sh
xmake f --qt="$HOME/PenMods/aarch64-linux-qt-5.15.2" --arch=arm64-v8a --build-platform=YDP02X \
  --target-channel=dev --toolchain=zig -m release -vD \
  --cross=aarch64-linux-gnu.2.27 --force-debug-log=false -c
xmake build PenMods
# verify: strings -a build/linux/arm64-v8a/release/libPenMods.so | grep -E '^X\.Y\.Z$'
```

### 3. Package into the OTA repo

Clone `git@github.com:Lyrecoul/penmods-ota.git`. Its layout:

| Path | Content |
|---|---|
| `updates.json` | Manifest served at jsDelivr `@main` (the URL the updater fetches) |
| `template/_do_update.sh` | On-device installer: remounts `/` rw, copies `libPenMods.so` to `/userdata/PenMods/`, touches `INSTALL_SUCCESSFULLY` |
| `template/libPenMods.so` | Staging copy of the current release `.so` |
| `update_vX.Y.Z.zip` | The published archive: `_do_update.sh` + `libPenMods.so` at the **archive root** (no top-level dir) |

The OTA repo tags with a **lowercase** `v` (`v2.0.0`, `v2.3.0`) — distinct from this repo's `VX.Y.Z`.

`zip` is not installed on the maintainer host; build the archive with Python instead:

```sh
cp build/linux/arm64-v8a/release/libPenMods.so <ota>/template/libPenMods.so
python3 - <<'PY'
import zipfile, os, hashlib
os.chdir('<ota>')
zp = 'update_vX.Y.Z.zip'
with zipfile.ZipFile(zp, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
    for name in ('_do_update.sh', 'libPenMods.so'):
        zi = zipfile.ZipInfo(name)
        zi.external_attr = (0o755 if name.endswith('.sh') else 0o644) << 16
        z.writestr(zi, open(os.path.join('template', name), 'rb').read())
print(os.path.getsize(zp), hashlib.md5(open(zp, 'rb').read()).hexdigest())
PY
```

Then add an entry to `updates.json` (`size`/`md5` are those of the **zip**, not the `.so`):

```json
{
  "version": [X, Y, Z],
  "note": "中文更新说明，显示在设备的升级弹窗中",
  "size": <zip bytes>,
  "download": "https://cdn.jsdelivr.net/gh/lyrecoul/penmods-ota@vX.Y.Z/update_vX.Y.Z.zip",
  "md5": "<zip md5>"
}
```

Manifest rules enforced by `Updater::check()`: every entry the device may be running must be listed, otherwise the
check aborts with `Cannot get self version from mod_versions`; keep older entries alongside new ones. An optional
`"next": [X, Y, Z]` on an entry forces the upgrade path to that version instead of the highest in the list.
`download` must point at a jsDelivr URL whose `md5` matches, and `src/mod/Updater.cpp` validates the md5 before running
`_do_update.sh`.

```sh
git add -A
git commit -m "release: add vX.Y.Z"
git tag -a vX.Y.Z -m "vX.Y.Z"
git push origin main
git push origin vX.Y.Z
```

### 4. Verify through the CDN

jsDelivr caches **versioned paths permanently (~1 year)**, so the manifest and the artifact can lag a push by seconds
to minutes even though GitHub already has them.

```sh
# manifest the device actually fetches; must list the new version and its md5
curl -sL https://cdn.jsdelivr.net/gh/lyrecoul/penmods-ota@main/updates.json
# artifact must exist and match the manifest md5 (note: CDN md5 is of the zip)
curl -sL -o /tmp/p.zip https://cdn.jsdelivr.net/gh/lyrecoul/penmods-ota@vX.Y.Z/update_vX.Y.Z.zip
md5sum /tmp/p.zip
```

If `@main` is stale, force a refresh with the purge API (`https://purge.jsdelivr.net/gh/lyrecoul/penmods-ota@main/updates.json`),
then re-poll; it can take a couple of attempts.

**Never force-move a tag that was already pushed** (`git tag -f` + `git push --force`). Versioned paths are cached
forever, so the tag keeps serving the old artifact and the manifest md5 no longer matches, which makes devices fail with
`ERROR_MD5_CHECK`. A tag's other files re-resolve correctly, but the already-fetched path stays poisoned — the fix is to
publish under a fresh filename (e.g. `update_vX.Y.Z-r1.zip`) and point `download` at it, as done for v2.3.0.

## On-Device UI Verification

The device has no Android `logcat`; `YoudaoDictPen` writes stdout to `/userdata/applog/DictPen_<YYYYMMDD_HHMMSS>.log`.
QML `console.log(...)` lines appear there prefixed with `[qml]`. After restarting the app, grep that file for
`ReferenceError|TypeError|Unable to assign|Cannot read|is not a function` to catch broken QML bindings.

### Screen orientation

- The physical panel is 170×320 (portrait) and Weston runs with `transform=270` (`/etc/xdg/weston/weston.ini`), so
  the logical Qt UI is 320×170 landscape. `weston-screenshooter`, `evtest`, `/dev/uinput`, and `/dev/fb0` exist on
  the device; `/dev/fb0` is blank, and the screenshooter results are the raw portrait framebuffer (see below).

### Screenshots

The tested YDP02X unit already runs Weston with `--debug` (`cat /proc/$(pidof weston)/cmdline`), so the debug protocol
is available and no launcher edit is needed:

```sh
adb shell 'cd /tmp && rm -f wayland-screenshot-*.png && \
  XDG_RUNTIME_DIR=/var/run WAYLAND_DISPLAY=wayland-0 weston-screenshooter'
adb pull /tmp/wayland-screenshot-*.png
```

The PNG is the raw panel framebuffer (portrait 170×320); rotate it 270° (`PIL: img.rotate(270, expand=True)`) to get
the 320×170 UI. If `weston-screenshooter` reports a protocol error, the unit was started without `--debug`: back up
`/etc/init.d/S50launcher`, add `--debug` to the `weston ...` command (~line 96), reboot (or restart Weston), and
restore the file when done.

### Touch injection (for UI navigation)

Driving the UI over ADB needs a `/dev/uinput` device. Two findings on the tested unit:

- Emitting MT-B touch events (`ABS_MT_SLOT` / `ABS_MT_TRACKING_ID` / `ABS_MT_POSITION_X` / `ABS_MT_POSITION_Y` +
  `BTN_TOUCH`, `INPUT_PROP_DIRECT`) makes Weston open the device, but the resulting `wl_touch` events never reach the
  Qt client — the UI does not react.
- An **absolute pointer** device works: `EV_ABS` `ABS_X` (0..170) / `ABS_Y` (0..320) plus `BTN_LEFT`, without
  `INPUT_PROP_DIRECT`. Weston sends pointer motion/button events, which the QML `MouseArea`s handle.

The kernel is 4.4, so `UI_DEV_SETUP` / `UI_ABS_SETUP` do not exist. Use the legacy path: enable the event bits with
`UI_SET_EVBIT` / `UI_SET_KEYBIT` / `UI_SET_ABSBIT`, then `write()` a filled `struct uinput_user_dev` (name,
`id.bustype`, `absmin[]` / `absmax[]`) before `UI_DEV_CREATE`.

Coordinate mapping (same for both device types): compositor UI point `(ux, uy)` → raw device coordinates
`raw_x = uy`, `raw_y = 319 - ux`. A click is one motion report (`ABS_X` / `ABS_Y` + `SYN_REPORT`), then `BTN_LEFT`
1/0; a drag is press, a series of motion reports, release — this scrolls QML `Flickable` / `ListView`s. In left-hand
mode the app rotates its own content 180°, so tap `(319 - ux, 169 - uy)` instead (the compositor transform itself
never changes).

```sh
zig cc -target aarch64-linux-gnu.2.27 -O2 -o injector injector.c   # glibc target cannot be -static
adb push injector /tmp/ && adb shell 'chmod +x /tmp/injector'
adb shell '/tmp/injector tap ui 160 107'              # click a UI point
adb shell '/tmp/injector swipe ui 290 107 20 107'     # drag / scroll
```

Confirm every step with a screenshot. `evtest /dev/input/eventN` prints a device's capabilities and events, which
isolates “device created but the app ignores it” from “wrong coordinates”. Remove `/tmp/injector` when done.

Navigation facts: the home page (`YIndexPage`) is a horizontal scroll list (items 112×102, left margin 10, spacing 8,
`y` 56..158) whose entries are 查词翻译 / AI 助手 / 录音机 / 单词本 / 听力练习 / 历史 / 插件管理 / 更多设置
→ `YSettingPage`; scroll it with a horizontal drag. `听力练习` → `YAudioPage`, whose `文件管理` tile opens the file
browser (`录音文件` and `MUSIC` hold audio, `.mp4` files start mpv through `externalPlayer`). The plugin manager lists
one card per plugin — toggle on the right, `打开` / `卸载` at the bottom of the card — and re-scans when the page
opens. The settings grid is a single vertical column of 58px cells. The quick settings panel opens by dragging down
from the top edge and closes by dragging back up.

### Restart hygiene

Restart the app with `adb shell 'sync; killall YoudaoDictPen'`; a guardian relaunches it within a few seconds. Wait
for `pidof YoudaoDictPen` to be stable (~10 s) before killing it again: `PluginManager` writes a `.loading` marker
before loading each plugin `.so`, so killing the process mid-load leaves the marker behind and the next start treats
that plugin as crashed, auto-disabling it. After a burst of restarts, check and clean up:

```sh
adb shell 'for d in /userdisk/PenMods/plugins/*/; do [ -f "$d/.loading" ] && echo "loading: $d"; done'
adb shell 'rm -f /userdisk/PenMods/plugins/*/.loading /userdisk/PenMods/plugins/<id>/.disabled'
```

Only the newest `/userdata/applog/DictPen_*.log` is kept (older files are removed at startup), the device clock runs
in UTC while the host is UTC+8, and `pidof YoudaoDictPen` normally reports two PIDs.

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
