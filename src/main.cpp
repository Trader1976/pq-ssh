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

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    installBundledColorSchemes();
    // Global dark palette
    QPalette dark;
    dark.setColor(QPalette::Window, QColor(0,0,0));
    dark.setColor(QPalette::Base, QColor(0,0,0));
    dark.setColor(QPalette::Text, QColor(255,255,255));
    dark.setColor(QPalette::Button, QColor(20,20,20));
    dark.setColor(QPalette::ButtonText, QColor(255,255,255));

    app.setPalette(dark);

    qDebug() << "PQ-SSH starting (qDebug)";
    std::cout << "PQ-SSH starting (std::cout)" << std::endl;

    MainWindow w;
    w.show();

    return app.exec();
}