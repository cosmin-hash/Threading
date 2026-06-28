// main.cpp
// Qt 6 visualisation of the work-stealing thread-pool scheduler distilled in
// ../work_stealing_pool_core.cpp.
//
//   SUBMIT (round-robin)  ->  per-worker DEQUES  ->  N WORKER threads
//                                   ^                       |
//                                   └── idle workers STEAL ─┘
//
// The threading is genuine (see WorkStealingPool); the workers emit Qt signals
// that the GUI thread turns into animated shapes so the behaviour is visible.
// Each task is a random shape with its own colour; a watchable delay keeps it
// legible. Tasks a worker ran itself accumulate in one pile, tasks it stole from
// another worker in another.
#include <QApplication>

#include "MainWindow.h"

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    MainWindow w;
    w.resize(1240, 720);
    w.show();
    return app.exec();
}
