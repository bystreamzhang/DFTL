import sys
import json
import re

# 在perf.data所在文件夹用 perf script -F comm,pid,tid,cpu,time > out-perf-script.txt 生成 out-perf-script.txt
# 生成json： python3 perf2json.py out-perf-script.txt > trace.json

def parse_perf_script(filename):
    trace_events = []
    
    # 修正后的正则表达式：兼容 PID 和 PID/TID 两种格式
    # 格式解释:
    #   ^\s* -> 行首允许有空格
    #   (.+?)      -> 进程名 (Group 1)
    #   \s+        -> 空格分隔
    #   (\d+(?:/\d+)?) -> PID/TID 部分，匹配 "123" 或 "123/456" (Group 2)
    #   \s+        -> 空格分隔
    #   \[(\d+)\]  -> [CPU号] (Group 3)
    #   \s+        -> 空格分隔
    #   (\d+\.\d+) -> 时间戳 (Group 4)
    #   :          -> 结尾冒号
    pattern = re.compile(r"^\s*(.+?)\s+(\d+(?:/\d+)?)\s+\[(\d+)\]\s+(\d+\.\d+):")

    print(f"Processing {filename}...", file=sys.stderr)
    
    line_count = 0
    match_count = 0
    
    with open(filename, 'r') as f:
        for line in f:
            line_count += 1
            # 跳过注释或空行
            if line.strip().startswith('#') or not line.strip():
                continue
                
            match = pattern.match(line)
            if match:
                match_count += 1
                comm = match.group(1).strip()
                pid_tid_str = match.group(2)
                cpu = int(match.group(3))
                time_str = match.group(4)
                
                # 解析 PID 和 TID
                # 如果是 "1991576/1991578" 格式，取后面那个作为 TID (线程ID)
                if '/' in pid_tid_str:
                    parts = pid_tid_str.split('/')
                    pid = int(parts[0])
                    tid = int(parts[1])
                else:
                    pid = int(pid_tid_str)
                    tid = pid # 单线程程序 PID=TID

                # Chrome Tracing 时间单位是微秒 (us)
                ts = float(time_str) * 1000000
                
                event = {
                    "name": comm,        # 显示进程/线程名
                    "cat": "PERF",
                    "ph": "X",           # Complete Event
                    "ts": ts,
                    "dur": 10,           # 采样点持续时间设短一点 (10us)，以免重叠太严重
                    "pid": pid,          # 进程ID归类
                    "tid": tid,          # 线程ID区分轨道
                    "args": {
                        "cpu": cpu
                    }
                }
                trace_events.append(event)
            else:
                # 调试用：如果前几行匹配失败，打印出来看看
                if line_count < 5:
                    print(f"Failed to match line: {line.strip()}", file=sys.stderr)

    print(f"Processed {line_count} lines, matched {match_count} events.", file=sys.stderr)
    return {"traceEvents": trace_events}

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: python3 perf2json.py <input_file>", file=sys.stderr)
        sys.exit(1)
        
    data = parse_perf_script(sys.argv[1])
    print(json.dumps(data))