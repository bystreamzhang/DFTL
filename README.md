# 项目背景

[“Massive Storage”第三届大学生信息存储技术竞赛· 挑战赛](https://developer.huaweicloud.com/competition/information/1300000150) SSD赛道

赛题概括：如何在内存容量受限情况下，仍然提供小粒度的FTL，从而提升大容量盘在系统侧的易用性。

详细介绍：[赛题二-大容量SSD数据管理优化 - 复赛](https://github.com/bystreamzhang/DFTL/blob/master/docs/%E7%AB%9E%E8%B5%9B%E8%B5%9B%E9%A2%98%E4%BA%8C-%E5%A4%A7%E5%AE%B9%E9%87%8FSSD%E6%95%B0%E6%8D%AE%E7%AE%A1%E7%90%86%E4%BC%98%E5%8C%96%20-%20%E5%A4%8D%E8%B5%9B.pdf)

# 核心挑战

考虑使用DFTL架构，存在以下挑战：

1. 访问时延瓶颈：CMT对数据局部性支持不足，面对大规模顺序读写场景性能不佳
2. 内存占用瓶颈：标准 DFTL 的 GTD 需要存储完整的物理页号，内存占用大
3. 延迟抖动与阻塞瓶颈：DFTL使用传统同步架构，高并发场景下QoS无法保障，Miss时的阻塞会导致吞吐率剧烈波动（赛题中，sscanf的耗时过大导致处理请求的工作线程阻塞）

详情：[AIMS小分队-答辩PPT](https://github.com/bystreamzhang/DFTL/blob/master/docs/AIMS%E5%B0%8F%E5%88%86%E9%98%9F-%E7%AD%94%E8%BE%A9PPT.pdf)

# 解决方案

本项目提出的架构为AIMS-FTL，对大量顺序写场景进行特殊优化。

提出解决方案：

1. 映射页缓存（TPC）
2. 位图压缩GTD
3. SQ队列和提交批处理

详情：[AIMS小分队-答辩PPT](https://github.com/bystreamzhang/DFTL/blob/master/docs/AIMS%E5%B0%8F%E5%88%86%E9%98%9F-%E7%AD%94%E8%BE%A9PPT.pdf)

# 关键成果

全国决赛三等奖：[赛题二决赛排名](https://github.com/bystreamzhang/DFTL/blob/master/docs/%E8%B5%9B%E9%A2%98%E4%BA%8C%E5%86%B3%E8%B5%9B%E6%8E%92%E5%90%8D.xlsx)

时延约Rank3，内存占用Rank7 (4MB以下)。测试阶段Rank5。

综合答辩阶段成绩，最终排名：Rank4。

# 如何运行


## Requirement：
Ubuntu  ≥ 	18.04
cmake	≥  	3.27.5
make	≥ 	4.1
GCC		≥ 	7.5

## Build & Run
```shell
cd project
mkdir build
cd build
cmake ..
make
./project_hw -i ../dataset/input_seq.txt -o ../dataset/output_seq.txt -v ../dataset/val_seq.txt
# 或者如下，取决于文件路径
./project_hw -i ../trace.txt -o ../output.txt -v ../read_result.txt
# 也可以使用.vscode文件夹的tasks.json和launch.json
```

```shell
# example
[root@localhost project]# mkdir build
[root@localhost project]# cd build/
[root@localhost build]# cmake ..
[root@localhost build]# make
[ 33%] Building C object CMakeFiles/project_hw.dir/main.c.o
[ 66%] Building C object CMakeFiles/project_hw.dir/ftl/ftl.c.o
[100%] Linking C executable project_hw
[100%] Built target project_hw
[root@localhost build]# ll
total 60
-rw-r--r--. 1 root root 14203 Sep 19 17:01 CMakeCache.txt
drwxr-xr-x. 6 root root  4096 Sep 19 17:02 CMakeFiles
-rw-r--r--. 1 root root  1634 Sep 19 17:01 cmake_install.cmake
-rw-r--r--. 1 root root  5868 Sep 19 17:01 Makefile
-rwxr-xr-x. 1 root root 26416 Sep 19 17:02 project_hw
[root@localhost build]# ./project_hw -i ../dataset/input_seq.txt -o ../dataset/output_seq.txt -v ../dataset/val_seq.txt 
Welcome to HW project.
The input file path is: ../dataset/input_seq.txt
The output file path is: ../dataset/output_seq.txt
The validate file path is: ../dataset/val_seq.txt
input io count = 26048576

Key Metrics:
	testIOCount:			 524169
	algorithmRunningDuration:	 448.909 (ms)
	accuracy:			 100.00
	memoryUse:			 0 (KB)

指标写入文件 ./metrics.txt
```

# 如何测试

## 测试数据生成

使用`build_dataset2.c`等。可自行调整参数。

会生成trace.txt和read_result.txt文件。

## 性能测试

### 延时

代码内有运行时间统计（包含了输入时间）。

也可使用火焰图，见 profile.sh。

### 内存

分析Peak Memory(应用层视角)使用valgrind。
```
# --tool=massif: 指定使用堆分析工具
# --time-unit=ms: 时间轴单位用毫秒（默认是指令数，毫秒更直观）
# --detailed-freq=1: 尽可能详细地记录快照
valgrind --tool=massif --time-unit=ms --detailed-freq=1 \
    ./build/project_hw -i ./trace.txt -o ./output.txt -v ./read_result.txt
```
分析Max RSS使用 /usr/bin/time -v ./build/project_hw ...

更具体的可以用**pmap（推荐，测试结果和赛事方评测结果接近）**：

在运行程序时，运行：`pmap -x <pid> | sort -k 3 -n -r`

运行的不同时间，内存占用可能不同，可以等到运行到中后期再用pmap测试。

## 系统调用分析

`strace -c ./project_hw_final -i ../trace.txt -o ../output.txt -v ../read_result.txt`
`strace -c ./project_hw_nosetvbuf -i ../trace.txt -o ../output.txt -v ../read_result.txt` 可分析系统调用次数。与本项目关系不大。
