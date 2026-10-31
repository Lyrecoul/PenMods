# Rime runtime regression tests

Run from the repository root:

```sh
python3 tests/rime/run.py
```

The runner compiles the production `RimeWrapper`, `RimeRuntime`, and `Backend` sources against recording implementations of the librime C API. It does not initialize a real Rime installation or modify user dictionaries. Native Qt5Core development files and `moc`, spdlog, librime headers, and nlohmann/json headers are required. The Backend integration test uses Qt5Qml/Qt5Quick when available, otherwise Qt6Qml/Qt6Quick and its matching `moc`.

The project’s xmake librime 1.15.0 headers are preferred. Override discovery with `RIME_INCLUDE_DIR=/path/to/include`; `CXX` and the Qt5 `MOC` can also be overridden.

Coverage includes:

- Constructing and resetting unused editors creates no native sessions; first pinyin input creates one session and subsequent input reuses it.
- Separate editors retain independent composition and lifetime. Maintenance invalidates every old generation, including when a newly allocated session reuses an old pointer ID.
- Space, tab, Return, and navigation use X11 keysyms. Unsupported Unicode is returned to the caller rather than truncated to a Latin-1 key.
- Candidate indices select the displayed page; an older API fallback adds the page offset. Invalid indices never reach librime.
- API output is copied and freed before Qt callbacks; reentrant clear, schema changes, or destruction cannot deliver stale commits.
- Schema binding is lazy and selects a changed scheme without rebuilding the keyboard or allocating a second session.
- Real Backend maintenance reserves the API boundary before the worker starts, blocks session operations throughout native join, and reports the native success/failure notification only after join completes.
- Sync and same-schema deployment recover cached editors independently. Schema IDs longer than a fixed stack buffer remain complete.

The device integration check also exercises the actual YDP02X QML keyboard: Back/Home reuse, pinyin and space selection, preedit deletion, ordinary text mixed with pinyin, maintenance rejection, and input after sync/redeployment. Temporary probe code belongs only in a staging resource tree; regenerate and deploy the production resources after testing.
