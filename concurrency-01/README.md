

https://github.com/user-attachments/assets/b6f9917e-6474-4ec0-a495-ed929a3e57fc

# Threading Model Visualiser — Single-Slot Condition-Variable Hand-off

<!-- VIDEO -->
<!-- ^ Demo screen-capture goes here: drag a .mp4/.gif into the GitHub editor, or link a release asset. -->

![C++17](https://img.shields.io/badge/C%2B%2B-17-00599C?logo=cplusplus&logoColor=white)
![Qt 6](https://img.shields.io/badge/Qt-6-41CD52?logo=qt&logoColor=white)
![CMake](https://img.shields.io/badge/build-CMake-064F8C?logo=cmake&logoColor=white)
![platform](https://img.shields.io/badge/platform-Windows%20%7C%20Linux%20%7C%20macOS-555)
![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)

![pattern](https://img.shields.io/badge/pattern-producer--consumer-8957e5)
![sync](https://img.shields.io/badge/sync-mutex%20%2B%20condition__variable-1f6feb)
![hand-off](https://img.shields.io/badge/hand--off-single%20slot-fb8500)
![backpressure](https://img.shields.io/badge/backpressure-drop--newest-d29922)

A Qt 6 desktop app that **visually animates a real multithreading model**: a dedicated
reader thread pulls frames from an input source and hands each, through a single mutex-guarded
slot, to one worker thread that processes it and posts the finished frame to the GUI for
display. The threading is genuine `std::thread` / `std::mutex` / `std::condition_variable` /
`std::atomic` code — the GUI only *paints* what the worker threads report.

It is the visual analogue of a small console program ([`producer-consumer-cv.cpp`](src/producer-consumer-cv.cpp))
that distils the concurrency core of a Qt 6 / C++20 dual-display MP4 player ("Mp4DualPlayer"):
one worker thread doing per-frame decode/scale/convert off the UI thread, fed through a single
hand-off slot that drops frames it cannot keep up with.

![pipeline](docs/screenshot.png)

> See [`ARCHITECTURE.md`](ARCHITECTURE.md) for the threading-model diagram and data flow.

---

## The threading model

```
  READER thread  ──►  [ single slot ]  ──►  WORKER thread  ──►  GUI thread
  (input source)      (mutex + condvar)     (processing)        (display)
       produces        drops if full         decodes            consumes
```

* **One reader thread** (a `std::thread`). It pulls frames from an input source — here
  synthesised by `makeFrame()`; in the real player it reads & decodes the next frame from
  disk, a socket or a capture device — and posts each into a **single** hand-off slot. It
  runs faster than the worker, so it regularly finds the slot still full.
* **A single-slot, mutex-guarded hand-off** (`std::mutex` + `std::condition_variable` + one
  `Frame`). When a frame is read while the slot is still occupied, the **incoming (newest)**
  frame is **dropped** — real-time playback favours freshness over completeness, and the
  reader can never block or buffer without bound.
* **One worker thread** (the consumer). It **sleeps on the condition variable** until a frame
  is posted or stop is requested — no polling, an idle worker uses 0% CPU. It takes the
  pending frame, releases the mutex, does the slow per-frame work **outside the lock**, then
  hands the finished frame to the GUI thread (via a queued signal) for display.
* **The GUI thread only consumes.** It never produces and is never blocked; it displays
  finished frames and accumulates them.
* **Cooperative, deterministic shutdown**: set the stop flag, wake the worker (`notify_all`),
  then `join()` **both** the reader and worker. No thread is ever killed mid-frame — each
  finishes the step it is on and exits.

All of this lives in [`FramePipeline.h`](src/FramePipeline.h) / [`FramePipeline.cpp`](src/FramePipeline.cpp)
and contains **no GUI/painting code** — it only knows about `Frame` and emits Qt signals.

### Two possible outcomes for a frame

| Outcome       | Where it happens                                  | Pile (visual)            |
|---------------|---------------------------------------------------|--------------------------|
| **Displayed** | worker finished it and handed it to the GUI       | `DISPLAYED` (full alpha) |
| **Dropped**   | read while the slot was still full → newest dropped | `DROPPED` (dimmed)     |

---

## How the visualisation maps to the model

| Model concept                  | Visual element                                                        |
|--------------------------------|----------------------------------------------------------------------|
| reader / worker / GUI thread   | a 50px **station** circle with a two-line label                       |
| frame                          | a coloured **shape token** with a small id badge                     |
| single hand-off slot           | a rounded **holding box**; turns **amber** when occupied              |
| a thread actively working      | a rotating yellow **270° arc** ring around the worker station        |
| worker processing a frame      | the token **pulses** (radius wobble) at the worker                    |
| an outcome                     | the token flies to a labelled **pile** (6-column grid of mini-shapes) |

Every frame is one of six shapes — circle, square, rectangle, star, triangle, hexagon — each
in its own vivid random colour. Tokens move along dashed flow lines at ~60 fps using an
`easeInOut` interpolation. A **watchable delay** (hundreds of ms, not the skeleton's tens of
ms) keeps everything legible.

### File layout (one class per pair, `AUTOMOC` on)

| File | Responsibility |
|------|----------------|
| [`Shapes.h`](src/Shapes.h) / [`Shapes.cpp`](src/Shapes.cpp) | the `Frame` item, `ShapeKind` enum, and the `paintShape()` primitive (shared by tokens and piles) |
| [`Token.h`](src/Token.h) | the animated-token struct, its state machine, and **all station/pile anchor points** for this topology |
| [`FramePipeline.h`](src/FramePipeline.h) / [`FramePipeline.cpp`](src/FramePipeline.cpp) | the **threading backbone** — the reader + worker threads, the slot, signals; no GUI |
| [`Canvas.h`](src/Canvas.h) / [`Canvas.cpp`](src/Canvas.cpp) | the animation loop + painting; owns the pipeline; the GUI thread (a pure consumer) |
| [`MainWindow.h`](src/MainWindow.h) / [`MainWindow.cpp`](src/MainWindow.cpp) | window, Start/Stop buttons, live stats label |
| [`main.cpp`](src/main.cpp) | entry point |

Supporting files: [`CMakeLists.txt`](CMakeLists.txt) (cross-platform build, Qt 6 **or** Qt 5),
[`run.ps1`](run.ps1) / [`run.sh`](run.sh) (Windows / Unix launchers),
and [`producer-consumer-cv.cpp`](src/producer-consumer-cv.cpp) (the console skeleton this
app visualises).

---

## How the GUI stays correct under concurrency

The GUI thread must **only paint** and must never be blocked by, or race with, the worker
threads. Several deliberate choices make that safe.

### 1. The GUI is a consumer, never a participant

The reader and worker threads never touch widgets. They emit Qt signals delivered with
**`Qt::QueuedConnection`** (see the `connect(...)` calls in `Canvas::start()`), so every
cross-thread notification is marshalled into a `QMetaCallEvent` and handled on the GUI
thread's event loop. The painting code reads only GUI-thread-owned state.

### 2. The custom type crossing threads is registered

`Frame` travels through queued signals, so it is declared with `Q_DECLARE_METATYPE(Frame)` in
[`Shapes.h`](src/Shapes.h) and registered with `qRegisterMetaType<Frame>("Frame")` in the
`FramePipeline` constructor. Without this, queued delivery of a `Frame` would assert at
runtime.

### 3. Shutdown joins, it never kills

`Canvas::stop()` calls `FramePipeline::shutdown()`, which sets the stop flag under the mutex,
`notify_all()`s the condition variable, then `join()`s the reader and the worker. Each thread
finishes the step it is on first — the reader its current read sleep, the worker the frame it
is processing — so Stop returns in at most one read period plus one processing delay, and no
frame is ever abandoned half-done. After the join, `processEvents()` drains any late queued
signals before the pipeline is deleted.

### 4. The slow work happens outside the lock

`FramePipeline::workLoop()` copies the pending frame and **releases the mutex before** the
simulated per-frame work (`sleep_for`; in the real player, decode/scale/convert). The reader
and worker therefore only ever serialise on the brief slot enqueue/dequeue, never on the
expensive stage.

---

## The concurrency pitfall this app fixes (cross-thread signal ordering)

> Symptom: occasionally a token would sail into the single-slot box and **never leave** — a
> ghost frame sitting in a slot whose real capacity is one, while later frames piled up
> behind it.

This was **not** a bug in the threading core (the real slot holds exactly one frame at a
time). It was a bookkeeping bug in the GUI's *mirror* of the pipeline, caused by an ordering
guarantee that Qt does **not** make.

### Why it happened

The token for a frame is driven entirely by signals coming from **two different threads**:

* `frameAccepted(f)` → spawn the token at the reader (emitted by the **reader** thread)
* `processingStarted(id)` / `frameReady(f)` → advance the token to the worker, then to the
  GUI (emitted by the **worker** thread)

Qt guarantees queued events are delivered **in order per sender thread**, but gives **no
ordering guarantee between two different sender threads**. Now look at the hand-off:

```cpp
// reader thread                         // worker thread
m_pending = f; m_hasPending = true;
m_cv.notify_one();
                          // (lock released here)
// ... reader can be preempted RIGHT HERE ...
                                         m_cv.wait(...);          // wakes: slot is full
                                         frame = m_pending;       // takes it, unlocks
                                         emit processingStarted(frame.id);  // posted FIRST
emit frameAccepted(f);                   // posted only now, SECOND
```

So this interleaving is possible:

1. The reader posts frame **X** into the slot and releases the mutex, **but is preempted
   before emitting `frameAccepted(X)`**.
2. The worker wakes on the CV, takes X, and emits `processingStarted(X)` (and possibly
   `frameReady(X)`) — these get posted to the GUI event queue **first**.
3. The reader resumes and only now posts `frameAccepted(X)`.

The GUI therefore processed the *advance* signals **before** the *spawn*:
`onProcessingStarted(X)` looked for X's token, found nothing, and did nothing; then
`onFrameAccepted(X)` spawned X heading into the slot. Its "started"/"ready" signals were
already gone, so the token reached the slot and **stayed there forever**. (Note that
`processingStarted` and `frameReady` share one sender — the worker — so they keep their
relative order; only `frameAccepted` can arrive late.)

### The fix — defer out-of-order advances

Implemented in [`Canvas.cpp`](src/Canvas.cpp) / [`Canvas.h`](src/Canvas.h):

* A small map `std::unordered_map<int, PendingStage> m_pending` parks the *furthest stage*
  announced for a frame whose token has not spawned yet, instead of dropping it:

  ```cpp
  void Canvas::onProcessingStarted(int id) {
      m_workerBusy = true;
      if (!advanceToWorker(id))      // token not spawned yet?
          m_pending[id] = StStarted; //   remember it has at least started
  }
  void Canvas::onFrameReady(Frame f) {
      m_workerBusy = false;
      if (!advanceToGui(f.id))
          m_pending[f.id] = StReady;
  }
  ```

* `onFrameAccepted` checks that map first. If an advance is already waiting, it spawns the
  token and routes it **straight to the stage already reached** instead of into the slot:

  ```cpp
  auto it = m_pending.find(frame.id);
  if (it != m_pending.end()) {
      const PendingStage stage = it->second;
      m_pending.erase(it);
      tok.state = (stage == StReady) ? Token::ToGui : Token::ToWorker;
      tok.to    = (stage == StReady) ?  kGuiPt      :  kWorkerPt;
      m_tokens.push_back(tok);
      return;                         // never enters the slot mirror
  }
  ```

* The two advance handlers were factored into `advanceToWorker(id)` / `advanceToGui(id)`
  helpers (each returns `false` when the token is not known yet), and the map is cleared in
  `start()`.

The result: **every** accepted frame has its outcome applied exactly once, regardless of the
order the reader's and worker's signals happen to reach the GUI. No token can be stranded in
the slot, and the on-screen slot is provably bounded to a single occupant.

---

## Build & run

The project is **OS-independent** — the same sources build and run on Windows, Linux and
macOS. The code is plain C++17 (`std::thread` / `std::mutex` / `std::condition_variable`) plus
Qt Widgets; the [`CMakeLists.txt`](CMakeLists.txt) auto-detects **Qt 6 or Qt 5**, links the
platform threading library, and emits a console-less `.exe` on Windows and a proper `.app`
bundle on macOS.

### Prerequisites

* **Qt 6 or Qt 5** with the *Widgets* module
* a **C++17** compiler (GCC, Clang, or MSVC)
* **CMake ≥ 3.16** (and, optionally, Ninja — any CMake generator works)

### Generic build (Linux / macOS / Windows)

Point CMake at your Qt installation with `CMAKE_PREFIX_PATH` (the directory that contains
`lib/cmake/Qt6`, e.g. `.../Qt/6.9.2/gcc_64` on Linux, `.../Qt/6.9.2/macos` on macOS,
`.../Qt/6.9.2/mingw_64` or `.../msvc2019_64` on Windows):

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="<your-qt-kit-dir>"
cmake --build build
```

The resulting binary is `build/ThreadingViz` on Linux, `build/ThreadingViz.app` on macOS, and
`build/ThreadingViz.exe` on Windows. If Qt came from a system package (e.g. `apt install
qt6-base-dev` or `brew install qt`), you can usually omit `CMAKE_PREFIX_PATH` entirely.

### Windows (Qt MinGW kit)

Replace the example paths below with **your own** Qt installation directories:

```powershell
$qt    = "C:\Qt\6.9.2\mingw_64"           # <-- your Qt kit
$mingw = "C:\Qt\Tools\mingw1310_64\bin"   # <-- your MinGW
$ninja = "C:\Qt\Tools\Ninja"
$env:PATH = "$qt\bin;$mingw;$ninja;$env:PATH"

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release `
      -DCMAKE_PREFIX_PATH="$qt" -DCMAKE_CXX_COMPILER="$mingw\g++.exe"
cmake --build build

# Deploy the Qt runtime DLLs next to the exe (once):
& "$qt\bin\windeployqt.exe" build\ThreadingViz.exe
```

A convenience launcher [`run.ps1`](run.ps1) is included; it reads optional `QT_DIR` /
`MINGW_DIR` environment variables (falling back to documented defaults), puts the Qt runtime
on `PATH`, and launches the exe. Alternatively run `build\ThreadingViz.exe` directly after
`windeployqt`.

### Linux / macOS

After the generic build above, use the included [`run.sh`](run.sh) (first make it
executable):

```bash
chmod +x run.sh          # once
./run.sh                 # or: QT_DIR=~/Qt/6.9.2/gcc_64 ./run.sh
```

It adds `$QT_DIR/lib` to the loader path when set, then launches `build/ThreadingViz`
(Linux) or opens `build/ThreadingViz.app` (macOS). If Qt is installed system-wide you can
also just run the binary directly. To bundle Qt for distribution, use
[`macdeployqt`](https://doc.qt.io/qt-6/macos-deployment.html) on macOS or
[`linuxdeployqt`](https://github.com/probonopd/linuxdeployqt) / an AppImage on Linux.

### Using it

Press **Start** to launch the reader and worker threads; **Stop** flags + joins them cleanly
(it stays disabled until started). The stats line shows live **Read / Dropped / Displayed**
counts.

### Continuous integration

CI lives at the repository root — [`/.github/workflows/build.yml`](../.github/workflows/build.yml)
builds this project together with the other three on Ubuntu, Windows and macOS on every push/PR
and smoke-tests the binary headlessly on Linux. The build badge is in the
[collection README](../README.md).

---

## Tuning

The cadences that make the frame-drop behaviour visible live at the bottom of
[`Canvas.h`](src/Canvas.h):

```cpp
static constexpr int kReadMs = 650;    // input read/decode cadence (reader thread)
static constexpr int kWorkMs = 1300;   // heavy per-frame processing (worker thread)
```

Because the reader (650 ms) is about twice as fast as the worker (1300 ms), the slot is full
roughly half the time and frames are steadily dropped. Raise `kWorkMs` or lower `kReadMs` for
more dramatic drops; bring them closer together to see the slot drain cleanly. The station and
pile coordinates are in [`Token.h`](src/Token.h).
