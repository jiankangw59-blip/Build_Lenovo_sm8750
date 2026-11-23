# LZ4KDS / SDDC 集成核对

普通 LZ4KD 与 LZ4KDS 的关系已从固定公开源码核对：`zcomp.c` 的 lz4kds_backend_ops 在普通编解码之外提供差分编解码操作；lz4kd_pure_backend_ops 的差分操作为 NULL。`crystal_sddc_create()` 仅在主算法支持差分且为 4 KiB 页时创建 manager，普通 lz4kd 不会因此自动启用 SDDC。

启用路径涉及下列配置；本实验没有写入主候选的配置。

```text
# CONFIG_ZRAM is not set
CONFIG_CRYSTAL_HYBRIDSWAP=y
CONFIG_CRYSTAL_HYBRIDSWAP_ZRAM_WRITEBACK=y
CONFIG_CRYSTAL_HYBRIDSWAP_SDDC=y
CONFIG_CRYSTAL_HYBRIDSWAP_SDDC_LZ4KD=y
CONFIG_ARM64_4K_PAGES=y
CONFIG_CRYSTAL_HYBRIDSWAP_ZRAM_DEF_COMP="lz4kds"
```

SDDC 格式直接落入 ZMS 另由 `CRYSTAL_HYBRIDSWAP_SDDC_ZMS_NATIVE` 控制，不能与普通压缩流打包回写混为一项。所给 KO 跳过 SDDC 表示的回写，固定公开源码包含 native SDDC-to-ZMS，因此两者的写回/读回路径不能凭函数名视为相同，现有材料不足以断言整体新旧。

`CRYSTAL_HYBRIDSWAP_ZRAM_LOCK_DEF_COMP=y` 会拒绝主算法的任何写入，包括重复写当前值。Scene 或独立 zramN 测试工具必须跳过同值写入；若要在同一测试内核的两个 zramN 上分别选择 lz4kd/lz4kds，则测试候选需允许选择主算法。两种算法的公平对照也需留意：private codec 编入后，名为 lz4kd 的普通路径使用同份私有基础编码器；未编入时使用 Crypto API 的实现，基础编码器可能有差异。

`CRYSTAL_HYBRIDSWAP_SDDC_KUNIT_TEST=y` 可编入公开源码已有的 6 项 codec 测试与 24 项状态测试。需要运行并检查结果，只有编译通过不能算 KUnit 验收通过。状态覆盖包括引用发布、generation 重用、reservation pin、slot owner、snapshot identity、stale job、稀疏写回状态、中止 reservation、mutation 序号回绕、flatten 快照、wire header、manager 准入及清理。

独立 zramN 的实际数据验证应确保触发过 SDDC：公开源码的准入门槛为压缩对象大于 256 字节，delta 目标还需大于 512 字节，收益超过 24 字节 header 与 8 字节门槛。零页、重复字节等很小的压缩对象会跳过该路径，不能据此判定 SDDC 已工作。可用每页 2 KiB 确定性随机块重复两次作为普通压缩后仍约 2 KiB 的数据；完全相同页组用于 alias，少量成对字节变化用于 delta。

应把 sddc_stat 的实际 alias/delta 增量与已知页面的逐字节校验同时作为结果：覆盖 ref slot 重写与 discard、引用 ID/generation 复用、转换工作与覆写交错、压缩流在锁释放前消费、pending pin 中止与释放、ZMS pack/flush 后真实冷读、batch-in/prefetch、错误返回与重试、休眠恢复。native SDDC-to-ZMS 还需核对落盘 delta/alias 引用仍被固定、读回时 cookie 与页哈希有效，不能仅用 resident-hit 读回充当后端冷读。

本次没有找到可具体确认的 codec 错页/零页根因。KO 与保留源码的差分输出上限契约确有差别，但公开 `zcomp_decompress_delta()` 与 borrowed 版本都会先初始化上限为 PAGE_SIZE，调用链满足其包装器要求。因此不应把放宽上限当作已证明有效的主驱动稳定性修复。随机损坏流被语法接受也不等于修复或复现了已知内核问题；正确引用与上层校验仍需在完整状态机中验证。
