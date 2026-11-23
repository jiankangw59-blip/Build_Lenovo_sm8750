/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Host-only allocation/type shim. Never installed in a kernel source tree. */
#ifndef LZ4KDS_LAB_COMPAT_H
#define LZ4KDS_LAB_COMPAT_H
#include <stddef.h>
typedef __UINT8_TYPE__ uint8_t;
typedef __UINT16_TYPE__ uint16_t;
typedef __UINT32_TYPE__ uint32_t;
typedef __UINT64_TYPE__ uint64_t;
typedef __INT64_TYPE__ int64_t;
typedef __UINTPTR_TYPE__ uintptr_t;
typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
typedef _Bool bool;
#define true 1
#define false 0
#define SZ_4K 4096U
#define PAGE_SIZE SZ_4K
#define GFP_KERNEL 0
#define EINVAL 22
#define ENOMEM 12
#define EOPNOTSUPP 95
#define ERR_PTR(value) ((void *)(__INTPTR_TYPE__)(value))
#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)
#define check_add_overflow(a, b, result) __builtin_add_overflow(a, b, result)
void *memcpy(void *, const void *, size_t);
void *memset(void *, int, size_t);
void *lab_zalloc(size_t);
void lab_free(void *);
#define kzalloc(size, flags) lab_zalloc(size)
#define kvzalloc(size, flags) lab_zalloc(size)
#define kfree(ptr) lab_free(ptr)
#define kvfree(ptr) lab_free(ptr)
#endif
