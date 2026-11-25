#!/system/bin/sh
STATE=/data/adb/ace6-kernel-efficiency/policy.state
MASK=/sys/devices/virtual/workqueue/cpumask
[ -r "$STATE" ] || exit 0
# Restore only if the values still match the policy applied by this module.
old_mask=$(sed -n 's/^workqueue_mask_before=//p' "$STATE")
old_ecn=$(sed -n 's/^tcp_ecn_before=//p' "$STATE")
case "$old_mask" in
    ''|*[!0-9a-fA-F,]*) ;;
    *) [ "$(cat "$MASK" 2>/dev/null)" != 3f ] || printf '%s\n' "$old_mask" > "$MASK" ;;
esac
case "$old_ecn" in
    0|1|2) [ "$(cat /proc/sys/net/ipv4/tcp_ecn 2>/dev/null)" != 2 ] || printf '%s\n' "$old_ecn" > /proc/sys/net/ipv4/tcp_ecn ;;
esac
exit 0
