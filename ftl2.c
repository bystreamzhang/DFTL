#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <errno.h>
#include <inttypes.h>
#include "ftl.h"

/*
DFTL-lite（按需加载的映射页 + LRU页缓存）

设计要点：
- 将LBA空间拆分为固定大小的映射页（4KB），页内为定长条目（默认8字节ppn）。
- 未分配的映射页读为0；首次写入时再分配页内存（Copy-On-Write到私有页）。
- LRU缓存维护最近使用的映射页；淘汰时把私有页指针“交还”到镜像目录，避免多余复制。
- 内存统计通过统一封装的FTLMalloc/FTLFree完成。

注意：
- 本实现聚焦于运行内存受控、读写路径清晰；不包含盘上持久化、日志与崩溃恢复。
*/

/* 可调参数 */

// 单条映射条目字节数：默认使用8字节（u64）
#define ENTRY_U64

/* 映射页大小：4KB */
#define MAP_PAGE_BYTES 4096u

/* 页缓存容量（单位：映射页个数），可按需调节 */
#ifndef MCACHE_PAGES
#define MCACHE_PAGES 8192u  /* 约占用32MB缓存页，另加索引开销 */
#endif

/* LBA 可访问的上限（不含），用于越界保护；按数据集规模配置 */
#ifndef LBA_MAX_PLUS1
#define LBA_MAX_PLUS1 25000000ull
#endif

/* 输出缓冲大小：适度增大可降低fprintf开销 */
#ifndef OUTPUT_BUFFER_BYTES
#define OUTPUT_BUFFER_BYTES (4u << 20) /* 4MB */
#endif

/* -------- 条目规格选择 -------- */
#if defined(ENTRY_U64)
#define ENTRY_BYTES 8u
#else
#define ENTRY_BYTES 8u
#endif

/* 每页条目数（8字节=512项/页） */
#define ENTRIES_PER_PAGE (MAP_PAGE_BYTES / ENTRY_BYTES)

/* -------- 内存统计封装 -------- */

static uint64_t g_mem_used = 0;
static uint64_t g_mem_peak = 0;

static inline void mem_on_alloc(size_t sz)
{
    g_mem_used += (uint64_t)sz;
    if (g_mem_used > g_mem_peak) g_mem_peak = g_mem_used;
}
static inline void mem_on_free(size_t sz)
{
    g_mem_used -= (uint64_t)sz;
}

static void* FTLMalloc(size_t size)
{
    void *p = malloc(size);
    if (!p) {
        fprintf(stderr, "malloc %zu failed: %s\n", size, strerror(errno));
        exit(EXIT_FAILURE);
    }
    mem_on_alloc(size);
    return p;
}

static void* FTLCalloc(size_t n, size_t size)
{
    void *p = calloc(n, size);
    if (!p) {
        fprintf(stderr, "calloc %zu failed: %s\n", n * size, strerror(errno));
        exit(EXIT_FAILURE);
    }
    mem_on_alloc(n * size);
    return p;
}

static void FTLFree(void *p, size_t size)
{
    if (p) {
        free(p);
        mem_on_free(size);
    }
}

/* -------- 条目读写 -------- */

static inline void entry_store(uint8_t *base, uint32_t idx, uint64_t v)
{
#if ENTRY_BYTES == 8u
    ((uint64_t *)base)[idx] = v;
#else
    /* 预留扩展：6字节压缩等 */
    ((uint64_t *)base)[idx] = v;
#endif
}

static inline uint64_t entry_load(const uint8_t *base, uint32_t idx)
{
#if ENTRY_BYTES == 8u
    return ((const uint64_t *)base)[idx];
#else
    return ((const uint64_t *)base)[idx];
#endif
}

/* -------- lba 映射到 映射页号/页内偏移 -------- */

static inline uint64_t lba_to_mpn(uint64_t lba)
{
#if ENTRY_BYTES == 8u
    return lba >> 9;  /* 512项/页，右移9位 */
#else
    return lba / (uint64_t)ENTRIES_PER_PAGE;
#endif
}

static inline uint32_t lba_to_off(uint64_t lba)
{
#if ENTRY_BYTES == 8u
    return (uint32_t)(lba & 511u);
#else
    return (uint32_t)(lba % (uint64_t)ENTRIES_PER_PAGE);
#endif
}

