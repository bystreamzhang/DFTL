/*
构造数据：
gcc -O2 -march=native -std=c11 -Wall -Wextra -o build_dataset2-seq build_dataset2-seq.c
./build_dataset2-seq
*/
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>

#define IO_NUM_WRITES 250000000ULL
#define IO_NUM_RANDOM 1000000ULL
#define LPN_MAX (1ULL << 36)
#define PPN_MAX (1ULL << 36)
#define REPORT_INTERVAL 10000000ULL

// 哈希表参数（按需调小以适配内存）
#define BUCKET_COUNT 536870912U  // 2^29 桶，约 2GB（每桶4字节索引）
#define NODE_CAPACITY 320000000U // 节点池容量（预估3.2e8），约 ~24B/节点

// 索引类型：-1 表示空
typedef int32_t idx_t;

typedef struct
{
    uint64_t key; // lpn
    uint64_t val; // ppn
    idx_t next;   // 下一个节点索引，-1表示无
} Node;

static idx_t *buckets = NULL; // 大小 BUCKET_COUNT，存链表头索引
static Node *nodes = NULL;    // 节点池
static uint32_t node_top = 0; // 已用节点计数

// xorshift64* PRNG
static inline uint64_t rng_next(uint64_t *s)
{
    uint64_t x = *s;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    *s = x;
    return x * 2685821657736338717ULL;
}
static inline uint64_t rand_below(uint64_t *s, uint64_t n)
{
    return rng_next(s) % n;
}

static inline uint32_t hash64(uint64_t x)
{
    // 64->32 bit mix，然后取桶索引
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return (uint32_t)x;
}

// 查找：存在则返回1并输出val；否则0
static int map_get(uint64_t k, uint64_t *out_v)
{
    uint32_t h = hash64(k) & (BUCKET_COUNT - 1); // BUCKET_COUNT为2的幂
    idx_t i = buckets[h];
    while (i != -1)
    {
        if (nodes[i].key == k)
        {
            *out_v = nodes[i].val;
            return 1;
        }
        i = nodes[i].next;
    }
    return 0;
}

// 插入或更新
static void map_put(uint64_t k, uint64_t v)
{
    uint32_t h = hash64(k) & (BUCKET_COUNT - 1);
    idx_t i = buckets[h];
    while (i != -1)
    {
        if (nodes[i].key == k)
        {
            nodes[i].val = v;
            return;
        }
        i = nodes[i].next;
    }
    // 头插新节点
    if (node_top >= NODE_CAPACITY)
    {
        fprintf(stderr, "[Error] Node pool exhausted at %u\n", node_top);
        exit(2);
    }
    idx_t ni = (idx_t)node_top++;
    nodes[ni].key = k;
    nodes[ni].val = v;
    nodes[ni].next = buckets[h];
    buckets[h] = ni;
}

int main()
{
    // 分配内存：注意内存占用
    buckets = (idx_t *)malloc((size_t)BUCKET_COUNT * sizeof(idx_t));
    nodes = (Node *)malloc((size_t)NODE_CAPACITY * sizeof(Node));
    if (!buckets || !nodes)
    {
        fprintf(stderr, "[Error] cannot allocate hash structures. Reduce BUCKET_COUNT/NODE_CAPACITY.\n");
        return 1;
    }
    for (uint64_t i = 0; i < BUCKET_COUNT; i++)
        buckets[i] = -1;

    FILE *trace = fopen("trace-seq.txt", "w");
    FILE *readout = fopen("read_result-seq.txt", "w");
    if (!trace || !readout)
    {
        fprintf(stderr, "[Error] cannot open output files\n");
        return 1;
    }
    static char buf1[1 << 20], buf2[1 << 20];
    setvbuf(trace, buf1, _IOFBF, sizeof(buf1));
    setvbuf(readout, buf2, _IOFBF, sizeof(buf2));

    uint64_t seed = 0x9e3779b97f4a7c15ULL ^ (uint64_t)time(NULL);

    const uint64_t total_io = IO_NUM_WRITES + IO_NUM_RANDOM;
    fprintf(trace, "io count\n%llu\n", (unsigned long long)total_io);

    // 写段：lpn=1..IO_NUM_WRITES
    for (uint64_t i = 0; i < IO_NUM_WRITES; i++)
    {
        uint64_t lpn = i;
        uint64_t ppn = rand_below(&seed, PPN_MAX);
        map_put(lpn, ppn);
        fprintf(trace, "1 %llu %llu\n",
                (unsigned long long)lpn,
                (unsigned long long)ppn);

        if ((i + 1) % REPORT_INTERVAL == 0)
        {
            fprintf(stderr, "[Write] %llu / %llu (nodes=%u)\n",
                    (unsigned long long)(i + 1),
                    (unsigned long long)IO_NUM_WRITES,
                    node_top);
        }
        if ((i + 1) % 5000000ULL == 0)
            fflush(trace);
    }
    fprintf(stderr, "[Write] Done. Enter random phase...\n");
    fflush(trace);

    // 随机IO段
    for (uint64_t i = 0; i < IO_NUM_RANDOM; i++)
    {
        if ((i + 1) % 5000ULL == 0 || i <= 10)
            fprintf(stderr, "[Last 1M] %llu / %llu (nodes=%u)\n",
                    (unsigned long long)(i + 1),
                    (unsigned long long)IO_NUM_RANDOM,
                    node_top);
        uint64_t op = rand_below(&seed, 2);
        //uint64_t lpn = rand_below(&seed, IO_NUM_WRITES);
        uint64_t lpn = i;
        uint64_t ppn = rand_below(&seed, PPN_MAX);

        if (op == 1)
        {
            map_put(lpn, ppn);
            fprintf(trace, "1 %llu %llu\n",
                    (unsigned long long)lpn,
                    (unsigned long long)ppn);
        }
        else
        {
            uint64_t val;
            int ok = map_get(lpn, &val);
            uint64_t out = ok ? val : (uint64_t)-1;
            fprintf(trace, "0 %llu %llu\n",
                    (unsigned long long)lpn,
                    (unsigned long long)out);
            fprintf(readout, "%llu\n", (unsigned long long)out);
        }

        if ((i + 1) % REPORT_INTERVAL == 0)
        {
            fprintf(stderr, "[RandIO] %llu / %llu (nodes=%u)\n",
                    (unsigned long long)(i + 1),
                    (unsigned long long)IO_NUM_RANDOM,
                    node_top);
        }
        if ((i + 1) % 5000ULL == 0)
            fflush(trace);
    }

    fclose(trace);
    fclose(readout);
    fprintf(stderr, "Done. Total IO lines: %llu, nodes used: %u\n",
            (unsigned long long)total_io, node_top);
    return 0;
}