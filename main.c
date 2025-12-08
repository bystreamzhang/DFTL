/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/time.h>
#include "public.h"
#include "ftl/ftl.h"

#define MAX_LINE_LENGTH 256

/* 读取文件内容并解析 */
int ParseFile(const char *filename, IOVector *ioVector)
{
    FILE *file = fopen(filename, "r");
    if (!file) {
        perror("[Error] Failed to open file");
        return RETURN_ERROR;
    }

    char line[256];
    int64_t ioCount = 0;
    IOUnit *ioArray = NULL;

    while (fgets(line, sizeof(line), file)) {
        if (strncmp(line, "io count", 8) == 0) {
            if (fgets(line, sizeof(line), file)) {
                sscanf(line, "%u", &ioVector->len);
                ioArray = (IOUnit *)malloc(ioVector->len * sizeof(IOUnit));
                printf("input io count = %u\n", ioVector->len);
            }
        } else {
            IOUnit io;
            sscanf(line, "%u %llu %llu", &io.type, &io.lba, &io.ppn);
            ioArray[ioCount] = io;
            ioCount++;
        }
    }
    fclose(file);

    if (ioVector->len != ioCount) {
        printf("[Error] len(%lld) != ioCount(%lld)\n", ioVector->len, ioCount);
        return RETURN_ERROR;
    }
    if (ioVector->len > MAX_IO_NUM) {
        printf("[Error] IO number(%u) should less than %u\n", ioVector->len, MAX_IO_NUM);
        return RETURN_ERROR;
    }
    ioVector->ioArray = ioArray;

    return RETURN_OK;
}

