# le9uo 独立实机检查

此检查面向 PLQ110 / 6.6.89 的独立 le9uo 构建。先核对编译产物与运行中的 kernel notes，再运行探针：

```sh
python efficiency/le9uo_probe.py --serial DEVICE_SERIAL \
  --expected-notes COMPILED_NOTES_SHA256 --test-controls \
  --output le9uo-live-controls.json
```

检查 `workingset_protection=1`、`anon_min_ratio=0`、`clean_low_ratio=5`、`clean_min_ratio=0`。`--test-controls` 重写原值，并验证比例参数拒绝 -1 / 101；不提高内存压力、不改变日常策略。错内核阴性测试应返回未通过，并在任何控制写入前退出。

比例单位是节点总 RAM 的百分比。默认 5% 为干净文件缓存的软下限，覆盖范围包含干净文件页，不能称为只保护可执行文件页。探针记录当前干净文件页与阈值快照，快照不证明保护分支实际执行。

完整验收还需运行 `device_gates.py`，使用同次启动的早期日志和编译 notes；另做自有 ZRAM 页面写回、读回校验。自动回写验证需要先独立记录 `bd_stat` 写计数，再执行真实熄屏事件、等待至少 300 秒，并调用 `scene_native/verify_auto_writeback.py`。跨启动日志或旧计数应被拒绝。

参数检查、数据完整性与五项底线验收不代表已测得降低 refault、前台延迟或功耗。收益需要与原生基线在相同负载下另行对比。
