#!/system/bin/sh
set -eu
cd "$1"
node=/dev/ace6_thread_affinity
probe=
watcher=

cleanup() {
	./affinityctl clear >/dev/null 2>&1 || true
	if [ -n "$watcher" ]; then
		kill "$watcher" 2>/dev/null || true
		wait "$watcher" 2>/dev/null || true
	fi
	if [ -n "$probe" ]; then
		kill "$probe" 2>/dev/null || true
		wait "$probe" 2>/dev/null || true
	fi
}
grep -qx 'rules=0' "$node"
trap cleanup EXIT
trap 'exit 1' HUP TERM INT

await_line() {
	local file=$1 match=$2 attempts=$3
	until grep -q "$match" "$file"; do
		attempts=$((attempts - 1))
		if [ "$attempts" -le 0 ]; then
			echo "Missing $match in $file" >&2
			cat "$node" >&2
			return 1
		fi
		sleep 0.1
	done
}

find_thread() {
	for comm in /proc/"$probe"/task/*/comm; do
		if [ "$(cat "$comm")" = "$1" ]; then
			local thread=${comm%/*}
			echo "${thread##*/}"
			return
		fi
	done
	return 1
}

mask() {
	await_line "/proc/$1/status" "^Cpus_allowed_list:[[:space:]]*$2$" 30
	printf '%s=%s\n' "$3" "$2"
}

mkdir rules
printf 'ace6_game_probe=0-5\n' > rules/app_profile_rules.prop
printf 'ace6_game_probe{a6_manual}=3\n' > rules/applist.prop
printf 'scene_foreground_pkg=other.app\n' > scene
printf '180\n' > brightness
printf '<map>\n<boolean name="ace6_game_probe" value="true" />\n</map>\n' > games.xml
mkfifo commands
./ace6_game_probe commands > probe.log &
probe=$!
await_line probe.log "^ready $probe$" 30
echo "$probe" > /dev/cpuset/top-app/tasks
toybox taskset -p ff "$probe" >/dev/null

printf 'burn 85 a6_hot\nburn 69 a6_warm\nburn 58 a6_third\nburn 90 a6_manual\nspawn a6_idle\n' > commands
./affinityctl watch-games "$PWD/scene" "$PWD/brightness" "$PWD/games.xml" > watch.log 2>&1 &
watcher=$!
await_line "$node" '^foreground=other.app$' 30
./affinityctl adapt-load rules ace6_game_probe > load-status

hot=$(find_thread a6_hot)
warm=$(find_thread a6_warm)
third=$(find_thread a6_third)
manual=$(find_thread a6_manual)
idle=$(find_thread a6_idle)
mask "$idle" 0-1 background_idle
printf 'scene_foreground_pkg=ace6_game_probe\nvisible_pkgs=ace6_game_probe\n' > scene.next
mv scene.next scene
await_line "$node" "^game_tgid=$probe$" 30
await_line "$node" '^game_finalized=1$' 420
await_line "$node" '^game_samples=16$' 1
mask "$hot" 6-7 hot
mask "$warm" 6-7 warm
mask "$third" 4-7 third
mask "$manual" 3 manual
mask "$idle" 0-5 foreground_idle
cat "$node"

printf '0\n' > brightness
await_line "$node" '^game_tgid=0$' 30
mask "$hot" 0-1 screen_off
mask "$manual" 3 screen_off_manual
echo 'PASS dynamic game ranking, manual precedence and screen-off release'
