// iSH pixman accelerator delivery shim: an LD_PRELOAD library interposing
// pixman's public API (pixman has no provider/plugin mechanism, unlike
// OpenSSL) to route eligible composite/fill calls through the host-native
// kernel accelerator (kernel/ish_accel_pix.c, ISH_SYS_PIXOP = 0xacc1)
// instead of running pixman's own C implementation instruction-by-
// instruction under emulation. See pixman_accel_plan.md for the full
// design; this is Phase 2.
//
// SAFETY CONTRACT: every interposed function calls straight through to the
// REAL pixman implementation (dlsym RTLD_NEXT) whenever the call doesn't
// exactly match the accelerator's supported shape (32bpp a8r8g8b8/
// x8r8g8b8 src/dst; SRC or OVER; OVER may carry an a8 mask (the glyph/
// text-rendering shape) but SRC-with-mask and any other op+mask
// combination decline (real, but not yet differential-tested); no
// transform, no non-identity filter, no repeat, no alpha map, no clip on
// ANY of dst/src/mask; in-bounds). Nothing here approximates -- only
// "accelerate bit-exactly" or "decline to real pixman" are possible
// outcomes, exactly like the kernel side. Only PUBLIC pixman accessors are
// used to inspect an image's state; the five properties pixman has no
// getter for (transform/repeat/filter/alpha-map/clip) are shadow-tracked
// by interposing their SETTERS, with the shadow entry for an image
// invalidated the moment real pixman_image_unref() reports the image was
// actually freed (verified empirically: it returns nonzero exactly on the
// unref that drops the refcount to zero, never before) -- so a later
// malloc() reusing the same address can never inherit stale shadow state.
//
// Fails closed everywhere: probing ISH_SYS_PIXOP happens once at load; on
// ENOSYS (stock iSH, real Linux, or the accelerator simply disabled) every
// interposed function becomes a pure pass-through for the rest of the
// process's life. A malloc failure while shadow-tracking an image is
// likewise treated as "assume the worst" (mark the image as not simple)
// rather than losing track of a property.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pixman.h>

extern long syscall(long, ...);
#define ISH_SYS_PIXOP 0xacc1
enum { PIX_OP_FILL = 0, PIX_OP_COPY = 1, PIX_OP_OVER = 2, PIX_OP_OVER_MASK = 3 };
enum { PIX_FLAG_SRC_OPAQUE = 1u << 0 };

struct ish_pix_req {
    uint32_t op, flags;
    uint64_t dst, src, mask;
    uint32_t dst_stride, src_stride, mask_stride;
    int32_t dst_x, dst_y;
    int32_t src_x, src_y;
    int32_t mask_x, mask_y;
    uint32_t width, height;
    uint32_t fill_pixel;
};

static long pixop(uint32_t op, uint32_t flags,
        void *dst, uint32_t dst_stride, int32_t dst_x, int32_t dst_y,
        void *src, uint32_t src_stride, int32_t src_x, int32_t src_y,
        uint32_t width, uint32_t height, uint32_t fill_pixel) {
    struct ish_pix_req r = {
        .op = op, .flags = flags,
        .dst = (uint64_t) (uintptr_t) dst, .src = (uint64_t) (uintptr_t) src,
        .dst_stride = dst_stride, .src_stride = src_stride,
        .dst_x = dst_x, .dst_y = dst_y, .src_x = src_x, .src_y = src_y,
        .width = width, .height = height, .fill_pixel = fill_pixel,
    };
    return syscall(ISH_SYS_PIXOP, &r);
}

