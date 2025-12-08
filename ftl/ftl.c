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

// 目前发现，对于本赛题的数据，CMT没什么用，可以直接取消，保留TPC就行
//#define USE_CMT

//#define MULTI_THREADS

#define DEBUG_FTL

// 用0表示无效PPA，mpn要加1才能得到对应ppn，ppn要减1得到mpn。
// 输入ppn如果为0：暂时也视为无效ppa
#define INVALID_PPA     (0ULL)
#define INVALID_LPN     (~(0ULL))
#define UNMAPPED_PPA    (~(0ULL))

enum {
    CLEAN = 0,
    DIRTY = 1
} DIRTY_STATE;

#define MAX_MEMORY // ！！！提交到平台时需要假设有16G的内存，参数不能调太小否则评测可能会很慢！但也得设保险一点避免超内存

// 虽然说有16GB内存，但未必都能用于程序，所以留点余量。下面几个大组件加起来大概8GB
#ifdef MAX_MEMORY

#define MCACHE_PAGES (1u << 25) // 假设分配8G给CMT，每个条目是48B，约有1.8e8条目(178,956,970),
#define CMT_HASH_SIZE (1u << 27) // 考虑到CMT哈希表大小和CMT可能接近，保险起见CMT取1<<25 (33,554,432)，此时CMT大小最多1.5GB。哈希表大小是4倍但条目是8B，最多使用1GB空间
#define TPC_MAX_PAGES    (1u << 19)  // 假设分配6GB给TPC的page，大概能存1.57e6个页, 取1<<19 (524,288)。此时TPC最多使用2GB
#define TPC_HASH_SIZE    (1u << 23) // TPC的哈希表和page的大小差距就很大了，容量可以设大些

#else

#define MCACHE_PAGES (1u << 20) // Max Cache Pages in CMT
#define CMT_HASH_SIZE (1u << 22) // 最好取2的幂次方，就可将取模改为按位与
#define TPC_MAX_PAGES    (1u << 14)  // 16K 页 ≈ 64MB
#define TPC_HASH_SIZE    (1u << 18)

#endif

#define MAP_PAGE_BYTES 4096u
#define LBA_MAX_PLUS1 (1ull << 36)
#define CMT_HASH_MASK (CMT_HASH_SIZE - 1)
#define TPC_HASH_MASK (TPC_HASH_SIZE - 1)

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

// 多线程优化
#ifdef MULTI_THREADS

#include <pthread.h>
#include <sys/sysinfo.h>
#define CMT_LOCK_SHARDS 4096u
#define TPC_LOCK_SHARDS 4096u
#define CMT_LOCK_MASK (CMT_LOCK_SHARDS - 1)
#define TPC_LOCK_MASK (TPC_LOCK_SHARDS - 1)

static pthread_mutex_t cmt_locks[CMT_LOCK_SHARDS];
static pthread_mutex_t tpc_locks[TPC_LOCK_SHARDS];
static pthread_mutex_t tpc_lru_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t cmt_global_lock = PTHREAD_MUTEX_INITIALIZER;

static inline pthread_mutex_t *cmt_lock_for_lpn(uint64_t lpn) {
    return &cmt_locks[lpn & CMT_LOCK_MASK];
}

static inline pthread_mutex_t *tpc_lock_for_mpn(uint64_t mpn) {
    return &tpc_locks[mpn & TPC_LOCK_MASK];
}

#endif

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
    uint64_t tpc_used; // 包括tpc哈希表
    uint64_t tpc_page_used;
    uint64_t cmt_query_cnt;
    uint64_t cmt_hit_cnt;
    uint64_t tpc_query_cnt;
    uint64_t tpc_hit_cnt;
    uint64_t cmt_dirty_handle_cnt;
    uint64_t tpc_dirty_handle_cnt;
    #ifdef MULTI_THREADS
    uint64_t threads_used;
    #endif
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
    if (!p)
    {
        fprintf(stderr, "malloc %zu failed: %s\n", size, strerror(errno));
        exit(EXIT_FAILURE);
    }
    return p;
}

static void *FTLCalloc(size_t n, size_t size)
{
    void *p = calloc(n, size);
    if (!p)
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
    int flags = write ? (O_CREAT | O_RDWR) : O_RDONLY;
    int fd = open(path, flags, 0644);
    if (fd < 0)
    {
        perror(path);
        exit(1);
    }
    return fd;
}

// 读写：返回实际字节数或-1
static ssize_t pread_full(int fd, void *buf, size_t len, off_t off)
{
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

    g_ssdstats.map_pages_read_cnt++; // 统计读页数(写页数在ssdstats_on_write_map中统计)

    return (ssize_t)n;
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
    *field += sz;
    g_memstats.total_used += sz;
    if (g_memstats.total_used > g_memstats.peak_used)
        g_memstats.peak_used = g_memstats.total_used;
}

static inline void memstats_sub(uint64_t *field, uint64_t sz)
{
    if (*field >= sz)
        *field -= sz;
    else
        *field = 0;
    if (g_memstats.total_used >= sz)
        g_memstats.total_used -= sz;
    else
        g_memstats.total_used = 0;
}

