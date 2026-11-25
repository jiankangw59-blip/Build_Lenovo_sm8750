#!/system/bin/sh
set -eu
umask 077

module=${0%/*}
controller=$module/affinityctl
rules=$module/rules
manual=$rules/applist.prop
previous=$rules/applist.previous
legacy=/data/adb/modules/AppOpt_Aloazny
max_bytes=65536

fail() {
	printf '%s\n' "$*" >&2
	exit 1
}

file_hash() {
	sha256sum "$1" | cut -d' ' -f1
}

case "${1:-}" in
	status) exec "$controller" status ;;
	read)
		exec 9> "$module/.config.lock"
		flock -s 0 <&9
		file_hash "$manual"
		exec cat "$manual"
		;;
	import)
		if [ -f "$legacy/applist.conf" ]; then
			exec cat "$legacy/applist.conf"
		fi
		[ -f "$legacy/applist.prop" ] || fail '未找到旧 AppOpt 配置。'
		exec cat "$legacy/applist.prop"
		;;
	previous)
		[ -f "$previous" ] || fail '还没有上一次保存的备份。'
		exec cat "$previous"
		;;
	apply|save) ;;
	*) fail '支持 status、read、import、previous、apply 和 save。' ;;
esac

exec 9> "$module/.config.lock"
flock -x 0 <&9
[ -f "$module/ready" ] && [ -f "$module/active" ] && ! pidof AppOpt >/dev/null ||
	fail '内核绑核服务尚未就绪，或 AppOpt 正在运行。'

if [ "$1" = apply ]; then
	"$controller" check "$rules" >/dev/null
	exec "$controller" adapt-load "$rules"
fi

[ "${2:-}" = "$(file_hash "$manual")" ] || fail '规则文件已经变化，请重新读取后再保存。'
stage=$(mktemp -d "$rules/.editor.XXXXXX")
trap 'rm -r -- "$stage"' EXIT
trap 'exit 1' HUP INT TERM
head -c "$((max_bytes + 1))" > "$stage/applist.prop"
[ "$(wc -c < "$stage/applist.prop")" -le "$max_bytes" ] || fail '手写规则文件不能超过 64 KiB。'
if cmp -s "$stage/applist.prop" "$manual"; then
	"$controller" status
	exit 0
fi

for name in template_rules.prop app_profile_rules.prop app_profile_rules.profiles auto_rules.prop measured_game_rules.prop; do
	[ ! -f "$rules/$name" ] || ln -s "$rules/$name" "$stage/$name"
done
"$controller" check "$stage" >/dev/null
cp -p "$manual" "$stage/original"
mv -f "$stage/applist.prop" "$manual"
if ! "$controller" adapt-load "$rules"; then
	mv -f "$stage/original" "$manual"
	fail '内核未应用新规则，已恢复原文件。'
fi
mv -f "$stage/original" "$previous"
