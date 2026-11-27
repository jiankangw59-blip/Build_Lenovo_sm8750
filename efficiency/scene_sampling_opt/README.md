# SceneSamplingOpt API 102

Scene 主进程专用的采样去重、Mini同值显示提交、线程浮窗生命周期与快捷面板刷新率保存修复模块，当前版本1.0.6。采样相关原调度、记录频率、强制刷新、统计换算保留；刷新率保存的唯一改动见下文。不修改原 APK 或 daemon，不新增周期任务。

只支持本次审阅的 `com.omarea.vtools` APK：versionCode `920261102`，SHA256 `11b9ce48ebddb1ffab21601b44bccc0ae68d5bda325923fd924380b060010ff3`。版本、结构或摘要不同则跳过，不猜测匹配。

## 实现

- 快捷面板 `a.cj0` 仅在原 `e=true`（显示管理启用）、`f=false`（非特殊目标）、`g=false` 时，将其构造器的第4参数改为true，由原构造器正常初始化final字段。完整保留原 `ed1.j(app,null,max)` 和 `ed1.i()` 保存路径，不另行补跑/排队保存，不改变 `dynamic_control`、原开启总调度的路径或手动显示请求。原28条规则中的其他条目、原min值、min0及0删除语义继续由Scene原代码处理；全局 `features/refresh_rate.conf` 不被此修复写入。
- 刷新率诊断只在真实快捷面板选择的调用域观察原 `mg0.i(...,"refresh_rate.conf",bytes)` 返回值，区分保存成功/false/异常；原 `ed1.i()` 会忽略false，本模块报告它而不冒充成功或自动重放用户编辑。保存的是用户意图，不等RPC或物理显示ACK。该修复不单独提供 `dynamic_control=false` 时的自动显示规则消费者，需与独立事件显示backend分别验收。
- Mini仅在当前已附着窗口的`a.bi0.run()`动态调用域内，处理四个精确TextView和CPU图。文字必须是普通String/NORMAL，且无watcher/editor/filter/transform/movement/selection/autoLink/marquee/auto-size等特殊语义；首轮、内容/样式变化、待布局、未知状态均执行原方法。
- CPU图只在真实队列完整内容、成功快照、样式与几何状态都相同时省`setData`；不保存共享`fi0.x`数组引用。GPU滑动历史每轮仍执行原更新。Mini保持1500ms原采样，所有RPC/字符串组装仍保留；本项不能当作减少daemon原Mini采样成本。
- 修复 `a.qi0.b()` 关闭线程浮窗时，经 `d → e` 再次启动隐藏采样的问题；原息屏路径也会调用该关闭方法。只在关闭调用域阻止重启，并要求启动/首次采样属于当前已附着窗口。保留列表1000ms、详情250ms与正常可见功能，不拦通用get-threads。
- WindowManager.addView在本机先返回、再执行首帧附着。当前窗口尚未附着时只登记一次附着事件，收到后再启动原任务；不创建定时重试或额外采样循环。
- 启用时主线程一次性确认静态窗口为空/已脱离，再取消其遗留的静态任务x；不扫描或取消其他协程。恢复中的suspend调用保留原成功/失败处理。首次加载默认观察模式，须显式启用该修复；本机部署完成后此开关会保存在模块自己的设置中。
- `a.at0.g(long)` 在专用可重入锁中调用完整原方法，等待者按自身原 TTL 再判断。`0` 强制刷新不被吞掉，充电控制 1 秒和记录/UI 5 秒要求均保留。
- `a.c41.b()/c()` 共用另一个锁。full 每次保留原查询；min 只复用原本允许的 500ms full 缓存，缓存时间仍从原请求起点算。不新增 min TTL。
- 内存去重代码限定真实 `a.bi0.run()` 同线程、同窗口、同 ActivityManager 和 `fi0.k` 对象的连续两次查询。使用 framework Parcel 方法完整复制 ROM 字段，复制失败执行第二次原查询。原第二个对象分配保留。
- **本机 PLQ110 为高通 `sun`，上述内存分支只用于 `mt[0-9]{2,}` 平台，因此正式配置不安装这两个无收益的内存 Hook。** 代码及隔离验证保留，不能把显式 fixture 命中当作本机真实收益。

作用域只包含 `com.omarea.vtools`。没有 system_server/SystemUI scope、网络权限、服务、唤醒锁或周期线程。诊断只在 adb 显式请求时执行。

入口继承 `io.github.libxposed.api.XposedModule`；元数据在 `META-INF/xposed`，`autoHotReload=true`。热重载移除旧 receiver/hooks，复用 bootstrap ReentrantLock 防止两代调用交叠失锁；诊断 probe 或旧调用未结束时拒绝切换，不强行丢任务。