typedef enum
{
    MEM_CLASS_CTRL = 1,
    MEM_CLASS_ENTRIES,
    MEM_CLASS_GTD,
    MEM_CLASS_CMT_HASH,
    MEM_CLASS_TPC,
    MEM_CLASS_TPC_PAGE,
    #ifdef MULTI_THREADS
    MEM_CLASS_PIPELINE
    #endif
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
    #ifdef MULTI_THREADS
    case MEM_CLASS_PIPELINE:
        memstats_add(&g_memstats.threads_used, size);
        break;
    #endif
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
    #ifdef MULTI_THREADS
    case MEM_CLASS_PIPELINE:
        memstats_add(&g_memstats.threads_used, size);
        break;
    #endif
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
    #ifdef MULTI_THREADS
    case MEM_CLASS_PIPELINE:
        memstats_sub(&g_memstats.threads_used, size);
        break;
    #endif
    default:
        memstats_sub(&g_memstats.ctrl_used, size);
        break;
    }
}

static inline void ssdstats_on_write_map(off_t offset, size_t len)
{
    uint64_t end = (uint64_t)offset + (uint64_t)len;
    if (end > g_ssdstats.map_max_off)
        g_ssdstats.map_max_off = end;
    g_ssdstats.map_pages_written_bytes += len;
    g_ssdstats.map_pages_written_cnt ++;
}

static inline void ssdstats_refresh_from_fs(int fd_map)
{
    struct stat st;
    if (fd_map >= 0 && fstat(fd_map, &st) == 0)
    {
        g_ssdstats.map_st_size = (uint64_t)st.st_size;
        g_ssdstats.map_st_blocks = (uint64_t)st.st_blocks * 512ull;
    }
}

static ssize_t pwrite_full_with_stats(int fd, const void *buf, size_t len, off_t off)
{
    ssize_t n = pwrite_full(fd, buf, len, off);
    if (n == (ssize_t)len)
        ssdstats_on_write_map(off, len);
    return n;
}

#define FTL_MALLOC_CTRL(sz_tt) FTLMallocEx((sz_tt), MEM_CLASS_CTRL)
#define FTL_CALLOC_CTRL(n, sz_per) FTLCallocEx((n), (sz_per), MEM_CLASS_CTRL)
#define FTL_MALLOC_ENTRIES(n) FTLCallocEx((n), sizeof(cmt_entry), MEM_CLASS_ENTRIES)
#define FTL_MALLOC_GTD(n) FTLCallocEx((n), sizeof(uint64_t), MEM_CLASS_GTD)
#define FTL_MALLOC_TPC(n, sz_per) FTLCallocEx((n), sz_per, MEM_CLASS_TPC)
#define FTL_MALLOC_TPC_PAGE(n) FTLCallocEx((n), MAP_PAGE_BYTES, MEM_CLASS_TPC_PAGE)
#define FTL_MALLOC_CMT_HASH_BUCKETS(n) FTLCallocEx((n), sizeof(cmt_entry *), MEM_CLASS_CMT_HASH)
#define FTL_FREE_CTRL(p, sz_tt) FTLFreeEx((p), (sz_tt), MEM_CLASS_CTRL)
#define FTL_FREE_ENTRIES(p, cap) FTLFreeEx((p), (size_t)(cap) * sizeof(cmt_entry), MEM_CLASS_ENTRIES)
#define FTL_FREE_GTD(p, cap) FTLFreeEx((p), (size_t)(cap) * sizeof(uint64_t), MEM_CLASS_GTD)
#define FTL_FREE_TPC(p, sz_tt) FTLFreeEx((p), (sz_tt), MEM_CLASS_TPC)
#define FTL_FREE_CMT_HASH_BUCKETS(p, cap) FTLFreeEx((p), (size_t)(cap) * sizeof(cmt_entry *), MEM_CLASS_CMT_HASH)
#define FTL_FREE_TPC_PAGE(p) FTLFreeEx((p), MAP_PAGE_BYTES, MEM_CLASS_TPC_PAGE)
#define SSD_PWRITE_MAP(fd, buf, len, off) pwrite_full_with_stats((fd), (buf), (len), (off))

#ifdef MULTI_THREADS
#define FTL_MALLOC_PIPELINE(sz_tt) FTLMallocEx((sz_tt), MEM_CLASS_PIPELINE)
#define FTL_FREE_PIPELINE(p, sz_tt) FTLFreeEx((p), (sz_tt), MEM_CLASS_PIPELINE)
#endif

// 映射条目
#define EPP (MAP_PAGE_BYTES / ENTRY_BYTES)

#if ENTRY_BYTES == 8u
static inline uint64_t lpn_to_mpn(uint64_t lpn) { return lpn >> 9; }
static inline uint32_t lpn_to_off(uint64_t lpn) { return (uint32_t)(lpn & 511); }
#else
static inline uint64_t lpn_to_mpn(uint64_t lpn) { return lpn / (uint64_t)EPP; }
static inline uint32_t lpn_to_off(uint64_t lpn) { return (uint32_t)(lpn % (uint64_t)EPP); }
#endif

// mpn-ppn本来可以自定义为相同，但希望将0设置为INVALID(这样GTD表初始化不需要全部赋值一遍) 所以+1
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

