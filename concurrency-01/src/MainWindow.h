// MainWindow.h
// Top-level window: the canvas plus Start/Stop buttons and a live stats line.
#pragma once

#include <QWidget>

class Canvas;
class QLabel;
class QPushButton;

class MainWindow : public QWidget {
    Q_OBJECT
public:
    MainWindow();

private slots:
    void onStats(int produced, int dropped, int processed, const QString& status);

private:
    Canvas*      m_canvas;
    QPushButton* m_startBtn;
    QPushButton* m_stopBtn;
    QLabel*      m_stats;
};