static long pixop_mask(uint32_t flags,
        void *dst, uint32_t dst_stride, int32_t dst_x, int32_t dst_y,
        void *src, uint32_t src_stride, int32_t src_x, int32_t src_y,
        void *mask, uint32_t mask_stride, int32_t mask_x, int32_t mask_y,
        uint32_t width, uint32_t height) {
    struct ish_pix_req r = {
        .op = PIX_OP_OVER_MASK, .flags = flags,
        .dst = (uint64_t) (uintptr_t) dst, .src = (uint64_t) (uintptr_t) src, .mask = (uint64_t) (uintptr_t) mask,
        .dst_stride = dst_stride, .src_stride = src_stride, .mask_stride = mask_stride,
        .dst_x = dst_x, .dst_y = dst_y, .src_x = src_x, .src_y = src_y, .mask_x = mask_x, .mask_y = mask_y,
        .width = width, .height = height,
    };
    return syscall(ISH_SYS_PIXOP, &r);
}

// ---- accelerator availability probe -------------------------------------
static int g_accel_available = -1;
static pthread_once_t g_probe_once = PTHREAD_ONCE_INIT;

static void probe_accel(void) {
    // A width=0 FILL is a defined no-op in the kernel (returns 0 success)
    // whenever the accelerator is enabled and passed self-test, and ENOSYS
    // otherwise (disabled, or the syscall doesn't exist at all -- real
    // Linux, or stock iSH). dst_stride must still satisfy the kernel's own
    // alignment check even though width=0 short-circuits before it's used.
    long ret = pixop(PIX_OP_FILL, 0, NULL, 4, 0, 0, NULL, 0, 0, 0, 0, 0, 0);
    g_accel_available = (ret == 0) ? 1 : 0;
}

static int accel_available(void) {
    pthread_once(&g_probe_once, probe_accel);
    return g_accel_available == 1;
}

// ---- real pixman entry points, resolved lazily --------------------------
typedef void (*composite32_fn)(pixman_op_t, pixman_image_t *, pixman_image_t *, pixman_image_t *,
        int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t);
typedef pixman_bool_t (*fill_fn)(uint32_t *, int, int, int, int, int, int, uint32_t);
typedef pixman_bool_t (*unref_fn)(pixman_image_t *);
typedef pixman_bool_t (*set_transform_fn)(pixman_image_t *, const pixman_transform_t *);
typedef void (*set_repeat_fn)(pixman_image_t *, pixman_repeat_t);
typedef pixman_bool_t (*set_filter_fn)(pixman_image_t *, pixman_filter_t, const pixman_fixed_t *, int);
typedef void (*set_alpha_map_fn)(pixman_image_t *, pixman_image_t *, int16_t, int16_t);
typedef pixman_bool_t (*set_clip_region_fn)(pixman_image_t *, const pixman_region16_t *);
typedef pixman_bool_t (*set_clip_region32_fn)(pixman_image_t *, const pixman_region32_t *);
typedef void (*set_has_client_clip_fn)(pixman_image_t *, pixman_bool_t);
typedef pixman_bool_t (*get_component_alpha_fn)(pixman_image_t *);
typedef uint32_t *(*get_data_fn)(pixman_image_t *);
typedef int (*get_stride_fn)(pixman_image_t *);
typedef int (*get_width_fn)(pixman_image_t *);
typedef int (*get_height_fn)(pixman_image_t *);
typedef pixman_format_code_t (*get_format_fn)(pixman_image_t *);

#define RESOLVE(var, type, name) \
    static type var; \
    if (var == NULL) var = (type) dlsym(RTLD_NEXT, name)

// ---- per-image shadow state (properties pixman has no public getter for)
// A chained hash table keyed by the pixman_image_t pointer. An image
// absent from the table is, by construction, still at pixman's own
// defaults (no transform, REPEAT_NONE, NEAREST filter, no alpha map, no
// clip) -- entries are only ever created lazily by a setter, and removed
// the instant real pixman_image_unref() reports the image was actually
// freed, so a stale entry can never outlive the object it describes.
struct image_state {
    pixman_image_t *key;
    int has_transform;
    int has_alpha_map;
    int has_clip;
    int nontrivial_filter; // filter set to anything other than NEAREST
    pixman_repeat_t repeat;
    struct image_state *next;
};
#define STATE_BUCKETS 256
static struct image_state *g_buckets[STATE_BUCKETS];
static pthread_mutex_t g_state_lock = PTHREAD_MUTEX_INITIALIZER;

