# Chatbot session storage and shared UI colors

## Session storage (#23)

`ChatBot::saveSessions()` debounces updates for 800 ms. A dedicated single-worker
`QThreadPool` serializes and commits snapshots outside the UI thread. Only one
snapshot can be in flight; changes made meanwhile schedule a subsequent save.
Snapshots use Qt's implicitly shared containers. The worker retains its last
successful snapshot and compares message storage identity plus session metadata,
so unchanged histories are not serialized again.

The existing module path convention produces `/userdata/PenModssessions.json`.
The new store is `/userdata/PenModssessions.json.d/`, containing:

- `index.json`: the active session id and referenced session filenames.
- `<sha256>.json`: one compact JSON document per session, named by its contents.

Each document is written with `QSaveFile`. Session files are immutable and are
committed before the index. Only after the index commits are unreferenced files
removed. A failed save retains the previous cache and retries. Normal shutdown
waits for the worker and commits the latest snapshot. Abrupt termination can lose
changes within the debounce window or an unfinished save, but cannot expose a
partially written JSON file.

When no index exists, startup reads the legacy file and schedules migration. The
legacy file remains untouched as a migration backup; once an index exists it is
the source of truth. Rolling back to an older PenMods version will read that
legacy snapshot, not subsequent edits. To back up current history, copy the whole
`.json.d` directory while the app is stopped, along with the attachment directories.

If an index or a referenced document cannot be read, saving is disabled for that
process to protect the original store. Diagnose the startup warning before
repairing the store; do not delete the index to recover automatically, since the
legacy snapshot may be older.

Startup still reads all sessions synchronously, one file at a time. This change
removes repeated full-history saves from the UI thread; lazy loading at startup
is a separate improvement. It deliberately retains complete reasoning and tool
results rather than truncating existing user history.

Run `python3 scripts/check_session_store.py` for a host regression check. It
compiles the production serializer/writer with host Qt5Core and xmake's installed
nlohmann_json headers, then checks single-session edits, deletion, active-session
selection and failed writes against a 30-session/3000-message fixture. It does not
exercise the app's event loop; verify migration, editing and reopening on-device.

## Shared palette (#24)

`scripts/generate_palette.py` generates the ignored source
`resource/models/YDP02X/qml/commons/YColors.qml`. Regenerate the resource header
with `cd scripts && bash gen_qt_res.sh YDP02X` afterward. The generated header is
what git tracks and what CI checks.

The generator provides nine neutral steps and six hue families (`azure`, `teal`,
`greenFamily`, `amber`, `rose`, `violet`). Each family has `tint`, `surface`, `line`,
`solid`, `hover` and `text` steps. Colors are generated in OKLab by solving for a
contrast target against `neutral.n800`; chroma is a fixed fraction of the sRGB
gamut boundary at that lightness and hue.

Use semantic tokens (`surface`, `textPrimary`, `accent`, `danger`, etc.) for shared
roles. Use family steps for contextual colors: for example `rose.text` for warning
text and `rose.tint` for its background. `onSolid` stays pure white so small text
on every family's `solid` step passes 4.5:1. Legacy color properties remain
available and forward to the new tokens; `greenFamily` avoids colliding with the
legacy scalar `green` property.

Run `python3 scripts/generate_palette.py --check` to verify the committed resource
matches the generator and the six families meet their contrast targets. CI runs
this without needing an unpacked working QML tree. Existing host-provided image
assets, including home-card background images, retain their original colors.
The common color literals have been migrated; module-specific illustrations and
remaining custom colors can be migrated separately as their pages are reviewed.
