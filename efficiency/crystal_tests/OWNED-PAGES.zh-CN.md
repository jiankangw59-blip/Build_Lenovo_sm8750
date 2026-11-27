# 主 ZRAM 的自有页数据验证

`owned_pages_probe.c` 面向已配置好的主 zram0。它不从块设备读取或写入原有数据，不修改容量、压缩算法、backing 或 writeback_limit，也不执行 swapoff/reset、全局 idle 标记或 drop_caches。

测试载荷固定为本进程的 4 MiB 匿名私有映射，使用已校准的 `sddc-pairs-2k-v1` 数据。只对其中连续 64 个 4 KiB 页调用 MADV_PAGEOUT；通过 `/proc/self/pagemap` 取得每页的真实交换槽，并在每次 `page_index=slot` 请求前重新核对映射。随后以不带 O_EXCL 的只读主块设备 fd 执行 fdatasync/fsync，再 fault-in 并逐字节校验全部 4 MiB。主块 fd 不执行 read/pread/write/pwrite。

```sh
python efficiency/crystal_tests/run_owned_probe.py --serial DEVICE_SERIAL \
  --backend crystal --codec lz4kd --expected-sddc 0 \
  --expected-notes COMPILED_NOTES_SHA256 \
  --expected-backing-file /data/adb/REVIEWED_STATE/REVIEWED_BACKING.img \
  --binary FRESH_BUILD_DIR/crystal-owned-pages-probe --output OWNED_REPORT.json
```

实验主设备确实选中 lz4kds 后，可明确指定 `--codec lz4kds --expected-sddc 1`。本工具不会替用户切换主设备算法。原生对照使用 `--backend native --codec lz4kd`。`--check-only` 只读；预检必须先通过 PLQ110、完整 notes 白名单及独立原始 notes 哈希、sole swap、codec、明确 backing 所有者/权限/标签和预算余量，才会暂存或执行 helper。C 本体还会再次核对 notes 原始字节和 backing 文件身份。

使用已有的预算，最多发起 64 次自有交换槽请求。报告分开记录逻辑请求数、观察到的预算消耗、bd_stat 增量和 Crystal 的 ZMS 物理页/dirty_pages。Crystal 要求显式 flush 后观察到 dirty_pages=0，以及真实物理写入和随后读回计数增长；不会要求物理回写恰好等于 64 页。其他主设备并发 I/O 会影响这些全局计数，报告不能证明每个请求独占对应物理 I/O，`all_requested_slots_backed_proven` 保持 false。

全部自有数据一致、交换槽所有权核对、真实后端 I/O 与 flush 是不同证据。该探针使用 MADV_PAGEOUT，不能证明 Kcompressd 的 kswapd 卸载路径，也不证明 SDDC 引用代际或并发生命周期安全，不代表 UFS NAND 写入量或功耗收益。

`build.py` 会同时构建私有设备 helper 和本探针。host 自测命令如下，只测试拒绝契约、真实编译 C 的 pagemap 解码和 4 MiB 数据模式，不操作手机：

```sh
python efficiency/crystal_tests/test_owned_host.py --library FRESH_BUILD_DIR/pattern.dll \
  --output OWNED_HOST_TEST_REPORT.json
```
