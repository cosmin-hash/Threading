// Shapes.h
// Visual identity of a work item (shape kind + colour) and the low-level routine
// that paints one shape. Shared by the moving tokens and the accumulated piles.
#pragma once

#include <QColor>
#include <QMetaType>
#include <QPointF>
#include <QString>

class QPainter;

// The kinds of shape a work item can be drawn as. Each item picks one at random.
enum ShapeKind { Circle, Square, Rectangle, Star, Triangle, Hexagon, ShapeCount };

// A "work item" handed across the staged pipeline. Beyond the id and the text
// payload (which the real model carries), it holds a shape + colour so each stage
// can echo the item's visual identity back to the GUI as it flows through.
struct WorkItem {
    int     id = -1;
    QString text;       // the payload the pipeline actually transports
    int     shape = Circle;
    QColor  color;
};
Q_DECLARE_METATYPE(WorkItem)   // allow WorkItem to cross threads via queued signals

// Paint one shape centred at c with radius r, colour and alpha (0..1).
void paintShape(QPainter& p, int shape, const QPointF& c, double r,
                QColor color, double alpha);
