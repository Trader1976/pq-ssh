// AppTheme.cpp
#include "AppTheme.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QStringList>
#include <QColor>

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

        // Inputs (base look)
        "QLineEdit, QComboBox, QTextEdit, QPlainTextEdit {"
        "   background-color: #1A1A1A;"
        "   border: 1px solid #333333;"
        "   padding: 4px;"
        "   border-radius: 3px;"
        "   color: #EEEEEE;"
        "}"

        // --------------------------------
        // SpinBox (FIX: do NOT style subcontrols, avoids missing top border)
        // --------------------------------
        "QSpinBox {"
        "   background-color: #1A1A1A;"
        "   border: 1px solid #3A3A3A;"
        "   padding: 4px;"
        "   padding-right: 22px;"   // room for native buttons
        "   border-radius: 3px;"
        "   color: #EEEEEE;"
        "}"

        // Make item-view check indicators visible in lists (QListWidget etc.)
        "QAbstractItemView::indicator { width: 18px; height: 18px; }"
        "QAbstractItemView::indicator:unchecked {"
        "   border: 1px solid #00FF99;"
        "   background-color: #121212;"
        "}"
        "QAbstractItemView::indicator:checked {"
        "   background-color: #00FF99;"
        "   border: 1px solid #00FF99;"
        "}"
        "QAbstractItemView::indicator:unchecked:hover {"
        "   border: 1px solid #7cffc8;"
        "   background-color: #1a1a1a;"
        "}"
        "QAbstractItemView::indicator:checked:hover {"
        "   background-color: #7cffc8;"
        "   border: 1px solid #7cffc8;"
        "}"
        "QAbstractItemView::indicator:disabled {"
        "   border: 1px solid #3a3a3a;"
        "   background-color: #161616;"
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

        // Item views (Trees/Tables) — used by Files tab
        "QTreeView, QTableView, QTableWidget {"
        "   background-color: #121212;"
        "   alternate-background-color: #1A1A1A;"
        "   border: 1px solid #2A2A2A;"
        "   gridline-color: #2A2A2A;"
        "   selection-background-color: #00CC77;"  // calmer than full neon
        "   selection-color: #000000;"
        "   outline: 0;"
        "}"
        "QTreeView::item, QTableView::item, QTableWidget::item {"
        "   padding: 2px 6px;"
        "}"
        "QTreeView::item:selected, QTableView::item:selected, QTableWidget::item:selected {"
        "   background-color: #00CC77;"
        "   color: #000000;"
        "}"

        // Headers (remote table header etc.)
        "QHeaderView::section {"
        "   background-color: #1E1E1E;"
        "   color: #DDDDDD;"
        "   padding: 4px 8px;"
        "   border: 0;"
        "   border-right: 1px solid #2A2A2A;"
        "}"

        // Checkboxes (improve contrast for hover/disabled/focus)
        "QCheckBox::indicator:unchecked:hover {"
        "   border: 1px solid #7cffc8;"
        "   background-color: #1a1a1a;"
        "}"
        "QCheckBox::indicator:checked:hover {"
        "   background-color: #7cffc8;"
        "   border: 1px solid #7cffc8;"
        "}"
        "QCheckBox::indicator:disabled {"
        "   border: 1px solid #3a3a3a;"
        "   background-color: #161616;"
        "}"
        "QCheckBox:disabled {"
        "   color: #777777;"
        "}"
        "QCheckBox:focus {"
        "   outline: none;"
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

        // Scrollbars (less neon, CPUNK accent on hover)
        "QScrollBar:vertical {"
        "   width: 12px;"
        "   background: #121212;"
        "   margin: 0;"
        "}"
        "QScrollBar::handle:vertical {"
        "   background: #2A2A2A;"
        "   min-height: 30px;"
        "   border-radius: 6px;"
        "}"
        "QScrollBar::handle:vertical:hover {"
        "   background: #3A3A3A;"
        "}"
        "QScrollBar::handle:vertical:pressed {"
        "   background: #00FF99;"
        "}"
        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {"
        "   height: 0;"
        "}"
        "QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical {"
        "   background: #121212;"
        "}"

        "QScrollBar:horizontal {"
        "   height: 12px;"
        "   background: #121212;"
        "   margin: 0;"
        "}"
        "QScrollBar::handle:horizontal {"
        "   background: #2A2A2A;"
        "   min-width: 30px;"
        "   border-radius: 6px;"
        "}"
        "QScrollBar::handle:horizontal:hover {"
        "   background: #3A3A3A;"
        "}"
        "QScrollBar::handle:horizontal:pressed {"
        "   background: #00FF99;"
        "}"
        "QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal {"
        "   width: 0;"
        "}"
        "QScrollBar::add-page:horizontal, QScrollBar::sub-page:horizontal {"
        "   background: #121212;"
        "}"
    );
}

