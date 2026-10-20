# D2R HarmonyOS

**An open-source HarmonyOS (OpenHarmony) compatibility layer for Diablo II: Resurrected with the Reign of the Warlock expansion.** It runs the Nintendo Switch release you own — expansion included — natively on HarmonyOS devices. The original game code executes directly on the device with no emulated CPU; the platform services it expects are provided by a small host, and the graphics API is reimplemented on Vulkan.

This repository is the HarmonyOS port of [d2r-mobile](https://github.com/jhaizhou-ops/d2r-mobile) (iOS / macOS / Apple TV). The iOS and macOS sources remain in the tree for reference; everything HarmonyOS lives under [`dm/harmony/`](dm/harmony/).

中文: [README.zh-CN.md](README.zh-CN.md)

Platform: HarmonyOS (OpenHarmony, API 26) · Test device: Huawei Pura X Max (Maleoon 935 GPU, folded 1848×1264) · Game version: Switch release with Reign of the Warlock, 1.0.34.0 · License: MIT · **Bring your own game files**

| | |
|:---:|:---|
| ~60 fps | Character selection, steady (16 ms CPU / 16 ms GPU per frame) |
| 54–59 fps | In town on a cool device |
| ~30 fps | Thermal-throttling wall once the device heats up (both CPU and GPU inflate; see below) |

## What it is, and what it is not

**It is:**

- The game's own aarch64 program executing directly on the device. Only the handful of instructions that cannot run unchanged under the HarmonyOS kernel are rewritten; every other byte of the program is left exactly as shipped.
- The platform services the game expects — system calls, file system, graphics API, audio, input — provided by a small host written for HarmonyOS. The graphics API (NVN) is reimplemented on Vulkan, and the shaders are translated offline from the game's originals.
- A complete HarmonyOS application template (ArkTS UI + native bridge): touch controls, save sync, on-device performance tools.

**It is not:**

- An emulator, cloud gaming, or streaming.
- **A source of game content. It contains none and distributes none.** There is no game program, art, text, data table or shader in this repository; all of that is generated on your own machine from your own files (see [docs/GAME_FILES.md](docs/GAME_FILES.md)). The interface information recorded to make the port work is listed item by item in [docs/DERIVED.md](docs/DERIVED.md).
- Online. There is no multiplayer; the game runs fully offline.

## HarmonyOS-specific modifications

The port layer under `dm/harmony/`:

- **Runtime bridge** (`runtime/`): execution environment for the game binary with fast thread-local storage, signal handling and crash fixes; HarmonyOS HLE services (account, friends, file system, misc, svc, time) replacing the Apple implementations.
- **Graphics backend** (`graphics/`): NVN-to-Vulkan translation (`xbg_game`), command recording, descriptor management, render-pass handling, and an NVN trace tool (`xbg_nvn_trace.c`) for debugging.
- **Application template** (`app/`): ArkTS shell with a native bridge (`libxblhost.so`), save staging and cloud sync, game-data delivery through the HAP rawfile channel, tablet device support, PNG launcher icons, SDK 26.0.0 / hvigor 6.26.1 build configuration.
- **On-device diagnostics**: a built-in CPU sampler (triggered by dropping a file into the app's sandbox), a GPU fault dump, Float16 / 8-bit-storage feature detection, and world-readable game logs so `hdc` can pull them without root.
- **Foldable robustness**: swapchain retirement is drained through `drainRetiredSwaps`, which fixed a crash when folding/unfolding the device.

## Performance optimizations

All numbers measured on the Pura X Max in game. Each optimization carries a runtime marker file so it can be A/B-tested in the same session without reinstalling (see [`dm/harmony/tools/device/`](dm/harmony/tools/device/)).

### Submission queue thread (`xbl-queue`)

Queue submission and presentation moved off the game's graphics thread into a dedicated thread; submissions are asynchronous. Same-session A/B: **35 → 47 fps**. Revert marker: `gfx-sync-queue`.

### Own-region descriptor mode (mode 3)

The device exposes only 8 dynamic uniform buffers, fewer than the game binds. Shader groups that exceed the limit switch to an "own region" mode: per-frame-slot persistent descriptor arenas plus a cross-frame descriptor memo. Descriptor-set rebuilds dropped from **276 per frame to 0**, with **942/942 memo hits** and zero misses in a full character-select run.

### Texel homes (correctness + performance)

`UNIFORM_TEXEL_BUFFER` data used to be snapshotted per submission into the slot stream arena, and its `VkBufferView`s were destroyed at slot retirement — persistent memo entries then referenced stale memory, causing screen darkening and flicker. Texel data now lives in per-slot persistent guest-range copies ("texel homes") with long-lived views, refreshed once per recording via a serial check and invalidated on CPU writes. This fixed the visual corruption and a follow-up regression where every texel-bearing draw rebuilt its descriptor set (~370 sets per frame).

### Ranged flush/invalidate

`Buffer::flush`/`invalidate` operate on the allocation's byte range (rounded to `VkPhysicalDeviceLimits::nonCoherentAtomSize` alignment) instead of `VK_WHOLE_SIZE`.

### Cached readback memory

The readback arena moved from uncached to `HOST_CACHED` memory. Uncached reads were running at ~180 MB/s; the same 1.3 MB of per-frame writebacks went from **7.3 ms to 0.36 ms**, and total per-frame cleanup from **15 ms to ~1 ms**. A/B marker: `gfx-readback-uncached`.

### Counter-epoch garbage collection

The query-epoch map grew unboundedly during a session (observed 6,409 → 30,589 entries, with per-frame query cleanup inflating from 0.37 ms to 2.70 ms). Epochs are now reclaimed as soon as no pending report references them, keeping the map at 5–7 entries and query cleanup at ~0.03 ms — a same-session **31 → 39 fps** improvement on a hot device. A/B marker: `gfx-no-epoch-gc`.

### Smaller wins

Command-availability cache, pipeline-key copies changed to `memcpy`, texture-version checks for write tracking, render-pass merge and load elision, exact-match log filtering with an address cache, fast TLS, and assorted crash fixes.

### Thermal behavior

The ~30 fps town wall is thermal throttling, not a code limit: a cool restart holds 54–59 fps, then both CPU (~1.5×) and GPU times inflate as the device warms until it settles near 30 fps / 33 ms GPU. Future work targets dynamic 3D resolution under thermal load.

## Repository layout

```
dm/harmony/        HarmonyOS port: runtime, graphics, app template
  runtime/         execution bridge + HLE services
  graphics/        NVN-to-Vulkan backend
  app/             ArkTS application (libxblhost native bridge)
  tools/device/    device-side scripts: build/sign/install, fps capture,
                   same-session A/B toggles, core pinning, profiler aggregation
dm/ios, dm/tvos    upstream Apple-platform sources (reference)
docs/              game-file preparation and derived-interface documentation
```

## Building

The application builds with the OpenHarmony SDK 26.0.0 and hvigor 6.26.1 (`hvigorw assembleHap` inside `dm/harmony/app`); packaging and signing run through Docker via [`tools/device/rebuild_hap.sh`](dm/harmony/tools/device/rebuild_hap.sh), which also verifies that the `.so` files inside the signed HAP match the build intermediates. Game shaders and data are staged from your own files; see [docs/GAME_FILES.md](docs/GAME_FILES.md).

## Acknowledgements

The game program, translation infrastructure and the original iOS/macOS port are by the [d2r-mobile](https://github.com/jhaizhou-ops/d2r-mobile) authors. Diablo II: Resurrected is a trademark of Blizzard Entertainment. This project is not affiliated with or endorsed by Blizzard.
