// SHA-512 crypt ($6$), the format Linux stores in /etc/shadow (Ulrich Drepper's
// specification, default 5000 rounds). The app hashes the password itself so
// only the hash reaches Linux (chpasswd -e); the plaintext stays in app memory.
#include "PasswordHash.h"
#include <CommonCrypto/CommonDigest.h>
#include <CommonCrypto/CommonRandom.h>
#include <string.h>

static const char b64[] = "./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";

static char *encode24(char *out, unsigned b2, unsigned b1, unsigned b0, int n) {
    unsigned w = (b2 << 16) | (b1 << 8) | b0;
    while (n-- > 0) { *out++ = b64[w & 0x3f]; w >>= 6; }
    return out;
}

int lr_sha512_crypt(const char *password, const char *salt_in, char *out, size_t out_size) {
    size_t plen = strlen(password), slen = strnlen(salt_in, 16);
    if (out_size < 3 + slen + 1 + 86 + 1) return -1;
    const unsigned char *p = (const unsigned char *)password, *s = (const unsigned char *)salt_in;
    unsigned char a[64], b[64], dp[64], ds[64];
    CC_SHA512_CTX ctx, alt;

    CC_SHA512_Init(&alt); CC_SHA512_Update(&alt, p, (CC_LONG)plen); CC_SHA512_Update(&alt, s, (CC_LONG)slen);
    CC_SHA512_Update(&alt, p, (CC_LONG)plen); CC_SHA512_Final(b, &alt);

    CC_SHA512_Init(&ctx); CC_SHA512_Update(&ctx, p, (CC_LONG)plen); CC_SHA512_Update(&ctx, s, (CC_LONG)slen);
    size_t cnt;
    for (cnt = plen; cnt > 64; cnt -= 64) CC_SHA512_Update(&ctx, b, 64);
    CC_SHA512_Update(&ctx, b, (CC_LONG)cnt);
    for (cnt = plen; cnt > 0; cnt >>= 1) {
        if (cnt & 1) CC_SHA512_Update(&ctx, b, 64);
        else CC_SHA512_Update(&ctx, p, (CC_LONG)plen);
    }
    CC_SHA512_Final(a, &ctx);

    CC_SHA512_Init(&alt); for (cnt = 0; cnt < plen; cnt++) CC_SHA512_Update(&alt, p, (CC_LONG)plen); CC_SHA512_Final(dp, &alt);
    unsigned char P[256]; if (plen > sizeof P) return -1;
    for (cnt = 0; cnt < plen; cnt++) P[cnt] = dp[cnt % 64];

    CC_SHA512_Init(&alt); for (cnt = 0; cnt < 16u + a[0]; cnt++) CC_SHA512_Update(&alt, s, (CC_LONG)slen); CC_SHA512_Final(ds, &alt);
    unsigned char S[16];
    for (cnt = 0; cnt < slen; cnt++) S[cnt] = ds[cnt % 64];

    for (int r = 0; r < 5000; r++) {
        CC_SHA512_Init(&ctx);
        if (r & 1) CC_SHA512_Update(&ctx, P, (CC_LONG)plen); else CC_SHA512_Update(&ctx, a, 64);
        if (r % 3) CC_SHA512_Update(&ctx, S, (CC_LONG)slen);
        if (r % 7) CC_SHA512_Update(&ctx, P, (CC_LONG)plen);
        if (r & 1) CC_SHA512_Update(&ctx, a, 64); else CC_SHA512_Update(&ctx, P, (CC_LONG)plen);
        CC_SHA512_Final(a, &ctx);
    }

    char *o = out;
    memcpy(o, "$6$", 3); o += 3; memcpy(o, salt_in, slen); o += slen; *o++ = '$';
    static const int order[21][3] = {{0,21,42},{22,43,1},{44,2,23},{3,24,45},{25,46,4},{47,5,26},{6,27,48},
        {28,49,7},{50,8,29},{9,30,51},{31,52,10},{53,11,32},{12,33,54},{34,55,13},{56,14,35},{15,36,57},
        {37,58,16},{59,17,38},{18,39,60},{40,61,19},{62,20,41}};
    for (int i = 0; i < 21; i++) o = encode24(o, a[order[i][0]], a[order[i][1]], a[order[i][2]], 4);
    o = encode24(o, 0, 0, a[63], 2);
    *o = '\0';
    memset(P, 0, sizeof P); memset(dp, 0, sizeof dp); memset(b, 0, sizeof b);
    return 0;
}

int lr_random_salt(char out[17]) {
    unsigned char bytes[16];
    if (CCRandomGenerateBytes(bytes, sizeof bytes) != 0) return -1;
    for (int i = 0; i < 16; i++) out[i] = b64[bytes[i] & 0x3f];
    out[16] = '\0';
    return 0;
}