static unsigned state_hash(pixman_image_t *img) {
    uintptr_t v = (uintptr_t) img;
    return (unsigned) ((v >> 4) ^ (v >> 13)) % STATE_BUCKETS;
}

// Caller must hold g_state_lock.
static struct image_state *state_find(pixman_image_t *img) {
    struct image_state *s = g_buckets[state_hash(img)];
    while (s != NULL && s->key != img)
        s = s->next;
    return s;
}

// Caller must hold g_state_lock. Returns NULL on allocation failure --
// callers treat that as "assume not simple" (fail closed), never silently
// dropping a property that was actually set.
static struct image_state *state_get_or_create(pixman_image_t *img) {
    struct image_state *s = state_find(img);
    if (s != NULL)
        return s;
    s = calloc(1, sizeof(*s));
    if (s == NULL)
        return NULL;
    s->key = img;
    s->repeat = PIXMAN_REPEAT_NONE;
    unsigned h = state_hash(img);
    s->next = g_buckets[h];
    g_buckets[h] = s;
    return s;
}

static void state_remove(pixman_image_t *img) {
    unsigned h = state_hash(img);
    struct image_state **pp = &g_buckets[h];
    while (*pp != NULL) {
        if ((*pp)->key == img) {
            struct image_state *dead = *pp;
            *pp = dead->next;
            free(dead);
            return;
        }
        pp = &(*pp)->next;
    }
}

// ---- ISH_PIXMAN_STATS=1 decline/accelerate accounting -------------------
enum {
    STAT_ACCEL_COMPOSITE, STAT_ACCEL_FILL, STAT_ACCEL_MASK,
    STAT_DECLINE_MASK_FORMAT, STAT_DECLINE_OP, STAT_DECLINE_FORMAT,
    STAT_DECLINE_TRANSFORM, STAT_DECLINE_REPEAT, STAT_DECLINE_FILTER,
    STAT_DECLINE_ALPHA_MAP, STAT_DECLINE_CLIP, STAT_DECLINE_COMPONENT_ALPHA,
    STAT_DECLINE_BOUNDS, STAT_DECLINE_UNAVAILABLE, STAT_DECLINE_SYSCALL,
    STAT_DECLINE_FILL_BPP,
    STAT_COUNT,
};
static const char *const stat_names[STAT_COUNT] = {
    "accelerated-composite", "accelerated-fill", "accelerated-mask",
    "decline-mask-format", "decline-op", "decline-format",
    "decline-transform", "decline-repeat", "decline-filter",
    "decline-alpha-map", "decline-clip", "decline-component-alpha",
    "decline-bounds", "decline-accel-unavailable", "decline-syscall-refused",
    "decline-fill-bpp",
};
static unsigned long g_stats[STAT_COUNT];
static int stats_enabled(void) {
    static int enabled = -1;
    if (enabled == -1)
        enabled = getenv("ISH_PIXMAN_STATS") != NULL ? 1 : 0;
    return enabled;
}
static void note(int stat) {
    if (stats_enabled())
        __atomic_fetch_add(&g_stats[stat], 1, __ATOMIC_RELAXED);
}

__attribute__((destructor))
static void dump_stats(void) {
    if (!stats_enabled())
        return;
    fprintf(stderr, "[ish-pixman-shim] pid=%d\n", (int) getpid());
    for (int i = 0; i < STAT_COUNT; i++) {
        if (g_stats[i] != 0)
            fprintf(stderr, "[ish-pixman-shim]   %-28s %lu\n", stat_names[i], g_stats[i]);
    }
}

// ---- interposed state-tracking setters ----------------------------------
// Every one of these calls through to the real pixman implementation
// FIRST (so a real failure return is honored exactly as pixman intends),
// then updates the shadow table to match -- the shadow always describes
// what pixman was actually told, never what we assumed it would accept.