// TPC: Translation Page Cache,
// 重点是翻译页内部数据，额外记录了mpn-ppa等信息，可看作在OOB区域存储的元数据
typedef struct tpc_page {
    uint64_t mpn;
    uint64_t ppa;     // 翻译页的ppa（0为未分配）
    uint8_t  dirty;
    uint8_t  loaded;  // buf是否有效
    uint8_t  *buf;    // 4KB
    QTAILQ_ENTRY(tpc_page) lru_link;
    struct tpc_page *hnext;
} tpc_page;

typedef struct {
    tpc_page **tpc_hash_table;
    QTAILQ_HEAD(tpc_lru_list, tpc_page) lru_list;
    uint32_t size;
    uint32_t capacity;
} tpc;

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

    uint64_t *gtd;   // GTD, 记录mpn->mpn_ppa

    cmt_hash_table cmt_hash;
    int fd_map;

    tpc tpc;
} FTL;

static FTL *g = NULL;

static inline uint64_t tpc_hash_mpn(uint64_t mpn) { 
    return mpn & TPC_HASH_MASK;
}

static void tpc_init(tpc *c, uint32_t capacity)
{
    c->capacity = capacity;
    c->size = 0;
    c->tpc_hash_table = (tpc_page **)FTL_MALLOC_TPC(TPC_HASH_SIZE, sizeof(tpc_page *));
    QTAILQ_INIT(&c->lru_list);
    #ifdef MULTI_THREADS
    for (unsigned i = 0; i < TPC_LOCK_SHARDS; i++) 
        pthread_mutex_init(&tpc_locks[i], NULL);
    #endif
}

static void TPCDestroy(FTL *d)
{
    tpc *c = &d->tpc;
    // 遍历LRU链，写回并释放
    #ifdef MULTI_THREADS
    pthread_mutex_lock(&tpc_lru_lock);
    #endif
    tpc_page *p;
    while ((p = QTAILQ_FIRST(&c->lru_list)) != NULL)
    {
        QTAILQ_REMOVE(&c->lru_list, p, lru_link);
        #ifdef MULTI_THREADS
        pthread_mutex_unlock(&tpc_lru_lock);
        pthread_mutex_t* lk = tpc_lock_for_mpn(p->mpn);
        pthread_mutex_lock(lk);
        #endif
        if (p->dirty)
        {
            if (p->ppa == INVALID_PPA)
            {
                // 首次分配
                p->ppa = mpn_to_ppa(p->mpn);
                d->gtd[p->mpn] = p->ppa;
            }
            off_t base = (off_t)ppa_to_mpn(p->ppa) * MAP_PAGE_BYTES;
            ssize_t w = SSD_PWRITE_MAP(d->fd_map, p->buf, MAP_PAGE_BYTES, base);
            if (w != (ssize_t)MAP_PAGE_BYTES)
            {
                perror("TPC destroy writeback failed");
                exit(1);
            }
        }
        // 从哈希桶删除
        uint64_t idx = tpc_hash_mpn(p->mpn);
        tpc_page **pp = &c->tpc_hash_table[idx];
        while (*pp)
        {
            if (*pp == p)
            {
                *pp = p->hnext;
                break;
            }
            pp = &(*pp)->hnext;
        }
        FTL_FREE_TPC_PAGE(p->buf);
        FTL_FREE_TPC(p, sizeof(tpc_page));
        #ifdef MULTI_THREADS
        pthread_mutex_unlock(lk);
        pthread_mutex_lock(&tpc_lru_lock);
        #endif
    }
    #ifdef MULTI_THREADS
    pthread_mutex_unlock(&tpc_lru_lock);
    for (unsigned i = 0; i < TPC_LOCK_SHARDS; i++) 
        pthread_mutex_destroy(&tpc_locks[i]);
    #endif
    FTL_FREE_TPC(c->tpc_hash_table, TPC_HASH_SIZE * sizeof(tpc_page *));
    c->tpc_hash_table = NULL;
    c->size = 0;
}

static void tpc_writeback_page(FTL *d, tpc_page *p)
{
    if (!p || !p->dirty)
        return;
    memstats_add(&g_memstats.tpc_dirty_handle_cnt, 1);
    if (p->ppa == INVALID_PPA)
    {
        p->ppa = mpn_to_ppa(p->mpn);
        d->gtd[p->mpn] = p->ppa;
    }
    off_t base = (off_t)ppa_to_mpn(p->ppa) * MAP_PAGE_BYTES;
    ssize_t w = SSD_PWRITE_MAP(d->fd_map, p->buf, MAP_PAGE_BYTES, base);
    if (w != (ssize_t)MAP_PAGE_BYTES)
    {
        perror("TPC writeback failed");
        exit(1);
    }
    p->dirty = CLEAN;
}

