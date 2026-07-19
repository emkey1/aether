// Guest-side HLE fingerprint dumper. Run inside the target rootfs:
//   cc -O2 -o hlefp hle_fingerprint_guest.c -ldl
//   ./hlefp GUEST_ABI_ARM64 glibc-2.41-devuan >> jit/hle-table.inc
//
// dlsym() returns the RESOLVED implementation address -- for glibc's ifunc
// symbols (memcpy & friends on aarch64) that is the per-hwcap variant the
// dynamic linker actually selected under iSH's advertised AT_HWCAP, i.e.
// exactly the entry point block translation will see when guest code calls
// through the PLT. Dumping the prologue here is therefore both simpler and
// more faithful than parsing the stripped .so offline.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>

static const struct { const char *sym; const char *fn; } FNS[] = {
    { "memcpy", "HLE_MEMCPY" },
    { "memmove", "HLE_MEMMOVE" },
    { "memset", "HLE_MEMSET" },
    { "strlen", "HLE_STRLEN" },
    { "memcmp", "HLE_MEMCMP" },
    { "strcmp", "HLE_STRCMP" },
    { "strncmp", "HLE_STRNCMP" },
    { "memchr", "HLE_MEMCHR" },
    { "strchr", "HLE_STRCHR" },
    { "strcpy", "HLE_STRCPY" },
    { "stpcpy", "HLE_STPCPY" },
    { "strncpy", "HLE_STRNCPY" },
    { "strcat", "HLE_STRCAT" },
    { "strrchr", "HLE_STRRCHR" },
    { "strnlen", "HLE_STRNLEN" },
    { "memrchr", "HLE_MEMRCHR" },
};

int main(int argc, char **argv) {
    const char *abi = argc > 1 ? argv[1] : "GUEST_ABI_ARM64";
    const char *label = argc > 2 ? argv[2] : "unknown";
    for (unsigned i = 0; i < sizeof(FNS)/sizeof(FNS[0]); i++) {
        unsigned char *p = (unsigned char *) dlsym(RTLD_DEFAULT, FNS[i].sym);
        if (p == 0) {
            fprintf(stderr, "%s: unresolved\n", FNS[i].sym);
            continue;
        }
        printf("{ %s, %s, \"%s:%s\",\n  { ", abi, FNS[i].fn, FNS[i].sym, label);
        for (int b = 0; b < 64; b++)
            printf("0x%02x%s", p[b], b == 63 ? "" : ",");
        printf(" } },\n");
    }
    return 0;
}
