# 设备侧构建与性能工具 | Device build & profiling tools

中文 | English 简述附后。

这些脚本服务于真机迭代回路：改代码 → 出包 → 装机 → 量帧率 → 采样分析。它们操作的是 `prepare_app.py` 组装出的**仓外** App 工程与已签名设备，不碰源码树。

## 脚本

- `rebuild_hap.sh <out.hap>` — hvigor 重编原生库，Docker 内打包+签名，校验包内运行库 SHA 与构建产物一致。签名口令只走环境变量（`HAP_KEY_PWD` 等），不进命令行历史以外的任何文件。
- `run_town.sh <hap>` — 装机、启动、模拟按键进城镇，打印 PERF 帧率行、每线程 CPU 与机身温度。
- `ab.sh <marker> <cycles> [sec] [field]` — 同一会话内交替创建/删除某个 `files/` 标记，按 PERF 行里的对应字段分组对比帧率。发热会污染跨会话对比，同会话交替是本仓性能结论的默认证据形式。
- `pin.sh "<prefix> <mask>..."` — 线程绑核标记，随后打印绑核结果与帧率。
- `deploy_shaders.sh <files...>` — 推送本地生成的着色器库与 manifest 并启动，检查日志里的 FATAL/FENCE_FAIL。
- `thr.py a.txt b.txt [sec]` — 两次 `/proc/<pid>/task` 快照的每线程 CPU 差分（`run_town.sh` 内部使用）。
- `prof_agg.py <samples.txt> <summary|attr|chain|proj> [thread] [arg]` — 应用内 CPU 采样器（`files/cpu-sample` 触发，输出 `cpu-samples.txt`）的聚合。需要 `XBL_PROF_DIR` 指向去符号的 `libxblhost.so`/`libxblr_execution.so`（以及可选 `musl.syms`）。

## 环境变量

`HDC`（缺省 `hdc`）、`BUNDLE`（缺省模板包名 `com.xiaoboluo.harmony.bootstrap`）、`OHOS_SDK`、`DM_WORK`。签名相关见 `rebuild_hap.sh` 头部注释。

## 运行时标记（files/ 目录，多数按 PERF 窗口重读，可同会话 A/B）

| 标记 | 作用 |
|---|---|
| `gfx-no-own` / `-nopersist` / `-nomemo` | 超限 UBO“自有区域”模式的开关与二分 |
| `gfx-no-xmemo` / `gfx-no-descmemo` / `gfx-no-dynpartial` | 跨帧/按提交描述符记忆、部分动态偏移 |
| `gfx-readback-uncached` | 回读内存改用非缓存（A/B 用，默认缓存） |
| `gfx-no-epoch-gc` | 计数器 epoch 不回收（A/B 用，默认回收） |
| `gfx-no-wtrack` / `gfx-no-imgcache` / `gfx-no-bindcache` / `gfx-no-noload` / `gfx-no-warm` | 写跟踪快照、图像缓存、绑定缓存、pass 合并、预热 |
| `gfx-no-perf-parts` | 关闭每阶段 CPU 计时（省约 1 ms/帧） |
| `gfx-sync-queue` / `gfx-sync-submit` / `gfx-sync-mode` / `gfx-op-sync` | 提交/呈现同步化与逐操作等待（诊断用） |
| `cpu-sample`（内容 `[秒] [g]`）、`cpu-pin`、`cpu-bench` | 采样、绑核、微基准 |
| `fps-30` | 30 Hz 呈现 |

---

These scripts serve the on-device iteration loop: edit → package → install → measure → profile. They operate on the out-of-tree App project assembled by `prepare_app.py` and a signed device; they never touch the source tree. Signing secrets pass through environment variables only. `ab.sh` alternates a runtime marker inside one session, which is the default form of evidence for performance claims here because heat pollutes cross-session comparisons. See the table above for the runtime markers the graphics/runtime layers honor.
