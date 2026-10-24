<!--
SPDX-FileCopyrightText: 2026 shadPS4 Emulator Project
SPDX-License-Identifier: GPL-2.0-or-later
-->

<div align="center">

# infamous-pc

### inFAMOUS Second Son on PC — an emulator build tuned for one game

**~1.8× the frame rate and much steadier frame pacing than the native PC port we measured, on the same machine and the same title-screen scene.**

[**⬇ Download (Windows x64)**](https://github.com/MeWeGo/infamous-pc/releases/latest) ·
[Quick start](#quick-start) ·
[Benchmarks](#benchmarks) ·
[Tuning](#tuning) ·
[Build](#build-from-source) ·
[Credits](#credits)

![Platform](https://img.shields.io/badge/platform-Windows%20x64-blue)
![Graphics](https://img.shields.io/badge/graphics-Vulkan%201.3-red)
![License](https://img.shields.io/badge/license-GPL--2.0--or--later-green)
[![Downloads](https://img.shields.io/github/downloads/MeWeGo/infamous-pc/total.svg)](https://github.com/MeWeGo/infamous-pc/releases)
[![Stars](https://img.shields.io/github/stars/MeWeGo/infamous-pc?style=flat)](https://github.com/MeWeGo/infamous-pc/stargazers)

</div>

---

## What is this?

`infamous-pc` runs **inFAMOUS Second Son** (CUSA00223, update 1.07) and **inFAMOUS First Light** on Windows through a [shadPS4](https://github.com/shadps4-emu/shadPS4) fork whose only goal is **speed and stability on these games**. Everything in the render path — buffer cache, readbacks, upload threads, pipeline compilation — is profiled against inFAMOUS and tuned for it, not for the average PS4 title.

> [!NOTE]
> No game files are included. You need your own dump of the game (base + update 1.07 merged in one folder) and system modules (`sys_modules`) from your own PS4. Bugs and glitches remain; this is a performance-first build.

## Quick start

1. Download `infamous-pc-<version>-windows-x86_64.zip` from the [latest release](https://github.com/MeWeGo/infamous-pc/releases/latest) and extract it.
2. Install the [Visual C++ Redistributable 2015–2022 (x64)](https://aka.ms/vs/17/release/vc_redist.x64.exe) if you have not already.
3. Run **`Play inFAMOUS.bat`** and paste the path of the folder that contains `eboot.bin` (or drag the folder onto the `.bat`).
4. **First launch compiles shaders, so expect stutter.** Keep the shader cache (`%APPDATA%\shadPS4\cache`) — it is what makes every later launch smooth.

Equivalent manual command:

```sh
shadps4.exe --show-fps -g "<path-to>\CUSA00223-merged\eboot.bin"
```

**In-game keys:** `F10` FPS counter · `F11` fullscreen · `F12` screenshot / RenderDoc capture. Xbox, DualShock and DualSense controllers work out of the box; touchpad swipes are mapped to the pad (see [Controls](#controls)).

## Benchmarks

Same PC, same game files, same live-3D title scene, no input, warm shader caches, fresh process per run.

**Test system:** AMD Ryzen 7 5800H · NVIDIA GeForce RTX 3070 Laptop (8 GB) · 32 GB RAM · Windows 11

| Build | Median FPS | Worst frame | Draws / frame |
| :--- | :---: | :---: | :---: |
| **infamous-pc** (`ecafe1e3`, this repo) | **30.6** | **40 ms** | ~7,950 |
| [iss_pc](https://github.com/HayatoG/iss_pc) beta `88e912f`, renderer v1 | 16.6 | ~150–170 ms | ~7,575 |
| [iss_pc](https://github.com/HayatoG/iss_pc) `9c24de1` (Oct 16 release), renderer v1 | 14.9 | ~170–180 ms | ~7,550 |

What this does **and does not** show:

- It is a **title-scene** measurement. Gameplay (open-world streaming, powers, cutscenes) has **not** been benchmarked yet; numbers there will differ. Community results on other hardware are very welcome — please open an issue with your GPU, CPU and FPS-counter screenshot.
- Both builds translate roughly the same draw stream on the CPU side. The host GPU is far from saturated in both cases; the difference is how efficiently the command processor records and submits work.
- `iss_pc` renderer v2 (native passes) was **not** compared fairly: our run overlapped its first-launch compile of ~8,000 compute shaders, so we make no claim about it.
- Method: launch with `--show-fps`, discard shader-compile warm-up, take the median of ≥20 stats windows (10 s each for `infamous-pc`, 5 s each for `iss_pc`).

## What makes it fast

Work done on top of upstream shadPS4, grouped by theme:

**Frame pacing & CPU cost**
- Dedicated **upload worker** moves upload copies and page protection off the GPU thread; large uploads are split into parts.
- **Deferred readbacks:** only the thread that needs the data waits, and only for the submission that wrote it.
- Buffer-cache **readback window tuned to 128 KB** and prefetch backoff to **1** for inFAMOUS (built-in defaults).
- **Static red-zone / SSE4a patching** enabled by default; duplicate vertex/index buffer binds removed.
- Recorder-thread descriptor sets, texture/binding fast paths, narrower draw barriers.
- Optional **second compute queue** (async compute) — off by default, `SHADPS4_ASYNC_COMPUTE=1` to try it.

**Stability & VRAM**
- Residency allocation **retries on out-of-memory**, allocates past the budget instead of failing, and logs memory use.
- Pageable device-local memory; pipeline creation retried if the driver fails.
- Persistent driver `VkPipelineCache` across launches.
- Single-queue readback, wave64 atomics, SRT-walker overrun and window-visibility fixes.

**Shaders**
- BP01–BP12 backports, wave64 ballot lowering with an exhaustive order-independent uniformity check (`ReachesLaneVaryingSource`), shared-memory (LDS) barriers, SDWA-compare and unnormalized-sampler fixes, shader patches applied to cached shaders, async compute-pipeline builds.

**Input**
- Gamepad touchpad swipes, two-finger swipes, stick-driven touchpad fingers (L3/R3).

**Diagnostics**
- GPU draw timing, per-frame perf lines (`OnFlip: Perf:`), sampling profiler, and lock-free read-fault guest-RIP aggregation to find which game code stalls on page faults.

## Recommended settings

| Setting | Value |
| :--- | :--- |
| FSR | **Off** — causes corruption in inFAMOUS |
| Debug → Readbacks | **Precise** |
| Debug → Direct Memory Access | **On** |
| Debug → Shader Cache | **On** |
| Vblank frequency | your target frame rate |

The launcher installs a per-game config (`CUSA00223.json`: +512 MB direct memory, precise readbacks, pipeline cache) only if you do not already have one.

**4 GB VRAM cards:** supported by the OOM-retry and part-wise upload work, but keep the shader cache, close other GPU apps, and expect a one-time stutter the first time a new effect (e.g. a new power) appears.

## Tuning

No rebuild needed — set environment variables before launching (e.g. in `Play inFAMOUS.bat`):

| Variable | Default | Effect |
| :--- | :---: | :--- |
| `SHAD_READBACK_WINDOW_KB` | `128` | GPU readback window size |
| `SHAD_PREFETCH_BACKOFF_MAX` | `1` | Stale-window backoff |
| `SHAD_HOT_WINDOW_SECS` | `5` | How long read-back memory is copied ahead |
| `SHAD_PREFETCH_OFF` | `0` | `1` disables readback prefetch |
| `SHAD_AHEAD_MINRUN` / `SHAD_AHEAD_MAX` / `SHAD_AHEAD_BASE` | `2` / `16` / `4` | Page-invalidation lookahead (`MAX=64` restores a wide scan) |
| `SHADPS4_ASYNC_COMPUTE` | off | `1` routes eligible compute to a second queue, `0` forces it off |

Older builds used a 512–1024 KB window; if you see regressions on a very different GPU, try `SHAD_READBACK_WINDOW_KB=512` and tell us.

## Controls

| Input | Function |
| :--- | :--- |
| Xbox / DualShock / DualSense | Works out of the box |
| Gamepad swipe, L3 / R3 | Touchpad swipes and stick-driven touchpad fingers; two-finger swipes supported |
| Keyboard | Per-game bindings in the in-app Controller settings (up to 3 keys per action) |
| `F10` / `F11` / `F12` | FPS counter / fullscreen / screenshot or RenderDoc capture |

## Build from source

```sh
git clone --recursive https://github.com/MeWeGo/infamous-pc.git
cd infamous-pc
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Detailed guides: [Windows](./documents/building-windows.md) · [Linux](./documents/building-linux.md) · [macOS](./documents/building-macos.md) · [Docker](./documents/building-docker.md) · [Debugging](./documents/Debugging/Debugging.md)

Run the tests:

```sh
cmake -S . -B build -DENABLE_TESTS=ON
cmake --build build --target shadps4_gcn_test shadps4_settings_test
./build/tests/shadps4_gcn_test --gtest_filter='Wave64*'
```

## Reporting problems

Open an [issue](https://github.com/MeWeGo/infamous-pc/issues) with: GPU + driver version, CPU, VRAM, the `shadPS4` log from `%APPDATA%\shadPS4\log`, and an FPS-counter screenshot. Performance reports (even "it runs at X FPS on my GPU") are the most useful thing you can send.

## Credits

`infamous-pc` stands on other people's work, and we are grateful for it:

- **infamous-pc custom build:** Eye Of The Ruins, 0xDeftones
- **Parent build:** [rayhlancaner-png/shadPS4-Infamous-FixBuild](https://github.com/rayhlancaner-png/shadPS4-Infamous-FixBuild) — the 1.0.10–1.0.16 performance/VRAM/touchpad line this fork merges; 4 GB OOM fix by TOXIC-KID (#6)
- **Shader fixes:** the BP01–BP12 backports were ported from [HayatoG/iss_pc](https://github.com/HayatoG/iss_pc), a native PC port of the same game
- **Emulator:** [shadPS4](https://github.com/shadps4-emu/shadPS4) — [georgemoralis](https://github.com/georgemoralis), [psucien](https://github.com/psucien), [viniciuslrangel](https://github.com/viniciuslrangel), [roamic](https://github.com/roamic), [squidbus](https://github.com/squidbus), [frodo](https://github.com/baggins183), [Stephen Miller](https://github.com/StevenMiller123), [kalaposfos13](https://github.com/kalaposfos13) and all upstream contributors

<a href="https://github.com/MeWeGo/infamous-pc/graphs/contributors">
  <img src="https://contrib.rocks/image?repo=MeWeGo/infamous-pc&max=24">
</a>

## License

[GPL-2.0-or-later](./LICENSE). Upstream: [shadps4-emu/shadPS4](https://github.com/shadps4-emu/shadPS4). Not affiliated with Sony Interactive Entertainment or Sucker Punch Productions; *inFAMOUS* is their trademark.
