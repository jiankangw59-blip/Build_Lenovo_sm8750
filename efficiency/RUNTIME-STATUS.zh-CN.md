# Ace 6 / 6.6.89 实机验收记录

基线构建：[45218661660](https://github.com/jiankangw59-blip/Build_Oneplus_Realme_Action/actions/runs/45218661660)。
内核源提交为 `c1802dd6f3343e71c166a3c67c50554c81880b6e`，保留 BakaSU、SUSFS、FUSE 重验证及原厂模块。

已完成实机刷入、重启和五项底线验收：

- 32,921 项原厂导入 CRC 检查通过；重启后原厂模块正常加载，旧 ReKernel 加载模块已备份并停用。
- RCU Lazy 为 Y，存在 rcuop/0–7；节能队列为 Y、全局 mask=3f，观察到的无绑定 worker 均位于 CPU0–5。
- AppOpt 原配置及八个线程的亲和性保持原值。
- ReKernel 为官方源码内建，Generic Netlink 返回 11.7，无重复加载的 rekernel.ko。此传输模式不创建 /proc/rekernel。
- 再次实机查询到 family=28、events 组=7；system_server 已订阅该组，套接字丢包计数为 0。驱动、协议查询和客户端订阅均有证据，实际冻结后唤醒场景尚未专项验收。
- 原生 zram0 为 16 GiB、纯 lz4kd。对测试进程自身的 64 个交换页面逐一回写，物理读写计数各增加 64，完整校验 4 MiB 数据一致。

额外测试：独立 ZRAM 两轮各 32 MiB 混合数据回写后读回一致；BBRv3 的 32 组本地连接数据校验通过；独立 F2FS 镜像压缩、重挂载读回正常，GC 线程为 SCHED_IDLE、I/O idle。现有 /data 文件系统特性未变。TEO/Menu 补丁已编入，但本机当前实际 governor 为 qcom-cpu-lpm。

## Scene 原生回写适配

`scene_native/prepare_scene.py` 只接受已审查的两份 Scene 源文件哈希，生成最小适配。辅助脚本只有在机型 PLQ110、内核 notes 哈希位于本机允许列表、厂商 HybridSwap 驱动和节点均不存在时启用；旧内核继续采用原保护逻辑。

回写使用 root 私有目录 `/data/adb/ace6-kernel-efficiency` 的 8 GiB 独立文件，权限 0600、ksu_file 标签；不格式化回写文件或操作厂商 crypto 映射。保留 Scene 配置文件，尊重显式 `zram_writeback=false`。运行中重配须满足可用 RAM 超过已用交换空间 2 GiB，失败时保留或恢复普通 ZRAM。

`startup.sh --check-only` 只读检查；`--native-writeback-only` 只配置 ZRAM，避免重复执行 Scene 的其他系统调优。重复调用已验证跳过 swapoff/reset。原脚本备份在设备私有状态目录及主机审计目录。该适配的本次验证为运行中配置，后续重启仍需读回确认。

现已增加模块二的自动回写服务，配置独立保存于私有状态目录的 `native-writeback.conf`，不改原有 swap_config.conf。事件监听采用与当前 AppOpt 相同的 brightness + inotify；本机 actual_brightness 即使亮屏也返回 0，禁止用它判断熄屏。0–1 的亮度视为熄屏/AOD，当前正常亮屏实测为 8。

策略为熄屏标记冷页、连续等待 300 秒后回写，每次最多 128 MiB，每天最多 2 GiB，轮次间隔至少 900 秒；压缩存储实际占用须至少 1 GiB、电池温度不高于 40 度。保留原有 8 GiB 启动期总写回预算，不在每次触发时补满。nice=19、CPU0–5、I/O idle；等待采用 inotify/PSI 和不唤醒设备的 BOOTTIME 定时器，无空闲周期扫描。亮屏会取消等待，达到预算后回到事件等待。

实机校准包含一次阴性和阳性验证：修订程序完成回写前，检测器拒绝以旧日志/旧计数判定成功；随后由真实亮屏→熄屏事件触发，在一次亮屏取消后重新等待 300 秒，自动写出 32,768 页，计数从 32,896 增至 65,664。单轮日志耗时含写盘共 304 秒；完成后预算、内核标识、当前进程、数据 I/O 错误计数及回写增量的检查全部通过。再做 4 MiB 自有页面写回与读回校验通过，失败读写计数为 0。当前脚本已运行；新自动服务的开机自启入口已接入，但尚未重启验收。

`scene_native/build_scene.py` 可从原始哈希锁定的 Scene 文件重建载荷和本地 native 控制器，输出逐文件 SHA256；`verify_auto_writeback.py` 验证一个有独立计数基线的自动回写轮次，避免把仅手动测试通过或残留日志当成自动策略已经生效。

## 待完成

- 用户选择先做日常体感测试，功耗采样暂缓。一次性采样进程已在冷却阶段终止，没有正式测量数据。新旧内核均无有效功耗基线，不作省电幅度结论；后续若恢复采样，应补测上一版 6.6.89，并采用同条件的旧版/新版对比。采样器不会自动重启。
- Kcompressd 与 le9uo 独立候选均已构建成功，分别为 [45236244009](https://github.com/jiankangw59-blip/Build_Oneplus_Realme_Action/actions/runs/45236244009) 和 [45234533599](https://github.com/jiankangw59-blip/Build_Oneplus_Realme_Action/actions/runs/45234533599)。两者均完成主机复核：32,921 项 CRC、六组 BTF 布局及候选 boot 打包读回均通过。均未刷入，运行时完整性和受控对比仍待完成。
- Kcompressd 候选明确 nice=19、CPU0–5，保留异步与原生 zswap 的 writeback 配对语义。
- Kcompressd 首次候选构建在编译预检中失败：6.6.89 缺少上游候选依赖的 MTHP_STAT_ZSWPOUT、mem_cgroup_zswap_writeback_enabled 和 zswap_is_enabled。后续适配沿用已验收基线的 CONFIG_ZSWAP=n，只允许原生同步 ZRAM 卸载，不引入虚构的新版接口替身。保留本版本的原生 writeback 配对语义，修订版已完成编译及 KMI 检查。
- le9uo 候选将三个新标志放在 scan_control 的既有 byte45 填充中；额外 BTF 检查禁止移动原字段。先使用 0/5/0 的匿名硬下限、干净缓存软下限、干净缓存硬下限比例，避免上游默认两个 15% 硬下限提前触发 OOM。其保护范围为干净文件缓存，不能称为只保护可执行文件。

当前功能验收通过不代表待机省电幅度或整套优化已经定版。
