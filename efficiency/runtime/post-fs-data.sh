#!/system/bin/sh
set -eu
umask 077
module=${0%/*}
. "$module/enrolled.sh"
opd2413_enrolled || exit 0
[ ! -L "$state" ] || exit 1
mkdir -p "$state"

initialize_setting() {
    [ ! -L "$state/$1" ] || return 1
    [ -f "$state/$1" ] && return 0
    cp "$module/defaults/$1" "$state/$1.tmp.$$"
    mv -f "$state/$1.tmp.$$" "$state/$1"
}

initialize_setting swap_config.conf
printf '%s lz4kds\n' "$notes" > "$state/known-crystal-kernels.tmp.$$"
mv -f "$state/known-crystal-kernels.tmp.$$" "$state/known-crystal-kernels"
for key in crystal-writeback.enabled crystal-kernel-writeback.enabled; do
    [ ! -L "$state/$key" ] || exit 1
    printf '%s\n' "$notes" > "$state/$key.tmp.$$"
    mv -f "$state/$key.tmp.$$" "$state/$key"
done
if [ -c /dev/xcy_reclaim ] && [ -c /dev/xcy_reclaim_events ]; then
    if /data/adb/ksu/bin/ksud sepolicy apply "$module/xcy_sepolicy.rule" &&
       sh "$module/prepare_xcy_devices.sh"; then
        cat /proc/sys/kernel/random/boot_id > "$state/xcy-devices-labeled"
    fi
fi
sh "$module/early_crystal_writeback.sh"
