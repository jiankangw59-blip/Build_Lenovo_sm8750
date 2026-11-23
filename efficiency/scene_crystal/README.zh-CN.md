# Scene 模块二：Crystal 早期初始化

此版本只初始化已明确登记的 Crystal 数据面，**不启动自动回写策略**。纯 LZ4KD 对照候选和后续 LZ4KDS 实验使用同一结构，但每个内核必须分别登记预期算法。当前只有主机控制流测试与载荷构建检查，没有 Crystal 真机初始化验收。

## 启用条件

所有条件同时满足才会执行：

- 机型为 PLQ110，存在 Crystal 的 `zms_stat`、`backing_dev`、`hybridswap_enable` 和 `hybridswap_core_enable` 节点。
- 私有目录 `/data/adb/ace6-kernel-efficiency` 存在，且不是符号链接。
- 私有目录存在普通文件 `crystal-writeback.enabled`。
- 当前 `/sys/kernel/notes` SHA256 在独立 `known-crystal-kernels` 中唯一对应一个算法。每行严格为 `<64位小写SHA256> <lz4kd或lz4kds>`；不复用原生的 `known-native-kernels`。
- 原有 `/data/swap_config.conf` 以字面标量指定 `zram=true`，`zram_size` 在 256～65536 MiB，`comp_algorithm` 与登记算法一致。当前配置 `16384 / lz4kd` 会建立 16 GiB 纯 LZ4KD；不改配置文件。

未登记、原生或 le9uo 内核在创建日志、锁或 loop 之前退出。算法不匹配、关闭 ZRAM 配置、容量为 0 或不支持的配置会标记 deferred 并保留现状，不绕过内核的算法锁。

`zram_writeback=false` 会选择普通 RAM ZRAM，不绑定回写文件；缺省和显式 true 会请求下面的私有回写设备。读取配置不执行 shell 代码，也不调用 dos2unix。

## 私有 backing 与转换

只复用已经在服务/人工准备阶段创建的 `/data/adb/ace6-kernel-efficiency/zms-backing.img`：固定 8 GiB、root:root、0600、普通文件、非符号链接。早期脚本不会创建、分配或格式化该文件，只设置所需的 `ksu_file` 标签并复用/建立唯一 loop 映射。不会接触原生 `zram-backing.img` 或厂商 crypto mapper。

完整匹配容量、算法、优先级 0 与 backing 的配置直接跳过 swapoff/reset/格式化，也不重置剩余逻辑回写预算。需要转换时，只有当前交换设备为 zram0、已用交换不超过 64 MiB，且可用 RAM 至少为已用交换加 2 GiB 才继续。发现外部 backing、多个私有 loop 别名、文件不符合条件或 RAM 不足会保留现状。

新 loop 编号返回后最多等待 3 秒让 ueventd 创建块节点。初始化使用 `/system/bin/losetup`、`swapoff`、`mkswap`、`swapon -p 0`，避免 BusyBox 的参数差异。已选中预期算法时跳过 comp_algorithm 写入，适配内核主算法锁。

转换失败后只在自身已停掉交换且没有外部 backing 时尝试恢复普通 RAM ZRAM；先尝试配置容量，元数据分配失败再尝试原容量。不会为了回退而格式化 backing 文件。恢复同样需要真机核验，主机模拟不能证明 Android syscall 行为。

## 调用和自动策略边界

hash-locked `prepare_scene.py` 从原始四份 Scene 文件生成载荷，post-fs-data 先调用 Crystal helper，再调用原生 helper。两套白名单独立，原生 helper 在 Crystal 节点存在时退出。晚期原有 startup 对完全匹配配置直接跳过；若不匹配，原有 HybridSwap runtime guard 会拒绝 teardown。原生自动回写 launcher 也会因原生 profile 不满足而退出。

本版本把 Crystal 的 enabled/core_enabled 保持 0，不设置 memcg 阈值，不启用 Crystal 周期策略，也没有添加 Crystal 自动控制器。ZMS 的功能性 dirty flush 和 GC 属于驱动数据路径，可能在手动测试后运行；“自动策略关闭”不代表所有内部工作都被停掉。

建立新 backing 时设置的 `writeback_limit=2097152` 是 8 GiB **逻辑 slot 迁移上限**，不是物理写量计量。正确自动回写需要后续按逻辑迁移、压缩流打包和实际后端 I/O 分开计量，不能直接复用原生 `limit + bd_writes` 的判定。

同一启动的 boot ID、notes、procversion、dmesg 保存到 `early-kernel-startup.raw`；初始化结果为 `early-crystal-writeback.status`，其中包含 boot_id、算法、目标容量、自动策略关闭和恢复状态。读取旧 status 时必须核对 boot_id。

## 已有主机检查

`run_host_checks.py --bash <Git Bash> --output <新审计目录>` 在私有目录模拟 sysfs 和 swap 工具，覆盖 19 项控制流：未启用/原生/le9uo退出、重复登记拒绝、配置选择、外部 backing、繁忙交换、低 RAM、幂等、两种算法的结构、关闭回写、配置容量，以及绑定/容量分配失败后的 RAM 恢复。测试明确标记 `kernel_io_mocked=true`、`android_runtime_verified=false`。

`scene_native/build_scene.py` 对生成的全部 shell 做 bash -n、重建原生事件程序，并输出全部文件 SHA256 和四份源哈希。与已验收原生载荷相比，已有 9 个配置/脚本/程序字节保持不变；已有文件仅 post-fs-data 增加 Crystal 前置调用。当前未部署到设备，也没有把主机结果当作启动或数据验证。

真机后续仍需核对：当前 notes 和 boot ID、早期结果、16 GiB/正确算法/优先级、原厂模块、既有原生控制器没有误启动；私有测试 zram 的 writeback 后必须再 fsync，并用真实后端读写增量和数据校验确认 ZMS 落盘。
