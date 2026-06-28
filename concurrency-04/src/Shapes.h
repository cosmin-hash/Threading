// Shapes.h
// Visual identity of a task (shape kind + colour) and the low-level routine that
// paints one shape. Shared by the moving tokens and the accumulated piles.
#pragma once

#include <QColor>
#include <QMetaType>
#include <QPointF>

class QPainter;

// The kinds of shape a task can be drawn as. Each task picks one at random.
enum ShapeKind { Circle, Square, Rectangle, Star, Triangle, Hexagon, ShapeCount };

// A "task" submitted to the work-stealing pool. Beyond the id (which the real
// scheduler needs), it carries the shape + colour so a worker can echo its visual
// identity back to the GUI when it runs it, plus the two facts that drive the
// stealing behaviour: how heavy the task is, and which worker's deque it was
// originally submitted to (round-robin).
struct Task {
    int    id = -1;
    int    shape = Circle;
    QColor color;
    bool   expensive = false;   // every 4th task is heavy -> creates the imbalance
    int    home = 0;            // worker whose deque it was submitted to (round-robin)
};
Q_DECLARE_METATYPE(Task)   // allow Task to cross threads via queued signals

// Paint one shape centred at c with radius r, colour and alpha (0..1).
void paintShape(QPainter& p, int shape, const QPointF& c, double r,
                QColor color, double alpha);
