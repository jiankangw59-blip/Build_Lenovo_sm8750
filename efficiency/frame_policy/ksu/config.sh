#!/system/bin/sh
set -u
umask 077
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
    name=defaults
    if [ -f "$state/current" ]; then
        name=$(cat "$state/current")
        case "$name" in defaults|generation.*) ;; *) return 1;; esac
        case "$name" in *[!a-zA-Z0-9._-]*) return 1;; esac
        if [ "$name" = defaults ]; then active=$module/defaults; else active=$state/$name; fi
        [ -d "$active" ] && [ ! -L "$active" ] || return 1
    fi
}
settle_handoff() {
    if [ -e "$state/legacy-stop-pending" ]; then
        clean=$("$controller" legacy-clean) || {
            fail '旧 FAS 交接尚未确认，保留 pending 记录；未恢复旧控制器'; return 1;
        }
        if [ "$clean" = legacy_was_running=1 ]; then
            cat /proc/sys/kernel/random/boot_id > "$state/suspended-legacy-boot.tmp.$$" &&
                mv "$state/suspended-legacy-boot.tmp.$$" "$state/suspended-legacy-boot" || return 1
        fi
        rm -f "$state/legacy-stop-pending" || return 1
    fi
}
resume_owned_legacy() {
    settle_handoff || return 1
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
    # Older immutable generations predate FAS data. Their compatibility value
    # is always balance/1 FPS, supplied by the module without rewriting history.
    fas=$chosen/fas.bin
    [ -f "$fas" ] || fas=$module/defaults/fas.bin
    if [ "$(cat "$chosen/enabled")" != 1 ]; then
        # An explicit limit still requires kernel support when stored disabled.
        # Legacy disabled configurations also work without a frame policy device.
        if [ -f "$fas" ]; then
            fas_size=$(wc -c < "$fas") || return 1
            if [ "$fas_size" -eq 88 ]; then
                "$controller" check-fas "$fas" >/dev/null || return 1
            fi
        fi
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
    if [ "$(cat "$chosen/scene-required")" = 1 ] &&
       ! printf '%s\n' "$native" | grep -q 'features=.*scene_table'; then
        fail '当前内核尚未接入 Scene 策略表，不能应用 fast 或逐应用 Scene 配置'; return 1
    fi
    aux=$chosen/aux.bin
    aux_required=0
    if [ -f "$chosen/aux-required" ]; then
        aux_required=$(cat "$chosen/aux-required") || return 1
        case "$aux_required" in 0|1) ;; *) fail '辅助参数标记无效'; return 1;; esac
    fi
    if [ ! -f "$aux" ]; then
        [ "$aux_required" = 0 ] || { fail '辅助参数文件缺失'; return 1; }
        aux=$module/defaults/aux.bin
    fi
    if [ "$aux_required" = 1 ] && ! printf '%s\n' "$native" | grep -q 'features=.*scene_aux'; then
        fail '当前内核不支持 Scene 辅助调度'; return 1
    fi
    if [ "$aux_required" = 1 ] && ! printf '%s\n' "$native" | grep -q 'features=.*scene_table'; then
        fail 'Scene 辅助调度需要内核场景表支持'; return 1
    fi
    "$controller" check-aux "$aux" >/dev/null || return 1
    aux_count=$(od -An -tu4 -j12 -N4 "$aux") || return 1
    [ "$((aux_count > 0 ? 1 : 0))" = "$aux_required" ] || { fail '辅助参数与标记不一致'; return 1; }
    # Validate exact bytes AND the kernel capability before stopping either
    # controller. check-fas is read-only; load-fas below performs the real ioctl.
    "$controller" check-fas "$fas" >/dev/null || return 1
    if [ -f "$legacy" ]; then
        stopped=$("$controller" suspend-legacy) || return 1
    fi
    settle_handoff || return 1
    apply_touched=1
    "$controller" disable >/dev/null || return 1
    if printf '%s\n' "$native" | grep -q 'features=.*scene_table'; then
        "$controller" load-scene "$chosen/scene.bin" >/dev/null || return 1
    fi
    "$controller" load-aux "$aux" >/dev/null || return 1
    if printf '%s\n' "$native" | grep -q 'features=.*game_relations'; then
        "$controller" load-rules "$chosen/games.bin" >/dev/null || return 1
    fi
    "$controller" load-fas "$fas" >/dev/null || return 1
    "$controller" load "$chosen/config.bin" > "$state/last-apply.txt" || return 1
    grep -qx 'last_error=0' "$state/last-apply.txt" || return 1
    if printf '%s\n' "$native" | grep -q 'features=.*scene_aux'; then
        grep -qx 'aux_error=0' "$state/last-apply.txt" || return 1
        grep -qx 'gpu_aux_error=0' "$state/last-apply.txt" || return 1
        grep -qx 'gpu_aux_rollback_error=0' "$state/last-apply.txt" || return 1
    fi
    if printf '%s\n' "$native" | grep -Eq '^features=([^,]*,)*fas_util_limit_v1(,|$)'; then
        expected_limit=3000
        if [ "$(wc -c < "$fas")" -eq 88 ]; then
            expected_limit=$(od -An -tu4 -j80 -N4 "$fas") || return 1
        fi
        grep -qx "fas_util_limit_mfps=$((expected_limit))" "$state/last-apply.txt" || {
            fail '内核下调上限读回不一致，未提交新配置'; return 1;
        }
    fi
    cat /proc/sys/kernel/random/boot_id > "$state/applied-boot" || return 1
    return 0
}
status() {
    if [ -c /dev/frame_policy ]; then
        cat /dev/frame_policy
        if ! grep -q 'features=.*fps_per_game' /dev/frame_policy; then
            echo 'module_note=当前内核只应用全局帧率，逐游戏帧率与跨簇规则尚未应用'
        fi
        if ! grep -q 'features=.*scene_table' /dev/frame_policy; then
            echo 'module_note=当前内核只支持基础全局 Scene 频率模板'
        fi
        if ! grep -q 'features=.*fas_tuning' /dev/frame_policy; then
            echo 'module_note=当前内核缺少 FAS 调优接口，启用前需要包含 fas_tuning 的内核'
        fi
    else
        echo 'module_note=当前内核没有原生帧调度接口'
    fi
    if [ -f "$state/legacy-stop-pending" ]; then
        echo 'module_handoff=pending; cleanup must be confirmed before restoration'
        cat "$state/legacy-stop-pending"
    fi
}