pixman_bool_t pixman_image_set_transform(pixman_image_t *image, const pixman_transform_t *transform) {
    RESOLVE(real, set_transform_fn, "pixman_image_set_transform");
    pixman_bool_t ret = real(image, transform);
    pthread_mutex_lock(&g_state_lock);
    struct image_state *s = state_get_or_create(image);
    if (s != NULL)
        s->has_transform = (transform != NULL);
    pthread_mutex_unlock(&g_state_lock);
    return ret;
}

void pixman_image_set_repeat(pixman_image_t *image, pixman_repeat_t repeat) {
    RESOLVE(real, set_repeat_fn, "pixman_image_set_repeat");
    real(image, repeat);
    pthread_mutex_lock(&g_state_lock);
    struct image_state *s = state_get_or_create(image);
    if (s != NULL)
        s->repeat = repeat;
    pthread_mutex_unlock(&g_state_lock);
}

pixman_bool_t pixman_image_set_filter(pixman_image_t *image, pixman_filter_t filter,
        const pixman_fixed_t *filter_params, int n_filter_params) {
    RESOLVE(real, set_filter_fn, "pixman_image_set_filter");
    pixman_bool_t ret = real(image, filter, filter_params, n_filter_params);
    pthread_mutex_lock(&g_state_lock);
    struct image_state *s = state_get_or_create(image);
    if (s != NULL)
        s->nontrivial_filter = (filter != PIXMAN_FILTER_NEAREST);
    pthread_mutex_unlock(&g_state_lock);
    return ret;
}

void pixman_image_set_alpha_map(pixman_image_t *image, pixman_image_t *alpha_map, int16_t x, int16_t y) {
    RESOLVE(real, set_alpha_map_fn, "pixman_image_set_alpha_map");
    real(image, alpha_map, x, y);
    pthread_mutex_lock(&g_state_lock);
    struct image_state *s = state_get_or_create(image);
    if (s != NULL)
        s->has_alpha_map = (alpha_map != NULL);
    pthread_mutex_unlock(&g_state_lock);
}

pixman_bool_t pixman_image_set_clip_region(pixman_image_t *image, const pixman_region16_t *region) {
    RESOLVE(real, set_clip_region_fn, "pixman_image_set_clip_region");
    pixman_bool_t ret = real(image, region);
    pthread_mutex_lock(&g_state_lock);
    struct image_state *s = state_get_or_create(image);
    if (s != NULL)
        s->has_clip = (region != NULL);
    pthread_mutex_unlock(&g_state_lock);
    return ret;
}

pixman_bool_t pixman_image_set_clip_region32(pixman_image_t *image, const pixman_region32_t *region) {
    RESOLVE(real, set_clip_region32_fn, "pixman_image_set_clip_region32");
    pixman_bool_t ret = real(image, region);
    pthread_mutex_lock(&g_state_lock);
    struct image_state *s = state_get_or_create(image);
    if (s != NULL)
        s->has_clip = (region != NULL);
    pthread_mutex_unlock(&g_state_lock);
    return ret;
}

void pixman_image_set_has_client_clip(pixman_image_t *image, pixman_bool_t client_clip) {
    RESOLVE(real, set_has_client_clip_fn, "pixman_image_set_has_client_clip");
    real(image, client_clip);
    // Conservative: client-clip means pixman defers clipping to the
    // caller, which we have no way to inspect -- treat it the same as a
    // real clip region (decline) rather than assuming it's a no-op.
    pthread_mutex_lock(&g_state_lock);
    struct image_state *s = state_get_or_create(image);
    if (s != NULL && client_clip)
        s->has_clip = 1;
    pthread_mutex_unlock(&g_state_lock);
}

pixman_bool_t pixman_image_unref(pixman_image_t *image) {
    RESOLVE(real, unref_fn, "pixman_image_unref");
    pixman_bool_t freed = real(image);
    if (freed) {
        pthread_mutex_lock(&g_state_lock);
        state_remove(image);
        pthread_mutex_unlock(&g_state_lock);
    }
    return freed;
}

