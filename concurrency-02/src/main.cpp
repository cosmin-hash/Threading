// main.cpp
// Qt 6 visualisation of the multithreading model distilled in
// ../multi-producer-bounded-queue.cpp.
//
//   N producer threads  ->  per-pane bounded queues (drop-oldest)  ->  one
//   consumer thread takes the freshest frame per pane and skips the stale ones.
//
// The threading is genuine (see QuadPipeline); the worker threads emit Qt
// signals that the GUI thread turns into animated shapes so the behaviour is
// visible. Each frame is a random shape with its own colour; watchable delays
// keep it readable. Displayed frames accumulate in one pile, drop-oldest
// evictions in another, and bypassed stale frames in a third.
#include <QApplication>

#include "MainWindow.h"

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    MainWindow w;
    w.resize(1240, 820);
    w.show();
    return app.exec();
}
