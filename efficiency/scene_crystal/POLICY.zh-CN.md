# Crystal 事件驱动自动回写：待验收载荷

本轮仅准备代码、host控制流检查和Android NDK构建。未部署、未创建启用标记、未登记新的内核notes，也没有启动自动回写。最终目标为6.6.89 Crystal + ZMS + LZ4KDS，首轮关闭native SDDC ZMS，使用flatten-to-ordinary后打包压缩对象落盘。

## 事件与准入

`start_crystal_writeback.sh` → 已有 `native_writeback_event --exec` → 独立 `crystal_writeback_daemon.sh`。原事件C源码和二进制构建方式保持不变，控制器继承nice19、CPU0–5与I/O idle。使用其已有brightness/inotify、阻塞PSI、`--watch`、`--grace`接口，不添加周期扫描。PSI与亮灭屏仅触发判断，连续熄屏300秒后再次完整检查；冷却中亮屏取消本次等待，尚未提交回写。已提交的writeback/flush必须结算，不将其日志伪装成“取消且无I/O”。

新增默认配置独立于Scene原配置：auto=false、冷却300秒、单轮最多128MiB逻辑slot、物理日计费目标2048MiB、ZRAM驻留至少1024MiB、两次尝试至少900秒、温度不超过400（40℃）、亮度阈值1。可下调批量和日目标；上限不超过128/2048，冷却和间隔不得下调到300/900以下。可用RAM固定至少512MiB。Android 32位有符号shell比较前使用KB，物理大计数用受限的awk整数范围处理；不执行策略配置中的shell代码。

自动策略要求已有早期profile准入，再同时要求：

- 精确PLQ110、独立Crystal notes→codec登记、初始化启用标记、私有8GiB backing及loop归属一致；已有ZRAM容量/算法/优先级符合Scene配置，且它是唯一活跃交换设备。
- 独立普通文件 `crystal-writeback-auto.enabled`，独立 `crystal-writeback.conf` 中显式auto=true；已有blocked文件时拒绝启动。缺省配置auto=false，打包不创建marker。
- 原有Scene配置仍请求ZRAM及回写；不覆盖配置或AppOpt。内核主算法必须已选中登记codec，不写comp_algorithm，不reset/swapoff，不重建loop或backing。
- 内核hybridswap_enable/core_enable都为0、writeback_limit_enable为1、页大小4KiB、启动完成、温度/RAM/亮度/驻留门槛满足。
- LZ4KDS要求SDDC enabled=1且真实 `integrity_failures`、`decode_failures`、`flatten_failures`、`delta_proof_failures` 四项可读；任一缺失则在日志、账本、锁之前退出。原公开源没有导出integrity_failures，因此当前缺该字段的内核不能自动启用。SDDC=n的纯LZ4KD控制候选可使用enabled=0分支。

原生/le9uo内核在任何日志或账本写入前退出。两套控制器白名单、配置、PID/状态分离；为了共享同一物理日账，它们使用同一个 `native-writeback.lock`，`flock -n 9 9>&9` 显式继承FD，避免Android mksh的CLOEXEC问题。

## 逻辑预算与物理计费

内核限额按成功迁移的逻辑slot扣减。设原剩余限额saved=100000，本轮cap=32768，迁移完成remaining=0，实际打包只写4096个物理页：

```
logical_consumed = cap - remaining = 32768
恢复剩余逻辑额度 = saved - logical_consumed = 67232
日账物理收费 = ZMS physical_write_pages增量 = 4096
```

不会用saved-physical恢复成95904。单轮cap取128MiB、剩余物理日目标、saved逻辑额度三者最小值。打包/GC/部分zspage落盘会改变实际物理写量，不能把逻辑cap当作物理硬限。

共享原有 `native-writeback.daily`，格式保持 `<UTC YYYYMMDD> <物理4KiB页数>`，原生计费不会因切换Crystal而清零。同日干净重启仍保留已收费，新boot的物理计数从0继续追加；UTC向后跳变则拒绝，不补满。跨日未观测的增量保守计入新一天，无法从累计计数恢复准确历史时间。

独立 `crystal-writeback.physical` 保存bootID、notes、backing身份和物理counter水位。身份含loop、loop/zram设备号、私有文件设备号/inode/容量/owner/mode。启动、每次已有事件、批次前后、恢复时采样ZMS `physical_write_pages/bytes`，包含控制器可观察到的手动写入与GC；同时读取真实bd_stat以独立记录过滤后的写量。不能用bd_stat替代总物理计费，因为GC可不计入它。

先持久化日账，再推进水位；两个文件之间崩溃可能重复收费，采用保守多扣，**不是exactly-once事务**。物理counter、身份、日期均不变时不重写日账或水位，不因压力事件重复fsync。被动等事件时没有新增采样定时器；因此关停/断电前最后一次采样之后的GC、控制器未运行期间并已跨boot丢失的累计值无法绝对重建。

