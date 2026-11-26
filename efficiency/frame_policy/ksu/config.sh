#!/system/bin/sh
set -u
module=${0%/*}
state=/data/adb/frame-policy
controller=$module/framectl
legacy=/data/adb/modules/fas_rs/event-control.sh

fail() { echo "$*" >&2; return 1; }
bootstrap() {
    [ ! -L "$state" ] || return 1
    mkdir -p "$state" && chmod 700 "$state"
}
current_dir() {
    active=$module/defaults
    if [ -f "$state/current" ]; then
        name=$(cat "$state/current")
        case "$name" in generation.*) ;; *) return 1;; esac
        case "$name" in *[!a-zA-Z0-9._-]*) return 1;; esac
        active=$state/$name
        [ -d "$active" ] && [ ! -L "$active" ] || return 1
    fi
}
resume_owned_legacy() {
    if [ -f "$state/suspended-legacy-boot" ] && [ -f "$legacy" ]; then
        if [ "$(cat "$state/suspended-legacy-boot")" = "$(cat /proc/sys/kernel/random/boot_id)" ]; then
            sh "$legacy" start || return 1
        fi
        rm -f "$state/suspended-legacy-boot"
    fi
}
apply_dir() {
    chosen=$1
    apply_touched=0
    if [ "$(cat "$chosen/enabled")" != 1 ]; then
        apply_touched=1
        if [ -c /dev/frame_policy ]; then "$controller" disable >/dev/null || return 1; fi
        rm -f "$state/applied-boot"
        resume_owned_legacy
        return $?
    fi
    [ "$(getprop ro.product.model)" = PLQ110 ] || { fail '设备型号不匹配'; return 1; }
    [ -c /dev/frame_policy ] || { fail '当前内核没有原生帧调度接口'; return 1; }
    library=$(sha256sum /system/lib64/libgui.so)
    [ "${library%% *}" = "$(cat "$module/defaults/libgui.sha256")" ] || {
        fail '系统版本已变化，请更新配置包'; return 1;
    }
    native=$(cat /dev/frame_policy) || return 1
    flags=$(od -An -tu1 -j4 -N1 "$chosen/config.bin")
    if [ "$((flags & 2))" != 0 ] && ! printf '%s\n' "$native" | grep -q 'features=.*gpu_file_credentials'; then
        fail '当前内核未包含 GPU 权限修复，请先关闭 gpu'; return 1
    fi
    if [ -f "$legacy" ]; then
        stopped=$("$controller" suspend-legacy) || return 1
        if [ "$stopped" = legacy_was_running=1 ]; then
            cat /proc/sys/kernel/random/boot_id > "$state/suspended-legacy-boot"
        fi
    fi
    apply_touched=1
    if printf '%s\n' "$native" | grep -q 'features=.*game_relations'; then
        "$controller" load-rules "$chosen/games.bin" >/dev/null || return 1
    fi
    "$controller" load "$chosen/config.bin" > "$state/last-apply.txt" || return 1
    grep -qx 'last_error=0' "$state/last-apply.txt" || return 1
    cat /proc/sys/kernel/random/boot_id > "$state/applied-boot" || return 1
    return 0
}
status() {
    if [ -c /dev/frame_policy ]; then
        cat /dev/frame_policy
        if ! grep -q 'features=.*fps_per_game' /dev/frame_policy; then
            echo 'module_note=当前内核只应用全局帧率，逐游戏帧率与跨簇规则尚未应用'
        fi
    else
        echo 'module_note=当前内核没有原生帧调度接口'
    fi
}

bootstrap || exit 1
case "${1:-status}" in
    read)
        current_dir || exit 1
        hash=$(sha256sum "$active/settings.json")
        printf '%s\n' "${hash%% *}"
        cat "$active/settings.json"
        ;;
    seed) cat "$module/defaults/seed.json";;
    status) status;;
    boot)
        current_dir || exit 1
        # An untouched/dormant module never changes an existing manual session.
        [ "$(cat "$active/enabled")" = 1 ] || exit 0
        exec 7>"$state/config.lock"
        flock -x 7 7>&7 || exit 1
        apply_dir "$active" || { [ "$apply_touched" != 1 ] || "$controller" disable >/dev/null 2>&1; resume_owned_legacy; exit 1; }
        ;;
    save)
        exec 7>"$state/config.lock"
        flock -x 7 7>&7 || exit 1
        current_dir || exit 1
        old=$active
        hash=$(sha256sum "$old/settings.json")
        [ "${2:-}" = "${hash%% *}" ] || { fail '配置已被修改，请刷新后重试'; exit 1; }
        pending=$(mktemp -d "$state/generation.XXXXXXXX") || exit 1
        "$controller" unpack-settings "$pending" || exit 1
        if ! apply_dir "$pending"; then
            touched=$apply_touched
            if [ "$old" != "$module/defaults" ]; then
                if ! apply_dir "$old"; then "$controller" disable >/dev/null 2>&1; resume_owned_legacy; fi
            else
                [ "$touched" != 1 ] || "$controller" disable >/dev/null 2>&1
                resume_owned_legacy
            fi
            fail '应用失败，未保存新配置；请检查运行状态'
            exit 1
        fi
        if [ "$old" != "$module/defaults" ]; then printf '%s\n' "${old##*/}" > "$state/previous"; fi
        printf '%s\n' "${pending##*/}" > "$state/current.tmp.$$"
        mv "$state/current.tmp.$$" "$state/current" || exit 1
        status
        ;;
    rollback)
        exec 7>"$state/config.lock"
        flock -x 7 7>&7 || exit 1
        current_dir || exit 1
        old=$active
        previous=$(cat "$state/previous") || exit 1
        case "$previous" in generation.*) ;; *) exit 1;; esac
        case "$previous" in *[!a-zA-Z0-9._-]*) exit 1;; esac
        [ -d "$state/$previous" ] && [ ! -L "$state/$previous" ] || exit 1
        apply_dir "$state/$previous" || exit 1
        printf '%s\n' "${old##*/}" > "$state/previous"
        printf '%s\n' "$previous" > "$state/current.tmp.$$"
        mv "$state/current.tmp.$$" "$state/current" || exit 1
        status
        ;;
    *) fail 'usage: config.sh read|seed|status|boot|save HASH|rollback'; exit 2;;
esac
