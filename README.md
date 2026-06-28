# Threading Model Visualisers

<!-- VIDEO -->
<!-- ^ Collection sizzle-reel goes here: a montage of the four apps running (drag a .mp4/.gif into the GitHub editor, or link a release asset). -->

[![build](https://github.com/cosmin-hash/Threading/actions/workflows/build.yml/badge.svg)](https://github.com/cosmin-hash/Threading/actions/workflows/build.yml)
![C++](https://img.shields.io/badge/C%2B%2B-17%20%2F%2023-00599C?logo=cplusplus&logoColor=white)
![Qt 6](https://img.shields.io/badge/Qt-6-41CD52?logo=qt&logoColor=white)
![CMake](https://img.shields.io/badge/build-CMake-064F8C?logo=cmake&logoColor=white)
![platform](https://img.shields.io/badge/platform-Windows%20%7C%20Linux%20%7C%20macOS-555)
![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)
![projects](https://img.shields.io/badge/projects-4-8957e5)

A set of **Qt 6 desktop apps that visually animate real multithreading models**. Each one ports
a genuine `std::thread` / `std::mutex` / `std::condition_variable` / `std::atomic` console
program into a dedicated, GUI-free backbone class and renders what its worker threads report —
the GUI thread is always a pure **consumer** that only paints, never blocks. Every project shares
one visual vocabulary (shape-and-colour tokens, stations, holding areas, rotating "working" arcs,
and accumulating outcome piles) so the *difference between the patterns* is what stands out.

## The collection

| # | Project | Pattern | Sync primitives | Outcomes |
|---|---------|---------|-----------------|----------|
| 01 | [Single-Slot CV Hand-off](concurrency-01/README.md) | producer → consumer | `mutex` + `condition_variable` | Displayed / Dropped |
| 02 | [Multi-Producer Bounded Queues](concurrency-02/README.md) | N producers → 1 consumer | per-queue `mutex`, `atomic` | Displayed / Dropped / Skipped |
| 03 | [Staged Pipeline + Thread Pool](concurrency-03/README.md) | pipeline + worker pool | bounded MPMC channel (2× `condition_variable`) + `future` pool | Consumed / Throttled |
| 04 | [Work-Stealing Scheduler](concurrency-04/README.md) | work-stealing pool | `mutex`-per-deque + `atomic`s | Ran (own) / Stolen |

Each row links to a project; each project also ships an
[`ARCHITECTURE.md`](concurrency-01/ARCHITECTURE.md) with its threading-model diagram and data
flow, and a `docs/screenshot.png`. The **build** badge above covers all four (one repo-wide CI
run across Ubuntu, Windows and macOS).

## What they have in common

* **Genuine concurrency, not faked on a timer.** The threading lives in a backbone class
  (`FramePipeline` / `QuadPipeline` / `PipelineEngine` / `WorkStealingPool`) that knows nothing
  about painting. Worker threads emit Qt signals delivered with `Qt::QueuedConnection`; the GUI
  marshals them onto its event loop and only animates.
* **The GUI is never blocked.** Custom item types crossing threads are registered with
  `Q_DECLARE_METATYPE` + `qRegisterMetaType`. Where two different threads can announce events for
  the same item out of order, the canvas parks the furthest-announced stage and fast-forwards the
  token once it spawns — so nothing is ever stranded on screen.
* **Cooperative shutdown.** Every app sets a stop flag, wakes all threads, drains in-flight work,
  and `join()`s — no thread is killed mid-task.
* **Watchable pacing.** The real models run in tens of milliseconds; each visualiser slows the
  hand-off to hundreds of ms so the behaviour is legible at ~60 fps.
* **Same layout, same build.** Each project follows an identical structure
  (`src/`, `docs/`, `.github/`, top-level `ARCHITECTURE.md` / `CMakeLists.txt` / `LICENSE` /
  `README.md`) and builds cross-platform with CMake (Qt 6 or Qt 5, `Threads::Threads`).

## Console skeletons

Each app is the visual analogue of a small standalone console program. A copy of the relevant
skeleton lives in that project's `src/` (e.g. [`concurrency-04/src/work_stealing_pool_core.cpp`](concurrency-04/src/work_stealing_pool_core.cpp))
— it is reference material that distils the threading model, not compiled into the app.

## Build & run (any project)

The projects are OS-independent. From inside a project folder:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="<your-qt-kit-dir>"
cmake --build build
```

The result is `build/<App>` (Linux), `build/<App>.app` (macOS), or `build/<App>.exe` (Windows;
the Qt runtime is auto-deployed next to the exe via `windeployqt`). See each project's README for
platform-specific notes and the `run.ps1` / `run.sh` launchers.

## Continuous integration

A repo-level [`.github/workflows/build.yml`](.github/workflows/build.yml) builds **all four
projects** across Ubuntu, Windows and macOS on each push/PR, smoke-tests each binary headlessly on
Linux (`QT_QPA_PLATFORM=offscreen`), and uploads the built artifacts — this is what the **build**
badge at the top reflects. (Each project also keeps its own `build.yml` for standalone use.)

## License

[MIT](concurrency-01/LICENSE) © 2026 Cosmin Mandachescu — each project carries its own copy.
