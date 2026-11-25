#!/system/bin/sh
set -eu
umask 077

[ "$(getprop ro.product.model)" = OPD2413 ] || exit 1
state=/data/adb/opd2413-kernel-efficiency
backing=$state/zms-backing.img
size=8589934592

valid_backing() {
    [ ! -L "$1" ] && [ -f "$1" ] &&
        [ "$(stat -c '%s %u:%g %a' "$1")" = "$size 0:0 600" ] &&
        [ "$(stat -c %b "$1")" -ge 16777216 ]
}

[ ! -L "$state" ] || exit 1
mkdir -p "$state"
[ "$(stat -c '%u:%g %a' "$state")" = '0:0 700' ] || exit 1
if [ -e "$backing" ] || [ -L "$backing" ]; then
    valid_backing "$backing"
    exit $?
fi

# Keep at least 2 GiB available after reserving the whole backing file.
space=$(stat -f -c '%a %S' "$state")
printf '%s\n' "$space" | awk 'NF == 2 && $1 * $2 >= 10737418240 { ok = 1 } END { exit !ok }'
tmp=$(mktemp "$state/.zms-backing.XXXXXX")
trap 'rm -f "$tmp"' EXIT
trap 'exit 1' HUP INT TERM
/system/bin/fallocate -l "$size" "$tmp"
valid_backing "$tmp"
chcon u:object_r:ksu_file:s0 "$tmp"
# Publish the completed file without replacing a competing provisioner.
if ! ln -T "$tmp" "$backing"; then
    valid_backing "$backing"
fi
sync
