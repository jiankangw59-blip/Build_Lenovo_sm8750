#!/system/bin/sh
# Invoked through native_writeback_event --exec to inherit background policy.
DIR=${0%/*}
. "$DIR/native_writeback.sh" || exit 1
umask 077
ace6_native_profile_active || exit 0
STATE=$ACE6_NATIVE_STATE
NODE=/sys/block/zram0
EVENT="$DIR/native_writeback_event"
LOG="$STATE/native-writeback.log"
[ ! -L "$STATE/native-writeback.lock" ] || exit 1
exec 9>"$STATE/native-writeback.lock" || exit 1
flock -n 9 9>&9 || exit 0
watch_pid=''
wait_pid=''
inflight=0
last_round=0
rounds=0
stop=0

log() { printf '%s %s\n' "$(date -u +%FT%TZ)" "$*" >> "$LOG"; }
status() {
    printf 'phase=%s\npid=%s\nrounds=%s\nlast_reason=%s\n' "$1" "$$" "$rounds" "${2:-none}" > "$STATE/native-writeback.status.tmp"
    mv "$STATE/native-writeback.status.tmp" "$STATE/native-writeback.status"
}
valid_number() { case "$1" in ''|*[!0-9]*) return 1 ;; esac; }
load_config() {
    [ -f "$STATE/native-writeback.conf" ] && [ ! -L "$STATE/native-writeback.conf" ] || return 1
    . "$STATE/native-writeback.conf" || return 1
    native_writeback_screen_off_max=${native_writeback_screen_off_max:-1}
    [ "$native_writeback_auto" = true ] || return 1
    for value in "$native_writeback_idle_seconds" "$native_writeback_batch_mb" "$native_writeback_daily_mb" \
                 "$native_writeback_min_zram_mb" "$native_writeback_cooldown_seconds" "$native_writeback_max_temp" "$native_writeback_screen_off_max"; do
        valid_number "$value" || return 1
    done
    [ "$native_writeback_idle_seconds" -ge 300 ] && [ "$native_writeback_idle_seconds" -le 3600 ] &&
    [ "$native_writeback_batch_mb" -ge 4 ] && [ "$native_writeback_batch_mb" -le 256 ] &&
    [ "$native_writeback_daily_mb" -ge "$native_writeback_batch_mb" ] && [ "$native_writeback_daily_mb" -le 8192 ] &&
    [ "$native_writeback_min_zram_mb" -ge 128 ] && [ "$native_writeback_min_zram_mb" -le 4096 ] &&
    [ "$native_writeback_cooldown_seconds" -ge 300 ] && [ "$native_writeback_cooldown_seconds" -le 86400 ] &&
    [ "$native_writeback_max_temp" -ge 300 ] && [ "$native_writeback_max_temp" -le 450 ] &&
    [ "$native_writeback_screen_off_max" -le 7 ]
}
eligible() {
    load_config && ace6_native_profile_active || return 1
    [ "$(cat /sys/class/backlight/panel0-backlight/brightness)" -le "$native_writeback_screen_off_max" ] || return 1
    [ "$(getprop sys.boot_completed)" = 1 ] || return 1
    if [ -f /data/swap_config.conf ]; then
        # Explicit disabling in the existing Scene configuration always wins.
        grep -Eq '^zram_writeback=false([[:space:]]|$)' /data/swap_config.conf && return 1
    fi
    backing=$(cat "$NODE/backing_dev") || return 1
    ace6_native_backing_matches "$backing" || return 1
    case "$(cat "$NODE/comp_algorithm")" in *'[lz4kd]'*) ;; *) return 1 ;; esac
    [ "$(cat "$NODE/writeback_limit_enable")" = 1 ] || return 1
    swaps=$(awk 'NR>1 {print $1}' /proc/swaps) || return 1
    [ "$swaps" = /dev/block/zram0 ] || [ "$swaps" = /dev/zram0 ] || return 1
    # Android mksh tests/arithmetic can be signed 32-bit even on arm64.
    # Scale bytes in awk before comparing, including ZRAM usage above 2 GiB.
    memory_kb=$(awk '{printf "%.0f\n", $3 / 1024}' "$NODE/mm_stat") || return 1
    [ "$memory_kb" -ge "$((native_writeback_min_zram_mb * 1024))" ] || return 1
    temperature=$(cat /sys/class/power_supply/battery/temp) || return 1
    [ "$temperature" -le "$native_writeback_max_temp" ] || return 1
    available=$(awk '$1=="MemAvailable:" {print $2; exit}' /proc/meminfo)
    [ "$available" -ge 524288 ] || return 1
    return 0
}
save_daily() {
    printf '%s %s\n' "$day" "$charged" > "$STATE/native-writeback.daily.tmp" || return 1
    mv "$STATE/native-writeback.daily.tmp" "$STATE/native-writeback.daily"
}
daily_budget() {
    day=$(date -u +%Y%m%d) || return 1
    charged=0
    if [ -e "$STATE/native-writeback.daily" ]; then
        [ -f "$STATE/native-writeback.daily" ] && [ ! -L "$STATE/native-writeback.daily" ] || return 1
        read -r stored_day stored_charged < "$STATE/native-writeback.daily" || return 1
        valid_number "$stored_day" && valid_number "$stored_charged" || return 1
        [ "$stored_day" != "$day" ] || charged=$stored_charged
    fi
    remaining=$((native_writeback_daily_mb * 256 - charged))
    [ "$remaining" -gt 0 ]
}
settle() {
    [ "$inflight" = 1 ] || return 0
    read -r count reads after < "$NODE/bd_stat" || return 1
    [ "$after" -ge "$before" ] || return 1
    [ "$(cat "$NODE/backing_dev")" = "$round_backing" ] || return 1
    written=$((after - before))
    restored=$((saved_limit - written))
    [ "$restored" -ge 0 ] || restored=0
    # Restore only the unused portion of the ORIGINAL boot budget.
    printf '%s\n' "$restored" > "$NODE/writeback_limit" || return 1
    charged=$((charged - reserved + written))
    save_daily || return 1
    inflight=0
}
cleanup() {
    [ -z "$wait_pid" ] || { kill "$wait_pid" 2>/dev/null; wait "$wait_pid" 2>/dev/null; }
    settle || log 'ERROR budget recovery failed; no further writes issued'
    [ -z "$watch_pid" ] || { kill "$watch_pid" 2>/dev/null; wait "$watch_pid" 2>/dev/null; }
    [ ! -p "$STATE/native-writeback.events.$$" ] || rm -f "$STATE/native-writeback.events.$$"
    status stopped
}
trap cleanup EXIT
trap 'stop=1; [ -z "$wait_pid" ] || kill "$wait_pid" 2>/dev/null; exit 0' TERM INT
load_config || exit 0
printf '%s\n' "$$" > "$STATE/native-writeback.pid"
cat /proc/sys/kernel/random/boot_id > "$STATE/native-writeback.boot-id"
boot_id=$(cat /proc/sys/kernel/random/boot_id)
if [ -f "$STATE/native-writeback.last-round" ]; then
    read -r previous_boot previous_round < "$STATE/native-writeback.last-round"
    if [ "$previous_boot" = "$boot_id" ] && valid_number "$previous_round"; then
        last_round=$previous_round
    fi
fi
case "$(stat -c %s "$LOG" 2>/dev/null)" in
    ''|*[!0-9]*) ;;
    *) [ "$(stat -c %s "$LOG")" -lt 65536 ] || mv "$LOG" "$LOG.previous" ;;
esac
log 'START screen/PSI events; nice=19 CPU0-5 io=idle; no periodic idle scan'
mkfifo -m 600 "$STATE/native-writeback.events.$$" || exit 1
export NATIVE_WB_SCREEN_OFF_MAX=$native_writeback_screen_off_max
"$EVENT" --watch > "$STATE/native-writeback.events.$$" 2>> "$LOG" &
watch_pid=$!
exec 3<"$STATE/native-writeback.events.$$" || exit 1
status waiting_events
while IFS= read -r event <&3; do
    [ "$stop" = 0 ] || break
    case "$event" in screen_off|pressure) ;; screen_on) continue ;; *) log 'ERROR invalid event'; exit 1 ;; esac
    eligible || continue
    read -r uptime rest < /proc/uptime
    now=${uptime%%.*}
    [ "$last_round" = 0 ] || [ "$((now - last_round))" -ge "$native_writeback_cooldown_seconds" ] || continue
    daily_budget || { status waiting_events daily_budget_exhausted; continue; }
    log "MARK trigger=$event cold_seconds=$native_writeback_idle_seconds"
    printf 'all\n' > "$NODE/idle" || { log 'ERROR cold marking failed'; exit 1; }
    status waiting_cold_pages "$event"
    read -r uptime rest < /proc/uptime
    deadline=$((${uptime%%.*} + native_writeback_idle_seconds + 1))
    "$EVENT" --grace "$deadline" 3<&3 2>> "$LOG" &
    wait_pid=$!
    grace_rc=0
    wait "$wait_pid" || grace_rc=$?
    wait_pid=''
    case "$grace_rc" in
        0) ;;
        1) log 'CANCEL screen_on during cold grace'; status waiting_events screen_on; continue ;;
        *) log "ERROR event grace failed=$grace_rc"; exit 1 ;;
    esac
    eligible || { log 'SKIP eligibility changed during cold grace'; status waiting_events eligibility_changed; continue; }
    daily_budget || { status waiting_events daily_budget_exhausted; continue; }
    saved_limit=$(cat "$NODE/writeback_limit") || exit 1
    valid_number "$saved_limit" || exit 1
    batch=$((native_writeback_batch_mb * 256))
    [ "$batch" -le "$remaining" ] || batch=$remaining
    [ "$batch" -le "$saved_limit" ] || batch=$saved_limit
    [ "$batch" -gt 0 ] || { status waiting_events boot_budget_exhausted; continue; }
    round_backing=$backing
    read -r count reads before < "$NODE/bd_stat" || exit 1
    reserved=$batch
    charged=$((charged + reserved))
    save_daily || exit 1
    inflight=1
    status writing_idle_pages "$event"
    printf '%s\n' "$batch" > "$NODE/writeback_limit" || exit 1
    wb_rc=0
    printf 'idle\n' > "$NODE/writeback" 2> "$STATE/native-writeback.stderr.tmp" || wb_rc=$?
    settle || { log 'ERROR backing reset or budget settlement failed'; exit 1; }
    if [ "$wb_rc" != 0 ] && [ "$written" -lt "$batch" ]; then
        cat "$STATE/native-writeback.stderr.tmp" >> "$LOG"
        log "ERROR writeback stopped before the cap: pages=$written cap=$batch"
        exit 1
    fi
    rm -f "$STATE/native-writeback.stderr.tmp"
    rounds=$((rounds + 1))
    read -r uptime rest < /proc/uptime
    last_round=${uptime%%.*}
    printf '%s %s\n' "$boot_id" "$last_round" > "$STATE/native-writeback.last-round.tmp"
    mv "$STATE/native-writeback.last-round.tmp" "$STATE/native-writeback.last-round"
    log "DONE trigger=$event pages=$written bytes=$((written * 4096)) cap_reached=$([ "$written" = "$batch" ] && echo yes || echo no) boot_budget_left=$restored daily_pages=$charged"
    status waiting_events "pages_written=$written"
done
log 'STOP event source closed'
