// main.cpp
// Qt 6 visualisation of the producer/consumer threading model distilled in
// ../producer-consumer-cv.cpp.
//
//   PRODUCER (GUI thread)  ->  single-slot hand-off  ->  CONSUMER (worker thread)
//
// The threading is genuine (see FrameProcessor); the worker emits Qt signals
// that the GUI thread turns into animated shapes so the behaviour is visible.
// Each frame is a random shape with its own colour; a processing delay keeps it
// watchable. Processed frames accumulate in one pile, dropped frames in another.
#include <QApplication>

#include "MainWindow.h"

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    MainWindow w;
    w.resize(1240, 760);
    w.show();
    return app.exec();
}
