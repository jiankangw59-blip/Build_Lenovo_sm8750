state=/data/adb/opd2413-kernel-efficiency

opd2413_enrolled() {
    [ "$(getprop ro.product.model)" = OPD2413 ] || return 1
    [ ! -e "$module/disable" ] || return 1
    [ -r "$module/kernel.sha256" ] || return 1
    notes=$(sha256sum /sys/kernel/notes) || return 1
    notes=${notes%% *}
    grep -Fxq "$notes" "$module/kernel.sha256"
}
