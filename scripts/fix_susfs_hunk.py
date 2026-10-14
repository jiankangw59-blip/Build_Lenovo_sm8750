#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
补上 50_add_susfs_in_gki-android15-6.6.patch 在 fs/proc/task_mmu.c 缺失的 Hunk #6。

缺失的 hunk 作用 (原始补丁内容):
    @@ -999,8 +1056,15 @@ static int show_smaps_rollup(...)
    			/* Case 4 above */
    -			if (vma->vm_end > last_vma_end)
    +			if (vma->vm_end > last_vma_end) {
    +#ifdef CONFIG_KSU_SUSFS_SUS_MAP
    +				if (!vma->vm_file || !(SUSFS_IS_INODE_SUS_MAP(file_inode(vma->vm_file)))) {
    +					smap_gather_stats(vma, &mss, last_vma_end);
    +				}
    +#else
    				smap_gather_stats(vma, &mss, last_vma_end);
    +#endif // #ifdef CONFIG_KSU_SUSFS_SUS_MAP
    +			}

作用: 把被 sus_map 隐藏的内存区域, 从 /proc/pid/smaps_rollup 的汇总统计里剔除。
缺失后果: maps / smaps 路径的规避仍在, 但 smaps_rollup 汇总数字会对不上, 有被检测风险。

本脚本只在「缺失且能明确定位」时插入, 且插入后立即校验; 定位不到就明确报错而不是静默跳过。
用法: python3 fix_susfs_hunk.py [<common 目录>]
"""
import os, re, sys

COMMON = sys.argv[1] if len(sys.argv) > 1 else '.'
P = os.path.join(COMMON, 'fs', 'proc', 'task_mmu.c')

MARK = 'SUSFS_IS_INODE_SUS_MAP(file_inode(vma->vm_file))'

if not os.path.isfile(P):
    print('::error::找不到 %s' % P)
    sys.exit(1)

src = open(P, encoding='utf-8', errors='surrogateescape').read()

# ---- 已存在就退出 ----
if MARK in src:
    print('[OK] sus_map 的 smaps_rollup 规避已存在, 无需补')
    sys.exit(0)

# ---- 定位 show_smaps_rollup 函数体 ----
m = re.search(r'static int show_smaps_rollup\s*\([^)]*\)\s*\{', src)
if not m:
    print('::error::找不到 show_smaps_rollup() 定义')
    sys.exit(1)

# 取该函数到下一个顶层 "}" 之间的范围 (粗略但要够用)
start = m.end()
depth = 1
i = start
while i < len(src) and depth > 0:
    c = src[i]
    if c == '{':
        depth += 1
    elif c == '}':
        depth -= 1
    i += 1
body = src[start:i]
if depth != 0:
    print('::error::show_smaps_rollup() 大括号不配对, 放弃')
    sys.exit(1)

# ---- 在函数体内找目标片段 ----
# 关键特征: "/* Case 4 above */" 后面的 if (vma->vm_end > last_vma_end)
pat = re.compile(
    r'(/\* Case 4 above \*/\s*\n)'            # 1: 注释行(含缩进与换行)
    r'([ \t]*)'                                # 2: 缩进
    r'if \(vma->vm_end > last_vma_end\)\s*\n'  # 目标 if
    r'[ \t]*smap_gather_stats\(vma, &mss, last_vma_end\);'  # 目标语句
)

mm = pat.search(body)
if not mm:
    print('::error::在 show_smaps_rollup() 里找不到 "/* Case 4 above */" + '
          '"if (vma->vm_end > last_vma_end)" + "smap_gather_stats(...)" 组合')
    print('         可能内核源码结构不同, 请人工核对 (不会静默跳过)')
    sys.exit(1)

ind = mm.group(2)          # 基准缩进
inner = ind + '\t'         # 内层再缩进一个 tab

replacement = (
    mm.group(1)
    + ind + 'if (vma->vm_end > last_vma_end) {\n'
    + ind + '#ifdef CONFIG_KSU_SUSFS_SUS_MAP\n'
    + inner + 'if (!vma->vm_file || !(SUSFS_IS_INODE_SUS_MAP(file_inode(vma->vm_file)))) {\n'
    + inner + '\tsmap_gather_stats(vma, &mss, last_vma_end);\n'
    + inner + '}\n'
    + ind + '#else\n'
    + inner + 'smap_gather_stats(vma, &mss, last_vma_end);\n'
    + ind + '#endif // #ifdef CONFIG_KSU_SUSFS_SUS_MAP\n'
    + ind + '}'
)

new_body = body[:mm.start()] + replacement + body[mm.end():]
out = src[:start] + new_body + src[i:]

# ---- 校验 ----
if MARK not in out:
    print('::error::插入后仍未包含目标标记, 放弃写盘')
    sys.exit(1)
if out.count('SUSFS_IS_INODE_SUS_MAP(file_inode(vma->vm_file))') != 1:
    print('::error::插入后出现 %d 次, 不是 1 次, 放弃写盘'
          % out.count('SUSFS_IS_INODE_SUS_MAP(file_inode(vma->vm_file))'))
    sys.exit(1)

open(P, 'w', encoding='utf-8', errors='surrogateescape', newline='').write(out)
print('[OK] 已补上 sus_map 的 smaps_rollup 规避 (原补丁 Hunk #6)')
print('     文件: %s' % P)
