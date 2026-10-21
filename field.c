#include "bats_internal.h"

#include <cpuid.h>
#include <tmmintrin.h>

static int cpu_ssse3;

static int cpu_has_ssse3(void)
{
    unsigned int a;
    unsigned int b;
    unsigned int c;
    unsigned int d;

    if (!__get_cpuid(1, &a, &b, &c, &d)) {
        return 0;
    }
    return (c & bit_SSSE3) != 0;
}

static void gf_axpy_scalar(uint8_t *dst, const uint8_t *src, uint8_t a, size_t n)
{
    size_t i;

    if (a == 0) {
        return;
    }
    for (i = 0; i < n; i++) {
        dst[i] ^= gf_mul(a, src[i]);
    }
}

__attribute__((target("ssse3"))) static void gf_axpy_ssse3(uint8_t *dst, const uint8_t *src,
                                                           uint8_t a, size_t n)
{
    uint8_t lo_b[16];
    uint8_t hi_b[16];
    __m128i tab_lo;
    __m128i tab_hi;
    __m128i mask;
    size_t i;
    int k;

    if (a == 0 || n == 0) {
        return;
    }
    for (k = 0; k < 16; k++) {
        lo_b[k] = gf_mul_tab[((unsigned)a << 8) | (unsigned)k];
        hi_b[k] = gf_mul_tab[((unsigned)a << 8) | ((unsigned)k << 4)];
    }
    tab_lo = _mm_loadu_si128((const __m128i *)lo_b);
    tab_hi = _mm_loadu_si128((const __m128i *)hi_b);
    mask = _mm_set1_epi8(0x0f);
    for (i = 0; i + 16 <= n; i += 16) {
        __m128i x = _mm_loadu_si128((const __m128i *)(src + i));
        __m128i lo = _mm_shuffle_epi8(tab_lo, _mm_and_si128(x, mask));
        __m128i hi_n = _mm_and_si128(_mm_srli_epi16(x, 4), mask);
        __m128i hi = _mm_shuffle_epi8(tab_hi, hi_n);
        __m128i y = _mm_xor_si128(lo, hi);
        __m128i d = _mm_loadu_si128((const __m128i *)(dst + i));

        _mm_storeu_si128((__m128i *)(dst + i), _mm_xor_si128(d, y));
    }
    gf_axpy_scalar(dst + i, src + i, a, n - i);
}

static int gf_axpy_self_test(void)
{
    uint8_t src[64];
    uint8_t got[64];
    uint8_t expect[64];
    int a;
    int i;

    for (i = 0; i < 64; i++) {
        src[i] = (uint8_t)(i * 17 + 3);
    }
    for (a = 0; a < 256; a++) {
        memset(got, 0x5a, 20);
        memset(expect, 0x5a, 20);
        gf_axpy_scalar(expect, src, (uint8_t)a, 20);
        gf_axpy_ssse3(got, src, (uint8_t)a, 20);
        if (memcmp(got, expect, 20) != 0) {
            return -1;
        }
        memset(got, 0x11, sizeof(got));
        memset(expect, 0x11, sizeof(expect));
        gf_axpy_scalar(expect, src, (uint8_t)a, sizeof(src));
        gf_axpy_ssse3(got, src, (uint8_t)a, sizeof(src));
        if (memcmp(got, expect, sizeof(got)) != 0) {
            return -1;
        }
    }
    return 0;
}

static void gf_axpy_init(void)
{
    const char *force_scalar = getenv("BATS_SCALAR");

    cpu_ssse3 = cpu_has_ssse3();
    /* BATS_SCALAR=1 时改走标量，用来和 SSSE3 比同一条四跳的 CPU。 */
    if (force_scalar != NULL && force_scalar[0] != '\0' && force_scalar[0] != '0') {
        cpu_ssse3 = 0;
    }
    if (cpu_ssse3 && gf_axpy_self_test() != 0) {
        fprintf(stderr, "gf_axpy ssse3 自检失败\n");
        exit(1);
    }
    fprintf(stderr, "gf_axpy=%s\n", cpu_ssse3 ? "ssse3" : "scalar");
}

