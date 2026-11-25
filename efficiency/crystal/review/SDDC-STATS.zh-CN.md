# SDDC 只读完整性统计补丁

`sddc-integrity-stats.patch` 单独为后续自动回写候选补齐可观测性。它不改变现有两种SDDC候选，不应在已有构建结果上声称这些字段已经可用。

固定顺序是原始公开pin `13d291aef3d1d9607ad3fa5ff30dc23b2888a3da` → `sddc-integrity.patch` → `sddc-integrity-stats.patch` → 另由集成器执行charging/deferrable平台适配。两个patch都使用 `patch -p1 --fuzz=0 --batch --forward -i ...`，不能跳过每阶段源SHA校验。平台适配会另外改变zram_drv.c，因此下表是应用平台适配之前的SHA。

- 既有integrity patch SHA256：`dea7c0e140926560adbe5fa6a30942c0f3b919c14690de613b49fdaf704e2554`
- 本stats patch SHA256：`6690acd331ca6906ab065a389f67bdbaf917de5076e6cf9c42ff8165cfd6aba0`

| 目标文件 | stats后的SHA256 |
| --- | --- |
| sddc/crystal_sddc.h | `c3565977600b99388a85a0b71cf5114657812d53c40fafa0bac8e143d275c4a5` |
| sddc/crystal_sddc.c | `ed2e2a521865eced8bffac5629b5d1d72bb0e44f69ce19a90f3fddc2fd4f58a8` |
| zram_drv.c | `997e92108bec7237d314f5c95eed31a2f60c18625e42e17f669e0808867112f9` |

精确原始、中间、最终SHA保存在 `sddc-integrity-stats.json`。

只在Crystal私有 `crystal_sddc_stats_snapshot` 末尾增加 `integrity_checks`、`integrity_failures`、`integrity_skipped`，保持已有成员相对顺序；`get_stats` 从既有atomic64计数读取这三个字段；`sddc_stat`末尾追加同名三行，原46项顺序保持，总计49项。CONFIG=n及无manager时仍由现有sizeof(snapshot)清零路径返回零。

没有改变对象内容、wire格式、ZMS格式、已有数据校验逻辑、全局内核结构或导出KMI，不添加工作队列、定时器或新的计数写入。统计本身不保证整个多字段快照原子一致，跨读取发生并发更新时仍应按单调计数处理。

`verify_sddc_stats.py` 已在独立fixture按顺序零fuzz应用两个补丁，逐阶段核对3文件完整SHA；检查49个格式字段与参数完全同序且无重复、最坏长度小于4KiB，检查get_stats的3项atomic读取与enabled0/1模式模型。运行：

```powershell
python -X utf8 efficiency/crystal/review/verify_sddc_stats.py `
  --upstream C:/Users/22703/Documents/module-audits/ace6-efficiency-20261123/crystal-upstream `
  --output C:/Users/22703/Documents/module-audits/ace6-efficiency-20261123/crystal-reverse-review/sddc-stats-new
```

这是host静态与模式检查，未编译完整内核、执行KUnit或验证手机运行时。事件控制器对LZ4KDS缺失 `integrity_failures`、`decode_failures`、`flatten_failures` 或 `delta_proof_failures` 的内核拒绝启动，不以0填补缺失统计；当前没有这些导出的候选仍只能继续手动私有设备数据验证。
