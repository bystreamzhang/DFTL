#define _GNU_SOURCE

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <errno.h>
#include <inttypes.h>
#include "ftl.h"

#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <pthread.h>
#include <sys/sysinfo.h>

// 原注释：目前发现，对于本赛题的数据，CMT没什么用，可以直接取消，保留TPC就行
//#define USE_CMT

//决赛添加
//#define DEBUG_FTL // 打印调试信息(主要是预估内存占用)。为了加速，提交时应该注释掉。实际延时影响其实小于0.5%
#define SMALL_GTD_ARRAY
#define FAST_DESTROY   // 跳过销毁逻辑，
#define FAST_CONSTANTS // 针对决赛的常数优化，可能注释掉一些检查和未实现功能
//#define FAST_CONSTANTS_PREAD //pread_full更安全可靠，而开启此优化后直接使用pread。优化小于1% (94,92,92 vs 91,93,92)
#define SETVBUF // 使用更大的输入缓冲区，延时优化约2%~3%
#define ZERO_COPY_DMA // 使用零拷贝DMA读写SSD文件，需内核支持，优化约1%
#define CACHE_LINE_OPTIMIZE // 消除结构体填充 (Cache Line 利用率优化)，优化约1%~2%
#define LAST_HIT_OPTIMIZE // 利用最近命中优化TPC查找，优化小于1%
#define LIKELY_OPTIMIZE // 使用likely和unlikely宏优化分支预测，可能有1%以下的优化

#define MPN_SENTINEL (~0ULL) // 表示无效的 MPN

#ifdef CACHE_LINE_OPTIMIZE
// 快速计算实际内存地址；page_pool_base 是基地址，idx 是页索引
#define GET_TPC_PTR(d, idx) ((d)->page_pool_base + (size_t)(idx) * MAP_PAGE_BYTES)
#endif

#ifdef LIKELY_OPTIMIZE
#define likely(x)       __builtin_expect(!!(x), 1)
#define unlikely(x)     __builtin_expect(!!(x), 0)
#else
#define likely(x)       (x)
#define unlikely(x)     (x)
#endif

#define CHECK_TPC_ENTRY_VALID(e) ((e)->mpn != MPN_SENTINEL)

//#define SMALL_INPUT_MODE
#ifdef SMALL_INPUT_MODE
#define SMALL_INPUT_THRESHOLD (30000000ull)
#endif

// TPC 配置
#define TPC_WAYS 4
#define TPC_WAYS_MASK (TPC_WAYS - 1)
#define TPC_SETS 64
#define TPC_SET_MASK (TPC_SETS - 1)

// TaskBatch 模型
#define BATCH_SIZE   4096
#define QUEUE_DEPTH  16

typedef struct {
    uint32_t type;
    uint64_t lba;
    uint64_t ppn;
} TaskSimple;

typedef struct {
    TaskSimple tasks[BATCH_SIZE];
    int count;
} TaskBatch;

// 全局流水线结构：只用单 worker 线程和 TaskBatch 队列，保持输出顺序
typedef struct {
    TaskBatch *batch_queue[QUEUE_DEPTH];
    TaskBatch *free_batches[QUEUE_DEPTH + 2];
    int free_count;
    int head;
    int tail;
    volatile int finished;

    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;

    pthread_t worker_tid;
    FILE *output_file;
} PipelineSimple;

static PipelineSimple g_pl = {0};

// 用0表示无效PPA，mpn要加1才能得到对应ppn，ppn要减1得到mpn。
// 输入ppn如果为0：暂时也视为无效ppa
#define INVALID_PPA     (0ULL)
#define INVALID_LPN     (~(0ULL))
#define UNMAPPED_PPA    (~(0ULL))

enum {
    CLEAN = 0,
    DIRTY = 1
} DIRTY_STATE;

// 原 TPC 参数（未使用的部分保留）
#define MCACHE_PAGES (1u << 10)
#define CMT_HASH_SIZE (1u << 12)
// #define TPC_MAX_PAGES    (1u << 8)
// #define TPC_HASH_SIZE    (1u << 12)

#define MAP_PAGE_BYTES 4096u
#define LBA_MAX_PLUS1 (1ull << 36)
#define CMT_HASH_MASK (CMT_HASH_SIZE - 1)
// #define TPC_HASH_MASK (TPC_HASH_SIZE - 1)

// 字节压缩优化 (暂时搁置)
#define USE_U64_ENTRY

#if defined(USE_U64_ENTRY)
#define ENTRY_BYTES 8u
#else
#define ENTRY_BYTES 6u
#endif
#if defined(USE_U64_ENTRY) && defined(USE_U48_ENTRY)
#error "Define only one of USE_U64_ENTRY or USE_U48_ENTRY"
#endif
#if !defined(USE_U64_ENTRY) && !defined(USE_U48_ENTRY)
#define USE_U64_ENTRY
#endif

// ========== MemStats / SsdStats 及通用内存封装，保持原样 ==========

typedef struct MemStats
{
    uint64_t total_used;
    uint64_t peak_used;
    uint64_t ctrl_used;
    uint64_t cmt_entrys_used;
    uint64_t cmt_hash_used;
    uint64_t free_cmt_entry_cnt;
    uint64_t used_cmt_entry_cnt;
    uint64_t gtd_used;
    uint64_t tpc_used; // 包括tpc哈希表或TPC控制结构
    uint64_t tpc_page_used;
    uint64_t cmt_query_cnt;
    uint64_t cmt_hit_cnt;
    uint64_t tpc_query_cnt;
    uint64_t tpc_hit_cnt;
    uint64_t cmt_dirty_handle_cnt;
    uint64_t tpc_dirty_handle_cnt;
    uint64_t threads_used; // 用于统计多线程/流水线的内存开销
} MemStats;

typedef struct SsdStats
{
    uint64_t map_st_size;
    uint64_t map_st_blocks;
    uint64_t map_max_off;
    uint64_t map_pages_written_bytes;
    uint64_t map_pages_written_cnt;
    uint64_t map_pages_read_cnt;
} SsdStats;

static MemStats g_memstats = {0};
static SsdStats g_ssdstats = {0};

static void *FTLMalloc(size_t size)
{
    void *p = malloc(size);
    if (unlikely(!p))
    {
        fprintf(stderr, "malloc %zu failed: %s\n", size, strerror(errno));
        exit(EXIT_FAILURE);
    }
    return p;
}

static void *FTLCalloc(size_t n, size_t size)
{
    void *p = calloc(n, size);
    if (unlikely(!p))
    {
        fprintf(stderr, "calloc %zu failed: %s\n", n * size, strerror(errno));
        exit(EXIT_FAILURE);
    }
    return p;
}

static void FTLFree(void *p, size_t size)
{
    (void)size;
    if (p)
        free(p);
}

