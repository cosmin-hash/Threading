// Token.h
// A visual token: one animated shape mirroring one task as it travels through the
// work-stealing scheduler (submit -> a worker's deque -> a worker runs it ->
// completed pile). Movement is driven by the GUI-thread animation timer; state
// transitions are driven by signals from the submitter and worker threads.
#pragma once

#include <QColor>
#include <QPointF>

#include "Shapes.h"

// ---- Topology (logical coordinates inside the canvas) ----------------------
//
//   SUBMIT ──► WORKER 0 deque ──► WORKER 0 ──► COMPLETED (own) pile
//   (round-    WORKER 1 deque ──► WORKER 1 ─┐
//    robin)    WORKER 2 deque ──► WORKER 2 ─┴► an idle worker STEALS from a busy
//              WORKER 3 deque ──► WORKER 3    one's deque ──► STOLEN pile
//
// Each worker owns a horizontal deque lane to its left. New tasks land at the
// BACK (right, nearest the worker); the owner pops the back (LIFO); a thief
// steals from the FRONT (left, far end).
inline constexpr int  kWorkers = 4;

inline const QPointF  kSubmitPt(120, 325);     // round-robin dispatcher
inline const double   kWorkerX  = 700.0;       // x of every worker station
inline const double   kWorkerY[kWorkers] = {115, 255, 395, 535};

inline const double   kDequeBackX = 600.0;     // BACK / owner end (nearest worker)
inline const double   kDequeGap   = 44.0;      // spacing between queued tasks
inline const double   kLaneLeftX  = 326.0;     // visual left edge of a deque lane

inline const QPointF  kOwnPilePt(1055, 175);   // COMPLETED (own) pile centre
inline const QPointF  kStolenPilePt(1055, 470);// STOLEN & DONE pile centre

inline const double   kRadius = 21.0;

inline QPointF kWorkerPt(int i) { return QPointF(kWorkerX, kWorkerY[i]); }

// Where the task at order-index `idx` sits in a deque holding `n` tasks. Index 0
// is the FRONT (oldest, steal end, far left); n-1 is the BACK (newest, owner end,
// pinned at kDequeBackX). Removing either end makes the rest slide toward the back.
inline QPointF kDequeSlot(int home, int idx, int n) {
    const double x = kDequeBackX - (n - 1 - idx) * kDequeGap;
    return QPointF(x, kWorkerY[home]);
}

struct Token {
    enum State {
        Queued,        // sitting in (homing toward) its slot in a worker's deque
        ToWorker,      // pulled out (own pop or steal) -> flying to the worker
        Working,       // pulsing at the worker while the task runs
        ToOwnPile,     // finished by its owner -> flying to the COMPLETED pile
        ToStolenPile,  // finished by a thief  -> flying to the STOLEN pile
        OwnDone,        // landed in the own pile (reaped)
        StolenDone      // landed in the stolen pile (reaped)
    };

    int     id = -1;
    int     shape = Circle;
    QColor  color;
    bool    expensive = false;  // heavy task (drawn a little larger, with a ring)
    int     home = 0;           // which worker's deque it was submitted to
    int     ranBy = -1;         // which worker actually ran it
    bool    stolen = false;     // ran by a worker other than its home -> stolen

    State   state = Queued;
    QPointF pos;                // current position
    QPointF from;               // current segment start
    QPointF to;                 // current segment end
    double  t = 0.0;            // 0..1 progress along the current segment
    double  speed = 1.0;
    double  pulse = 0.0;
};
