# Architecture

`WorkStealingViz` (concurrency-04) visually animates a **work-stealing thread-pool scheduler**:
each worker owns its own double-ended queue (deque), runs its own tasks LIFO, and — when idle —
**steals** work from a random busy worker's deque. The design goal: avoid the lock bottleneck of
one central queue, and let idle cores self-balance the load by pulling work from busy ones.

![Concurrency architecture](docs/screenshot.png)

## Threading model

```
                       ┌─► WORKER 0 deque ─► WORKER 0  (heavy load)
  SUBMIT  ─round-robin─┤   WORKER 1 deque ─► WORKER 1 ─┐
  (one thread)         │   WORKER 2 deque ─► WORKER 2  ├─ an idle worker STEALS from
                       └─► WORKER 3 deque ─► WORKER 3 ─┘  a random victim's FRONT
```

| Object | Thread | Responsibility |
| --- | --- | --- |
| submitter loop | one `std::thread` | distribute tasks **round-robin** to the workers' deques (`pushBack`) |
| per-worker deque | shared (`std::deque` + `std::mutex`, one per worker) | owner pops the **BACK** (LIFO); a thief steals the **FRONT** (FIFO) |
| worker loop | one `std::thread` per worker (4) | drain own deque; when empty, steal from a random victim; else sleep on a CV |
| [`Canvas`](src/Canvas.cpp) | GUI thread | animate tokens, paint deques/stations/piles — consumer only |

The backbone ([`WorkStealingPool.h`](src/WorkStealingPool.h) /
[`WorkStealingPool.cpp`](src/WorkStealingPool.cpp)) contains **no GUI code** — it only knows
about `Task` and emits Qt signals delivered with `Qt::QueuedConnection`. Each deque is
mutex-guarded (correct and clear) rather than lock-free; the work-stealing *structure* is the
lesson. Every task whose home is worker 0 is heavy, so its deque backs up while idle workers
steal the surplus — making the rebalancing visible.

## Data flow for one task

```
submitter: makeTask(i) → push to deque[i % workers]   ── worker 0's tasks are heavy
worker: popBack(own)         → run (sleep by cost)     ── LIFO, contention-free
     or stealFront(victim)   → run (sleep by cost)     ── FIFO, opposite end
  → emit taskStarted() / taskFinished()                ── queued signals → GUI thread
GUI thread: animate token → COMPLETED (own) or STOLEN pile
```

## Outcomes

| Outcome | Where it happens | Pile |
| --- | --- | --- |
| **Ran (own)** | a worker popped it from its own deque's back | `COMPLETED (own)` (full alpha) |
| **Stolen** | an idle worker stole it from another worker's front | `STOLEN & DONE` (dimmed) |

Both are successful completions; because worker 0 hoards the heavy tasks, the **stolen** pile
typically fills faster — that *is* the work-stealing lesson, made visible.

## Correctness notes

* **Cooperative, drain-then-join shutdown**: flag stop, wake everyone, join the submitter (so
  no new work appears), then let workers drain the backlog (idle workers keep stealing during
  shutdown) and join — no task in flight is abandoned.
* **Cross-thread signal ordering**: `taskSubmitted` (submitter thread) can arrive at the GUI
  *after* `taskStarted` / `taskFinished` (worker thread). `Canvas` parks the furthest stage in
  a map and fast-forwards the token to the worker or the correct pile when the spawn finally
  arrives — see the README's concurrency section.

The console distillation this visualises is
[`src/work_stealing_pool_core.cpp`](src/work_stealing_pool_core.cpp).
