# Early-only support for explicitly enrolled Crystal kernels. No auto policy.
ACE6_CRYSTAL_STATE=/data/adb/opd2413-kernel-efficiency
ACE6_CRYSTAL_BACKING=$ACE6_CRYSTAL_STATE/zms-backing.img
ACE6_CRYSTAL_NODE=/sys/block/zram0
ACE6_CRYSTAL_CONFIG=$ACE6_CRYSTAL_STATE/swap_config.conf

ace6_crystal_profile_active() {
    [ -d "$ACE6_CRYSTAL_STATE" ] && [ ! -L "$ACE6_CRYSTAL_STATE" ] || return 1
    [ -f "$ACE6_CRYSTAL_STATE/crystal-writeback.enabled" ] || return 1
    [ ! -L "$ACE6_CRYSTAL_STATE/crystal-writeback.enabled" ] || return 1
    [ "$(getprop ro.product.model)" = OPD2413 ] || return 1
    [ -f "$ACE6_CRYSTAL_NODE/zms_stat" ] || return 1
    [ -f "$ACE6_CRYSTAL_NODE/backing_dev" ] || return 1
    [ -f "$ACE6_CRYSTAL_NODE/hybridswap_core_enable" ] || return 1
    [ -f "$ACE6_CRYSTAL_NODE/hybridswap_enable" ] || return 1
    local actual matches
    actual=$(sha256sum /sys/kernel/notes 2>/dev/null) || return 1
    actual=${actual%% *}
    case "$actual" in ''|*[!0-9a-f]*) return 1 ;; esac
    [ "${#actual}" = 64 ] || return 1
    # Every enrollment binds one exact kernel to one expected compressor.
    # A later LZ4KDS experiment must be explicitly enrolled, not inferred.
    matches=$(awk -v sha="$actual" '$1==sha && NF==2 {print $2}' \
        "$ACE6_CRYSTAL_STATE/known-crystal-kernels" 2>/dev/null) || return 1
    case "$matches" in lz4kd|lz4kds) ;; *) return 1 ;; esac
    ACE6_CRYSTAL_CODEC=$matches
}

ace6_crystal_status() {
    local output=$ACE6_CRYSTAL_STATE/early-crystal-writeback.status
    {
        printf 'result=%s\nreason=%s\n' "$1" "$2"
        printf 'codec=%s\ntarget_mb=%s\nauto_policy=disabled\n' \
            "${ACE6_CRYSTAL_CODEC:-unknown}" "${ACE6_CRYSTAL_TARGET_MB:-unknown}"
        printf 'boot_id=%s\n' "$(cat /proc/sys/kernel/random/boot_id)"
        printf 'ram_swap_recovered=%s\n' "${_ACE6_CRYSTAL_RECOVERED:-not_needed}"
    } > "$output.tmp.$$" && mv -f "$output.tmp.$$" "$output"
}

ace6_crystal_config_value() {
    # Read literal scalar assignments without running shell code or dos2unix.
    awk -v key="$1" -v fallback="$2" '
    BEGIN {value=fallback; quote=sprintf("%c",39)}
    $0 ~ "^[[:space:]]*" key "[[:space:]]*=" {
        value=$0; sub("^[[:space:]]*" key "[[:space:]]*=", "", value)
        sub(/\r$/, "", value); sub(/[[:space:]]+#.*$/, "", value)
        gsub(/^[[:space:]]+|[[:space:]]+$/, "", value)
        if ((substr(value,1,1)=="\"" && substr(value,length(value),1)=="\"") ||
            (substr(value,1,1)==quote && substr(value,length(value),1)==quote))
            value=substr(value,2,length(value)-2)
    }
    END {print value}' "$ACE6_CRYSTAL_CONFIG"
}

ace6_crystal_read_config() {
    [ -r "$ACE6_CRYSTAL_CONFIG" ] || return 1
    [ "$(ace6_crystal_config_value zram false)" = true ] || return 1
    ACE6_CRYSTAL_TARGET_MB=$(ace6_crystal_config_value zram_size 0) || return 1
    case "$ACE6_CRYSTAL_TARGET_MB" in ''|*[!0-9]*) return 1 ;; esac
    # Bounds protect early boot from malformed/unbounded metadata allocations.
    [ "$ACE6_CRYSTAL_TARGET_MB" -ge 256 ] && [ "$ACE6_CRYSTAL_TARGET_MB" -le 65536 ] || return 1
    local wanted
    wanted=$(ace6_crystal_config_value comp_algorithm '') || return 1
    [ "$wanted" = "$ACE6_CRYSTAL_CODEC" ] || return 1
    ACE6_CRYSTAL_WANT_BACKING=$(ace6_crystal_config_value zram_writeback default) || return 1
    case "$ACE6_CRYSTAL_WANT_BACKING" in default|true) ACE6_CRYSTAL_WANT_BACKING=true ;;
        false) ;; *) return 1 ;; esac
    ACE6_CRYSTAL_TARGET_BYTES=$(awk -v mb="$ACE6_CRYSTAL_TARGET_MB" 'BEGIN {printf "%.0f",mb*1048576}')
}