QString AppTheme::orange()
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
        "   border: 1px solid #FF9800;"
        "   padding: 6px 10px;"
        "   border-radius: 4px;"
        "}"
        "QPushButton:hover {"
        "   background-color: #2A2A2A;"
        "}"
        "QPushButton:pressed {"
        "   background-color: #FF9800;"
        "   color: #000000;"
        "}"

        // Inputs (base look)
        "QLineEdit, QComboBox, QTextEdit, QPlainTextEdit {"
        "   background-color: #1A1A1A;"
        "   border: 1px solid #333333;"
        "   padding: 4px;"
        "   border-radius: 3px;"
        "   color: #EEEEEE;"
        "}"

        // Item-view indicators (lists)
        "QAbstractItemView::indicator { width: 18px; height: 18px; }"
        "QAbstractItemView::indicator:unchecked {"
        "   border: 1px solid #FF9800;"
        "   background-color: #121212;"
        "}"
        "QAbstractItemView::indicator:checked {"
        "   background-color: #FF9800;"
        "   border: 1px solid #FF9800;"
        "}"
        "QAbstractItemView::indicator:unchecked:hover {"
        "   border: 1px solid #ffbf66;"
        "   background-color: #1a1a1a;"
        "}"
        "QAbstractItemView::indicator:checked:hover {"
        "   background-color: #ffbf66;"
        "   border: 1px solid #ffbf66;"
        "}"
        "QAbstractItemView::indicator:disabled {"
        "   border: 1px solid #3a3a3a;"
        "   background-color: #161616;"
        "}"

        // --------------------------------
        // SpinBox (FIX: do NOT style subcontrols, avoids missing top border)
        // --------------------------------
        "QSpinBox {"
        "   background-color: #1A1A1A;"
        "   border: 1px solid #3A3A3A;"
        "   padding: 4px;"
        "   padding-right: 22px;"
        "   border-radius: 3px;"
        "   color: #EEEEEE;"
        "}"

        // Combo popup
        "QComboBox QAbstractItemView {"
        "   background-color: #1E1E1E;"
        "   selection-background-color: #FF9800;"
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
        "   border: 1px solid #FF9800;"
        "   background-color: #121212;"
        "}"
        "QCheckBox::indicator:checked {"
        "   background-color: #FF9800;"
        "   border: 1px solid #FF9800;"
        "}"

        // List widgets
        "QListWidget {"
        "   background-color: #181818;"
        "   border: 1px solid #333333;"
        "}"
        "QListWidget::item:selected {"
        "   background-color: #FF9800;"
        "   color: black;"
        "}"

        // Item views (Trees/Tables) — used by Files tab
        "QTreeView, QTableView, QTableWidget {"
        "   background-color: #121212;"
        "   alternate-background-color: #1A1A1A;"
        "   border: 1px solid #2A2A2A;"
        "   gridline-color: #2A2A2A;"
        "   selection-background-color: #E08600;"  // calmer than full orange
        "   selection-color: #000000;"
        "   outline: 0;"
        "}"
        "QTreeView::item, QTableView::item, QTableWidget::item {"
        "   padding: 2px 6px;"
        "}"
        "QTreeView::item:selected, QTableView::item:selected, QTableWidget::item:selected {"
        "   background-color: #E08600;"
        "   color: #000000;"
        "}"

        // Headers
        "QHeaderView::section {"
        "   background-color: #1E1E1E;"
        "   color: #DDDDDD;"
        "   padding: 4px 8px;"
        "   border: 0;"
        "   border-right: 1px solid #2A2A2A;"
        "}"

        // Checkbox hover/disabled
        "QCheckBox::indicator:unchecked:hover {"
        "   border: 1px solid #ffbf66;"
        "   background-color: #1a1a1a;"
        "}"
        "QCheckBox::indicator:checked:hover {"
        "   background-color: #ffbf66;"
        "   border: 1px solid #ffbf66;"
        "}"
        "QCheckBox::indicator:disabled {"
        "   border: 1px solid #3a3a3a;"
        "   background-color: #161616;"
        "}"
        "QCheckBox:disabled {"
        "   color: #777777;"
        "}"

        // Tabs
        "QTabWidget::pane {"
        "   border: 0px;"
        "}"
        "QTabBar::tab {"
        "   height: 14px;"
        "   padding: 4px 10px;"
        "   background: #1a1208;"
        "   color: #ffd6a0;"
        "   border: 1px solid #3a240f;"
        "   border-bottom: none;"
        "}"
        "QTabBar::tab:selected {"
        "   background: #FF9800;"
        "   color: black;"
        "}"
        "QTabBar::tab:hover {"
        "   background: #c77700;"
        "   color: #fff3e6;"
        "}"

        // Scrollbars
        "QScrollBar:vertical {"
        "   width: 12px;"
        "   background: #121212;"
        "   margin: 0;"
        "}"
        "QScrollBar::handle:vertical {"
        "   background: #2A2A2A;"
        "   min-height: 30px;"
        "   border-radius: 6px;"
        "}"
        "QScrollBar::handle:vertical:hover {"
        "   background: #3A3A3A;"
        "}"
        "QScrollBar::handle:vertical:pressed {"
        "   background: #FF9800;"
        "}"
        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {"
        "   height: 0;"
        "}"
        "QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical {"
        "   background: #121212;"
        "}"

        "QScrollBar:horizontal {"
        "   height: 12px;"
        "   background: #121212;"
        "   margin: 0;"
        "}"
        "QScrollBar::handle:horizontal {"
        "   background: #2A2A2A;"
        "   min-width: 30px;"
        "   border-radius: 6px;"
        "}"
        "QScrollBar::handle:horizontal:hover {"
        "   background: #3A3A3A;"
        "}"
        "QScrollBar::handle:horizontal:pressed {"
        "   background: #FF9800;"
        "}"
        "QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal {"
        "   width: 0;"
        "}"
        "QScrollBar::add-page:horizontal, QScrollBar::sub-page:horizontal {"
        "   background: #121212;"
        "}"
    );
}

