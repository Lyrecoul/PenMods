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

### CA Certificates (the rootfs has no trust store)

The stock rootfs ships **no working CA store** — usually an empty `/etc/ssl/certs`, sometimes a "full" one built
from materialised symlinks (see below) — so every TLS peer verification fails with the misleading
`unable to get local issuer certificate`; `curl`, the FFmpeg player plugin (static OpenSSL 3) and Qt's network
stack all hit it (see [issue #15](https://github.com/Lyrecoul/PenMods/issues/15); the same rootfs carries a complete
Mozilla bundle at `/usr/share/grpc/roots.pem` that nothing reads).

`src/helper/CaCertificates` (Singleton, constructed in `BeforeMain`, work deferred to `beforeUiInitialization`)
does nothing when the device already has a *usable* store, and otherwise has two paths:

1. **bind mount (preferred, fixes the whole machine).** The embedded bundle is split into one
   `<X509 subject hash>.0` file per certificate in a staging directory (`/tmp/penmods-ca`, falling back to
   `/userdata/PenMods/ca-staging`), which is then `mount(2)`-ed with `MS_BIND` over `/etc/ssl/certs`. Nothing is
   written to the rootfs — the mount just shadows the empty directory and disappears on reboot. Covers this
   process' Qt, the statically linked OpenSSL 3 inside `libffmpegplayerplugin.so`, and every `curl`/`mpv` we exec.
2. **in-process default trust anchors (fallback).** If the mount is impossible (no `CAP_SYS_ADMIN`, private mount
   namespace, missing target directory) the bundle is fed to `QSslConfiguration::setDefaultConfiguration()`, which
   covers `QNetworkAccessManager`, `QSslSocket::connectToHostEncrypted(host, port)` and `QtWebSockets` — this
   process only.

`PENMODS_NO_CA_MOUNT=1` skips path 1, which is how the fallback is exercised on a device.

Rules that cost a debugging session:

- **CApath only honours `<hash>.0` file names.** Dropping a PEM bundle into `/etc/ssl/certs` under any other name is
  silently ignored (verified: plain-named bundle in a CApath dir → `curl` 000, hashed dir → 200). The device has no
  `openssl`/`c_rehash`, so the names are precomputed on the host into `resource/certs/ca-bundle.hashes` (see below).
- **The gate is filesystem-based and must validate the *contents***, *not*
  `QSslConfiguration::systemCaCertificates()`. Qt 5.15's enumeration disagrees with the store the handshake
  actually uses: pointing `SSL_CERT_DIR` at a 3-certificate directory still makes `systemCaCertificates()` report all
  242 system certificates, while the handshake honours the 3. It is also why
  `defaultConfiguration().caCertificates()` reads 0 on a healthy host — the anchors are loaded by OpenSSL's default
  paths at handshake time, not into the configuration.
- **`ls -1 /etc/ssl/certs | wc -l` is not a validity check** (that is issue #15's own diagnostic, and it lies). A
  real YDP02X was found with 438 entries where **every one of the 292 `<hash>.0` files was a 20–38 byte text file
  containing a filename** (`GlobalSign_Root_R46.pem`) — symlinks materialised into text by a copy/zip tool — plus 146
  `<name>.pem` files that CApath never reads. The store was 100% non-functional while looking full. The gate therefore
  requires at least one `<hash>.<n>` entry that is ≥100 bytes and starts with `-----BEGIN CERTIFICATE-----` (or a DER
  `SEQUENCE`, or a usable `/etc/ssl/cert.pem`), and the post-mount check re-counts through the mount.
- **Never iterate `/proc/<pid>/mountinfo` with `atEnd()`/`readLine()`.** procfs reports `st_size == 0`, so
  `QFile::atEnd()` is true immediately and the loop body never runs; that silently disabled "reuse the mount the
  previous app instance made" (symptom: a second `MS_BIND` layer per restart, or the gate taking the "device is fine"
  branch). Read `readAll()` and `split('\n')`.
- **Don't build anything on `SSL_CERT_FILE`/`SSL_CERT_DIR`.** The host Qt 5.15 honours them at handshake time, the
  device's Qt build (also 5.15.2 + OpenSSL 1.1.1i) ignores `SSL_CERT_DIR`: an empty directory there still yields a
  200 while `curl` with the same variable fails. Measured end-to-end on a YDP02X, `QNetworkAccessManager` reports
  `err=6 SSL handshake failed` against the vendor's dead store and `200 err=0` with the mount in place — the mount
  is the lever, not an inherited environment variable.
- **Assigning `caCertificates` replaces the system store instead of adding to it** (verified: injecting a single
  unrelated root makes every handshake fail while the system store is intact). That is why the fallback only runs
  when the device store is unusable.
- Not in `BeforeMain` (or any other pre-`QCoreApplication` hook): `QSslConfiguration::defaultConfiguration()`
  initialises QtNetwork's TLS backend, which needs the instance to resolve plugin/library paths, and caches the
  failure.
- The mount is **namespace-scoped**: anything we spawn inherits it, but "system-wide" only holds if the app shares
  init's mount namespace. On the tested YDP02X every process (init, adbd, the app) is in `mnt:[4026531840]`, so an
  unrelated `adb shell` sees the mount — `readlink /proc/$(pidof YoudaoDictPen)/ns/mnt` vs `/proc/1/ns/mnt` on a new
  unit/model.
- Re-running `_install()` would stack mounts (`MS_BIND` allows it), hence the `once_flag`; `isMounted()` parses
  field 5 of `/proc/self/mountinfo` so a mount left over from the previous app instance is reused, not duplicated.
- A staging rewrite is detected by a SHA-256 marker file (`.penmods-ca`, first 16 hex chars). Because the mount
  points at the *directory*, a new bundle only needs the files rewritten in place — no remount. Consequently
  `clearDirectory()` deletes the *contents* and never the directory itself: unlinking the mount source leaves the
  mount point serving an unlinked inode (looks fine, never updates again).
- On-device verification without any network: a tiny statically-dlopen'd probe that reads a saved chain
  (`openssl s_client -showcerts`) and calls `X509_STORE_set_default_paths()` + `X509_verify_cert()` through the
  device's own `libcrypto.so.1.1` reports `OK` against the mount and `error 20 unable to get local issuer
  certificate` with `SSL_CERT_DIR` pointed at an empty directory. That is the cheapest proof the CApath is live.
- If the OTA installer ever writes `/etc/ssl` itself, remember it would land in the shadow directory while the mount
  is up. The installer currently does not, and does not need to: this mount is the fix.

Bundle maintenance:

- Sources of truth: `resource/certs/ca-bundle.pem` (Mozilla bundle with the `#` label lines stripped) and
  `resource/certs/ca-bundle.hashes` (one `openssl x509 -subject_hash` per certificate, same order). Refresh with
  `scripts/update_ca_bundle.sh [source.pem]` (needs host `openssl`; device sources work too:
  `adb pull /usr/share/grpc/roots.pem` then pass it), then re-run `xmake f` — the `PenMods` target's `on_config`
  writes `$(builddir)/config/CaBundleData.h` (`kCaBundlePem`, `kCaBundleHashes[]`, `kCaBundleCount`, ~182 KB). The
  generator asserts `#hashes == #certificates`; it lives inside `on_config` because xmake 3.x gives the `xmake.lua`
  main chunk neither `io` nor `import`, and target scripts run in a sandbox that cannot see main-chunk globals.
- **Do not pre-compress it.** Raw PEM in `.rodata` costs +181,603 bytes of `libPenMods.so` (+108 KiB in the OTA zip,
  because zip deflates the `.so` anyway — a zlib blob saves the same zip bytes but is incompressible itself) and its
  pages stay file-backed and evictable. A zlib blob would save ~75 KiB of mapping while turning the inflated 182 KiB
  into anonymous heap and needing unpack code — worse for this device.
- Do not trim the bundle to "just the hosts we need": both paths are only used when the device has no trust store at
  all, and the chatbot `baseUrl` is user-supplied, so a partial bundle breaks every unlisted endpoint with the same
  silent failure this section exists to fix.
- Old-style `-subject_hash_old` names are not generated: everything on this device (curl 7.58 with OpenSSL 1.1.1i,
  the player's OpenSSL 3.5) uses the SHA-1 based hash. Add them if a 1.0.x consumer ever appears.

### Fontconfig Font Cache (startup pre-check)

`src/helper/FontCache` (Singleton, constructed in `BeforeMain`, work deferred to `beforeUiInitialization` like
`CaCertificates`) makes sure the device can actually hand out fonts, and logs its decision to
`/userdata/applog/DictPen_*.log` (`grep FontCache` on the newest file). It exists because the stock image ships no fontconfig cache: a cold `fc-list` on
the tested YDP02X takes **39.9 s** to scan the 144 fonts in `/usr/lib/fonts` and write `/var/cache/fontconfig`, while
a warm one takes **40 ms** — and the things that read fonts (`mpv`'s libass subtitles, Qt's own fontconfig backend,
any child process we spawn) hit that cost in the middle of the UI.

Two steps, in this order:

1. **Font directory.** `/etc/fonts/fonts.conf` lists exactly one `<dir>/usr/share/fonts</dir>`, but the fonts are
   installed in `/usr/lib/fonts` and the stock image ships **no** symlink between them: on a clean unit fontconfig —
   and resvg-js/fontdb — therefore sees no font at all, and the 40 s scan above only happens once the directory is
   reachable (the test unit got it from a hand-made symlink, which is also why the measurements exist). If
   `/usr/share/fonts` is missing, dangling, pointing elsewhere, or an *empty* directory, it is replaced with
   `-> /usr/lib/fonts`. A directory that already holds files but no font is left untouched (and then the cache build
   is skipped) — that is somebody else's layout, and scanning it would only produce a valid-looking cache containing
   no fonts.
2. **Cache manifest.** `fc-cache` runs only when no usable `<hash>-<machine>.cache-<n>` exists in `/var/cache/fontconfig`
   (or the fonts directory is newer than the newest cache file, i.e. fonts were added later). It runs **without `-f`**,
   so an up-to-date cache costs ~37 ms instead of a full rescan. The run is asynchronous (`QProcess` + `finished`) and
   took **43.4 s** on a real cold cache; the UI is fully usable meanwhile.

Rules that cost a debugging session:

- A cache entry counts only if it is >= 1 KiB **and** starts with the fontconfig magic `04 FC 02 FC`. The size gate
  matters: a cache written while the font directory was still empty is small but otherwise perfectly valid, and
  accepting it would report "cache ready" on a machine whose fonts fontconfig cannot see.
- The gate is filesystem-based for the same reason as the CA store gate: pointing at an enumerating API
  (`fc-list`) is itself the 40 s cost this check exists to avoid.
- `/var/cache/fontconfig` is the compiled-in location (`fc-cache -v` only ever cleans that one); `$XDG_CACHE_HOME/fontconfig`
  and `$HOME/.cache/fontconfig` are only scanned to avoid a pointless rebuild.
- Writing the symlink may need the rootfs remounted rw; it is retried once after `mount -o remount,rw /` on
  `EROFS`/`EACCES`/`EPERM`. `Mod::onUiCompleted()` remounts anyway, but that is later than this check.
- `PENMODS_NO_FONT_CACHE=1` skips step 2 (the symlink repair still happens), which is how a device with a deliberately
  broken cache is exercised.
- The MathJax service does **not** depend on the cache: resvg-js/fontdb parses `fonts.conf` itself. It does depend on
  step 1 — that symlink is the only way `/usr/share/fonts` becomes reachable on a stock unit, so without it the
  renderer silently loses CJK.

### MathJax Formula Rendering Service

`mathjax-server/` is the local service that renders LaTeX to PNG for the AI assistant (Node + MathJax 3 +
resvg-js); `build.sh` bundles it into a single-file aarch64 executable with esbuild + pkg. It is **not included
in the OTA package** — the binary has to be shipped to users separately (the client starts it from
`chatbot.mathServerPath`, launching it when the assistant page opens and no process is detected).

- Output size: always rasterize at the fixed `REF_EM` base size and **only shrink, never enlarge**
  (`MAX_RENDER_WIDTH` caps the width; height is unlimited and the client scrolls). Scaling a small formula up
  to a "minimum readable height" made the font size vary several-fold within a single answer.
- Wrapping: a formula that does not fit is broken into several images at one uniform size instead of being
  shrunk as a whole — first stacked inside `\boxed{…}` (`\begin{array}{l}`), then split at top-level
  operators/arrows/full-width punctuation, and finally hard-split inside `\text{…}` with the macros re-closed
  at the cut. The break tables are `BREAK_BEFORE_COMMANDS` / `BREAK_AFTER_COMMANDS` and the per-formula line
  budget is `MAX_WRAP_LINES`. If every break attempt fails, rasterization shrinks the single whole formula to
  the width cap — that is a last resort, not the normal path.
- **CJK depends on a reachable font; system fonts are the default.** MathJax emits
  `<text font-family="serif">` for characters its own fonts lack (Chinese and friends); with no CJK-capable face
  reachable, resvg draws nothing for those runs and the remaining glyphs collapse together. The tested YDP02X
  ships CJK system fonts in `/usr/lib/fonts` (`NotoSansSC-*.otf`, `HYQiHei-*.ttf`), but `fonts.conf` only points at
  `/usr/share/fonts`, which the stock image does not create — without that symlink resvg renders no CJK at all.
  `FontCache` (see `### Fontconfig Font Cache`) creates it at startup, and resvg-js 2.6.2 then reaches the fonts
  through fontdb's pure-Rust fontconfig support (it parses `/etc/fonts/fonts.conf` itself, follows the symlink, and
  needs no fontconfig cache; verified on-device by rendering `\text{中文}` through a server build that did no font
  work at all). The service therefore renders with `loadSystemFonts: true` and `defaultFontFamily: "Noto Sans SC"`
  and ships **no** font file. An explicit font is used only when one is actually supplied — `MATHJAX_FONT=<path>`, or a file named `wqy-microhei.ttc` in
  `/userdata/PenMods` or `/userdisk/PenMods` — for units that genuinely lack CJK fonts; that mode copies the file
  atomically into a writable dir (`MATHJAX_FONT_DIR` → temp dir → `/userdata/PenMods` → `/userdisk/PenMods`),
  reads the real family from its name table, rewrites only the `<text>` font-family and sets
  `loadSystemFonts: false`. Both paths are checked by a startup probe that renders `汉` and counts ink, so
  `cjkFont` reports a *measured* result instead of an assumption; if neither path inks, only Chinese is lost and
  every other formula still renders. The probe only counts ink, so a font that lacks CJK but defines `.notdef`
  renders tofu boxes and still passes. In resvg-js 2.6.2 the `serifFamily` / `sansSerifFamily` / `cursiveFamily` /
  `fantasyFamily` / `monospaceFamily` options are parsed but never read — only `defaultFontFamily` matters.
- `/health` returns `renderer` (currently 3), `cjkFont`, `cjkFontPath` (only in explicit-font mode, otherwise
  null), `cjkFontFamily`, `cjkFontSource` (`system` | `file` | `none`) and `cjkFontError`; `/render` returns only
  `renderer`, `cjkFont` and `cjkFontFamily`. The client probes `/health`, treats `renderer >= 2` as modern, and
  warns once each when the renderer is too old or `cjkFont === false`. **Bump `RENDERER_VERSION`** whenever
  renderer behaviour changes (3 = system fonts by default, no bundled font).
- The client paces itself at roughly 8 requests/second and de-duplicates concurrent requests for the same
  formula (`MathCache.js`, which also owns the 1200-entry cache); the 429 exponential backoff lives in
  `MathBubble.qml`. An old server lets through only 10 requests/second per IP, which used to reject the second
  half of the formulas in a long answer. Server-side limiting is `RATE_LIMIT_PER_SECOND`.
- Verify with: `echo '{"latex":"\\boxed{\\text{中文}}","display":true,"screenWidth":320}' | curl -s -X POST
  http://127.0.0.1:3000/render -H 'Content-Type: application/json' -d @-`, then save the returned base64 as a
  PNG and check it by eye (the dimensions alone do not tell you whether the Chinese glyphs are missing).
- Smoke-testing on the dev host needs the x86_64 binding, which this repo does not ship. Install it **without
  touching the arm64 one**, e.g. `npm install --prefix /tmp/resvgx64 @resvg/resvg-js-linux-x64-gnu@2.6.2` and run
  with `NODE_PATH=/tmp/resvgx64/node_modules node index.js`. Do **not** run a bare
  `npm install @resvg/resvg-js-linux-x64-gnu` in `mathjax-server/`: npm treats the arm64 package as
  platform-incompatible and prunes it, and `build.sh` then produces a ~5 MB smaller binary that dies on the device
  with `Cannot find module '@resvg/resvg-js-linux-arm64-gnu'`. Restore it with
  `npm pack @resvg/resvg-js-linux-arm64-gnu@2.6.2` + `tar -xzf … --strip-components=1 -C
  node_modules/@resvg/resvg-js-linux-arm64-gnu`, and always start the freshly built binary once before pushing it
  to a device.

### Host Objects Used from PenMods QML

| QML name | Host class | Notes |
|---|---|---|
| `settingManager` | `YSettingManager` | `isRightHandMode`, `lcdBrightness`, `updateVolumeAndLcd()`, … |
| `mediaPlayerManager` | `YMediaPlayerManager` | Host music player: `title`, `playState`, `progress`, `currentPos`, `duration`, `hasLrc`, `onClickedPlay/Pause/Prev/Next()` |
| `YEnum` | host enums | `PLAYING`, `STOPPED`, `PM_AudioPlayer`, `Screen.Width/Height`, … |

Left/right hand mode is a 180° rotation applied to the main window in QML (`settingManager.isRightHandMode ? 0 : 180`);
the host's `YSettingManager::rotate(int)` is an empty stub, so nothing else in the system learns about it. The value is
persisted as `righthandmode` in `/userdata/DictPenData/NeteaseYoudao/YoudaoDictPen.conf`.

### Music Player Temporary `.mp3` Links

`MusicPlayer::_play` gives the host a symlink named `<original base name>.mp3` (in
`/userdata/PenMods/audiolinks/<sha1(path)[0..8]>/`) for every non-mp3 file, because the host picks its decode path from
the suffix. The link is load bearing for the whole playback session — not just for the first `playAudio()`:
`YMediaPlayerManager::onClickedPlay()` starts with `FileUtils::fileExists(entity->localFile())` and, when it fails,
runs `fileNotExist()` + `onResetPlayer()` without touching the original file. The visible symptom is a player that
ignores the play button **forever** after a pause (`playState` never leaves `PAUSED`), which is what "暂停后无法再次播放"
means.

Rules:

- **Never delete the current link while the session may still use it.** Two deletions caused that bug: leaving the
  player page used to call `MusicPlayer::cleanupTempSymlinks()` from the `isInPlayerCenterPageChanged` hook (removed),
  and `_play` deleted the previous link before creating the new one (so a paused song plus any later `_play` — a
  song switch, or two interleaved `_play` calls — could unlink the live path). Links are now kept for the process
  lifetime and purged once in the `MusicPlayer` constructor.
- `onClickedPlay` is hooked to `recoverIfPlayFailed()`: the repair path recreates the link from the recorded
  `mTempAudioTarget` and retries the play synchronously (log: `已重建音频临时软链接` / `临时软链接丢失已修复`). Restart
  the whole song only when the host's entity really was wiped (`hostLocalFile()` empty).
- Link names must keep the original base name (the host derives the `.lrc` name from `QFileInfo(localFile).fileName()`
  minus 4 chars); same-named songs in different folders are separated by the hash directory. Never switch to
  `QTemporaryFile`-style names or hash suffixes in the file name itself.
- `/tmp` is a tmpfs other processes can clear — the links live under the module directory for that reason.
- `mCurrentPlaying.mFile` is now assigned (`setPlaying(idx, file)`); the "same file, don't restart" early return also
  requires `isHostPlaying()`, so tapping a paused song still restarts it instead of becoming a no-op.

### Module Organization

| Directory | Purpose |
|---|---|
| `src/base/` | Hook macros, SymDB, YPointer, reverse-engineered types |
| `src/common/` | Event bus, Config, Utils, Downloader, Singleton base |
| `src/mod/` | Entry point (Mod.cpp), Engine, version info, OTA updater |
| `src/tweaker/` | Feature flags, DB limit patches, wordbook tweaks, keyboard |
| `src/dict/` | Custom dictionaries (`customDict`): scans `/userdisk/PenMods/dicts/`, queries user-made `.dat` containers through the vendor reader and injects the hit into the main result page as its own section (see `doc/DICT_FORMAT_ANALYSIS.md`) |
| `src/filemanager/` | File browser, MusicPlayer, VideoPlayer, TextReader, ImageViewer |
| `external/ffmpeg-player/` | Vendored upstream QML video player plugin (in-scene-graph rendering, ASS/LRC subtitles, MPP hard decode behind a runtime gate) |
| `src/helper/` | AntiEmbs, CaCertificates, FontCache, NetworkSettings, DeveloperSettings, ServiceManager |
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

Package `com.github.penuniverse` (1.0). Context properties registered in Engine.cpp or constructors: `mod`, `musicPlayer`, `videoPlayer`, `textReader`, `fileManager`, `imageViewer`, `workBookTweaks`, `queryTweaks`, `columnDb`, `batteryInfo`, `locker`, `wallpaperManager`, `mediaSession`,
`customDict`. `PageIndex` and `MediaSession` are uncreatable enum types. **All QML must fit 320×170 touchscreen** — prefer Youdao custom components over stock QML.

### QML Resource Workflow

- Edit the source QML files under `resource/models/YDP02X/`. The whole `qml/` tree is listed in
  `.git/info/exclude`, so it is untracked/ignored by git — only the regenerated `qrc_qml.h` is committed. Stage
  that generated header together with the C++ changes.
- A clone therefore has no editable sources: `scripts/unpack_qt_res.sh [YDP02X]` parses the committed header and writes the
  tree back out (in place, or to `-o DIR`), so a fresh checkout can be worked on. It restores each file's mtime from the
  header too, because `rcc` embeds mtimes; `--check` compares the tree against the header (exit 1 when out of sync) and
  `-f` overwrites files whose contents differ. Note that it cannot reproduce directory entry order, which `rcc` follows,
  so a regenerated header may order entries differently even when every file matches.
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
  handle is invalidated by the takeover and no later status call reaches the panel.
- The C ABI session handle is a monotonically increasing integer token encoded in the `void*`, never a pointer: a
  takeover used to `delete`/`new` the handle object, the allocator handed back the same address, and the stale handle
  from the revoked plugin still passed the liveness check. Keep tokens non-reusable.
- **All C ABI state lives in the function-local `state()` singleton in `MediaSession.cpp`.** `init_plugin*` runs inside
  `BeforeMain()`, which can execute before this library's own static initializers; a file-scope `QString` (or any
  non-trivially-constructed object) is still null there and dereferences to a crash. Never move that state back to
  namespace scope. Panel visibility is exactly
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

