# 私有 ZRAM / Crystal ZMS 回写验证

工具只操作 `hot_add` 新建的 `zramN`（N > 0），每个种子重新创建设备，固定写入 32 MiB 已知数据，绑定新的 128 MiB 私有 loop 文件。拒绝 zram0、active-swap 别名和外来 backing；缺少机型、kernel notes、后端标记或所需编解码器时，必须在设备写入前退出。

本目录的编译和 host 测试不会使用 ADB；它们不代表内核功能已经通过实机测试。

```sh
python efficiency/crystal_tests/build.py --ndk ANDROID_NDK_ROOT --output FRESH_BUILD_DIR
python efficiency/crystal_tests/test_host.py --library FRESH_BUILD_DIR/pattern.dll --output HOST_TEST_REPORT.json
```

实机命令需要明确串号、编译 notes 白名单与后端：

```sh
python efficiency/crystal_tests/run_probe.py --serial DEVICE_SERIAL \
  --backend crystal --expected-notes COMPILED_NOTES_SHA256 \
  --expected-main-codec lz4kd --expected-main-sddc 0 \
  --codec lz4kd --expected-sddc 0 \
  --binary FRESH_BUILD_DIR/crystal-block-probe --output CRYSTAL_REPORT.json
```

`--check-only` 只读核对，不读取有创建副作用的 `hot_add`。原生对照使用 `--backend native --codec lz4kd`。主设备的编解码器要求与新建私有设备独立；实验内核可保持主 zram0 为 lz4kd，只在新建的、尚未初始化的私有设备上选择 lz4kds。已匹配算法时跳过写入，以兼容同值写入也返回 EPERM 的 Crystal 锁定策略。

每轮顺序为写入 → sysfs writeback → **再次对块设备 fdatasync/fsync** → 确认 ZMS dirty_pages=0、物理写计数增长 → 首次全量已知模式读回并确认物理读计数增长。填充阶段的 fdatasync 早于 writeback，单独使用不够。工具不执行全局 drop_caches，也不打开 Crystal 全局自动策略开关。

默认 `mixed` 模式与已有原生测试一致：零页、重复字节、随机和稀疏四类数据，两轮种子固定。SDDC 准入测试可使用 `--codec lz4kds --expected-sddc 1 --pattern sddc-pairs --require-sddc-conversions`：每页是 2 KiB 确定性随机块重复两次，每组两页相同，变体仅修改少量对应字节。必须检查实际 sddc_stat；选中 lz4kds 本身不能证明 alias/delta 已执行。本工具暂不宣称 SDDC 引用代际和并发生命周期专项测试已通过。

报告分别保留逻辑输入、压缩 payload、ZMS packed bytes、物理页计数、loop 扇区与 flush 计数。Crystal 的 writeback_limit 按逻辑 slot 消耗，bd_stat 和 zms_stat 有不同统计范围，不能套用原生“一逻辑页对应一回写块”或固定预算加计数公式。

```sh
python efficiency/crystal_tests/compare.py --native NATIVE_REPORT.json \
  --crystal CRYSTAL_REPORT.json --output COMPARISON.json
```

对照要求两边均为真实成功的实机报告、种子和完整输入相同。loop 指标包含该后端的 flush/GC I/O，但不包含创建测试文件的文件系统元数据，也不等于 UFS NAND 物理写入量。不同 codec 的对比不能把差异全部归因于 ZMS 打包；本工具不证明省电幅度。

失败清理会重新核对新设备和 loop 的所有权。若发现设备被激活、backing 改变或身份变化，保留设备、文件和阶段目录供人工核查，避免操作外来数据。