// ---- accelerability check ------------------------------------------------
// Returns 0 (declines) if `image` has ANY property outside the
// accelerator's supported shape. Also records WHY via `note`, when tracing
// is on, so ISH_PIXMAN_STATS output tells you what to widen next.
static int image_is_simple(pixman_image_t *image) {
    RESOLVE(real_gca, get_component_alpha_fn, "pixman_image_get_component_alpha");
    if (real_gca(image)) {
        note(STAT_DECLINE_COMPONENT_ALPHA);
        return 0;
    }
    pthread_mutex_lock(&g_state_lock);
    struct image_state *s = state_find(image);
    int has_transform = s != NULL && s->has_transform;
    int has_alpha_map = s != NULL && s->has_alpha_map;
    int has_clip = s != NULL && s->has_clip;
    int nontrivial_filter = s != NULL && s->nontrivial_filter;
    pixman_repeat_t repeat = s != NULL ? s->repeat : PIXMAN_REPEAT_NONE;
    pthread_mutex_unlock(&g_state_lock);
    if (has_transform) { note(STAT_DECLINE_TRANSFORM); return 0; }
    if (repeat != PIXMAN_REPEAT_NONE) { note(STAT_DECLINE_REPEAT); return 0; }
    if (nontrivial_filter) { note(STAT_DECLINE_FILTER); return 0; }
    if (has_alpha_map) { note(STAT_DECLINE_ALPHA_MAP); return 0; }
    if (has_clip) { note(STAT_DECLINE_CLIP); return 0; }
    return 1;
}

// ---- interposed composite/fill entry points ------------------------------

