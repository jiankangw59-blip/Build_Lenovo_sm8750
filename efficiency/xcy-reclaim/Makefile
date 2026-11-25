obj-y := xcy_reclaim_runtime.o xcy_reclaim_psi.o xcy_reclaim_rank.o
obj-y += xcy_reclaim_urgent.o xcy_reclaim_importance.o xcy_reclaim_identity.o
obj-y += xcy_reclaim_mailbox.o xcy_reclaim_candidates.o xcy_reclaim_plan.o
obj-y += xcy_reclaim_reply.o xcy_reclaim_channel.o xcy_reclaim_engine.o
obj-y += xcy_reclaim_signal.o xcy_reclaim_protection.o xcy_reclaim_sources.o
obj-y += xcy_reclaim_policy.o xcy_reclaim_lease.o xcy_reclaim_scene.o
obj-y += xcy_reclaim_traffic.o

CFLAGS_REMOVE_xcy_reclaim_urgent.o := -mgeneral-regs-only $(CC_FLAGS_LTO)
CFLAGS_xcy_reclaim_urgent.o := -fno-lto
