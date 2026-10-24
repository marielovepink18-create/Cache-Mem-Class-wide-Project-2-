/* kern.h - inner loops shared by bw.c / loaded.c / lat.c */
#ifndef CM_KERN_H
#define CM_KERN_H
#include "common.h"

/* ---- block read: sum of nw 64-bit words (integer reduction: vectorises under -O3 without fast-math) */
static UNUSED inline uint64_t rd_block(const uint64_t *__restrict p, size_t nw) {
    uint64_t s0 = 0, s1 = 0, s2 = 0, s3 = 0;
    size_t i = 0;
    for (; i + 4 <= nw; i += 4) { s0 += p[i]; s1 += p[i + 1]; s2 += p[i + 2]; s3 += p[i + 3]; }
    for (; i < nw; i++) s0 += p[i];
    return s0 + s1 + s2 + s3;
}

/* ---- block write with regular (write-allocate) stores. Value varies so GCC cannot turn it into memset. */
static UNUSED inline void wr_block(uint64_t *__restrict p, size_t nw, uint64_t v) {
    for (size_t i = 0; i < nw; i++) p[i] = v + i;
}

/* ---- block write with non-temporal stores (bypass cache, no read-for-ownership). Needs 16 B alignment. */
static UNUSED inline void nt_block(uint64_t *p, size_t nw, uint64_t v) {
#if defined(__SSE2__)
    __m128i *q = (__m128i *)p;
    size_t n2 = nw / 2;
    for (size_t i = 0; i < n2; i++) _mm_stream_si128(q + i, _mm_set1_epi64x((long long)(v + i)));
#else
    wr_block(p, nw, v);
#endif
}

static UNUSED inline void store_fence(void) {
#if defined(__SSE2__)
    _mm_sfence();
#endif
}

/* ---- dependent pointer chase, 8x unrolled. n must be a multiple of 8. */
static UNUSED void *chase1(void *p, size_t n) {
    for (size_t i = 0; i < n; i += 8) {
        p = *(void **)p; p = *(void **)p; p = *(void **)p; p = *(void **)p;
        p = *(void **)p; p = *(void **)p; p = *(void **)p; p = *(void **)p;
    }
    return p;
}

/* ---- K independent chains advanced in lock-step (K outstanding loads) */
static UNUSED void chasek(void **p, int K, size_t n) {
    for (size_t i = 0; i < n; i++)
        for (int k = 0; k < K; k++) p[k] = *(void **)p[k];
}

/* Build K disjoint cycles over nn nodes spaced `stride` bytes apart.
 * rand=1: node order is a random permutation (each chain is a random cycle spanning the buffer)
 * rand=0: node order is address order (chain k = contiguous segment k, constant-stride walk)    */
static UNUSED int build_chains(char *base, size_t nn, size_t stride, int K, int rnd, uint64_t seed, void **start) {
    uint32_t *perm = (uint32_t *)malloc(nn * sizeof(uint32_t));
    if (!perm) return -1;
    for (size_t i = 0; i < nn; i++) perm[i] = (uint32_t)i;
    if (rnd) { rng_t r = { seed ? seed : 88172645463325252ULL }; shuffle_u32(perm, nn, &r); }
    size_t seg = nn / (size_t)K;
    for (int k = 0; k < K; k++) {
        size_t b = (size_t)k * seg, e = (k == K - 1) ? nn : b + seg;
        for (size_t j = b; j < e; j++) {
            size_t nx = (j + 1 < e) ? perm[j + 1] : perm[b];
            *(void **)(base + (size_t)perm[j] * stride) = base + nx * stride;
        }
        start[k] = base + (size_t)perm[b] * stride;
    }
    free(perm);
    return 0;
}

#endif
