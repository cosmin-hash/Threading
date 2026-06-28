// MainWindow.cpp
#include "MainWindow.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

#include "Canvas.h"

MainWindow::MainWindow() {
    setWindowTitle("Threading Model Visualiser - producer/consumer hand-off");

    m_canvas   = new Canvas;
    m_startBtn = new QPushButton("Start");
    m_stopBtn  = new QPushButton("Stop");
    m_stopBtn->setEnabled(false);

    m_startBtn->setMinimumHeight(34);
    m_stopBtn->setMinimumHeight(34);
    m_startBtn->setStyleSheet("font-weight:bold;");
    m_stopBtn->setStyleSheet("font-weight:bold;");

    m_stats = new QLabel("Idle. Press Start.");
    m_stats->setStyleSheet("color:#dfe3ee; font-size:12px;");

    auto* bar = new QHBoxLayout;
    bar->addWidget(m_startBtn);
    bar->addWidget(m_stopBtn);
    bar->addSpacing(20);
    bar->addWidget(m_stats, 1);

    auto* root = new QVBoxLayout(this);
    root->addWidget(m_canvas, 1);
    root->addLayout(bar);

    connect(m_startBtn, &QPushButton::clicked, this, [this] {
        m_canvas->start();
        m_startBtn->setEnabled(false);
        m_stopBtn->setEnabled(true);
    });
    connect(m_stopBtn, &QPushButton::clicked, this, [this] {
        m_canvas->stop();
        m_startBtn->setEnabled(true);
        m_stopBtn->setEnabled(false);
    });
    connect(m_canvas, &Canvas::statsChanged, this, &MainWindow::onStats);
}

void MainWindow::onStats(int read, int dropped, int displayed,
                         const QString& status) {
    m_stats->setText(
        QString("Read: %1    Dropped: %2    Displayed: %3      [%4]")
            .arg(read).arg(dropped).arg(displayed).arg(status));
}
