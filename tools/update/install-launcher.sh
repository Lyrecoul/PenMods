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
# BEGIN PenMods guarded launcher v3
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
    # /tmp is boot-scoped. Uptime avoids NTP jumps affecting crash detection.
    penmods_count_file=/tmp/penmods-launch-failures
    penmods_safe_file=/tmp/penmods-safe-mode
    penmods_failures=0
    if [ -f "$penmods_count_file" ]; then
        read -r penmods_failures < "$penmods_count_file" || penmods_failures=0
    fi
    case "$penmods_failures" in ''|*[!0-9]*) penmods_failures=0 ;; esac
    if [ "$penmods_failures" -ge 3 ]; then : > "$penmods_safe_file"; fi
    read -r penmods_started penmods_unused < /proc/uptime
    penmods_started=${penmods_started%%.*}
    if [ -f "$penmods_safe_file" ]; then
        echo "penmods-launch: safe mode active for this boot"
        if /userdata/PenMods/penmods-update stock -- ./YoudaoDictPen -platform wayland; then
            penmods_result=0
        else
            penmods_result=$?
        fi
    else
        echo "penmods-launch: starting helper"
        if [ "$penmods_watchdog_ready" = 0 ]; then
            penmods_result=125
        elif /userdata/PenMods/penmods-update run -- stdbuf -o0 ./YoudaoDictPen -platform wayland; then
            penmods_result=0
        else
            penmods_result=$?
        fi
        read -r penmods_ended penmods_unused < /proc/uptime
        penmods_ended=${penmods_ended%%.*}
        case "$penmods_result" in
            0|129|130|131|143) penmods_failures=0 ;;
            *)
                if [ "$((penmods_ended - penmods_started))" -lt 60 ]; then
                    penmods_failures=$((penmods_failures + 1))
                else
                    penmods_failures=0
                fi
                ;;
        esac
        echo "$penmods_failures" > "$penmods_count_file"
        if [ "$penmods_failures" -ge 3 ]; then
            : > "$penmods_safe_file"
            echo "penmods-launch: three fast failures; starting stock backup"
            if /userdata/PenMods/penmods-update stock -- ./YoudaoDictPen -platform wayland; then
                penmods_result=0
            else
                penmods_result=$?
            fi
        else
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
        fi
    fi
    # Always wait using a relative timer: cold-boot clock synchronization can
    # jump wall time forward and must not defeat the stock <15s crash counter.
    echo "penmods-launch: exit ($penmods_result); guardian cooldown"
    sleep 16
    exit "$penmods_result"
)
# END PenMods guarded launcher v3
HOOK
)
guarded='        elif /userdata/PenMods/penmods-update run -- stdbuf -o0 ./YoudaoDictPen -platform wayland; then'
counts=$(awk -v before="$original" -v after="$legacy" -v guarded="$guarded" '
    $0 == before { original++ }
    $0 == after { replacement++ }
    $0 == guarded { guards++ }
    END { printf "%d %d %d", original, replacement, guards }
' "$launcher")
if [ "$counts" = '0 0 1' ] && [ "$(awk '/^# BEGIN PenMods guarded launcher v3$/,/^# END PenMods guarded launcher v3$/' "$launcher")" = "$replacement" ]; then
    echo "PenMods guarded launcher already installed"; exit 0
fi
upgrade_v2=0
if [ "$counts" = '0 0 0' ] && [ -f "$backup" ] && [ ! -L "$backup" ]; then
    old_block=$(awk '/^# BEGIN PenMods guarded launcher v2$/,/^# END PenMods guarded launcher v2$/' "$launcher")
    old_digest=$(printf '%s\n' "$old_block" | sha256sum | awk '{print $1}')
    if [ "$old_digest" = 'ae0251cbd0fa7389186ae57beab258a3136164b13791927ed3df2312f51c3720' ]; then
        normalized=$(awk -v original="$original" '
            /^# BEGIN PenMods guarded launcher v2$/ { print original; skip=1; next }
            /^# END PenMods guarded launcher v2$/ { skip=0; next }
            !skip { print }
        ' "$launcher")
        [ "$normalized" = "$(cat "$backup")" ] || { echo "launcher differs from backup" >&2; exit 1; }
        upgrade_v2=1
    fi
fi
case "$counts:$upgrade_v2" in
    '0 1 0:0'|'1 0 0:0'|'0 0 0:1') ;;
    *) echo "unrecognized or ambiguous runDictPen layout; refusing to modify" >&2; exit 1 ;;
esac
if [ -e "$backup" ] || [ -L "$backup" ]; then
    [ -f "$backup" ] && [ ! -L "$backup" ] || { echo "invalid existing launcher backup" >&2; exit 1; }
    # A v1 hook is upgraded in place; compare its normalized stock command.
    normalized=$(awk -v before="$legacy" -v after="$original" '{ if ($0 == before) print after; else print }' "$launcher")
    if [ "$upgrade_v2" = 0 ]; then
        [ "$normalized" = "$(cat "$backup")" ] || { echo "launcher differs from existing backup" >&2; exit 1; }
    fi
else
    cp -p "$launcher" "$backup"
    awk -v before="$legacy" -v after="$original" '{ if ($0 == before) print after; else print }' "$launcher" > "$backup"
    sync
fi
temporary=$launcher.penmods-update.$$
[ ! -e "$temporary" ] && [ ! -L "$temporary" ] || { echo "temporary launcher already exists" >&2; exit 1; }
trap 'rm -f "$temporary"' EXIT HUP INT TERM
cp -p "$launcher" "$temporary"
awk -v before="$original" -v old="$legacy" -v after="$replacement" '
    /^# BEGIN PenMods guarded launcher v2$/ { print after; skip=1; next }
    /^# END PenMods guarded launcher v2$/ { skip=0; next }
    !skip { if ($0 == before || $0 == old) print after; else print }
' "$launcher" > "$temporary"
sync
mv -f "$temporary" "$launcher"
sync
trap - EXIT HUP INT TERM
echo "PenMods launcher installed; next app start imports baseline and activates pending updates"
