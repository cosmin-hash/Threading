# Threading Model Visualiser — Multi-Producer Bounded Queues


![C++17](https://img.shields.io/badge/C%2B%2B-17-00599C?logo=cplusplus&logoColor=white)
![Qt 6](https://img.shields.io/badge/Qt-6-41CD52?logo=qt&logoColor=white)
![CMake](https://img.shields.io/badge/build-CMake-064F8C?logo=cmake&logoColor=white)
![platform](https://img.shields.io/badge/platform-Windows%20%7C%20Linux%20%7C%20macOS-555)
![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)

![pattern](https://img.shields.io/badge/pattern-multi--producer%20%2F%20single--consumer-8957e5)
![queues](https://img.shields.io/badge/queues-bounded%20%C3%974-1f6feb)
![backpressure](https://img.shields.io/badge/backpressure-drop--oldest-d29922)
![consumer](https://img.shields.io/badge/consumer-freshest--frame-2ea043)

A Qt 6 desktop app that **visually animates a real multithreading model**: N independent
producer threads each feeding their own bounded queue, drained by a single consumer that
keeps only the freshest frame per pane. The threading is genuine `std::thread` /
`std::mutex` / `std::atomic` code — the GUI only *paints* what the worker threads report.

It is the visual analogue of a small console program
([`multi-producer-bounded-queue.cpp`](src/multi-producer-bounded-queue.cpp)) that distils the
concurrency core of a Qt 6 / C++20 four-pane video player ("QtOpenGLQuadPlayers"): four image
streams, each decoded on its own thread and rendered by a single render loop.



https://github.com/user-attachments/assets/84f5faee-cff1-4cdd-b0bb-adde7c907a17



> See [`ARCHITECTURE.md`](ARCHITECTURE.md) for the threading-model diagram and data flow.

---

## The threading model

```
  PRODUCER 0 ──► [ bounded queue 0 ] ─┐
  PRODUCER 1 ──► [ bounded queue 1 ] ─┤
  PRODUCER 2 ──► [ bounded queue 2 ] ─┼──► CONSUMER ──► displayed
  PRODUCER 3 ──► [ bounded queue 3 ] ─┘   (render loop)
        (decode threads)                   (one thread)
```

* **N producers** (one `std::thread` per pane). Each generates `Frame`s at its own cadence
  and pushes them into **its own** `BoundedFrameQueue`. The panes run at deliberately
  different rates so the fast ones overrun their queue.
* **Per-pane bounded queues** (capacity 3, one `std::mutex` + `std::deque` each). When a
  push finds the queue full it **drops the OLDEST** frame ("drop-oldest" backpressure) so a
  slow consumer can never make a producer block or grow memory without bound.
* **One consumer** (a single `std::thread`, the render loop). Every tick it polls each
  queue and takes the **freshest** frame, **skipping** any staler ones still buffered. This
  keeps display latency low — you always render the newest decoded frame.
* **Cooperative, deterministic shutdown**: set an atomic stop flag, then `join()` every
  producer and the consumer **before** the queues are destroyed. No thread is ever killed
  mid-frame, and no producer can outlive the queue it writes to.

All of this lives in [`QuadPipeline.h`](src/QuadPipeline.h) / [`QuadPipeline.cpp`](src/QuadPipeline.cpp)
and contains **no GUI/painting code** — it only knows about `Frame` and emits Qt signals.

### Three possible outcomes for a frame

| Outcome       | Where it happens                              | Pile (visual)            |
|---------------|-----------------------------------------------|--------------------------|
| **Displayed** | consumer took it as the freshest in its pane  | `DISPLAYED` (full alpha) |
| **Dropped**   | evicted by drop-oldest when its queue was full| `DROPPED` (dimmed)       |
| **Skipped**   | consumer bypassed it as stale                 | `SKIPPED` (dimmed)       |

---

## How the visualisation maps to the model

| Model concept                  | Visual element                                                        |
|--------------------------------|----------------------------------------------------------------------|
| producer thread                | a 40px **station** circle (`PRODUCER n`) on the left                  |
| frame                          | a coloured **shape token** with a small id badge                     |
| bounded queue                  | a rounded **holding area** with 3 slots; turns **amber** when occupied|
| a thread actively working      | a rotating yellow **270° arc** ring around the station               |
| consumer rendering a frame     | the token **pulses** (radius wobble) at the consumer                  |
| an outcome                     | the token flies to a labelled **pile** (6-column grid of mini-shapes) |

Tokens move along dashed flow lines at ~60 fps using an `easeInOut` interpolation. A
**watchable delay** (hundreds of ms, not the skeleton's single-digit ms) keeps everything
legible.

### File layout (one class per pair, `AUTOMOC` on)

| File | Responsibility |
|------|----------------|
| [`Shapes.h`](src/Shapes.h) / [`Shapes.cpp`](src/Shapes.cpp) | the `Frame` item, `ShapeKind` enum, and the `paintShape()` primitive (shared by tokens and piles) |
| [`Token.h`](src/Token.h) | the animated-token struct, its state machine, and **all station/pile anchor points** for this topology |
| [`QuadPipeline.h`](src/QuadPipeline.h) / [`QuadPipeline.cpp`](src/QuadPipeline.cpp) | the **threading backbone** — real threads, queues, signals; no GUI |
| [`Canvas.h`](src/Canvas.h) / [`Canvas.cpp`](src/Canvas.cpp) | the animation loop + painting; owns the pipeline; the GUI thread (a pure consumer) |
| [`MainWindow.h`](src/MainWindow.h) / [`MainWindow.cpp`](src/MainWindow.cpp) | window, Start/Stop buttons, live stats label |
| [`main.cpp`](src/main.cpp) | entry point |

Supporting files: [`CMakeLists.txt`](CMakeLists.txt) (cross-platform build, Qt 6 **or** Qt 5),
[`run.ps1`](run.ps1) / [`run.sh`](run.sh) (Windows / Unix launchers),
and [`multi-producer-bounded-queue.cpp`](src/multi-producer-bounded-queue.cpp) (the console
skeleton this app visualises).

---

## How the GUI stays correct under concurrency

The GUI thread must **only paint** and must never be blocked by, or race with, the worker
threads. Several deliberate choices make that safe.

### 1. The GUI is a consumer, never a participant

Worker threads never touch widgets. They emit Qt signals that are delivered with
**`Qt::QueuedConnection`** (see the `connect(...)` calls in `Canvas::start()`), so every
cross-thread notification is marshalled into a `QMetaCallEvent` and handled on the GUI
thread's event loop. The painting code reads only GUI-thread-owned state.

### 2. Custom type crossing threads is registered

`Frame` is sent through queued signals, so it is declared with `Q_DECLARE_METATYPE(Frame)`
in [`Shapes.h`](src/Shapes.h) and registered with `qRegisterMetaType<Frame>("Frame")` in the
`QuadPipeline` constructor. Without this, queued delivery of a `Frame` would assert at
runtime.

### 3. Shutdown joins, it never kills

`Canvas::stop()` calls `QuadPipeline::shutdown()`, which sets the atomic stop flag and
`join()`s every thread. To stay responsive, the worker sleeps are **interruptible**
(`interruptibleSleep()` wakes every 20 ms and rechecks the flag), so Stop returns in at
most a fraction of a producer period rather than blocking for a full 1.5 s tick. After the
join, `processEvents()` drains any late queued signals before the pipeline is deleted.

### 4. The drop-oldest / take-freshest logic is honest

`BoundedFrameQueue::push()` and `popLatest()` are the real backpressure logic, each fully
guarded by the queue's own `std::mutex`. The slow per-frame work (here a sleep; in the real
player, decode/render) always happens **outside** the lock, so producers and the consumer
never serialise on each other beyond the brief enqueue/dequeue.

---

## The concurrency pitfall this app fixes (cross-thread signal ordering)

> Symptom: after running for a while, one pane's on-screen queue would show **4** tokens
> even though capacity is 3 — a token spilling outside the box that never left.

This was **not** a bug in the threading core (the real queues are always ≤ 3). It was a
bookkeeping bug in the GUI's *mirror* of the queues, caused by a subtle ordering guarantee
that Qt does **not** make.

### Why it happened

The GUI keeps a per-pane `std::deque<int> m_queueIds` mirroring what each queue visually
holds, so it can place, slide, and remove tokens. It is maintained purely from signals:

* `framePushed(X)` → add X to the mirror (emitted by a **producer** thread)
* `frameDropped(X)` / `frameSkipped(X)` / `frameDisplayed(X)` → remove X (the last two are
  emitted by the **consumer** thread)

Qt guarantees queued events are delivered **in order per sender thread**, but gives **no
ordering guarantee between two different sender threads**. Now look at the producer:

```cpp
std::vector<Frame> dropped = m_queues[pane]->push(f); // locks & UNLOCKS the queue mutex
// ... producer can be preempted right here ...
emit framePushed(f);                                  // emit happens AFTER the unlock
```

So this interleaving is possible:

1. Producer pushes frame **X** into the queue and releases the mutex, **but is preempted
   before emitting `framePushed(X)`**.
2. The consumer locks the queue, consumes X, and emits `frameDisplayed(X)` /
   `frameSkipped(X)` — these get posted to the GUI event queue **first**.
3. The producer resumes and only now posts `framePushed(X)`.

The GUI therefore processed the *terminal* signal **before** the *push*:
`onFrameDisplayed(X)` looked for X in the mirror, found nothing, and did nothing; then
`onFramePushed(X)` added X to the mirror — **permanently**. X became a "ghost" with no
remaining signal to ever remove it. The fastest pane (producer 2, 320 ms) accrued ghosts
first, so its queue was the first to visibly overflow.

### The fix — defer out-of-order terminal outcomes

Implemented in [`Canvas.cpp`](src/Canvas.cpp) / [`Canvas.h`](src/Canvas.h):

* A small map `std::unordered_map<int, Outcome> m_pendingTerminal` parks any
  displayed/skipped/dropped outcome that arrives for a frame the GUI hasn't seen pushed yet
  (instead of silently discarding it):

  ```cpp
  void Canvas::onFrameDisplayed(Frame frame) {
      if (!routeTerminal(frame.id, frame.pane, OutDisplayed))   // token not spawned yet?
          m_pendingTerminal[frame.id] = OutDisplayed;            // park it
      emitStats("running");
  }
  ```

* `onFramePushed` checks that map first. If an outcome is already waiting, it spawns the
  token and routes it **straight to its outcome** instead of parking it in the queue:

  ```cpp
  auto pit = m_pendingTerminal.find(frame.id);
  if (pit != m_pendingTerminal.end()) {
      const Outcome outcome = pit->second;
      m_pendingTerminal.erase(pit);
      m_tokens.push_back(tok);
      routeTerminal(frame.id, frame.pane, outcome);      // never enters the mirror
      return;
  }
  ```

* The three terminal handlers were consolidated into one `routeTerminal(id, pane, outcome)`
  helper (returns `false` when the token isn't known yet), and the map is cleared in
  `start()`.

The result: **every** pushed frame has its outcome applied exactly once, regardless of the
order the two threads' signals happen to reach the GUI. The mirror can never strand a
token, and the on-screen queue is provably bounded by capacity. Soak-tested past 500
produced frames with every queue staying at ≤ 3.

> Note on a transient: the same lock-release-before-emit window can momentarily show a
> queue at cap+1 for a few milliseconds until the matching terminal signal is processed.
> The deferral logic guarantees that always self-corrects; it is never permanent.

---

## Build & run

The project is **OS-independent** — the same sources build and run on Windows, Linux and
macOS. The code is plain C++17 (`std::thread` / `std::mutex` / `std::atomic`) plus Qt Widgets;
the [`CMakeLists.txt`](CMakeLists.txt) auto-detects **Qt 6 or Qt 5**, links the platform
threading library, and emits a console-less `.exe` on Windows and a proper `.app` bundle on
macOS.

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

### Run on Linux / macOS

After the generic build above, launch with the included [`run.sh`](run.sh):

```bash
chmod +x run.sh        # first time only
./run.sh               # or: QT_DIR=~/Qt/6.9.2/gcc_64 ./run.sh
```

It adds `$QT_DIR/lib` to the loader path when set, then launches `build/ThreadingViz`
(Linux) or opens `build/ThreadingViz.app` (macOS). If Qt is installed system-wide you can
also just run the binary directly. To bundle Qt for distribution, use
[`macdeployqt`](https://doc.qt.io/qt-6/macos-deployment.html) on macOS or
[`linuxdeployqt`](https://github.com/probonopd/linuxdeployqt) / an AppImage on Linux.

### Using it

Press **Start** to launch the threads; **Stop** flags + joins them cleanly (it stays
disabled until started). The stats line shows live **Produced / Dropped / Skipped /
Displayed** counts.

### Continuous integration

CI lives at the repository root — [`/.github/workflows/build.yml`](../.github/workflows/build.yml)
builds this project together with the other three on Ubuntu, Windows and macOS on every push/PR
and smoke-tests the binary headlessly on Linux. The build badge is in the
[collection README](../README.md).

---

## Tuning

The cadences that make backpressure visible live at the bottom of
[`QuadPipeline.h`](src/QuadPipeline.h):

```cpp
static constexpr int kPeriods[kPaneCount] = {650, 1000, 320, 1500}; // per-pane producer ms
static constexpr int kTickMs = 1200;                                 // consumer tick ms
```

Pane 2 (320 ms) is much faster than the 1200 ms consumer tick, so it overruns capacity and
exercises drop-oldest; pane 3 (1500 ms) is slower than the tick, so its queue is sometimes
empty. Queue depth is `kCapacity` and the topology coordinates are in
[`Token.h`](src/Token.h).
