#!/bin/sh
# 部署本地生成的着色器库到设备并启动，然后检查日志中的失败行。
# Deploy a locally built shader library to the device, launch, and check the log for failures.
# Usage: deploy_shaders.sh <shader-file>...   (manifest.json 会一并推送 | manifest.json is pushed too)
# Env: HDC (default: hdc), BUNDLE, WAIT (default 100)
H=${HDC:-hdc}
B=${BUNDLE:-com.xiaoboluo.harmony.bootstrap}
D=/data/storage/el2/base/haps/entry/files/game-shaders
$H shell aa force-stop $B >/dev/null
for f in "$@" manifest.json; do $H file send -b $B "$f" $D/$(basename "$f") | tail -1; done
$H shell "power-shell wakeup; power-shell setmode 602" >/dev/null; $H shell aa start -a EntryAbility -b $B; sleep ${WAIT:-100}
rm -f /tmp/exp_gg.txt; $H file recv -b $B /data/storage/el2/base/haps/entry/files/game-graphics.txt /tmp/exp_gg.txt >/dev/null 2>&1
L=$(grep -n 'enabledRobustness2NullDescriptor' /tmp/exp_gg.txt | tail -1 | cut -d: -f1); tail -n +$L /tmp/exp_gg.txt > /tmp/exp_run.txt
N=$(grep -n 'FENCE_FAIL\|FATAL' /tmp/exp_run.txt | head -1 | cut -d: -f1); echo "fail line $N of $(wc -l </tmp/exp_run.txt)"
grep 'opsync n=' /tmp/exp_run.txt | tail -1; grep -m3 'FATAL\|FENCE_FAIL' /tmp/exp_run.txt | cut -c1-300
