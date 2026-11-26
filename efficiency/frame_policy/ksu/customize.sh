#!/system/bin/sh
[ "$ARCH" = arm64 ] || abort '只支持 ARM64。'
[ "$(getprop ro.product.model)" = PLQ110 ] || abort '此配置包用于一加 Ace 6。'
chmod 700 "$MODPATH/framectl" "$MODPATH/config.sh"
chmod 600 "$MODPATH/defaults/seed.json" "$MODPATH/defaults/settings.json"
ui_print '配置会在 WebUI 保存应用后接管；首次安装默认关闭。'
