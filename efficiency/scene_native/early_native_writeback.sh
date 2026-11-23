#!/system/bin/sh
# Before app startup, avoid tearing down a busy Android default swap later.
DIR=${0%/*}
. "$DIR/native_writeback.sh" || exit 0
ace6_native_profile_active || exit 0
STATE=$ACE6_NATIVE_STATE
umask 077
{
    cat /proc/sys/kernel/random/boot_id
    sha256sum /sys/kernel/notes
    cat /proc/version
    dmesg
} > "$STATE/early-kernel-startup.raw" 2>&1
[ -r /data/swap_config.conf ] || exit 0
# Creation/allocation is a service-time operation, never an early-boot task.
[ -f "$ACE6_NATIVE_BACKING" ] && [ ! -L "$ACE6_NATIVE_BACKING" ] || exit 0
[ "$(stat -c %s "$ACE6_NATIVE_BACKING")" = 8589934592 ] || exit 0
used=$(awk '$1=="/dev/block/zram0" || $1=="/dev/zram0" {print $4; exit}' /proc/swaps)
used=${used:-0}
case "$used" in *[!0-9]*) exit 0 ;; esac
if [ "$used" -gt 65536 ]; then
    printf 'result=deferred\nreason=default_swap_already_busy\nused_kb=%s\n' "$used" > "$STATE/early-native-writeback.status"
    exit 0
fi
printf 'result=starting\nused_kb=%s\n' "$used" > "$STATE/early-native-writeback.status"
debug=''
[ ! -f "$STATE/debug-native-init" ] || debug=-x
if sh $debug "$DIR/startup.sh" --native-writeback-only > "$STATE/early-native-writeback.log" 2>&1; then
    printf 'result=complete\n' > "$STATE/early-native-writeback.status"
else
    result=$?
    printf 'result=failed\nexit_code=%s\n' "$result" > "$STATE/early-native-writeback.status"
fi
# Preserve Android boot and the late Scene fallback on an optional setup error.
exit 0