## flush与错误恢复

`crystal_writeback_flush --block` 固定只读打开 `/dev/block/zram0`，O_NOFOLLOW且fstat确认块设备后fsync；不执行raw read/write。只读fsync触发驱动已有flush通路。函数返回0仍不能独立证明真实落盘：必须另查dirty_pages=0，并记录ZMS物理写及失败计数增量。内核sleeping门控可能令flush直接返回0但保留dirty，这种情况会保留pending并停止自动提交。

同一个小helper的 `--state` 只接受固定名单的控制器状态文件，只读fsync文件和父目录，保证journal/账本原子rename后的持久化顺序；不解析状态文件内容。它不改变原生事件程序。

修改本轮逻辑cap之前先持久化 `crystal-writeback.pending`，记录boot/notes/backing、saved/cap、原物理/软件错误计数。新版journal为v2，包含ZMS `physical_read_failed_pages`基线；旧v1、v2缺字段或非法字段均拒绝恢复，不以0补齐。状态为armed→restoring→restored；恢复计划先持久化，再写回saved-logical_consumed。若进程恰好在写回后、phase保存前终止，后续只接受计划中的旧值或新值，避免重复补额度。保存的额度被外部改变则拒绝重写。

同boot、身份和计数稳定时可完成遗留flush、物理计费及清journal。跨boot遗留pending、外部backing、可观测counter回退、flush错误、dirty未清零等均fail-closed；保留journal、写blocked原因，不使用旧saved给新设备补额度，也不自动重启新一轮。blocked/pending需人工核对，不能通过重启服务自动抹掉。正常干净重启不会清当日日账；尝试时间在提交前持久化，服务重启不会绕过15分钟间隔。

sysfs writeback返回非零时，仅在remaining=0且logical_consumed=cap、ZMS物理读/写失败均未增、SDDC四项错误和io_stat读写失败均未增、flush已完成且dirty=0时，才记录quota_stop。GC的读失败可能只增加ZMS `physical_read_failed_pages`，不增加io_stat的前台failed_reads，因此必须独立记录和比较；该计数缺失时不安装cap，回退时拒绝恢复旧额度，增量非0时保留journal并blocked。即使writeback返回0也采用同样错误判定。物理写量小于逻辑cap是正常压缩效果。其余partial错误会结算已发生I/O并停止；不能把块I/O失败为0等同SDDC软件路径成功。

结算使用flush后最终一次物理observer快照，同时计费和计算物理读/写失败增量，再读取SDDC/io_stat软件错误；不会先按旧快照判定无错、随后刷新计数却忽略新观察值。九项定点注入回归覆盖最终观察时的八类错误以及正常增加的物理写入。sysfs多个文件并非原子联合快照；最后一次快照之后发生的并发I/O仍属于后续观察边界，不承诺全局零竞态。

## 物理上限边界

审查固定源：zram_drv.c:2173-2176仅按逻辑迁移扣limit；zms.c:676-695在I/O完成后累计物理页并回调core计费；ZMS实现本身没有统一quota准入。core quota服务于策略估算/事后计数，并不能约束所有手动写入、dirty flush和GC。

因此2GiB是实际计费后的停止目标，不承诺未来GC永不写、NAND物理写量严格小于2GiB或单轮物理严格小于128MiB。达到/超过目标后不再自动提交新冷页，仍保留必要的已提交dirty落盘和前台读回。失败BIO的部分底层写入与UFS/F2FS写放大也不能从成功physical_write_pages完全估算。需要内核统一准入/GC预算设计才能进一步讨论硬限。

## 验证与启用边界

`test_policy_host.py` 使用本目录真实shell，隔离sysfs/proc、模拟block/fsync和event边界，覆盖逻辑/物理分离、共享账本、保守崩溃收费、quota/partial错误、SDDC错误、重启/外部状态、取消，以及GC物理读错误和v1/损坏v2 journal拒绝。`test_policy_daemon_host.py`针对daemon执行独立小回归；最终观察顺序收紧后使用`test_policy_final_snapshot_host.py`做定点注入，并在运行前后锁定全部生产脚本/C/配置SHA。各阶段结果数量和对应源哈希见verification.json。等待时间和内核I/O是mock，不能证明300秒真实计时、Android mksh、磁盘flush、真实后台优先级或packed数据正确。`build_scene.py`用NDK -Wall/-Wextra/-Werror构建两个程序，全部shell做bash -n，并生成完整manifest。

启用之前仍需完成手动私有zramN的实际packed写入→zram fdatasync/fsync→dirty=0→首次真实后端冷读与完整pattern/SHA校验，实际SDDC数据路径与32项KUnit通过，以及stats导出候选的构建/启动核验。之后才可由主代理登记精确notes、选择对应codec并显式启用用户态策略。本轮不执行这些动作，也不覆盖已有策略配置。
