#include "zepto.h"
#include <cstdio>
#include <cstring>
#include <iostream>
#include <vector>

// Copy the RS internals inline for testing
static constexpr int K = 223, P = 32, N = 255;

struct GF256 {
    uint8_t exp[512], log[256];
    GF256() {
        uint8_t v = 1;
        for (int i = 0; i < 255; i++) { exp[i] = v; log[v] = (uint8_t)i; v = (uint8_t)((v << 1) ^ (v & 0x80 ? 0x11D : 0)); }
        for (int i = 255; i < 512; i++) exp[i] = exp[i-255];
        exp[255] = exp[0]; log[0] = 0;
    }
    uint8_t mul(uint8_t a, uint8_t b) const { if (!a||!b) return 0; return exp[(uint16_t)log[a]+(uint16_t)log[b]]; }
    uint8_t inv(uint8_t a) const { return a ? exp[255-log[a]] : 0; }
    uint8_t add(uint8_t a, uint8_t b) const { return a^b; }
};
static const GF256 gf;

static std::vector<uint8_t> rs_gen_poly(int p) {
    std::vector<uint8_t> g(p+1,0); g[0]=1;
    for (int i=0;i<p;i++) { uint8_t r=gf.exp[i];
        for (int j=p;j>0;j--) g[j]=gf.add(g[j-1],gf.mul(g[j],r));
        g[0]=gf.mul(g[0],r);
    } return g;
}

static void rs_encode_block(const uint8_t* data, int k, int p, const uint8_t* g, uint8_t* parity) {
    std::vector<uint8_t> poly(k+p,0);
    for (int i=0;i<k;i++) poly[p+i]=data[i];
    for (int i=k+p-1;i>=p;i--) {
        uint8_t coeff=poly[i]; if(!coeff) continue;
        for(int j=0;j<=p;j++) poly[i-p+j]^=gf.mul(coeff,g[j]);
    }
    for(int i=0;i<p;i++) parity[i]=poly[i];
}

// Copy the decode_block for testing
static int rs_decode_block(uint8_t* cw, int k, int p, const uint8_t* g) {
    int n = k + p;
    uint8_t S[256]; bool all_zero = true;
    for (int i = 0; i < p; i++) {
        uint8_t a = gf.exp[i];
        uint8_t val = 0;
        for (int j = n - 1; j >= 0; j--) val = gf.add(gf.mul(val, a), cw[j]);
        S[i] = val;
        if (val) all_zero = false;
    }
    if (all_zero) return 0;

    uint8_t C[256] = {0}, B[256] = {0};
    C[0] = 1; B[0] = 1;
    int L = 0, m = 1; uint8_t b = 1;
    for (int r = 0; r < p; r++) {
        uint8_t d = S[r];
        for (int i = 1; i <= L && i <= r; i++) d ^= gf.mul(C[i], S[r - i]);
        if (d == 0) { m++; continue; }
        uint8_t fb = gf.mul(d, gf.inv(b));
        if (2 * L <= r) {
            uint8_t T[256]; memcpy(T, C, sizeof(T));
            for (int j = m; j < 256; j++) C[j] ^= gf.mul(fb, B[j - m]);
            L = r + 1 - L; memcpy(B, T, sizeof(T)); b = d; m = 1;
        } else {
            for (int j = m; j < 256; j++) C[j] ^= gf.mul(fb, B[j - m]);
            m++;
        }
    }

    int degC = 0;
    for (int i = 0; i < 256; i++) if (C[i]) degC = i;
    if (degC > p / 2) return -1;

    int n_errors = 0, error_pos[256];
    for (int j = 0; j < n; j++) {
        uint8_t x = gf.exp[255 - j];
        uint8_t val = 0, xpow = 1;
        for (int i = 0; i <= degC; i++) { val ^= gf.mul(C[i], xpow); xpow = gf.mul(xpow, x); }
        if (val == 0) { error_pos[n_errors++] = j; }
    }
    if (n_errors != degC) return -1;

    uint8_t Omega[256] = {0};
    for (int i = 0; i < p; i++) {
        if (S[i] == 0) continue;
        for (int j = 0; j <= degC && i + j < p; j++) Omega[i + j] ^= gf.mul(S[i], C[j]);
    }

    uint8_t Cder[256] = {0};
    for (int i = 1; i <= degC; i += 2) Cder[i - 1] = C[i];

    for (int ei = 0; ei < n_errors; ei++) {
        int pos = error_pos[ei];
        uint8_t x_inv = gf.exp[(255 - pos) % 255];
        uint8_t Omega_val = 0, xpow = 1;
        for (int i = 0; i < p; i++) { Omega_val ^= gf.mul(Omega[i], xpow); xpow = gf.mul(xpow, x_inv); }
        uint8_t Cder_val = 0; xpow = 1;
        for (int i = 0; i < degC; i++) { Cder_val ^= gf.mul(Cder[i], xpow); xpow = gf.mul(xpow, x_inv); }
        if (Cder_val == 0) return -1;
        cw[pos] ^= gf.mul(gf.exp[pos], gf.mul(Omega_val, gf.inv(Cder_val)));
    }
    return n_errors;
}

