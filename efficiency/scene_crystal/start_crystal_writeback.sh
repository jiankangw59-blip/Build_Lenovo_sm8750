#!/system/bin/sh
DIR=${0%/*}
. "$DIR/crystal_writeback_policy.sh" || exit 1
# Native/le9uo exits before logs, locks, journals or shared ledger writes.
ace6_crystal_profile_active && cw_load_config && cw_identity && cw_software_counters || exit 0
[ -x "$DIR/native_writeback_event" ] && [ -x "$CW_FLUSH" ] &&
    [ -f "$DIR/crystal_writeback_daemon.sh" ] || exit 1
nohup "$DIR/native_writeback_event" --exec "$DIR/crystal_writeback_daemon.sh" \
    </dev/null >>"$CW_STATE/crystal-writeback-launch.log" 2>&1 &
