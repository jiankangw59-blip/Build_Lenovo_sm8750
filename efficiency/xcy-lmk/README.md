# Ace 6 小菜叶选杀内核试验补丁

基线为 OnePlusOSS `android_kernel_common_oneplus_sm8750` 的
`65b954a773b183a9999b06bb1b2f921f12eaaf11`，Linux 6.6.118。
`../patches/xcy-lmk-policy-6.6.118.patch` 添加默认关闭的
`CONFIG_XCY_LMK_POLICY`。启用后提供 root 专用的 `/dev/xcy_lmk`。
构建工作流提供 `enable_xcy_lmk_policy` 开关，并在制品中记录配置和符号。

- `XCY_LMK_RANK` 接收通过 Android 业务过滤的整包 PSS、退后台时长和最近使用标志。沿用当前 `pressure_reclaim` 路径的 60% 大内存池、60 秒近似年龄窗口，以及紧急压力下的 90% 分数窗口。紧急分数使用 Q12 定点运算，保留原浮点实现作为对照。
- `XCY_LMK_READ_TASKS` 按 PID、UID 和 `/proc/PID/stat` 的启动时刻检查身份，一次返回最多 512 个进程的 RSS、交换页和 `oom_score_adj`。这些计数适合做低开销预筛，整包 PSS 继续采用 Android 的统计口径。
- Scene、AMS、音频和包级白名单提供候选资格。原守护进程继续承担内存压力确认、终止前重新校验和 pidfd 信号。接口按请求运行，可用于和现有结果对照。

## 回放验证

在已应用补丁的内核树以外运行以下命令。设置 `kernel` 为内核树路径，`native` 为小菜叶 opt13 源码的 `src` 目录。

```sh
test_include=$(mktemp -d)
install -D "$kernel/include/uapi/linux/xcy_lmk.h" "$test_include/linux/xcy_lmk.h"
cc -std=c11 -Wall -Wextra -Werror -O2 -fsanitize=undefined \
   -I"$test_include" -I"$kernel/mm" -I"$native" \
   efficiency/xcy-lmk/test_policy.c -o /tmp/test-xcy-lmk
/tmp/test-xcy-lmk
```
