#ifndef ISH_ACCEL_PIX_H
#define ISH_ACCEL_PIX_H

#include <stdbool.h>
#include <stdint.h>

// Host-native pixel kernels for the pixman accelerator (ISH_SYS_PIXOP,
// kernel/ish_accel_pix.c). Operate on DIRECT host pointers already resolved
// by kernel/user.c's user_transform_rect/user_transform_rect_two -- these
// functions never touch guest memory or locks themselves. Every kernel here
// must be bit-exact against the corresponding real pixman operation on
// a8r8g8b8 pixel data (validated by tests/manual/pixman_accel.c's
// differential harness); any op that can't be made bit-exact must be
// declined by the caller (kernel/ish_accel_pix.c's dispatch), never
// approximated here.

// Fill `pixels` consecutive a8r8g8b8 pixels at `dst` with `value` (plain
// overwrite, matching pixman_fill's actual semantics -- NOT a real XOR
// despite the historical `_xor` parameter name in pixman's own API,
// verified empirically against real pixman before this was written).
void ish_pix_fill_row(void *dst, uint32_t pixels, uint32_t value);

// Copy `pixels` consecutive a8r8g8b8/x8r8g8b8 pixels from `src` to `dst`
// (PIXMAN_OP_SRC). Straight forward memcpy -- the caller (ish_accel_pix.c)
// has already declined any request where src and dst regions could overlap,
// so this never needs memmove-style direction handling.
void ish_pix_copy_row(const void *src, void *dst, uint32_t pixels);

// Premultiplied-alpha OVER of `pixels` consecutive a8r8g8b8 src pixels onto
// a8r8g8b8 dst pixels: out_c = src_c + mul_un8(dst_c, 255 - src_a), applied
// uniformly to all 4 byte lanes (A,R,G,B, each saturating). If
// `src_is_opaque` is true (the src surface is x8r8g8b8, whose top byte
// pixman treats as always-opaque padding rather than real alpha), src_a is
// forced to 255 for every pixel regardless of what's actually in that byte,
// which degenerates the blend to a straight copy -- handled here rather than
// by a separate kernel so both format combinations share one bit-exact
// arithmetic path.
void ish_pix_over_row(const void *src, void *dst, uint32_t pixels, bool src_is_opaque);

#endif
