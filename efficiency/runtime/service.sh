#!/system/bin/sh
# This companion only applies runtime policy. It contains no module overlays.
umask 077
STATE_DIR=/data/adb/opd2413-kernel-efficiency
MASK_NODE=/sys/devices/virtual/workqueue/cpumask
mkdir -p "$STATE_DIR" || exit 1

model=$(getprop ro.product.model)
if [ "$model" != OPD2413 ]; then
    printf 'status=skipped\nreason=wrong-device\nmodel=%s\n' "$model" > "$STATE_DIR/policy.state"
    exit 0
fi

# A bounded boot wait; there is no resident timer or policy reapply loop.
attempt=0
while [ "$(getprop sys.boot_completed)" != 1 ] && [ "$attempt" -lt 120 ]; do
    sleep 1
    attempt=$((attempt + 1))
done
if [ "$(getprop sys.boot_completed)" != 1 ]; then
    printf 'status=failed\nreason=boot-not-complete\n' > "$STATE_DIR/policy.state"
    exit 1
fi

before_mask=$(cat "$MASK_NODE" 2>/dev/null)
before_ecn=$(cat /proc/sys/net/ipv4/tcp_ecn 2>/dev/null)
if ! printf '3f\n' > "$MASK_NODE"; then
    printf 'status=failed\nreason=workqueue-mask-write\n' > "$STATE_DIR/policy.state"
    exit 1
fi
after_mask=$(cat "$MASK_NODE" 2>/dev/null)
if [ "$after_mask" != 3f ]; then
    printf 'status=failed\nreason=workqueue-mask-readback\nactual=%s\n' "$after_mask" > "$STATE_DIR/policy.state"
    exit 1
fi
if ! printf '2\n' > /proc/sys/net/ipv4/tcp_ecn; then
    printf 'status=failed\nreason=tcp-ecn-write\n' > "$STATE_DIR/policy.state"
    exit 1
fi
after_ecn=$(cat /proc/sys/net/ipv4/tcp_ecn 2>/dev/null)
[ "$after_ecn" = 2 ] || exit 1
printf 'status=applied\nmodel=%s\nworkqueue_mask_before=%s\nworkqueue_mask_after=%s\ntcp_ecn_before=%s\ntcp_ecn_after=%s\n' \
    "$model" "$before_mask" "$after_mask" "$before_ecn" "$after_ecn" > "$STATE_DIR/policy.state"