void pixman_image_composite32(pixman_op_t op, pixman_image_t *src, pixman_image_t *mask, pixman_image_t *dest,
        int32_t src_x, int32_t src_y, int32_t mask_x, int32_t mask_y,
        int32_t dest_x, int32_t dest_y, int32_t width, int32_t height) {
    RESOLVE(real, composite32_fn, "pixman_image_composite32");
    do {
        if (!accel_available()) { note(STAT_DECLINE_UNAVAILABLE); break; }
        // v1 only validated OVER-with-mask (the glyph/text-rendering shape);
        // SRC-with-mask and any other op+mask combination are real pixman
        // operations we simply haven't differential-tested, so they decline
        // rather than guess at untested arithmetic.
        if (mask != NULL && op != PIXMAN_OP_OVER) { note(STAT_DECLINE_OP); break; }
        if (mask == NULL && op != PIXMAN_OP_SRC && op != PIXMAN_OP_OVER) { note(STAT_DECLINE_OP); break; }
        if (width <= 0 || height <= 0) break; // real pixman handles the degenerate case; no benefit either way

        RESOLVE(get_format, get_format_fn, "pixman_image_get_format");
        pixman_format_code_t dst_fmt = get_format(dest);
        pixman_format_code_t src_fmt = get_format(src);
        int src_opaque_fmt = (src_fmt == PIXMAN_x8r8g8b8);
        if (dst_fmt != PIXMAN_a8r8g8b8 || (src_fmt != PIXMAN_a8r8g8b8 && src_fmt != PIXMAN_x8r8g8b8)) {
            note(STAT_DECLINE_FORMAT);
            break;
        }
        if (mask != NULL && get_format(mask) != PIXMAN_a8) {
            note(STAT_DECLINE_MASK_FORMAT);
            break;
        }
        if (!image_is_simple(src) || !image_is_simple(dest) || (mask != NULL && !image_is_simple(mask)))
            break; // image_is_simple already recorded the specific reason

        RESOLVE(get_data, get_data_fn, "pixman_image_get_data");
        RESOLVE(get_stride, get_stride_fn, "pixman_image_get_stride");
        RESOLVE(get_width, get_width_fn, "pixman_image_get_width");
        RESOLVE(get_height, get_height_fn, "pixman_image_get_height");
        int dst_w = get_width(dest), dst_h = get_height(dest);
        int src_w = get_width(src), src_h = get_height(src);
        // Real pixman auto-clips a composite rect to each image's own
        // bounds; replicating that exactly is extra complexity for no real
        // payoff (every caller we've profiled passes an already-in-bounds
        // rect), so simplify to decline whenever any part would be
        // out-of-bounds rather than attempting to clip ourselves.
        if (src_x < 0 || src_y < 0 || dest_x < 0 || dest_y < 0 ||
                (int64_t) src_x + width > src_w || (int64_t) src_y + height > src_h ||
                (int64_t) dest_x + width > dst_w || (int64_t) dest_y + height > dst_h) {
            note(STAT_DECLINE_BOUNDS);
            break;
        }
        if (mask != NULL) {
            int mask_w = get_width(mask), mask_h = get_height(mask);
            if (mask_x < 0 || mask_y < 0 ||
                    (int64_t) mask_x + width > mask_w || (int64_t) mask_y + height > mask_h) {
                note(STAT_DECLINE_BOUNDS);
                break;
            }
        }

        uint32_t *dst_bits = get_data(dest);
        uint32_t *src_bits = get_data(src);
        uint32_t dst_stride = (uint32_t) get_stride(dest); // pixman_image_get_stride is documented in BYTES
        uint32_t src_stride = (uint32_t) get_stride(src);
        long ret;
        if (mask != NULL) {
            uint32_t *mask_bits = get_data(mask);
            uint32_t mask_stride = (uint32_t) get_stride(mask);
            ret = pixop_mask(src_opaque_fmt ? PIX_FLAG_SRC_OPAQUE : 0,
                    dst_bits, dst_stride, dest_x, dest_y,
                    src_bits, src_stride, src_x, src_y,
                    mask_bits, mask_stride, mask_x, mask_y,
                    (uint32_t) width, (uint32_t) height);
        } else {
            ret = pixop(op == PIXMAN_OP_SRC ? PIX_OP_COPY : PIX_OP_OVER,
                    src_opaque_fmt ? PIX_FLAG_SRC_OPAQUE : 0,
                    dst_bits, dst_stride, dest_x, dest_y,
                    src_bits, src_stride, src_x, src_y,
                    (uint32_t) width, (uint32_t) height, 0);
        }
        if (ret == 0) {
            note(mask != NULL ? STAT_ACCEL_MASK : STAT_ACCEL_COMPOSITE);
            return;
        }
        note(STAT_DECLINE_SYSCALL);
    } while (0);
    real(op, src, mask, dest, src_x, src_y, mask_x, mask_y, dest_x, dest_y, width, height);
}

pixman_bool_t pixman_fill(uint32_t *bits, int stride, int bpp, int x, int y, int width, int height, uint32_t xor_) {
    RESOLVE(real, fill_fn, "pixman_fill");
    if (accel_available() && width > 0 && height > 0) {
        if (bpp == 32) {
            // pixman_fill's `stride` is documented (and verified empirically
            // against real pixman before this shim was written -- see
            // pixman_accel_plan.md) to be in 32-bit WORDS; our syscall's
            // dst_stride is bytes throughout.
            uint32_t stride_bytes = (uint32_t) stride * 4;
            long ret = pixop(PIX_OP_FILL, 0, bits, stride_bytes, x, y,
                    NULL, 0, 0, 0, (uint32_t) width, (uint32_t) height, xor_);
            if (ret == 0) {
                note(STAT_ACCEL_FILL);
                return 1;
            }
            note(STAT_DECLINE_SYSCALL);
        } else {
            note(STAT_DECLINE_FILL_BPP);
        }
    } else if (!accel_available()) {
        note(STAT_DECLINE_UNAVAILABLE);
    }
    return real(bits, stride, bpp, x, y, width, height, xor_);
}