static void tpc_evict_one(FTL *d)
{
    tpc *c = &d->tpc;
    #ifdef MULTI_THREADS
    pthread_mutex_lock(&tpc_lru_lock);
    #endif
    tpc_page *victim = QTAILQ_LAST(&c->lru_list);
    if (!victim) {
        #ifdef MULTI_THREADS
        pthread_mutex_unlock(&tpc_lru_lock);
        #endif
        return;
    }
    // 在持有 lru_lock 状态下，从 LRU 链表安全地剔除 victim
    QTAILQ_REMOVE(&c->lru_list, victim, lru_link);
    c->size--;
    #ifdef MULTI_THREADS
    pthread_mutex_unlock(&tpc_lru_lock);
    // 之后对该页执行写回与哈希移除：持有它的 shard 锁
    pthread_mutex_t *lk = tpc_lock_for_mpn(victim->mpn);
    pthread_mutex_lock(lk);
    #endif
    tpc_writeback_page(d, victim);
    uint64_t idx = tpc_hash_mpn(victim->mpn);
    tpc_page **pp = &c->tpc_hash_table[idx];
    while (*pp){
        if (*pp == victim){
            *pp = victim->hnext;
            break;
        }
        pp = &(*pp)->hnext;
    }
    FTL_FREE_TPC_PAGE(victim->buf);
    FTL_FREE_TPC(victim, sizeof(tpc_page));
    #ifdef MULTI_THREADS
    pthread_mutex_unlock(lk);
    #endif
}

static tpc_page *tpc_get_page(FTL *d, uint64_t mpn, int is_write)
{
    tpc *c = &d->tpc;
    #ifdef MULTI_THREADS
    pthread_mutex_t* lk = tpc_lock_for_mpn(mpn);
    pthread_mutex_lock(lk);
    #endif
    uint64_t idx = tpc_hash_mpn(mpn);
    tpc_page *p = c->tpc_hash_table[idx];
    memstats_add(&g_memstats.tpc_query_cnt, 1);
    while (p) {
        if (p->mpn == mpn) {
            memstats_add(&g_memstats.tpc_hit_cnt, 1);
            #ifdef MULTI_THREADS
            pthread_mutex_lock(&tpc_lru_lock);
            #endif
            QTAILQ_REMOVE(&c->lru_list, p, lru_link);
            QTAILQ_INSERT_HEAD(&c->lru_list, p, lru_link);
            #ifdef MULTI_THREADS
            pthread_mutex_unlock(&tpc_lru_lock);
            pthread_mutex_unlock(lk);
            #endif
            return p;
        }
        p = p->hnext;
    }
    if (c->size >= c->capacity) {
        #ifdef MULTI_THREADS
        pthread_mutex_unlock(lk);
        #endif
        tpc_evict_one(d); // TPC满了的情况
        #ifdef MULTI_THREADS
        pthread_mutex_lock(lk);
        #endif
    }
    p = (tpc_page *)FTL_MALLOC_TPC(1, sizeof(tpc_page));
    memset(p, 0, sizeof(*p));
    p->mpn = mpn;
    p->buf = (uint8_t *)FTL_MALLOC_TPC_PAGE(1);
    p->dirty = CLEAN;
    p->loaded = 0;

    // 查GTD，决定是否读取
    uint64_t ppa = d->gtd[mpn];
    p->ppa = ppa;
    if (ppa == INVALID_PPA)
    {
        // 未分配：写场景免读；读场景可视为未映射（不加载）
        if (is_write) {
            p->loaded = 1; // 零页即有效
        }
        else {
            // 保持loaded=0, 对读写场景loaded的值会导致不同的后续操作
        }
    }
    else {
        off_t base = (off_t)ppa_to_mpn(ppa) * MAP_PAGE_BYTES;
        ssize_t r = pread_full(d->fd_map, p->buf, MAP_PAGE_BYTES, base);
        if (r < 0) {
            perror("TPC pread page failed");
            exit(1);
        }
        p->loaded = 1;
    }

    p->hnext = c->tpc_hash_table[idx];
    c->tpc_hash_table[idx] = p;
    #ifdef MULTI_THREADS
    pthread_mutex_lock(&tpc_lru_lock);
    #endif
    QTAILQ_INSERT_HEAD(&c->lru_list, p, lru_link);
    #ifdef MULTI_THREADS
    pthread_mutex_unlock(&tpc_lru_lock);
    pthread_mutex_unlock(lk);
    #endif
    c->size++;
    return p;
}

