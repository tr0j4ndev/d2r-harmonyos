# D2R HarmonyOS

**开源的《暗黑破坏神 II：狱火重生》术士君临 鸿蒙（OpenHarmony）兼容层。** 把你自己购买的 Nintendo Switch 版游戏（含「术士君临」资料片）原生带到鸿蒙设备上：原版游戏代码直接在设备上执行，不模拟 CPU；游戏期望的平台服务由一个小型宿主提供，图形 API 在 Vulkan 上完整重实现。

本仓库是 [d2r-mobile](https://github.com/jhaizhou-ops/d2r-mobile)（iOS / macOS / Apple TV）的鸿蒙移植。iOS 与 macOS 源码保留在树中供参考，鸿蒙相关的全部内容在 [`dm/harmony/`](dm/harmony/)。

English: [README.en.md](README.en.md)

平台：鸿蒙（OpenHarmony，API 26）· 测试设备：华为 Pura X Max（Maleoon 935 GPU，折叠态 1848×1264）· 游戏版本：Switch 版「术士君临」1.0.34.0 · 许可证：MIT · **需要自备游戏文件**

| | |
|:---:|:---|
| ~60 帧 | 角色选择界面，稳定（每帧 CPU 16ms / GPU 16ms） |
| 54–59 帧 | 城镇内，设备凉爽时 |
| ~30 帧 | 设备发热后的热降频墙（CPU 与 GPU 耗时同步膨胀，详见下文） |

## 它是什么，不是什么

**它是：**

- 游戏自己的 aarch64 程序直接在设备上执行。只有极少数无法在鸿蒙内核下原样运行的指令被改写，程序的其余字节与出厂时完全一致。
- 游戏期望的平台服务——系统调用、文件系统、图形 API、音频、输入——由一个为鸿蒙编写的小型宿主提供。图形 API（NVN）在 Vulkan 上重实现，着色器由游戏原版离线翻译而来。
- 一套完整的鸿蒙应用模板（ArkTS 界面 + 原生桥）：触屏操控、存档同步、设备端性能工具。

**它不是：**

- 模拟器、云游戏或串流。
- **游戏内容的来源。本仓库不包含也不分发任何游戏内容**：没有游戏程序、美术、文本、数据表或着色器；这些都在你自己的机器上由你自己的文件生成（见 [docs/GAME_FILES.md](docs/GAME_FILES.md)）。仓库中仅包含让移植工作所需的接口信息，逐条列在 [docs/DERIVED.md](docs/DERIVED.md)。
- 联网游戏。没有多人模式，游戏完全离线运行，不需要任何在线账号。

## 针对鸿蒙的修改

移植层位于 `dm/harmony/`：

- **运行时桥**（`runtime/`）：游戏二进制的执行环境，含快速线程局部存储、信号处理与崩溃修复；以鸿蒙 HLE 服务（账号、好友、文件系统、杂项、svc、时间）替换 Apple 平台的实现。
- **图形后端**（`graphics/`）：NVN 到 Vulkan 的翻译（`xbg_game`）、命令录制、描述符管理、渲染通道处理，以及用于调试的 NVN 跟踪工具（`xbg_nvn_trace.c`）。
- **应用模板**（`app/`）：ArkTS 外壳与原生桥（`libxblhost.so`）、存档暂存与云同步、经 HAP rawfile 通道的游戏资料投递、平板设备支持、PNG 启动图标、SDK 26.0.0 / hvigor 6.26.1 构建配置。
- **设备端诊断**：内置 CPU 采样器（往应用沙箱丢一个文件即触发）、GPU 故障转储、Float16 / 8-bit 存储特性探测，以及全局可读的游戏日志（`hdc` 免 root 直接拉取）。
- **折叠屏健壮性**：交换链退役经 `drainRetiredSwaps` 排空，修复了折叠/展开设备时的闪退。

## 性能优化

以下数字全部在 Pura X Max 实机游戏内测得。每项优化都带运行时标记文件，可在同一会话内免重装做 A/B 对比（见 [`dm/harmony/tools/device/`](dm/harmony/tools/device/)）。

### 提交队列线程（`xbl-queue`）

队列提交与呈现从游戏图形线程移入专用线程，提交异步化。同会话 A/B：**35 → 47 帧**。回退标记：`gfx-sync-queue`。

### 自有区域描述符模式（模式 3）

设备只暴露 8 个动态 uniform buffer，少于游戏的绑定数量。超限的着色器组切换到「自有区域」模式：按帧槽位复用的持久描述符区域 + 跨帧描述符记忆。描述符集重建从**每帧 276 次降为 0**，一次完整的角色选择运行中记忆命中 **942/942**、零未命中。

### Texel 持久副本（正确性 + 性能）

`UNIFORM_TEXEL_BUFFER` 的数据过去随每次提交快照进槽位流区域，其 `VkBufferView` 在槽位退役时即销毁——持久记忆项因此引用到过期内存，造成画面变暗与闪烁。现在 texel 数据驻留在按槽位的持久客址副本（「texel homes」）中并持有长期 view，每次录制经序列号校验刷新一次、CPU 写入时失效。这既修复了画面错误，也修复了随后发现的回退：每个带 texel 的绘制都在重建描述符集（每帧约 370 套）。

### 按范围 flush/invalidate

`Buffer::flush`/`invalidate` 改为只操作分配的字节范围（按 `nonCoherentAtomSize` 对齐取整），不再用 `VK_WHOLE_SIZE`。

### 缓存回读内存

回读 arena 从未缓存改为 `HOST_CACHED` 内存。未缓存读的带宽只有约 180 MB/s；同样每帧 1.3 MB 的写回从 **7.3ms 降到 0.36ms**，每帧总清理时间从 **15ms 降到约 1ms**。A/B 标记：`gfx-readback-uncached`。

### 计数器 epoch 回收

查询 epoch 映射表随会话无限增长（实测 6,409 → 30,589 条，每帧查询清理从 0.37ms 膨胀到 2.70ms）。现在 epoch 在没有任何待读报告引用时即回收，表稳定保持 5–7 条、查询清理约 0.03ms——热机上同会话 **31 → 39 帧**。A/B 标记：`gfx-no-epoch-gc`。

### 小项优化

命令可用性缓存、管线键改用 `memcpy`、写跟踪的纹理版本校验、渲染通道合并与免加载、日志精确匹配的地址缓存、快速 TLS，以及若干崩溃修复。

### 热行为

城镇约 30 帧的天花板是热降频而非代码限制：冷机重启能保持 54–59 帧，随后随设备升温，CPU（约 1.5 倍）与 GPU 耗时同步膨胀，直到稳定在 30 帧 / GPU 33ms 附近。后续计划：热负载下的动态 3D 分辨率。

## 仓库结构

```
dm/harmony/        鸿蒙移植：运行时、图形、应用模板
  runtime/         执行桥 + HLE 服务
  graphics/        NVN 到 Vulkan 后端
  app/             ArkTS 应用（libxblhost 原生桥）
  tools/device/    设备端脚本：构建/签名/安装、帧率采集、
                   同会话 A/B 开关、绑核、采样聚合
dm/ios, dm/tvos    上游 Apple 平台源码（参考）
docs/              游戏文件准备与派生接口文档
```

## 构建

应用使用 OpenHarmony SDK 26.0.0 与 hvigor 6.26.1 构建（在 `dm/harmony/app` 内执行 `hvigorw assembleHap`）；打包与签名经 Docker 完成，见 [`tools/device/rebuild_hap.sh`](dm/harmony/tools/device/rebuild_hap.sh)，脚本还会校验签名 HAP 内的 `.so` 与构建中间产物一致。游戏着色器与资料由你自己的文件生成，见 [docs/GAME_FILES.md](docs/GAME_FILES.md)。

## 致谢

游戏程序、翻译基础设施与最初的 iOS/macOS 移植来自 [d2r-mobile](https://github.com/jhaizhou-ops/d2r-mobile) 的作者。《暗黑破坏神 II：狱火重生》是暴雪娱乐的商标。本项目与暴雪无任何关联，亦未获其背书。
