#define _POSIX_C_SOURCE 200809L
/* 游走一 / 游走零。
 * 游走一：整个源里只有一个比特是 1。
 * 游走零：整个源里只有一个比特是 0，其余都是 1。
 * 另外各跑一遍按字节循环的整段图案，长度用线上的 T。
 * 路径是 bats_encode，再可选一跳或两跳 bats_recode，最后 bats_decode。
 * 方程是否可解只跟种子和 batch 有关，跟载荷无关，所以同一组参数只搜索一次 batch 数。
 */
#include "bats_codec.h"
#include "psi_line.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { HOP_SEED_1 = 0x11, HOP_SEED_2 = 0x22, CODE_SEED = 0xB175 };

static int g_fail;

static void report_mismatch(const char *name, const uint8_t *src, const uint8_t *dst, int T);

static double now_sec(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void fill_cycle_one(uint8_t *src, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++) {
        src[i] = (uint8_t)(1u << (i & 7u));
    }
}

static void fill_cycle_zero(uint8_t *src, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++) {
        src[i] = (uint8_t)~(1u << (i & 7u));
    }
}

static void fill_impulse(uint8_t *src, size_t n, size_t bit, int walking_one)
{
    size_t byte = bit / 8u;
    uint8_t mask = (uint8_t)(1u << (bit & 7u));

    if (walking_one) {
        memset(src, 0, n);
        if (byte < n) {
            src[byte] = mask;
        }
    } else {
        memset(src, 0xff, n);
        if (byte < n) {
            src[byte] = (uint8_t)~mask;
        }
    }
}

/* 1 还原正确，0 还解不开，-1 解出来但和原文不同，-2 接口失败。 */
static int roundtrip(const uint8_t *src, uint8_t *dst, int K, int T, int n_batches, int hops,
                     int keep)
{
    size_t pkt = (size_t)BATS_M * (size_t)T;
    size_t coeff_n = (size_t)BATS_M * (size_t)BATS_M;
    uint8_t *arena;
    uint8_t *coeffs;
    uint8_t *buf[2];
    uint8_t *cbuf[2];
    BatsSymbol *syms;
    uint64_t hop_state[2];
    int batch;
    int rc;

    if (n_batches < 1 || keep < 1 || keep > BATS_M || hops < 0 || hops > 2) {
        return -2;
    }
    arena = malloc((size_t)n_batches * pkt);
    coeffs = malloc((size_t)n_batches * coeff_n);
    buf[0] = malloc(pkt);
    buf[1] = malloc(pkt);
    cbuf[0] = malloc(coeff_n);
    cbuf[1] = malloc(coeff_n);
    syms = malloc((size_t)n_batches * (size_t)BATS_M * sizeof(BatsSymbol));
    if (arena == NULL || coeffs == NULL || buf[0] == NULL || buf[1] == NULL || cbuf[0] == NULL ||
        cbuf[1] == NULL || syms == NULL) {
        free(arena);
        free(coeffs);
        free(buf[0]);
        free(buf[1]);
        free(cbuf[0]);
        free(cbuf[1]);
        free(syms);
        return -2;
    }
    hop_state[0] = HOP_SEED_1;
    hop_state[1] = HOP_SEED_2;
    for (batch = 0; batch < n_batches; batch++) {
        int hop;
        int cur = 0;
        int n_in = BATS_M;

        if (bats_encode(src, K, T, CODE_SEED, (uint32_t)batch, cbuf[0], buf[0]) != 0) {
            rc = -2;
            goto out;
        }
        for (hop = 0; hop < hops; hop++) {
            int nxt = cur ^ 1;
            int use = keep < n_in ? keep : n_in;
            int n_out = 0;

            if (bats_recode(cbuf[cur], buf[cur], use, T, &hop_state[hop], cbuf[nxt], buf[nxt],
                            &n_out) != 0 ||
                n_out != BATS_M) {
                rc = -2;
                goto out;
            }
            cur = nxt;
            n_in = n_out;
        }
        memcpy(coeffs + (size_t)batch * coeff_n, cbuf[cur], coeff_n);
        memcpy(arena + (size_t)batch * pkt, buf[cur], pkt);
    }
    for (batch = 0; batch < n_batches; batch++) {
        int j;

        for (j = 0; j < BATS_M; j++) {
            BatsSymbol *s = &syms[(size_t)batch * BATS_M + (size_t)j];

            s->batch_id = (uint32_t)batch;
            memcpy(s->coeff, coeffs + ((size_t)batch * BATS_M + (size_t)j) * BATS_M, BATS_M);
            s->payload = arena + (size_t)batch * pkt + (size_t)j * (size_t)T;
        }
    }
    memset(dst, 0x5a, (size_t)K * (size_t)T);
    rc = bats_decode(syms, n_batches * BATS_M, K, T, CODE_SEED, dst, NULL, NULL);
    if (rc == 1 && memcmp(dst, src, (size_t)K * (size_t)T) != 0) {
        rc = -1;
    }
out:
    free(syms);
    free(cbuf[1]);
    free(cbuf[0]);
    free(buf[1]);
    free(buf[0]);
    free(coeffs);
    free(arena);
    return rc;
}

