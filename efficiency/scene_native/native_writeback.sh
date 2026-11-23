# Native-ZRAM support for verified PLQ110 kernels. Sourced by Scene's tools.sh.
ACE6_NATIVE_STATE=/data/adb/ace6-kernel-efficiency
ACE6_NATIVE_BACKING=$ACE6_NATIVE_STATE/zram-backing.img

ace6_native_profile_active() {
    [ -f "$ACE6_NATIVE_STATE/native-writeback.enabled" ] || return 1
    [ "$(getprop ro.product.model)" = PLQ110 ] || return 1
    [ -f /sys/block/zram0/backing_dev ] || return 1
    [ -f /sys/block/zram0/writeback ] || return 1
    [ "$(getprop persist.sys.oplus.nandswap)" != true ] || return 1
    [ ! -e /sys/block/zram0/hybridswap_core_enable ] || return 1
    [ ! -e /sys/block/zram0/hybridswap_loop_device ] || return 1
    local entry actual
    for entry in /sys/module/*hybridswap*; do
        [ ! -d "$entry" ] || return 1
    done
    actual=$(sha256sum /sys/kernel/notes 2>/dev/null) || return 1
    actual=${actual%% *}
    grep -Fxq "$actual" "$ACE6_NATIVE_STATE/known-native-kernels" || return 1
}

ace6_native_loop_path() {
    local name number attempt
    case "$1" in
        /dev/block/loop*|/dev/loop*|loop*) name=${1##*/} ;;
        *) return 1 ;;
    esac
    number=${name#loop}
    case "$number" in ''|*[!0-9]*) return 1 ;; esac
    # LOOP_CTL_GET_FREE can precede ueventd's creation of the device node.
    # This is a bounded startup wait, never a background polling task.
    attempt=0
    while [ ! -b "/dev/block/$name" ] && [ "$attempt" -lt 30 ]; do
        /system/bin/sleep 0.1
        attempt=$((attempt + 1))
    done
    [ -b "/dev/block/$name" ] || {
        echo '[ACE6-NATIVE] Loop node was not created within three seconds' >&2
        return 1
    }
    printf '/dev/block/%s\n' "$name"
}

ace6_native_backing_matches() {
    local device owner
    device=$(ace6_native_loop_path "$1") || return 1
    owner=$(cat "/sys/block/${device##*/}/loop/backing_file" 2>/dev/null) || return 1
    [ "$owner" = "$ACE6_NATIVE_BACKING" ]
}

ace6_native_transition_safe() {
    case "$1" in ''|none) return 0 ;; esac
    ace6_native_backing_matches "$1" || {
        echo '[ACE6-NATIVE] Preserve an unowned backing device' >&2
        return 1
    }
}

