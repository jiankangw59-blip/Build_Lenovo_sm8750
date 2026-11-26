#!/system/bin/sh
state=/data/adb/frame-policy
module=${0%/*}
if [ -f "$state/applied-boot" ] &&
   [ "$(cat "$state/applied-boot")" = "$(cat /proc/sys/kernel/random/boot_id)" ]; then
    "$module/framectl" disable >/dev/null || exit 1
fi
rm -f "$state/applied-boot"
if [ -f "$state/suspended-legacy-boot" ] && [ -f /data/adb/modules/fas_rs/event-control.sh ]; then
    if [ "$(cat "$state/suspended-legacy-boot")" = "$(cat /proc/sys/kernel/random/boot_id)" ]; then
        sh /data/adb/modules/fas_rs/event-control.sh start
    fi
    rm -f "$state/suspended-legacy-boot"
fi
# User configurations and previous generations remain available for reinstall.