/* -------- 缓存与索引结构 -------- */

typedef struct CacheNode {
    uint64_t mpn;      /* 映射页号 */
    uint8_t *page;     /* 页数据指针（可能指向共享零页或私有页） */
    uint8_t dirty;     /* 是否被修改过（当前版本不做落盘，仅用于调试观察） */
    uint8_t owned;     /* 是否为私有页（1=可写，0=共享零页只读） */
    struct CacheNode *prev;
    struct CacheNode *next;
    struct CacheNode *hnext;
} CacheNode;

typedef struct {
    CacheNode **buckets;
    uint32_t mask;     /* 桶数-1（2的幂） */
} CacheHash;

typedef struct {
    /* 镜像目录：mpn -> 私有页指针；NULL 表示尚未分配（读返回0） */
    uint8_t **page_dir;
    uint64_t mpn_count;

    /* 共享零页（只读，返回未写过的0值） */
    uint8_t *zero_page;

    /* LRU缓存 */
    CacheNode *nodes;
    uint32_t capacity;
    CacheNode *lru_head;
    CacheNode *lru_tail;
    CacheHash hash;
} DFTL;

static DFTL *g = NULL;

/* -------- 简易哈希与LRU -------- */

static uint32_t next_pow2(uint32_t x)
{
    if (x <= 1) return 1;
    --x;
    x |= x >> 1;
    x |= x >> 2;
    x |= x >> 4;
    x |= x >> 8;
    x |= x >> 16;
    return x + 1;
}

static uint32_t hash_mpn(uint64_t mpn)
{
    /* 64-bit mix 再截断 */
    mpn ^= mpn >> 33;
    mpn *= 0xff51afd7ed558ccdULL;
    mpn ^= mpn >> 33;
    mpn *= 0xc4ceb9fe1a85ec53ULL;
    mpn ^= mpn >> 33;
    return (uint32_t)mpn;
}

static void hash_init(CacheHash *h, uint32_t expect)
{
    const uint32_t n = next_pow2(expect ? expect * 2u : 8u);
    h->mask = n - 1u;
    h->buckets = (CacheNode **)FTLCalloc(n, sizeof(CacheNode *));
}

static void hash_destroy(CacheHash *h)
{
    const size_t bytes = (size_t)(h->mask + 1u) * sizeof(CacheNode *);
    FTLFree(h->buckets, bytes);
    h->buckets = NULL;
    h->mask = 0;
}

static CacheNode* hash_find(CacheHash *h, uint64_t mpn)
{
    CacheNode *p = h->buckets[hash_mpn(mpn) & h->mask];
    while (p) {
        if (p->mpn == mpn) return p;
        p = p->hnext;
    }
    return NULL;
}

static void hash_insert(CacheHash *h, CacheNode *n)
{
    const uint32_t idx = hash_mpn(n->mpn) & h->mask;
    n->hnext = h->buckets[idx];
    h->buckets[idx] = n;
}

static void hash_remove(CacheHash *h, CacheNode *n)
{
    const uint32_t idx = hash_mpn(n->mpn) & h->mask;
    CacheNode *p = h->buckets[idx];
    CacheNode *prev = NULL;
    while (p) {
        if (p == n) {
            if (prev) prev->hnext = p->hnext;
            else h->buckets[idx] = p->hnext;
            n->hnext = NULL;
            return;
        }
        prev = p;
        p = p->hnext;
    }
}

static void lru_init(DFTL *d)
{
    d->lru_head = d->lru_tail = NULL;
}

static void lru_unlink(DFTL *d, CacheNode *n)
{
    if (n->prev) n->prev->next = n->next;
    else d->lru_head = n->next;
    if (n->next) n->next->prev = n->prev;
    else d->lru_tail = n->prev;
    n->prev = n->next = NULL;
}

static void lru_push_front(DFTL *d, CacheNode *n)
{
    n->prev = NULL;
    n->next = d->lru_head;
    if (d->lru_head) d->lru_head->prev = n;
    else d->lru_tail = n;
    d->lru_head = n;
}

static CacheNode* lru_pop_back(DFTL *d)
{
    CacheNode *t = d->lru_tail;
    if (!t) return NULL;
    lru_unlink(d, t);
    return t;
}