# A generation is immutable once selected. Its parent is committed with its
# files; state/current is the sole atomic commit point for runtime and history.
record_parent() {
    printf '%s\n' "${2##*/}" > "$1/parent-generation"
}
commit_current() {
    printf '%s\n' "${1##*/}" > "$state/current.tmp.$$" &&
        mv "$state/current.tmp.$$" "$state/current"
}
recover_current() {
    if ! apply_dir "$1"; then
        if [ ! -c /dev/frame_policy ] || "$controller" disable >/dev/null 2>&1; then
            rm -f "$state/applied-boot"
            resume_owned_legacy || return 1
        else
            fail '当前配置恢复及内核清理均失败；未启动旧 FAS'; return 1
        fi
        fail '当前配置恢复失败，已停止原生控制'; return 1
    fi
}
stop_owned_native() {
    this_boot=$(cat /proc/sys/kernel/random/boot_id) || return 1
    for marker in applied-boot suspended-legacy-boot; do
        if [ -f "$state/$marker" ] && [ "$(cat "$state/$marker")" = "$this_boot" ]; then
            if [ -c /dev/frame_policy ]; then "$controller" disable >/dev/null || return 1; fi
            rm -f "$state/applied-boot"
            return 0
        fi
    done
}
transaction() {
    candidate=$1
    original=$2
    if ! apply_dir "$candidate"; then
        touched=$apply_touched
        if [ "$touched" = 1 ]; then recover_current "$original" || :; fi
        fail '应用失败，未保存新配置；请检查运行状态和 pending 记录'
        return 1
    fi
    if ! commit_current "$candidate"; then
        # Rename failure must not leave a new runtime behind an old pointer.
        recover_current "$original" || :
        fail '配置提交失败，已尝试恢复原运行配置；未保存新配置'
        return 1
    fi
    status
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
    scene-seed) cat "$module/defaults/scene-seed.json";;
    aux-seed) cat "$module/defaults/scene-aux-seed.json";;
    status) status;;
    teardown)
        exec 7>"$state/config.lock"
        flock -x 7 7>&7 || exit 1
        stop_owned_native || exit 1
        settle_handoff || exit 1
        # A confirmed pending handoff can establish ownership after the first check.
        stop_owned_native || exit 1
        resume_owned_legacy || exit 1
        ;;
    boot)
        exec 7>"$state/config.lock"
        flock -x 7 7>&7 || exit 1
        current_dir || exit 1
        # Re-read only after locking: a concurrent save may select a new generation.
        enabled=$(cat "$active/enabled") || exit 1
        [ "$enabled" = 1 ] || exit 0
        if ! apply_dir "$active"; then
            if [ "$apply_touched" = 1 ]; then
                if "$controller" disable >/dev/null 2>&1; then resume_owned_legacy || :; fi
            fi
            exit 1
        fi
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
        record_parent "$pending" "$old" || exit 1
        transaction "$pending" "$old" || exit 1
        ;;
    rollback)
        exec 7>"$state/config.lock"
        flock -x 7 7>&7 || exit 1
        current_dir || exit 1
        old=$active
        if [ -f "$old/parent-generation" ]; then
            previous=$(cat "$old/parent-generation") || exit 1
        elif [ "$old" != "$module/defaults" ] && [ -f "$state/previous" ]; then
            previous=$(cat "$state/previous") || exit 1 # Previous module format.
        else
            fail '没有可恢复的上一代配置'; exit 1
        fi
        case "$previous" in defaults|generation.*) ;; *) exit 1;; esac
        case "$previous" in *[!a-zA-Z0-9._-]*) exit 1;; esac
        if [ "$previous" = defaults ]; then source=$module/defaults; else source=$state/$previous; fi
        [ -d "$source" ] && [ ! -L "$source" ] || exit 1
        pending=$(mktemp -d "$state/generation.XXXXXXXX") || exit 1
        for leaf in settings.json config.bin games.bin scene.bin enabled scene-required; do
            cat "$source/$leaf" > "$pending/$leaf" || exit 1
        done
        if [ -f "$source/fas.bin" ]; then
            cat "$source/fas.bin" > "$pending/fas.bin" || exit 1
        else
            cat "$module/defaults/fas.bin" > "$pending/fas.bin" || exit 1
        fi
        if [ -f "$source/aux.bin" ]; then
            cat "$source/aux.bin" > "$pending/aux.bin" || exit 1
            if [ -f "$source/aux-required" ]; then
                cat "$source/aux-required" > "$pending/aux-required" || exit 1
            else
                printf '0\n' > "$pending/aux-required" || exit 1
            fi
        else
            cat "$module/defaults/aux.bin" > "$pending/aux.bin" || exit 1
            printf '0\n' > "$pending/aux-required" || exit 1
        fi
        record_parent "$pending" "$old" || exit 1
        transaction "$pending" "$old" || exit 1
        ;;
    *) fail 'usage: config.sh read|seed|scene-seed|status|boot|save HASH|rollback|teardown'; exit 2;;
esac
