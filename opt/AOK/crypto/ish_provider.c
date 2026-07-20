// iSH OpenSSL provider: routes ChaCha20 (the stream cipher OpenSSH's
// chacha20-poly1305@openssh.com uses via cipher-chachapoly-libcrypto) through
// the iSH crypto accelerator syscall, so ssh/scp encrypt at host-native speed
// instead of emulating libcrypto. Loaded via openssl.cnf; if the accelerator
// is unavailable (ISH_CRYPTO_ACCEL off, or self-test failed), the provider
// declines to register the algorithm so OpenSSL falls back to its default.
//
// Build (in-guest, against vendored OpenSSL core headers):
//   gcc -O2 -fPIC -shared -I<hdrs> -o ish.so ish_provider.c
//
// Scope: correct for the one-init + one-cipher-call pattern ssh uses, and for
// 64-byte-aligned streaming updates. (Arbitrary sub-block-straddling updates
// would need keystream buffering; not implemented -- such callers are rare and
// this provider simply must not be selected for them. See has_partial below.)

#include <openssl/core.h>
#include <openssl/core_dispatch.h>
#include <openssl/core_names.h>
#include <openssl/params.h>
#include <openssl/crypto.h>
#include <string.h>
#include <stdint.h>

extern long syscall(long, ...);
#define ISH_SYS_AEAD 0xacc0
#define ISH_ALG_CHACHA20 1

struct ish_aead_req {
    uint32_t op, alg;
    uint64_t key, nonce, aad, aadlen, in, inlen, out, tag;
};

#define CC_KEYLEN 32
#define CC_IVLEN  16

struct cc_ctx {
    unsigned char key[CC_KEYLEN];
    unsigned char iv[CC_IVLEN];
    uint64_t offset;    // stream bytes processed so far
    int has_key;
    int has_partial;    // set if a non-64-aligned update happened (poison)
};

// Probe the accelerator once: encrypt 0 bytes. 0 => available.
static int accel_ok(void) {
    static int cached = -1;
    if (cached < 0) {
        unsigned char k[32] = {0}, iv[16] = {0};
        struct ish_aead_req r = {0};
        r.alg = ISH_ALG_CHACHA20;
        r.key = (uint64_t) (uintptr_t) k;
        r.nonce = (uint64_t) (uintptr_t) iv;
        cached = syscall(ISH_SYS_AEAD, &r) == 0 ? 1 : 0;
    }
    return cached;
}

// Run the accelerator over one buffer at the ctx's current stream offset.
// Returns 1 on success. Advances offset. Requires 64-alignment of offset.
static int accel_run(struct cc_ctx *c, unsigned char *out, const unsigned char *in, size_t len) {
    if (c->offset % 64 != 0) { c->has_partial = 1; return 0; }
    unsigned char iv[CC_IVLEN];
    memcpy(iv, c->iv, CC_IVLEN);
    // advance the 32-bit LE block counter by offset/64 (wraps like OpenSSL)
    uint32_t ctr;
    memcpy(&ctr, iv, 4);
    ctr += (uint32_t) (c->offset / 64);
    memcpy(iv, &ctr, 4);
    struct ish_aead_req r = {0};
    r.alg = ISH_ALG_CHACHA20;
    r.key = (uint64_t) (uintptr_t) c->key;
    r.nonce = (uint64_t) (uintptr_t) iv;
    r.in = (uint64_t) (uintptr_t) in;
    r.inlen = len;
    r.out = (uint64_t) (uintptr_t) out;
    if (syscall(ISH_SYS_AEAD, &r) != 0)
        return 0;
    c->offset += len;
    return 1;
}

static OSSL_FUNC_cipher_newctx_fn cc_newctx;
static OSSL_FUNC_cipher_freectx_fn cc_freectx;
static OSSL_FUNC_cipher_dupctx_fn cc_dupctx;
static OSSL_FUNC_cipher_encrypt_init_fn cc_einit;
static OSSL_FUNC_cipher_decrypt_init_fn cc_dinit;
static OSSL_FUNC_cipher_update_fn cc_update;
static OSSL_FUNC_cipher_final_fn cc_final;
static OSSL_FUNC_cipher_cipher_fn cc_cipher;
static OSSL_FUNC_cipher_get_params_fn cc_get_params;
static OSSL_FUNC_cipher_get_ctx_params_fn cc_get_ctx_params;
static OSSL_FUNC_cipher_set_ctx_params_fn cc_set_ctx_params;
static OSSL_FUNC_cipher_gettable_params_fn cc_gettable_params;
static OSSL_FUNC_cipher_gettable_ctx_params_fn cc_gettable_ctx_params;
static OSSL_FUNC_cipher_settable_ctx_params_fn cc_settable_ctx_params;