static int open_file(const char *path, bool write)
{
    // 如果ssd文件存在，将其内容清空(O_TRUNC)
    //int flags = write ? (O_CREAT | O_RDWR | O_TRUNC) : O_RDONLY;
    int flags = write ? (O_CREAT | O_RDWR) : O_RDONLY;
    int fd = open(path, flags, 0644);
    if (unlikely(fd < 0))
    {
        perror(path);
        exit(1);
    }
    return fd;
}

// 读写：返回实际字节数或-1
static ssize_t pread_full(int fd, void *buf, size_t len, off_t off)
{
#ifdef FAST_CONSTANTS_PREAD
    return pread(fd, buf, MAP_PAGE_BYTES, off);
#else
    uint8_t *p = (uint8_t *)buf;
    size_t n = 0;
    while (n < len)
    {
        ssize_t r = pread(fd, p + n, len - n, off + (off_t)n);
        if (r < 0)
        {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (r == 0)
            break; // 稀疏文件/EOF，允许
        n += (size_t)r;
    }
    if (n < len)
        memset(p + n, 0, len - n);

#ifdef DEBUG_FTL
    g_ssdstats.map_pages_read_cnt++; // 统计读页数(写页数在ssdstats_on_write_map中统计)
#endif
    return (ssize_t)n;
#endif
}

static ssize_t pwrite_full(int fd, const void *buf, size_t len, off_t off)
{
    const uint8_t *p = (const uint8_t *)buf;
    size_t n = 0;
    while (n < len)
    {
        ssize_t r = pwrite(fd, p + n, len - n, off + (off_t)n);
        if (r < 0)
        {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (r == 0)
            break; // 非预期但防止死循环
        n += (size_t)r;
    }
    return (ssize_t)n;
}

static inline double to_gb(uint64_t bytes) { return (double)bytes / 1024.0 / 1024.0 / 1024.0; }

static inline void memstats_add(uint64_t *field, uint64_t sz)
{
#ifdef DEBUG_FTL
    *field += sz;
    g_memstats.total_used += sz;
    if (g_memstats.total_used > g_memstats.peak_used)
        g_memstats.peak_used = g_memstats.total_used;
#endif
}

static inline void memstats_sub(uint64_t *field, uint64_t sz)
{
#ifdef DEBUG_FTL
    if (*field >= sz)
        *field -= sz;
    else
        *field = 0;
    if (g_memstats.total_used >= sz)
        g_memstats.total_used -= sz;
    else
        g_memstats.total_used = 0;
#endif
}

typedef enum
{
    MEM_CLASS_CTRL = 1,
    MEM_CLASS_ENTRIES,
    MEM_CLASS_GTD,
    MEM_CLASS_CMT_HASH,
    MEM_CLASS_TPC,
    MEM_CLASS_TPC_PAGE,
    MEM_CLASS_PIPELINE
} MemClass;

static void *FTLMallocEx(size_t size, MemClass cls)
{
    void *p = FTLMalloc(size);
    switch (cls)
    {
    case MEM_CLASS_CTRL:
        memstats_add(&g_memstats.ctrl_used, size);
        break;
    case MEM_CLASS_ENTRIES:
        memstats_add(&g_memstats.cmt_entrys_used, size);
        break;
    case MEM_CLASS_GTD:
        memstats_add(&g_memstats.gtd_used, size);
        break;
    case MEM_CLASS_CMT_HASH:
        memstats_add(&g_memstats.cmt_hash_used, size);
        break;
    case MEM_CLASS_TPC:
        memstats_add(&g_memstats.tpc_used, size);
        break;
    case MEM_CLASS_TPC_PAGE:
        memstats_add(&g_memstats.tpc_page_used, size);
        break;
    case MEM_CLASS_PIPELINE:
        memstats_add(&g_memstats.threads_used, size);
        break;
    default:
        memstats_add(&g_memstats.ctrl_used, size);
        break;
    }
    return p;
}

static void *FTLCallocEx(size_t n, size_t size, MemClass cls)
{
    void *p = FTLCalloc(n, size);
    size_t total = n * size;
    switch (cls)
    {
    case MEM_CLASS_CTRL:
        memstats_add(&g_memstats.ctrl_used, total);
        break;
    case MEM_CLASS_ENTRIES:
        memstats_add(&g_memstats.cmt_entrys_used, total);
        break;
    case MEM_CLASS_GTD:
        memstats_add(&g_memstats.gtd_used, total);
        break;
    case MEM_CLASS_CMT_HASH:
        memstats_add(&g_memstats.cmt_hash_used, total);
        break;
    case MEM_CLASS_TPC:
        memstats_add(&g_memstats.tpc_used, total);
        break;
    case MEM_CLASS_TPC_PAGE:
        memstats_add(&g_memstats.tpc_page_used, total);
        break;
    case MEM_CLASS_PIPELINE:
        memstats_add(&g_memstats.threads_used, total);
        break;
    default:
        memstats_add(&g_memstats.ctrl_used, total);
        break;
    }
    return p;
}

static void FTLFreeEx(void *p, size_t size, MemClass cls)
{
    FTLFree(p, size);
    switch (cls)
    {
    case MEM_CLASS_CTRL:
        memstats_sub(&g_memstats.ctrl_used, size);
        break;
    case MEM_CLASS_ENTRIES:
        memstats_sub(&g_memstats.cmt_entrys_used, size);
        break;
    case MEM_CLASS_GTD:
        memstats_sub(&g_memstats.gtd_used, size);
        break;
    case MEM_CLASS_CMT_HASH:
        memstats_sub(&g_memstats.cmt_hash_used, size);
        break;
    case MEM_CLASS_TPC:
        memstats_sub(&g_memstats.tpc_used, size);
        break;
    case MEM_CLASS_TPC_PAGE:
        memstats_sub(&g_memstats.tpc_page_used, size);
        break;
    case MEM_CLASS_PIPELINE:
        memstats_sub(&g_memstats.threads_used, size);
        break;
    default:
        memstats_sub(&g_memstats.ctrl_used, size);
        break;
    }
}

static inline void ssdstats_on_write_map(off_t offset, size_t len)
{
#ifdef DEBUG_FTL
    uint64_t end = (uint64_t)offset + (uint64_t)len;
    if (end > g_ssdstats.map_max_off)
        g_ssdstats.map_max_off = end;
    g_ssdstats.map_pages_written_bytes += len;
    g_ssdstats.map_pages_written_cnt ++;
#endif
}

static inline void ssdstats_refresh_from_fs(int fd_map)
{
#ifdef DEBUG_FTL
    struct stat st;
    if (fd_map >= 0 && fstat(fd_map, &st) == 0)
    {
        g_ssdstats.map_st_size = (uint64_t)st.st_size;
        g_ssdstats.map_st_blocks = (uint64_t)st.st_blocks * 512ull;
    }
#endif
}

static ssize_t pwrite_full_with_stats(int fd, const void *buf, size_t len, off_t off)
{
    ssize_t n = pwrite_full(fd, buf, len, off);
    #ifdef DEBUG_FTL
    if (n == (ssize_t)len)
        ssdstats_on_write_map(off, len);
    #endif
    return n;
}

#define FTL_MALLOC_CTRL(sz_tt) FTLMallocEx((sz_tt), MEM_CLASS_CTRL)
#define FTL_CALLOC_CTRL(n, sz_per) FTLCallocEx((n), (sz_per), MEM_CLASS_CTRL)
#define FTL_MALLOC_ENTRIES(n) FTLCallocEx((n), sizeof(cmt_entry), MEM_CLASS_ENTRIES)
#ifdef SMALL_GTD_ARRAY
#define GTD_BITMAP_BYTES(n_mpns) (((n_mpns) + 7ull) / 8ull)
#define FTL_MALLOC_GTD(n_mpns) FTLCallocEx(GTD_BITMAP_BYTES((n_mpns)), 1u, MEM_CLASS_GTD)
#define FTL_FREE_GTD(p, n_mpns) FTLFreeEx((p), GTD_BITMAP_BYTES((n_mpns)), MEM_CLASS_GTD)
#else
#define FTL_MALLOC_GTD(n) FTLCallocEx((n), sizeof(uint64_t), MEM_CLASS_GTD)
#define FTL_FREE_GTD(p, cap) FTLFreeEx((p), (size_t)(cap) * sizeof(uint64_t), MEM_CLASS_GTD)
#endif
#define FTL_MALLOC_TPC(sz_tt) FTLMallocEx((sz_tt), MEM_CLASS_TPC)
#define FTL_MALLOC_TPC_PAGE(n) FTLCallocEx((n), MAP_PAGE_BYTES, MEM_CLASS_TPC_PAGE)
#define FTL_MALLOC_CMT_HASH_BUCKETS(n) FTLCallocEx((n), sizeof(cmt_entry *), MEM_CLASS_CMT_HASH)
#define FTL_FREE_CTRL(p, sz_tt) FTLFreeEx((p), (sz_tt), MEM_CLASS_CTRL)
#define FTL_FREE_ENTRIES(p, cap) FTLFreeEx((p), (size_t)(cap) * sizeof(cmt_entry), MEM_CLASS_ENTRIES)
#define FTL_FREE_TPC(p, sz_tt) FTLFreeEx((p), (sz_tt), MEM_CLASS_TPC)
#define FTL_FREE_CMT_HASH_BUCKETS(p, cap) FTLFreeEx((p), (size_t)(cap) * sizeof(cmt_entry *), MEM_CLASS_CMT_HASH)
#define FTL_FREE_TPC_PAGE(p) FTLFreeEx((p), MAP_PAGE_BYTES, MEM_CLASS_TPC_PAGE)
#define SSD_PWRITE_MAP(fd, buf, len, off) pwrite_full_with_stats((fd), (buf), (len), (off))

#define FTL_MALLOC_PIPELINE(sz_tt) FTLMallocEx((sz_tt), MEM_CLASS_PIPELINE)
#define FTL_FREE_PIPELINE(p, sz_tt) FTLFreeEx((p), (sz_tt), MEM_CLASS_PIPELINE)

// 映射条目
#define EPP (MAP_PAGE_BYTES / ENTRY_BYTES)

#if ENTRY_BYTES == 8u
static inline uint64_t lpn_to_mpn(uint64_t lpn) { return lpn >> 9; }
static inline uint32_t lpn_to_off(uint64_t lpn) { return (uint32_t)(lpn & 511); }
#else
static inline uint64_t lpn_to_mpn(uint64_t lpn) { return lpn / (uint64_t)EPP; }
static inline uint32_t lpn_to_off(uint64_t lpn) { return (uint32_t)(lpn % (uint64_t)EPP); }
#endif

static inline uint64_t mpn_to_ppa(uint64_t mpn) { return mpn + 1; }
static inline uint64_t ppa_to_mpn(uint64_t ppa) { return ppa - 1; }

static inline void entry_store_u64(uint8_t *base, uint32_t idx, uint64_t v)
{
#if ENTRY_BYTES == 8u
    ((uint64_t *)base)[idx] = v;
#else
    uint8_t *p = base + (size_t)idx * 6u;
    uint64_t x = v & 0xFFFFFFFFFFFFull;
    p[0] = (uint8_t)(x);
    p[1] = (uint8_t)(x >> 8);
    p[2] = (uint8_t)(x >> 16);
    p[3] = (uint8_t)(x >> 24);
    p[4] = (uint8_t)(x >> 32);
    p[5] = (uint8_t)(x >> 40);
#endif
}

static inline uint64_t entry_load_u64(const uint8_t *base, uint32_t idx)
{
#if ENTRY_BYTES == 8u
    return ((const uint64_t *)base)[idx];
#else
    const uint8_t *p = base + (size_t)idx * 6u;
    uint64_t x = (uint64_t)p[0] | ((uint64_t)p[1] << 8) | ((uint64_t)p[2] << 16) | ((uint64_t)p[3] << 24) | ((uint64_t)p[4] << 32) | ((uint64_t)p[5] << 40);
    return x;
#endif
}

// ========== 替换原来的 TPC 实现：采用 4-way set associative + 预分配 page_pool ==========

// TPC 结构
#ifndef CACHE_LINE_OPTIMIZE
typedef struct {
    uint64_t mpn;
    uint8_t *buffer;
    uint8_t dirty;
} TpcEntry;
#else
typedef struct {
    uint64_t mpn;
    uint32_t buf_idx;  // 改为索引
    uint8_t dirty;
    uint8_t pad[3];    // 凑齐 16 bytes，保证对齐
} TpcEntry;
#endif

typedef struct {
    TpcEntry ways[TPC_WAYS];
    uint8_t next_victim;
} TpcSet;

// ========== CMT 结构（原样保留，默认 USE_CMT 未启用） ==========

typedef struct cmt_entry
{
    uint64_t lpn;
    uint64_t ppn;
    uint8_t dirty;
    QTAILQ_ENTRY(cmt_entry) entry;
    struct cmt_entry *hnext; // for hash
} cmt_entry;

typedef struct
{
    cmt_entry **cmt_table;
} cmt_hash_table;

// ========== FTL 主控制块：融合 TPC / GTD / CMT / 多线程状态 ==========

typedef struct
{
    uint64_t total_lpns;
    uint64_t total_mpns;

    cmt_entry *cmt_entries;
    uint64_t tt_entries;

    QTAILQ_HEAD(free_cmt_entry_list, cmt_entry) free_cmt_entry_list;
    uint64_t free_cmt_entry_cnt;

    QTAILQ_HEAD(cmt_entry_list, cmt_entry) cmt_entry_list;
    uint64_t used_cmt_entry_cnt;

#ifdef SMALL_GTD_ARRAY
    uint8_t *gtd; // 位图，每 bit 表示对应 mpn 是否已分配
#else
    uint64_t *gtd;   // GTD, 记录mpn->mpn_ppa
#endif

    cmt_hash_table cmt_hash;
    int fd_map;

    // 新 TPC：4-way 组相联 + 预分配页池
    TpcSet tpc_sets[TPC_SETS];
    uint8_t *page_pool_base; // 共 TPC_SETS*TPC_WAYS 页

    bool small_input_mode;

    bool multi_threaded;

    // Last-Hit 优化缓存
#ifdef LAST_HIT_OPTIMIZE
    uint64_t last_mpn;
    TpcEntry *last_entry;
#endif
} FTL;

static FTL *g = NULL;

// ========== GTD 位图操作（原有 SMALL_GTD_ARRAY 逻辑） ==========

#ifdef SMALL_GTD_ARRAY
static inline bool gtd_is_allocated(const FTL *d, uint64_t mpn)
{
    return (d->gtd[mpn >> 3] >> (mpn & 7u)) & 1u;
}

static inline void gtd_mark_allocated(FTL *d, uint64_t mpn)
{
    d->gtd[mpn >> 3] |= (uint8_t)(1u << (mpn & 7u));
}
#endif

// ========== TPC 4-way 实现 ==========

static inline uint32_t tpc_get_set_idx(uint64_t mpn) {
    return (uint32_t)(mpn & TPC_SET_MASK);
}

static inline void tpc_flush_entry(FTL *d, TpcEntry *e) {
    if (CHECK_TPC_ENTRY_VALID(e) && e->dirty) {
        off_t offset = (off_t)(e->mpn) * MAP_PAGE_BYTES;
#ifndef CACHE_LINE_OPTIMIZE
        if (SSD_PWRITE_MAP(d->fd_map, e->buffer, MAP_PAGE_BYTES, offset) != (ssize_t)MAP_PAGE_BYTES) {
            perror("pwrite failed");
            exit(1);
        }
#else
        uint8_t *real_buffer = GET_TPC_PTR(d, e->buf_idx);
        if (SSD_PWRITE_MAP(d->fd_map, real_buffer, MAP_PAGE_BYTES, offset) != (ssize_t)MAP_PAGE_BYTES) {
            perror("pwrite failed");
            exit(1);
        }
#endif
        e->dirty = 0;
        memstats_add(&g_memstats.tpc_dirty_handle_cnt, 1);
    }
}

static uint8_t *tpc_get_buffer(FTL *d, uint64_t mpn, int is_write) {
    memstats_add(&g_memstats.tpc_query_cnt, 1);
#ifdef LAST_HIT_OPTIMIZE
    // 检查是否命中上一次访问的页
    if (likely(d->last_mpn == mpn)) {
        if (is_write) d->last_entry->dirty = 1;
        memstats_add(&g_memstats.tpc_hit_cnt, 1);
#ifdef CACHE_LINE_OPTIMIZE
        return GET_TPC_PTR(d, d->last_entry->buf_idx);
#else
        return d->last_entry->buffer;
#endif
    }
#endif
    uint32_t set_idx = tpc_get_set_idx(mpn);
    TpcSet *set = &d->tpc_sets[set_idx];

    // 查找命中
    for (int i = 0; i < TPC_WAYS; i++) {
        if (set->ways[i].mpn == mpn) {
            if (is_write) set->ways[i].dirty = 1;
            memstats_add(&g_memstats.tpc_hit_cnt, 1);
#ifdef LAST_HIT_OPTIMIZE
            d->last_mpn = mpn;
            d->last_entry = &set->ways[i];
#endif
#ifndef CACHE_LINE_OPTIMIZE
            return set->ways[i].buffer;
#else
            return GET_TPC_PTR(d, set->ways[i].buf_idx);
#endif
        }
    }
    // 未命中：选 victim
    int victim_way = set->next_victim;
    TpcEntry *e = &set->ways[victim_way];
    set->next_victim = (uint8_t)((set->next_victim + 1) & TPC_WAYS_MASK);

    if (CHECK_TPC_ENTRY_VALID(e)) {
        tpc_flush_entry(d, e);
    }

    e->mpn = mpn;
    e->dirty = is_write ? 1 : 0;

#ifdef LAST_HIT_OPTIMIZE
    d->last_mpn = mpn;
    d->last_entry = e;
#endif

#ifndef CACHE_LINE_OPTIMIZE
    uint8_t *real_buffer = e->buffer;
#else
    uint8_t *real_buffer = GET_TPC_PTR(d, e->buf_idx);
#endif

    // 查看 GTD 决定是否读盘
#ifdef SMALL_GTD_ARRAY
    if (gtd_is_allocated(d, mpn)) {
        off_t offset = (off_t)mpn * MAP_PAGE_BYTES;
        if (pread_full(d->fd_map, real_buffer, MAP_PAGE_BYTES, offset) < 0) {
            memset(real_buffer, 0, MAP_PAGE_BYTES);
        }
    } else {
        if (is_write) gtd_mark_allocated(d, mpn);
        memset(real_buffer, 0, MAP_PAGE_BYTES);
    }
#else
    uint64_t ppa = d->gtd[mpn];
    if (ppa == INVALID_PPA) {
        if (is_write) {
            d->gtd[mpn] = mpn_to_ppa(mpn);
        }
        memset(real_buffer, 0, MAP_PAGE_BYTES);
    } else {
        off_t offset = (off_t)ppa_to_mpn(ppa) * MAP_PAGE_BYTES;
        if (pread_full(d->fd_map, real_buffer, MAP_PAGE_BYTES, offset) < 0) {
            memset(real_buffer, 0, MAP_PAGE_BYTES);
        }
    }
#endif
    return real_buffer;
}

// ========== CMT 相关（保持原逻辑，默认 USE_CMT 未启用） ==========

static uint64_t gtd_get_ppa_for_mpn(FTL *d, uint64_t mpn) {
    if (mpn >= d->total_mpns) {
        fprintf(stderr, "gtd_get_ppa_for_mpn: mpn out of range\n");
        exit(1);
    }
#ifdef SMALL_GTD_ARRAY
    if (!gtd_is_allocated(d, mpn)) {
        return INVALID_PPA;
    }
    return mpn_to_ppa(mpn);
#else
    return d->gtd[mpn];
#endif
}

// 从TPC读取lpn对应ppn（未分配则返回UNMAPPED）
static uint64_t read_ppn_from_map_with_gtd(FTL *d, uint64_t lpn)
{
    // 原实现：使用 tpc_page 结构及 LRU；现改为使用 tpc_get_buffer
    uint64_t mpn = lpn_to_mpn(lpn);
    uint32_t off = lpn_to_off(lpn);
    uint8_t *buf = tpc_get_buffer(d, mpn, 0);
#ifdef FAST_CONSTANTS
    return ((const uint64_t *)buf)[off];
#else
    uint64_t v = entry_load_u64(buf, off);
    if (v == 0) return UNMAPPED_PPA;
    return v;
#endif
}

// 写入条目到TPC页，必要时分配映射页并在写回时统一刷盘
static void write_ppn_to_map_with_gtd(FTL *d, uint64_t lpn, uint64_t ppn)
{
    // 原实现：获取 tpc_page，可能懒分配 buf；现在统一使用 tpc_get_buffer
    uint64_t mpn = lpn_to_mpn(lpn);
    uint32_t off = lpn_to_off(lpn);
    uint8_t *buf = tpc_get_buffer(d, mpn, 1);
    entry_store_u64(buf, off, ppn);
}

// 以下 CMT hash 仍保留（如后续开启 USE_CMT 时可用）
static uint64_t hash_lpn(uint64_t lpn) {
    return lpn & CMT_HASH_MASK;
}

static void cachehash_init(cmt_hash_table *h, uint64_t sz)
{
    h->cmt_table = (cmt_entry **)FTL_MALLOC_CMT_HASH_BUCKETS(sz);
    memset(h->cmt_table, 0, (size_t)sz * sizeof(cmt_entry *));
}

static void cachehash_destroy(cmt_hash_table *h)
{
    FTL_FREE_CMT_HASH_BUCKETS(h->cmt_table, CMT_HASH_SIZE);
    h->cmt_table = NULL;
}

static cmt_entry *cachehash_find(cmt_hash_table *h, uint64_t lpn)
{
    memstats_add(&g_memstats.cmt_query_cnt, 1);
    cmt_entry *p = h->cmt_table[hash_lpn(lpn)];
    while (p)
    {
        if (p->lpn == lpn){
            memstats_add(&g_memstats.cmt_hit_cnt, 1);
            return p;
        }
        p = p->hnext;
    }
    return NULL;
}

static void cachehash_insert(cmt_hash_table *h, cmt_entry *n)
{
    uint64_t idx = hash_lpn(n->lpn);
    n->hnext = h->cmt_table[idx];
    h->cmt_table[idx] = n;
}

static void cachehash_remove(cmt_hash_table *h, cmt_entry *n)
{
    uint32_t idx = hash_lpn(n->lpn);
    cmt_entry *p = h->cmt_table[idx], *prev = NULL;
    while (p)
    {
        if (p == n)
        {
            if (prev)
                prev->hnext = p->hnext;
            else
                h->cmt_table[idx] = p->hnext;
            n->hnext = NULL;
            return;
        }
        prev = p;
        p = p->hnext;
    }
    perror("Cachehash_remove did not find the entry\n");
}

static void cmt_entry_bind(FTL *d, cmt_entry *n, uint64_t lpn, uint64_t ppn, uint8_t dirty)
{
    n->lpn = lpn;
    n->ppn = ppn;
    n->dirty = dirty;
    cachehash_insert(&d->cmt_hash, n);
    QTAILQ_INSERT_HEAD(&d->cmt_entry_list, n, entry);
    d->used_cmt_entry_cnt++;
}

static void handback_if_needed(FTL *d, cmt_entry *n)
{
    if (n->dirty == DIRTY) {
        memstats_add(&g_memstats.cmt_dirty_handle_cnt, 1);
        write_ppn_to_map_with_gtd(d, n->lpn, n->ppn);
        n->dirty = CLEAN;
    }
}

static cmt_entry *cache_get_entry(FTL *d, uint64_t lpn)
{
    cmt_entry *n = cachehash_find(&d->cmt_hash, lpn);
    if (n) {
        QTAILQ_REMOVE(&d->cmt_entry_list, n, entry);
        QTAILQ_INSERT_HEAD(&d->cmt_entry_list, n, entry);
        return n;
    }
    cmt_entry *e;
    if (!QTAILQ_EMPTY(&d->free_cmt_entry_list)) {
        e = QTAILQ_FIRST(&d->free_cmt_entry_list);
        QTAILQ_REMOVE(&d->free_cmt_entry_list, e, entry);
        d->free_cmt_entry_cnt--;
    } else {
        e = QTAILQ_LAST(&d->cmt_entry_list);
        if (!e) {
            fprintf(stderr, "CMT empty\n");
            exit(1);
        }
        handback_if_needed(d, e);
        cachehash_remove(&d->cmt_hash, e);
        QTAILQ_REMOVE(&d->cmt_entry_list, e, entry);
        d->used_cmt_entry_cnt--;
    }
    uint64_t ppn = read_ppn_from_map_with_gtd(d, lpn);
    cmt_entry_bind(d, e, lpn, ppn, CLEAN);
    return e;
}

// ========== 资源报告（保持原版，但补充线程内存统计） ==========

static void PrintResourceReport(const char *title, FTL *d)
{
    fprintf(stdout, "\n================ Resource Report: %s ================\n", title);
#ifdef DEBUG_FTL
    fprintf(stdout, "Configures:\n");
    fprintf(stdout, "  - Total(Max) LPNS:     %" PRIu64 " pages (= %.6f GB)\n", d->total_lpns, to_gb(d->total_lpns * 4096ull));
    fprintf(stdout, "  - Max Cache Pages (Entries) in CMT:   %" PRIu64 " entries ( %" PRIu64 " B per entry) (about %.6f GB)\n",
            d->tt_entries, (uint64_t)sizeof(cmt_entry), to_gb(d->tt_entries * sizeof(cmt_entry)));
    fprintf(stdout, "  - CMT Hash Table Size:    %" PRIu64 " buckets\n", (uint64_t)CMT_HASH_SIZE);
#ifdef SMALL_GTD_ARRAY
    fprintf(stdout, "  - GTD Size:     %" PRIu64 " entries (= %.6f GB)\n",
            d->total_mpns, to_gb(d->total_mpns * sizeof(uint8_t)));
#else
    fprintf(stdout, "  - GTD Size:     %" PRIu64 " entries (= %.6f GB)\n",
            d->total_mpns, to_gb(d->total_mpns * sizeof(uint64_t)));
#endif
    fprintf(stdout, "  - TPC Sets:     %u, Ways per Set: %u, Total Pages: %u (= %.6f GB)\n",
            (unsigned)TPC_SETS, (unsigned)TPC_WAYS,
            (unsigned)(TPC_SETS * TPC_WAYS),
            to_gb((uint64_t)TPC_SETS * TPC_WAYS * MAP_PAGE_BYTES));

    uint64_t total = g_memstats.total_used;
    fprintf(stdout, "Heap Memory (current / peak): %" PRIu64 " B (%.6f GB) / %" PRIu64 " B (%.6f GB)\n",
            total, to_gb(total), g_memstats.peak_used, to_gb(g_memstats.peak_used));
    fprintf(stdout, "  - Control structures:   %" PRIu64 " B (%.6f GB)\n",
            g_memstats.ctrl_used, to_gb(g_memstats.ctrl_used));
    fprintf(stdout, "  - CMT entries:      %" PRIu64 " B (%.6f GB)\n",
            g_memstats.cmt_entrys_used, to_gb(g_memstats.cmt_entrys_used));
    fprintf(stdout, "  - GTD uses:      %" PRIu64 " B (%.6f GB)\n",
            g_memstats.gtd_used, to_gb(g_memstats.gtd_used));
    fprintf(stdout, "  - CMT hash table:   %" PRIu64 " B (%.6f GB)\n",
            g_memstats.cmt_hash_used, to_gb(g_memstats.cmt_hash_used));
    fprintf(stdout, "  - free cmt entry cnt:     %" PRIu64 " , %" PRIu64 " B (%.6f GB)\n",
            d->free_cmt_entry_cnt,
            d->free_cmt_entry_cnt * (uint64_t)sizeof(cmt_entry),
            to_gb(d->free_cmt_entry_cnt * (uint64_t)sizeof(cmt_entry)));
    fprintf(stdout, "  - used cmt entry cnt:     %" PRIu64 " , %" PRIu64 " B (%.6f GB)\n",
            d->used_cmt_entry_cnt,
            d->used_cmt_entry_cnt * (uint64_t)sizeof(cmt_entry),
            to_gb(d->used_cmt_entry_cnt * (uint64_t)sizeof(cmt_entry)));
    fprintf(stdout, "  - CMT hit ratio:     %.6f (%" PRIu64 " / %" PRIu64 ")\n",
            g_memstats.cmt_query_cnt ? (double)g_memstats.cmt_hit_cnt / g_memstats.cmt_query_cnt : 0.0,
            g_memstats.cmt_hit_cnt, g_memstats.cmt_query_cnt);
    fprintf(stdout, "  - TPC hit ratio:     %.6f (%" PRIu64 " / %" PRIu64 ")\n",
            g_memstats.tpc_query_cnt ? (double)g_memstats.tpc_hit_cnt / g_memstats.tpc_query_cnt : 0.0,
            g_memstats.tpc_hit_cnt, g_memstats.tpc_query_cnt);
    fprintf(stdout, "  - CMT dirty entry handle cnt (not including FTLDestory):     %" PRIu64 "\n",
            g_memstats.cmt_dirty_handle_cnt);
    fprintf(stdout, "  - TPC dirty entry handle cnt (not including FTLDestory):     %" PRIu64 "\n",
            g_memstats.tpc_dirty_handle_cnt);
    fprintf(stdout, "  - TPC control+hash uses:      %" PRIu64 " B (%.6f GB)\n",
            g_memstats.tpc_used, to_gb(g_memstats.tpc_used));
    fprintf(stdout, "  - TPC pages (page_pool_base): %" PRIu64 " B (%.6f GB)\n",
            g_memstats.tpc_page_used, to_gb(g_memstats.tpc_page_used));
    fprintf(stdout, "  - Threads/pipeline memory:    %" PRIu64 " B (%.6f GB)\n",
            g_memstats.threads_used, to_gb(g_memstats.threads_used));

    fprintf(stdout, "SSD Usage (from filesystem stat):\n");
    fprintf(stdout, "  - map.ssd size:   %" PRIu64 " B (%.6f GB), blocks: %" PRIu64 " B (%.6f GB)\n",
            g_ssdstats.map_st_size, to_gb(g_ssdstats.map_st_size),
            g_ssdstats.map_st_blocks, to_gb(g_ssdstats.map_st_blocks));

    fprintf(stdout, "SSD Logical write accounting (program-side):\n");
    fprintf(stdout, "  - map pages written:     %" PRIu64 " B (%.6f GB)\n",
            g_ssdstats.map_pages_written_bytes, to_gb(g_ssdstats.map_pages_written_bytes));
    fprintf(stdout, "  - map pages written cnt:     %" PRIu64 " \n", g_ssdstats.map_pages_written_cnt);
    fprintf(stdout, "  - map pages read cnt:     %" PRIu64 " \n", g_ssdstats.map_pages_read_cnt);
    fprintf(stdout, "  - map max end offset:    %" PRIu64 " B (%.6f GB)\n",
            g_ssdstats.map_max_off, to_gb(g_ssdstats.map_max_off));
#else
    fprintf(stdout, "Resource report is disabled. Compile with DEBUG_FTL to enable it.\n");
#endif
    fprintf(stdout, "=====================================================\n");
}

// ========== FTL 接口：Init / Destroy / Read / Modify ==========

void FTLInit(uint64_t len)
{
    if (g)
        return;
    g = (FTL *)FTL_MALLOC_CTRL(sizeof(FTL));
    memset(g, 0, sizeof(FTL));

    const uint64_t entries_per_page = (uint64_t)EPP;
    uint64_t total_lpns = LBA_MAX_PLUS1;
    uint64_t total_mpns = (total_lpns + entries_per_page - 1ull) / entries_per_page;
    g->total_lpns = total_lpns;
    g->total_mpns = total_mpns;

#ifdef SMALL_GTD_ARRAY
    g->gtd = (uint8_t *)FTL_MALLOC_GTD(g->total_mpns);
#else
    g->gtd = (uint64_t *)FTL_MALLOC_GTD(g->total_mpns);
#endif

#ifdef USE_CMT
    g->tt_entries = MCACHE_PAGES;
    g->cmt_entries = (cmt_entry *)FTL_MALLOC_ENTRIES(g->tt_entries);
    g->free_cmt_entry_cnt = 0;
    g->used_cmt_entry_cnt = 0;
    QTAILQ_INIT(&g->free_cmt_entry_list);
    QTAILQ_INIT(&g->cmt_entry_list);

    struct cmt_entry *cmt_entry;
    for (int i = 0; i < MCACHE_PAGES; i++) {
        cmt_entry = &g->cmt_entries[i];
        cmt_entry->dirty = CLEAN;
        cmt_entry->lpn = INVALID_LPN;
        cmt_entry->ppn = UNMAPPED_PPA;
        cmt_entry->hnext = NULL;
        QTAILQ_INSERT_TAIL(&g->free_cmt_entry_list, cmt_entry, entry);
        g->free_cmt_entry_cnt++;
    }
    if(g->free_cmt_entry_cnt != g->tt_entries){
        perror("FTLInit: free_cmt_entry_cnt != tt_entries");
        exit(EXIT_FAILURE);
    }
    cachehash_init(&g->cmt_hash, CMT_HASH_SIZE);
#endif

    // 原 tpc_init(g->tpc, TPC_MAX_PAGES); 已不用
    // --- 新增：TPC 预分配 page_pool_base 并绑定到每个 TpcEntry ---
#ifndef ZERO_COPY_DMA
    size_t total_pages = (size_t)TPC_SETS * TPC_WAYS;
    g->page_pool_base = (uint8_t *)FTL_MALLOC_TPC_PAGE(total_pages);
#else
    size_t total_bytes = (size_t)TPC_SETS * TPC_WAYS * MAP_PAGE_BYTES;
    // 使用 posix_memalign 替代 malloc，强制 4096 字节对齐
    if (posix_memalign((void **)&g->page_pool_base, 4096, total_bytes) != 0) {
        perror("posix_memalign failed");
        exit(1);
    }
    memset(g->page_pool_base, 0, total_bytes);
    
    memstats_add(&g_memstats.tpc_page_used, total_bytes);
#endif
    if (unlikely(!g->page_pool_base)) { perror("malloc pool failed"); exit(1); }
    // 这里将 page_pool_base 记入 tpc_page_used
    // FTL_MALLOC_TPC_PAGE 内已经统计 tpc_page_used，无需重复计数

    for (int i = 0; i < TPC_SETS; i++) {
        g->tpc_sets[i].next_victim = 0;
        for (int j = 0; j < TPC_WAYS; j++) {
            g->tpc_sets[i].ways[j].dirty = 0;
            g->tpc_sets[i].ways[j].mpn   = MPN_SENTINEL;
#ifndef CACHE_LINE_OPTIMIZE
            g->tpc_sets[i].ways[j].buffer = g->page_pool_base +
                ((size_t)(i * TPC_WAYS + j) * MAP_PAGE_BYTES);
#else
            g->tpc_sets[i].ways[j].buf_idx = (uint32_t)(i * TPC_WAYS + j);
#endif
        }
    }

    g->fd_map = open_file("map.ssd", true);
    if (unlikely(g->fd_map < 0)) { perror("open map.ssd failed"); exit(1); }

#ifdef LAST_HIT_OPTIMIZE
    g->last_mpn = MPN_SENTINEL; 
    g->last_entry = NULL;
#endif

    memset(&g_pl, 0, sizeof(g_pl));

    // 初始化队列与 batch pool
    int total_batches = QUEUE_DEPTH + 2;
    for (int i = 0; i < total_batches; i++) {
        g_pl.free_batches[i] = (TaskBatch *)FTL_MALLOC_PIPELINE(sizeof(TaskBatch));
    }
    g_pl.free_count = total_batches;
    g_pl.head = 0;
    g_pl.tail = 0;
    g_pl.finished = 0;
    pthread_mutex_init(&g_pl.mutex, NULL);
    pthread_cond_init(&g_pl.not_empty, NULL);
    pthread_cond_init(&g_pl.not_full, NULL);

    // 可选优化：提示内核随机访问
#if defined(POSIX_FADV_RANDOM)
    posix_fadvise(g->fd_map, 0, 0, POSIX_FADV_RANDOM);
#endif
}

void FTLDestroy()
{
#ifdef FAST_DESTROY
    // 原逻辑：直接 return，跳过刷脏页和释放。若要严格销毁，可注释掉 FAST_DESTROY 宏。
    return;
#else
    if (!g) return;
#ifdef USE_CMT
    for (uint64_t i = 0; i < g->tt_entries; ++i) {
        cmt_entry *n = &g->cmt_entries[i];
        if (n->lpn != INVALID_LPN && n->dirty) {
            write_ppn_to_map_with_gtd(g, n->lpn, n->ppn);
            n->dirty = CLEAN;
        }
        n->lpn = INVALID_LPN;
        n->ppn = UNMAPPED_PPA;
        n->hnext = NULL;
    }
#endif

    // === 新 TPC：刷新所有脏页到文件 ===
    for (int i = 0; i < TPC_SETS; i++) {
        for (int j = 0; j < TPC_WAYS; j++) {
            if (g->tpc_sets[i].ways[j].mpn != MPN_SENTINEL) {
                tpc_flush_entry(g, &g->tpc_sets[i].ways[j]);
            }
        }
    }

    if (g->fd_map >= 0) {
        close(g->fd_map);
        g->fd_map = -1;
    }
#ifdef USE_CMT
    cachehash_destroy(&g->cmt_hash);
    FTL_FREE_ENTRIES(g->cmt_entries, g->tt_entries);
    g->cmt_entries = NULL;
#endif
    FTL_FREE_GTD(g->gtd, g->total_mpns);
    g->gtd = NULL;
    if (g->page_pool_base) {
        size_t total_pages = (size_t)TPC_SETS * TPC_WAYS;
        FTLFreeEx(g->page_pool_base, total_pages * MAP_PAGE_BYTES, MEM_CLASS_TPC_PAGE);
        g->page_pool_base = NULL;
    }
    FTL_FREE_CTRL(g, sizeof(FTL));
    g = NULL;
#endif
}

uint64_t FTLRead(uint64_t lba) {
#ifdef USE_CMT
    cmt_entry *n = cache_get_entry(g, lba);
    return n->ppn;
#else
    uint64_t ppn = read_ppn_from_map_with_gtd(g, lba);
    return ppn;
#endif
}

bool FTLModify(uint64_t lba, uint64_t ppn) {
#ifdef USE_CMT
    cmt_entry *n = cache_get_entry(g, lba);
    n->ppn = ppn;
    n->dirty = DIRTY;
#else
    write_ppn_to_map_with_gtd(g, lba, ppn);
#endif
    return true;
}

// ========== 下面是多线程部分 ==========

static void *WorkerThread(void *arg) {
    TaskBatch *batch;
    while (1) {
        pthread_mutex_lock(&g_pl.mutex);
        while (g_pl.head == g_pl.tail && !g_pl.finished) {
            pthread_cond_wait(&g_pl.not_empty, &g_pl.mutex);
        }

        if (g_pl.head == g_pl.tail && g_pl.finished) {
            pthread_mutex_unlock(&g_pl.mutex);
            break;
        }

        batch = g_pl.batch_queue[g_pl.head];
        g_pl.head = (g_pl.head + 1) % QUEUE_DEPTH;
        pthread_mutex_unlock(&g_pl.mutex);

        for (int i = 0; i < batch->count; i++) {
            if (batch->tasks[i].type == IO_READ) {
                uint64_t res = FTLRead(batch->tasks[i].lba);
                fprintf(g_pl.output_file, "%" PRIu64 "\n", res);
            } else {
                FTLModify(batch->tasks[i].lba, batch->tasks[i].ppn);
            }
        }

        pthread_mutex_lock(&g_pl.mutex);
        g_pl.free_batches[g_pl.free_count++] = batch;
        pthread_cond_signal(&g_pl.not_full);
        pthread_mutex_unlock(&g_pl.mutex);
    }
    return NULL;
}

// ========== 进度打印函数，保持原样 ==========

void PercentageBasedProgress(uint64_t current, uint64_t total, int* lastPercent) {
    int currentPercent = (int)((current * 100) / total);
    if (currentPercent != *lastPercent) {
        printf("\rProgress: %d%%", currentPercent);
        fflush(stdout);
        *lastPercent = currentPercent;
    }
}

// ========== AlgorithmRun：融合多线程流水线，保持接口与输出兼容 ==========

uint32_t AlgorithmRun(IOVector *ioVector, const char *outputFile) {
    uint64_t ret;
    int lastPercent = -1;

    FILE *output = fopen(outputFile, "w");
    if (!output) {
        perror("Failed to open outputFile");
        exit(EXIT_FAILURE);
    }
    
    // FTL 初始化
    FTLInit(ioVector->len);
    g_pl.output_file = output;

    // 启动单 worker 线程（FTLRead/FTLModify 在其中执行）
    pthread_create(&g_pl.worker_tid, NULL, WorkerThread, NULL);

    FILE *input = fopen(ioVector->inputFile, "r");
    if (!input) {
        perror("Failed to open inputFile");
        fclose(output);
        exit(EXIT_FAILURE);
    }
#ifdef SETVBUF
    // [新增] 设置 1MB 的输入缓冲区
    char *input_buffer = malloc(1024 * 1024);
    if (input_buffer) {
        setvbuf(input, input_buffer, _IOFBF, 1024 * 1024);
    }
#endif
    char line[256];
    fgets(line, sizeof(line), input); // 跳过头1
    fgets(line, sizeof(line), input); // 跳过头2

    // 判断是否使用多线程流水线：
    // 这里简单策略：len 大于 SMALL_INPUT_THRESHOLD 就用多线程，否则用原单线程逻辑
#ifdef SMALL_INPUT_MODE
    if (ioVector->len <= SMALL_INPUT_THRESHOLD) {
        // ======= 原单线程逻辑保留 =======
        for (uint64_t i = 0; i < ioVector->len; ++i) {
            if (!fgets(line, sizeof(line), input)) break;
            sscanf(line, "%u %llu %llu",
                   &ioVector->ioUnit.type,
                   &ioVector->ioUnit.lba,
                   &ioVector->ioUnit.ppn);
            if (ioVector->ioUnit.type == IO_READ) {
                ret = FTLRead(ioVector->ioUnit.lba);
                fprintf(output, "%llu\n", ret);
            } else {
                FTLModify(ioVector->ioUnit.lba, ioVector->ioUnit.ppn);
            }
            PercentageBasedProgress(i, ioVector->len, &lastPercent);
        }
    } else {
#endif
        // ======= 新：多线程流水线（基于 TaskBatch 方案） =======
        
        TaskBatch *current_batch = NULL;

        // 先取一个空 batch
        pthread_mutex_lock(&g_pl.mutex);
        while (g_pl.free_count == 0) pthread_cond_wait(&g_pl.not_full, &g_pl.mutex);
        current_batch = g_pl.free_batches[--g_pl.free_count];
        pthread_mutex_unlock(&g_pl.mutex);
        current_batch->count = 0;

        for (uint64_t i = 0; i < ioVector->len; ++i) {
            fgets(line, sizeof(line), input);
            sscanf(line, "%u %llu %llu", &ioVector->ioUnit.type, &ioVector->ioUnit.lba, &ioVector->ioUnit.ppn);
            TaskSimple t = {ioVector->ioUnit.type, ioVector->ioUnit.lba, ioVector->ioUnit.ppn};
            current_batch->tasks[current_batch->count++] = t;

            if (unlikely(current_batch->count == BATCH_SIZE)) {
                pthread_mutex_lock(&g_pl.mutex);
                while ((g_pl.tail + 1) % QUEUE_DEPTH == g_pl.head) {
                    pthread_cond_wait(&g_pl.not_full, &g_pl.mutex);
                }
                g_pl.batch_queue[g_pl.tail] = current_batch;
                g_pl.tail = (g_pl.tail + 1) % QUEUE_DEPTH;
                pthread_cond_signal(&g_pl.not_empty);

                while (g_pl.free_count == 0) {
                    pthread_cond_wait(&g_pl.not_full, &g_pl.mutex);
                }
                current_batch = g_pl.free_batches[--g_pl.free_count];
                pthread_mutex_unlock(&g_pl.mutex);
                current_batch->count = 0;
            }
            PercentageBasedProgress(i, ioVector->len, &lastPercent);
        }

        if (current_batch->count > 0) {
            pthread_mutex_lock(&g_pl.mutex);
            while ((g_pl.tail + 1) % QUEUE_DEPTH == g_pl.head) {
                pthread_cond_wait(&g_pl.not_full, &g_pl.mutex);
            }
            g_pl.batch_queue[g_pl.tail] = current_batch;
            g_pl.tail = (g_pl.tail + 1) % QUEUE_DEPTH;
            pthread_cond_signal(&g_pl.not_empty);
            pthread_mutex_unlock(&g_pl.mutex);
        }

        pthread_mutex_lock(&g_pl.mutex);
        g_pl.finished = 1;
        pthread_cond_broadcast(&g_pl.not_empty);
        pthread_mutex_unlock(&g_pl.mutex);

        pthread_join(g_pl.worker_tid, NULL);

        // 释放流水线资源并计入线程内存统计（FTL_MALLOC_PIPELINE 已经统计）
        // for (int i = 0; i < ioVector->len; i++) FTL_FREE_PIPELINE(g_pl.free_batches[i], sizeof(TaskBatch));
        pthread_mutex_destroy(&g_pl.mutex);
        pthread_cond_destroy(&g_pl.not_empty);
        pthread_cond_destroy(&g_pl.not_full);
#ifdef SMALL_INPUT_MODE
    }
#endif

    ssdstats_refresh_from_fs(g->fd_map);
    PrintResourceReport("AlgorithmRun summary", g);

    FTLDestroy();
#ifdef SETVBUF
    free(input_buffer);
#endif
    fclose(output);
    fclose(input);

    return RETURN_OK;
}
