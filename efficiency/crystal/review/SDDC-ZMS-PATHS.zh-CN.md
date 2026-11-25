# LZ4KDS驻留SDDC与ZMS两种回写路径

当前候选在代码与配置层已经组合Crystal、ZMS和LZ4KDS驻留SDDC；回写时采用flatten后的普通压缩对象打包。它没有保持alias/delta wire直接落盘，不能把两条路径混称为“native SDDC ZMS已经启用/验证”。本报告只读核对实现，不确认实机启用或验收，也不据此要求必须开启native。

实际Kconfig符号是 **CONFIG_CRYSTAL_HYBRIDSWAP_SDDC_ZMS_NATIVE**，不是`...SDDC_ZMS`。公开pin为`13d291aef3d1d9607ad3fa5ff30dc23b2888a3da`；下述行号指原始公开源。`efficiency/integrate_crystal.py`的`crystal_configuration()`对当前全部Crystal profiles显式设n，集成阶段另加编译拒绝y的guard；上游Kconfig:134-144的默认y不会覆盖这个明确配置。

| 比较点 | NATIVE=n：当前路径 | NATIVE=y：另一条可选路径 |
| --- | --- | --- |
| 驻留RAM | 仍可使用REF、ALIAS、DELTA及LZ4KDS字典/差分解码 | 同样支持驻留SDDC |
| 回写REF | 复制普通压缩流，或原始4KiB对象，交给ZMS打包 | REF仍走同样的flatten路径 |
| 回写ALIAS | flatten复制引用对象的普通压缩流，形成独立普通对象 | 为alias生成16字节v2 header，含ref_id、generation与packed sizes；不在该alias对象中复制完整引用内容 |
| 回写DELTA | 先基于引用还原目标的**普通压缩流**，不必先解压整页再重新压缩，再交给ZMS | 保留现有24字节delta header加差分payload |
| backing中的关系 | ZMS对象不再依赖SDDC引用图；ZMS的handle/布局元数据仍在RAM | ZMS存alias/delta wire；对应不可变引用对象仍在RAM，由稀疏wb_state持有，不是把整张引用图持久化到盘 |
| admission失败 | 按正常flatten/错误处理 | 不保证每项都走native；不支持的kind或native capture失败仍可回退flatten |
| 读取 | 从ZMS加载普通流，普通解码/原始页恢复 | 读ZMS wire前后校验快照、cookie/header；pin住驻留引用，再做alias/delta恢复 |
| 原SDDC页hash | 普通ZMS对象不携带原SDDC的page_hash；不能把codec成功当作独立整页hash验证 | RAM wb_state保留page_hash，native整页恢复后检查；现有integrity overlay使不符返回EIO。它仍不是可跨重启恢复的盘上校验元数据 |
| 资源权衡 | 放弃该对象在盘上的跨页alias/delta节省，保留ZMS普通流打包 | 可能减少目标对象落盘字节，同时延长RAM引用对象寿命；不能预设总内存、I/O或功耗一定更低 |

关键实现：`crystal_sddc.c:3444-3448`只在y接受ALIAS/DELTA native kind；`zram_drv.c:1968-1993`选择capture或flatten，2132-2149提交并finalize。`crystal_sddc_flatten:3405-3441`恢复普通流；`native_wb_capture:3486-3611`保留DELTA/生成ALIAS header及引用pin；`install/finalize/free:3614-3744`管理wb_state及wb_aliases/wb_deltas；读取经`zram_drv.c:2744-2757`的native restore分支。

两条路径都没有跨重启恢复交换内容的能力：基础ZMS对象布局及native引用身份/对象仍依赖RAM元数据。不能将backing文件保留等同于可恢复持久存储。

## 32项KUnit与私有lifecycle的覆盖

32项是26项state加6项codec的声明库存，包含通用引用/代际、快照、稀疏状态、header、abort/release等单元检查；其中已有native相关低层测试，**不是32项native落盘生命周期测试**，也不包含真实ZMS后端、GC、BIO、冷读完整往返。

本轮额外发现原`wb_sparse_state_test`无条件按y期待wb snapshot，而当前n的`snapshot_locked:2984`直接返回resident身份，存在必失败预期。独立`sddc-kunit-config.patch`已按n/y分别真实断言、不skip，26+6声明不变，生产路径不改；详见`KUNIT-CONFIG-REVIEW.zh-CN.md`。host提取检查不等于真实KUnit启动通过。

现有私有lifecycle在n配置下覆盖驻留alias/delta出现、候选源覆盖/discard、flatten→ZMS→显式flush→前台冷读和已知内容；并发分支只做驻留A/B重写/读取，不做native writeback并发。它仅记录`wb_aliases+wb_deltas>0`为可选观测，不强制两种native对象实际进入ZMS。源覆盖/discard发生在writeback之前，也未直接识别每个REF/generation。因此同一脚本即使在y上得到passed，也不能单独宣布native引用跨后端生命周期已经完整验证。

## 若后续只增加native=y，最少还需的证据

1. 建立明确独立变体，记录实际最终config、notes、代码/补丁SHA，并只改变native选择及对应的编译guard；当前profile故意拒绝y，不能静默改成y绕过。测试配置补丁同时适用于n/y，不以skip取代测试。
2. 在未挂载/未swapon的独占zramN真实产生ALIAS与DELTA，writeback并flush后、首次读取前，分别要求`wb_aliases>0`和`wb_deltas>0`及各自bytes有效，不能只看驻留alias/delta或两者任选一个。确认capture→install→finalize实际发生，而非全部fallback。
3. 维持现有全量已知数据/长度/hash、dirty=0、无物理/软件错误，并同时要求前台bd_stat读取、ZMS总物理读、loop读扇区增长，防止GC-only读取冒充冷读。记录native wire字节与仍驻留ref_bytes，不从逻辑页数推算物理节省。
4. 将覆盖/discard/槽位复用加入**native-WB之后**，保留alias/delta目标，并再次完整读回；至少观察明确的native对象和相关引用身份。若仍无法逐槽识别REF或generation，只能报告候选源测试，不能宣称所有关联引用已覆盖。
5. 增加native读取pin与目标重写/discard/GC移动同时进行的私有设备用例，最终按已提交版本校验；包含discard时必须允许正确零页并建立顺序边界，不能照搬当前“零页必错”的无discard A/B判据。完成后等待wb_aliases/wb_deltas与引用占用回到应有基线，检查reset/stop及未发布capture回滚，不遗留pin/reservation。
6. 针对私有设备的capture失败、ZMS store失败/读取EIO及过期cookie/header建立可观测负例，要求错误传播、旧内容/引用寿命保护、清理和恢复符合契约。现有codec/header单元测试不能替代这些真实事务边界。

这些是后续单变量验证要求，不是本轮已经执行的测试。本轮没有修改profile、打新包或运行ADB。
