#!/system/bin/sh
DIR=${0%/*}
. "$DIR/crystal_writeback.sh" || exit 1

# Enrollment is restricted to builds with the repaired ZMS backing BIO path.
selection=$ACE6_CRYSTAL_STATE/crystal-kernel-writeback.enabled
[ -f "$selection" ] && [ ! -L "$selection" ] || exit 0
ace6_crystal_profile_active || exit 0
notes=$(sha256sum /sys/kernel/notes) || exit 1
notes=${notes%% *}
grep -Fqx "$notes" "$selection" || exit 0

umask 077
status=$ACE6_CRYSTAL_STATE/crystal-kernel-writeback.status
early_status=$ACE6_CRYSTAL_STATE/early-crystal-writeback.status
boot_id=$(cat /proc/sys/kernel/random/boot_id) || exit 1
buffers=/dev/memcg/memory.avail_buffers

record_status() {
    {
        printf 'result=%s\nreason=%s\npolicy=crystal_kernel\n' "$1" "$2"
        printf 'boot_id=%s\nnotes_sha256=%s\ncodec=%s\n' "$boot_id" "$notes" "$ACE6_CRYSTAL_CODEC"
        printf 'avail_buffers_mib=%s\n' "${watermarks:-unknown}"
    } > "$status.tmp.$$" && mv -f "$status.tmp.$$" "$status"
}

read_watermarks() {
    awk '
        $1=="avail_buffers:"     && $2 ~ /^[0-9]+$/ {a=$2; n++}
        $1=="min_avail_buffers:" && $2 ~ /^[0-9]+$/ {m=$2; n++}
        $1=="high_avail_buffers:" && $2 ~ /^[0-9]+$/ {h=$2; n++}
        $1=="free_swap_threshold:" && $2 ~ /^[0-9]+$/ {s=$2; n++}
        END {
            if (n!=4 || a=="" || m=="" || h=="" || s=="") exit 1
            print a, m, h, s
        }' "$buffers"
}

kernel_policy_active() {
    [ "$(cat "$ACE6_CRYSTAL_NODE/hybridswap_enable")" = "hybridswap enable reclaim_in enable swapd enable" ] &&
    [ "$(cat "$ACE6_CRYSTAL_NODE/hybridswap_core_enable")" = "hybridswap enable reclaim_in enable" ]
}

writeback_budget_available() {
    [ "$(cat "$ACE6_CRYSTAL_NODE/writeback_limit_enable")" = 1 ] &&
    awk 'NR==1 && $1 ~ /^[0-9]+$/ && $1>0 {ok=1} END {exit !ok}' "$ACE6_CRYSTAL_NODE/writeback_limit"
}

if ! grep -Fqx "boot_id=$boot_id" "$early_status" ||
   ! grep -Fqx 'result=complete' "$early_status" ||
   ! ace6_crystal_read_config ||
   [ "$ACE6_CRYSTAL_WANT_BACKING" != true ] ||
   ! ace6_crystal_config_matches ||
   ! awk '$1=="enabled:" && $2==1 {found=1} END {exit !found}' "$ACE6_CRYSTAL_NODE/zms_stat"; then
    record_status deferred crystal_backing_not_ready
    exit 0
fi

# The performance HAL can briefly pause swapd during startup.
if [ "$(cat "$ACE6_CRYSTAL_NODE/hybridswap_swapd_pause")" = 1 ] &&
   writeback_budget_available; then
    sleep 3
fi
if [ "$(cat "$ACE6_CRYSTAL_NODE/hybridswap_swapd_pause")" != 0 ] ||
   ! writeback_budget_available; then
    record_status deferred paused_or_no_writeback_budget
    exit 0
fi

watermarks=$(read_watermarks) || {
    record_status failed unreadable_memory_watermarks
    exit 1
}

# The kernel needs a configured pressure threshold before it schedules work.
# Preserve existing OEM/ERM policy values when they are already configured.
seeded=no
if [ "$watermarks" = '0 0 0 0' ] &&
   ! grep -Fqx 'effective_avail_buffers_source: erm' "$buffers"; then
    defaults='1024 1024 1536 1024'
    if ! printf '%s\n' "$defaults" > "$buffers"; then
        record_status failed cannot_configure_memory_watermarks
        exit 1
    fi
    watermarks=$(read_watermarks) || watermarks=unknown
    if [ "$watermarks" != "$defaults" ]; then
        record_status failed memory_watermarks_readback
        exit 1
    fi
    seeded=yes
fi

if ! kernel_policy_active; then
    if ! ace6_crystal_write_node 1 hybridswap_enable || ! kernel_policy_active; then
        ace6_crystal_write_node 0 hybridswap_enable
        if [ "$seeded" = yes ] && [ "$(read_watermarks)" = "$defaults" ]; then
            printf '0 0 0 0\n' > "$buffers"
        fi
        watermarks=$(read_watermarks) || watermarks=unknown
        record_status failed kernel_enable_readback
        exit 1
    fi
fi

# ColorOS boot services can reset the global value while apps keep Scene's setting.
if [ "$ACE6_CRYSTAL_MODEL" = OPD2413 ]; then
    swappiness=$(ace6_crystal_config_value swappiness '')
else
    swappiness=$(awk -F= '$1=="swappiness" && $2 ~ /^[0-9]+$/ {print $2}' "$ACE6_CRYSTAL_CONFIG")
fi
if [ -n "$swappiness" ] && [ "$swappiness" -le 200 ] 2>/dev/null &&
   [ "$(cat /dev/memcg/apps/memory.swappiness 2>/dev/null)" = "$swappiness" ] &&
   [ "$(cat /proc/sys/vm/swappiness)" != "$swappiness" ]; then
    printf '%s\n' "$swappiness" > /proc/sys/vm/swappiness || true
fi

record_status enabled configured