QString AppTheme::windowsBasic()
{
    return QStringLiteral(
        /* Base window */
        "QWidget {"
        "   background-color: #F0F0F0;"
        "   color: #000000;"
        "}"

        /* Fonts */
        "QLabel, QPushButton, QLineEdit, QPlainTextEdit, QTextEdit,"
        "QComboBox, QSpinBox, QListWidget, QCheckBox,"
        "QMenuBar, QMenu, QStatusBar, QTabWidget, QTabBar {"
        "   font-size: 14px;"
        "}"

        /* Buttons */
        "QPushButton {"
        "   background-color: #E1E1E1;"
        "   border: 1px solid #A0A0A0;"
        "   padding: 6px 10px;"
        "   border-radius: 3px;"
        "}"
        "QPushButton:hover {"
        "   background-color: #D6D6D6;"
        "}"
        "QPushButton:pressed {"
        "   background-color: #C8C8C8;"
        "}"

        /* Inputs */
        "QLineEdit, QTextEdit, QPlainTextEdit, QComboBox, QSpinBox {"
        "   background-color: #FFFFFF;"
        "   border: 1px solid #A0A0A0;"
        "   padding: 4px;"
        "}"

        /* Lists */
        "QListWidget {"
        "   background-color: #FFFFFF;"
        "   border: 1px solid #B0B0B0;"
        "}"
        "QListWidget::item:selected {"
        "   background-color: #3399FF;"
        "   color: #FFFFFF;"
        "}"

        /* Tables / trees */
        "QTreeView, QTableView, QTableWidget {"
        "   background-color: #FFFFFF;"
        "   alternate-background-color: #F6F6F6;"
        "   border: 1px solid #B0B0B0;"
        "   selection-background-color: #3399FF;"
        "   selection-color: white;"
        "}"

        /* Headers */
        "QHeaderView::section {"
        "   background-color: #E6E6E6;"
        "   border: 1px solid #B0B0B0;"
        "   padding: 4px 6px;"
        "}"

        /* Tabs */
        "QTabBar::tab {"
        "   background: #E6E6E6;"
        "   border: 1px solid #B0B0B0;"
        "   padding: 4px 10px;"
        "}"
        "QTabBar::tab:selected {"
        "   background: #FFFFFF;"
        "   border-bottom: 0;"
        "}"

        /* Scrollbars */
        "QScrollBar {"
        "   background: #F0F0F0;"
        "}"
        "QScrollBar::handle {"
        "   background: #C0C0C0;"
        "   border-radius: 4px;"
        "}"
    );
}

