# Crystal 自动回写策略交叉审阅

本次仅检查本地主机源码及构建载荷，不连接手机。auto默认false，未部署、未启动。当前LZ4KDS候选缺少对外integrity统计，launcher会在写日志、锁和日账之前退出；本审阅不代替新版组合profile的构建、启动和实际数据验证。

## 已发现并修复的问题

**GC物理读失败原先可能被当作正常quota停止。** 原策略只保存ZMS `physical_write_failed_pages`；`io_stat`的failed_reads属于前台BIO失败，不能替代GC物理读失败。内核路径为`zram_zms_gc_run_locked`→`zms_compact`→`zms_read_frozen_block_unlocked`，其失败增加ZMS `physical_read_failed_pages`和诊断用bd_read_failures，不要求增加前台io_stat。

因此，原代码在logical cap耗尽、GC读失败而flush之后dirty=0的组合下，可能仍输出`quota_stop=yes`并允许下一轮。主代理已授权最小修复：强制读取物理读失败计数、加入v2 pending基线、恢复前检查计数回退、结算比较其增量；出现错误时保留journal和blocked。旧v1、缺字段/非法字段v2不推测基线，也不恢复旧额度。新增quota与成功返回两种GC读错误回归及损坏journal阴性检查。

**最终观察值曾可能被旧错误判定覆盖。** 原结算先计算round delta和SDDC/io_stat错误，随后`cw_observe_physical`再次刷新计数；若错误恰好在最终刷新前出现，旧判断可仍为0。现在flush后只使用最终物理observer快照进行物理计费与错误增量计算，其后再读取SDDC/io_stat；最后一次实际读到的错误不会被早先的0覆盖。九项定点回归分别在这一边界注入物理读/写失败、四项SDDC失败、前台读/写失败及正常物理写增长，核对实际round计数与journal处理。

## 高影响路径结论

| 路径 | 审阅结论 |
| --- | --- |
| logical cap恢复 | 以`saved-(cap-remaining)`恢复。packed写4096页、逻辑消耗32768页时，100000只恢复到67232；不会恢复成95904。journal先于安装cap持久化。 |
| armed/restoring崩溃恢复 | cap未安装时识别仍为原saved；恢复计划先持久化，再修改限额，只接受计划中的旧值或新值。不会重复加额度；无法解释的外部变化拒绝恢复。 |
| 跨boot/身份pending | 要求bootID、notes、loop/zram设备号、文件inode/容量/owner/mode相同；不把旧saved写到新boot或外部backing。跨boot遗留pending停止并保留证据。 |
| 日账与物理水位 | 先持久化共享日账，再持久化水位。两者之间崩溃可多扣，不能因此退回额度；新boot只追加新计数，不清同日日账。UTC向后跳转拒绝。 |
| quota与错误 | logical remaining=0仍需flush成功、dirty=0、ZMS物理读/写失败及四项SDDC错误和前台io_stat错误均未增加。新的GC读失败检查不能由io_stat替代。 |
| 亮屏取消与TERM | grace期间亮屏在提交前取消；TERM结束grace并在EXIT中结算既有pending。已提交的writeback/flush不记录成无I/O取消；恢复失败留journal并blocked。 |
| flock与子进程 | 共享native-writeback.lock由父shell持有FD9；`flock -n 9 9>&9`明确传给加锁命令。grace只显式继承所需FIFO FD3；cleanup会kill/wait事件子进程。Android mksh与SIGKILL实际继承行为仍属待机验证范围。 |
| root fsync helper | `--block`固定O_RDONLY打开zram0、O_NOFOLLOW、fstat检查块设备，仅fsync。`--state`仅固定名单、root私有普通文件和父目录fsync；源码没有raw block read/write、swapoff或reset。 |
| prepare/build钩子 | 原Scene四个输入哈希锁定；service仅在startup成功后调用独立launcher。build编译原事件程序与fsync helper、检查全部shell语法、生成逐文件SHA；不生成auto enabled marker，独立conf不覆盖swap_config.conf。 |

NDK构建后的实际flush ELF也经`llvm-readelf --dyn-symbols`核对：相关I/O导入为open/fstat/fsync，未导入read/write/pread/pwrite/readv/writev；结合固定路径和O_RDONLY源码，未发现raw block数据I/O路径。

## 明确保留的边界

- 日物理目标是观察到成功ZMS物理页之后停止新提交的目标，不是GC、UFS NAND或F2FS写放大的硬上限。被动等待事件期间、服务未运行期间且跨boot丢失的累计增量无法重建；源码与说明已经注明。
- 进程SIGKILL或掉电无法运行TERM/EXIT清理；安全边界依赖持久journal及后续fail-closed恢复。未把主机mock视为真机断电恢复证据。
- quota判定基于可观测状态和已导出的错误计数，shell返回码本身不能提供精确内核errno，也不证明所有未导出的软件路径均无错误。
- 多个sysfs文件不构成原子快照；最后观察完成之后发生的并发错误只能在后续观察中发现，本修复不声明全局绝对零竞态。
- 本轮未重跑已通过且未受改动影响的早期初始化19项；本轮相关策略回归、源哈希、NDK构建和新版prepared payload见`scene_crystal/verification.json`。未测试真实300秒等待、Android后台优先级、真实flush、续航或性能收益。
