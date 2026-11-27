/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef RESTORED_LZ4KDS_CODEC_H
#define RESTORED_LZ4KDS_CODEC_H
#include "shim/lab_compat.h"
struct restored_lz4kds_codec;
struct restored_lz4kds_codec *restored_lz4kds_create(void);
void restored_lz4kds_destroy(struct restored_lz4kds_codec *);
int restored_lz4kds_compress(struct restored_lz4kds_codec *, const void *, unsigned, void *, unsigned *);
int restored_lz4kds_decompress(struct restored_lz4kds_codec *, const void *, unsigned, void *, unsigned *);
int restored_lz4kds_compress_delta(struct restored_lz4kds_codec *, const void *, unsigned,
                                 const void *, unsigned, void *, unsigned *, unsigned);
int restored_lz4kds_decompress_delta(struct restored_lz4kds_codec *, const void *, unsigned,
                                   const void *, unsigned, void *, unsigned *);
int restored_lz4kds_decompress_delta_borrowed(struct restored_lz4kds_codec *, const void *, unsigned,
                                            const void *, unsigned, const void **, unsigned *);
#endif