static void PrintResourceReport(const char *title, FTL *d)
{
    fprintf(stdout, "\n================ Resource Report: %s ================\n", title);
    fprintf(stdout, "Configures:\n");
    #ifdef MULTI_THREADS
    fprintf(stdout, "  - Multi-threads: ON \n");
    #else
    fprintf(stdout, "  - Multi-threads: OFF \n");
    #endif
    fprintf(stdout, "  - Total(Max) LPNS:     %" PRIu64 " pages (= %.6f GB)\n", d->total_lpns, to_gb(d->total_lpns * 4096ull));
    fprintf(stdout, "  - Max Cache Pages (Entries) in CMT:   %" PRIu64 " entries ( %" PRIu64 " B per entry) (about %.6f GB)\n", d->tt_entries, sizeof(cmt_entry), to_gb(d->tt_entries * sizeof(cmt_entry)));
    fprintf(stdout, "  - CMT Hash Table Size:    %" PRIu64 " buckets\n", CMT_HASH_SIZE);
    fprintf(stdout, "  - GTD Size:     %" PRIu64 " entries (= %.6f GB)\n", d->total_mpns, to_gb(d->total_mpns * sizeof(uint64_t)));
    fprintf(stdout, "  - TPC Max Pages:    %" PRIu64 " pages (= %.6f GB)\n", d->tpc.capacity, to_gb((uint64_t)d->tpc.capacity * MAP_PAGE_BYTES));
    fprintf(stdout, "  - TPC Hash Table Size:    %" PRIu64 " buckets\n", TPC_HASH_SIZE);

    uint64_t total = g_memstats.total_used;
    fprintf(stdout, "Heap Memory (current / peak): %" PRIu64 " B (%.6f GB) / %" PRIu64 " B (%.6f GB)\n",
            total, to_gb(total), g_memstats.peak_used, to_gb(g_memstats.peak_used));
    fprintf(stdout, "  - Control structures:   %" PRIu64 " B (%.6f GB)\n", g_memstats.ctrl_used, to_gb(g_memstats.ctrl_used));
    fprintf(stdout, "  - CMT entries:      %" PRIu64 " B (%.6f GB)\n", g_memstats.cmt_entrys_used, to_gb(g_memstats.cmt_entrys_used));
    fprintf(stdout, "  - GTD uses:      %" PRIu64 " B (%.6f GB)\n", g_memstats.gtd_used, to_gb(g_memstats.gtd_used));
    fprintf(stdout, "  - CMT hash table:   %" PRIu64 " B (%.6f GB)\n", g_memstats.cmt_hash_used, to_gb(g_memstats.cmt_hash_used));
    fprintf(stdout, "  - free cmt entry cnt:     %" PRIu64 " , %" PRIu64 " B (%.6f GB)\n", d->free_cmt_entry_cnt, d->free_cmt_entry_cnt * sizeof(cmt_entry), to_gb(d->free_cmt_entry_cnt * sizeof(cmt_entry)));
    fprintf(stdout, "  - used cmt entry cnt:     %" PRIu64 " , %" PRIu64 " B (%.6f GB)\n", d->used_cmt_entry_cnt, d->used_cmt_entry_cnt * sizeof(cmt_entry), to_gb(d->used_cmt_entry_cnt * sizeof(cmt_entry)));
    fprintf(stdout, "  - CMT hit ratio:     %.6f (%" PRIu64 " / %" PRIu64 ")\n", (double) g_memstats.cmt_hit_cnt / g_memstats.cmt_query_cnt, g_memstats.cmt_hit_cnt, g_memstats.cmt_query_cnt);
    fprintf(stdout, "  - TPC hit ratio:     %.6f (%" PRIu64 " / %" PRIu64 ")\n", (double) g_memstats.tpc_hit_cnt / g_memstats.tpc_query_cnt, g_memstats.tpc_hit_cnt, g_memstats.tpc_query_cnt);
    fprintf(stdout, "  - CMT dirty entry handle cnt (not including FTLDestory):     %" PRIu64 "\n", g_memstats.cmt_dirty_handle_cnt);
    fprintf(stdout, "  - TPC dirty entry handle cnt (not including FTLDestory):     %" PRIu64 "\n", g_memstats.tpc_dirty_handle_cnt);
    fprintf(stdout, "  - TPC uses (include TPC hash table):      %" PRIu64 " B (%.6f GB)\n", g_memstats.tpc_used, to_gb(g_memstats.tpc_used));
    fprintf(stdout, "  - TPC pages:    %" PRIu64 " pages,  %" PRIu64 " B (%.6f GB)\n", d->tpc.size, g_memstats.tpc_page_used, to_gb(g_memstats.tpc_page_used));
    #ifdef MULTI_THREADS
    fprintf(stdout, "  - Multi-threads used:    %" PRIu64 " B (%.6f GB)\n", g_memstats.threads_used, to_gb(g_memstats.threads_used));
    #endif

    fprintf(stdout, "SSD Usage (from filesystem stat):\n");
    fprintf(stdout, "  - map.ssd size:   %" PRIu64 " B (%.6f GB), blocks: %" PRIu64 " B (%.6f GB)\n",
            g_ssdstats.map_st_size, to_gb(g_ssdstats.map_st_size),
            g_ssdstats.map_st_blocks, to_gb(g_ssdstats.map_st_blocks));

    fprintf(stdout, "SSD Logical write accounting (program-side):\n");
    fprintf(stdout, "  - map pages written:     %" PRIu64 " B (%.6f GB)\n", g_ssdstats.map_pages_written_bytes, to_gb(g_ssdstats.map_pages_written_bytes));
    fprintf(stdout, "  - map pages written cnt:     %" PRIu64 " \n", g_ssdstats.map_pages_written_cnt);
    fprintf(stdout, "  - map pages read cnt:     %" PRIu64 " \n", g_ssdstats.map_pages_read_cnt);
    fprintf(stdout, "  - map max end offset:    %" PRIu64 " B (%.6f GB)\n", g_ssdstats.map_max_off, to_gb(g_ssdstats.map_max_off));
    fprintf(stdout, "=====================================================\n");
}

static uint64_t gtd_get_ppa_for_mpn(FTL *d, uint64_t mpn) { 
    if (mpn >= d->total_mpns) { 
        fprintf(stderr, "gtd_get_ppa_for_mpn: mpn out of range\n"); 
        exit(1); 
    } 
    return d->gtd[mpn];
}

