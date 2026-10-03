# LinuxBench

[![Version](https://img.shields.io/badge/Version-0.3.0-brightgreen?style=for-the-badge)](CMakeLists.txt)
[![C++20](https://img.shields.io/badge/C%2B%2B-20-blue?style=for-the-badge)](https://isocpp.org)
[![Platform](https://img.shields.io/badge/Platform-Linux%20x86_64-green?style=for-the-badge)](https://www.linux.org)
[![Embree](https://img.shields.io/badge/Embree-4.4.1-6ea1c3?style=for-the-badge)](https://embree.github.io)
[![Dear ImGui](https://img.shields.io/badge/Dear%20ImGui-1.90.4-7c4dff?style=for-the-badge)](https://github.com/ocornut/imgui)
[![GLFW](https://img.shields.io/badge/GLFW-3.4-5c9e65?style=for-the-badge)](https://www.glfw.org)
[![License](https://img.shields.io/badge/License-MIT-yellow?style=for-the-badge)](LICENSE)

> **A native, Cinebench-style CPU stress-test and benchmark for Linux.**
> True path tracing. Real hardware. One clean score.

## Overview

LinuxBench is a single-purpose CPU benchmark that turns your machine into a
**true path tracer** and measures how fast it is. It renders a **single 4K
frame (3840×2160) at 1024 samples per pixel** from a procedural scene of
roughly **10.5 million triangles** — built on
[Intel Embree 4](https://embree.github.io) for production-grade SIMD BVH
construction and ray intersection — and scores the run by its exact
wall-clock time.

v0.2.0 is a fundamental architectural shift: where v0.1.x ran a 1080p
real-time frame-rate loop, LinuxBench now performs an offline-style render —
one massive, physically-based path-traced 4K frame — a workload heavy enough
to pin a Ryzen 9 9950X3D for a full minute.

v0.3.0 refines the run loop: the scored **benchmark** now renders **three
passes** back-to-back (each pass re-seeded, so none is a warm-cache repeat of
the last) and reports one combined score, while the new **stress test** mode
runs passes endlessly with a live last-pass score until you hit **Stop**.

The whole application is a single native binary: GLFW provides the window
and OpenGL context, Dear ImGui draws a full-window live UI (progress, score,
leaderboard), and a persistent worker pool path-traces tiles of the frame
while you watch the image assemble in real time. No Python wrappers, no
Electron, no telemetry — just C++20 and your silicon.

## Features

- **True path tracing** — every pixel is integrated with **1024 jittered
  samples per pixel**, each one a full light path through the scene with
  **global illumination up to 4 bounces** deep, lit only by the analytic
  dusk sky (gradient + sun).
- **PBR materials** — the 21,952-sphere field is a deliberate mix:
  **10% glass (IOR 1.5)**, **40% rough metal**, and **50% matte**, decided
  per sphere by an id hash, so every material branch is exercised.
- **Cosine-weighted hemisphere sampling** — the matte (Lambert) BRDF is
  sampled directly from its cosine-weighted distribution: unbiased diffuse
  integration with no importance-sampling waste.
- **Schlick's Fresnel approximation** — glass and metal use Schlick's F(θ)
  to split energy between reflection and refraction, with total internal
  reflection when the refraction angle overflows.
- **3-pass benchmark + stress test** — the scored run renders three full
  passes back-to-back (per-pass RNG re-seed: `worker_id + 1 + pass × 100`)
  and reports `score = (1,600,000 × 3) / total time`; the stress mode loops
  endlessly with a live last-pass score and a red **Stop** button that
  finishes the in-flight pass cleanly.
- **17+ billion ray traversals per pass** — 8.5 billion primary rays
  (3840×2160 × 1024 spp) plus secondary GI bounces, each one a full Embree
  BVH traversal against ~10.5M triangles (three passes per benchmark run).
- **Intel Embree 4.4.1** — hardware-SIMD BVH and intersection kernels
  (ISPC-free, no TBB) for a workload that actually saturates modern CPUs.
- **Persistent thread pool** — worker threads are created once and reused
  across runs; benchmark time is spent rendering, not spawning.
- **Lock-free live preview** — the framebuffer double-buffers its pixels
  under a short lock, so the UI uploads and renders every frame *while*
  workers are still writing the next one. No stalls, no deadlocks.
- **Time-precise scoring** — the frame's exact elapsed time is measured with
  `steady_clock`, so a CPU that finishes the same work a fraction of a
  second faster scores proportionally higher instead of being rounded down
  to whole frames.
- **Built-in leaderboard** — your run is ranked against reference CPU
  profiles (Ryzen 9 9950X3D, i9-14900K, Ryzen 7 7800X3D, M3 Max,
  i5-13600K) in a single sortable table.
- **Result export** — every run is appended to `linuxbench_results.csv`
  (timestamp, CPU name, score, duration) and the final frame is saved as
  `benchmark.ppm`.
- **Headless / CI mode** — `LINUXBENCH_AUTORUN=<seconds>` starts the run
  automatically and exits when it finishes (see [Usage](#usage)).
- **AppImage distribution** — `./build_appimage.sh` produces a
  standalone `LinuxBench-x86_64.AppImage` that runs on any compatible
  Linux desktop.
- **Full-window native UI** — Dear ImGui renders edge-to-edge across the
  GLFW window: no floating panels, no decorations, live progress, score
  readout, and leaderboard.

## Prerequisites

- **OS:** Linux x86_64 (runs under X11 / XWayland)
- **Toolchain:** CMake ≥ 3.16, a C++20 compiler (GCC ≥ 11 or Clang ≥ 14),
  `wget` (only for the AppImage build)
- **OpenGL / X11 development libraries** — on Debian/Ubuntu:

```bash
sudo apt install \
    build-essential cmake g++ wget \
    libgl-dev \
    libx11-dev \
    libxext-dev \
    libxcursor-dev \
    libxrandr-dev \
    libxinerama-dev \
    libxi-dev \
    libxkbcommon-dev
```

> **Note:** on first configure, CMake's `FetchContent` downloads and builds
> GLFW 3.4, Dear ImGui v1.90.4, and Intel Embree v4.4.1 from source — a
> working internet connection is required at configure time only.

## Building

```bash
cd LinuxBench

# Configure (Release is the default, stated explicitly here)
cmake -B build -DCMAKE_BUILD_TYPE=Release

# Build with all cores
cmake --build build -j$(nproc)

# Run
./build/LinuxBench
```

### Packaging as an AppImage

From the project root:

```bash
./build_appimage.sh
```

The script:

1. Builds the project in Release mode,
2. Downloads `linuxdeploy-x86_64.AppImage` (skipped if already present) and
   makes it executable,
3. Assembles a standard `AppDir` layout with the binary, a
   `linuxbench.desktop` entry, and a placeholder `LB` icon,
4. Runs `linuxdeploy --appdir AppDir --output appimage`.

The result is a self-contained **`LinuxBench-x86_64.AppImage`** in the
project root — copy it anywhere on a compatible x86_64 Linux machine and
run it:

```bash
./LinuxBench-x86_64.AppImage
```

## Usage

### Interactive (desktop)

1. Start the app — the full-window UI appears with two run buttons side by
   side.
2. Pick a mode:
   - **Run Benchmark (3 Passes)** — the official scored run: three full
     passes (3840×2160, 1024 spp each) back-to-back. The UI shows the current
     pass (`Pass 2 / 3`), the last completed pass's score, and the window
     title tracks tile progress (title updates are throttled to one per
     250 ms — no more flicker).
   - **Stress Test (Infinite)** — passes run endlessly (`Stress Test:
     Pass 14`) with the last-pass score updated live after every pass. Pure
     load: nothing is written to CSV or PPM.
3. While a run is in flight, the red **Stop** button finishes the current
   pass cleanly and returns to the idle UI. A completed benchmark shows your
   score on the leaderboard and prints the result to stdout:

   ```text
   LinuxBench: pass 1 complete — 48.500 s, 32989 pts/pass
   LinuxBench: pass 2 complete — 47.900 s, 33402 pts/pass
   LinuxBench: pass 3 complete — 48.100 s, 33264 pts/pass
   LinuxBench: benchmark complete — 3 passes, 144.500 s total, score 33218 pts
   Wrote benchmark.ppm (3840x2160, P6)
   Appended result to linuxbench_results.csv
   ```

> **Heads up:** a full 4K frame at 1024 spp is a heavy render — on current
> desktop hardware expect on the order of a minute or more per *pass*, so a
> complete benchmark takes roughly three times as long.

### Headless / CI (`LINUXBENCH_AUTORUN`)

On servers where nobody is present to press the button:

```bash
# Start 5 s after launch (gives time to settle), then exit after the first run
LINUXBENCH_AUTORUN=5 ./LinuxBench

# Start immediately
LINUXBENCH_AUTORUN=0 ./LinuxBench
```

Behavior:

- The benchmark starts automatically `<seconds>` after startup
  (a negative or unset value disables autorun).
- After the **first** run completes, results are written and the app
  **exits on its own** — ideal for CI pipelines and cron jobs.
- The app still uses a GLFW window, so it needs a display: run it under
  `xvfb-run` (or a Wayland compositor with XWayland) on truly headless
  boxes:

  ```bash
  LINUXBENCH_AUTORUN=0 xvfb-run -a ./LinuxBench
  ```

## Scoring System

The benchmark renders **three** path-traced 4K frames — each pass is
3840×2160 at 1024 samples per pixel — back-to-back. Each pass's random
numbers are re-seeded (`worker_id + 1 + pass × 100`), so every pass sees a
different noise pattern and none of them is a warm-cache repeat of the
last.

The score is the work done divided by the time it took:

```
score = (1,600,000 × 3) / total_elapsed_seconds
```

`total_elapsed_seconds` is the sum of the three passes, each measured with
`std::chrono::steady_clock` between the pass start and the moment its last
tile is provably complete, so fractional seconds are kept:

- 3 × 48.500 s = 145.500 s → **33,058 pts**
- 3 × 46.000 s = 138.000 s → **34,782 pts**
- 3 × 44.000 s = 132.000 s → **36,363 pts**

The 1,600,000 × 3 numerator scales with the workload (3 × 4K × 1024 spp path
tracing), tuned so a Ryzen 9 9950X3D lands around **33,200 pts**. This
rewards real speed differences instead of rounding them away to whole
integer frames.

### Stress test

**Stress Test (Infinite)** runs the same passes endlessly. It is not a
scored run: nothing is written to CSV or PPM. Instead, the UI shows the
**last completed pass's** score (`1,600,000 / pass_time`) updated after
every pass, so you can watch the machine hold — or not — under sustained
load. The red **Stop** button finishes the in-flight pass cleanly, reports
the last pass's score, and returns to the idle screen.

**How to compare CPUs:** run the same binary, on the same OS, with the same
number of active threads (LinuxBench uses all cores). Higher score =
faster CPU. Scores are only comparable across runs of the *same*
LinuxBench build — the scene, resolution, sample count, and bounce depth
are fixed by the binary, so you don't need to worry about settings drifting.

### Exported results

| File | Contents |
|---|---|
| `linuxbench_results.csv` | `Timestamp, CPU Name, Score, TimeMs` — appended once per completed benchmark run (header written only on the first line; stress tests never log) |
| `benchmark.ppm` | The final rendered frame, 3840×2160, binary P6 |
| stdout | Human-readable completion line (seconds, score) |

## Project Layout

```
LinuxBench/
├── CMakeLists.txt          # CMake ≥ 3.16, C++20; fetches GLFW / ImGui / Embree
├── build_appimage.sh       # One-shot AppImage packaging script
├── include/
│   ├── Framebuffer.h       # Double-buffered pixel buffer + PPM export
│   ├── Scene.h             # Embree scene: BVH build, path-traced traceRay()
│   ├── ThreadPool.h        # Persistent worker pool
│   ├── TileDispatcher.h    # 64 px tile work distribution
│   └── Vec3.h
└── src/
    ├── main.cpp            # UI loop, benchmark state machine, 3-pass / stress runs, scoring, CSV, leaderboard
    ├── Framebuffer.cpp
    ├── Scene.cpp           # Procedural ~10.5M-triangle scene + PBR BSDFs (matte / metal / glass)
    └── TileDispatcher.cpp
```

## License

LinuxBench is distributed under the [MIT License](LICENSE).
Copyright (c) 2026 LinuxBench Contributors.
