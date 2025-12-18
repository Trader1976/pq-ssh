/*
* PQ-SSH — Post-Quantum Secure Shell
 *
 * Copyright (c) 2025 Timo Erkvaara / CPUNK
 *
 * Licensed under the Apache License, Version 2.0
 * http://www.apache.org/licenses/LICENSE-2.0
 */

#include <QApplication>
#include <QCoreApplication>
#include <QDebug>
#include <iostream>

#include "MainWindow.h"
#include "ThemeInstaller.h"
#include "Logger.h"
#include "AppTheme.h"

#include <QSettings>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    // (Recommended) Set org/app for QSettings so it uses stable location + keys
    // If you already set these elsewhere, keep them consistent.
    QCoreApplication::setOrganizationName("CPUNK");
    QCoreApplication::setApplicationName("pq-ssh");

    // Install Qt → file logger first
    Logger::install("pq-ssh");

    // Load saved settings early
    QSettings s;
    Logger::setLogLevel(s.value("logging/level", 1).toInt());

    // Install bundled terminal color schemes (qtermwidget)
    installBundledColorSchemes();

    // Apply saved app theme
    const QString themeId = s.value("ui/theme", "cpunk-dark").toString();

    if (themeId == "cpunk-orange") {
        qApp->setStyleSheet(AppTheme::orange());
    } else if (themeId == "windows-basic") {
        qApp->setStyleSheet(AppTheme::windowsBasic());
    } else {
        qApp->setStyleSheet(AppTheme::dark());
    }


    qInfo() << "PQ-SSH starting (theme=" << themeId << ")";

    MainWindow w;
    w.show();

    return app.exec();
}
