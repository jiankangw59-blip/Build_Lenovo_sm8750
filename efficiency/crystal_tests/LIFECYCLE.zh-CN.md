# 私有设备 SDDC 生命周期验证

此工具只运行于 `hot_add` 新建的未激活 zramN（N > 0），复用 `PrivateRound` 的设备身份、active-swap、backing 所有权、loop 解绑和清理保护。C helper 再次拒绝 zram0、别名、非块节点、active-swap、错误容量，并以 O_DIRECT/O_EXCL 操作固定 32 MiB 私有设备。

默认顺序测试：

1. 用旧 seed 的 sddc-pairs 充满，要求实际 alias 与 delta 计数均大于 0，并全量读回。
2. 每 8 页仅把前 2 页覆盖为新 seed，校验整个 32 MiB：前 2 页必须是新数据，其余 6 页必须仍为旧数据。
3. 仅 discard 每组前 2 页，再全量校验：这些页必须为零，其余旧数据保持不变。
4. writeback 后同步私有 zramN，再经专用 helper 同步并定向驱逐同一 owner backing 文件缓存；首次冷读前后要求 loop status/sysfs DIO=1、4KiB 逻辑块与 mincore resident_pages=0。后置检查不先驱逐。保留 ZMS dirty_pages=0、前台 bd_stat 与 physical/loop 读取增长、全部已知数据校验条件。不清全局 cache，不测 NAND 或断电持久性。

```sh
python efficiency/crystal_tests/run_lifecycle.py --serial DEVICE_SERIAL \
  --expected-notes EXPERIMENTAL_KERNEL_NOTES_SHA256 \
  --expected-main-codec lz4kd --expected-main-sddc 0 \
  --binary FRESH_BUILD_DIR/crystal-lifecycle-probe \
  --backing-binary FRESH_BUILD_DIR/crystal-backing-probe --output LIFECYCLE_REPORT.json
```

私有设备固定使用 lz4kds/SDDC=1。主设备可继续保持 lz4kd；工具不修改主设备算法或日常参数。缺少 Crystal 标记、编译支持或 notes 不匹配时必须在设备写入前退出。`--check-only` 只读预检。

可选 `--concurrent-seconds 2`（可选 2–5 秒）会另外创建一个全新私有设备，填充 A 数据后启动两个互不重叠的写者，分别操作偶数页和奇数页，在各自页面交替写 A/B。读者每次只能接受完整 A 或完整 B；零页、混合页或错误立即失败。停止并 join 后，按每页最后一次成功写入的版本校验全部 32 MiB。运行期不比较不稳定的全盘 hash。并发用例不执行 discard，也不复用顺序用例已 discard 的设备。

报告保存各阶段 sddc_stat，包括 refs/stale/released_rewrite/released_discard/wb_aliases/wb_deltas。每组前两页仅是优先覆盖可能的源页：没有逐槽 REF 标记时，不能证明精确覆盖了每个 REF；也不能直接观察所有 generation ID。顺序与有限并发通过不代表所有竞态已被覆盖。

ZMS 原生 alias/delta wire 回写是否被观察到会单独记录；后端物理 I/O 与全量已知数据验证始终必需。loop/ZMS 统计不等于 UFS NAND 写入，也不证明功耗收益。

host 检查只验证真实编译 C 的模式、拒绝条件和 A/B 分类器，不模拟或宣布内核原子性已通过：

```sh
python efficiency/crystal_tests/test_lifecycle_host.py --library FRESH_BUILD_DIR/pattern.dll \
  --output LIFECYCLE_HOST_REPORT.json
```
