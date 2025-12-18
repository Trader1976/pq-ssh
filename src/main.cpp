/*
 * PQ-SSH — Post-Quantum Secure Shell
 *
 * Copyright (c) 2025 Timo Erkvaara / CPUNK
 *
 * Licensed under the Apache License, Version 2.0
 * http://www.apache.org/licenses/LICENSE-2.0
 */

#include <QApplication>
#include <QDebug>
#include <iostream>

#include "MainWindow.h"
#include "ThemeInstaller.h"
#include "Logger.h"

#include <QMutex>
#include <QMutexLocker>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    // Install Qt → file logger
    Logger::install("pq-ssh");

    installBundledColorSchemes();

    QPalette dark;
    dark.setColor(QPalette::Window, QColor(0,0,0));
    dark.setColor(QPalette::Base, QColor(0,0,0));
    dark.setColor(QPalette::Text, QColor(255,255,255));
    dark.setColor(QPalette::Button, QColor(20,20,20));
    dark.setColor(QPalette::ButtonText, QColor(255,255,255));
    app.setPalette(dark);

    qInfo() << "PQ-SSH starting";

    MainWindow w;
    w.show();

    return app.exec();
}