ace6_native_prepare_backing() {
    ace6_native_profile_active || return 1
    ace6_native_ram_headroom || return 1
    local free_kb path owner size found
    [ ! -L "$ACE6_NATIVE_STATE" ] || return 1
    mkdir -p "$ACE6_NATIVE_STATE" || return 1
    chmod 700 "$ACE6_NATIVE_STATE" || return 1
    [ ! -L "$ACE6_NATIVE_BACKING" ] || return 1
    if [ ! -e "$ACE6_NATIVE_BACKING" ]; then
        free_kb=$(df -Pk "$ACE6_NATIVE_STATE" | awk 'END {print $4}')
        case "$free_kb" in ''|*[!0-9]*) return 1 ;; esac
        [ "$free_kb" -ge 29360128 ] || {
            echo '[ACE6-NATIVE] Need 8 GiB backing plus 20 GiB storage headroom' >&2
            return 1
        }
        (umask 077; set -C; : > "$ACE6_NATIVE_BACKING") || return 1
        if ! fallocate -l 8589934592 "$ACE6_NATIVE_BACKING"; then
            rm -f "$ACE6_NATIVE_BACKING"
            return 1
        fi
    fi
    [ -f "$ACE6_NATIVE_BACKING" ] || return 1
    size=$(stat -c %s "$ACE6_NATIVE_BACKING") || return 1
    [ "$size" = 8589934592 ] || {
        echo '[ACE6-NATIVE] Preserve an unexpected backing file size' >&2
        return 1
    }
    chown 0:0 "$ACE6_NATIVE_BACKING" || return 1
    chmod 600 "$ACE6_NATIVE_BACKING" || return 1
    chcon u:object_r:ksu_file:s0 "$ACE6_NATIVE_BACKING" || return 1
    found=''
    for path in /sys/block/loop*/loop/backing_file; do
        [ -f "$path" ] || continue
        owner=$(cat "$path") || return 1
        [ "$owner" = "$ACE6_NATIVE_BACKING" ] || continue
        [ -z "$found" ] || {
            echo '[ACE6-NATIVE] Multiple aliases of the private backing file; stop' >&2
            return 1
        }
        found=${path#/sys/block/}
        found=/dev/block/${found%%/*}
    done
    if [ -z "$found" ]; then
        found=$(/system/bin/losetup -f) || return 1
        found=$(ace6_native_loop_path "$found") || return 1
        /system/bin/losetup "$found" "$ACE6_NATIVE_BACKING" || return 1
    fi
    ace6_native_backing_matches "$found" || return 1
    _ACE6_NATIVE_LOOP=$found
    exec 9<"$_ACE6_NATIVE_LOOP" || return 1
    /system/bin/dd if="$_ACE6_NATIVE_LOOP" of=/dev/null bs=4096 count=1 2>/dev/null || return 1
    ace6_native_profile_active || return 1
}

ace6_native_ram_headroom() {
    local used available swaps
    swaps=$(awk 'NR > 1 {print $1}' /proc/swaps) || return 1
    case "$swaps" in ''|/dev/block/zram0|/dev/zram0) ;; *) return 1 ;; esac
    used=$(awk '$1 == "/dev/block/zram0" || $1 == "/dev/zram0" {print $4; exit}' /proc/swaps)
    used=${used:-0}
    available=$(awk '$1 == "MemAvailable:" {print $2; exit}' /proc/meminfo)
    case "$used:$available" in *[!0-9:]*) return 1 ;; esac
    [ -n "$available" ] || return 1
    if [ "$used" -gt 0 ] && [ "$available" -lt "$((used + 2097152))" ]; then
        echo '[ACE6-NATIVE] Insufficient RAM headroom for a live swapoff; preserve current ZRAM' >&2
        return 1
    fi
    return 0
}

ace6_native_setup_backing() {
    ace6_native_profile_active || return 1
    [ -n "${_ACE6_NATIVE_LOOP:-}" ] || ace6_native_prepare_backing || return 1
    ace6_native_backing_matches "$_ACE6_NATIVE_LOOP" || return 1
    # This is a raw backing store, not a swap filesystem. Never format it.
    write_required_value "$_ACE6_NATIVE_LOOP" /sys/block/zram0/backing_dev || return 1
    write_required_value 2097152 /sys/block/zram0/writeback_limit || return 1
    write_required_value 1 /sys/block/zram0/writeback_limit_enable || return 1
    echo '[ACE6-NATIVE] Native private backing attached; vendor crypto mapping preserved'
}

ace6_native_cleanup_loop() {
    [ -n "${_ACE6_NATIVE_LOOP:-}" ] || return 0
    exec 9<&-
    local current
    current=$(cat /sys/block/zram0/backing_dev 2>/dev/null) || return 0
    ace6_native_backing_matches "$current" && return 0
    if ace6_native_backing_matches "$_ACE6_NATIVE_LOOP"; then
        /system/bin/losetup -d "$_ACE6_NATIVE_LOOP" || return 1
    fi
}

ace6_native_restore_ram_swap() {
    local active=0
    zram_swap_active || active=$?
    case "$active" in 0) return 0 ;; 1) ;; *) return 1 ;; esac
    [ "${zram_size:-0}" -gt 0 ] || return 0
    # The previous swapoff already restored application pages to RAM.
    # Recover ordinary ZRAM if the optional backing setup failed.
    write_required_value 1 /sys/block/zram0/reset || return 1
    write_required_value "$target_alg" /sys/block/zram0/comp_algorithm || return 1
    write_required_value "${zram_size}M" /sys/block/zram0/disksize || return 1
    # These functions are sourced before Scene defines its aliases. BusyBox
    # standalone swapon omits -p, so use Android's checked system tools.
    /system/bin/mkswap /dev/block/zram0 >/dev/null || return 1
    /system/bin/swapon /dev/block/zram0 -p 0 || return 1
}
