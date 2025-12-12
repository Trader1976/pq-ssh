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

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    qDebug() << "PQ-SSH starting (qDebug)";
    std::cout << "PQ-SSH starting (std::cout)" << std::endl;

    MainWindow w;
    w.show();

    return app.exec();
}