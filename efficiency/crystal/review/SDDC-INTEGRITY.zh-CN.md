# SDDC 数据完整性复核与最小加固

本轮没有找到已被真实输入或合法并发顺序证明的错页、零页根因。已确认两处完整性防线不足，并提供独立补丁：拒绝字节不同的 delta 自校验结果；完整页 hash 不符时向上返回 `-EIO`。这是防止静默错误的加固，不应描述为已经修复某个错页根因，也不能保证所有损坏都会被发现。

仅审阅固定公开源 `Wcoom/oneplus_sm8750_kernel@13d291aef3d1d9607ad3fa5ff30dc23b2888a3da`，并对照用户提供的 KO 反编译。两者属于不同实现变体，开发先后不确定；三个 KO 的内部 CRC 一致、6.6.118 vermagic 均不能证明本机 6.6.89 兼容。

## 生命周期与并发核对

以下行号均为未应用补丁的公开源，目录为 `drivers/crystalfrostwork/crystal_hybridswap`。

| 检查点 | 确认的执行顺序 | 边界 |
| --- | --- | --- |
| 借用的 delta_window | `sddc/crystal_sddc.c:3176` 获取 stream；3192 调用 borrowed 解码；3197 校验指针/长度；3205、3208 拷贝或3211完成普通解码；3217才释放 stream | 本路径未发现解锁后再访问借用指针。`zcomp.c:325-334` 保持 per-CPU stream 锁；codec 普通解码不覆写 delta_window |
| 槽位读取与重写 | `capture_locked:3224-3304` 在槽位锁下同时捕获 wire、expected_hash、代际与 ref pin，再解锁解码 | 合法重写不会要求把旧快照数据与新槽位 hash 比较，因此 hash 错误返回不会把正常版本变化当作损坏 |
| 引用发布与复用 | `ref_pin:973-1059` 核对 cookie 并 inc_not_zero；`ref_put:1064-1084` 在同一 XA 锁下移除最后引用，延后释放；generation跳过0 | ID 回收晚于引用实际释放。本轮没有构造出代际复用绕过有效 pin 的合法时序 |
| 后台作业提交 | `job_key_matches:1154-1190` 同时核对 mutation、handle、size、prio、reservation 与 flags；`commit:1890-1932` 在目标槽位锁下复核 | 过期作业不会仅凭相同 slot index 提交 |
| 相似页与 alias | `try_alias:1943-1968` 比较头尾及中间内容 | alias并非仅依赖 hash 或样本相同 |
| discard / writeback | `zram_drv.c:4140-4144,5282-5288` 等待 UNDER_WB；free_notify在5463-5469跳过忙槽位；2110-2163在安装前复核快照 | 未确认能让旧ZMS对象覆盖新槽位的路径 |
| flatten-to-ordinary | native SDDC_ZMS_NATIVE=n；`zram_drv.c:1962-1993` 捕获快照并 flatten，SDDC ref pin保持到完成；`crystal_sddc.c:3405-3441` 输出普通压缩流 | 无需持久化SDDC引用图。本补丁没有新增flatten后的整页解码/hash验证，也没有更改ZMS格式 |

用户 KO 的 `crystal_sddc_restore_page` 在反编译 C:39685 使用拷贝式 `zcomp_decompress_delta`，不是该公开源的 borrowed API；不能把一边的指针生存期结论直接套到另一边。

## 补丁改变

1. 原 `try_delta_from_source:2132-2136` 只核对解码返回值与长度。新增 `crystal_sddc_delta_proof_valid`，逐字节比较已还原的压缩流与仍由作业持有的原始 `target_data`。不一致时走既有 `goto out`，释放本次临时资源、不提交目标替换，保留目标原数据。源槽位若已被独立提升为有效 REF，不需要回滚该正确变换。
2. 原 `check_page_hash:808-836` 在失败时只计数并打印日志。改为返回 `-EIO`，并在普通 resident、已锁定 resident、native-WB restore 三个调用点赋给 `ret`。保留缺失 expected_hash 时的既有跳过行为，不改变旧快照兼容语义。
3. 加入两个私有 KUnit 用例：同长度错误 proof 在头、中、尾均被拒绝；用独立 zsmalloc pool 和 raw REF 调用真实普通/locked读取入口，正确 hash 返回0，错误 hash 两次均返回 `-EIO`，并检查解码/完整性计数、引用计数与 active_ops恢复。测试不建立真实zram设备，不改全局参数、zram0或用户backing。