/* 将缓存节点持有的“私有页”归还给目录，避免内存泄漏 */
static void return_owned_page_if_any(DFTL *d, CacheNode *n)
{
    if (!n->owned) return; /* 指向零页，无需处理 */

    uint8_t *old = d->page_dir[n->mpn];
    if (old && old != n->page) {
        /* 保护式释放：不期望出现，但不妨做一次兜底 */
        FTLFree(old, MAP_PAGE_BYTES);
    }
    d->page_dir[n->mpn] = n->page;
    n->page = NULL;
    n->owned = 0;
    n->dirty = 0;
}

/* 选择一个缓存节点：优先淘汰末尾，否则挑选尚未绑定的节点 */
static CacheNode* cache_grab_node(DFTL *d)
{
    CacheNode *victim = lru_pop_back(d);
    if (victim) {
        hash_remove(&d->hash, victim);
        return_owned_page_if_any(d, victim);
        return victim;
    }

    /* 查找从未使用过的节点 */
    for (uint32_t i = 0; i < d->capacity; ++i) {
        CacheNode *c = &d->nodes[i];
        if (c->page == NULL && c->hnext == NULL && c->prev == NULL && c->next == NULL) {
            return c;
        }
    }
    /* 正常不会到达 */
    return NULL;
}

static void bind_node(DFTL *d, CacheNode *n, uint64_t mpn, uint8_t *page_ptr, uint8_t owned, uint8_t dirty)
{
    n->mpn = mpn;
    n->page = page_ptr;
    n->owned = owned;
    n->dirty = dirty;
    hash_insert(&d->hash, n);
    lru_push_front(d, n);
}

static CacheNode* load_page(DFTL *d, uint64_t mpn)
{
    CacheNode *n = cache_grab_node(d);
    if (!n) {
        fprintf(stderr, "cache node allocation failed\n");
        exit(EXIT_FAILURE);
    }

    uint8_t *p = d->page_dir[mpn];
    if (p) {
        /* 将目录中的私有页指针移交到缓存节点，避免复制 */
        d->page_dir[mpn] = NULL;
        bind_node(d, n, mpn, p, /*owned=*/1, /*dirty=*/0);
    } else {
        /* 未分配：读为零页（只读） */
        bind_node(d, n, mpn, d->zero_page, /*owned=*/0, /*dirty=*/0);
    }
    return n;
}

static inline CacheNode* cache_get(DFTL *d, uint64_t mpn)
{
    CacheNode *n = hash_find(&d->hash, mpn);
    if (n) {
        lru_unlink(d, n);
        lru_push_front(d, n);
        return n;
    }
    return load_page(d, mpn);
}

/* -------- 生命周期与对外API -------- */

void FTLInit()
{
    if (g) return;

    DFTL *d = (DFTL *)FTLMalloc(sizeof(DFTL));
    memset(d, 0, sizeof(DFTL));

    /* 计算需要的映射页总数 */
    const uint64_t epp = (uint64_t)ENTRIES_PER_PAGE;
    const uint64_t total_lba = LBA_MAX_PLUS1;
    const uint64_t mpn_cnt = (total_lba + epp - 1ull) / epp;
    d->mpn_count = mpn_cnt;

    d->page_dir = (uint8_t **)FTLCalloc((size_t)mpn_cnt, sizeof(uint8_t *));
    d->zero_page = (uint8_t *)FTLMalloc(MAP_PAGE_BYTES);
    memset(d->zero_page, 0, MAP_PAGE_BYTES);

    d->capacity = MCACHE_PAGES ? MCACHE_PAGES : 1024u; /* 兜底 */
    d->nodes = (CacheNode *)FTLCalloc(d->capacity, sizeof(CacheNode));
    hash_init(&d->hash, d->capacity);
    lru_init(d);

    g = d;
}