ace6_crystal_swap_active() {
    [ -r /proc/swaps ] || return 2
    awk '$1=="/dev/block/zram0" || $1=="/dev/zram0" {found=1} END {exit !found}' /proc/swaps
}

ace6_crystal_codec_selected() {
    local algorithms
    algorithms=$(cat "$ACE6_CRYSTAL_NODE/comp_algorithm") || return 1
    case "$algorithms" in *"[$ACE6_CRYSTAL_CODEC]"*) return 0 ;; esac
    return 1
}

ace6_crystal_write_node() {
    case "$2" in reset|disksize|comp_algorithm|backing_dev|writeback_limit|writeback_limit_enable|hybridswap_enable|hybridswap_core_enable) ;;
        *) return 1 ;; esac
    printf '%s\n' "$1" > "$ACE6_CRYSTAL_NODE/$2"
}

ace6_crystal_ensure_codec() {
    # Locked candidates reject even an identical comp_algorithm write.
    ace6_crystal_codec_selected && return 0
    [ "$(cat "$ACE6_CRYSTAL_NODE/disksize")" = 0 ] || return 1
    ace6_crystal_write_node "$ACE6_CRYSTAL_CODEC" comp_algorithm || return 1
    ace6_crystal_codec_selected
}

ace6_crystal_loop_path() {
    local name number attempt=0
    case "$1" in /dev/block/loop*|/dev/loop*|loop*) name=${1##*/} ;; *) return 1 ;; esac
    number=${name#loop}
    case "$number" in ''|*[!0-9]*) return 1 ;; esac
    while [ ! -b "/dev/block/$name" ] && [ "$attempt" -lt 30 ]; do
        /system/bin/sleep 0.1
        attempt=$((attempt + 1))
    done
    [ -b "/dev/block/$name" ] || return 1
    printf '/dev/block/%s\n' "$name"
}

ace6_crystal_backing_matches() {
    local device owner
    device=$(ace6_crystal_loop_path "$1") || return 1
    owner=$(cat "/sys/block/${device##*/}/loop/backing_file" 2>/dev/null) || return 1
    [ "$owner" = "$ACE6_CRYSTAL_BACKING" ]
}

ace6_crystal_current_backing() {
    local current
    current=$(cat "$ACE6_CRYSTAL_NODE/backing_dev") || return 1
    printf '%s' "$current" | tr -d '()'
}

ace6_crystal_transition_safe() {
    local current
    current=$(ace6_crystal_current_backing) || return 1
    case "$current" in ''|none) return 0 ;; esac
    ace6_crystal_backing_matches "$current"
}

ace6_crystal_config_matches() {
    ace6_crystal_swap_active || return 1
    [ "$(cat "$ACE6_CRYSTAL_NODE/disksize")" = "$ACE6_CRYSTAL_TARGET_BYTES" ] || return 1
    ace6_crystal_codec_selected || return 1
    [ "$(awk '$1=="/dev/block/zram0" || $1=="/dev/zram0" {print $5; exit}' /proc/swaps)" = 0 ] || return 1
    local backing
    backing=$(ace6_crystal_current_backing) || return 1
    if [ "$ACE6_CRYSTAL_WANT_BACKING" = true ]; then
        ace6_crystal_validate_backing && ace6_crystal_backing_matches "$backing"
    else
        [ -z "$backing" ] || [ "$backing" = none ]
    fi
}

ace6_crystal_ram_headroom() {
    local used available swaps
    swaps=$(awk 'NR>1 {print $1}' /proc/swaps) || return 1
    case "$swaps" in ''|/dev/block/zram0|/dev/zram0) ;; *) return 1 ;; esac
    used=$(awk '$1=="/dev/block/zram0" || $1=="/dev/zram0" {print $4; exit}' /proc/swaps)
    used=${used:-0}
    available=$(awk '$1=="MemAvailable:" {print $2; exit}' /proc/meminfo)
    case "$used:$available" in *[!0-9:]*) return 1 ;; esac
    [ -n "$available" ] || return 1
    [ "$used" -le 65536 ] || return 1
    [ "$available" -ge "$((used + 2097152))" ]
}

ace6_crystal_validate_backing() {
    [ -f "$ACE6_CRYSTAL_BACKING" ] && [ ! -L "$ACE6_CRYSTAL_BACKING" ] || return 1
    [ "$(stat -c %s "$ACE6_CRYSTAL_BACKING")" = 8589934592 ] || return 1
    [ "$(stat -c %u:%a "$ACE6_CRYSTAL_BACKING")" = 0:600 ]
}

