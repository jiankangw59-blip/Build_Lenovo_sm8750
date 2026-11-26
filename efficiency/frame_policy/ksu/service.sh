#!/system/bin/sh
module=${0%/*}
if [ "$(getprop sys.boot_completed)" != 1 ]; then
    /data/adb/ksu/bin/resetprop --timeout 120 -w sys.boot_completed 0 >/dev/null 2>&1 || exit 1
fi
exec /system/bin/sh "$module/config.sh" boot
