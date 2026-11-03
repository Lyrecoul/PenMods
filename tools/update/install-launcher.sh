#!/bin/sh
# Install only the runDictPen hook. Install the native helper separately via
# scratch-file + rename; never overwrite an executing helper in place.
set -eu
prefix=
if [ "${1:-}" = "--rootfs-prefix" ]; then
    [ "$#" -eq 2 ] || { echo "usage: $0 [--rootfs-prefix DIR]" >&2; exit 1; }
    prefix=$2
    case "$prefix" in /*) ;; *) echo "prefix must be absolute" >&2; exit 1 ;; esac
elif [ "$#" -ne 0 ]; then
    echo "usage: $0 [--rootfs-prefix DIR]" >&2; exit 1
fi
launcher=$prefix/usr/bin/runDictPen
helper=$prefix/userdata/PenMods/penmods-update
backup=$launcher.pre-penmods-update
[ -f "$launcher" ] && [ ! -L "$launcher" ] || { echo "runDictPen must be a regular file" >&2; exit 1; }
[ -x "$helper" ] || { echo "install executable /userdata/PenMods/penmods-update first" >&2; exit 1; }
original='stdbuf -o0 ./YoudaoDictPen -platform wayland > $LOG_FILE 2>&1'
legacy='stdbuf -o0 /userdata/PenMods/penmods-update run -- ./YoudaoDictPen -platform wayland > $LOG_FILE 2>&1'
# Keep the safety boundary in the vendor shell: even a loader failure in the
# native helper must not become six fast guardian exits and a firmware reboot.
replacement=$(cat <<'HOOK'
# BEGIN PenMods guarded launcher v2
(
    # Test the open in a child shell: a failed exec redirection is fatal in dash.
    if [ -n "${LOG_FILE:-}" ] && ( : >> "$LOG_FILE" ) 2>/dev/null; then
        exec >> "$LOG_FILE" 2>&1
    elif ( : >> /tmp/penmods-launch.log ) 2>/dev/null; then
        exec >> /tmp/penmods-launch.log 2>&1
    fi
    # The vendor app normally stops this boot watchdog during initialization.
    # Component verification runs before the app and can exhaust its boot budget.
    # Move that same handoff ahead of the helper; if it fails, start the app now.
    penmods_watchdog_ready=1
    if [ -e /sys/devices/virtual/misc/watchdog/control ]; then
        if /bin/echo stop > /sys/devices/virtual/misc/watchdog/control; then
            echo "penmods-launch: vendor boot watchdog stopped before verification"
        else
            echo "penmods-launch: cannot stop boot watchdog; bypassing helper"
            penmods_watchdog_ready=0
        fi
    fi
    echo "penmods-launch: starting helper"
    # stdbuf belongs to the app, not the helper (it injects an LD_PRELOAD).
    if [ "$penmods_watchdog_ready" = 0 ]; then
        penmods_result=125
    elif /userdata/PenMods/penmods-update run -- stdbuf -o0 ./YoudaoDictPen -platform wayland; then
        penmods_result=0
    else
        penmods_result=$?
    fi
    case "$penmods_result" in
        0|129|130|131|143) ;;
        *)
            echo "penmods-launch: helper failed ($penmods_result); starting original app"
            unset PENMODS_UPDATE_ROOT PENMODS_UPDATE_GENERATION PENMODS_UPDATE_TOKEN
            if stdbuf -o0 ./YoudaoDictPen -platform wayland; then
                penmods_result=0
            else
                penmods_result=$?
            fi
            ;;
    esac
    # Always wait using a relative timer: cold-boot clock synchronization can
    # jump wall time forward and must not defeat the stock <15s crash counter.
    echo "penmods-launch: exit ($penmods_result); guardian cooldown"
    sleep 16
    exit "$penmods_result"
)
# END PenMods guarded launcher v2
HOOK
)
guarded='    elif /userdata/PenMods/penmods-update run -- stdbuf -o0 ./YoudaoDictPen -platform wayland; then'
counts=$(awk -v before="$original" -v after="$legacy" -v guarded="$guarded" '
    $0 == before { original++ }
    $0 == after { replacement++ }
    $0 == guarded { guards++ }
    END { printf "%d %d %d", original, replacement, guards }
' "$launcher")
if [ "$counts" = '0 0 1' ] && [ "$(awk '/^# BEGIN PenMods guarded launcher v2$/,/^# END PenMods guarded launcher v2$/' "$launcher")" = "$replacement" ]; then
    echo "PenMods guarded launcher already installed"; exit 0
fi
case "$counts" in
    '0 1 0'|'1 0 0') ;;
    *) echo "unrecognized or ambiguous runDictPen layout; refusing to modify" >&2; exit 1 ;;
esac
if [ -e "$backup" ] || [ -L "$backup" ]; then
    [ -f "$backup" ] && [ ! -L "$backup" ] || { echo "invalid existing launcher backup" >&2; exit 1; }
    # A v1 hook is upgraded in place; compare its normalized stock command.
    normalized=$(awk -v before="$legacy" -v after="$original" '{ if ($0 == before) print after; else print }' "$launcher")
    [ "$normalized" = "$(cat "$backup")" ] || { echo "launcher differs from existing backup" >&2; exit 1; }
else
    cp -p "$launcher" "$backup"
    awk -v before="$legacy" -v after="$original" '{ if ($0 == before) print after; else print }' "$launcher" > "$backup"
    sync
fi
temporary=$launcher.penmods-update.$$
[ ! -e "$temporary" ] && [ ! -L "$temporary" ] || { echo "temporary launcher already exists" >&2; exit 1; }
trap 'rm -f "$temporary"' EXIT HUP INT TERM
cp -p "$launcher" "$temporary"
awk -v before="$original" -v old="$legacy" -v after="$replacement" '{ if ($0 == before || $0 == old) print after; else print }' "$launcher" > "$temporary"
sync
mv -f "$temporary" "$launcher"
sync
trap - EXIT HUP INT TERM
echo "PenMods launcher installed; next app start imports baseline and activates pending updates"