static int find_batches(const uint8_t *src, uint8_t *dst, int K, int T, int hops, int keep,
                        int start, int cap, int step)
{
    int n;

    if (step < 1) {
        step = 1;
    }
    for (n = start; n <= cap; n += step) {
        int rc = roundtrip(src, dst, K, T, n, hops, keep);

        if (rc == 1) {
            return n;
        }
        if (rc == -1) {
            report_mismatch("译码结果", src, dst, T);
            return -2;
        }
        if (rc < 0) {
            return -1;
        }
    }
    return -1;
}

static void report_mismatch(const char *name, const uint8_t *src, const uint8_t *dst, int T)
{
    size_t i = 0;

    while (src[i] == dst[i]) {
        i++;
    }
    fprintf(stderr, "失败 %s：包 %d 字节 %d 期望 %02X 得到 %02X\n", name, T > 0 ? (int)(i / (size_t)T) : 0,
            T > 0 ? (int)(i % (size_t)T) : 0, src[i], dst[i]);
}

static void check_buffer(const char *name, const uint8_t *src, int K, int T, int hops, int keep,
                         int start, int cap, int step)
{
    uint8_t *dst = malloc((size_t)K * (size_t)T);
    double t0 = now_sec();
    int n;

    if (dst == NULL) {
        fprintf(stderr, "失败 %s：内存不足\n", name);
        g_fail++;
        return;
    }
    n = find_batches(src, dst, K, T, hops, keep, start, cap, step);
    if (n == -2) {
        fprintf(stderr, "失败 %s\n", name);
        g_fail++;
        free(dst);
        return;
    }
    if (n < 0) {
        fprintf(stderr, "失败 %s：%d 个 batch 仍不能还原\n", name, cap);
        g_fail++;
        free(dst);
        return;
    }
    printf("通过 %s  batches=%d  %.2fs\n", name, n, now_sec() - t0);
    fflush(stdout);
    free(dst);
}

static void walk_bits(const char *name, int K, int T, int hops, int keep, const int *packets,
                      int n_packets, const int *bytes, int n_bytes)
{
    size_t obj = (size_t)K * (size_t)T;
    uint8_t *src = malloc(obj);
    uint8_t *dst = malloc(obj);
    double t0 = now_sec();
    int n_batches = -1;
    int packet_n = n_packets > 0 ? n_packets : K;
    int pi;
    int npos = 0;
    int seen = 0;

    if (src == NULL || dst == NULL) {
        fprintf(stderr, "失败 %s：内存不足\n", name);
        g_fail++;
        free(src);
        free(dst);
        return;
    }
    for (pi = 0; pi < packet_n; pi++) {
        int bi;
        int packet = n_packets > 0 ? packets[pi] : pi;
        int byte_n = n_bytes > 0 ? n_bytes : T;

        if (packet < 0 || packet >= K) {
            fprintf(stderr, "失败 %s：包号 %d 超出 K=%d\n", name, packet, K);
            g_fail++;
            break;
        }
        for (bi = 0; bi < byte_n; bi++) {
            int byte = n_bytes > 0 ? bytes[bi] : bi;
            int bit;

            if (byte < 0 || byte >= T) {
                fprintf(stderr, "失败 %s：字节 %d 超出 T=%d\n", name, byte, T);
                g_fail++;
                goto done;
            }
            for (bit = 0; bit < 8; bit++) {
                size_t pos = ((size_t)packet * (size_t)T + (size_t)byte) * 8u + (size_t)bit;
                int pattern;

                for (pattern = 0; pattern < 2; pattern++) {
                    int rc;

                    fill_impulse(src, obj, pos, pattern == 0);
                    if (n_batches < 0) {
                        n_batches = find_batches(src, dst, K, T, hops, keep, K > 32 ? 16 : 1,
                                                 K > 32 ? 160 : 32, K > 32 ? 8 : 1);
                        if (n_batches == -2) {
                            fprintf(stderr, "失败 %s\n", name);
                            g_fail++;
                            goto done;
                        }
                        if (n_batches < 0) {
                            fprintf(stderr, "失败 %s：找不到能还原的 batch 数\n", name);
                            g_fail++;
                            goto done;
                        }
                    }
                    rc = roundtrip(src, dst, K, T, n_batches, hops, keep);
                    seen++;
                    if (rc != 1) {
                        fprintf(stderr,
                                "失败 %s：%s 包 %d 字节 %d 比特 %d（batch=%d，返回 %d）\n", name,
                                pattern == 0 ? "游走一" : "游走零", packet, byte, bit, n_batches, rc);
                        if (rc == -1) {
                            report_mismatch(name, src, dst, T);
                        }
                        g_fail++;
                        goto done;
                    }
                }
                npos++;
            }
        }
    }
    printf("通过 %s  位置=%d  图案=%d  batches=%d  %.2fs\n", name, npos, seen, n_batches,
           now_sec() - t0);
    fflush(stdout);
done:
    free(src);
    free(dst);
}

