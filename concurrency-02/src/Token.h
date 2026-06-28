// Token.h
// A visual token: one animated shape mirroring one frame as it travels through
// the pipeline:  producer -> per-pane bounded queue -> consumer -> a pile.
// Movement is driven by the GUI-thread animation timer; state transitions are
// driven by signals from the producer and consumer threads.
#pragma once

#include <QColor>
#include <QPointF>

#include "Shapes.h"

// ---------------------------------------------------------------------------
// Pipeline topology (logical coordinates inside the canvas).
//
//   producer[0] -- queue[0] (3 slots) --\
//   producer[1] -- queue[1] (3 slots) ---\
//   producer[2] -- queue[2] (3 slots) ----> CONSUMER -> DISPLAYED pile
//   producer[3] -- queue[3] (3 slots) ---/             (drop/skip -> piles)
// ---------------------------------------------------------------------------
inline constexpr int kPaneCount = 4;     // N independent producers
inline constexpr int kCapacity  = 3;     // bounded-queue depth per pane
inline constexpr double kRadius  = 20.0; // travelling token radius
inline constexpr double kSlotGap = 60.0; // spacing between queue slots

// Vertical row centre for a given pane.
inline double paneY(int pane) { return 112.0 + pane * 122.0; }

// The producer thread station for a pane (left edge).
inline QPointF producerPt(int pane) { return QPointF(110.0, paneY(pane)); }

// One of the 3 bounded-queue slots for a pane (slot 0 = oldest / front).
inline QPointF queueSlotPt(int pane, int slot) {
    return QPointF(330.0 + slot * kSlotGap, paneY(pane));
}

// The single consumer (render thread) station, centred over the four rows.
inline const QPointF kConsumerPt(780.0, 295.0);

// Outcome piles.
inline const QPointF kDisplayPilePt(1000.0, 625.0);  // freshest frame shown
inline const QPointF kDropPilePt(360.0, 625.0);      // drop-oldest backpressure
inline const QPointF kSkipPilePt(640.0, 625.0);      // stale frame bypassed

struct Token {
    enum State {
        ToQueue,        // producer -> its queue slot (also used to slide slots)
        InQueue,        // parked in a queue slot, waiting for the consumer
        ToConsumer,     // queue -> consumer (this frame was the freshest)
        AtConsumer,     // pulsing at the consumer while it "renders" the frame
        ToDisplayed,    // consumer -> DISPLAYED pile
        Displayed,      // landed in the displayed pile (reaped)
        ToDrop,         // queue -> DROPPED pile (drop-oldest backpressure)
        DroppedLanded,  // landed in the dropped pile (reaped)
        ToSkip,         // queue -> SKIPPED pile (stale, bypassed by consumer)
        SkippedLanded   // landed in the skipped pile (reaped)
    };

    int     id = -1;
    int     pane = -1;
    int     shape = Circle;
    QColor  color;
    State   state = ToQueue;
    int     slot = 0;       // queue slot this token currently targets / occupies
    QPointF pos;            // current position
    QPointF from;           // current segment start
    QPointF to;             // current segment end
    double  t = 0.0;        // 0..1 progress along the current segment
    double  speed = 1.0;
    double  pulse = 0.0;
};
