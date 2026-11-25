#!/system/bin/sh
module=${0%/*}
. "$module/enrolled.sh"
opd2413_enrolled || exit 0
sh "$module/apply_network_and_workqueue.sh"
sh "$module/start_crystal_kernel_writeback.sh"
if timeout 180 "$module/bind_traffic" wait-unlock; then
    sh "$module/activate_xcy.sh" activate
fi
