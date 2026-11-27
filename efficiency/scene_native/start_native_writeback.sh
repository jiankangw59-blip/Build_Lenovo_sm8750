#!/system/bin/sh
DIR=${0%/*}
. "$DIR/native_writeback.sh" || exit 1
ace6_native_profile_active || exit 0
STATE=$ACE6_NATIVE_STATE
[ -f "$STATE/native-writeback.conf" ] || exit 0
grep -qx 'native_writeback_auto=true' "$STATE/native-writeback.conf" || exit 0
[ -x "$DIR/native_writeback_event" ] || exit 1
[ -f "$DIR/native_writeback_daemon.sh" ] || exit 1
# The controller takes an exclusive lock; repeated Scene starts are harmless.
nohup "$DIR/native_writeback_event" --exec "$DIR/native_writeback_daemon.sh" \
    </dev/null >>"$STATE/native-writeback-launch.log" 2>&1 &