### Embedded FFmpeg Player (QML plugin)

`external/ffmpeg-player/` vendors [`Haikure/ffmpeg-player`](https://github.com/Haikure/ffmpeg-player) v1.0.3
(GPL-3.0) — a Qt 5 QML video player that renders **into the host scene graph** (custom `QSGNode`, static FFmpeg
3.4.8 + libass + OpenSSL 3 + libxml2), so the picture follows `YMainWindow.rotation` and PenMods controls can sit on
top of it. `xmake build ffmpegplayerplugin` produces `build/<plat>/<arch>/<mode>/qml/FFmpegPlayer/`;
`scripts/deploy_ffmpeg_player.sh` pushes it to `/userdata/PenMods/qml/FFmpegPlayer/` (tmp + rename, then restarts
the app). `src/mod/Engine.cpp` adds `/userdata/PenMods/qml` as an import path before the main QML loads, so the page
`resource/models/YDP02X/qml/audiopages/ExternalPlayer.qml` can `import FFmpegPlayer 1.0`;
`ExternalPlayer::open()` skips the mpv process while that module exists (mpv stays the fallback). Full analysis and
device measurements: `doc/FFMPEG_PLAYER_ANALYSIS.md`.

- **Hard-decode gate — do not remove.** The stock device tree disables the VPU (`/proc/device-tree/vpu_combo` =
  `disabled`), and the vendor MPP userspace still reports `mpp_check_support_format() == 0`; `mpp_init()` then
  dereferences NULL and **kills the whole process** (measured: `unhandled level 2 translation fault (11) at 0x00000000`,
  `PC is at 0x0`). `Mod::BeforeMain()` therefore sets `FFPLAYER_HWDEC=0` whenever `/dev/vpu_service` is missing. With a
  VPU-enabled kernel/DTB the same build uses `h264_rkmpp`/`hevc_rkmpp` and `hardwareDecoding` becomes true. Any process
  that loads this plugin outside the mod (e.g. `qmlscene` smoke tests) must set `FFPLAYER_HWDEC=0` itself — note the
  device's `adbd` environment may already carry it (`cat /proc/$(pidof adbd)/environ`).
- **VPU experimental switch.** `src/tweaker/VpuUnlock.*` (context property `vpuUnlock`, switch under
  更多设置 → 系统微调 → 实验性功能) turns the VPU patch on and off from the UI: it backs the factory boot image up to
  `/userdisk/PenMods/boot_stock.img` once, rewrites `vpu_combo/status` **inside the DTB in the boot image**
  (`/userdisk/PenMods/boot_vpu.img`), flashes the active `boot_<slot>` partition with a read-back md5 check, and
  reboots; turning it off restores the backup and reboots. Only the property *value* is rewritten, never its length —
  shortening `disabled\0` to 5 bytes would move the next token and corrupt the blob. Node matching must not assume a
  depth (`vpu_combo` sits under the root). The player page is `qml/audiopages/ExternalPlayer.qml` (tap reveals the OSD when it is
  hidden and toggles play/pause when it is shown; long press = 2x boost; horizontal drag = seek with preview;
  `-10s/+10s`/speed/subtitle chips; auto `.ass`/`.lrc` next to the video; plugin-drawn progress bar). The back button
  lives inside the OSD so the picture stays clean while the OSD is hidden.
- Build inputs: the vendored subproject is **skipped unless `PENMODS_WITH_PLAYER=1`** is exported (`xmake build` then
  builds the mod only, which is what CI does — the player's dependency chain needs ~40 min of from-source FFmpeg/OpenSSL3/
  libass and a container with `unzip`/`perl`). `PENMODS_FFMPEG_PLAYER_SW=1` additionally builds a software-only variant
  (or simply remove `external/dictpen/librockchip_mpp.so.1` + `libdrm.so.2`); the upstream-supplied
  `ffplayer-openssl3` package is pinned to `3.5.6` (xmake-repo has no `3.5.7`) and `network_revision` is bumped
  to force ffmpeg package rebuilds. `external/dictpen/*.so`
  are **symlinks** in git (a zip download turns them into text files and breaks `-lrockchip_mpp`). On this dev machine
  the xmake-repo `python` recipe needed `--enable-optimizations` removed (PGO test flake) because `harfbuzz` builds
  through `meson`.
- The vendored `src/` matches upstream v1.0.3. The adaptive N×N downsampling code in `src/VideoRenderNode.hpp`
  retains historical `// PENMODS PATCH` comments but is already upstream. Zoom disables multi-tap downsampling
  while keeping bilinear filtering (`downsamplingEnabled: !zoomMode`, `smooth: true`); `smooth` independently
  selects linear or nearest filtering for GL and CPU rendering. Preserve the PenMods-specific build integration.
  After any `src/` change, do a **full** plugin rebuild — an incremental build can mix v-old/v-new
  objects and abort the host at `VideoPlayer` construction (see `external/ffmpeg-player/UPSTREAM.md`).

### Touch Calibration (udev rule)

`mod::TouchCalibration` (`src/tweaker/TouchCalibration.*`, context property `touchCalibration`, switch in
更多设置 → 系统微调 → 实验性功能) writes `/etc/udev/rules.d/99-penmods-touch-calibration.rules`:

```
ATTRS{name}=="hyn_ts|ft3427_ts", ENV{LIBINPUT_CALIBRATION_MATRIX}="1 0 0 0 1 0.003125"
```

The device runs udevd (220) and Weston 8.0.0 on libinput 10.13, which reads `LIBINPUT_CALIBRATION_MATRIX` when it opens
the touchscreen — this is the mechanism Weston's own `weston-touch-calibrator` (also installed) prints for persistence.
`udevd` only parses rules at boot and libinput only reads the property when the device is opened, hence the switch
reboots the machine; the app is on the rootfs (`/dev/root / ext4 rw`, and `Mod::onUiCompleted()` remounts it rw anyway),
so the rule itself survives reboots but not a firmware reflash.

- The touch IC is **not** the same on every unit: the tested YDP02X reports `hyn_ts` (i2c-1 @0x1a, `/dev/input/event2`),
  while another YDP02X uses `ft3427_ts`; YDPG3/YDP03X names are unknown. Add names to the alternation in
  `RULE_DEVICES` — a rule whose `ATTRS{name}` matches nothing is silently inert.
- The matrix compensates one pixel: `ABS_MT_POSITION_X/Y` is `0..170`/`0..320` on a 170x320 panel, so the outermost
  row normalizes onto the pixel just outside the window and QML never sees the press; +1/320 pulls it back in.
  Verify one toggle with `udevadm test /sys/class/input/event2 | grep LIBINPUT_CALIBRATION_MATRIX=` (no reboot needed)
  and the applied value afterwards with `libinput-list-devices` (the `Calibration:` line).
- `weston-touch-calibrator` needs Weston started with `--debug` (`/etc/init.d/S50launcher` ~line 96, restore when done),
  which also re-enables `weston-screenshooter`.

## Experimental OCR Backend (PP-OCRv5)

`tools/ppocr-backend/` holds an **opt-in, off-by-default** replacement for the pen's line
recognition stage: PaddleOCR's PP-OCRv5 mobile recognizer on ncnn. Read
`doc/PPOCR_BACKEND_ANALYSIS.md` for the reverse engineering and measurements, and the
tool's `README.md` for the how-to. Nothing in `src/` (and therefore nothing in the xmake
build) depends on it.

