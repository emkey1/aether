#ifndef ISH_ACCEL_CRYPTO_H
#define ISH_ACCEL_CRYPTO_H
#include <stdint.h>
#include <stddef.h>

// Host-native ChaCha20-Poly1305 AEAD (RFC 8439). Host pointers only; the
// syscall/device layer resolves and bounds-checks guest buffers first.
// ct must have room for ptlen bytes; tag is 16 bytes.
void ish_chacha20_poly1305_seal(const uint8_t key[32], const uint8_t nonce[12],
        const uint8_t *aad, size_t aadlen, const uint8_t *pt, size_t ptlen,
        uint8_t *ct, uint8_t tag[16]);

// Returns 0 on success (tag valid, pt filled), -1 on auth failure (pt untouched).
int ish_chacha20_poly1305_open(const uint8_t key[32], const uint8_t nonce[12],
        const uint8_t *aad, size_t aadlen, const uint8_t *ct, size_t ctlen,
        const uint8_t tag[16], uint8_t *pt);

// Validates the implementation against the RFC 8439 test vectors. Returns
// true on success. Called once before the accelerator is enabled.
_Bool ish_accel_crypto_selftest(void);

#endif
