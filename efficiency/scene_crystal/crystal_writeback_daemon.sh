#!/system/bin/sh
# Event provider/priority implementation is the unchanged native event binary.
DIR=${0%/*}
. "$DIR/crystal_writeback_policy.sh" || exit 1
ace6_crystal_profile_active && cw_load_config && cw_identity && cw_software_counters || exit 0
umask 077
LOG=$CW_STATE/crystal-writeback.log
EVENT=$DIR/native_writeback_event
cw_file_safe "$CW_STATE/native-writeback.lock" || exit 1
exec 9>"$CW_STATE/native-writeback.lock" || exit 1
flock -n 9 9>&9 || exit 0
watch_pid='' wait_pid='' rounds=0 stop=0 failed=0
CW_LAST_ROUND=0
log() { printf '%s %s\n' "$(date -u +%FT%TZ)" "$*" >> "$LOG"; }
status() {
    {
        printf 'phase=%s\npid=%s\nboot_id=%s\nrounds=%s\nreason=%s\n' "$1" "$$" "$CW_BOOT" "$rounds" "${2:-none}"
        printf 'charged_physical_pages=%s\nphysical_hard_cap=false\n' "${CW_CHARGED:-unknown}"
    } > "$CW_STATE/crystal-writeback.status.tmp.$$" &&
        mv -f "$CW_STATE/crystal-writeback.status.tmp.$$" "$CW_STATE/crystal-writeback.status"
}
fatal() {
    failed=1
    log "ERROR $1; no further automatic submissions"
    cw_block "$1" || log 'ERROR could not persist failure latch; preserve pending journal'
    status blocked "$1"
    exit 1
}
cleanup() {
    [ -z "$wait_pid" ] || { kill "$wait_pid" 2>/dev/null; wait "$wait_pid" 2>/dev/null; }
    # Once a syscall submitted data, cancellation still settles/flushed data;
    # it is never reported as a successful no-I/O screen-on cancellation.
    if [ "$failed" = 0 ]; then
        recovery_rc=0
        cw_settle_pending || recovery_rc=$?
        [ "$recovery_rc" != 0 ] || { cw_observe_physical || recovery_rc=1; }
        if [ "$recovery_rc" != 0 ]; then
            failed=1
            cw_block interrupted_round_recovery_failed || true
            log 'ERROR interrupted round retained its journal and budget; review required'
        fi
    fi
    [ -z "$watch_pid" ] || { kill "$watch_pid" 2>/dev/null; wait "$watch_pid" 2>/dev/null; }
    [ ! -p "$CW_STATE/crystal-writeback.events.$$" ] || rm -f "$CW_STATE/crystal-writeback.events.$$"
    [ "$failed" != 0 ] || status stopped
}
trap cleanup EXIT
trap 'stop=1; [ -z "$wait_pid" ] || kill "$wait_pid" 2>/dev/null; exit 0' TERM INT

cw_settle_pending || fatal pending_recovery_failed
cw_observe_physical || fatal initial_accounting_failed
if [ -e "$CW_STATE/crystal-writeback.last-round" ]; then
    cw_file_safe "$CW_STATE/crystal-writeback.last-round" || fatal invalid_last_round
    read -r previous_boot previous_round extra < "$CW_STATE/crystal-writeback.last-round" || fatal invalid_last_round
    [ -z "$extra" ] && cw_small "$previous_round" || fatal invalid_last_round
    [ "$previous_boot" != "$CW_BOOT" ] || CW_LAST_ROUND=$previous_round
fi
printf '%s\n' "$$" > "$CW_STATE/crystal-writeback.pid"
cat /proc/sys/kernel/random/boot_id > "$CW_STATE/crystal-writeback.boot-id"
size=$(stat -c %s "$LOG" 2>/dev/null)
case "$size" in ''|*[!0-9]*) ;; *) [ "$size" -lt 65536 ] || mv -f "$LOG" "$LOG.previous" ;; esac
log 'START event-driven Crystal; nice19 CPU0-5 ioidle; driver policy remains disabled'
mkfifo -m 600 "$CW_STATE/crystal-writeback.events.$$" || fatal event_fifo_failed
export NATIVE_WB_SCREEN_OFF_MAX=$CW_OFF_MAX
"$EVENT" --watch > "$CW_STATE/crystal-writeback.events.$$" 2>> "$LOG" &
watch_pid=$!
exec 3<"$CW_STATE/crystal-writeback.events.$$" || fatal event_fifo_open_failed
status waiting_events
while IFS= read -r event <&3; do
    [ "$stop" = 0 ] || break
    case "$event" in screen_off|pressure|screen_on) ;; *) fatal invalid_event ;; esac
    cw_observe_physical || fatal event_accounting_failed
    [ "$event" != screen_on ] || { status waiting_events screen_on; continue; }
    cw_eligible || continue
    read -r uptime rest < /proc/uptime
    now=${uptime%%.*}
    cw_small "$now" || fatal invalid_uptime
    [ "$CW_LAST_ROUND" = 0 ] || [ "$((now - CW_LAST_ROUND))" -ge "$CW_INTERVAL" ] || continue
    [ "$CW_REMAINING" -gt 0 ] || { status waiting_events daily_target_reached; continue; }
    printf 'all\n' > "$CW_NODE/idle" || fatal cold_mark_failed
    log "MARK trigger=$event continuous_cold_seconds=$CW_IDLE"
    status waiting_cold_pages "$event"
    deadline=$((now + CW_IDLE + 1))
    "$EVENT" --grace "$deadline" 3<&3 2>> "$LOG" &
    wait_pid=$!
    grace_rc=0
    wait "$wait_pid" || grace_rc=$?
    wait_pid=''
    case "$grace_rc" in
        0) ;;
        1) log 'CANCEL screen_on during grace; no writeback submitted'; status waiting_events screen_on; continue ;;
        *) fatal event_grace_failed ;;
    esac
    cw_eligible || { status waiting_events eligibility_changed; continue; }
    cw_observe_physical || fatal before_round_accounting_failed
    [ "$CW_REMAINING" -gt 0 ] || { status waiting_events daily_target_reached; continue; }
    cw_mark_attempt || fatal interval_journal_failed
    begin_rc=0
    cw_begin_round || begin_rc=$?
    case "$begin_rc" in
        0) ;;
        2) status waiting_events boot_or_daily_budget_exhausted; continue ;;
        *) fatal round_preparation_failed ;;
    esac
    status writing_idle_pages "$event"
    wb_rc=0
    printf 'idle\n' > "$CW_NODE/writeback" 2> "$CW_STATE/crystal-writeback.stderr.tmp" || wb_rc=$?
    settle_rc=0
    cw_settle_pending || settle_rc=$?
    [ "$settle_rc" = 0 ] || fatal "settlement_failed_rc=$settle_rc"
    cap_stop=no
    if [ "$wb_rc" != 0 ]; then
        if [ "$CW_ROUND_LIMIT_LEFT" = 0 ] && [ "$CW_ROUND_LOGICAL" = "$CW_ROUND_CAP" ] &&
           [ "$CW_ROUND_FAILED" = 0 ] && [ "$CW_ROUND_READ_FAILED" = 0 ] &&
           [ "$CW_ROUND_SOFTWARE_ERROR" = 0 ]; then
            cap_stop=yes
        else
            cat "$CW_STATE/crystal-writeback.stderr.tmp" >> "$LOG"
            fatal partial_writeback_error
        fi
    fi
    rm -f "$CW_STATE/crystal-writeback.stderr.tmp"
    rounds=$((rounds + 1))
    log "DONE trigger=$event logical_consumed=$CW_ROUND_LOGICAL physical_pages=$CW_ROUND_PHYSICAL bd_stat_pages=$CW_ROUND_BD quota_stop=$cap_stop restored_logical_limit=$CW_ROUND_RESTORED daily_physical_pages=$CW_CHARGED"
    [ "$CW_REMAINING" -gt 0 ] && reason=round_complete || reason=daily_target_reached
    status waiting_events "$reason"
done
log 'STOP event source closed'
