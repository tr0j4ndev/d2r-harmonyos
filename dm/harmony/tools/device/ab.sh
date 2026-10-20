#!/bin/sh
# 同一会话内交替开关一个 files/ 标记，按 PERF 窗口字段分组平均。
# Alternate a files/ marker off/on inside the running session, then average PERF windows by its field.
# Usage: ab.sh <marker-file-name> <cycles> [seconds-per-phase] [perf-field]
# Env: HDC (default: hdc), BUNDLE (default: template bundle name)
H=${HDC:-hdc}
B=${BUNDLE:-com.xiaoboluo.harmony.bootstrap}
F=/data/app/el2/100/base/$B/haps/entry/files
M=$1; N=${2:-3}; T=${3:-14}
start=$($H shell "grep -c 'NVN PERF v=1' $F/game-graphics.txt" | tr -d '\r')
: > /tmp/exp_empty
i=0
while [ $i -lt $N ]; do
  $H shell "rm -f $F/$M"; sleep $T
  $H file send -b $B /tmp/exp_empty /data/storage/el2/base/haps/entry/files/$M >/dev/null; sleep $T
  i=$((i+1))
done
$H shell "rm -f $F/$M"
$H shell "grep 'NVN PERF v=1' $F/game-graphics.txt" | tail -n +$((start+1)) > /tmp/exp_ab_lines.txt
K=${4:-perfparts} python3 - <<'EOF'
import re,collections,os
K=os.environ['K']
d=collections.defaultdict(list)
for l in open('/tmp/exp_ab_lines.txt'):
    m=re.search(r'fps=([\d.]+) submit_cpu_ms=([\d.]+) present_cpu_ms=([\d.]+) gpu_ms=([\d.]+).*'+K+r'=(\d)',l)
    if m: d[m.group(5)].append(tuple(float(x) for x in m.groups()[:4]))
for k,v in sorted(d.items()):
    n=len(v); avg=[sum(x[i] for x in v)/n for i in range(4)]
    print(K+'=%s windows=%d fps=%.1f submit=%.2f present=%.2f gpu=%.2f'%(k,n,*avg))
    print('   fps:',' '.join('%.0f'%x[0] for x in v))
EOF