static void *cc_newctx(void *provctx) {
    (void) provctx;
    struct cc_ctx *c = OPENSSL_zalloc(sizeof(*c));
    return c;
}
static void cc_freectx(void *vctx) {
    struct cc_ctx *c = vctx;
    if (c) { OPENSSL_cleanse(c, sizeof(*c)); OPENSSL_free(c); }
}
static void *cc_dupctx(void *vctx) {
    struct cc_ctx *c = vctx, *d;
    if (c == NULL) return NULL;
    d = OPENSSL_malloc(sizeof(*d));
    if (d) memcpy(d, c, sizeof(*d));
    return d;
}

static int cc_init(struct cc_ctx *c, const unsigned char *key, size_t keylen,
        const unsigned char *iv, size_t ivlen) {
    if (key != NULL) {
        if (keylen != CC_KEYLEN) return 0;
        memcpy(c->key, key, CC_KEYLEN);
        c->has_key = 1;
    }
    if (iv != NULL) {
        if (ivlen != CC_IVLEN) return 0;
        memcpy(c->iv, iv, CC_IVLEN);
    }
    c->offset = 0;
    c->has_partial = 0;
    return 1;
}
static int cc_einit(void *vctx, const unsigned char *key, size_t keylen,
        const unsigned char *iv, size_t ivlen, const OSSL_PARAM params[]) {
    (void) params; return cc_init(vctx, key, keylen, iv, ivlen);
}
static int cc_dinit(void *vctx, const unsigned char *key, size_t keylen,
        const unsigned char *iv, size_t ivlen, const OSSL_PARAM params[]) {
    (void) params; return cc_init(vctx, key, keylen, iv, ivlen); // symmetric
}

static int cc_do(struct cc_ctx *c, unsigned char *out, size_t *outl,
        const unsigned char *in, size_t inl) {
    if (!c->has_key || c->has_partial) return 0;
    if (inl > 0 && !accel_run(c, out, in, inl)) return 0;
    *outl = inl;
    return 1;
}
// Streaming update: whole-block updates chain correctly; a non-final update
// whose length isn't a multiple of 64 poisons the ctx (accel_run refuses).
static int cc_update(void *vctx, unsigned char *out, size_t *outl, size_t outsize,
        const unsigned char *in, size_t inl) {
    (void) outsize; return cc_do(vctx, out, outl, in, inl);
}
static int cc_final(void *vctx, unsigned char *out, size_t *outl, size_t outsize) {
    (void) vctx; (void) out; (void) outsize; *outl = 0; return 1;
}
// One-shot EVP_Cipher entry (what ssh uses): offset is 0, always aligned.
static int cc_cipher(void *vctx, unsigned char *out, size_t *outl, size_t outsize,
        const unsigned char *in, size_t inl) {
    (void) outsize; return cc_do(vctx, out, outl, in, inl);
}

