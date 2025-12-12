// src/ThemeInstaller.cpp
#include "ThemeInstaller.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QStringList>
#include <QDebug>

// Pick a destination directory that QTermWidget will actually read.
// On many distros QTermWidget reads only the system dir (/usr/share/...).
// We'll try system dir if it exists and is writable, otherwise fall back to user dir.
static QString pickSchemeDestDir()
{
    const QString systemDir = "/usr/share/qtermwidget5/color-schemes";
    const QFileInfo sysInfo(systemDir);

    if (sysInfo.exists() && sysInfo.isDir() && sysInfo.isWritable()) {
        return systemDir;
    }

    // Fallback: per-user dir (no sudo needed)
    const QString base =
        QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation); // ~/.local/share
    return base + "/qtermwidget5/color-schemes";
}

void installBundledColorSchemes()
{
    const QString destDir = pickSchemeDestDir();
    if (destDir.isEmpty())
        return;

    QDir().mkpath(destDir);

    // These must match your .qrc entries. If your qrc uses:
    // <qresource prefix="/schemes"><file>color-schemes/CPUNK-DNA.colorscheme</file>...</qresource>
    // then the actual resource path is:
    // :/schemes/color-schemes/CPUNK-DNA.colorscheme
    const QStringList schemeFiles = {
        "CPUNK-DNA.colorscheme",
        "CPUNK-Aurora.colorscheme",
    };

    for (const QString &name : schemeFiles) {
        const QString src = ":/schemes/color-schemes/" + name;
        const QString dst = destDir + "/" + name;

        // Don’t overwrite existing files (user may have customized them)
        if (QFileInfo::exists(dst)) {
            continue;
        }

        if (!QFileInfo::exists(src)) {
            qWarning() << "ThemeInstaller: resource missing:" << src;
            continue;
        }

        QFile in(src);
        if (!in.open(QIODevice::ReadOnly)) {
            qWarning() << "ThemeInstaller: failed to open resource:" << src << in.errorString();
            continue;
        }

        QFile out(dst);
        if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            qWarning() << "ThemeInstaller: failed to write:" << dst << out.errorString()
                       << "(If you want system-wide install, run the app from an installed package or with permissions.)";
            continue;
        }

        out.write(in.readAll());
        out.close();
        qDebug() << "ThemeInstaller: installed" << name << "->" << dst;
    }
}
