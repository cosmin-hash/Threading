// Shapes.h
// Visual identity of a frame (shape kind + colour) and the low-level routine
// that paints one shape. Shared by the moving tokens and the accumulated piles.
#pragma once

#include <QColor>
#include <QMetaType>
#include <QPointF>

class QPainter;

// The kinds of shape a frame can be drawn as. Each frame picks one at random.
enum ShapeKind { Circle, Square, Rectangle, Star, Triangle, Hexagon, ShapeCount };

// A "frame" handed across the threading pipeline. Beyond the id (which the real
// model needs), it carries the shape + colour so the worker can echo its visual
// identity back to the GUI when it processes it.
struct Frame {
    int    id = -1;
    int    shape = Circle;
    QColor color;
};
Q_DECLARE_METATYPE(Frame)   // allow Frame to cross threads via queued signals

// Paint one shape centred at c with radius r, colour and alpha (0..1).
void paintShape(QPainter& p, int shape, const QPointF& c, double r,
                QColor color, double alpha);