static int cc_get_params(OSSL_PARAM params[]) {
    OSSL_PARAM *p;
    if ((p = OSSL_PARAM_locate(params, OSSL_CIPHER_PARAM_BLOCK_SIZE)) &&
            !OSSL_PARAM_set_size_t(p, 1)) return 0;
    if ((p = OSSL_PARAM_locate(params, OSSL_CIPHER_PARAM_KEYLEN)) &&
            !OSSL_PARAM_set_size_t(p, CC_KEYLEN)) return 0;
    if ((p = OSSL_PARAM_locate(params, OSSL_CIPHER_PARAM_IVLEN)) &&
            !OSSL_PARAM_set_size_t(p, CC_IVLEN)) return 0;
    if ((p = OSSL_PARAM_locate(params, OSSL_CIPHER_PARAM_MODE)) &&
            !OSSL_PARAM_set_uint(p, 0 /* EVP_CIPH_STREAM_CIPHER */)) return 0;
    return 1;
}
static int cc_get_ctx_params(void *vctx, OSSL_PARAM params[]) {
    (void) vctx; OSSL_PARAM *p;
    if ((p = OSSL_PARAM_locate(params, OSSL_CIPHER_PARAM_KEYLEN)) &&
            !OSSL_PARAM_set_size_t(p, CC_KEYLEN)) return 0;
    if ((p = OSSL_PARAM_locate(params, OSSL_CIPHER_PARAM_IVLEN)) &&
            !OSSL_PARAM_set_size_t(p, CC_IVLEN)) return 0;
    return 1;
}
static int cc_set_ctx_params(void *vctx, const OSSL_PARAM params[]) {
    (void) vctx; (void) params; return 1;
}
static const OSSL_PARAM cc_known_params[] = {
    OSSL_PARAM_size_t(OSSL_CIPHER_PARAM_BLOCK_SIZE, NULL),
    OSSL_PARAM_size_t(OSSL_CIPHER_PARAM_KEYLEN, NULL),
    OSSL_PARAM_size_t(OSSL_CIPHER_PARAM_IVLEN, NULL),
    OSSL_PARAM_uint(OSSL_CIPHER_PARAM_MODE, NULL),
    OSSL_PARAM_END
};
static const OSSL_PARAM *cc_gettable_params(void *p) { (void) p; return cc_known_params; }
static const OSSL_PARAM *cc_gettable_ctx_params(void *c, void *p) { (void) c; (void) p; return cc_known_params; }
static const OSSL_PARAM *cc_settable_ctx_params(void *c, void *p) { (void) c; (void) p; return cc_known_params; }

static const OSSL_DISPATCH cc_functions[] = {
    { OSSL_FUNC_CIPHER_NEWCTX, (void (*)(void)) cc_newctx },
    { OSSL_FUNC_CIPHER_FREECTX, (void (*)(void)) cc_freectx },
    { OSSL_FUNC_CIPHER_DUPCTX, (void (*)(void)) cc_dupctx },
    { OSSL_FUNC_CIPHER_ENCRYPT_INIT, (void (*)(void)) cc_einit },
    { OSSL_FUNC_CIPHER_DECRYPT_INIT, (void (*)(void)) cc_dinit },
    { OSSL_FUNC_CIPHER_UPDATE, (void (*)(void)) cc_update },
    { OSSL_FUNC_CIPHER_FINAL, (void (*)(void)) cc_final },
    { OSSL_FUNC_CIPHER_CIPHER, (void (*)(void)) cc_cipher },
    { OSSL_FUNC_CIPHER_GET_PARAMS, (void (*)(void)) cc_get_params },
    { OSSL_FUNC_CIPHER_GET_CTX_PARAMS, (void (*)(void)) cc_get_ctx_params },
    { OSSL_FUNC_CIPHER_SET_CTX_PARAMS, (void (*)(void)) cc_set_ctx_params },
    { OSSL_FUNC_CIPHER_GETTABLE_PARAMS, (void (*)(void)) cc_gettable_params },
    { OSSL_FUNC_CIPHER_GETTABLE_CTX_PARAMS, (void (*)(void)) cc_gettable_ctx_params },
    { OSSL_FUNC_CIPHER_SETTABLE_CTX_PARAMS, (void (*)(void)) cc_settable_ctx_params },
    { 0, NULL }
};

static const OSSL_ALGORITHM ish_ciphers[] = {
    { "ChaCha20", "provider=ish", cc_functions, "iSH-accelerated ChaCha20" },
    { NULL, NULL, NULL, NULL }
};
static const OSSL_ALGORITHM ish_ciphers_none[] = { { NULL, NULL, NULL, NULL } };

static const OSSL_ALGORITHM *ish_query(void *provctx, int operation_id, int *no_cache) {
    (void) provctx; *no_cache = 0;
    if (operation_id == OSSL_OP_CIPHER)
        return accel_ok() ? ish_ciphers : ish_ciphers_none;
    return NULL;
}

static void ish_teardown(void *provctx) { (void) provctx; }

static const OSSL_DISPATCH ish_provider_funcs[] = {
    { OSSL_FUNC_PROVIDER_QUERY_OPERATION, (void (*)(void)) ish_query },
    { OSSL_FUNC_PROVIDER_TEARDOWN, (void (*)(void)) ish_teardown },
    { 0, NULL }
};

int OSSL_provider_init(const OSSL_CORE_HANDLE *handle, const OSSL_DISPATCH *in,
        const OSSL_DISPATCH **out, void **provctx) {
    (void) handle; (void) in; *provctx = NULL;
    *out = ish_provider_funcs;
    return 1;
}
