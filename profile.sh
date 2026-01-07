#!/bin/bash

# 配置路径 (请根据实际情况修改这里！)
BUILD_DIR="." # 这里假设当前目录是 build 目录
EXE_NAME="project_hw"
FLAMEGRAPH_DIR="../FlameGraph"

# ⚠️ 关键：请确认你的输入文件名和路径是否正确！
# 如果你的文件在 dataset 目录下，请改为 ../dataset/input_random.txt
INPUT_FILE="../trace2.txt"      
OUTPUT_FILE="../output2.txt"
VAL_FILE="../read_result2.txt"

# --- 自动检查环节 ---

# 1. 检查可执行文件
if [ ! -f "$BUILD_DIR/$EXE_NAME" ]; then
    echo "❌ Error: Executable not found at $BUILD_DIR/$EXE_NAME"
    echo "👉 Please run: cd build && cmake .. && make"
    exit 1
fi

# 2. 检查 FlameGraph 工具
if [ ! -d "$FLAMEGRAPH_DIR" ]; then
    echo "❌ Error: FlameGraph dir not found at $FLAMEGRAPH_DIR"
    echo "👉 Please run: git clone https://github.com/brendangregg/FlameGraph.git"
    exit 1
fi

# 3. 检查输入数据文件
if [ ! -f "$INPUT_FILE" ]; then
    echo "❌ Error: Input file not found: $INPUT_FILE"
    echo "👉 Please edit profile.sh and change INPUT_FILE to your actual file path."
    exit 1
fi

# --- 开始运行 ---

DATA_FILE="perf.data"
SVG_FILE="flamegraph.svg"

# 清理旧数据
rm -f $DATA_FILE $SVG_FILE
rm -f map.ssd output.txt

echo "🔥 Starting perf record... (Running $EXE_NAME)"

# 运行 perf
# 注意：这里我们让 perf 运行在当前目录下，这样它能找到当前目录的输入文件
sudo perf record --sample-cpu -F 99 -g --call-graph dwarf \
    $BUILD_DIR/$EXE_NAME -i $INPUT_FILE -o $OUTPUT_FILE -v $VAL_FILE

# 检查 perf 是否生成了数据
if [ ! -s "$DATA_FILE" ]; then
    echo "❌ Error: perf.data is empty. The program might have crashed or failed to start."
    exit 1
fi

echo "⚙️  Generating FlameGraph..."
sudo perf script -i $DATA_FILE > out.perf
$FLAMEGRAPH_DIR/stackcollapse-perf.pl out.perf > out.folded
$FLAMEGRAPH_DIR/flamegraph.pl out.folded > $SVG_FILE

rm out.perf out.folded

echo "⚙️  Generating Chrome Tracing..."


echo "✅ Done! Open $SVG_FILE in your browser (or VS Code)."