static void check_cycles(const char *tag, int K, int T, int hops, int keep, int start, int cap,
                         int step)
{
    size_t obj = (size_t)K * (size_t)T;
    uint8_t *src = malloc(obj);
    char name[128];

    if (src == NULL) {
        fprintf(stderr, "失败 %s：内存不足\n", tag);
        g_fail++;
        return;
    }
    fill_cycle_one(src, obj);
    snprintf(name, sizeof(name), "%s 整段游走一 hops=%d keep=%d K=%d T=%d", tag, hops, keep, K, T);
    check_buffer(name, src, K, T, hops, keep, start, cap, step);
    fill_cycle_zero(src, obj);
    snprintf(name, sizeof(name), "%s 整段游走零 hops=%d keep=%d K=%d T=%d", tag, hops, keep, K, T);
    check_buffer(name, src, K, T, hops, keep, start, cap, step);
    free(src);
}

int main(void)
{
    const int edge_pk[] = {0, 1, 15, 16, 64, 127};
    const int edge_bytes[] = {0, 1, 14, 15, 16, 17, 30, 31};
    const int wide_pk[] = {0, 1, 127};
    const int wide_bytes[] = {0, 1, 15, 16, 17, 1368, 1369};
    double t0 = now_sec();

    bats_init();
    printf("默认 Ψ，编码后两跳 recode 再译码\n");
    {
        const int only = 0;

        walk_bits("K=1 T=1", 1, 1, 2, 8, &only, 1, NULL, 0);
        walk_bits("K=1 T=48", 1, 48, 0, 8, &only, 1, NULL, 0);
        walk_bits("K=1 T=48 一跳", 1, 48, 1, 8, &only, 1, NULL, 0);
        walk_bits("K=1 T=48 两跳", 1, 48, 2, 8, &only, 1, NULL, 0);
    }
    walk_bits("K=8 T=17 两跳", 8, 17, 2, 8, NULL, 0, NULL, 0);
    walk_bits("K=128 T=32 边界包和字节", 128, 32, 2, 8, edge_pk, 6, edge_bytes, 8);
    check_cycles("默认 Ψ", 128, 32, 0, 8, 24, 160, 8);
    check_cycles("默认 Ψ", 128, 32, 2, 8, 24, 160, 8);
    check_cycles("默认 Ψ", 128, 32, 2, 7, 24, 200, 8);
    check_cycles("默认 Ψ", 128, 1370, 2, 8, 24, 160, 8);
    check_cycles("默认 Ψ", 128, 1370, 2, 7, 24, 200, 8);
    walk_bits("K=128 T=1370 边界", 128, 1370, 2, 8, wide_pk, 3, wide_bytes, 7);

    if (!psi_set(psi_line_w, psi_line_D)) {
        fprintf(stderr, "失败：无法装入线上的 Ψ\n");
        return 1;
    }
    printf("线上 Ψ，编码后两跳 recode 再译码\n");
    check_cycles("线上 Ψ", 128, 1370, 2, 8, 16, 40, 2);
    check_cycles("线上 Ψ", 128, 1370, 2, 7, 16, 48, 2);
    walk_bits("线上 Ψ K=128 T=1370 边界", 128, 1370, 2, 8, wide_pk, 3, wide_bytes, 7);

    if (g_fail != 0) {
        fprintf(stderr, "游走图案未通过，失败 %d 组\n", g_fail);
        return 1;
    }
    printf("游走一和游走零全部通过  %.2fs\n", now_sec() - t0);
    return 0;
}
