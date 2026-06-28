// Token.h
// A visual token: one animated shape mirroring one frame as it travels through
// the pipeline (reader -> slot -> worker -> GUI display). Movement is driven by
// the GUI-thread animation timer; state transitions are driven by signals from
// the reader and worker threads.
#pragma once

#include <QColor>
#include <QPointF>

#include "Shapes.h"

// Pipeline anchor points (logical coordinates inside the canvas).
inline const QPointF kReaderPt(150, 290);     // input-source reader thread
inline const QPointF kSlotPt(450, 290);       // single hand-off slot
inline const QPointF kWorkerPt(740, 290);     // processing worker thread
inline const QPointF kGuiPt(1010, 290);       // GUI display (consumer)
inline const QPointF kDisplayPt(1010, 540);   // DISPLAYED pile centre
inline const QPointF kDropPt(450, 540);       // DROPPED pile centre
inline const QPointF kBouncePt(388, 290);     // where a dropped frame deflects
inline const double  kRadius = 24.0;

struct Token {
    enum State {
        ToSlot,      // reader -> slot
        InSlot,      // sitting in the slot, waiting for the worker
        ToWorker,    // slot -> worker
        Processing,  // pulsing in the worker while the heavy work runs
        ToGui,       // worker -> GUI display
        Displayed,   // landed in the displayed pile (reaped)
        Bounce,      // reader -> bounce point (slot was full)
        ToDrop,      // bounce point -> dropped pile
        Dropped      // landed in the dropped pile (reaped)
    };

    int     id = -1;
    int     shape = Circle;
    QColor  color;
    State   state = ToSlot;
    QPointF pos;        // current position
    QPointF from;       // current segment start
    QPointF to;         // current segment end
    double  t = 0.0;    // 0..1 progress along the current segment
    double  speed = 1.0;
    double  pulse = 0.0;
};
