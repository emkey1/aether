# iSH crypto-accelerator OpenSSL provider

Routes OpenSSL's `ChaCha20` stream cipher through the iSH crypto accelerator
syscall (ISH_SYS_AEAD / kernel/ish_accel.c) so it runs host-native instead of
being emulated instruction-by-instruction. This is the cipher OpenSSH's
default `chacha20-poly1305@openssh.com` uses (via cipher-chachapoly-libcrypto),
so loading this provider accelerates ssh/scp transparently -- no changes to ssh.

Requires the accelerator enabled (ISH_CRYPTO_ACCEL=1 / the app's crypto-accel
toggle). If unavailable the provider declines the algorithm and OpenSSL falls
back to its own ChaCha20, so it is always safe to load.

## Measured (Release/-O2 build, riscv64, 16 KiB records)
- `EVP_chacha20` via ish provider: ~496 MB/s vs ~26 MB/s default = ~19x
- transparent (openssl.cnf, legacy `EVP_chacha20()` as ssh calls it): ~15x
- output bit-identical to OpenSSL's default provider (200-case differential)

## Install
    sh build-provider.sh            # builds + prints the openssl.cnf snippet
Then merge that snippet into /etc/ssl/openssl.cnf (or set OPENSSL_CONF).

## Scope / limitations
- ChaCha20 stream only so far. ChaCha20-Poly1305 AEAD and AES-256-GCM
  providers are the planned follow-ons (the accelerator already does the AEAD).
- Correct for the one-init + one-cipher pattern ssh uses and 64-byte-aligned
  streaming updates; a mid-stream sub-block-straddling update poisons the ctx
  (accel declines -> the caller errors rather than getting wrong data). Such
  callers are rare; the provider must simply not be preferred for them.
- Must be built per guest arch and shipped in the rootfs (a rootfs-prep step).
