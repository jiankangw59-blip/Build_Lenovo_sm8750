if [ "$(getprop ro.product.model)" != OPD2413 ]; then
    abort 'This module is packaged for OPD2413'
fi
set_perm "$MODPATH/bind_traffic" 0 0 0700
config=/data/adb/opd2413-kernel-efficiency/swap_config.conf
if [ ! -e "$config" ] || grep -qx 'zram_writeback=true' "$config"; then
    if sh "$MODPATH/provision_crystal_backing.sh"; then
        ui_print '- Crystal writeback storage is ready (8 GiB)'
    else
        ui_print '! Crystal writeback storage is unavailable. RAM swap can still start.'
    fi
fi
