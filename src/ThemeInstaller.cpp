// src/ThemeInstaller.cpp
#include "ThemeInstaller.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QStringList>
#include <QDebug>

/**
 * pickSchemeDestDir()
 *
 * Purpose:
 *  Decide where bundled QTermWidget color scheme files should be installed.
 *
 * Background:
 *  - QTermWidget loads color schemes from specific directories.
 *  - On many Linux distributions, it only reads system-wide locations:
 *      /usr/share/qtermwidget5/color-schemes
 *  - Writing there usually requires root privileges.
 *
 * Strategy:
 *  1) If the system directory exists AND is writable, use it.
 *     (Typical when running from a distro package or with elevated rights.)
 *  2) Otherwise, fall back to the per-user data directory:
 *      ~/.local/share/qtermwidget5/color-schemes
 *
 * This allows:
 *  - No-sudo installs for developers and users
 *  - System-wide installs when possible
 */
static QString pickSchemeDestDir()
{
    const QString systemDir = "/usr/share/qtermwidget5/color-schemes";
    const QFileInfo sysInfo(systemDir);

    // Prefer system directory if it exists and is writable
    if (sysInfo.exists() && sysInfo.isDir() && sysInfo.isWritable()) {
        return systemDir;
    }

    // Fallback: per-user directory (no sudo required)
    const QString base =
        QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
        // Typically resolves to ~/.local/share

    return base + "/qtermwidget5/color-schemes";
}

/**
 * installBundledColorSchemes()
 *
 * Purpose:
 *  Installs CPUNK-provided terminal color schemes so that QTermWidget
 *  can discover and use them.
 *
 * What this does:
 *  - Copies .colorscheme files embedded in Qt resources (.qrc)
 *  - Writes them to a directory that QTermWidget scans
 *  - Does NOT overwrite existing files
 *
 * When this should be called:
 *  - Once during application startup
 *  - Before opening terminal widgets
 *
 * Important:
 *  - This is NOT a theme selector
 *  - This is NOT a UI feature
 *  - It is an installer / bootstrap helper
 */
void installBundledColorSchemes()
{
    const QString destDir = pickSchemeDestDir();
    if (destDir.isEmpty())
        return;

    // Ensure destination directory exists
    QDir().mkpath(destDir);

    /**
     * List of bundled scheme files.
     *
     * These MUST match the Qt resource paths defined in your .qrc file.
     *
     * Example .qrc:
     *   <qresource prefix="/schemes">
     *     <file>color-schemes/CPUNK-DNA.colorscheme</file>
     *   </qresource>
     *
     * Resource access path becomes:
     *   :/schemes/color-schemes/CPUNK-DNA.colorscheme
     */
    const QStringList schemeFiles = {
        "CPUNK-DNA.colorscheme",
        "CPUNK-Aurora.colorscheme",
    };

    for (const QString &name : schemeFiles) {
        const QString src = ":/schemes/color-schemes/" + name;
        const QString dst = destDir + "/" + name;

        // Do NOT overwrite existing schemes
        // (user might have edited or replaced them)
        if (QFileInfo::exists(dst)) {
            continue;
        }

        // Resource missing → build or qrc problem
        if (!QFileInfo::exists(src)) {
            qWarning() << "ThemeInstaller: resource missing:" << src;
            continue;
        }

        // Open embedded resource
        QFile in(src);
        if (!in.open(QIODevice::ReadOnly)) {
            qWarning() << "ThemeInstaller: failed to open resource:"
                       << src << in.errorString();
            continue;
        }

        // Write scheme to destination directory
        QFile out(dst);
        if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            qWarning() << "ThemeInstaller: failed to write:"
                       << dst << out.errorString()
                       << "(If you want system-wide install, run with proper permissions.)";
            continue;
        }

        out.write(in.readAll());
        out.close();

        qDebug() << "ThemeInstaller: installed" << name << "->" << dst;
    }
}