void gf_axpy(uint8_t *dst, const uint8_t *src, uint8_t a, size_t n)
{
    if (a == 0 || n == 0) {
        return;
    }
    if (cpu_ssse3) {
        gf_axpy_ssse3(dst, src, a, n);
        return;
    }
    gf_axpy_scalar(dst, src, a, n);
}

/* GF(256)、随机数，以及系数矩阵的行约化。源、中继、目的都用。 */

static uint8_t gf_exp[512];
static uint8_t gf_log[256];
uint8_t gf_mul_tab[256 * 256];

void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) {
        fprintf(stderr, "内存不足\n");
        exit(1);
    }
    return p;
}

void *xrealloc(void *p, size_t n) {
    void *q = realloc(p, n ? n : 1);
    if (!q) {
        fprintf(stderr, "内存不足\n");
        exit(1);
    }
    return q;
}

void gf_init(void) {
    unsigned x = 1;
    for (int i = 0; i < 255; i++) {
        gf_exp[i] = (uint8_t)x;
        gf_log[x] = (uint8_t)i;
        x <<= 1;
        if (x & 0x100) {
            x ^= 0x11D;
        }
    }
    for (int i = 255; i < 512; i++) {
        gf_exp[i] = gf_exp[i - 255];
    }
    for (int a = 0; a < 256; a++) {
        for (int b = 0; b < 256; b++) {
            uint8_t v = 0;
            if (a != 0 && b != 0) {
                v = gf_exp[gf_log[a] + gf_log[b]];
            }
            gf_mul_tab[(a << 8) | b] = v;
        }
    }
    gf_axpy_init();
}

uint8_t gf_inv(uint8_t a) {
    if (a == 0) {
        fprintf(stderr, "对 0 求逆\n");
        exit(1);
    }
    return gf_exp[255 - gf_log[a]];
}

