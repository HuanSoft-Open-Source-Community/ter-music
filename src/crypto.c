#include "../include/crypto.h"
#include <string.h>
#include <stdio.h>

#define CRYPTO_SALT "ter-music-remote-password-v1"
#define KEY_BYTES 32

/* ---------- standalone SHA256 ---------- */

static const unsigned int K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
    0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
    0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

#define ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
#define CH(e, f, g) (((e) & (f)) ^ (~(e) & (g)))
#define MAJ(a, b, c) (((a) & (b)) ^ ((a) & (c)) ^ ((b) & (c)))
#define SIG0(x) (ROTR(x, 2) ^ ROTR(x, 13) ^ ROTR(x, 22))
#define SIG1(x) (ROTR(x, 6) ^ ROTR(x, 11) ^ ROTR(x, 25))
#define sig0(x) (ROTR(x, 7) ^ ROTR(x, 18) ^ ((x) >> 3))
#define sig1(x) (ROTR(x, 17) ^ ROTR(x, 19) ^ ((x) >> 10))

static void sha256(const unsigned char *data, size_t len, unsigned char out[32]) {
    unsigned int H[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
    };
    unsigned char block[64];
    size_t i, n;
    unsigned long long bit_len = (unsigned long long)len * 8;

    // Process full blocks
    for (i = 0; i + 64 <= len; i += 64) {
        unsigned int W[64];
        unsigned int a, b, c, d, e, f, g, h, t1, t2;
        int t;

        memcpy(block, data + i, 64);
        for (t = 0; t < 16; t++)
            W[t] = ((unsigned int)block[t*4] << 24) | (block[t*4+1] << 16) |
                   (block[t*4+2] << 8) | block[t*4+3];
        for (t = 16; t < 64; t++)
            W[t] = sig1(W[t-2]) + W[t-7] + sig0(W[t-15]) + W[t-16];

        a = H[0]; b = H[1]; c = H[2]; d = H[3];
        e = H[4]; f = H[5]; g = H[6]; h = H[7];

        for (t = 0; t < 64; t++) {
            t1 = h + SIG1(e) + CH(e, f, g) + K[t] + W[t];
            t2 = SIG0(a) + MAJ(a, b, c);
            h = g; g = f; f = e; e = d + t1;
            d = c; c = b; b = a; a = t1 + t2;
        }

        H[0] += a; H[1] += b; H[2] += c; H[3] += d;
        H[4] += e; H[5] += f; H[6] += g; H[7] += h;
    }

    // Padding
    size_t rem = len - i;
    memset(block, 0, 64);
    memcpy(block, data + i, rem);
    block[rem] = 0x80;

    if (rem >= 56) {
        // Need an extra block
        unsigned int W[64];
        unsigned int a, b, c, d, e, f, g, h, t1, t2;
        int t;

        for (t = 0; t < 16; t++)
            W[t] = ((unsigned int)block[t*4] << 24) | (block[t*4+1] << 16) |
                   (block[t*4+2] << 8) | block[t*4+3];
        for (t = 16; t < 64; t++)
            W[t] = sig1(W[t-2]) + W[t-7] + sig0(W[t-15]) + W[t-16];

        a = H[0]; b = H[1]; c = H[2]; d = H[3];
        e = H[4]; f = H[5]; g = H[6]; h = H[7];

        for (t = 0; t < 64; t++) {
            t1 = h + SIG1(e) + CH(e, f, g) + K[t] + W[t];
            t2 = SIG0(a) + MAJ(a, b, c);
            h = g; g = f; f = e; e = d + t1;
            d = c; c = b; b = a; a = t1 + t2;
        }

        H[0] += a; H[1] += b; H[2] += c; H[3] += d;
        H[4] += e; H[5] += f; H[6] += g; H[7] += h;

        memset(block, 0, 64);
    }

    // Write length in last 8 bytes
    block[56] = (unsigned char)(bit_len >> 56);
    block[57] = (unsigned char)(bit_len >> 48);
    block[58] = (unsigned char)(bit_len >> 40);
    block[59] = (unsigned char)(bit_len >> 32);
    block[60] = (unsigned char)(bit_len >> 24);
    block[61] = (unsigned char)(bit_len >> 16);
    block[62] = (unsigned char)(bit_len >> 8);
    block[63] = (unsigned char)(bit_len);

    // Process final block
    {
        unsigned int W[64];
        unsigned int a, b, c, d, e, f, g, h, t1, t2;
        int t;

        for (t = 0; t < 16; t++)
            W[t] = ((unsigned int)block[t*4] << 24) | (block[t*4+1] << 16) |
                   (block[t*4+2] << 8) | block[t*4+3];
        for (t = 16; t < 64; t++)
            W[t] = sig1(W[t-2]) + W[t-7] + sig0(W[t-15]) + W[t-16];

        a = H[0]; b = H[1]; c = H[2]; d = H[3];
        e = H[4]; f = H[5]; g = H[6]; h = H[7];

        for (t = 0; t < 64; t++) {
            t1 = h + SIG1(e) + CH(e, f, g) + K[t] + W[t];
            t2 = SIG0(a) + MAJ(a, b, c);
            h = g; g = f; f = e; e = d + t1;
            d = c; c = b; b = a; a = t1 + t2;
        }

        H[0] += a; H[1] += b; H[2] += c; H[3] += d;
        H[4] += e; H[5] += f; H[6] += g; H[7] += h;
    }

    for (i = 0; i < 8; i++) {
        out[i*4]   = (unsigned char)(H[i] >> 24);
        out[i*4+1] = (unsigned char)(H[i] >> 16);
        out[i*4+2] = (unsigned char)(H[i] >> 8);
        out[i*4+3] = (unsigned char)(H[i]);
    }
}

/* ---------- encrypt / decrypt ---------- */

static void derive_key(unsigned char key[KEY_BYTES]) {
    sha256((const unsigned char *)CRYPTO_SALT, strlen(CRYPTO_SALT), key);
}

void crypto_encrypt(const char *plaintext, char *hex_out, size_t hex_out_size) {
    unsigned char key[KEY_BYTES];
    derive_key(key);

    size_t len = strlen(plaintext);
    for (size_t i = 0; i < len && i * 2 + 1 < hex_out_size; i++) {
        unsigned char c = (unsigned char)plaintext[i] ^ key[i % KEY_BYTES];
        sprintf(hex_out + i * 2, "%02x", c);
    }
    if (len * 2 < hex_out_size)
        hex_out[len * 2] = '\0';
    else
        hex_out[hex_out_size - 1] = '\0';
}

void crypto_decrypt(const char *hex_in, char *plaintext_out, size_t plaintext_size) {
    unsigned char key[KEY_BYTES];
    derive_key(key);

    size_t hex_len = strlen(hex_in);
    size_t out_len = hex_len / 2;
    if (out_len >= plaintext_size) out_len = plaintext_size - 1;

    for (size_t i = 0; i < out_len; i++) {
        unsigned int byte;
        sscanf(hex_in + i * 2, "%02x", &byte);
        plaintext_out[i] = (char)((unsigned char)byte ^ key[i % KEY_BYTES]);
    }
    plaintext_out[out_len] = '\0';
}
