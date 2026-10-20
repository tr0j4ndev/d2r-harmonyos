#!/bin/sh
# 设置线程绑核标记，等待后打印绑核结果与帧率行。
# Apply a cpu-pin marker, wait, print pin results and fps lines.
# Usage: pin.sh "<prefix> <mask>[\n...]" [wait-seconds]
# Env: HDC (default: hdc), BUNDLE (default: template bundle name)
H=${HDC:-hdc}
B=${BUNDLE:-com.xiaoboluo.harmony.bootstrap}
F=/data/app/el2/100/base/$B/haps/entry/files
printf "$1\n" > /tmp/exp_pinfile
$H file send -b $B /tmp/exp_pinfile /data/storage/el2/base/haps/entry/files/cpu-pin >/dev/null
sleep ${2:-12}
$H shell "cat $F/cpu-pin.txt | head -3; grep 'NVN PERF v=1' $F/game-graphics.txt | tail -4" | grep -o 'tid=[0-9]* mask=[0-9a-f]* rc=-\?[0-9]*\|fps=[0-9.]* submit_cpu_ms=[0-9.]* present_cpu_ms=[0-9.]* gpu_ms=[0-9.]*'
