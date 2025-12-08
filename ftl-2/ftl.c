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

#define DEBUG_FTL

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
    MEM_CLASS_TPC_PAGE
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
}

static void TPCDestroy(FTL *d)
{
    tpc *c = &d->tpc;
    // 遍历LRU链，写回并释放
    tpc_page *p;
    while ((p = QTAILQ_FIRST(&c->lru_list)) != NULL)
    {
        QTAILQ_REMOVE(&c->lru_list, p, lru_link);
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
    }

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
    tpc_page *victim = QTAILQ_LAST(&c->lru_list);
    if (!victim)
        return;
    tpc_writeback_page(d, victim);
    QTAILQ_REMOVE(&c->lru_list, victim, lru_link);
    c->size--;
    uint64_t idx = tpc_hash_mpn(victim->mpn);
    tpc_page **pp = &c->tpc_hash_table[idx];
    while (*pp)
    {
        if (*pp == victim)
        {
            *pp = victim->hnext;
            break;
        }
        pp = &(*pp)->hnext;
    }
    FTL_FREE_TPC_PAGE(victim->buf);
    FTL_FREE_TPC(victim, sizeof(tpc_page));
}

static tpc_page *tpc_get_page(FTL *d, uint64_t mpn, int is_write)
{
    tpc *c = &d->tpc;
    uint64_t idx = tpc_hash_mpn(mpn);
    tpc_page *p = c->tpc_hash_table[idx];
    memstats_add(&g_memstats.tpc_query_cnt, 1);
    while (p) {
        if (p->mpn == mpn) {
            memstats_add(&g_memstats.tpc_hit_cnt, 1);
            QTAILQ_REMOVE(&c->lru_list, p, lru_link);
            QTAILQ_INSERT_HEAD(&c->lru_list, p, lru_link);
            return p;
        }
        p = p->hnext;
    }
    if (c->size >= c->capacity) {
        tpc_evict_one(d); // TPC满了的情况
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
    QTAILQ_INSERT_HEAD(&c->lru_list, p, lru_link);
    c->size++;
    return p;
}

static void PrintResourceReport(const char *title, FTL *d)
{
    fprintf(stdout, "\n================ Resource Report: %s ================\n", title);
    fprintf(stdout, "Configures:\n");
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
    return entry_load_u64(pg->buf, off);
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
    cmt_entry *n = cachehash_find(&d->cmt_hash, lpn);
    if (n) {
        QTAILQ_REMOVE(&d->cmt_entry_list, n, entry);
        QTAILQ_INSERT_HEAD(&d->cmt_entry_list, n, entry);
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

    tpc_init(&g->tpc, TPC_MAX_PAGES);

    g->fd_map = open_file("map.ssd", true);
}

void FTLDestroy()
{
    if (!g) return;

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

    TPCDestroy(g);

    if (g->fd_map >= 0) {
        close(g->fd_map);
        g->fd_map = -1;
    }

    cachehash_destroy(&g->cmt_hash);
    FTL_FREE_ENTRIES(g->cmt_entries, g->tt_entries);
    g->cmt_entries = NULL;
    FTL_FREE_GTD(g->gtd, g->total_mpns); 
    g->gtd = NULL;
    FTL_FREE_CTRL(g, sizeof(FTL));
    g = NULL;
}

uint64_t FTLRead(uint64_t lba) {
    cmt_entry *n = cache_get_entry(g, lba);
    return n->ppn;
}

bool FTLModify(uint64_t lba, uint64_t ppn) {
    cmt_entry *n = cache_get_entry(g, lba);
    n->ppn = ppn;
    n->dirty = DIRTY;
    return true;
}

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

    gettimeofday(&end, NULL);

    ssdstats_refresh_from_fs(g->fd_map);
    PrintResourceReport("AlgorithmRun summary", g);

    FTLDestroy();
    fclose(file);

    seconds = end.tv_sec - start.tv_sec;
    useconds = end.tv_usec - start.tv_usec;
    during_us = ((seconds) * 1000000.0 + useconds);
    printf("algorithmRunningDuration:\t %f us\n", during_us);

    return RETURN_OK;
}