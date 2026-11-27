#!/system/bin/sh
module=${0%/*}
/system/bin/sh "$module/config.sh" teardown || exit 1
# User configurations and previous generations remain available for reinstall.