- The vendor OCR lives in `/oem/YoudaoDictPen/output/aarch64_libs/libyocr.so` (ncnn + Eigen +
an FST decoder) with models in `../ocr_model/`. Detection and line segmentation are **not** there —
they are inside `libYoudaoStitch.so` (`seg_middle_line`/`SegLine`), so replacing `libyocr.so`
leaves the whole scan pipeline otherwise intact. The shipped app never enables the 150 MB FST
language model, so the vendor path is greedy decoding and a greedy-vs-greedy comparison is fair.
- `libYoudaoStitch.so` imports exactly six symbols from `libyocr.so`; the shim re-implements
them. Do **not** try to hook `yocr_recognize(cv::Mat const&)` from PenMods: its `std::string` /
`cv::Mat` parameters are libstdc++/OpenCV C++ ABI, while libPenMods.so is libc++ (zig).
- The shim must be built with an `aarch64-linux-gnu-g++` whose sysroot is glibc 2.27 and which
ships `omp.h`/`libgomp`, and must link libstdc++ **dynamically**. zig cannot be used: it defines
`_OPENMP` but has no OpenMP runtime, so every ncnn layer loses its `#pragma omp parallel for`
and the model runs single-threaded (measured 3.8x slower).
- Invariants baked into the shim, each of which cost a debugging session: OpenCV encodes the
channel count as `cn-1`; a crop can be a non-continuous ROI so rows must be walked with
`step.p[0]`; the network has one output class beyond the dictionary (the space) and dropping it
concatenates English words; and a crop's height is already ~48 before we see it, so lowering
`target_h` is a second resample that costs dense-Chinese accuracy (knob: `/userdisk/ppocr_target_h`).
- The backend is toggled either from the shell (`penmods-ppocr.sh`, or `scripts/deploy.sh` while
developing) or from the mod's settings page (**更多设置 → 系统调整 → 实验性功能**, backed by
`src/tweaker/OcrBackend.cpp` / context property `ocrBackend`). Both read the same three files
(`/userdisk/ppocr_backend`, `/userdisk/Qtlib/libyocr.so`, `/userdisk/ppocr_target_h`) — keep it
that way instead of adding a config key that can drift out of sync with reality. Enabling or
disabling has to restart the app; the accuracy/speed knob does not.
- Deployment shadows the vendor library via the app's own `LD_LIBRARY_PATH`
(`/userdisk/Qtlib/` is its first entry and does not otherwise exist). `scripts/deploy.sh on|off`
handles install/revert, including a loader pre-flight (`LD_TRACE_LOADED_OBJECTS`) before the app
is restarted. Never `adb push` over that file while the app runs — see the section below.

