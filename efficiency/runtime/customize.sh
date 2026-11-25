if [ "$(getprop ro.product.model)" != OPD2413 ]; then
    abort 'This module is packaged for OPD2413'
fi
set_perm "$MODPATH/bind_traffic" 0 0 0700
