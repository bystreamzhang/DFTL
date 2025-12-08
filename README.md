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
