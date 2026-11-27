# LZ4KDS 独立源码还原实验

本目录能独立编译并执行普通 LZ4KD 编解码、LZ4KDS 所用差分编解码及其包装器；不会安装模块或修改手机。完整 SDDC 引用管理源码保留为审查参考，尚未完成内核并发验收。

`restored_codec.c` 是结合所给三个 KO 反编译结果和 DWARF 声明，重新整理的可读 C 包装层。`vendor/` 保留固定提交 `Wcoom/oneplus_sm8750_kernel@13d291aef3d1d9607ad3fa5ff30dc23b2888a3da` 的 GPL/BSD 原始代码，SHA256 由 `source-manifest.json` 锁定，许可证和作者头保留。`shim/` 只向宿主提供类型、内存分配和边界检查；不会复制到内核源码中。

当前编译目标为普通 C 标量实现；未运行 ARM64 NEON、原 KO 或内核中的 SDDC worker。它不证明与所给 KO 二进制完全等价，也不证明已解决应用错页、零页或 system_server 崩溃问题。

## 已核对的契约

- KO DWARF：codec 16 字节，encode_state 位于 0、delta_window 位于 8；差分头 24 字节，magic/version/header_size/ref_id/ref_generation/ref_size/target_size 的偏移为 0/4/6/8/12/16/20。
- 普通输入固定 4 KiB；引用和差分目标各最多 4 KiB，共享连续 8 KiB 窗口；编解码状态使用 32 KiB。
- 普通编码返回“不可压缩”时保持输出容量不变。测试显式保留原始页，绝不把未产生的压缩流当成零页或合法输出。
- 已发现真实版本差异：所给 KO 的差分解码包装器接受 1..4096 字节的输出上限；保留源码版本要求恰好 4096。重建包装层保留 KO 的契约，两者都编译后交叉测试。
- 所给 KO 的 ZMS 回写会跳过 SDDC 表示；固定公开源码另有 native SDDC-to-ZMS 路径。两者不是同一个完整版本，不能仅凭函数同名视为等价，也没有证据认定所给 KO 或公开源码哪一份整体更新。

LZ4KDS 不只是换一个压缩算法名称。该实现同时涉及普通压缩流、差分流、引用 ID/代际、slot 快照、alias/delta 生命周期和读回恢复。codec 往返通过只覆盖其中的字节转换，不能替代引用与 ZMS 的并发、失败注入和真实块设备测试。

## 复现

Windows 需要 LLVM Clang 和 Windows SDK 的 kernel32.lib；不会依赖 Visual Studio CRT。Linux 可使用 Clang 和系统 libc。构建目录必须是新的目录，建议放到仓库外。

```powershell
python -X utf8 efficiency/lz4kds_lab/build.py --output C:\temp\lz4kds-build
python -X utf8 efficiency/lz4kds_lab/test_codec.py --library C:\temp\lz4kds-build\codec.dll --output C:\temp\lz4kds-build\tests.json
python -X utf8 efficiency/lz4kds_lab/test_guard_pages.py --library C:\temp\lz4kds-build\codec.dll --output C:\temp\lz4kds-build\guard-pages.json
python -X utf8 efficiency/lz4kds_lab/check_recovery.py --analysis C:\Users\22703\Documents\module-audits\ace6-three-ko-source-20261124 --output C:\temp\lz4kds-build\recovery.json
```

Linux 将 library 参数换成构建输出的 libcodec.so。构建使用 O2、Wall/Wextra/Werror；厂商源文件哈希不匹配即停止。报告记录编译命令、编译器、库哈希、输入种子和各项测试计数。

也可以用 NDK 交叉编译 ARM64 Android API 26 的实验库；交叉编译通过只证明可构建，未在手机执行：

```powershell
python -X utf8 efficiency/lz4kds_lab/build.py --target android-arm64 --ndk C:\Users\22703\AppData\Local\Android\Sdk\ndk\28.2.13676358 --output C:\temp\lz4kds-arm64
```

测试覆盖普通页、零页、随机不可压缩页、完整 8 KiB 差分窗口、压缩流差分后恢复原页、copy 与 borrowed 两种接口、截断和随机损坏流、参数边界、分配失败及释放。每个输出缓冲区带前后哨兵，模拟分配器也检查头尾哨兵和释放后的存活分配数。Windows 的额外保护页测试将输入和输出的实际结尾放在不可访问页前，用于捕获相应边界上的越界读取/写入。这些检查不等同于完整 ASan/KASAN，尚未覆盖所有内部读取和内核竞态。

随机损坏流可能恰好符合 codec 语法；编解码器不自带端到端校验和。报告单独记录被接受的随机样本，不能把“没有崩溃”写成“坏数据都被拒绝”。真正的引用内容校验、代际校验和读回页哈希必须继续在 SDDC/ZMS 层验证。

## 本次实际结果

宿主 Clang 22.1.4 编译通过，NDK Clang 19.0.1 的 ARM64 Android API26 交叉编译通过。宿主扩大测试采用 10,000 轮已知数据、50,000 轮双路径随机损坏输入：

- 普通页 8,750 次压缩往返、1,250 次不可压缩 raw 回退。
- 完整 8 KiB 窗口 delta 往返 10,000 次、压缩流差分后恢复已知页面 10,000 次；另包含 3 组零页/完全相同数据的差分测试。
- 损坏输入解码调用 100,000 次，99,972 次拒绝、28 次语法接受；有效流截断拒绝 120 次。
- 分配失败清理 6 次、参数边界 9 次；buffer/heap 哨兵无破坏，最终存活分配为 0。
- 独立保护页测试额外执行 20,011 次调用，未触发保护页访问异常，最终存活分配为 0。

扩大测试不代表手机端 LZ4KDS 已安全。Android 实验库没有在手机运行，SDDC 引用管理与 ZMS 并发路径未执行，未测量功耗或 UFS 写入收益。

## 与当前候选的边界

用户已进一步指定最终目标为 Crystal HybridSwap + ZMS + LZ4KDS，先前的纯 LZ4KD 候选保留为对照。本实验没有修改主候选配置、加载模块、绑定厂商 1 GiB 分区或变更 AppOpt。SDDC/LZ4KDS 主设备启用前，需先在独立 zramN 验证引用发布/销毁/复用、过期 generation、并发覆写/回收、ZMS 错误恢复、休眠恢复及已知内容冷读。具体集成要点见 `INTEGRATION-NOTES.zh-CN.md`。
