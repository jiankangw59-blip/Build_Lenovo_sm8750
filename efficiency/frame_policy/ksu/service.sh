#!/system/bin/sh
module=${0%/*}
state=/data/adb/frame-policy
resetprop=/data/adb/ksu/bin/resetprop
startup_budget=120
umask 077

wait_boot_completed() {
    bootprop_before=$(getprop sys.boot_completed)
    if [ "$bootprop_before" != 1 ]; then
        "$resetprop" --timeout "$startup_budget" -w sys.boot_completed 0
    fi
}

requires_vendor_post_boot() {
    [ "$(getprop ro.product.model)" = PLQ110 ] || return 1
    active=$module/defaults
    if [ -f "$state/current" ]; then
        name=$(cat "$state/current") || return 1
        case "$name" in *[!a-zA-Z0-9._-]*) return 1;; esac
        case "$name" in
            defaults) ;;
            generation.*) active=$state/$name ;;
            *) return 1 ;;
        esac
    fi
    [ -d "$active" ] && [ ! -L "$active" ] &&
        [ "$(cat "$active/enabled" 2>/dev/null)" = 1 ] &&
        [ "$(cat "$active/aux-required" 2>/dev/null)" = 1 ]
}

wait_vendor_post_boot() {
    # The sun vendor script publishes this after installing both WALT policies.
    vendorprop_before=$(getprop vendor.post_boot.parsed)
    vendorprop_after=$vendorprop_before
    vendor_wait_exit=0
    while [ "$vendorprop_after" != 1 ]; do
        case "$vendorprop_after" in
            ''|0) ;;
            *) echo "Unexpected vendor.post_boot.parsed value" >&2; return 1 ;;
        esac
        read -r uptime_now _ < /proc/uptime || return 1
        # Round down the remaining whole seconds to keep the shared deadline.
        vendor_wait_budget=$((startup_budget - (${uptime_now%%.*} - ${uptime_before_wait%%.*}) - 1))
        if [ "$vendor_wait_budget" -le 0 ]; then
            stage=wait_vendor_post_boot_budget
            echo "Boot readiness budget expired before vendor.post_boot.parsed=1" >&2
            return 1
        fi
        vendorprop_old=$vendorprop_after
        "$resetprop" --timeout "$vendor_wait_budget" -w vendor.post_boot.parsed "$vendorprop_old"
        vendor_wait_exit=$?
        vendorprop_after=$(getprop vendor.post_boot.parsed)
        if [ "$vendor_wait_exit" != 0 ] || [ "$vendorprop_after" = "$vendorprop_old" ]; then
            echo "Could not wait for vendor.post_boot.parsed=1 (resetprop exit $vendor_wait_exit)" >&2
            return 1
        fi
    done
}

wait_startup() {
    stage=wait_boot_completed
    vendor_wait_exit=skipped
    read -r uptime_before_wait _ < /proc/uptime || return 1
    wait_boot_completed
    wait_exit=$?
    bootprop_after=$(getprop sys.boot_completed)
    read -r uptime_after_wait _ < /proc/uptime
    if [ "$wait_exit" != 0 ] || [ "$bootprop_after" != 1 ]; then
        echo "Could not wait for sys.boot_completed=1 (resetprop exit $wait_exit)" >&2
        return 1
    fi
    if requires_vendor_post_boot; then
        stage=wait_vendor_post_boot
        wait_vendor_post_boot || return 1
    fi
}

run_boot() {
    wait_startup
    command_exit=$?
    [ "$command_exit" = 0 ] || return "$command_exit"
    stage=config_boot
    if [ -f "$state/trace-next-boot" ] && [ ! -L "$state/trace-next-boot" ]; then
        rm -f "$state/trace-next-boot" || return 1
        stage=config_boot_trace
        /system/bin/sh -x "$module/config.sh" boot
    else
        /system/bin/sh "$module/config.sh" boot
    fi
    command_exit=$?
    return "$command_exit"
}

capture() {
    tail -c 8192 > "$1"
}

cleanup() {
    rm -f "$logs/out" "$logs/err" "$logs/stdout" "$logs/stderr" "$logs/status"
    rmdir "$logs"
}

logs=
if ! { boot_id=$(cat /proc/sys/kernel/random/boot_id) &&
    [ -n "$boot_id" ] && [ ! -L "$state" ] &&
    mkdir -p "$state" && chmod 700 "$state" &&
    logs=$(mktemp -d "$state/.boot-last.XXXXXXXX") &&
    mkfifo "$logs/out" "$logs/err"; } 2>/dev/null
then
    [ -z "$logs" ] || cleanup 2>/dev/null
    wait_startup || exit 1
    exec /system/bin/sh "$module/config.sh" boot
fi

trap 'cleanup 2>/dev/null' EXIT
capture "$logs/stdout" < "$logs/out" &
out_pid=$!
capture "$logs/stderr" < "$logs/err" &
err_pid=$!
run_boot > "$logs/out" 2> "$logs/err"
result=$?
wait "$out_pid"
wait "$err_pid"
read -r uptime_finished _ < /proc/uptime
if printf 'boot_id=%s\nstage=%s\nexit=%s\ncommand_exit=%s\nwait_exit=%s\nbootprop_before=%s\nbootprop_after=%s\nuptime_before_wait=%s\nuptime_after_wait=%s\nuptime_finished=%s\nvendorprop_before=%s\nvendorprop_after=%s\nvendor_wait_exit=%s\nvendor_wait_budget=%s\n' \
    "$boot_id" "$stage" "$result" "$command_exit" "$wait_exit" "$bootprop_before" "$bootprop_after" \
    "$uptime_before_wait" "$uptime_after_wait" "$uptime_finished" "$vendorprop_before" "$vendorprop_after" \
    "$vendor_wait_exit" "$vendor_wait_budget" > "$logs/status" &&
    mv -f "$logs/stdout" "$state/boot-last.stdout" &&
    mv -f "$logs/stderr" "$state/boot-last.stderr"
then
    mv -f "$logs/status" "$state/boot-last.status"
fi
exit "$result"
