// Token.h
// A visual token: one animated shape mirroring one work item as it travels
// through the staged pipeline:
//
//   SOURCE -> [channel 1] -> TRANSFORM -> {pool worker} -> [channel 2] -> SINK
//
// Movement is driven by the GUI-thread animation timer; state transitions are
// driven by signals emitted from the source / transform / pool-worker / sink
// threads. All of this model's station and pile anchor points live here.
#pragma once

#include <QColor>
#include <QPointF>

#include "Shapes.h"

// Number of worker threads in the pool (matches PipelineEngine's pool size).
// Kept here too so the canvas can lay out one station per worker.
inline constexpr int kPoolSize = 4;

// ---- Pipeline anchor points (logical coordinates inside the canvas) --------
inline const QPointF kSourcePt   (95, 258);    // SOURCE thread (producer)
inline const QPointF kChan1Pt    (255, 258);   // bounded channel 1 (cap 4)
inline const QPointF kTransformPt(415, 258);   // TRANSFORM dispatcher thread
inline const QPointF kChan2Pt    (815, 258);   // bounded channel 2 (cap 4)
inline const QPointF kSinkPt     (975, 258);   // SINK thread (consumer)

inline const QPointF kConsumedPt (975, 515);   // CONSUMED pile centre (success)
inline const QPointF kThrottledPt(255, 515);   // THROTTLED pile centre (dimmed)

inline const double  kRadius        = 22.0;
inline const int     kChannelCap    = 4;        // capacity of each bounded channel
inline const double  kChanSlotGap   = 33.0;     // vertical spacing of channel slots
inline const double  kPoolWorkerGap = 90.0;     // vertical spacing of pool stations

// Centre of pool worker station i (0..kPoolSize-1), fanned out vertically about
// the main flow line at x = 615.
inline QPointF kPoolPt(int i) {
    const double cy = 258.0;
    const double top = cy - (kPoolSize - 1) * kPoolWorkerGap / 2.0;
    return QPointF(615.0, top + i * kPoolWorkerGap);
}

// Centre of channel slot i (0..kChannelCap-1) inside a channel box at centre c.
inline QPointF kChanSlotPt(const QPointF& c, int i) {
    const double top = c.y() - (kChannelCap - 1) * kChanSlotGap / 2.0;
    return QPointF(c.x(), top + i * kChanSlotGap);
}

struct Token {
    enum State {
        ToChan1,     // source -> channel 1
        InChan1,     // buffered in channel 1, waiting for the transform stage
        ToTransform, // channel 1 -> transform dispatcher
        AtTransform, // queued at the dispatcher, waiting for a free pool worker
        ToWorker,    // transform -> assigned pool worker
        Processing,  // pulsing in a pool worker while the heavy task runs
        ToChan2,     // pool worker -> channel 2
        InChan2,     // buffered in channel 2, waiting for the sink
        ToSink,      // channel 2 -> sink
        ToConsumed,  // sink -> consumed pile
        Consumed,    // landed in the consumed pile (reaped)
        ToThrottled, // a dim "backpressure stall" ghost falling to the throttled pile
        Throttled    // landed in the throttled pile (reaped)
    };

    int     id = -1;
    int     shape = Circle;
    QColor  color;
    State   state = ToChan1;
    int     worker = -1;    // which pool station this token is routed through
    QPointF pos;            // current position
    QPointF from;           // current segment start
    QPointF to;             // current segment end
    double  t = 0.0;        // 0..1 progress along the current segment
    double  speed = 1.0;
    double  pulse = 0.0;
    bool    ghost = false;  // true for the dim backpressure-stall marker
};
