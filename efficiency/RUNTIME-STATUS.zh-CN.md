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

`scene_native/prepare_scene.py` 对原始 startup、tools、service、post-fs-data 四份文件逐一锁定哈希，生成最小适配。辅助脚本只有在机型 PLQ110、内核 notes 哈希位于本机允许列表、厂商 HybridSwap 驱动和节点均不存在时启用；旧内核继续采用原保护逻辑。

回写使用 root 私有目录 `/data/adb/ace6-kernel-efficiency` 的 8 GiB 独立文件，权限 0600、ksu_file 标签；不格式化回写文件或操作厂商 crypto 映射。保留 Scene 配置文件，尊重显式 `zram_writeback=false`。运行中重配须满足可用 RAM 超过已用交换空间 2 GiB，失败时保留或恢复普通 ZRAM。

`startup.sh --check-only` 只读检查；`--native-writeback-only` 只配置 ZRAM，避免重复执行 Scene 的其他系统调优。重复调用已验证跳过 swapoff/reset。原脚本备份在设备私有状态目录及主机审计目录。

Kcompressd 首次开机发现原生回写初始化过晚：系统默认 4 GiB ZRAM 在应用启动后已经繁忙，原有 RAM 余量保护正确拒绝 swapoff。现通过 post-fs-data 在应用启动前初始化已验证的原生配置；只复用已有的 8 GiB 回写文件，默认交换空间已用超过 64 MiB 则跳过早期初始化，保留后续服务及 RAM 余量保护。另修复两处开机兼容问题：losetup 返回空闲编号后，最多等待 3 秒让 ueventd 创建块节点；恢复路径明确使用 /system/bin 的 mkswap/swapon，避免 BusyBox standalone 缺少 -p 参数。没有降低 swapoff 的 RAM 保护门槛。

修订后的实际重启已经确认早期初始化和后续 Scene 服务均完成，zram0 为 16 GiB、[lz4kd]、优先级 0，绑定私有 loop 回写设备，自动回写控制器自启。生成载荷十个文件的安装哈希全部一致，原有 swap_config.conf 哈希保持不变。

现已增加模块二的自动回写服务，配置独立保存于私有状态目录的 `native-writeback.conf`，不改原有 swap_config.conf。事件监听采用与当前 AppOpt 相同的 brightness + inotify；本机 actual_brightness 即使亮屏也返回 0，禁止用它判断熄屏。0–1 的亮度视为熄屏/AOD，当前正常亮屏实测为 8。

策略为熄屏标记冷页、连续等待 300 秒后回写，每次最多 128 MiB，每天最多 2 GiB，轮次间隔至少 900 秒；压缩存储实际占用须至少 1 GiB、电池温度不高于 40 度。保留原有 8 GiB 启动期总写回预算，不在每次触发时补满。nice=19、CPU0–5、I/O idle；等待采用 inotify/PSI 和不唤醒设备的 BOOTTIME 定时器，无空闲周期扫描。亮屏会取消等待，达到预算后回到事件等待。

基线实机校准包含一次阴性和阳性验证：修订程序完成回写前，检测器拒绝以旧日志/旧计数判定成功；随后由真实亮屏→熄屏事件触发，在一次亮屏取消后重新等待 300 秒，自动写出 32,768 页，计数从 32,896 增至 65,664。单轮日志耗时含写盘共 304 秒；完成后预算、内核标识、当前进程、数据 I/O 错误计数及回写增量的检查全部通过。再做 4 MiB 自有页面写回与读回校验通过，失败读写计数为 0。

Kcompressd 修订开机流程后，又从本次启动的独立计数 64 开始验证：旧启动日志的阴性对照返回未通过；熄屏自动等待 300 秒后写出 32,768 页，物理写计数增至 32,832，含实际写盘耗时 305 秒。本次启动、当前自启进程、预算和数据 I/O 的全部检查通过。随后再做 4 MiB 自有数据回写读回校验通过，失败读写计数仍为 0。自动服务的开机自启和完整工作轮次均已有实机证据。

`scene_native/build_scene.py` 可从原始哈希锁定的 Scene 文件重建载荷和本地 native 控制器，输出逐文件 SHA256；`verify_auto_writeback.py` 验证一个有独立计数基线的自动回写轮次，避免把仅手动测试通过或残留日志当成自动策略已经生效。

## Kcompressd 独立版本实机状态

