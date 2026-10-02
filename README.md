# LinuxBench

[![C++20](https://img.shields.io/badge/C%2B%2B-20-blue?style=for-the-badge)](https://isocpp.org)
[![Platform](https://img.shields.io/badge/Platform-Linux%20x86_64-green?style=for-the-badge)](https://www.linux.org)
[![Embree](https://img.shields.io/badge/Embree-4.4.1-6ea1c3?style=for-the-badge)](https://embree.github.io)
[![Dear ImGui](https://img.shields.io/badge/Dear%20ImGui-1.90.4-7c4dff?style=for-the-badge)](https://github.com/ocornut/imgui)
[![GLFW](https://img.shields.io/badge/GLFW-3.4-5c9e65?style=for-the-badge)](https://www.glfw.org)
[![License](https://img.shields.io/badge/License-MIT-yellow?style=for-the-badge)](LICENSE)

> **A native, Cinebench-style CPU stress-test and benchmark for Linux.**
> Real raytracing. Real hardware. One clean score.

## Overview

LinuxBench is a single-purpose CPU benchmark that turns your machine into a
raytracer and measures how fast it is. It renders a 1920×1080 frame from a
procedural scene of roughly **10 million triangles** — built on
[Intel Embree 4](https://embree.github.io) for production-grade SIMD BVH
construction and ray intersection — and runs that exact workload in a
precise **10-second window**. The score is your throughput: how many full
frames per second the CPU can push, scaled to thousands of points.

The whole application is a single native binary: GLFW provides the window
and OpenGL context, Dear ImGui draws a full-window live UI (progress, score,
leaderboard), and a persistent worker pool renders tiles of the frame while
you watch the image assemble in real time. No Python wrappers, no Electron,
no telemetry — just C++20 and your silicon.

## Features

- **Heavy, realistic workload** — a procedural scene of 28³ (21,952)
  tessellated spheres merged into one triangle-mesh geometry (~10M
  triangles), one BVH built by Embree, and **hard shadows** via a full
  second BVH traversal per pixel.
- **Intel Embree 4.4.1** — hardware-SIMD BVH and intersection kernels
  (ISPC-free, no TBB) for a workload that actually saturates modern CPUs.
- **Persistent thread pool** — worker threads are created once and reused
  across runs; benchmark time is spent rendering, not spawning.
- **Lock-free live preview** — the framebuffer double-buffers its pixels
  under a short lock, so the UI uploads and renders every frame *while*
  workers are still writing the next one. No stalls, no deadlocks.
- **Time-precise scoring** — the 10-second window is measured with
  `steady_clock` and fractional throughput is kept, so a CPU that is a
  fraction of a second faster scores proportionally higher instead of
  being rounded down to whole frames.
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

1. Start the app — the full-window UI appears with a **Run Benchmark**
   button.
2. Click **Run Benchmark**. The benchmark renders for exactly 10 seconds
   while the live preview shows the frame being assembled; the window title
   tracks frame/tile progress.
3. When the window closes, the UI shows your score, places you on the
   leaderboard, and prints the result to stdout:

   ```text
   LinuxBench: benchmark complete — 10.0 s, 881 frames, score 88100 pts
   Wrote benchmark.ppm (1920x1080, P6)
   Appended result to linuxbench_results.csv
   ```

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

The benchmark runs for exactly **10.0 seconds** and counts how many complete
1920×1080 frames it renders. The score is:

```
score = frames / exact_elapsed_seconds × 1000
```

elapsed time is measured with `std::chrono::steady_clock` between the first
frame start and the window close, so fractional frames per second are kept:

- 880 frames in exactly 10.00 s → **88,000 pts**
- the same CPU doing 881 frames → **88,100 pts**

This rewards real speed differences instead of rounding them away to whole
integer frames.

**How to compare CPUs:** run the same binary, on the same OS, with the same
number of active threads (LinuxBench uses all cores). Higher score =
faster CPU. Scores are only comparable across runs of the *same*
LinuxBench build — the scene, resolution, and shadow workload are fixed
by the binary, so you don't need to worry about settings drifting.

### Exported results

| File | Contents |
|---|---|
| `linuxbench_results.csv` | `Timestamp, CPU Name, Score, TimeMs` — appended once per run (header written only on the first line) |
| `benchmark.ppm` | The final rendered frame, 1920×1080, binary P6 |
| stdout | Human-readable completion line (seconds, frames, score) |

## Project Layout

```
LinuxBench/
├── CMakeLists.txt          # CMake ≥ 3.16, C++20; fetches GLFW / ImGui / Embree
├── build_appimage.sh       # One-shot AppImage packaging script
├── include/
│   ├── Framebuffer.h       # Double-buffered pixel buffer + PPM export
│   ├── Scene.h             # Embree scene: BVH build, traceRay()
│   ├── ThreadPool.h        # Persistent worker pool
│   ├── TileDispatcher.h    # 32 px tile work distribution
│   └── Vec3.h
└── src/
    ├── main.cpp            # UI loop, benchmark state machine, scoring, CSV, leaderboard
    ├── Framebuffer.cpp
    ├── Scene.cpp           # Procedural ~10M-triangle scene + shadow rays
    └── TileDispatcher.cpp
```

## License

LinuxBench is distributed under the [MIT License](LICENSE).
Copyright (c) 2026 LinuxBench Contributors.
