#!/bin/bash

# 生成时间戳 (格式: YYYYMMDD_HHMMSS)
TIMESTAMP=$(date "+%Y%m%d_%H%M%S")

# 配置路径 (请根据实际情况修改这里！)
EXE_PATH=$1  # 程序路径，接收命令行参数(eg. ./profile-benchmarks.sh ./benchmarks/project_hw_lowmem)
FLAMEGRAPH_DIR="./FlameGraph"

# ⚠️ 关键：请确认你的输入文件名和路径是否正确！

# PROG_ARGS="-i ./trace.txt -o ./output.txt -v ./read_result.txt"
PROG_ARGS="-i ./trace2.txt -o ./output2.txt -v ./read_result2.txt"
# PROG_ARGS="-i ./trace-a-lot-random.txt -o ./output-a-lot-random.txt -v ./read_result-a-lot-random.txt"

# --- 自动检查环节 ---

# 1. 检查可执行文件
if [ ! -f "$EXE_PATH" ]; then
    echo "❌ Error: Executable not found at $EXE_PATH"
    exit 1
fi

# 2. 检查 FlameGraph 工具
if [ ! -d "$FLAMEGRAPH_DIR" ]; then
    echo "❌ Error: FlameGraph dir not found at $FLAMEGRAPH_DIR"
    echo "👉 Please run: git clone https://github.com/brendangregg/FlameGraph.git"
    exit 1
fi

# --- 开始运行 ---

DATA_FILE="perf.data"
SVG_FILE="flamegraph_${TIMESTAMP}.svg"

# 清理旧数据
rm -f $DATA_FILE $SVG_FILE
rm -f map.ssd output.txt

echo "🔥 Starting perf record... (Running $EXE_PATH ${PROG_ARGS})"
# 运行 perf
# 注意：这里我们让 perf 运行在当前目录下，这样它能找到当前目录的输入文件
sudo perf record --sample-cpu -F 99 -g --call-graph dwarf -o ${DATA_FILE} \
    $EXE_PATH ${PROG_ARGS}

# 检查 perf 是否生成了数据
if [ ! -s "$DATA_FILE" ]; then
    echo "❌ Error: perf.data is empty. The program might have crashed or failed to start."
    exit 1
fi

echo "⚙️  Generating FlameGraph..."
#sudo perf script -i $DATA_FILE > out.perf
#$FLAMEGRAPH_DIR/stackcollapse-perf.pl out.perf > out.folded
#$FLAMEGRAPH_DIR/flamegraph.pl out.folded > $SVG_FILE

#rm out.perf out.folded

# 这里添加2> /dev/null把输出重定向了，因为我运行时有大串报错，但是图没啥问题
sudo perf script -i ${DATA_FILE} 2> /dev/null | ${FLAMEGRAPH_DIR}/stackcollapse-perf.pl | ${FLAMEGRAPH_DIR}/flamegraph.pl > ${SVG_FILE}

echo "⚙️  Generating Chrome Tracing..."


echo "✅ Done! Open $SVG_FILE in your browser (or VS Code)."