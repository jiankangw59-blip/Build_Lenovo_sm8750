#!/system/bin/sh
# The only policy targets are the three short-lived probe processes below.
set -eu
cd "$1"
group=/dev/cpuset/ace6-affinity-probe-$$
pids=

cleanup() {
    ./affinityctl clear >/dev/null 2>&1 || true
    for pid in $pids; do kill "$pid" 2>/dev/null || true; done
    for pid in $pids; do wait "$pid" 2>/dev/null || true; done
    if [ -d "$group" ]; then rmdir "$group"; fi
    rmmod ace6_thread_affinity
}
trap cleanup EXIT

await_mask() {
    attempts=0
    while [ "$attempts" -lt 70 ]; do
        actual=$(awk '/^Cpus_allowed_list:/{print $2}' "/proc/$1/status" 2>/dev/null || true)
        if [ "$actual" = "$2" ]; then
            printf 'PASS %s (%s)\n' "$3" "$actual"
            return
        fi
        attempts=$((attempts + 1))
        sleep 0.05
    done
    printf 'FAIL %s expected=%s actual=%s\n' "$3" "$2" "$actual" >&2
    ./affinityctl status >&2
    exit 1
}

start_probe() {
    mkfifo "$2.fifo"
    "./$1" "$2.fifo" > "$2.log" &
    last_pid=$!
    pids="$pids $last_pid"
    attempts=0
    until grep -q '^ready ' "$2.log"; do
        attempts=$((attempts + 1))
        [ "$attempts" -lt 60 ] || exit 1
        sleep 0.05
    done
}

mkdir rules malformed
cat > rules/template_rules.prop <<'RULES'
ace6_aff_probe{a6probe_base}=2
ace6_aff_probe{a6probe_thr*}=2
ace6_aff_probe:*=2
*{a6probe_gl[0-9]}=1
RULES
cat > rules/applist.prop <<'RULES'
ace6_aff_probe{a6probe_base}=1
ace6_aff_probe{a6probe_hot*}=3
RULES
printf 'ace6_aff_probe{a6probe_base}=99\n' > malformed/applist.prop
ln -s ace6_aff_probe ace6_aff_probe:side
ln -s ace6_aff_probe ace6_aff_alias

start_probe ace6_aff_probe main
main=$last_pid
original=$(awk '/^Cpus_allowed_list:/{print $2}' "/proc/$main/status")
./affinityctl load rules > load-status
await_mask "$main" 1 'load existing thread, manual beats template'
printf 'name a6probe_hot7\n' > main.fifo
await_mask "$main" 3 'rename matches thread wildcard'

if ./affinityctl load malformed > invalid-status 2>&1; then
    echo 'FAIL invalid rule accepted' >&2
    exit 1
fi
await_mask "$main" 3 'invalid reload leaves active rules intact'

printf 'spawn a6probe_thr7\n' > main.fifo
tries=0
worker=
while [ "$tries" -lt 60 ] && [ -z "$worker" ]; do
    for path in "/proc/$main/task/"*; do
        if [ "$(cat "$path/comm" 2>/dev/null || true)" = a6probe_thr7 ]; then
            worker=${path##*/}
            break
        fi
    done
    tries=$((tries + 1))
    sleep 0.05
done
[ -n "$worker" ]
await_mask "$worker" 2 'new thread matches its own rule'

start_probe ace6_aff_probe:side side
side=$last_pid
await_mask "$side" 2 'package subprocess default rule'
start_probe ace6_aff_alias global
global=$last_pid
printf 'name a6probe_gl7\n' > global.fifo
await_mask "$global" 1 'global character-class rule'

printf 'name a6probe_miss\n' > main.fifo
await_mask "$main" "$original" 'rename out of rule restores prior mask'
printf 'ace6_aff_probe{a6probe_miss}=0-3\n' > rules/applist.prop
./affinityctl load rules > reload-status
mkdir "$group"
cat /dev/cpuset/mems > "$group/mems"
printf '0-1\n' > "$group/cpus"
printf '%s\n' "$main" > "$group/tasks"
await_mask "$main" 0-1 'cgroup attach preserves allowed CPUs'
[ "$(cat "/proc/$main/cpuset")" = "/${group##*/}" ]

printf 'ace6_aff_probe{a6probe_miss}=1\n' > rules/applist.prop
./affinityctl load rules > reload-status
await_mask "$main" 1 'custom CPU list changes on hot reload'
./affinityctl clear
await_mask "$main" 0-1 'clear restores mask inside current cpuset'
await_mask "$worker" "$original" 'clear restores inherited worker baseline'

toybox taskset -p 2 "$worker" >/dev/null
printf 'ace6_aff_probe{a6probe_thr*}=1-2\n' > rules/applist.prop
./affinityctl load rules > reload-status
await_mask "$worker" 1 'external-module route keeps app-owned affinity'
./affinityctl clear
await_mask "$worker" 1 'clear does not overwrite app-owned affinity'
printf 'PASS AppOpt still running (%s)\n' "$(pidof AppOpt)"
