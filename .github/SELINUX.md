# SELinux 用户查询与实时执行面的边界

`selinux_seqno.py` 在 SUSFS 补丁之后应用兼容修改；`selinux_projection.py` 负责用户查询边界。BakaSU、SUSFS 源码及修改后的九个文件均由 CI 记录哈希，配套管理器使用同一份输入重放补丁并核对哈希。

`selinux_ids.py` 在上述边界之上增加经过验证的 ID 快路径。兼容标记和快照指针属于每个 `selinux_policy` 对象；所有发布入口都在 RCU 发布前清除复制来的旧标记。完整策略提交、KSU 初始注入及结构性更新执行完整校验；只有已审计的纯权限批次和原生布尔更新，且旧证明、当前策略指针、快照、相邻代数及符号表规模均匹配时，才继承已有证明。未知命令、部分失败的结构修改及任一条件失配均回退完整校验。校验包括原厂主名称前缀、双向别名、user/role/type/sens/cat ID、MLS 模式，以及 class 和 common/class-specific permission 名称与位值。live-only 名称映射到原厂 ID 会触发回退。

兼容时，显式用户入口采用从原版函数生成的单遍 live 解析器，在进入 MLS 前拒绝新增 user/role/type，保留 live 合法性检查并追加快照检查；AV 决策直接使用实时 SID 的上下文，不做额外上下文转译或分配。失配时保留规范化映射，并按名称转换 class 和权限位，新增或有歧义的权限默认拒绝。原版内部解析器和实际内核 AVC 热函数保持原样。

新增内部元数据位于 `selinux_policy` 末尾，四项编译断言检查旧字段偏移。结构总大小仍有变化；依赖该内部结构大小的预编译模块需要单独核查。每个策略准备发布时记录一次 generation、兼容标记及快照标识；查询不添加日志或原子计数。可短暂跟踪 `string_to_context_struct_user_ids`、`security_compute_av_user_ids` 和回退函数来验证实际分支，移除跟踪后再做时序测试。零额外转译分配仅指兼容 AV 分支，原版 MLS 解析仍有自身的位图分配，不能由此承诺固定纳秒开销或严格恒定的耗时比。

- 只在首次 KSU 规则修改前保留不可变的策略数据库快照；快照的 `sidtab = NULL`，不创建第二套 SID 缓存。
- `security_context_to_sid_user()` 显式请求快照校验。兼容时直接单遍解析到实时 SID 表；失配回退时，原版解析器只扫描一次私有副本中的原始输入，再将规范化后的上下文按名称转译到实时策略，经过原版实时解析器校验后登记到实时 SID 表。重复 MLS 类别先被折叠，避免合法长输入被完整扫描两次而非法长输入仅扫描一次。普通、default、force 转换入口不请求投影。
- selinuxfs 的七个交易节点使用该输入包装；`selinux_setprocattr()` 仅替换普通解析调用。原版权限检查、空串处理、强制解析分支及实际域切换保持原样。挂载、xattr、网络标签入口继续使用原版接口。
- `security_compute_av_user()` 的隐藏分支从实时 SID 取上下文，按名称转换到快照符号，再使用原版决策计算函数。临时分配沿用不可睡眠路径；失败时保留初始化的拒绝决策。实际内核 `security_compute_av()` 和 `avd_init()` 保持不变。
- status、上下文校验及用户决策共用无分配、无 AVC 查询的选择器。6.6 的隐藏状态页保持 `sequence=0, policyload=0`；真实策略的发布、AVC 和状态通知同步使用真实代数 N。

`create/relabel/member/user/validatetrans` 的决策函数保持原版。2026-11-22 在 OPD2413 上验证：`untrusted_app` 的五个节点均在 open 被拒绝；`app_zygote` 的前四个在 write 被拒绝，`validatetrans` 在 open 被拒绝。此结论只适用于被测试策略；回归探针若发现节点可达，会报失败，需审计对应的用户决策入口。

CI 检查原版函数体、预处理结果和 ARM64 对象的调用引用，并确认 SUSFS 构建没有启用 BakaSU 的旧运行时函数表接管。完整启动镜像还必须通过 AVB 和内核载荷校验。

实机回归需分别静态编译 `scripts/selinux_probe.c` 和 `scripts/selinux_sid_probe.c`，然后执行：

```sh
python3 scripts/run_selinux_verify.py --serial DEVICE \
  --probe /path/to/selinux_probe --sid-probe /path/to/selinux_sid_probe \
  --app-uid ACTUAL_APP_UID --boot /path/to/boot.img \
  --require-module-rules --output /path/to/acceptance
```

该检查会应用一次冗余的 getattr 规则以验证策略代数通知，并切换系统设置页面采样 AVC。验收包含 status 的 read/mmap 与四项 ABI 检查、真实与隐藏规则、九组 SID 增量 `[0,0,32,0,32,0,0,0,0]`、64 次拒绝查询的零 SID 增长、动态改策略后的代数同步、AVC 命中率以及新增 RCU/睡眠告警。上述探针通过后，仍需对 Luna 等目标应用冷启动复测。