void FTLDestroy()
{
    if (!g) return;

    /* 将缓存中的私有页指针归还目录 */
    for (uint32_t i = 0; i < g->capacity; ++i) {
        CacheNode *n = &g->nodes[i];
        if (n->page && n->owned) {
            uint8_t *old = g->page_dir[n->mpn];
            if (old && old != n->page) {
                FTLFree(old, MAP_PAGE_BYTES);
            }
            g->page_dir[n->mpn] = n->page;
            n->page = NULL;
        }
    }

    /* 释放目录内所有已分配页 */
    for (uint64_t mpn = 0; mpn < g->mpn_count; ++mpn) {
        if (g->page_dir[mpn]) {
            FTLFree(g->page_dir[mpn], MAP_PAGE_BYTES);
            g->page_dir[mpn] = NULL;
        }
    }

    FTLFree(g->zero_page, MAP_PAGE_BYTES);
    hash_destroy(&g->hash);
    FTLFree(g->nodes, (size_t)g->capacity * sizeof(CacheNode));
    FTLFree(g->page_dir, (size_t)g->mpn_count * sizeof(uint8_t *));
    FTLFree(g, sizeof(DFTL));
    g = NULL;
}

/* 读：未写过返回0（来自零页） */
uint64_t FTLRead(uint64_t lba)
{
    if (!g) return 0;
    if (lba >= LBA_MAX_PLUS1) {
        /* 越界读，按未写过处理 */
        return 0;
    }

    const uint64_t mpn = lba_to_mpn(lba);
    const uint32_t off = lba_to_off(lba);
    CacheNode *n = cache_get(g, mpn);
    return entry_load(n->page, off);
}

/* 写：首次写入该映射页时分配私有页（COW），再写入条目 */
bool FTLModify(uint64_t lba, uint64_t ppn)
{
    if (!g) return false;
    if (lba >= LBA_MAX_PLUS1) {
        /* 越界写：忽略或返回false，这里选择忽略并返回false */
        return false;
    }

    const uint64_t mpn = lba_to_mpn(lba);
    const uint32_t off = lba_to_off(lba);
    CacheNode *n = cache_get(g, mpn);

    if (!n->owned) {
        /* 当前绑定零页：为写入分配私有页 */
        uint8_t *p = (uint8_t *)FTLMalloc(MAP_PAGE_BYTES);
        /* 零页为全0，首次写入无需拷贝，直接清零即可 */
        memset(p, 0, MAP_PAGE_BYTES);
        n->page = p;
        n->owned = 1;
    }

    entry_store(n->page, off, ppn);
    n->dirty = 1;
    return true;
}

/* -------- 驱动与统计 -------- */

uint32_t AlgorithmRun(IOVector *ioVector, const char *outputFile)
{
    struct timeval t0, t1;
    FTLInit();

    FILE *fp = fopen(outputFile, "w");
    if (!fp) {
        perror("open outputFile");
        exit(EXIT_FAILURE);
    }

    /* 为输出设置较大的缓冲区，降低系统调用次数 */
    char *obuf = (char *)FTLMalloc(OUTPUT_BUFFER_BYTES);
    if (setvbuf(fp, obuf, _IOFBF, OUTPUT_BUFFER_BYTES) != 0) {
        FTLFree(obuf, OUTPUT_BUFFER_BYTES);
        obuf = NULL;
    }

    gettimeofday(&t0, NULL);

    const uint32_t n = (uint32_t)ioVector->len; /* 数据集内给的是32位内的量 */
    for (uint32_t i = 0; i < n; ++i) {
        const IOUnit *u = &ioVector->ioArray[i];
        if (u->type == IO_READ) {
            const uint64_t v = FTLRead(u->lba);
            /* 使用PRIu64避免不同平台格式差异 */
            fprintf(fp, "%" PRIu64 "\n", v);
        } else {
            (void)FTLModify(u->lba, u->ppn);
        }
    }

    gettimeofday(&t1, NULL);

    fclose(fp);
    if (obuf) FTLFree(obuf, OUTPUT_BUFFER_BYTES);

    FTLDestroy();

    const long sec = (t1.tv_sec - t0.tv_sec);
    const long usec = (t1.tv_usec - t0.tv_usec);
    const double elapsed_us = (double)sec * 1000000.0 + (double)usec;

    /* 打印单位与数值一致：这里以微秒与毫秒各输出一次 */
    printf("algorithmRunningDuration(us):  %.0f us\n", elapsed_us);
    printf("algorithmRunningDuration(ms):  %.3f ms\n", elapsed_us / 1000.0);
    printf("Max memory used:               %llu B\n", (unsigned long long)g_mem_peak);

    return RETURN_OK;
}