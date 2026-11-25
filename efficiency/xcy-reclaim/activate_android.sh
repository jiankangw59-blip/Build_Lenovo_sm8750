#!/system/bin/sh
set -eu

state=${0%/*}
policy=/dev/xcy_reclaim_policy
scene=/data/user/0/com.omarea.vtools/files/xiaocaiye_a11y_state
control=/data/adb/modules/xiaocaiye/XiaoCaiYe/control_impl.sh
boot=$(cat /proc/sys/kernel/random/boot_id)

enrolled() {
    [ -c "$policy" ] && [ -x "$state/bind_traffic" ] &&
    [ -f "$state/active-kernel" ] && [ ! -L "$state/active-kernel" ] || return 1
    notes=$(sha256sum /sys/kernel/notes) || return 1
    grep -Fqx "${notes%% *}" "$state/active-kernel"
}

phase_is() {
    [ "$(cat "$state/boot-status" 2>/dev/null)" = "$boot $1" ]
}

record_phase() {
    umask 077
    tmp=$state/.boot-status.$$
    printf '%s %s\n' "$boot" "$1" > "$tmp"
    mv -f "$tmp" "$state/boot-status"
}

framework_ready() {
    [ "$(getprop sys.user.0.ce_available)" = true ] &&
    [ -f "$scene" ] && [ ! -L "$scene" ] &&
    grep -Fqx 'version=1' "$scene" &&
    grep -Fqx 'source=scene_accessibility' "$scene" || return 1
    server=$(pidof system_server) || return 1
    case "$server" in ''|*[!0-9]*) return 1 ;; esac
    fds=$(ls -l "/proc/$server/fd") || return 1
    printf '%s\n' "$fds" | grep -q ' -> /dev/xcy_reclaim$' &&
    printf '%s\n' "$fds" | grep -q ' -> /dev/xcy_reclaim_events$'
}

activate() {
    phase_is active && return 0
    phase_is stopped && return 0
    phase_is fallback && return 0
    if framework_ready && ! pidof proc_guardd xcy-memory-helper >/dev/null 2>&1 &&
       "$state/bind_traffic" enable; then
        if record_phase active; then
            return 0
        fi
    fi
    printf 'disable\n' > "$policy" || return 1
    record_phase fallback || return 1
    if ! pidof proc_guardd >/dev/null 2>&1; then
        /system/bin/sh "$control" start-legacy-proc-guard 9>&-
    fi
    return 1
}

case "${1:-}" in
    owns-startup)
        enrolled && ! phase_is fallback
        ;;
    activate|resume|deactivate)
        enrolled || exit 0
        umask 077
        exec 9> "$state/activation.lock"
        # mksh otherwise marks descriptors opened by exec close-on-exec.
        flock -x 9 9>&9
        case "$1" in
            activate) activate ;;
            resume)
                if phase_is stopped; then
                    record_phase pending
                fi
                if [ "$(getprop sys.user.0.ce_available)" = true ]; then
                    activate
                fi
                ;;
            deactivate)
                printf 'disable\n' > "$policy"
                record_phase stopped
                ;;
        esac
        ;;
    *) exit 2 ;;
esac