QString AppTheme::neo()
{
    return QStringLiteral(
        // Base: deep navy, not near-black
        "QWidget {"
        "   background-color: #0B1020;"
        "   color: #E9ECFF;"
        "}"

        "QLabel, QPushButton, QLineEdit, QPlainTextEdit, QTextEdit, "
        "QComboBox, QSpinBox, QListWidget, QCheckBox, "
        "QMenuBar, QMenu, QStatusBar, QTabWidget, QTabBar {"
        "   font-size: 14px;"
        "}"

        // Buttons: violet border, purple hover, pink press
        "QPushButton {"
        "   background-color: #121A33;"
        "   border: 1px solid #8B5CF6;"   // violet
        "   padding: 6px 10px;"
        "   border-radius: 6px;"
        "}"
        "QPushButton:hover {"
        "   background-color: #18214A;"
        "}"
        "QPushButton:pressed {"
        "   background-color: #F472B6;"   // pink
        "   color: #0B1020;"
        "}"

        // Inputs: bluish panels
        "QLineEdit, QComboBox, QTextEdit, QPlainTextEdit {"
        "   background-color: #0F1730;"
        "   border: 1px solid #24305C;"
        "   padding: 5px;"
        "   border-radius: 6px;"
        "   color: #F4F6FF;"
        "}"

        // --------------------------------
        // SpinBox (FIX: do NOT style subcontrols, avoids missing top border)
        // --------------------------------
        "QSpinBox {"
        "   background-color: #0F1730;"
        "   border: 1px solid #24305C;"
        "   padding: 5px;"
        "   padding-right: 22px;"
        "   border-radius: 6px;"
        "   color: #F4F6FF;"
        "}"

        // Combo popup
        "QComboBox QAbstractItemView {"
        "   background-color: #0F1730;"
        "   selection-background-color: #8B5CF6;"
        "   selection-color: #0B1020;"
        "}"

        // Checkboxes
        "QCheckBox::indicator { width: 18px; height: 18px; }"
        "QCheckBox { spacing: 6px; color: #E9ECFF; }"
        "QCheckBox::indicator:unchecked {"
        "   border: 1px solid #8B5CF6;"
        "   background-color: #0B1020;"
        "}"
        "QCheckBox::indicator:checked {"
        "   background-color: #8B5CF6;"
        "   border: 1px solid #8B5CF6;"
        "}"

        // Item-view indicators (lists) — use violet in Neo (NOT orange)
        "QAbstractItemView::indicator { width: 18px; height: 18px; }"
        "QAbstractItemView::indicator:unchecked {"
        "   border: 1px solid #8B5CF6;"
        "   background-color: #0B1020;"
        "}"
        "QAbstractItemView::indicator:checked {"
        "   background-color: #8B5CF6;"
        "   border: 1px solid #8B5CF6;"
        "}"
        "QAbstractItemView::indicator:unchecked:hover {"
        "   border: 1px solid #a78bfa;"
        "   background-color: #0f1730;"
        "}"
        "QAbstractItemView::indicator:checked:hover {"
        "   background-color: #a78bfa;"
        "   border: 1px solid #a78bfa;"
        "}"
        "QAbstractItemView::indicator:disabled {"
        "   border: 1px solid #24305C;"
        "   background-color: #0E1530;"
        "}"

        // Lists: different shade, selection violet
        "QListWidget {"
        "   background-color: #0E1530;"
        "   border: 1px solid #24305C;"
        "}"
        "QListWidget::item:selected {"
        "   background-color: #8B5CF6;"
        "   color: #0B1020;"
        "}"

        // Trees/tables: stronger contrast
        "QTreeView, QTableView, QTableWidget {"
        "   background-color: #0B1020;"
        "   alternate-background-color: #0E1530;"
        "   border: 1px solid #24305C;"
        "   gridline-color: #24305C;"
        "   selection-background-color: #F472B6;"  // PINK selection so it’s obviously different
        "   selection-color: #0B1020;"
        "   outline: 0;"
        "}"
        "QTreeView::item, QTableView::item, QTableWidget::item { padding: 2px 6px; }"

        // Headers
        "QHeaderView::section {"
        "   background-color: #121A33;"
        "   color: #E9ECFF;"
        "   padding: 4px 8px;"
        "   border: 0;"
        "   border-right: 1px solid #24305C;"
        "}"

        // Checkbox hover/disabled
        "QCheckBox::indicator:unchecked:hover {"
        "   border: 1px solid #a78bfa;"
        "   background-color: #0f1730;"
        "}"
        "QCheckBox::indicator:checked:hover {"
        "   background-color: #a78bfa;"
        "   border: 1px solid #a78bfa;"
        "}"
        "QCheckBox::indicator:disabled {"
        "   border: 1px solid #24305C;"
        "   background-color: #0E1530;"
        "}"
        "QCheckBox:disabled {"
        "   color: #6f7399;"
        "}"

        // Tabs
        "QTabWidget::pane { border: 0px; }"
        "QTabBar::tab {"
        "   height: 14px;"
        "   padding: 4px 10px;"
        "   background: #0E1530;"
        "   color: #C7CBFF;"
        "   border: 1px solid #24305C;"
        "   border-bottom: none;"
        "}"
        "QTabBar::tab:selected {"
        "   background: #8B5CF6;"
        "   color: #0B1020;"
        "}"
        "QTabBar::tab:hover {"
        "   background: #18214A;"
        "   color: #F4F6FF;"
        "}"

        // Menus
        "QMenu {"
        "   background-color: #0F1730;"
        "   border: 1px solid #24305C;"
        "   padding: 6px;"
        "}"
        "QMenu::item { padding: 6px 18px; border-radius: 6px; }"
        "QMenu::item:selected {"
        "   background-color: #8B5CF6;"
        "   color: #0B1020;"
        "}"

        // Scrollbars
        "QScrollBar:vertical { width: 12px; background: #0B1020; margin: 0; }"
        "QScrollBar::handle:vertical { background: #24305C; min-height: 30px; border-radius: 6px; }"
        "QScrollBar::handle:vertical:hover { background: #2F3A6E; }"
        "QScrollBar::handle:vertical:pressed { background: #8B5CF6; }"
        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }"
        "QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: #0B1020; }"

        "QScrollBar:horizontal { height: 12px; background: #0B1020; margin: 0; }"
        "QScrollBar::handle:horizontal { background: #24305C; min-width: 30px; border-radius: 6px; }"
        "QScrollBar::handle:horizontal:hover { background: #2F3A6E; }"
        "QScrollBar::handle:horizontal:pressed { background: #8B5CF6; }"
        "QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width: 0; }"
        "QScrollBar::add-page:horizontal, QScrollBar::sub-page:horizontal { background: #0B1020; }"
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

QColor AppTheme::accent(const QString& themeId)
{
    if (themeId == "cpunk-orange")
        return QColor("#FF9800");

    if (themeId == "windows-basic")
        return QColor("#0078D7");   // Windows blue

    // default: CPUNK Dark
    return QColor("#00FF99");
}
