#!/system/bin/sh
DIR=${0%/*}
. "$DIR/crystal_writeback.sh" || exit 0
# An unlisted/native/le9uo kernel exits before creating logs, locks or loops.
ace6_crystal_profile_active || exit 0
umask 077
{
    cat /proc/sys/kernel/random/boot_id
    sha256sum /sys/kernel/notes
    cat /proc/version
    dmesg
} > "$ACE6_CRYSTAL_STATE/early-kernel-startup.raw" 2>&1
ace6_crystal_read_config || {
    ace6_crystal_status deferred configuration_not_requested_or_unsupported
    exit 0
}
[ ! -L "$ACE6_CRYSTAL_STATE/crystal-init.lock" ] || exit 0
exec 8>"$ACE6_CRYSTAL_STATE/crystal-init.lock" || exit 0
flock -n 8 8>&8 || exit 0
trap 'ace6_crystal_cleanup_loop' EXIT
ace6_crystal_status starting configuration
if ace6_crystal_configure > "$ACE6_CRYSTAL_STATE/early-crystal-writeback.log" 2>&1; then
    :
else
    _ACE6_CRYSTAL_RECOVERED=not_attempted
    if [ "${_ACE6_CRYSTAL_TOUCHED_SWAP:-}" = yes ]; then
        _ACE6_CRYSTAL_RECOVERED=no
        ace6_crystal_restore_ram_swap >> "$ACE6_CRYSTAL_STATE/early-crystal-writeback.log" 2>&1 || true
    fi
    ace6_crystal_status failed initialization_failed
fi
# Preserve Android boot and the existing protected late Scene service.
exit 0
