#!/system/bin/sh
set -eu
cd "$1"
watcher=
probe=

cleanup() {
    if [ -n "$watcher" ]; then
        kill "$watcher" 2>/dev/null || true
        wait "$watcher" 2>/dev/null || true
    fi
    ./affinityctl clear >/dev/null 2>&1 || true
    if [ -n "$probe" ]; then
        kill "$probe" 2>/dev/null || true
        wait "$probe" 2>/dev/null || true
    fi
    rmmod ace6_thread_affinity
}
trap cleanup EXIT

await_mask() {
    while [ "$#" -gt 0 ]; do
        attempts=0
        while :; do
            actual=$(awk '/^Cpus_allowed_list:/{print $2}' "/proc/$1/status" 2>/dev/null || true)
            if [ "$actual" = "$2" ]; then
                printf 'PASS %s (%s)\n' "$3" "$actual"
                break
            fi
            attempts=$((attempts + 1))
            if [ "$attempts" -ge 70 ]; then
                printf 'FAIL %s expected=%s actual=%s\n' "$3" "$2" "$actual" >&2
                ./affinityctl status >&2
                cat watch.log >&2
                return 1
            fi
            sleep 0.05
        done
        shift 3
    done
}

set_scene() {
    printf 'scene_foreground_pkg=%s\nvisible_pkgs=%s\n' "$1" "$2" > scene.next
    mv scene.next scene
}

find_thread() {
    attempts=0
    while [ "$attempts" -lt 45 ]; do
        for thread in "/proc/$probe/task/"*; do
            if [ "$(cat "$thread/comm" 2>/dev/null || true)" = "$1" ]; then
                echo "${thread##*/}"
                return
            fi
        done
        attempts=$((attempts + 1))
        sleep 0.05
    done
    return 1
}

mkdir rules
printf 'ace6_aff_probe=0-3\n' > rules/app_profile_rules.prop
printf 'ace6_aff_probe{a6_manual}=3\n' > rules/applist.prop
set_scene other.app other.app
printf '180\n' > brightness
mkfifo commands
./ace6_aff_probe commands > probe.log &
probe=$!
printf 'spawn a6_worker\nspawn a6_manual\n' > commands
worker=$(find_thread a6_worker)
manual=$(find_thread a6_manual)

./affinityctl watch "$PWD/scene" "$PWD/brightness" > watch.log 2>&1 &
watcher=$!
./affinityctl adapt-load rules ace6_aff_probe > load-status
await_mask "$worker" 0-1 'background worker narrows' "$manual" 3 'manual rule wins in background'

if ./affinityctl watch "$PWD/scene" "$PWD/brightness" > second-watch.log 2>&1; then
    echo 'FAIL a second Scene owner was accepted' >&2
    exit 1
fi
grep -q 'Device or resource busy' second-watch.log
printf 'PASS a second Scene owner was rejected\n'

set_scene ace6_aff_probe ace6_aff_probe
await_mask "$worker" 0-3 'foreground restores worker capacity'
set_scene other.app 'other.app,ace6_aff_probe'
await_mask "$worker" 0-3 'visible secondary window keeps foreground capacity'
printf '0\n' > brightness
await_mask "$worker" 0-1 'display-off wins over visible window' "$manual" 3 'manual rule wins with display off'
grep -q '^display_off=1$' /dev/ace6_thread_affinity
printf '180\n' > brightness
await_mask "$worker" 0-3 'display-on restores visible capacity'

set_scene other.app other.app
await_mask "$worker" 0-1 'atomic Scene replacement returns to background'
kill "$watcher"
wait "$watcher"
watcher=
await_mask "$worker" 0-3 'watcher exit reverts to static rules'
await_mask "$manual" 3 'static manual rule survives watcher exit'
./affinityctl clear
await_mask "$manual" 0-3 'clear restores the original manual-thread mask'
