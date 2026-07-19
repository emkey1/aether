// ChaCha20-Poly1305 via OpenSSL EVP: RFC 8439 sect 2.8.2 test vector
// (correctness ground truth) + throughput loop (timing). Links libcrypto.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
// Forward-declare the stable OpenSSL 3 EVP ABI (dev headers not in the guest).
typedef void EVP_CIPHER; typedef void EVP_CIPHER_CTX; typedef void ENGINE;
extern const EVP_CIPHER *EVP_chacha20_poly1305(void);
extern EVP_CIPHER_CTX *EVP_CIPHER_CTX_new(void);
extern void EVP_CIPHER_CTX_free(EVP_CIPHER_CTX *);
extern int EVP_EncryptInit_ex(EVP_CIPHER_CTX*,const EVP_CIPHER*,ENGINE*,const unsigned char*,const unsigned char*);
extern int EVP_EncryptUpdate(EVP_CIPHER_CTX*,unsigned char*,int*,const unsigned char*,int);
extern int EVP_EncryptFinal_ex(EVP_CIPHER_CTX*,unsigned char*,int*);
extern int EVP_CIPHER_CTX_ctrl(EVP_CIPHER_CTX*,int,int,void*);
#define EVP_CTRL_AEAD_GET_TAG 0x10

static double now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec+t.tv_nsec*1e-9; }

// RFC 8439 2.8.2 vectors
static const unsigned char key[32] = {
 0x80,0x81,0x82,0x83,0x84,0x85,0x86,0x87,0x88,0x89,0x8a,0x8b,0x8c,0x8d,0x8e,0x8f,
 0x90,0x91,0x92,0x93,0x94,0x95,0x96,0x97,0x98,0x99,0x9a,0x9b,0x9c,0x9d,0x9e,0x9f};
static const unsigned char iv[12] = {0x07,0,0,0,0x40,0x41,0x42,0x43,0x44,0x45,0x46,0x47};
static const unsigned char aad[12] = {0x50,0x51,0x52,0x53,0xc0,0xc1,0xc2,0xc3,0xc4,0xc5,0xc6,0xc7};
static const char *pt_str = "Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the future, sunscreen would be it.";
// Expected first 16 ciphertext bytes and tag from the RFC
static const unsigned char ct0[16] = {0xd3,0x1a,0x8d,0x34,0x64,0x8e,0x60,0xdb,0x7b,0x86,0xaf,0xbc,0x53,0xef,0x7e,0xc2};
static const unsigned char tag_exp[16] = {0x1a,0xe1,0x0b,0x59,0x4f,0x09,0xe2,0x6a,0x7e,0x90,0x2e,0xcb,0xd0,0x60,0x06,0x91};

static int seal(const unsigned char *pt, int ptlen, unsigned char *ct, unsigned char *tag){
    EVP_CIPHER_CTX *c = EVP_CIPHER_CTX_new(); int l, ok=0;
    if(!EVP_EncryptInit_ex(c, EVP_chacha20_poly1305(), NULL, key, iv)) goto out;
    EVP_EncryptUpdate(c, NULL, &l, aad, sizeof aad);
    EVP_EncryptUpdate(c, ct, &l, pt, ptlen);
    EVP_EncryptFinal_ex(c, ct+l, &l);
    EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_AEAD_GET_TAG, 16, tag);
    ok=1;
out: EVP_CIPHER_CTX_free(c); return ok;
}

int main(int argc, char **argv){
    unsigned char ct[256], tag[16];
    int ptlen = (int)strlen(pt_str);
    seal((const unsigned char*)pt_str, ptlen, ct, tag);
    int bad = memcmp(ct, ct0, 16) || memcmp(tag, tag_exp, 16);
    printf("rfc8439-vector: %s\n", bad ? "FAIL" : "PASS");
    if (bad) return 1;

    // throughput: seal a bigger buffer many times
    int bufsz = argc>1?atoi(argv[1]):8192;
    long iters = argc>2?atol(argv[2]):20000;
    static unsigned char big[65536], obuf[65536];
    memset(big, 0x5a, sizeof big);
    if (bufsz>(int)sizeof big) bufsz=sizeof big;
    volatile unsigned long sink=0;
    double t0=now();
    for(long i=0;i<iters;i++){ seal(big,bufsz,obuf,tag); sink+=tag[0]; }
    double t1=now();
    fprintf(stderr,"sink=%lu\n",sink);
    printf("%d %ld %.4f\n", bufsz, iters, t1-t0);
    return 0;
}
