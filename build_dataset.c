#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>

#define WRITE_RATIO     0.7     // 写占 70%，读占 30%
#define IO_NUM          250000000   // 总 IO 数量
#define LPN_RANGE       300000000   // 逻辑地址范围略大于 IO 数量
#define HASH_SIZE       400000003   // 哈希表大小（取质数）
#define REPORT_INTERVAL 10000000    // 输出进度间隔

typedef struct HashEntry {
    uint64_t lpn;
    uint64_t ppn;
    struct HashEntry *next;
} HashEntry;

static HashEntry **hashTable = NULL;

// 简单哈希函数
static inline uint64_t hash(uint64_t key) {
    return key % HASH_SIZE;
}

// 插入键值对（写入 LPN→PPN）
void insert(uint64_t lpn, uint64_t ppn) {
    uint64_t h = hash(lpn);
    HashEntry *entry = (HashEntry *)malloc(sizeof(HashEntry));
    entry->lpn = lpn;
    entry->ppn = ppn;
    entry->next = hashTable[h];
    hashTable[h] = entry;
}

// 查找键值（读操作时使用）
uint64_t lookup(uint64_t lpn) {
    uint64_t h = hash(lpn);
    HashEntry *entry = hashTable[h];
    while (entry) {
        if (entry->lpn == lpn) return entry->ppn;
        entry = entry->next;
    }
    return (uint64_t)-1;
}

// 生成随机64位
uint64_t rand64() {
    return ((uint64_t)rand() << 32) ^ rand();
}

int main() {
    srand((unsigned)time(NULL));
    hashTable = (HashEntry **)calloc(HASH_SIZE, sizeof(HashEntry *));
    if (!hashTable) {
        printf("[Error] cannot allocate hashTable\n");
        return -1;
    }

    FILE *trace = fopen("trace.txt", "w");
    FILE *result = fopen("read_result.txt", "w");
    if (!trace || !result) {
        printf("[Error] cannot open output files\n");
        return -1;
    }

    fprintf(trace, "io count\n%u\n", IO_NUM);

    uint64_t writeCount = 0, readCount = 0;

    // --- 第一步：先生成所有写请求 ---
    uint64_t totalWrites = (uint64_t)(IO_NUM * WRITE_RATIO);
    for (uint64_t i = 0; i < totalWrites; i++) {
        uint64_t lpn = rand64() % LPN_RANGE;
        uint64_t ppn = rand64() % (LPN_RANGE * 2);

        // 确保 LPN != PPN 且 LPN 唯一
        while (lookup(lpn) != (uint64_t)-1 || lpn == ppn) {
            lpn = rand64() % LPN_RANGE;
            ppn = rand64() % (LPN_RANGE * 2);
        }

        insert(lpn, ppn);
        fprintf(trace, "1 %llu %llu\n", lpn, ppn);

        if ((i + 1) % REPORT_INTERVAL == 0)
            printf("[Write] Generated %llu / %llu\n", i + 1, totalWrites);
        writeCount++;
    }

    // --- 第二步：生成读请求 ---
    uint64_t totalReads = IO_NUM - totalWrites;
    for (uint64_t i = 0; i < totalReads; i++) {
        // 从已写入集合中随机选一个 LPN
        uint64_t idx = rand() % HASH_SIZE;
        HashEntry *entry = hashTable[idx];
        while (!entry) {
            idx = (idx + 1) % HASH_SIZE;
            entry = hashTable[idx];
        }

        // 随机遍历桶链表
        int step = rand() % 4;
        while (entry && step--) entry = entry->next;

        if (!entry) continue;

        uint64_t lpn = entry->lpn;
        uint64_t ppn = lookup(lpn);
        fprintf(trace, "0 %llu %llu\n", lpn, ppn);
        fprintf(result, "%llu\n", ppn);

        if ((i + 1) % REPORT_INTERVAL == 0)
            printf("[Read] Generated %llu / %llu\n", i + 1, totalReads);
        readCount++;
    }

    fclose(trace);
    fclose(result);
    printf("Done. Writes=%llu Reads=%llu\n", writeCount, readCount);

    return 0;
}
