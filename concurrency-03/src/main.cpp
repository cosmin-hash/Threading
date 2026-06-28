// main.cpp
// Qt 6 visualisation of the staged-pipeline + thread-pool threading model
// distilled in ../staged-pipeline-threadpool.cpp.
//
//   SOURCE -> [bounded channel] -> TRANSFORM -> {thread pool} -> [bounded channel] -> SINK
//
// The threading is genuine (see PipelineEngine, which composes a BoundedChannel
// and a ThreadPool); the real threads emit Qt signals that the GUI thread turns
// into animated shapes so the behaviour is visible. Each work item is a random
// shape with its own colour; a watchable delay keeps it legible. Consumed items
// accumulate in one pile; backpressure stalls in another.
#include <QApplication>

#include "MainWindow.h"

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    MainWindow w;
    w.resize(1240, 780);
    w.show();
    return app.exec();
}
