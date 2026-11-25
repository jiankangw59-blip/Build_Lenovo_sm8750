# Ace 6 线程绑核

规则使用 AppOpt 的 `进程{线程}=CPU列表` 和 `进程=CPU列表` 格式。进程可以包含子进程后缀，选择器支持 `*`、`?` 和字符类。内核在线程创建、执行、改名和 cpuset 迁移时重新匹配。重新加载时也处理已启动的线程。

`affinityctl check 规则目录` 校验规则，`load` 原子替换策略，`status` 查看运行状态，`clear` 撤销策略并恢复仍由本后端持有的亲和性。未加载规则时，模块不调度绑核工作。规则文件按模板、应用分类、自动规则、实测游戏和手工规则的顺序读入。

`compact_profiles.py 源规则目录 输出目录` 将重复的自动生成配置写成 `app_profile_rules.profiles`。加载器按原顺序展开，手工文件仍为 `applist.prop`。可用 `affinityctl dump` 比较压缩前后的提交内容。

外部模块用于现机小范围测试。内建版本通过原调度器的亲和性路径更新应用此前请求的掩码，并记录原请求供撤销。Action 的 `enable_thread_affinity` 仅供 Ace 6 6.6.118 选择，开机后仍需加载规则才会生效。

`affinityctl watch` 监听现有 Scene 状态文件和面板亮度节点。`affinityctl adapt-load 规则目录 [进程模式]` 启用前后台分核。省略进程模式时，自适应覆盖主用户应用。手工规则和实测游戏规则优先。副窗口仍可见时沿用前台分配，退出监听后回到静态规则。Android cpuset 继续限制实际可用的核。

自适应部分覆盖应用焦点、可见窗口、息屏状态和普通线程角色。Winlator 专用跨组策略超出此版范围。Android ARM64 控制工具由 NDK clang 一起编译 `affinityctl.c` 与 `adaptive_watch.c`。