ace6_crystal_prepare_backing() {
    ace6_crystal_validate_backing || return 1
    # The file must already exist. Early boot never allocates or formats it.
    chcon u:object_r:ksu_file:s0 "$ACE6_CRYSTAL_BACKING" || return 1
    local path owner found=''
    for path in /sys/block/loop*/loop/backing_file; do
        [ -f "$path" ] || continue
        owner=$(cat "$path") || return 1
        [ "$owner" = "$ACE6_CRYSTAL_BACKING" ] || continue
        [ -z "$found" ] || return 1
        found=${path#/sys/block/}
        found=/dev/block/${found%%/*}
    done
    if [ -z "$found" ]; then
        found=$(/system/bin/losetup -f) || return 1
        found=$(ace6_crystal_loop_path "$found") || return 1
        /system/bin/losetup "$found" "$ACE6_CRYSTAL_BACKING" || return 1
        _ACE6_CRYSTAL_CREATED_LOOP=$found
    fi
    ace6_crystal_backing_matches "$found" || return 1
    _ACE6_CRYSTAL_LOOP=$found
    exec 9<"$_ACE6_CRYSTAL_LOOP" || return 1
    /system/bin/dd if="$_ACE6_CRYSTAL_LOOP" of=/dev/null bs=4096 count=1 2>/dev/null
}

ace6_crystal_cleanup_loop() {
    exec 9<&-
    [ -n "${_ACE6_CRYSTAL_CREATED_LOOP:-}" ] || return 0
    local current
    current=$(ace6_crystal_current_backing) || return 0
    [ "$current" != "$_ACE6_CRYSTAL_CREATED_LOOP" ] || return 0
    ace6_crystal_backing_matches "$_ACE6_CRYSTAL_CREATED_LOOP" || return 0
    /system/bin/losetup -d "$_ACE6_CRYSTAL_CREATED_LOOP"
}

ace6_crystal_restore_ram_swap() {
    local active=0 bytes
    ace6_crystal_swap_active || active=$?
    case "$active" in 0) _ACE6_CRYSTAL_RECOVERED=active_swap_preserved; return 0 ;;
        1) ;; *) return 1 ;; esac
    ace6_crystal_transition_safe || return 1
    # If the requested metadata allocation failed, also try the original size.
    for bytes in "$ACE6_CRYSTAL_TARGET_BYTES" "${_ACE6_CRYSTAL_ORIGINAL_BYTES:-0}"; do
        case "$bytes" in ''|0|*[!0-9]*) continue ;; esac
        ace6_crystal_write_node 1 reset || return 1
        ace6_crystal_ensure_codec || return 1
        ace6_crystal_write_node "$bytes" disksize || continue
        /system/bin/mkswap /dev/block/zram0 >/dev/null || continue
        /system/bin/swapon /dev/block/zram0 -p 0 || continue
        _ACE6_CRYSTAL_RECOVERED=yes
        return 0
    done
    return 1
}

ace6_crystal_configure() {
    ace6_crystal_transition_safe || { ace6_crystal_status deferred foreign_backing; return 0; }
    # This candidate only establishes storage. Existing event-driven native
    # policy is ineligible on Crystal; Crystal's periodic policy stays off.
    ace6_crystal_write_node 0 hybridswap_enable || return 1
    ace6_crystal_write_node 0 hybridswap_core_enable || return 1
    if ace6_crystal_config_matches; then
        ace6_crystal_status complete already_matches
        return 0
    fi
    ace6_crystal_ram_headroom || { ace6_crystal_status deferred busy_swap_or_ram_headroom; return 0; }
    if [ "$ACE6_CRYSTAL_WANT_BACKING" = true ]; then
        ace6_crystal_prepare_backing || { ace6_crystal_status deferred private_backing_unavailable; return 0; }
    fi
    _ACE6_CRYSTAL_ORIGINAL_BYTES=$(cat "$ACE6_CRYSTAL_NODE/disksize") || return 1
    local active=0
    ace6_crystal_swap_active || active=$?
    case "$active" in
        0) /system/bin/swapoff /dev/block/zram0 || return 1 ;;
        1) ;; *) return 1 ;;
    esac
    _ACE6_CRYSTAL_TOUCHED_SWAP=yes
    active=0
    ace6_crystal_swap_active || active=$?
    [ "$active" = 1 ] || return 1
    ace6_crystal_transition_safe || return 1
    ace6_crystal_write_node 1 reset || return 1
    ace6_crystal_ensure_codec || return 1
    if [ "$ACE6_CRYSTAL_WANT_BACKING" = true ]; then
        ace6_crystal_write_node "$_ACE6_CRYSTAL_LOOP" backing_dev || return 1
        ace6_crystal_backing_matches "$(ace6_crystal_current_backing)" || return 1
        # A conservative logical-slot cap, not a physical-write accounting rule.
        ace6_crystal_write_node 2097152 writeback_limit || return 1
        ace6_crystal_write_node 1 writeback_limit_enable || return 1
    fi
    ace6_crystal_write_node "$ACE6_CRYSTAL_TARGET_BYTES" disksize || return 1
    /system/bin/mkswap /dev/block/zram0 >/dev/null || return 1
    /system/bin/swapon /dev/block/zram0 -p 0 || return 1
    ace6_crystal_config_matches || return 1
    ace6_crystal_status complete configured
}
