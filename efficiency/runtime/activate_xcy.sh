#!/system/bin/sh
set -eu
module=${0%/*}
. "$module/enrolled.sh"
opd2413_enrolled || exit 1

boot=$(cat /proc/sys/kernel/random/boot_id)
status=$state/xcy-reclaim.status
control=/data/adb/modules/xiaocaiye/XiaoCaiYe/control_impl.sh
scene=/data/user/0/com.omarea.vtools/files/xiaocaiye_a11y_state

case "${1:-}" in
    owns-startup)
        [ "$(cat "$status" 2>/dev/null)" = "$boot active" ]
        exit $?
        ;;
    activate) ;;
    *) exit 2 ;;
esac

[ -f "$state/xcy-devices-labeled" ] &&
    [ "$(cat "$state/xcy-devices-labeled")" = "$boot" ] || exit 0
[ "$(getprop sys.user.0.ce_available)" = true ] || exit 0
[ -f "$scene" ] && [ ! -L "$scene" ] || exit 0
grep -Fqx 'source=scene_accessibility' "$scene" || exit 0
grep -Fq 'stop-legacy-proc-guard)' "$control" || exit 0
server=$(pidof system_server) || exit 0
case "$server" in ''|*[!0-9]*) exit 0 ;; esac
fds=$(ls -l "/proc/$server/fd") || exit 0
printf '%s\n' "$fds" | grep -q ' -> /dev/xcy_reclaim$' || exit 0
printf '%s\n' "$fds" | grep -q ' -> /dev/xcy_reclaim_events$' || exit 0

umask 077
exec 9>"$state/xcy-activation.lock"
flock -n 9 9>&9 || exit 0
[ "$(cat "$status" 2>/dev/null || true)" != "$boot active" ] || exit 0
if sh "$control" stop-legacy-proc-guard 9>&- &&
   ! pidof proc_guardd xcy-memory-helper >/dev/null &&
   "$module/bind_traffic" enable; then
    printf '%s active\n' "$boot" > "$status"
else
    printf 'disable\n' > /dev/xcy_reclaim_policy
    printf '%s fallback\n' "$boot" > "$status"
    sh "$control" start-legacy-proc-guard 9>&-
fi
