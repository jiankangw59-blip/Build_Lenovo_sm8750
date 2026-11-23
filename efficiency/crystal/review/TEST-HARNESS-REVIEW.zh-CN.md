# Crystal 私有生命周期 / 主设备自有页探针交叉审阅

本轮没有发现需要阻止安全运行的误写zram0、覆盖其他swap、清理误删用户backing或A/B数据判据错误。发现并最小修复了一处会让“首次冷读经过后端”结论假阳性的计数归因问题。修复后新增6项聚焦Python判据回归通过；未重跑原有host模型、未执行ADB或手机测试、未commit/push。

## 已修复：GC读增长不能独立证明前台冷读

原 `run_probe.py` 的Crystal分支及 `run_lifecycle.py` 顺序用例，仅要求flushed→after的 `physical_read_pages` 与loop读取扇区增长。公开固定源 `13d291aef3d1d9607ad3fa5ff30dc23b2888a3da`：

- `zram_drv.c:1314-1345`：充电、至少128个used blocks并满足dirty/partial等条件时，后台GC可运行。
- `zms.c:1761-1764`：GC搬移源/目标对象会调用 `zms_read_frozen_block_unlocked`。
- `zms.c:775`：该调用执行 `zms_read_block(..., account_bd_stat=false)`。
- `zms.c:676-705`：GC读取仍增长总physical_read_pages及实际loop I/O，但不增长bd_stat_read_pages。
- `zms.c:2678`：前台ZMS读使用account_bd_stat=true，增长bd_stat_read_pages。

因此，当验读窗口内有GC读取，而前台读取仅命中驻留缓存时，旧判据仍可能通过。已知数据逐字节验证本身仍有效，不能据此把损坏数据称为通过；缺口在于真实后端读取的归因。

最小修改：在 `run_probe.py` 新增共享 `cold_read_reached_backend`，Crystal必须同时满足实际 `bd_stat[1]` 前台读增长、ZMS总物理读增长、loop读扇区增长。`run_lifecycle.py` 调用同一判据；native保留原生前台读+loop增长语义。两份测量报告另记录 `foreground_bd_read_pages_delta`。已知字节、完整长度、hash、dirty=0、物理写入、loop写入、I/O错误等原有条件均未删除。

`test_cold_read_host.py` 执行真实共享Python函数，覆盖GC-only拒绝、三种读证据齐备通过、缺物理增长拒绝、缺loop增长拒绝、计数回退拒绝、native控制语义，6项全部通过。Python编译与diff空白检查通过。证据：

```
module-audits/ace6-efficiency-20261123/crystal-reverse-review/test-harness-cold-read-host.json
```

这只是判据回归，没有重现真实GC并发，也没有执行内核I/O。没有更改C helper，因此本轮不重复NDK编译。修复的准确diff是两个Python调用点加共享判据和测量字段，另增加一个聚焦测试文件。

## 核对通过的实际路径

| 范围 | 源码证据 | 结论 |
| --- | --- | --- |
| 私有设备所有权 | `run_lifecycle.py`直接复用`run_probe.PrivateRound`；`run_probe.py`的assert_owned核对sysfs inode、设备号、active swap别名、backing owner和32MiB容量 | 不是另一套缺少guard的生命周期创建器 |
| C端私有设备拒绝条件 | `private_logic.h`只接受规范`/dev/block/zram1..1023`；`private_device.h`核对直接块节点、active swap设备号、O_NOFOLLOW/O_EXCL、sysfs dev及固定32MiB容量 | zram0、symlink、swap别名或错误容量不能进入本工具的raw I/O |
| 生命周期范围 | `lifecycle_probe.c`按8192页×4096操作；覆盖与discard仅每8页中的前2页；discard每次2页，最后一组不越界 | 固定32MiB；旧目标页与被覆盖/清零源页的期望分开计算 |
| seed判据 | wrapper与C都拒绝相同seed；wrapper明确uint64范围；`lifecycle_seed_safe`与Python均拒绝1024组使源随机状态为0的seed | 默认两seed有效；A/B同一页由不同初始state经可逆xorshift生成，不会因合法seed相同而误判 |
| 并发A/B | 两writer独占偶/奇页；每页成功pwrite后更新该页version；reader只接受完整A或B且拒绝全0；主线程只在join后读取version并全盘核对 | 没有用不稳定全盘hash判定并发中间态，也没有C层共享version读写数据竞争 |
| 超时与清理 | PrivateRound.cleanup先assert_owned，再`set -e`执行reset→hot_remove→归属核对→detach确认→unlink；异常使cleanup_ok=false并保留stage | helper尚持fd或I/O挂起时，公开`reset_store`在disk_openers/claim非0返回-EBUSY（zram_drv.c:5661-5666），不会继续解绑/unlink |
| 主设备自有页 | `owned_pages_probe.c:274-325`只分配本进程4MiB，对前64页MADV_PAGEOUT；用self pagemap取得slot，要求64个有效且不重复的swap entry；每次page_index前重新核对同一entry及身份 | 没有全局idle标记、reset/swapoff或扫描他人slot |
| 主设备flush | `owned_pages_probe.c:195-211`以O_RDONLY/O_NOFOLLOW打开zram0，fstat核对sysfs设备号，仅fdatasync/fsync | 没有对主块fd执行raw read/pread/write/pwrite；不向limit写入预算 |
| owned wrapper | `run_owned_probe.py`先完整只读预检、暂存后再核对boot/notes/backing/disksize；结束要求保护配置一致；只清理自己的probe暂存目录 | 不解绑或删除已有主backing，也不把预算消耗后的低余额当成需要补满 |

## 可以接受但不能扩大的证据边界

- 私有顺序测试只优先改写每组第一对“可能的REF源”；未直接识别全部REF或generation。报告明确`ref_slots_directly_identified=false`等字段，不能称为所有引用复用路径已证明。
- 并发2–5秒是发出stop前的工作窗口，join及阻塞内核I/O可能更久。host侧120秒超时不能独立证明远端helper已经退出；私有reset/openers及清理失败保留机制才是保护backing的依据。三个线程均有进度不证明每个slot实际发生过读写重叠，报告`all_races_proven=false`正确。
- 主owned只发出64次自有slot逻辑请求；主设备readonly fsync会drain全设备已有dirty数据。物理计数还可能包含其他应用/GC，不能把全局counter增长精确归因于这64页。现有`all_requested_slots_backed_proven=false`、`global_counters_may_include_concurrent_io=true`明确保留该边界。
- 通过自有4MiB逐字节核对证明这批自有数据内容正确，不能替代私有packed冷读或SDDC引用生命周期验收，也不能证明Kcompressd的kswapd路径。
- native对照与Crystal在预检marker、backend字段及比较器中区分；native成功、host模式通过均不能作为Crystal数据路径已经通过的证据。当前手机尚未刷Crystal的状态没有在此报告中升级为运行验收。

scene auto读失败计数遗漏由主代理另行委派修复，本轮未更改任何scene文件。
