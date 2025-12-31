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
#include <QGuiApplication>
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

// main.cpp
// --------
// Application entry point.
//
// Responsibilities:
// - Create QApplication
// - Set QCoreApplication metadata (org/app name/version) for QSettings and UI
// - Install logging + audit logging (session id, session start event)
// - Load persisted settings (language + theme) and apply them BEFORE MainWindow
// - Install bundled terminal color schemes
// - Create and show the MainWindow, then enter the Qt event loop
//
// Notes:
// - Translator must be installed before constructing UI widgets so tr() strings
//   resolve using the selected language.
// - Theme is applied via a global style sheet. Terminal color schemes are separate.

// Install the application translator for the selected language code.
// Returns true if a qm was loaded and installed, false otherwise.
//
// Expected qm paths in resources:
//   :/i18n/pqssh_en.qm
//   :/i18n/pqssh_es.qm
//   :/i18n/pqssh_fi.qm
//   :/i18n/pqssh_zh.qm   (if/when present)
//
// If you later want external qm files (next to executable), change qmPath accordingly.
static bool installAppTranslator(QApplication& app, const QString& langCode)
{
    QTranslator* tr = new QTranslator(&app); // app owns it
    const QString qmPath = QString(":/i18n/pqssh_%1.qm").arg(langCode);

    // If the qm is not found / cannot be loaded, do not keep the translator object.
    if (!tr->load(qmPath)) {
        delete tr;
        return false;
    }

    // Note: isEmpty() indicates whether the translator has any translations loaded.
    // It's normal for this to be false for a successful load.
    qDebug() << "langCode=" << langCode << "qm=" << qmPath << "loaded=" << !tr->isEmpty();

    app.installTranslator(tr);
    return true;
}

// Program entry point.
// Sets up app metadata, logging, translator/theme, then creates and runs MainWindow.
int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    // Application identity (affects QSettings storage keys and QStandardPaths layout)
    //
    // IMPORTANT:
    // - Use a stable "applicationName" for filesystem/config keys (no spaces).
    // - Use "applicationDisplayName" for UI only (can contain spaces/branding).
    //
    // This ensures QStandardPaths resolves to a consistent per-user folder, e.g.:
    //   ~/.config/CPUNK/pq-ssh/
    //   ~/.local/share/CPUNK/pq-ssh/
    QCoreApplication::setOrganizationName("CPUNK");
    QCoreApplication::setApplicationName("pq-ssh");                 // stable key for QSettings + paths
    QGuiApplication::setApplicationDisplayName("CPUNK PQ-SSH");     // UI only
    QCoreApplication::setApplicationVersion("0.9.0-alpha");

    // Logging + audit: install sinks and begin a session with a unique id.
    Logger::install("pq-ssh");
    AuditLogger::install("pq-ssh");
    AuditLogger::setSessionId(QUuid::createUuid().toString(QUuid::WithoutBraces));
    AuditLogger::writeEvent("session.start");

    QSettings s;

    // --- Translator (install BEFORE creating MainWindow) ---
    // Language codes are stored as short values like "en", "fi", "es", "zh".
    // Default is English (no translator needed).
    const QString lang = s.value("ui/language", "en").toString().trimmed();

    // Debug visibility: confirm bundled qm existence in resources.
    qInfo() << "QM exists fi:" << QFile(":/i18n/pqssh_fi.qm").exists();
    qInfo() << "QM exists en:" << QFile(":/i18n/pqssh_en.qm").exists();
    qInfo() << "QM exists es:" << QFile(":/i18n/pqssh_es.qm").exists();
    qInfo() << "QM exists zh:" << QFile(":/i18n/pqssh_zh.qm").exists();

    // Only install translator for non-English; "en" typically uses source strings.
    if (!lang.isEmpty() && lang != "en") {
        const bool ok = installAppTranslator(app, lang);
        qInfo() << "Translator install" << (ok ? "OK" : "FAILED") << "lang=" << lang;
    }

    // --- Theme ---
    // Apply global stylesheet early so the UI paints consistently from first show().
    const QString themeId = s.value("ui/theme", "cpunk-dark").toString();
    if (themeId == "cpunk-orange") qApp->setStyleSheet(AppTheme::orange());
    else if (themeId == "windows-basic") qApp->setStyleSheet(AppTheme::windowsBasic());
    else qApp->setStyleSheet(AppTheme::dark());

    // Install bundled terminal color schemes (qtermwidget schemes shipped with app).
    installBundledColorSchemes();

    // Create main UI window and enter the event loop.
    MainWindow w;
    w.show();
    return app.exec();
}