## Device Deployment & Recovery

### Deploying a new `libPenMods.so` / plugin `.so`

`libPenMods.so` reaches the app through a `NEEDED` entry on `YoudaoDictPen` (see `/userdisk/penmods/patch.sh`), so a
running app has it mapped. Overwriting it in place (`adb push` directly onto `/userdata/PenMods/libPenMods.so`) mutates
pages of a live mapping and makes the app execute mismatched code — it segfaults, the guardian restarts it, and the
firmware escalates a few crashes in a row into a `misc` `boot-recovery` reboot, leaving the device in the Rockchip
recovery ramdisk. Push to a scratch name and rename instead, which leaves the running process on the old inode:

```sh
adb push build/linux/arm64-v8a/release/libPenMods.so /userdata/PenMods/libPenMods.tmp.so
adb shell 'mv -f /userdata/PenMods/libPenMods.tmp.so /userdata/PenMods/libPenMods.so'
```

The same applies to an already-loaded plugin `.so` under `/userdisk/PenMods/plugins/<id>/`. After the rename, restart
the app (`killall YoudaoDictPen`) to pick the new file up.

Recovering a device already stuck there needs `adb shell auth`, a `misc` reflash and a reboot — see
[Recovering from a crash loop → recovery](#recovering-from-a-crash-loop--recovery).

### Recovering from a crash loop → recovery

Symptom: ADB reconnects with the device in a ramdisk root (`rootfs on / type rootfs`, only `/userdata` + `/userdisk`
mounted) and every `adb shell` answers `login with "adb shell auth" to continue.`. `YoudaoDictPen` is not running and
`/userdata/syslog/messages` holds repeated `unhandled level 2 translation fault ... Comm: YoudaoDictPen` dumps. The
firmware wrote `boot-recovery` + `recovery --recovery_online` into the BCB at offset `0x4000` of `/dev/block/by-name/misc`.

`system_a` itself is fine — do not "repair" it. Mount it read-only only to confirm, then run:

```sh
adb shell auth                      # password: CherryYoudao
# remove whatever caused the crash first, e.g. the offending plugin dir
adb shell 'rm -rf /userdisk/PenMods/plugins/<id>'
# restore a good library from a copy you kept as /userdata/PenMods/libPenMods.known_good.so
adb shell 'cp -f /userdata/PenMods/libPenMods.known_good.so /userdata/PenMods/libPenMods.so'
# clear the boot marker with the known-good misc image kept on the device
adb shell 'dd if=/userdisk/Music/OTHERS/misc.img of=/dev/block/by-name/misc bs=4096 conv=fsync; sync'
adb shell 'md5sum /dev/block/by-name/misc'   # must be 50e0f9d40f6912cbff7ac89b7862ab4c
adb shell 'reboot'
```

`loadSo()` keeps the `.loading` marker until `init_plugin`, `attach_engine`, `init_plugin_with_hook_api` and
`init_plugin_with_media_api` have all returned, so a crash in any of them is covered by the crash self-heal (the plugin
is auto-disabled on the next start). Keep that ordering: removing the marker earlier turns any init crash into an
infinite crash loop, which the firmware escalates into a `misc` `boot-recovery` reboot.

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
| `template/_do_update.sh` | On-device installer: remounts `/` rw, copies `libPenMods.so` to `/userdata/PenMods/`, then `libPenModsResources.so` when the package carries it, touches `INSTALL_SUCCESSFULLY` |
| `template/libPenMods.so` | Staging copy of the current release `.so` |
| `template/libPenModsResources.so` | Staging copy of the QML resource library (shipped since v2.4.0 — see below) |
| `update_vX.Y.Z.zip` | The published archive: `_do_update.sh` + the two `.so` files at the **archive root** (no top-level dir) |

The OTA repo tags with a **lowercase** `v` (`v2.0.0`, `v2.3.0`) — distinct from this repo's `VX.Y.Z`.

`zip` is not installed on the maintainer host; build the archive with Python instead:

```sh
cp build/linux/arm64-v8a/release/libPenMods.so <ota>/template/libPenMods.so
cp build/linux/arm64-v8a/release/libPenModsResources.so <ota>/template/libPenModsResources.so
python3 - <<'PY'
import zipfile, os, hashlib
os.chdir('<ota>')
zp = 'update_vX.Y.Z.zip'
with zipfile.ZipFile(zp, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
    for name in ('_do_update.sh', 'libPenMods.so', 'libPenModsResources.so'):
        zi = zipfile.ZipInfo(name)
        # writestr(ZipInfo) 只看条目自己的 compress_type，ZipFile(..., ZIP_DEFLATED) 对它无效 ——
        # 漏掉这一行整个包会变成 Stored：v2.4.0 与第一次 v2.5.0 的包都是 10.5 MB，正确的是 4.7 MB。
        zi.compress_type = zipfile.ZIP_DEFLATED
        zi.external_attr = (0o755 if name.endswith('.sh') else 0o644) << 16
        z.writestr(zi, open(os.path.join('template', name), 'rb').read())
print(os.path.getsize(zp), hashlib.md5(open(zp, 'rb').read()).hexdigest())
PY
unzip -v update_vX.Y.Z.zip | sed -n '3,6p'   # 每个条目都应是 Defl:N，不能是 Stored
```

Since v2.4.0 the package also carries `libPenModsResources.so`: a device that already has that file in
`/userdata/PenMods/` keeps using it in preference to the copy compiled into `libPenMods.so` (`Engine.cpp`), so updating
only the main library would pair new C++ with stale QML. `_do_update.sh` copies it when present, which also turns a
device that never had it into one that loads resources externally — same content either way.

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

### jsDelivr refresh discipline (skip this and every release stalls)

- The device fetches `@main/updates.json` and jsDelivr serves it with `cache-control: public,
  max-age=604800, s-maxage=43200` — a **12 hour CDN TTL, cached independently per edge node**. So
  after a release, "one device sees the new version while another does not" is expected behaviour
  and not a failed publish.
- `purge.jsdelivr.net` is the only way to make it immediate, and it is **rate limited per path**:
  after a few calls it answers `"throttled": true` with a `throttlingReset` countdown (a ~3300 s
  rolling window). **Changing IP does not reset it** — the limit follows the path, not the caller
  (measured: five purges of the same path, after which every purge inside the window is refused).
- Therefore purge **exactly once per release**, right after the push. If it is already throttled,
  wait for the window or for the 12 h TTL.
- Judge a release from GitHub and the **tagged artifact URL** (`@vX.Y.Z/update_*.zip` is a brand new
  path, immune to any cache, and its md5 must match the manifest) — never from the `@main` response.
- A query string (`?cb=123`) does **not** bust the jsDelivr cache; do not rely on it.
- (Alternative: host `updates.json` on `raw.githubusercontent.com`, whose TTL is only 5 minutes, but
  its reachability from mainland China is uncertain; doing it properly means giving `Updater`
  multiple URL fallbacks. Not done for now.)

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
- `doc/DICT_FORMAT_ANALYSIS.md` — offline dictionary (`localdict/*.dat`) container format, how the engine
  resolves/loads dictionaries, and how far dictionary extension can go (`tools/dict-probe/` holds the
  reader/writer probe and the vendor-reader validation harnesses)
- `doc/CUSTOM_DICT_GUIDE.md` — step-by-step guide for authors: write entries (JSONL/TSV) → build a `.dat`
  → validate → install into `/userdisk/PenMods/dicts/` → what shows up in the result page
- `binary/YoudaoDictPen.i64` — IDA Pro database for the target binary
