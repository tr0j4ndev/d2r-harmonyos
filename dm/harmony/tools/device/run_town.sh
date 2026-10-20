#!/bin/sh
# 装包、启动、自动点进城镇，打印帧率行与每线程 CPU。 | Install, launch, tap into town, print fps lines and per-thread CPU.
# Usage: run_town.sh <hap>
# Env: HDC (default: hdc), BUNDLE (default: template bundle name)
H=${HDC:-hdc}
B=${BUNDLE:-com.xiaoboluo.harmony.bootstrap}
F=/data/app/el2/100/base/$B/haps/entry/files
$H install -r "$1" 2>&1 | grep -o "install bundle successfully" || exit 1
$H shell "hidumper -s ThermalService -a -t" | grep -A1 -E "Type: (Battery|shell_front|soc_thermal|cpu|gpu)" | grep -o "[0-9]\{5\}" | tr "\n" " "; echo "<- temps"
$H shell aa start -a EntryAbility -b $B >/dev/null
# 等角色选择界面出现（日志持续增长），再按 A | wait for character select, then press A
sleep 50
$H shell uinput -T -c 1725 758 >/dev/null
sleep 40
$H shell snapshot_display -f /data/local/tmp/town.jpeg >/dev/null
$H file recv /data/local/tmp/town.jpeg /tmp/town.jpeg >/dev/null
$H shell "grep 'NVN PERF' $F/game-graphics.txt | tail -60" > /tmp/perf_tail.txt
grep 'v=1 sync' /tmp/perf_tail.txt | tail -4 | grep -o 'fps=[0-9.]* submit_cpu_ms=[0-9.]* present_cpu_ms=[0-9.]* gpu_ms=[0-9.]*'
grep -E 'parts_ms|cleanup_ms|desc_ns' /tmp/perf_tail.txt | tail -3
$H shell 'P=$(pidof '"$B"'); for t in /proc/$P/task/*; do cat $t/stat; done > /data/local/tmp/a.txt 2>/dev/null; sleep 5; for t in /proc/$P/task/*; do cat $t/stat; done > /data/local/tmp/b.txt 2>/dev/null'
$H file recv /data/local/tmp/a.txt /tmp/exp_a.txt >/dev/null; $H file recv /data/local/tmp/b.txt /tmp/exp_b.txt >/dev/null
python3 "$(dirname "$0")/thr.py" /tmp/exp_a.txt /tmp/exp_b.txt 5.05 | head -6
$H shell "hidumper -s ThermalService -a -t" | grep -A1 -E "Type: (Battery|shell_front|soc_thermal|cpu|gpu)" | grep -o "[0-9]\{5\}" | tr "\n" " "; echo "<- temps"
