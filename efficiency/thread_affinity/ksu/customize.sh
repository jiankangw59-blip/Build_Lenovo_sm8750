#!/system/bin/sh

old=/data/adb/modules/ace6_thread_affinity
legacy=/data/adb/modules/AppOpt_Aloazny
chmod 0700 "$MODPATH/affinityctl"

if [ -d "$old/rules" ] && [ "$old" != "$MODPATH" ]; then
	# Module upgrades carry existing edits and the last working copy forward.
	rm -f "$MODPATH/rules/app_profile_rules.prop" "$MODPATH/rules/app_profile_rules.profiles"
	for name in template_rules.prop app_profile_rules.prop app_profile_rules.profiles auto_rules.prop measured_game_rules.prop applist.prop applist.previous; do
		if [ -f "$old/rules/$name" ]; then
			cp -p "$old/rules/$name" "$MODPATH/rules/$name" || abort '无法保留现有绑核规则。'
		fi
	done
	[ ! -f "$old/active" ] || : > "$MODPATH/active"
elif [ ! -d "$legacy" ] || [ -f "$legacy/disable" ]; then
	: > "$MODPATH/active"
fi