## 构建

```powershell
python build.py --java-home 'C:\Program Files\Microsoft\jdk-17.0.20.101-hotspot'
python tests/run.py
python tests/run_thread_guard.py
python tests/run_mini.py
python tests/run_refresh_persistence.py
```

使用本地 SDK 36、JDK 17 和 `io.github.libxposed:api:102.0.0`。该依赖仅用于编译（等同 Gradle 的 `compileOnly("io.github.libxposed:api:102.0.0")`），不打包 API 实现。`dist/build-report.json` 记录输入、APK、证书和元数据摘要。构建脚本不会连接手机。

`tests/run_refresh_persistence.py` 可在Linux Actions直接运行，使用JDK17+，编译堆上限128MiB、测试堆64MiB；只编译生产 `RefreshRatePersistence` 与固定DEX行为fixture。它不构建APK、不访问ADB，不等于Android ART构造器Hook或重启恢复已验收。

## 单次诊断与回退

```powershell
python device.py stats
python device.py selftest
python device.py configure --battery off --monitor off --memory off --thread-window off --mini-render off
python device.py configure --battery on --monitor on --memory off --thread-window on --mini-render on
```

诊断 action 为 `cn.xiaocaiye.scenesamplingopt.DIAGNOSTIC`。发送者必须具有 `android.permission.DUMP`，并匹配 Scene 私有文件中的随机 token；工具不会打印 token。未授权调用拒绝执行。`probe_battery`/`probe_monitor` 为显式的一次性业务验证，不用于后台自检或负载统计。

五个开关仅写新增的 `scene_sampling_opt_settings`，不修改 Scene 的 global/monitor/powercfg、历史记录、浮窗位置或 profile。关闭开关恢复未来调用的原逻辑；已清理的隐藏任务不会被主动复活。Mini首次加载默认观察，当前手机验收后显式开启并持久保存；观察OFF仍有检查/缓存和Hook成本，不能用ON/OFF差值冒充相对旧构件的全部净收益。卸载后要在 Scene 进程重建时彻底移除已加载代码。

`threadWindow`诊断报告窗口、任务active状态、主线程读取标记、遗留任务取消和真实get-threads/详情RPC计数。`thread_ui_open/state/detail/back/close`只供显式验收：在无原线程窗口时调用原打开入口，以真实View点击进入详情、返回或关闭；不自动运行，不新增定时器，不拖动或写入布局。测试窗口存在时拒绝热重载，应先执行`thread_ui_close`。这些命令与其他诊断共享DUMP权限及私有token校验。

`refreshPersistence` 报告新构造器门槛修复次数、选择回调、真实原文件写入成功/失败/缺失和最后错误；`physicalDisplayVerified=false` 明确不是显示读回结论。新功能没有新增开关/偏好或驻留任务。热重载在原保存回调执行期间拒绝切换，完成后才移除旧hooks；刷新管理开关和原规则文件仍归Scene所有。已构建的callback的final门槛在其对象寿命内保留，卸载后需要关闭并重新打开快捷面板或重建Scene进程才恢复新构造器的原始行为。重启后新对象会重新走精确APK guard和同一构造器Hook。

## 证据边界

host合同直接编译生产SerialGate/RenderScope；线程窗口Hook合同直接编译生产ThreadWindowGuard，在最小Android/原DEX行为fixture中验证关闭/重新打开、owner隔离、resume异常与清理竞态。这些不代表Android Hook生效，需另核真实调用、窗口交互、统计字段、数据库记录及Scene/daemon状态。CPU百分比以单核为分母，不能换算成电池耗电比例；没有因果证据时不承诺节电百分比。

Mini独立合同直接编译生产MiniRenderDeduplicator，覆盖原可变数组/实际队列、文字副作用、样式变化、失败和TLS隔离。`miniRender`计数按真实scope内提交观察；`texts[].currentText`是最近一次提交前读取值，不是即时原子UI快照。需用实际提交省略、继续变化的字段、RPC频率、视觉检查及旧版/新版CPU比较评估收益；高命中率本身不是净节电证明。

原采样去重设计及验收见`module-audits/scene-power-design-20261116`、`module-audits/scene-sampling-deploy-20261116`；线程浮窗定位与修复见`module-audits/scene-thread-optimization-20261116`、`module-audits/scene-thread-fix-20261116`；Mini同值提交见`module-audits/scene-mini-render-20261116`。源包排除设备私有记录、token和构建/签名文件。