static uint64_t rng_u64(Rng *r) {
    uint64_t z = (r->s += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

uint32_t rng_below(Rng *r, uint32_t n) {
    return (uint32_t)(rng_u64(r) % n);
}

uint8_t rng_byte(Rng *r) {
    return (uint8_t)rng_u64(r);
}

/* 每个 batch 一条独立流：只依赖种子和 batch_id，丢批也不打乱别的批。
   不能把 batch_id 直接加上 splitmix 的步长，否则相邻 batch 只是同一条流错开一拍。 */
Rng rng_for_batch(uint64_t seed, uint32_t batch_id) {
    Rng r;
    uint64_t z = seed ^ (0xD1B54A32D192ED03ULL * ((uint64_t)batch_id + 1ULL));
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    r.s = z ^ (z >> 31);
    return r;
}

/*
 * A 是 nU×nR。成功时 Inv * S = I，S 的第 i 列是 A 的第 col_of_pivot[i] 列。
 * 返回秩。秩等于 nU 时 Inv 可用。
 */
int row_reduce(const uint8_t *A, int nU, int nR, int *col_of_pivot, uint8_t *Inv) {
    uint8_t *W;
    int rank = 0;
    int col;
    if (nU == 0) {
        return 0;
    }
    W = xmalloc((size_t)nU * (size_t)nR);
    memcpy(W, A, (size_t)nU * (size_t)nR);
    if (Inv) {
        memset(Inv, 0, (size_t)nU * (size_t)nU);
        for (col = 0; col < nU; col++) {
            Inv[(size_t)col * (size_t)nU + (size_t)col] = 1;
        }
    }
    for (col = 0; col < nR && rank < nU; col++) {
        int piv = -1;
        int row;
        uint8_t invp;
        for (row = rank; row < nU; row++) {
            if (W[(size_t)row * (size_t)nR + (size_t)col]) {
                piv = row;
                break;
            }
        }
        if (piv < 0) {
            continue;
        }
        if (piv != rank) {
            int c;
            for (c = 0; c < nR; c++) {
                uint8_t tmp = W[(size_t)rank * (size_t)nR + (size_t)c];
                W[(size_t)rank * (size_t)nR + (size_t)c] =
                    W[(size_t)piv * (size_t)nR + (size_t)c];
                W[(size_t)piv * (size_t)nR + (size_t)c] = tmp;
            }
            if (Inv) {
                for (c = 0; c < nU; c++) {
                    uint8_t tmp = Inv[(size_t)rank * (size_t)nU + (size_t)c];
                    Inv[(size_t)rank * (size_t)nU + (size_t)c] =
                        Inv[(size_t)piv * (size_t)nU + (size_t)c];
                    Inv[(size_t)piv * (size_t)nU + (size_t)c] = tmp;
                }
            }
        }
        invp = gf_inv(W[(size_t)rank * (size_t)nR + (size_t)col]);
        {
            int c;
            for (c = 0; c < nR; c++) {
                W[(size_t)rank * (size_t)nR + (size_t)c] =
                    gf_mul(W[(size_t)rank * (size_t)nR + (size_t)c], invp);
            }
            if (Inv) {
                for (c = 0; c < nU; c++) {
                    Inv[(size_t)rank * (size_t)nU + (size_t)c] =
                        gf_mul(Inv[(size_t)rank * (size_t)nU + (size_t)c], invp);
                }
            }
        }
        for (row = 0; row < nU; row++) {
            uint8_t f;
            int c;
            if (row == rank) {
                continue;
            }
            f = W[(size_t)row * (size_t)nR + (size_t)col];
            if (!f) {
                continue;
            }
            for (c = 0; c < nR; c++) {
                W[(size_t)row * (size_t)nR + (size_t)c] ^=
                    gf_mul(f, W[(size_t)rank * (size_t)nR + (size_t)c]);
            }
            if (Inv) {
                for (c = 0; c < nU; c++) {
                    Inv[(size_t)row * (size_t)nU + (size_t)c] ^=
                        gf_mul(f, Inv[(size_t)rank * (size_t)nU + (size_t)c]);
                }
            }
        }
        if (col_of_pivot) {
            col_of_pivot[rank] = col;
        }
        rank++;
    }
    free(W);
    return rank;
}

/* 行主序的 rows×M 系数矩阵。整批丢失（rows<=0）为 0。 */
int coeff_rank_matrix(const uint8_t *coeff, int rows) {
    if (rows <= 0 || coeff == NULL) {
        return 0;
    }
    return row_reduce(coeff, rows, M, NULL, NULL);
}

int gf_self_test(void) {
    int i;
    uint8_t seen[256];
    uint8_t A[4];
    uint8_t Inv[4];
    uint8_t S[4];
    int piv[2];
    int rank;
    uint8_t prod[4];
    memset(seen, 0, sizeof(seen));
    for (i = 0; i < 255; i++) {
        if (seen[gf_exp[i]]) {
            fprintf(stderr, "GF(256) 生成元重复\n");
            return 0;
        }
        seen[gf_exp[i]] = 1;
    }
    if (seen[0] || gf_mul(2, 128) != 0x1D) {
        fprintf(stderr, "GF(256) 乘法表错误\n");
        return 0;
    }
    for (i = 1; i < 256; i++) {
        if (gf_mul((uint8_t)i, gf_inv((uint8_t)i)) != 1) {
            fprintf(stderr, "GF(256) 逆元错误\n");
            return 0;
        }
    }
    /* [[0,1],[1,0]] 的行约化逆。 */
    A[0] = 0;
    A[1] = 1;
    A[2] = 1;
    A[3] = 0;
    rank = row_reduce(A, 2, 2, piv, Inv);
    if (rank != 2) {
        fprintf(stderr, "行约化未满秩\n");
        return 0;
    }
    S[0] = A[(size_t)0 * 2 + (size_t)piv[0]];
    S[1] = A[(size_t)0 * 2 + (size_t)piv[1]];
    S[2] = A[(size_t)1 * 2 + (size_t)piv[0]];
    S[3] = A[(size_t)1 * 2 + (size_t)piv[1]];
    prod[0] = gf_mul(Inv[0], S[0]) ^ gf_mul(Inv[1], S[2]);
    prod[1] = gf_mul(Inv[0], S[1]) ^ gf_mul(Inv[1], S[3]);
    prod[2] = gf_mul(Inv[2], S[0]) ^ gf_mul(Inv[3], S[2]);
    prod[3] = gf_mul(Inv[2], S[1]) ^ gf_mul(Inv[3], S[3]);
    if (prod[0] != 1 || prod[1] != 0 || prod[2] != 0 || prod[3] != 1) {
        fprintf(stderr, "行约化逆矩阵错误\n");
        return 0;
    }
    return 1;
}
