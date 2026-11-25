# Ace 6 线程绑核

规则使用 AppOpt 的 `进程{线程}=CPU列表` 和 `进程=CPU列表` 格式。进程可以包含子进程后缀，选择器支持 `*`、`?` 和字符类。内核在线程创建、执行、改名和 cpuset 迁移时重新匹配。重新加载时也处理已启动的线程。

`affinityctl check 规则目录` 校验规则，`load` 原子替换策略，`status` 查看运行状态，`clear` 撤销策略并恢复仍由本后端持有的亲和性。未加载规则时，模块不调度绑核工作。规则文件按模板、应用分类、自动规则、实测游戏和手工规则的顺序读入。

`compact_profiles.py 源规则目录 输出目录` 将重复的自动生成配置写成 `app_profile_rules.profiles`。加载器按原顺序展开，手工文件仍为 `applist.prop`。可用 `affinityctl dump` 比较压缩前后的提交内容。

支持 `profile_wire=1` 的内核会直接读入共享配置，按进程选择器定位规则集，再按精确线程名哈希查找，必要时检查通配及默认规则。引用记录保留各套配置的生效顺序。`rules` 显示逻辑生效数量，`wire_lines` 显示提交行数，`stored_rules` 显示实际存储的规则定义数。旧内核仍接收平铺规则，`affinityctl dump` 输出平铺格式，`dump-compact` 输出共享格式。

外部模块用于现机小范围测试。内建版本通过原调度器的亲和性路径更新应用此前请求的掩码，并记录原请求供撤销。Action 的 `enable_thread_affinity` 仅供 Ace 6 6.6.118 选择，开机后仍需加载规则才会生效。

`affinityctl watch` 监听现有 Scene 状态文件和面板亮度节点。`affinityctl adapt-load 规则目录 [进程模式]` 启用前后台分核。省略进程模式时，自适应覆盖主用户应用。手工规则和实测游戏规则优先。副窗口仍可见时沿用前台分配，退出监听后回到静态规则。Android cpuset 继续限制实际可用的核。

自适应部分覆盖应用焦点、可见窗口、息屏状态和普通线程角色。Winlator 专用跨组策略超出此版范围。Android ARM64 控制工具由 NDK clang 一起编译 `affinityctl.c` 与 `adaptive_watch.c`。

`affinityctl watch-games` 同时监听 Scene 的 `games.xml`，把前台游戏标记交给内核。没有手工整包规则的游戏会按实际线程运行时间选出前两名，绑到 `CPU6–7`。第三名达到持续负载阈值时使用 `CPU4–7`。实测和手写线程规则优先，线程仍受 Android cpuset 限制。采样结束、游戏失焦或息屏后，定时工作停止。已导入的实测规则保留，运行时排名随游戏进程和前台会话重新计算。

开机接管包把 `boot-service.sh` 安装为 KernelSU `service.sh`，在 `rules/applist.prop` 保留可编辑规则。控制进程只负责提交规则和转发 Scene 通知，绑核在内核执行。模块的操作按钮校验并重载规则。启动前检查设备接口和旧进程，失败时清除已接管策略并恢复旧模块。此入口面向线程亲和性。`uclamp`、优先级、系统服务开关和厂商频率调节继续由系统自己的配置负责。

用 `ksu/package.py --controller affinityctl --rules 规则目录 --output Ace6ThreadAffinity.zip` 打包配置界面。升级时继续使用已安装的 `rules/`。在 KernelSU 管理器打开模块 WebUI，可编辑手写规则、读入已停用 AppOpt 的 `applist.conf` 或 `applist.prop`，也可读回上次备份。点击保存会由内核校验完整配置并立即重载。
