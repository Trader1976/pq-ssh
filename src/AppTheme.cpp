// AppTheme.cpp
#include "AppTheme.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QStringList>

QString AppTheme::dark()
{
    return QStringLiteral(
        // Base widgets (NO global font-size!)
        "QWidget {"
        "   background-color: #121212;"
        "   color: #DDDDDD;"
        "}"

        // UI font size for normal widgets ONLY
        "QLabel, QPushButton, QLineEdit, QPlainTextEdit, QTextEdit, "
        "QComboBox, QSpinBox, QListWidget, QCheckBox, "
        "QMenuBar, QMenu, QStatusBar, QTabWidget, QTabBar {"
        "   font-size: 14px;"
        "}"

        // Buttons
        "QPushButton {"
        "   background-color: #1E1E1E;"
        "   border: 1px solid #00FF99;"
        "   padding: 6px 10px;"
        "   border-radius: 4px;"
        "}"
        "QPushButton:hover {"
        "   background-color: #2A2A2A;"
        "}"
        "QPushButton:pressed {"
        "   background-color: #00FF99;"
        "   color: #000000;"
        "}"

        // Inputs
        "QLineEdit, QSpinBox, QComboBox, QTextEdit, QPlainTextEdit {"
        "   background-color: #1A1A1A;"
        "   border: 1px solid #333333;"
        "   padding: 4px;"
        "   border-radius: 3px;"
        "   color: #EEEEEE;"
        "}"
        // --------------------------------
        // SpinBox (dark + readable arrows)
        // --------------------------------
        "QSpinBox {"
        "   background-color: #1A1A1A;"
        "   border: 1px solid #3A3A3A;"
        "   padding-right: 18px;"          // room for buttons
        "}"

        "QSpinBox::up-button, QSpinBox::down-button {"
        "   background-color: #202020;"
        "   border-left: 1px solid #3A3A3A;"
        "   width: 18px;"
        "}"

        "QSpinBox::up-button:hover, QSpinBox::down-button:hover {"
        "   background-color: #262626;"
        "}"

        // Dark arrows (almost black, but visible)
        "QSpinBox::up-arrow {"
        "   image: none;"
        "   width: 0;"
        "   height: 0;"
        "   border-left: 5px solid transparent;"
        "   border-right: 5px solid transparent;"
        "   border-bottom: 7px solid #8A8A8A;"
        "}"

        "QSpinBox::down-arrow {"
        "   image: none;"
        "   width: 0;"
        "   height: 0;"
        "   border-left: 5px solid transparent;"
        "   border-right: 5px solid transparent;"
        "   border-top: 7px solid #8A8A8A;"
        "}"

        // Combo popup
        "QComboBox QAbstractItemView {"
        "   background-color: #1E1E1E;"
        "   selection-background-color: #00FF99;"
        "   selection-color: black;"
        "}"

        // Checkboxes
        "QCheckBox {"
        "   spacing: 6px;"
        "   color: #DDDDDD;"
        "}"
        "QCheckBox::indicator {"
        "   width: 18px;"
        "   height: 18px;"
        "}"
        "QCheckBox::indicator:unchecked {"
        "   border: 1px solid #00FF99;"
        "   background-color: #121212;"
        "}"
        "QCheckBox::indicator:checked {"
        "   background-color: #00FF99;"
        "   border: 1px solid #00FF99;"
        "}"

        // List widgets
        "QListWidget {"
        "   background-color: #181818;"
        "   border: 1px solid #333333;"
        "}"
        "QListWidget::item:selected {"
        "   background-color: #00FF99;"
        "   color: black;"
        "}"

        // Tabs
        "QTabWidget::pane {"
        "   border: 0px;"
        "}"
        "QTabBar::tab {"
        "   height: 14px;"
        "   padding: 4px 10px;"
        "   background: #0f1a14;"
        "   color: #b7f5d1;"
        "   border: 1px solid #1f3a2c;"
        "   border-bottom: none;"
        "}"
        "QTabBar::tab:selected {"
        "   background: #00FF99;"
        "   color: black;"
        "}"
        "QTabBar::tab:hover {"
        "   background: #159a66;"
        "   color: #eafff4;"
        "}"

        // Scrollbars
        "QScrollBar:vertical {"
        "   width: 12px;"
        "   background: #1A1A1A;"
        "}"
        "QScrollBar::handle:vertical {"
        "   background: #00FF99;"
        "   min-height: 30px;"
        "}"
        "QScrollBar::add-line, QScrollBar::sub-line {"
        "   height: 0;"
        "}"
    );
}

static void installBundledColorSchemes()
{
    const QString destDir =
        QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
        + "/qtermwidget5/color-schemes";

    QDir().mkpath(destDir);

    const QStringList schemeFiles = {
        "CPUNK-DNA.colorscheme",
        "CPUNK-Aurora.colorscheme",
    };

    for (const QString &name : schemeFiles) {
        const QString src = ":/schemes/" + name;
        const QString dst = destDir + "/" + name;

        // Don’t overwrite user-customized themes
        if (QFileInfo::exists(dst))
            continue;

        QFile in(src);
        if (!in.open(QIODevice::ReadOnly))
            continue;

        QFile out(dst);
        if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate))
            continue;

        out.write(in.readAll());
    }
}