int main() {
    auto gen = rs_gen_poly(P);
    
    // Create test data
    uint8_t data[K] = {0};
    for (int i = 0; i < 100; i++) data[i] = (uint8_t)(i * 7 + 3);
    
    // Encode
    uint8_t parity[P];
    rs_encode_block(data, K, P, gen.data(), parity);
    
    // Build codeword: [parity(32)][data(223)]
    uint8_t cw[N];
    memcpy(cw, parity, P);
    memcpy(cw + P, data, K);
    
    // Test 1: syndrome zero for valid codeword
    bool all_zero = true;
    for (int i = 0; i < P; i++) {
        uint8_t a = gf.exp[i];
        uint8_t val = 0;
        for (int j = N - 1; j >= 0; j--) val = gf.add(gf.mul(val, a), cw[j]);
        if (val) { printf("S[%d]=%02x\n", i, val); all_zero = false; }
    }
    printf("Test 1: syndromes all_zero=%d (expect 1)\n", all_zero);
    
    // Test 2: decode with no errors → return 0
    uint8_t cw_copy[N];
    memcpy(cw_copy, cw, N);
    int ret = rs_decode_block(cw_copy, K, P, gen.data());
    printf("Test 2: decode(clean)=%d (expect 0)\n", ret);
    
    // Test 3: inject 1 error, verify correction
    uint8_t cw_err[N];
    memcpy(cw_err, cw, N);
    cw_err[50] ^= 0xFF;
    ret = rs_decode_block(cw_err, K, P, gen.data());
    int match = (ret == 1 && cw_err[50] == cw[50]);
    printf("Test 3: decode(1 error at pos 50)=%d (expect 1), byte match=%d (expect 1)\n", ret, match);
    
    // Test 4: inject 16 errors, verify data integrity
    int inj16[] = {0,5,10,17,31,50,80,100,120,140,160,180,200,220,240,254};
    memcpy(cw_err, cw, N);
    for (int i = 0; i < 16; i++) cw_err[inj16[i]] ^= 0xFF;
    ret = rs_decode_block(cw_err, K, P, gen.data());
    match = (ret == 16 && memcmp(cw_err + P, data, K) == 0);
    printf("Test 4: decode(16 errors)=%d (expect 16), data match=%d (expect 1)\n", ret, match);
    
    // Test 5: inject 17 errors (too many) → should fail
    memcpy(cw_err, cw, N);
    for (int i = 0; i < 16; i++) cw_err[inj16[i]] ^= 0xFF;
    cw_err[1] ^= 0xFF; // 17th error
    ret = rs_decode_block(cw_err, K, P, gen.data());
    printf("Test 5: decode(17 errors)=%d (expect -1)\n", ret);
    
    return 0;
}