// 从TPC读取lpn对应ppn（未分配则返回UNMAPPED）
static uint64_t read_ppn_from_map_with_gtd(FTL *d, uint64_t lpn)
{
    uint64_t mpn = lpn_to_mpn(lpn);
    uint32_t off = lpn_to_off(lpn);
    tpc_page *pg = tpc_get_page(d, mpn, 0);
    if (!pg->loaded)
    {
        // 页不存在（未分配），视为未映射
        return UNMAPPED_PPA;
    }
    uint64_t v = entry_load_u64(pg->buf, off);
    if (v == 0) return UNMAPPED_PPA; // 0 表示未映射
    return v;
    //return entry_load_u64(pg->buf, off);
}

// 写入条目到TPC页，必要时分配映射页并在写回时统一刷盘
static void write_ppn_to_map_with_gtd(FTL *d, uint64_t lpn, uint64_t ppn)
{
    uint64_t mpn = lpn_to_mpn(lpn);
    uint32_t off = lpn_to_off(lpn);
    tpc_page *pg = tpc_get_page(d, mpn, 1);
    if (!pg->loaded) {
        // 页缓存初始化
        memset(pg->buf, 0, MAP_PAGE_BYTES);
        pg->loaded = 1;
    }
    entry_store_u64(pg->buf, off, ppn);
    pg->dirty = DIRTY;
}

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
    // 这里哈希表的销毁做法不一定彻底，不过应该不影响最终结果
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

// 写回并释放页（不移除结构）
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
    #ifdef MULTI_THREADS
    pthread_mutex_lock(&cmt_global_lock);
    #endif
    cmt_entry *n = cachehash_find(&d->cmt_hash, lpn);
    if (n) {
        QTAILQ_REMOVE(&d->cmt_entry_list, n, entry);
        QTAILQ_INSERT_HEAD(&d->cmt_entry_list, n, entry);
        #ifdef MULTI_THREADS
        pthread_mutex_unlock(&cmt_global_lock);
        #endif
        return n;
    }
    // 未命中：1.从CMT获取一个空闲/淘汰节点 2.从GTD获取ppn 3.绑定
    cmt_entry *e;
    if (!QTAILQ_EMPTY(&d->free_cmt_entry_list)) {
        e = QTAILQ_FIRST(&d->free_cmt_entry_list);
        QTAILQ_REMOVE(&d->free_cmt_entry_list, e, entry);
        d->free_cmt_entry_cnt--;
    } else {
        e = QTAILQ_LAST(&d->cmt_entry_list);
        if (!e) {
            #ifdef MULTI_THREADS
            pthread_mutex_unlock(&cmt_global_lock);
            #endif
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
    #ifdef MULTI_THREADS
    pthread_mutex_unlock(&cmt_global_lock);
    #endif
    return e;
}

// 生命周期与API
void FTLInit()
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

    g->gtd = (uint64_t *)FTL_MALLOC_GTD(g->total_mpns);
    //memset(g->gtd, 0, g->total_mpns * sizeof(uint64_t));
    
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

    tpc_init(&g->tpc, TPC_MAX_PAGES);

    g->fd_map = open_file("map.ssd", true);
}