已按用户授权刷入 [45236244009](https://github.com/jiankangw59-blip/Build_Oneplus_Realme_Action/actions/runs/45236244009) 到 boot_b 并重启。boot_a 保持原值；6.6.89 基线 boot 已保留用于回退。当前 boot SHA256 为 `f10ed860535ae9be2fb57a0f21319dbebf448ee6c8bb12c6d67ba39cfe2ed3db`，运行中 notes SHA256 为 `5a22f9e54d430c52b89cfe0b2ff383effa1ec379771592b11a61d43d199ada98`。

当前启动的五项底线检查全部通过：原厂关键模块、RCU Lazy 与八核 NOCB、WQ mask=3f、原生 LZ4KD 回写、官方内建 ReKernel。早期日志捕获到本次启动的 `Re-Kernel hooked!`。4 MiB 自有数据测试写回和读回均校验通过，物理读写计数各增加 64，ZRAM 失败读写计数为 0。

Kcompressd 队列深度为 24，kcompressd0 为内核线程、nice=19、CPU0–5。40 秒有限测试临时将 watermark_scale_factor 从 99 提到 600，请求 kswapd 做后台回收；设备端退出恢复及超时恢复均已设置，实际测试结束读回恢复为 99。独立 trace 实例保留下至少 9,200 条由 kcompressd0 提交到 zram0 的写入事件，说明工作线程确实执行了换出任务。trace 缓冲区存在覆盖，不能把保留条数当作总写入量或完整性能样本。

测试进程只分配 64 MiB，设置 MADV_COLD，未使用 MADV_PAGEOUT 或填满 RAM；64 MiB 数据完整性校验通过，但本次测试进程自身没有被换出。因此 `kcompressd_probe.py` 的严格“自有页交换槽关联”判定仍返回未通过，不能把这次读回称为经 Kcompressd 换出后的已知数据校验。基线阴性对照在缺少功能时返回未通过且不启动负载或跟踪；当前自有页关联与性能受控对比仍待完成。私有跟踪实例已清理，原有全局 trace 状态保持不变。

## le9uo 独立版本实机状态

已按用户授权切换到 [45234533599](https://github.com/jiankangw59-blip/Build_Oneplus_Realme_Action/actions/runs/45234533599) 并重启；Kcompressd boot 已另存用于回退。当前 boot_b SHA256 为 `7b8002be51e53302a83a27320bc5a95964718d54f5a91a6cd7247d8ecaa33431`，notes 为 `17729afbb564bd411610b7240a4425f8e452630434b587a30acf1c5c29b42916`。

本次启动五项底线检查通过。`workingset_protection=1`，匿名硬下限、干净缓存软下限、干净缓存硬下限为 0/5/0；同值写入和越界拒绝验证通过，配置保持原值。干净文件页快照按 zoneinfo 的 `nr_dirty` 扣除脏页；本次 5% 软下限约为 755.76 MiB，干净文件页远高于该值，因此未证明保护分支已触发或 refault/性能改善。

本次自启控制器经熄屏 300 秒后自动写出 128 MiB，独立写计数从 64 增至 32,832，含 I/O 共 304 秒，同启动/当前进程/预算/读写错误检查全部通过。前后各一次 4 MiB 自有页面回写读回通过。AppOpt、qcom-cpu-lpm 和回收水位 99 保持原值，未增加全局内存压力或启动功耗采样。早期有 12 条 WARNING，其位置类别与已保存的两个 Kcompressd 启动快照相同；这不等于整机日志零警告，也不能据此认定为 le9uo 新增问题。

## Crystal / ZMS / LZ4KDS 移植目标

用户已明确将 ZRAM 最终目标改为 Crystal HybridSwap + ZMS + LZ4KDS。纯 LZ4KD 候选保留作对照及回退。固定公开源码提交为 `13d291aef3d1d9607ad3fa5ff30dc23b2888a3da`；用户提供的三 KO 反编译包用于核对接口、结构和数据路径。其 vermagic 为 6.6.118，不能直接当作本机 6.6.89 兼容模块；公开源和 KO 是不同变体，尚无证据确定整体版本先后。

已增加纯 LZ4KD Crystal 独立构建，保留原有 6.6.89、Clang 18、O2、ThinLTO、Kcompressd、原厂 CRC 和 BTF 布局检查。未导入硬件覆盖、模块替换或私有分配器。充电门控改用公开 power-supply API，周期 GC 采用 deferrable timer，保留必要写入与整理；它仍可能在 CPU 因其他事件活跃时周期执行，不能称为无轮询。

LZ4KDS 独立源码实验已完成宿主编译与 ARM64 交叉编译，普通页、完整 8 KiB 差分窗口、压缩流差分恢复、损坏输入、保护页及分配失败测试通过。详细范围见 `lz4kds_lab/README.zh-CN.md`；这尚未执行手机内核中的 SDDC 引用管理、异步转换或 ZMS 并发路径，也没有证明已解决错页/零页问题。Crystal 的真实 packed 落盘/冷读、LZ4KDS 独立设备验收和系统主设备启用仍待完成。

## 待完成

- 用户选择先做日常体感测试，功耗采样暂缓。一次性采样进程已在冷却阶段终止，没有正式测量数据。新旧内核均无有效功耗基线，不作省电幅度结论；后续若恢复采样，应补测上一版 6.6.89，并采用同条件的旧版/新版对比。采样器不会自动重启。
- Kcompressd 与 le9uo 独立候选均已完成构建、32,921 项 CRC、六组 BTF 布局、boot 打包读回和上述实机验收；两者的性能受控对比仍待完成。当前手机处于 le9uo 版本。
- Kcompressd 候选明确 nice=19、CPU0–5，保留异步与原生 zswap 的 writeback 配对语义。
- Kcompressd 首次候选构建在编译预检中失败：6.6.89 缺少上游候选依赖的 MTHP_STAT_ZSWPOUT、mem_cgroup_zswap_writeback_enabled 和 zswap_is_enabled。后续适配沿用已验收基线的 CONFIG_ZSWAP=n，只允许原生同步 ZRAM 卸载，不引入虚构的新版接口替身。保留本版本的原生 writeback 配对语义，修订版已完成编译及 KMI 检查。
- le9uo 候选将三个新标志放在 scan_control 的既有 byte45 填充中；额外 BTF 检查禁止移动原字段。先使用 0/5/0 的匿名硬下限、干净缓存软下限、干净缓存硬下限比例，避免上游默认两个 15% 硬下限提前触发 OOM。其保护范围为干净文件缓存，不能称为只保护可执行文件。

当前功能验收通过不代表待机省电幅度或整套优化已经定版。