/* 获取测试读IO数量 */
int GetIOCount(const char *filename1, const char *filename2)
{
    FILE *file1 = fopen(filename1, "r");
    FILE *file2 = fopen(filename2, "r");
    if (!file1 || !file2) {
        fprintf(stderr, "[Error] Opening files failed: %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }
    char line1[MAX_LINE_LENGTH], line2[MAX_LINE_LENGTH];
    uint64_t ioCount1 = 0;
    uint64_t ioCount2 = 0;

    while (fgets(line1, sizeof(line1), file1) != NULL) {
        ioCount1++;
    }
    while (fgets(line2, sizeof(line2), file2) != NULL) {
        ioCount2++;
    }

    if (ioCount1 != ioCount2) {
        return RETURN_ERROR;
    }
    return ioCount1;
}

/* 打印关键指标 */
void PrintMetrics(const KeyMetrics *metrics)
{
    printf("\nKey Metrics:\n");
    printf("\ttestIOCount:\t\t\t %u\n", metrics->testIOCount);
    printf("\talgorithmRunningDuration:\t %.3f (ms)\n", metrics->algorithmRunningDuration);
    printf("\taccuracy:\t\t\t %.2f\n", metrics->accuracy);
    printf("\tmemoryUse:\t\t\t %ld (KB)\n", metrics->memoryUse);
}

/* 将 KeyMetrics 结构体的内容保存到 TXT 文件 */
void SaveKeyMetricsToFile(const char *filename, const KeyMetrics *metrics)
{
    FILE *file = fopen(filename, "w");
    if (file == NULL) {
        perror("Error opening file");
        return;
    }

    fprintf(file, "/* 关键指标结构体 */\n");
    fprintf(file, "testIOCount: %u \n", metrics->testIOCount);
    fprintf(file, "algorithmRunningDuration(ms): %.2f \n", metrics->algorithmRunningDuration);
    fprintf(file, "memoryUse(ms): %lu \n", metrics->memoryUse);
    fprintf(file, "accuracy: %.2f \n", metrics->accuracy);

    fclose(file);
    printf("\n指标写入文件 %s\n", filename);
}

double CompareFiles(const char *filename1, const char *filename2)
{
    FILE *file1 = fopen(filename1, "r");
    FILE *file2 = fopen(filename2, "r");
    if (!file1 || !file2) {
        fprintf(stderr, "[Error] Opening files failed: %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }

    char line1[MAX_LINE_LENGTH], line2[MAX_LINE_LENGTH];
    uint64_t num1, num2;
    unsigned long totalLines = 0;
    unsigned long matchingLines = 0;
    double accuracy = 0.0;

    while (1) {
        if (fgets(line1, sizeof(line1), file1)) {
            if (!fgets(line2, sizeof(line2), file2)) {
                fprintf(stderr, "[Error] Output File have different number of lines\n");
                return RETURN_ERROR;
                break;
            }
            num1 = strtoull(line1, NULL, 10);
            num2 = strtoull(line2, NULL, 10);
            totalLines++;
            if (num1 == num2) {
                matchingLines++;
            } else {
                // printf("Mismatch at line %lu: %lu != %lu\n", totalLines, num1, num2);
            }
        } else {
            if (fgets(line2, sizeof(line2), file2)) {
                fprintf(stderr, "[Error] Output File have different number of lines\n");
                return RETURN_ERROR;
            }
            break;
        }
    }

    if (totalLines > 0) {
        accuracy = (double)matchingLines / totalLines * 100.0;
        // printf("Comparison results:\n");
        // printf("Total lines: %lu\n", totalLines);
        // printf("Matching lines: %lu\n", matchingLines);
        // printf("Accuracy: %.2f%%\n", accuracy);
    } else {
        printf("[Error] No lines to compare\n");
        return RETURN_ERROR;
    }

    fclose(file1);
    fclose(file2);
    return accuracy;
}

int main(int argc, char *argv[])
{
    printf("Welcome to HW project.\n");

    /* 输入dataset文件地址 */
    int opt;
    char *inputFile = NULL;
    char *outputFile = NULL;
    char *validateFile = NULL;
    int ret;

    /* 解析命令行参数 */
    while ((opt = getopt(argc, argv, "i:o:v:")) != -1) {
        switch (opt) {
            case 'i':
                inputFile = optarg;
                break;
            case 'o':
                outputFile = optarg;
                break;
            case 'v':
                validateFile = optarg;
                break;
            default:
                fprintf(stderr, "Usage: %s -i inputFile -v valFile -o outputFile. [example: ./main -i ./dataset/input_1.txt -o ./dataset/output_1.txt -v ./dataset/val_1.txt] \n", argv[0]);
                exit(EXIT_FAILURE);
        }
    }

    if (inputFile == NULL || validateFile == NULL) {
        if (inputFile == NULL) {
            printf("inputFile is NULL.\n");
        } else if (validateFile == NULL) {
            printf("validateFile is NULL.\n");
        }
        fprintf(stderr, "Usage example: ./main -i ./dataset/input_1.txt -o ./dataset/output_1.txt -v ./dataset/val_1.txt\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    printf("The input file path is: %s\n", inputFile);
    printf("The output file path is: %s\n", outputFile);
    printf("The validate file path is: %s\n", validateFile);

    IOVector *ioVector = (IOVector *)malloc(sizeof(IOVector));
    ret = ParseFile(inputFile, ioVector);
    if (ret < 0) {
        printf("[Error] Parse file failed\n");
        return RETURN_ERROR;
    }

    /* 记录开始时间 */
    struct timeval start, end;
    gettimeofday(&start, NULL);

    /* FTL算法执行 */
    AlgorithmRun(ioVector, outputFile);

    gettimeofday(&end, NULL);  // 记录结束时间
    long seconds, useconds;    // 秒数和微秒数
    seconds = end.tv_sec - start.tv_sec;
    useconds = end.tv_usec - start.tv_usec;

    /* 统计指标 */
    KeyMetrics metrics = {0};
    metrics.testIOCount = GetIOCount(validateFile, outputFile);
    if (metrics.testIOCount < 0) {
        metrics.testIOCount = 0;
        metrics.accuracy = 0;
    } else {
        metrics.accuracy = CompareFiles(validateFile, outputFile);
    }
    metrics.algorithmRunningDuration = ((seconds) * 1000000 + useconds) / 1000.0;
    metrics.memoryUse = 0;

    PrintMetrics(&metrics);
    /* 保存指标数据到文件 */
    SaveKeyMetricsToFile("./metrics.txt", &metrics);

    free(ioVector->ioArray);
    free(ioVector);
    return 0;
}
