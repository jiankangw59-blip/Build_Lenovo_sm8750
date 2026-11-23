/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Readable reconstruction of the supplied KO's codec wrapper contracts.
 * The compressor itself is separately pinned, licensed upstream source.
 * This is a host laboratory artifact, not a loadable Android driver.
 */
#include "restored_codec.h"
#include "vendor/lz4kd/lz4kd.h"

struct restored_lz4kds_codec {
    void *encode_state;
    unsigned char *delta_window;
};
_Static_assert(sizeof(struct restored_lz4kds_codec) == 16, "KO DWARF codec size");
_Static_assert(offsetof(struct restored_lz4kds_codec, delta_window) == 8, "KO field offset");

struct restored_lz4kds_codec *restored_lz4kds_create(void)
{
    struct restored_lz4kds_codec *codec = lab_zalloc(sizeof(*codec));
    if (!codec) return ERR_PTR(-ENOMEM);
    codec->encode_state = lab_zalloc(crystal_lz4kd_encode_state_bytes_min());
    codec->delta_window = lab_zalloc(8192);
    if (!codec->encode_state || !codec->delta_window) {
        restored_lz4kds_destroy(codec);
        return ERR_PTR(-ENOMEM);
    }
    return codec;
}

void restored_lz4kds_destroy(struct restored_lz4kds_codec *codec)
{
    if (!codec) return;
    lab_free(codec->delta_window);
    lab_free(codec->encode_state);
    lab_free(codec);
}

int restored_lz4kds_compress(struct restored_lz4kds_codec *codec,
                           const void *src, unsigned len, void *dst, unsigned *capacity)
{
    if (!codec || !src || !dst || !capacity || !*capacity || len != 4096) return -EINVAL;
    int ret = crystal_lz4kd_encode(codec->encode_state, src, dst, len, *capacity, 0);
    if (ret < 0) return -EINVAL;
    /* Zero means incompressible. The caller must preserve the raw page. */
    if (ret > 0) *capacity = ret;
    return 0;
}

int restored_lz4kds_decompress(struct restored_lz4kds_codec *codec,
                             const void *src, unsigned len, void *dst, unsigned *capacity)
{
    if (!codec || !src || !len || len > 4096 || !dst || !capacity || *capacity != 4096)
        return -EINVAL;
    int ret = crystal_lz4kd_decode(src, dst, len, *capacity);
    if (ret != 4096) return -EINVAL;
    *capacity = ret;
    return 0;
}

int restored_lz4kds_compress_delta(struct restored_lz4kds_codec *codec,
                                 const void *ref, unsigned ref_len, const void *src, unsigned len,
                                 void *dst, unsigned *capacity, unsigned limit)
{
    if (!codec || !ref || !ref_len || ref_len > 4096 || !src || len < 16 || len > 4096 ||
        !dst || !capacity || !*capacity) return -EINVAL;
    memcpy(codec->delta_window, ref, ref_len);
    unsigned char *current = codec->delta_window + ref_len;
    memcpy(current, src, len);
    int ret = crystal_lz4kd_encode_delta(codec->encode_state, codec->delta_window, current,
                                       dst, len, *capacity, limit);
    if (ret < 0) return -EINVAL;
    if (ret > 0) *capacity = ret;
    return 0;
}

int restored_lz4kds_decompress_delta_borrowed(struct restored_lz4kds_codec *codec,
                                            const void *src, unsigned len, const void *ref,
                                            unsigned ref_len, const void **restored, unsigned *capacity)
{
    if (!restored || !capacity) return -EINVAL;
    *restored = NULL;
    /* KO accepts any 1..4096 output ceiling; retained source requires 4096. */
    if (!codec || !src || !len || len > 4096 || !ref || !ref_len || ref_len > 4096 ||
        !*capacity || *capacity > 4096) return -EINVAL;
    memcpy(codec->delta_window, ref, ref_len);
    unsigned char *current = codec->delta_window + ref_len;
    int ret = crystal_lz4kd_decode_delta(src, codec->delta_window, current, len, *capacity);
    if (ret <= 0 || (unsigned)ret > *capacity) return -EINVAL;
    *restored = current;
    *capacity = ret;
    return 0;
}

int restored_lz4kds_decompress_delta(struct restored_lz4kds_codec *codec,
                                   const void *src, unsigned len, const void *ref, unsigned ref_len,
                                   void *dst, unsigned *capacity)
{
    const void *restored;
    if (!dst || !capacity) return -EINVAL;
    int ret = restored_lz4kds_decompress_delta_borrowed(codec, src, len, ref, ref_len,
                                                     &restored, capacity);
    if (ret) return ret;
    memcpy(dst, restored, *capacity);
    return 0;
}