状态 KUnit 从24增加到26，另有codec 6项，总计32项。它们只在测试配置打开时编入；必须启动执行后才能说通过。没有更改导出 KMI、wire/version、ZMS持久化格式；没有启用 SDDC_ZMS_NATIVE。

读取 hash 失败会成为可观测的I/O错误，而不再把错误内容成功返回。补丁没有提供纠错或备用数据源，swap消费者可能受到I/O错误影响。已有32位页hash也存在碰撞边界，不能替代测试中的已知pattern/完整SHA校验。

开销主要是后台成功delta候选增加一次最多PAGE_SIZE的memcmp。现有读取本就计算页hash，读取改动仅传播其结果；没有新增周期任务、轮询或唤醒。

## 校验与复跑

精确文件集合只有 `sddc/crystal_sddc.c`。元数据见 `sddc-integrity.json`。

| 构件 | SHA256 |
| --- | --- |
| 原始目标源 | `7f0b3dc9c0cc9a0ce54d3aa5dbc05aac2bb464f7778969281112981055a9af2d` |
| `../sddc-integrity.patch` | `dea7c0e140926560adbe5fa6a30942c0f3b919c14690de613b49fdaf704e2554` |
| 补丁后目标源 | `e51140d12d447b21dc61101f819ad764a60d84ca949c8956a9a55f8095f6aa05` |

在干净固定源根目录中应用：

```sh
patch -p1 --fuzz=0 --batch --forward -i /absolute/path/efficiency/crystal/sddc-integrity.patch
```

Windows宿主检查：

```powershell
python -X utf8 efficiency/crystal/review/test_sddc_integrity_host.py `
  --upstream C:/Users/22703/Documents/module-audits/ace6-efficiency-20261123/crystal-upstream `
  --output C:/Users/22703/Documents/module-audits/ace6-efficiency-20261123/crystal-reverse-review/sddc-integrity-host-new
```

输出目录必须不存在。宿主脚本校验原始/补丁/输出SHA，使用GNU patch --fuzz=0重新应用，并从补丁后源码提取两个真实C helper，使用LLVM22编译DLL、通过ctypes执行。已完成5238个proof断言及6个hash分支断言，并静态核对三个读取点都传播返回值；hash计算使用注入值，未执行内核的jhash、真实slot锁或zsmalloc。证据见 `sddc-integrity-host-checks.json`；原始构建输出和DLL留在本机audit的 `crystal-reverse-review/sddc-integrity-host-v1`。

本代理尚未编译完整内核、执行KUnit或操作手机。KUnit负例已写入patch，需要候选CI和启动日志验收。

## 尚未覆盖的实机窗口

后续只在未挂载、未swapon的独立私有zramN和独立backing测试，不对zram0注入损坏，也不执行全局drop_caches。

- 相同目标交替写两组完整pattern，另线程并发读、discard、观察队列处理；每次已完成写入的版本用测试同步序号约束，读取只能出现允许的完整版本或明确错误，不能把正常先后版本误判成错页。
- 快速重用同一slot与相似页引用，检查generation变化、过期job拒绝、引用对象在最后pin释放前不会销毁；读完后active_ops与临时引用应恢复。
- SDDC_ZMS_NATIVE=n的DELTA/ALIAS/REF分别writeback，随后对zramN执行fsync，dirty为0且后端物理写增加后再读；要求首次冷读物理读增加、全量pattern/SHA正确，flatten后的槽位成为普通ZMS对象。
- 私有设备正常读写同时重复stop/reset需要遵守设备openers/claim合同；本轮未覆盖真正内存压力、CPU迁移、reclaim、BIO错误恢复与循环reset的并发组合。
- 观察完整性失败和解码失败计数；增加任何失败必须保存该时段日志、期望pattern及操作序列。不能因本补丁把错误变成-EIO就声称数据路径已经稳定。