void FTLDestroy()
{
    if (!g) return;
    #ifdef USE_CMT
    for (uint64_t i = 0; i < g->tt_entries; ++i) {
        cmt_entry *n = &g->cmt_entries[i];
        if (n->lpn != INVALID_LPN && n->dirty) {
            // memstats_add(&g_memstats.cmt_dirty_handle_cnt, 1);
            write_ppn_to_map_with_gtd(g, n->lpn, n->ppn);
            n->dirty = CLEAN;
        }
        n->lpn = INVALID_LPN;
        n->ppn = UNMAPPED_PPA;
        n->hnext = NULL;
    }
    #endif

    TPCDestroy(g);

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
    FTL_FREE_CTRL(g, sizeof(FTL));
    g = NULL;
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

#ifdef MULTI_THREADS

typedef struct {
    uint32_t type;
    uint64_t lba;
    uint64_t ppn;
    uint64_t seq; // 读结果序号
} Task;

#define TASK_QUEUE_CAP (1u<<18)

typedef struct {
    Task* buf;
    uint32_t cap;
    uint32_t head, tail, cnt;
    pthread_mutex_t mu;
    pthread_cond_t cv_not_full;
    pthread_cond_t cv_not_empty;
    int closed;
} TaskQueue;

static void tq_init(TaskQueue* q, uint32_t cap){
    q->buf = (Task*)FTL_MALLOC_PIPELINE(cap * sizeof(Task));
    q->cap = cap; q->head=0; q->tail=0; q->cnt=0; q->closed=0;
    pthread_mutex_init(&q->mu, NULL);
    pthread_cond_init(&q->cv_not_full, NULL);
    pthread_cond_init(&q->cv_not_empty, NULL);
}

static void tq_destroy(TaskQueue* q){
    FTL_FREE_PIPELINE(q->buf, q->cap * sizeof(Task));
    pthread_mutex_destroy(&q->mu);
    pthread_cond_destroy(&q->cv_not_full);
    pthread_cond_destroy(&q->cv_not_empty);
}

static int tq_push(TaskQueue* q, Task t){
    pthread_mutex_lock(&q->mu);
    while(q->cnt == q->cap && !q->closed) {
        pthread_cond_wait(&q->cv_not_full, &q->mu);
    }
    if (q->closed) { 
        pthread_mutex_unlock(&q->mu); 
        return 0; 
    }
    q->buf[q->tail] = t;
    q->tail = (q->tail + 1) % q->cap;
    q->cnt++;
    pthread_cond_signal(&q->cv_not_empty);
    pthread_mutex_unlock(&q->mu);
    return 1;
}

static int tq_pop(TaskQueue* q, Task* out){
    pthread_mutex_lock(&q->mu);
    while(q->cnt == 0 && !q->closed) {
        pthread_cond_wait(&q->cv_not_empty, &q->mu);
    }
    if (q->cnt == 0 && q->closed) { 
        pthread_mutex_unlock(&q->mu); 
        return 0; 
    }
    *out = q->buf[q->head];
    q->head = (q->head + 1) % q->cap;
    q->cnt--;
    pthread_cond_signal(&q->cv_not_full);
    pthread_mutex_unlock(&q->mu);
    return 1;
}

static void tq_close(TaskQueue* q){
    pthread_mutex_lock(&q->mu);
    q->closed = 1;
    pthread_cond_broadcast(&q->cv_not_empty);
    pthread_cond_broadcast(&q->cv_not_full);
    pthread_mutex_unlock(&q->mu);
}

// 可调窗口规模：W = 2^PIPELINE_WIN_SHIFT 槽位
// 建议 22~26 范围：22->4M，24->16M，26->64M。默认 26（约 0.9GB 结果+就绪+ticket）。
#ifndef PIPELINE_WIN_SHIFT
#define PIPELINE_WIN_SHIFT 26
#endif
#define PIPELINE_WIN_SIZE ((uint64_t)1ULL << PIPELINE_WIN_SHIFT)
#define PIPELINE_WIN_MASK (PIPELINE_WIN_SIZE - 1ULL)

typedef struct {
    TaskQueue* q;
    // 固定环形窗口（大小 = 2^PIPELINE_WIN_SHIFT）
    // 每槽位：结果、就绪标记、当前 ticket（周期编号）
    uint64_t   *results;   // size = W
    uint8_t    *ready;     // size = W
    uint32_t   *ticket;    // size = W
    uint64_t    window_size;   // = W
    uint64_t    window_start;  // 下一个需要flush的seq
    uint64_t    total_ios;
    pthread_mutex_t res_mu;
    pthread_cond_t  res_cv;
} Pipeline;


static inline void pipeline_publish_read(Pipeline* pl, uint64_t seq, uint64_t value){
    uint64_t slot = seq & PIPELINE_WIN_MASK;
    uint32_t need_ticket = (uint32_t)(seq >> PIPELINE_WIN_SHIFT);
    pthread_mutex_lock(&pl->res_mu);
    pl->results[slot] = value;
    pl->ready[slot]   = 1;
    pl->ticket[slot]  = need_ticket; // 与 flush 中的 need_ticket 匹配
    pthread_cond_signal(&pl->res_cv);
    pthread_mutex_unlock(&pl->res_mu);
}

typedef struct {
    Pipeline*  pl;     
    TaskQueue* q;
} WorkerCtx;

static void* worker_main(void* arg){
    WorkerCtx* ctx = (WorkerCtx*)arg;
    Pipeline* pl = ctx->pl;
    TaskQueue* q = ctx->q;
    Task t;
    while (tq_pop(q, &t)){
        if (t.type == IO_READ){
            uint64_t ret = FTLRead(t.lba);
            pipeline_publish_read(pl, t.seq, ret);
        } else {
            (void)FTLModify(t.lba, t.ppn);
        }
    }
    return NULL;
}

// 简单的队列选择：按 mpn 分配，稳定且实现简单
static inline uint32_t pick_qid_by_mpn(uint64_t lba, uint32_t qnum){
    uint64_t mpn = (lba >> 9);
    return (uint32_t)(mpn % qnum);
}

// 根据CPU核数选择线程数，限制最大并发
static int choose_worker_count(void){
    int ncpu = get_nprocs();
    if (ncpu <= 0) ncpu = 4;
    int n = ncpu * 2;
    if (n > 16) n = 16;
    if (n < 2) n = 2;
    return n;
}
#endif

uint32_t AlgorithmRun(IOVector *ioVector, const char *outputFile) {
    struct timeval start, end;
    long seconds, useconds;
    double during_us;

    uint64_t ret;
    FILE *file = fopen(outputFile, "w");
    if (!file)
    {
        perror("Failed to open outputFile");
        exit(EXIT_FAILURE);
    }

    FTLInit();

    gettimeofday(&start, NULL);

    #ifdef MULTI_THREADS
    Pipeline pl;
    memset(&pl, 0, sizeof(pl));
    pl.total_ios   = ioVector->len;
    pl.window_size = PIPELINE_WIN_SIZE;
    pl.window_start= 0;
    pl.results = (uint64_t *)FTL_MALLOC_PIPELINE(pl.window_size * sizeof(uint64_t));
    pl.ready   = (uint8_t  *)FTL_MALLOC_PIPELINE(pl.window_size * sizeof(uint8_t));
    pl.ticket  = (uint32_t *)FTL_MALLOC_PIPELINE(pl.window_size * sizeof(uint32_t));
    if (!pl.results || !pl.ready || !pl.ticket) {
        fprintf(stderr, "pipeline alloc failed\n");
        return 1;
    }
    memset(pl.ready,  0, pl.window_size * sizeof(uint8_t));
    memset(pl.ticket, 0, pl.window_size * sizeof(uint32_t));
    pthread_mutex_init(&pl.res_mu, NULL);
    pthread_cond_init(&pl.res_cv, NULL);
    
    int nworkers = choose_worker_count();
    uint32_t qnum = (uint32_t)nworkers;
    TaskQueue* qs = (TaskQueue*)FTL_MALLOC_PIPELINE(qnum * sizeof(TaskQueue));
    for (uint32_t qi=0; qi<qnum; ++qi) tq_init(&qs[qi], TASK_QUEUE_CAP);

    pthread_t *tids = (pthread_t *)FTL_MALLOC_PIPELINE(nworkers * sizeof(pthread_t));
    WorkerCtx *ctxs = (WorkerCtx *)FTL_MALLOC_PIPELINE(nworkers * sizeof(WorkerCtx));
    for (int i=0;i<nworkers;++i){
        ctxs[i].pl = &pl;
        ctxs[i].q  = &qs[i];
        pthread_create(&tids[i], NULL, worker_main, &ctxs[i]);
    }
    #endif

    #ifndef MULTI_THREADS
    for (uint64_t i = 0; i < ioVector->len; ++i) {
        if (ioVector->ioArray[i].type == IO_READ) {
            ret = FTLRead(ioVector->ioArray[i].lba);
            fprintf(file, "%llu\n", ret);
        }
        else {
            FTLModify(ioVector->ioArray[i].lba, ioVector->ioArray[i].ppn);
        }
        #ifdef DEBUG_FTL
        if((i & ((1u<<20)-1)) == 0 || i == ioVector->len - 1) {
            printf("\rProcessing IO %u / %llu", i + 1, ioVector->len);
            fflush(stdout);
        }
        #endif
    }
    #else

    // 生产：按 mpn 分配队列
    for (uint64_t i = 0; i < ioVector->len; ++i) {
        Task t;
        t.type = ioVector->ioArray[i].type;
        t.lba = ioVector->ioArray[i].lba;
        t.ppn = ioVector->ioArray[i].ppn;
        t.seq = i;
        uint32_t qid = pick_qid_by_mpn(t.lba, qnum);
        if (!tq_push(&qs[qid], t)) {
            break;
        }
        #ifdef DEBUG_FTL
        if ((i & ((1u << 20) - 1)) == 0 || i == ioVector->len - 1) {
            printf("\rEnqueued IO %u / %lu", (unsigned)(i + 1), ioVector->len);
            fflush(stdout);
        }
        #endif
    }
    for (uint32_t qi = 0; qi < qnum; ++qi)
        tq_close(&qs[qi]);

    // 先 flush：按序输出 READ 结果；WRITE 直接推进（并更新 ticket）
    pthread_mutex_lock(&pl.res_mu);
    uint64_t next_seq = 0;
    while (next_seq < ioVector->len) {
        uint64_t slot = next_seq & PIPELINE_WIN_MASK;
        uint32_t need_ticket = (uint32_t)(next_seq >> PIPELINE_WIN_SHIFT);
        if (ioVector->ioArray[next_seq].type != IO_READ) {
            // 写请求：不输出结果。推进周期，允许后续 seq 复用该槽位
            pl.ready[slot] = 0;
            pl.ticket[slot] = need_ticket + 1;
            next_seq++;
        }
        else {
            // 读请求：等待该槽位在当前周期就绪
            while (!(pl.ticket[slot] == need_ticket && pl.ready[slot] == 1)) {
                pthread_cond_wait(&pl.res_cv, &pl.res_mu);
            }
            uint64_t ret = pl.results[slot];
            fprintf(file, "%" PRIu64 "\n", ret);
            // 清理并推进周期
            pl.ready[slot] = 0;
            pl.ticket[slot] = need_ticket + 1;
            next_seq++;
        }
    }
    pthread_mutex_unlock(&pl.res_mu);
    // flush 完成后等待 worker 退出
    for (int i = 0; i < nworkers; i++)
        pthread_join(tids[i], NULL);
    #endif

    gettimeofday(&end, NULL);

    ssdstats_refresh_from_fs(g->fd_map);
    PrintResourceReport("AlgorithmRun summary", g);

    #ifdef MULTI_THREADS
    FTL_FREE_PIPELINE(qs, qnum * sizeof(TaskQueue));
    FTL_FREE_PIPELINE(ctxs, nworkers * sizeof(WorkerCtx));
    FTL_FREE_PIPELINE(pl.results, pl.window_size * sizeof(uint64_t));
    FTL_FREE_PIPELINE(pl.ready,   pl.window_size * sizeof(uint8_t));
    FTL_FREE_PIPELINE(pl.ticket,  pl.window_size * sizeof(uint32_t));
    pthread_mutex_destroy(&pl.res_mu);
    pthread_cond_destroy(&pl.res_cv);
    #endif

    FTLDestroy();
    fclose(file);

    seconds = end.tv_sec - start.tv_sec;
    useconds = end.tv_usec - start.tv_usec;
    during_us = ((seconds) * 1000000.0 + useconds);
    printf("algorithmRunningDuration:\t %f us\n", during_us);

    return RETURN_OK;
}