# Crystal event policy and durable accounting. Does not launch itself.
. "${DIR:?}/crystal_writeback.sh" || return 1
CW_STATE=$ACE6_CRYSTAL_STATE
CW_NODE=$ACE6_CRYSTAL_NODE
CW_FLUSH=$DIR/crystal_writeback_flush

cw_number() {
    case "$1" in ''|*[!0-9]*|0[0-9]*) return 1 ;; esac
    # Canonical decimal bounds, without a process per scalar or mksh overflow.
    [ "${#1}" -lt 16 ] || { [ "${#1}" = 16 ] && [ "$1" \< 9007199254740992 ]; }
}
cw_small() {
    cw_number "$1" && { [ "${#1}" -lt 10 ] || { [ "${#1}" = 10 ] && [ "$1" \< 2147483648 ]; }; }
}
cw_file_safe() { [ ! -L "$1" ] && { [ ! -e "$1" ] || [ -f "$1" ]; }; }
cw_state_commit() {
    # Caller writes .tmp.$$ first. Flush both renamed file and its directory.
    cw_file_safe "$1" && cw_file_safe "$1.tmp.$$" || return 1
    mv -f "$1.tmp.$$" "$1" && "$CW_FLUSH" --state "$1"
}
cw_value() {
    awk -v key="$1" -v fallback="$2" '
        BEGIN {value=fallback}
        $0 ~ "^" key "=" {value=substr($0,length(key)+2); sub(/\r$/, "", value)}
        END {print value}' "$CW_STATE/crystal-writeback.conf"
}
cw_load_config() {
    [ ! -e "$CW_STATE/crystal-writeback.blocked" ] &&
        [ ! -L "$CW_STATE/crystal-writeback.blocked" ] || return 1
    [ -f "$CW_STATE/crystal-writeback-auto.enabled" ] &&
        [ ! -L "$CW_STATE/crystal-writeback-auto.enabled" ] || return 1
    [ -f "$CW_STATE/crystal-writeback.conf" ] &&
        [ ! -L "$CW_STATE/crystal-writeback.conf" ] || return 1
    [ "$(cw_value crystal_writeback_auto false)" = true ] || return 1
    CW_IDLE=$(cw_value crystal_writeback_idle_seconds 300)
    CW_BATCH_MB=$(cw_value crystal_writeback_batch_mb 128)
    CW_DAILY_MB=$(cw_value crystal_writeback_daily_mb 2048)
    CW_MIN_ZRAM_MB=$(cw_value crystal_writeback_min_zram_mb 1024)
    CW_INTERVAL=$(cw_value crystal_writeback_cooldown_seconds 900)
    CW_TEMP=$(cw_value crystal_writeback_max_temp 400)
    CW_OFF_MAX=$(cw_value crystal_writeback_screen_off_max 1)
    local value
    for value in "$CW_IDLE" "$CW_BATCH_MB" "$CW_DAILY_MB" "$CW_MIN_ZRAM_MB" \
                 "$CW_INTERVAL" "$CW_TEMP" "$CW_OFF_MAX"; do
        cw_small "$value" || return 1
    done
    [ "$CW_IDLE" -ge 300 ] && [ "$CW_IDLE" -le 3600 ] &&
    [ "$CW_BATCH_MB" -ge 4 ] && [ "$CW_BATCH_MB" -le 128 ] &&
    [ "$CW_DAILY_MB" -ge "$CW_BATCH_MB" ] && [ "$CW_DAILY_MB" -le 2048 ] &&
    [ "$CW_MIN_ZRAM_MB" -ge 128 ] && [ "$CW_MIN_ZRAM_MB" -le 4096 ] &&
    [ "$CW_INTERVAL" -ge 900 ] && [ "$CW_INTERVAL" -le 86400 ] &&
    [ "$CW_TEMP" -ge 300 ] && [ "$CW_TEMP" -le 400 ] && [ "$CW_OFF_MAX" -le 7 ]
}
cw_identity() {
    ace6_crystal_profile_active && ace6_crystal_validate_backing || return 1
    local current loop file_id loop_id zram_id
    current=$(ace6_crystal_current_backing) || return 1
    ace6_crystal_backing_matches "$current" || return 1
    loop=$(ace6_crystal_loop_path "$current") || return 1
    loop_id=$(cat "/sys/block/${loop##*/}/dev") || return 1
    zram_id=$(cat "$CW_NODE/dev") || return 1
    file_id=$(stat -c %d:%i:%s:%u:%a "$ACE6_CRYSTAL_BACKING") || return 1
    CW_BOOT=$(cat /proc/sys/kernel/random/boot_id) || return 1
    CW_NOTES=$(sha256sum /sys/kernel/notes) || return 1
    CW_NOTES=${CW_NOTES%% *}
    CW_ID="${loop##*/},$loop_id,$zram_id,$file_id"
    case "$CW_BOOT:$CW_NOTES:$CW_ID" in *[!a-zA-Z0-9:.,_-]*) return 1 ;; esac
    [ -n "$CW_BOOT" ] && [ "${#CW_NOTES}" = 64 ]
}
cw_eligible() {
    cw_load_config && cw_identity && ace6_crystal_read_config || return 1
    [ "$ACE6_CRYSTAL_WANT_BACKING" = true ] && ace6_crystal_config_matches || return 1
    [ "$(getprop sys.boot_completed)" = 1 ] || return 1
    [ "$(cat "$CW_NODE/hybridswap_enable")" = 0 ] &&
        [ "$(cat "$CW_NODE/hybridswap_core_enable")" = 0 ] || return 1
    [ "$(cat "$CW_NODE/writeback_limit_enable")" = 1 ] || return 1
    [ "$(/system/bin/getconf PAGESIZE)" = 4096 ] || return 1
    local brightness temperature available memory_kb swaps
    brightness=$(cat /sys/class/backlight/panel0-backlight/brightness)
    temperature=$(cat /sys/class/power_supply/battery/temp)
    available=$(awk '$1=="MemAvailable:" {print $2; exit}' /proc/meminfo)
    memory_kb=$(awk '{printf "%.0f",$3/1024}' "$CW_NODE/mm_stat")
    for value in "$brightness" "$temperature" "$available" "$memory_kb"; do
        cw_small "$value" || return 1
    done
    [ "$brightness" -le "$CW_OFF_MAX" ] && [ "$temperature" -le "$CW_TEMP" ] &&
        [ "$available" -ge 524288 ] && [ "$memory_kb" -ge "$((CW_MIN_ZRAM_MB * 1024))" ] || return 1
    swaps=$(awk 'NR>1 {print $1}' /proc/swaps)
    [ "$swaps" = /dev/block/zram0 ] || [ "$swaps" = /dev/zram0 ]
}
cw_counters() {
    local values extra
    values=$(awk '
        /^(enabled|ret|physical_write_pages|physical_write_bytes|physical_read_failed_pages|physical_write_failed_pages|dirty_pages|bd_stat_write_pages):/ {
            key=$1; sub(/:$/, "", key); if (seen[key]++ || NF!=2 || $2 !~ /^[0-9]+$/) bad=1; v[key]=$2
        }
        END {
            if (bad || v["enabled"]!=1 || !seen["ret"] || v["ret"]!=0 || !seen["physical_write_pages"] ||
                !seen["physical_write_bytes"] || !seen["physical_read_failed_pages"] || !seen["physical_write_failed_pages"] ||
                !seen["dirty_pages"] || !seen["bd_stat_write_pages"] ||
                v["physical_write_bytes"] != v["physical_write_pages"]*4096) exit 1
            print v["physical_write_pages"],v["physical_write_failed_pages"],v["dirty_pages"],v["bd_stat_write_pages"],v["physical_read_failed_pages"]
        }' "$CW_NODE/zms_stat") || return 1
    read -r CW_PHYS CW_FAILED CW_DIRTY CW_ZMS_BD CW_READ_FAILED extra <<EOF
$values
EOF
    [ -z "$extra" ] || return 1
    local blocks reads
    read -r blocks reads CW_BD extra < "$CW_NODE/bd_stat" || return 1
    [ -z "$extra" ] && cw_number "$blocks" && cw_number "$reads" && cw_number "$CW_ZMS_BD" || return 1
    cw_number "$CW_PHYS" && cw_number "$CW_FAILED" && cw_number "$CW_READ_FAILED" &&
        cw_number "$CW_DIRTY" && cw_number "$CW_BD"
}
cw_software_counters() {
    local values value extra
    CW_SDDC=0 CW_INTEGRITY=0 CW_DECODE=0 CW_FLATTEN=0 CW_PROOF=0
    if [ -r "$CW_NODE/sddc_stat" ]; then
        values=$(awk '
            /^(enabled|integrity_failures|decode_failures|flatten_failures|delta_proof_failures):/ {
                key=$1; sub(/:$/, "",key); if(seen[key]++ || NF!=2 || $2 !~ /^[0-9]+$/)bad=1; v[key]=$2
            }
            END {
                if(bad || !seen["enabled"])exit 1
                if(v["enabled"]==0){print "0 0 0 0 0"; exit}
                if(v["enabled"]!=1 || !seen["integrity_failures"] || !seen["decode_failures"] ||
                    !seen["flatten_failures"] || !seen["delta_proof_failures"])exit 1
                print "1",v["integrity_failures"],v["decode_failures"],v["flatten_failures"],v["delta_proof_failures"]
            }' "$CW_NODE/sddc_stat") || return 1
        read -r CW_SDDC CW_INTEGRITY CW_DECODE CW_FLATTEN CW_PROOF extra <<EOF
$values
EOF
        [ -z "$extra" ] || return 1
    fi
    # The pinned public source does not yet export integrity_failures. Such an
    # LZ4KDS kernel remains ineligible until a separately reviewed stats export.
    [ "$ACE6_CRYSTAL_CODEC" != lz4kds ] || [ "$CW_SDDC" = 1 ] || return 1
    read -r CW_IO_READ_FAILED CW_IO_WRITE_FAILED value extra < "$CW_NODE/io_stat" || return 1
    cw_number "$value" && cw_number "$extra" || return 1
    for value in "$CW_INTEGRITY" "$CW_DECODE" "$CW_FLATTEN" "$CW_PROOF" \
                 "$CW_IO_READ_FAILED" "$CW_IO_WRITE_FAILED"; do
        cw_number "$value" || return 1
    done
}
cw_daily_read() {
    CW_DAY=$(date -u +%Y%m%d) || return 1
    [ "${#CW_DAY}" = 8 ] && cw_number "$CW_DAY" || return 1
    CW_CHARGED=0
    CW_STORED_DAY=''
    local day pages extra
    if [ -e "$CW_STATE/native-writeback.daily" ]; then
        cw_file_safe "$CW_STATE/native-writeback.daily" || return 1
        read -r day pages extra < "$CW_STATE/native-writeback.daily" || return 1
        [ -z "$extra" ] && [ "${#day}" = 8 ] && cw_number "$day" && cw_number "$pages" || return 1
        # A backwards or invalid wall clock must never refill the daily budget.
        [ "$CW_DAY" -ge "$day" ] || return 1
        CW_STORED_DAY=$day
        [ "$CW_DAY" != "$day" ] || CW_CHARGED=$pages
    fi
}
cw_daily_save() {
    cw_file_safe "$CW_STATE/native-writeback.daily" &&
        cw_file_safe "$CW_STATE/native-writeback.daily.tmp.$$" || return 1
    printf '%s %s\n' "$CW_DAY" "$CW_CHARGED" > "$CW_STATE/native-writeback.daily.tmp.$$" &&
        cw_state_commit "$CW_STATE/native-writeback.daily"
}
cw_observe_physical() {
    # Called only while holding native-writeback.lock (also used by native).
    cw_identity && cw_counters && cw_daily_read || return 1
    local old_boot old_notes old_id old_count extra delta update_watermark=1
    old_count=0
    if [ -e "$CW_STATE/crystal-writeback.physical" ]; then
        cw_file_safe "$CW_STATE/crystal-writeback.physical" || return 1
        read -r old_boot old_notes old_id old_count extra < "$CW_STATE/crystal-writeback.physical" || return 1
        [ -z "$extra" ] && cw_number "$old_count" || return 1
        if [ "$old_boot" = "$CW_BOOT" ]; then
            [ "$old_notes" = "$CW_NOTES" ] && [ "$old_id" = "$CW_ID" ] || return 1
            [ "$old_count" != "$CW_PHYS" ] || update_watermark=0
        else
            old_count=0
        fi
    fi
    delta=$(awk -v n="$CW_PHYS" -v o="$old_count" 'BEGIN {if(n<o)exit 1; printf "%.0f",n-o}') || return 1
    CW_CHARGED=$(awk -v n="$CW_CHARGED" -v d="$delta" 'BEGIN {printf "%.0f",n+d}')
    cw_number "$CW_CHARGED" || return 1
    # Ledger first, watermark second: a crash may overcharge, never refund.
    if [ "$delta" != 0 ] || [ "$CW_STORED_DAY" != "$CW_DAY" ]; then
        cw_daily_save || return 1
    fi
    if [ "$update_watermark" = 1 ]; then
        cw_file_safe "$CW_STATE/crystal-writeback.physical.tmp.$$" || return 1
        printf '%s %s %s %s\n' "$CW_BOOT" "$CW_NOTES" "$CW_ID" "$CW_PHYS" \
            > "$CW_STATE/crystal-writeback.physical.tmp.$$" &&
            cw_state_commit "$CW_STATE/crystal-writeback.physical" || return 1
    fi
    CW_REMAINING=$(awk -v mb="$CW_DAILY_MB" -v used="$CW_CHARGED" \
        'BEGIN {left=mb*256-used; printf "%.0f",(left>0?left:0)}')
}
cw_pending_save() {
    cw_file_safe "$CW_STATE/crystal-writeback.pending" &&
        cw_file_safe "$CW_STATE/crystal-writeback.pending.tmp.$$" || return 1
    printf 'v2 %s %s %s %s %s %s %s %s %s %s %s %s %s %s %s %s %s %s %s %s\n' \
        "$P_BOOT" "$P_NOTES" "$P_ID" "$P_SAVED" "$P_CAP" "$P_PHASE" \
        "$P_REMAIN" "$P_RESTORE" "$P_CONSUMED" "$P_PHYS" "$P_FAILED" "$P_BD" \
        "$P_SDDC" "$P_INTEGRITY" "$P_DECODE" "$P_FLATTEN" "$P_PROOF" "$P_IO_READ" "$P_IO_WRITE" "$P_READ_FAILED" \
        > "$CW_STATE/crystal-writeback.pending.tmp.$$" &&
        cw_state_commit "$CW_STATE/crystal-writeback.pending"
}
cw_pending_load() {
    P_PHASE=none
    [ -e "$CW_STATE/crystal-writeback.pending" ] || return 0
    cw_file_safe "$CW_STATE/crystal-writeback.pending" || return 1
    local version extra value
    read -r version P_BOOT P_NOTES P_ID P_SAVED P_CAP P_PHASE P_REMAIN P_RESTORE P_CONSUMED \
        P_PHYS P_FAILED P_BD P_SDDC P_INTEGRITY P_DECODE P_FLATTEN P_PROOF P_IO_READ P_IO_WRITE P_READ_FAILED \
        extra < "$CW_STATE/crystal-writeback.pending" || return 1
    [ "$version" != none ] || { [ -z "$P_BOOT" ] || return 1; P_PHASE=none; return 0; }
    # v1 has no physical-read error baseline. Never infer a zero baseline or
    # migrate an in-flight journal: old/truncated records require review.
    [ "$version" = v2 ] && [ -z "$extra" ] || return 1
    case "$P_PHASE" in armed|restoring|restored) ;; *) return 1 ;; esac
    for value in "$P_SAVED" "$P_CAP" "$P_REMAIN" "$P_RESTORE" "$P_CONSUMED"; do
        cw_small "$value" || return 1
    done
    for value in "$P_PHYS" "$P_FAILED" "$P_READ_FAILED" "$P_BD" "$P_INTEGRITY" "$P_DECODE" \
                 "$P_FLATTEN" "$P_PROOF" "$P_IO_READ" "$P_IO_WRITE"; do
        cw_number "$value" || return 1
    done
    case "$P_SDDC" in 0|1) ;; *) return 1 ;; esac
    [ "$P_CAP" -gt 0 ] && [ "$P_CAP" -le "$P_SAVED" ] &&
        [ "$P_REMAIN" -le "$P_CAP" ] && [ "$P_CONSUMED" -le "$P_CAP" ]
}
cw_pending_clear() {
    printf 'none\n' > "$CW_STATE/crystal-writeback.pending.tmp.$$" &&
        cw_state_commit "$CW_STATE/crystal-writeback.pending"
}
cw_settle_pending() {
    cw_pending_load || return 1
    [ "$P_PHASE" != none ] || return 0
    cw_identity || return 1
    [ "$P_BOOT" = "$CW_BOOT" ] && [ "$P_NOTES" = "$CW_NOTES" ] && [ "$P_ID" = "$CW_ID" ] || return 1
    # Reject an observable reset before restoring any old logical allowance.
    cw_counters || return 1
    awk -v p="$CW_PHYS" -v op="$P_PHYS" -v f="$CW_FAILED" -v of="$P_FAILED" \
        -v r="$CW_READ_FAILED" -v orf="$P_READ_FAILED" \
        -v b="$CW_BD" -v ob="$P_BD" 'BEGIN {exit !(p>=op && f>=of && r>=orf && b>=ob)}' || return 1
    local current
    current=$(cat "$CW_NODE/writeback_limit") || return 1
    cw_small "$current" || return 1
    if [ "$P_PHASE" = armed ]; then
        if [ "$current" = "$P_SAVED" ] && [ "$P_SAVED" -gt "$P_CAP" ]; then
            # Journal was durable but the per-round cap was never installed.
            P_REMAIN=$P_CAP
        else
            [ "$current" -le "$P_CAP" ] || return 1
            P_REMAIN=$current
        fi
        P_CONSUMED=$((P_CAP - P_REMAIN))
        P_RESTORE=$((P_SAVED - P_CONSUMED))
        P_PHASE=restoring
        cw_pending_save || return 1
    fi
    if [ "$P_PHASE" = restoring ]; then
        if [ "$current" = "$P_RESTORE" ]; then
            : # Recovery after restore succeeded but before its phase persisted.
        elif [ "$current" = "$P_REMAIN" ]; then
            printf '%s\n' "$P_RESTORE" > "$CW_NODE/writeback_limit" || return 1
        else
            return 1
        fi
        P_PHASE=restored
        cw_pending_save || return 1
    fi
    # fsync may succeed without flushing while the driver is sleeping: dirty=0
    # and counters below are mandatory independent checks, not just helper rc.
    "$CW_FLUSH" --block || return 1
    # Use the observer's final physical snapshot for both charging and errors,
    # then read software/front-end errors. Do not discard errors by calculating
    # deltas from an earlier snapshot and refreshing the counters afterwards.
    cw_observe_physical && cw_software_counters || return 1
    [ "$CW_DIRTY" = 0 ] || return 1
    CW_ROUND_PHYSICAL=$(awk -v n="$CW_PHYS" -v o="$P_PHYS" 'BEGIN {if(n<o)exit 1; printf "%.0f",n-o}') || return 1
    CW_ROUND_FAILED=$(awk -v n="$CW_FAILED" -v o="$P_FAILED" 'BEGIN {if(n<o)exit 1; printf "%.0f",n-o}') || return 1
    CW_ROUND_READ_FAILED=$(awk -v n="$CW_READ_FAILED" -v o="$P_READ_FAILED" 'BEGIN {if(n<o)exit 1; printf "%.0f",n-o}') || return 1
    CW_ROUND_BD=$(awk -v n="$CW_BD" -v o="$P_BD" 'BEGIN {if(n<o)exit 1; printf "%.0f",n-o}') || return 1
    CW_ROUND_SOFTWARE_ERROR=0
    [ "$CW_SDDC:$CW_INTEGRITY:$CW_DECODE:$CW_FLATTEN:$CW_PROOF:$CW_IO_READ_FAILED:$CW_IO_WRITE_FAILED" = \
      "$P_SDDC:$P_INTEGRITY:$P_DECODE:$P_FLATTEN:$P_PROOF:$P_IO_READ:$P_IO_WRITE" ] || CW_ROUND_SOFTWARE_ERROR=1
    CW_ROUND_LOGICAL=$P_CONSUMED
    CW_ROUND_LIMIT_LEFT=$P_REMAIN
    CW_ROUND_RESTORED=$P_RESTORE
    CW_ROUND_CAP=$P_CAP
    # Preserve the durable journal on data-path errors; a service restart must
    # not silently turn the same failure into a fresh successful baseline.
    [ "$CW_ROUND_FAILED" = 0 ] && [ "$CW_ROUND_READ_FAILED" = 0 ] &&
        [ "$CW_ROUND_SOFTWARE_ERROR" = 0 ] || return 2
    cw_pending_clear
}
cw_begin_round() {
    cw_pending_load && [ "$P_PHASE" = none ] || return 1
    cw_observe_physical && cw_software_counters || return 1
    P_SAVED=$(cat "$CW_NODE/writeback_limit") || return 1
    cw_small "$P_SAVED" || return 1
    P_CAP=$((CW_BATCH_MB * 256))
    [ "$P_CAP" -le "$CW_REMAINING" ] || P_CAP=$CW_REMAINING
    [ "$P_CAP" -le "$P_SAVED" ] || P_CAP=$P_SAVED
    [ "$P_CAP" -gt 0 ] || return 2
    P_BOOT=$CW_BOOT P_NOTES=$CW_NOTES P_ID=$CW_ID
    P_PHASE=armed P_REMAIN=$P_CAP P_RESTORE=$P_SAVED P_CONSUMED=0
    P_PHYS=$CW_PHYS P_FAILED=$CW_FAILED P_READ_FAILED=$CW_READ_FAILED P_BD=$CW_BD
    P_SDDC=$CW_SDDC P_INTEGRITY=$CW_INTEGRITY P_DECODE=$CW_DECODE
    P_FLATTEN=$CW_FLATTEN P_PROOF=$CW_PROOF P_IO_READ=$CW_IO_READ_FAILED P_IO_WRITE=$CW_IO_WRITE_FAILED
    cw_pending_save || return 1
    printf '%s\n' "$P_CAP" > "$CW_NODE/writeback_limit"
}
cw_block() {
    cw_file_safe "$CW_STATE/crystal-writeback.blocked" &&
        cw_file_safe "$CW_STATE/crystal-writeback.blocked.tmp.$$" || return 1
    printf 'boot_id=%s\nnotes=%s\nreason=%s\n' "$CW_BOOT" "$CW_NOTES" "$1" \
        > "$CW_STATE/crystal-writeback.blocked.tmp.$$" &&
        cw_state_commit "$CW_STATE/crystal-writeback.blocked"
}
cw_mark_attempt() {
    local uptime rest
    read -r uptime rest < /proc/uptime || return 1
    CW_LAST_ROUND=${uptime%%.*}
    cw_small "$CW_LAST_ROUND" || return 1
    cw_file_safe "$CW_STATE/crystal-writeback.last-round" &&
        cw_file_safe "$CW_STATE/crystal-writeback.last-round.tmp.$$" || return 1
    printf '%s %s\n' "$CW_BOOT" "$CW_LAST_ROUND" > "$CW_STATE/crystal-writeback.last-round.tmp.$$" &&
        cw_state_commit "$CW_STATE/crystal-writeback.last-round"
}
