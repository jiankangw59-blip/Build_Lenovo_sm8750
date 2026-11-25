#!/system/bin/sh
set -eu

# Standard KernelSU post-fs-data entry. The root control node retains its
# boot default. Only the publisher and event reader are accessible to system.
publisher=/dev/xcy_reclaim
notices=/dev/xcy_reclaim_events
if [ ! -c "$publisher" ] || [ ! -c "$notices" ]; then
    exit 0
fi

label=u:object_r:xcy_reclaim_device:s0
chcon "$label" "$publisher" "$notices"
chown 0:1000 "$publisher" "$notices"
chmod 0660 "$publisher"
chmod 0440 "$notices"
