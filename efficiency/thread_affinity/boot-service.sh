#!/system/bin/sh

module=${0%/*}
controller=$module/affinityctl
rules=$module/rules
legacy=/data/adb/modules/AppOpt_Aloazny
node=/dev/ace6_thread_affinity
scene=/data/user/0/com.omarea.vtools/files/xiaocaiye_a11y_state
games=/data/user/0/com.omarea.vtools/shared_prefs/games.xml
display=/sys/class/backlight/panel0-backlight/brightness

# Both markers are set during the tested handover. Keeping the old module
# installed lets a failed startup or a rollback boot restore its policy.
[ -f "$module/active" ] && [ -f "$legacy/disable" ] || exit 0

if [ "${1:-}" = reload ]; then
	[ -f "$module/ready" ] && ! pidof AppOpt >/dev/null || exit 1
	"$controller" check "$rules" >/dev/null && "$controller" adapt-load "$rules"
	exit $?
fi

exec >> "$module/service.log" 2>&1
echo "$(date '+%F %T') starting kernel affinity policy"
if [ -c "$node" ] && ! grep -qx 'rules=0' "$node"; then
	echo 'Affinity rules already have an owner'
	exit 1
fi
watcher=
owned=

rollback() {
	trap - EXIT HUP INT TERM
	rm -f "$module/ready"
	if [ -n "$watcher" ]; then
		kill "$watcher" 2>/dev/null
		wait "$watcher"
	fi
	if [ -n "$owned" ] && ! "$controller" clear >/dev/null; then
		echo 'Could not clear kernel affinity policy'
		return 1
	fi
	rm -f "$module/active" "$legacy/disable"
	if ! pidof AppOpt >/dev/null && [ -f "$legacy/service.sh" ]; then
		sh "$legacy/service.sh" >> "$module/rollback.log" 2>&1
	fi
	echo "$(date '+%F %T') restored AppOpt"
}
trap rollback EXIT
trap 'exit 1' HUP INT TERM
rm -f "$module/ready"

grep -qx 'game_tgid=0' "$node" || exit 1
! pidof AppOpt >/dev/null || exit 1
"$controller" check "$rules" >/dev/null || exit 1

# Load the static configuration while the credential-encrypted Scene state is
# unavailable. Once the watcher publishes context, adaptive rules take effect.
owned=1
"$controller" adapt-load "$rules" || exit 1
until [ -d "${scene%/*}" ] && [ -d "${games%/*}" ] && [ -r "$display" ]; do
	sleep 5
done
"$controller" watch-games "$scene" "$display" "$games" &
watcher=$!
tries=0
until grep -q '^foreground=[^-]' "$node" || [ "$tries" -ge 20 ]; do
	kill -0 "$watcher" 2>/dev/null || exit 1
	tries=$((tries + 1))
	sleep 0.1
done
kill -0 "$watcher" 2>/dev/null || exit 1
: > "$module/ready"
wait "$watcher"
