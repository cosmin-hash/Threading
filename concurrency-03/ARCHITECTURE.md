# Architecture

`PipelineViz` (concurrency-03) visually animates a **3-stage pipeline built from two reusable
concurrency primitives** — a bounded MPMC channel (a blocking queue with backpressure) and a
`std::future`-returning thread pool. The design goal: a source is throttled by genuine
backpressure (never dropped, never unbounded), heavy work fans out across a worker pool, and
shutdown is deterministic and stage-ordered.

![Concurrency architecture](docs/screenshot.png)

## Threading model

```
  SOURCE ──►[ channel 1 ]──► TRANSFORM ──►┌─ POOL #0 ─┐──►[ channel 2 ]──► SINK
 (producer)  cap 4, blocks   (dispatcher)  │  POOL #1  │   cap 4, blocks   (consumer)
              when full        fan-out ───► │  POOL #2  │ ───► fan-in        consumes
                                            └─ POOL #3 ─┘
```

| Object | Thread | Responsibility |
| --- | --- | --- |
| source loop | one `std::thread` | emit `WorkItem`s into channel 1; **block** when it is full (backpressure) |
| `BoundedChannel<T>` | shared (`std::mutex` + two `std::condition_variable`s) | *not-full* / *not-empty* gates; `close()` wakes everyone for clean end-of-stream |
| transform loop | one `std::thread` (dispatcher) | pop channel 1, `submit()` to the pool, drain futures in order into channel 2 |
| `ThreadPool` | 4 worker `std::thread`s | run `std::packaged_task`s, return `std::future<R>`; idle workers sleep on a CV |
| sink loop | one `std::thread` | pop finished items from channel 2 until the stream ends |
| [`Canvas`](src/Canvas.cpp) | GUI thread | animate tokens, paint stages/piles — consumer only |

The whole backbone ([`PipelineEngine.h`](src/PipelineEngine.h) /
[`PipelineEngine.cpp`](src/PipelineEngine.cpp)) — including `BoundedChannel<T>` and `ThreadPool`
— is plain C++ and knows nothing about Qt. It only knows about `WorkItem` and emits Qt signals
delivered with `Qt::QueuedConnection`.

## Data flow for one item

```
source: push(item) into channel 1        ── BLOCKS if full → THROTTLED marker
transform: pop channel 1 → pool.submit() → std::future
  → drain futures in order → push into channel 2
sink: pop channel 2 (finished item)
  → emit itemConsumed()                  ── queued signal → GUI thread
GUI thread: animate token → CONSUMED pile
```

## Outcomes

| Outcome | Where it happens | Pile |
| --- | --- | --- |
| **Consumed** | flowed all the way through and was pulled by the sink | `CONSUMED` (full alpha) |
| **Throttled** | the source's `push()` blocked on a full channel (backpressure) | `THROTTLED` (dimmed) |

> Unlike a drop-queue, this model **never discards** an item. A throttle stall is not a lost
> item — the same item still flows through to `CONSUMED`; the dim `THROTTLED` marker just
> records that backpressure happened.

## Correctness notes

* **Cooperative, stage-ordered shutdown**: set the stop flag, `close()` channel 1 (waking the
  blocked source and transform), `join()` source → transform → sink in flow order, then shut
  the **pool down last** so its in-flight tasks finish — no thread is killed mid-work.

The console distillation this visualises is
[`src/staged-pipeline-threadpool.cpp`](src/staged-pipeline-threadpool.cpp).
