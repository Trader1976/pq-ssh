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
#include "AuditLogger.h"
#include <QUuid>
#include <QTranslator>
#include <QLocale>
#include <QFile>



static bool installAppTranslator(QApplication& app, const QString& langCode)
{
    // Expect qm inside resources:
    //   :/i18n/pqssh_en.qm
    //   :/i18n/pqssh_es.qm
    //   :/i18n/pqssh_fi.qm
    //
    // If you prefer external files, change path accordingly.

    QTranslator* tr = new QTranslator(&app); // app owns it
    const QString qmPath = QString(":/i18n/pqssh_%1.qm").arg(langCode);

    if (!tr->load(qmPath)) {
        delete tr;
        return false;
    }
    qDebug() << "langCode=" << langCode << "qm=" << qmPath << "loaded=" << tr->isEmpty();
    app.installTranslator(tr);
    return true;
}


int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    QCoreApplication::setOrganizationName("CPUNK");
    QCoreApplication::setApplicationName("pq-ssh");                 // stable key for QSettings
    QGuiApplication::setApplicationDisplayName("CPUNK PQ-SSH");
    QCoreApplication::setApplicationVersion("0.9.0-alpha");

    Logger::install("pq-ssh");
    AuditLogger::install("pq-ssh");
    AuditLogger::setSessionId(QUuid::createUuid().toString(QUuid::WithoutBraces));
    AuditLogger::writeEvent("session.start");

    QSettings s;

    // --- Translator (install BEFORE creating MainWindow) ---
    const QString lang = s.value("ui/language", "en").toString().trimmed();

    qInfo() << "QM exists fi:" << QFile(":/i18n/pqssh_fi.qm").exists();
    qInfo() << "QM exists en:" << QFile(":/i18n/pqssh_en.qm").exists();
    qInfo() << "QM exists en:" << QFile(":/i18n/pqssh_es.qm").exists();

    if (!lang.isEmpty() && lang != "en") {
        const bool ok = installAppTranslator(app, lang);
        qInfo() << "Translator install" << (ok ? "OK" : "FAILED") << "lang=" << lang;
    }

    // Theme
    const QString themeId = s.value("ui/theme", "cpunk-dark").toString();
    if (themeId == "cpunk-orange") qApp->setStyleSheet(AppTheme::orange());
    else if (themeId == "windows-basic") qApp->setStyleSheet(AppTheme::windowsBasic());
    else qApp->setStyleSheet(AppTheme::dark());

    installBundledColorSchemes();

    MainWindow w;
    w.show();
    return app.exec();
}

