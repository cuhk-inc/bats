/* 单核 CPU 基准，以及 BATS 处理码率随 CPU 占用的变化。
 *
 * 先把进程钉在一个逻辑核上，测这台机器满载时的整数吞吐和内存拷贝，
 * 再对 bats_encode / bats_recode / bats_decode 做同样的满载测量，
 * 然后用忙一段时间、睡一段时间的方式把占用压到目标百分比，看码率怎么掉。
 *
 * CPU 百分比都是相对这一个逻辑核：100 表示这个核满了，不是整机。
 * 度数分布用 bats_line 的 psi_line，K 与线上默认一致。
 */
#define _GNU_SOURCE

#include "bats_codec.h"
#include "psi_line.h"

#include <errno.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/resource.h>

enum {
    K_SRC = 128,
    T_UDP = 1370,
    T_LINE = 4096,
    MAX_BATCHES = 48,
    N_IN = BATS_M
};

static volatile uint64_t g_sink;

static double now_sec(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static double cpu_sec(void)
{
    struct rusage ru;

    getrusage(RUSAGE_SELF, &ru);
    return (double)ru.ru_utime.tv_sec + (double)ru.ru_utime.tv_usec * 1e-6 +
           (double)ru.ru_stime.tv_sec + (double)ru.ru_stime.tv_usec * 1e-6;
}

static int cpu_mhz(int cpu)
{
    char path[128];
    FILE *f;
    int khz;

    snprintf(path, sizeof(path),
             "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", cpu);
    f = fopen(path, "r");
    if (f == NULL) {
        return 0;
    }
    if (fscanf(f, "%d", &khz) != 1) {
        khz = 0;
    }
    fclose(f);
    return khz / 1000;
}

static void pin_cpu(int cpu)
{
    cpu_set_t set;

    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    if (sched_setaffinity(0, sizeof(set), &set) != 0) {
        perror("sched_setaffinity");
        exit(1);
    }
}

static void sleep_sec(double sec)
{
    struct timespec req;

    if (sec <= 0) {
        return;
    }
    req.tv_sec = (time_t)sec;
    req.tv_nsec = (long)((sec - (double)req.tv_sec) * 1e9);
    if (req.tv_nsec < 0) {
        req.tv_nsec = 0;
    }
    while (nanosleep(&req, &req) != 0) {
        if (errno != EINTR) {
            break;
        }
    }
}

static void read_model(char *dst, size_t n)
{
    FILE *f;
    char line[256];

    snprintf(dst, n, "unknown");
    f = fopen("/proc/cpuinfo", "r");
    if (f == NULL) {
        return;
    }
    while (fgets(line, sizeof(line), f) != NULL) {
        if (strncmp(line, "model name", 10) == 0) {
            char *colon = strchr(line, ':');
            size_t len;

            if (colon == NULL) {
                break;
            }
            colon++;
            while (*colon == ' ' || *colon == '\t') {
                colon++;
            }
            len = strlen(colon);
            while (len > 0 && (colon[len - 1] == '\n' || colon[len - 1] == '\r')) {
                colon[--len] = '\0';
            }
            snprintf(dst, n, "%s", colon);
            break;
        }
    }
    fclose(f);
}

static const char *virt_name(void)
{
    FILE *f;
    char buf[64];
    static char name[64];

    f = fopen("/sys/class/dmi/id/product_name", "r");
    if (f == NULL) {
        return "unknown";
    }
    if (fgets(buf, sizeof(buf), f) == NULL) {
        fclose(f);
        return "unknown";
    }
    fclose(f);
    buf[strcspn(buf, "\r\n")] = '\0';
    snprintf(name, sizeof(name), "%s", buf);
    return name;
}

/* 依赖链，编译器不能把循环删掉。返回每秒迭代次数。 */
static double baseline_integer(double seconds, double *cpu_pct, int *mhz)
{
    uint64_t x = 0x123456789abcdef0ull;
    uint64_t n = 0;
    double t0;
    double c0;
    double wall;
    double cpu;

    t0 = now_sec();
    c0 = cpu_sec();
    do {
        int i;

        for (i = 0; i < 1000; i++) {
            x = x * 6364136223846793005ull + 1442695040888963407ull;
            x ^= x >> 17;
        }
        n += 1000;
    } while (now_sec() - t0 < seconds);
    g_sink ^= x;
    wall = now_sec() - t0;
    cpu = cpu_sec() - c0;
    *cpu_pct = wall > 0 ? cpu / wall * 100.0 : 0;
    *mhz = cpu_mhz(0);
    return wall > 0 ? (double)n / wall : 0;
}

static double baseline_memcpy(double seconds, double *cpu_pct)
{
    const size_t bytes = 8u * 1024u * 1024u;
    uint8_t *a;
    uint8_t *b;
    uint64_t n = 0;
    double t0;
    double c0;
    double wall;
    double cpu;
    size_t i;

    a = malloc(bytes);
    b = malloc(bytes);
    if (a == NULL || b == NULL) {
        fprintf(stderr, "memcpy 基准内存不足\n");
        exit(1);
    }
    for (i = 0; i < bytes; i += 64) {
        a[i] = (uint8_t)i;
    }
    t0 = now_sec();
    c0 = cpu_sec();
    do {
        memcpy(b, a, bytes);
        n++;
        g_sink ^= b[n % bytes];
    } while (now_sec() - t0 < seconds);
    wall = now_sec() - t0;
    cpu = cpu_sec() - c0;
    *cpu_pct = wall > 0 ? cpu / wall * 100.0 : 0;
    free(a);
    free(b);
    return wall > 0 ? (double)n * (double)bytes / wall / 1e9 : 0;
}

typedef struct {
    uint8_t *src;
    uint8_t *dst;
    uint8_t *wire_payload;
    uint8_t *wire_coeff;
    BatsSymbol *symbols;
    int K;
    int T;
    int n_batches;
    int n_symbols;
    uint64_t seed;
    uint64_t recode_state;
    uint32_t batch_cursor;
    uint8_t coeff_out[BATS_M * BATS_M];
    uint8_t *payload_out;
} Job;

static int work_encode(Job *job)
{
    uint8_t coeff[BATS_M * BATS_M];
    uint8_t *payload = job->payload_out;

    if (bats_encode(job->src, job->K, job->T, job->seed, job->batch_cursor, coeff, payload) !=
        0) {
        return -1;
    }
    job->batch_cursor++;
    g_sink ^= payload[0];
    g_sink ^= payload[(size_t)job->T];
    return 0;
}

static int work_recode(Job *job)
{
    int n_out = 0;

    if (bats_recode(job->wire_coeff, job->wire_payload, N_IN, job->T, &job->recode_state,
                    job->coeff_out, job->payload_out, &n_out) != 0 ||
        n_out != BATS_M) {
        return -1;
    }
    g_sink ^= job->payload_out[0];
    return 0;
}

static int work_decode(Job *job)
{
    int ok;

    ok = bats_decode(job->symbols, job->n_symbols, job->K, job->T, job->seed, job->dst, NULL,
                     NULL);
    if (ok != 1) {
        return -1;
    }
    g_sink ^= job->dst[0];
    return 0;
}

static int prepare(Job *job, int T)
{
    int n;
    int j;
    int b;
    size_t payload_bytes;

    memset(job, 0, sizeof(*job));
    job->K = K_SRC;
    job->T = T;
    job->seed = 0xB175ull;
    job->recode_state = 1;
    job->src = calloc((size_t)job->K * (size_t)T, 1);
    job->dst = calloc((size_t)job->K * (size_t)T, 1);
    job->payload_out = malloc((size_t)BATS_M * (size_t)T);
    payload_bytes = (size_t)MAX_BATCHES * (size_t)BATS_M * (size_t)T;
    job->wire_payload = malloc(payload_bytes);
    job->wire_coeff = malloc((size_t)MAX_BATCHES * (size_t)BATS_M * BATS_M);
    job->symbols = calloc((size_t)MAX_BATCHES * BATS_M, sizeof(BatsSymbol));
    if (job->src == NULL || job->dst == NULL || job->payload_out == NULL ||
        job->wire_payload == NULL || job->wire_coeff == NULL || job->symbols == NULL) {
        fprintf(stderr, "工作集内存不足\n");
        return -1;
    }
    for (j = 0; j < job->K * T; j++) {
        job->src[j] = (uint8_t)(j * 17 + 3);
    }

    for (n = 12; n <= MAX_BATCHES; n += 2) {
        for (b = 0; b < n; b++) {
            uint8_t *coeff = job->wire_coeff + ((size_t)b * BATS_M) * BATS_M;
            uint8_t *payload =
                job->wire_payload + ((size_t)b * BATS_M) * (size_t)T;

            if (bats_encode(job->src, job->K, T, job->seed, (uint32_t)b, coeff, payload) !=
                0) {
                return -1;
            }
            for (j = 0; j < BATS_M; j++) {
                BatsSymbol *sym = &job->symbols[(size_t)b * BATS_M + (size_t)j];

                sym->batch_id = (uint32_t)b;
                memcpy(sym->coeff, coeff + (size_t)j * BATS_M, BATS_M);
                sym->payload = payload + (size_t)j * (size_t)T;
            }
        }
        memset(job->dst, 0, (size_t)job->K * (size_t)T);
        if (bats_decode(job->symbols, n * BATS_M, job->K, T, job->seed, job->dst, NULL, NULL) ==
                1 &&
            memcmp(job->src, job->dst, (size_t)job->K * (size_t)T) == 0) {
            job->n_batches = n;
            job->n_symbols = n * BATS_M;
            return 0;
        }
    }
    fprintf(stderr, "T=%d 在 %d 个 batch 内译码失败\n", T, MAX_BATCHES);
    return -1;
}

static void release_job(Job *job)
{
    free(job->src);
    free(job->dst);
    free(job->payload_out);
    free(job->wire_payload);
    free(job->wire_coeff);
    free(job->symbols);
}

static double calibrate(int (*fn)(Job *), Job *job, double *per_call)
{
    int n = 0;
    double t0;
    double elapsed;

    t0 = now_sec();
    do {
        if (fn(job) != 0) {
            return -1;
        }
        n++;
    } while (n < 5 || now_sec() - t0 < 0.05);
    elapsed = now_sec() - t0;
    *per_call = elapsed / (double)n;
    return 0;
}

static int run_paced(int (*fn)(Job *), Job *job, int target_pct, double seconds,
                     double per_call, uint64_t *units, double *wall, double *cpu)
{
    double t0;
    double c0;
    int burst;
    uint64_t n = 0;

    if (per_call <= 0) {
        return -1;
    }
    burst = (int)(0.008 / per_call);
    if (burst < 1) {
        burst = 1;
    }
    if (burst > 10000) {
        burst = 10000;
    }
    t0 = now_sec();
    c0 = cpu_sec();
    if (target_pct >= 100) {
        while (now_sec() - t0 < seconds) {
            int i;

            for (i = 0; i < burst; i++) {
                if (fn(job) != 0) {
                    return -1;
                }
            }
            n += (uint64_t)burst;
        }
    } else {
        double duty = (double)target_pct / 100.0;

        while (now_sec() - t0 < seconds) {
            int i;
            double sleep_for;

            for (i = 0; i < burst; i++) {
                if (fn(job) != 0) {
                    return -1;
                }
            }
            n += (uint64_t)burst;
            sleep_for = (double)burst * per_call * (1.0 - duty) / duty;
            sleep_sec(sleep_for);
        }
    }
    *units = n;
    *wall = now_sec() - t0;
    *cpu = cpu_sec() - c0;
    return 0;
}

static void report_row(const char *op, int target, double actual, double wire_mbps,
                       double goodput_mbps)
{
    printf("%-8s %6d %10.1f %12.2f %14.2f %14.1f\n", op, target, actual, wire_mbps,
           goodput_mbps, wire_mbps * 8.0);
}

static int bench_one(const char *op, int (*fn)(Job *), Job *job, int target, double seconds,
                     double wire_bytes, double goodput_bytes)
{
    double per_call = 0;
    uint64_t units = 0;
    double wall = 0;
    double cpu = 0;
    double actual;
    double wire_mbps;
    double goodput_mbps;

    if (calibrate(fn, job, &per_call) != 0) {
        fprintf(stderr, "%s 校准失败\n", op);
        return -1;
    }
    if (run_paced(fn, job, target, seconds, per_call, &units, &wall, &cpu) != 0) {
        fprintf(stderr, "%s 在目标 CPU %d%% 失败\n", op, target);
        return -1;
    }
    actual = wall > 0 ? cpu / wall * 100.0 : 0;
    wire_mbps = wall > 0 ? (double)units * wire_bytes / wall / 1e6 : 0;
    goodput_mbps = wall > 0 ? (double)units * goodput_bytes / wall / 1e6 : 0;
    report_row(op, target, actual, wire_mbps, goodput_mbps);
    fflush(stdout);
    return 0;
}

static int bench_T(int T, double seconds)
{
    static const int targets[] = {100, 75, 50, 25};
    Job job;
    int i;
    double encode_goodput;
    double encode_wire;
    double recode_wire;
    double decode_goodput;
    double decode_wire;

    fprintf(stderr, "准备 T=%d ...\n", T);
    if (prepare(&job, T) != 0) {
        return -1;
    }
    encode_wire = (double)BATS_M * (double)T;
    encode_goodput = (double)job.K * (double)T / (double)job.n_batches;
    recode_wire = (double)BATS_M * (double)T;
    decode_goodput = (double)job.K * (double)T;
    decode_wire = (double)job.n_symbols * (double)T;

    printf("\n# K=%d T=%d batches_per_object=%d symbols=%d psi=bats_line\n", job.K, T,
           job.n_batches, job.n_symbols);
    printf("# wire_MBps 是载荷字节/秒，不含包头。encode/recode 按每次写出的 %d 个包计。\n",
           BATS_M);
    printf("# encode goodput 把 %d 次 encode 合成一个 %d*%d 字节的对象。\n", job.n_batches,
           job.K, T);
    printf("# decode goodput 是每次成功还原的源字节。decode wire 是喂进译码器的载荷。\n");
    printf("%-8s %6s %10s %12s %14s %14s\n", "op", "target", "actual_cpu", "wire_MBps",
           "goodput_MBps", "wire_Mbitps");
    for (i = 0; i < 4; i++) {
        int target = targets[i];

        if (bench_one("encode", work_encode, &job, target, seconds, encode_wire,
                      encode_goodput) != 0 ||
            bench_one("recode", work_recode, &job, target, seconds, recode_wire,
                      recode_wire) != 0 ||
            bench_one("decode", work_decode, &job, target, seconds, decode_wire,
                      decode_goodput) != 0) {
            release_job(&job);
            return -1;
        }
    }
    release_job(&job);
    return 0;
}

int main(void)
{
    char model[160];
    const int cpu = 0;
    const double base_sec = 2.0;
    const double cell_sec = 1.5;
    double int_ops;
    double int_cpu;
    double copy_gbps;
    double copy_cpu;
    int mhz_idle;
    int mhz_load;
    int nproc;

    pin_cpu(cpu);
    read_model(model, sizeof(model));
    nproc = (int)sysconf(_SC_NPROCESSORS_ONLN);
    mhz_idle = cpu_mhz(cpu);
    bats_init();
    if (!psi_set(psi_line_w, psi_line_D)) {
        fprintf(stderr, "psi_set 失败\n");
        return 1;
    }

    printf("# host CPU benchmark, pinned to logical cpu %d\n", cpu);
    printf("model=%s\n", model);
    printf("product=%s\n", virt_name());
    printf("logical_cpus=%d\n", nproc);
    printf("idle_mhz=%d\n", mhz_idle);

    int_ops = baseline_integer(base_sec, &int_cpu, &mhz_load);
    copy_gbps = baseline_memcpy(base_sec, &copy_cpu);
    printf("integer_ops_per_s=%.0f\n", int_ops);
    printf("integer_cpu_pct_one_core=%.1f\n", int_cpu);
    printf("memcpy_GBps=%.2f\n", copy_gbps);
    printf("memcpy_cpu_pct_one_core=%.1f\n", copy_cpu);
    printf("load_mhz=%d\n", mhz_load);
    printf("# 上面整数和 memcpy 是这台机器单核满载的基准。下面 CPU 百分比同样相对这一个核。\n");
    fflush(stdout);

    if (bench_T(T_UDP, cell_sec) != 0 || bench_T(T_LINE, cell_sec) != 0) {
        return 1;
    }
    printf("sink=%llu\n", (unsigned long long)g_sink);
    return 0;
}
