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
        "QWidget {"
        "   background-color: #121212;"
        "   color: #DDDDDD;"
        "}"

        "QLabel, QPushButton, QLineEdit, QPlainTextEdit, QTextEdit, "
        "QComboBox, QSpinBox, QListWidget, QCheckBox, "
        "QMenuBar, QMenu, QStatusBar, QTabWidget, QTabBar {"
        "   font-size: 14px;"
        "}"

        "QPushButton {"
        "   background-color: #1E1E1E;"
        "   border: 1px solid #00FF99;"
        "   padding: 6px 10px;"
        "   border-radius: 4px;"
        "}"
        "QPushButton:hover { background-color: #2A2A2A; }"
        "QPushButton:pressed { background-color: #00FF99; color: #000000; }"

        "QLineEdit, QSpinBox, QComboBox, QTextEdit, QPlainTextEdit {"
        "   background-color: #1A1A1A;"
        "   border: 1px solid #333333;"
        "   padding: 4px;"
        "   border-radius: 3px;"
        "   color: #EEEEEE;"
        "}"

        "QSpinBox {"
        "   background-color: #1A1A1A;"
        "   border: 1px solid #3A3A3A;"
        "   padding-right: 18px;"
        "}"
        "QSpinBox::up-button, QSpinBox::down-button {"
        "   background-color: #202020;"
        "   border-left: 1px solid #3A3A3A;"
        "   border-top: 1px solid #3A3A3A;"
        "   width: 18px;"
        "}"
        "QSpinBox::up-button:hover, QSpinBox::down-button:hover { background-color: #262626; }"
        "QSpinBox::up-arrow {"
        "   image: none; width: 0; height: 0;"
        "   border-left: 5px solid transparent;"
        "   border-right: 5px solid transparent;"
        "   border-bottom: 7px solid #8A8A8A;"
        "}"
        "QSpinBox::down-arrow {"
        "   image: none; width: 0; height: 0;"
        "   border-left: 5px solid transparent;"
        "   border-right: 5px solid transparent;"
        "   border-top: 7px solid #8A8A8A;"
        "}"

        // ✅ Combo popup hover + selected
        "QComboBox QAbstractItemView {"
        "   background-color: #1E1E1E;"
        "   border: 1px solid #2A2A2A;"
        "   outline: 0;"
        "}"
        "QComboBox QAbstractItemView::item {"
        "   padding: 6px 10px;"
        "}"
        "QComboBox QAbstractItemView::item:hover {"
        "   background-color: #159a66;"
        "   color: #eafff4;"
        "}"
        "QComboBox QAbstractItemView::item:selected {"
        "   background-color: #00FF99;"
        "   color: #000000;"
        "}"

        // Checkboxes
        "QCheckBox { spacing: 6px; color: #DDDDDD; }"
        "QCheckBox::indicator { width: 18px; height: 18px; margin: 2px; }"
        "QCheckBox::indicator:unchecked { border: 1px solid #00FF99; background-color: #121212; }"
        "QCheckBox::indicator:checked { background-color: #00FF99; border: 1px solid #00FF99; }"
        "QCheckBox::indicator:unchecked:hover { border: 1px solid #7cffc8; background-color: #1a1a1a; }"
        "QCheckBox::indicator:checked:hover { background-color: #7cffc8; border: 1px solid #7cffc8; }"
        "QCheckBox::indicator:disabled { border: 1px solid #3a3a3a; background-color: #161616; }"
        "QCheckBox:disabled { color: #777777; }"

        // ✅ List/ItemView checkboxes (Targets list etc.)
        "QAbstractItemView::indicator { width: 18px; height: 18px; margin: 2px; }"
        "QAbstractItemView::indicator:unchecked { border: 1px solid #00FF99; background-color: #121212; }"
        "QAbstractItemView::indicator:checked { background-color: #00FF99; border: 1px solid #00FF99; }"
        "QAbstractItemView::indicator:unchecked:hover { border: 1px solid #7cffc8; background-color: #1a1a1a; }"
        "QAbstractItemView::indicator:checked:hover { background-color: #7cffc8; border: 1px solid #7cffc8; }"
        "QAbstractItemView::indicator:disabled { border: 1px solid #3a3a3a; background-color: #161616; }"

        "QListWidget { background-color: #181818; border: 1px solid #333333; }"
        "QListWidget::item { padding: 3px 6px; }"
        "QListWidget::item:selected { background-color: #00FF99; color: black; }"

        "QTreeView, QTableView, QTableWidget {"
        "   background-color: #121212;"
        "   alternate-background-color: #1A1A1A;"
        "   border: 1px solid #2A2A2A;"
        "   gridline-color: #2A2A2A;"
        "   selection-background-color: #00CC77;"
        "   selection-color: #000000;"
        "   outline: 0;"
        "}"
        "QTreeView::item, QTableView::item, QTableWidget::item { padding: 2px 6px; }"
        "QTreeView::item:selected, QTableView::item:selected, QTableWidget::item:selected {"
        "   background-color: #00CC77; color: #000000;"
        "}"

        "QHeaderView::section {"
        "   background-color: #1E1E1E;"
        "   color: #DDDDDD;"
        "   padding: 4px 8px;"
        "   border: 0;"
        "   border-right: 1px solid #2A2A2A;"
        "}"

        "QTabWidget::pane { border: 0px; }"
        "QTabBar::tab {"
        "   height: 14px;"
        "   padding: 4px 10px;"
        "   background: #0f1a14;"
        "   color: #b7f5d1;"
        "   border: 1px solid #1f3a2c;"
        "   border-bottom: none;"
        "}"
        "QTabBar::tab:selected { background: #00FF99; color: black; }"
        "QTabBar::tab:hover { background: #159a66; color: #eafff4; }"

        "QScrollBar:vertical { width: 12px; background: #121212; margin: 0; }"
        "QScrollBar::handle:vertical { background: #2A2A2A; min-height: 30px; border-radius: 6px; }"
        "QScrollBar::handle:vertical:hover { background: #3A3A3A; }"
        "QScrollBar::handle:vertical:pressed { background: #00FF99; }"
        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }"
        "QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: #121212; }"

        "QScrollBar:horizontal { height: 12px; background: #121212; margin: 0; }"
        "QScrollBar::handle:horizontal { background: #2A2A2A; min-width: 30px; border-radius: 6px; }"
        "QScrollBar::handle:horizontal:hover { background: #3A3A3A; }"
        "QScrollBar::handle:horizontal:pressed { background: #00FF99; }"
        "QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width: 0; }"
        "QScrollBar::add-page:horizontal, QScrollBar::sub-page:horizontal { background: #121212; }"
        // Menus (top menu dropdowns)
        "QMenu {"
        "   background-color: #1A1A1A;"
        "   color: #DDDDDD;"
        "   border: 1px solid #2A2A2A;"
        "   padding: 6px;"
        "}"
        "QMenu::separator {"
        "   height: 1px;"
        "   background: #2A2A2A;"
        "   margin: 6px 8px;"
        "}"
        "QMenu::item {"
        "   padding: 6px 18px;"
        "   border-radius: 6px;"
        "}"
        "QMenu::item:selected, QMenu::item:hover {"
        "   background-color: #00FF99;"
        "   color: #000000;"
        "}"
        "QMenu::item:disabled {"
        "   color: #777777;"
        "}"

    );
}

QString AppTheme::orange()
{
    return QStringLiteral(
        "QWidget { background-color: #121212; color: #DDDDDD; }"

        "QLabel, QPushButton, QLineEdit, QPlainTextEdit, QTextEdit, "
        "QComboBox, QSpinBox, QListWidget, QCheckBox, "
        "QMenuBar, QMenu, QStatusBar, QTabWidget, QTabBar { font-size: 14px; }"

        "QPushButton { background-color: #1E1E1E; border: 1px solid #FF9800; padding: 6px 10px; border-radius: 4px; }"
        "QPushButton:hover { background-color: #2A2A2A; }"
        "QPushButton:pressed { background-color: #FF9800; color: #000000; }"

        "QLineEdit, QSpinBox, QComboBox, QTextEdit, QPlainTextEdit {"
        "   background-color: #1A1A1A;"
        "   border: 1px solid #333333;"
        "   padding: 4px;"
        "   border-radius: 3px;"
        "   color: #EEEEEE;"
        "}"

        "QSpinBox { background-color: #1A1A1A; border: 1px solid #3A3A3A; padding-right: 18px; }"
        "QSpinBox::up-button, QSpinBox::down-button { background-color: #202020; border-left: 1px solid #3A3A3A; border-top: 1px solid #3A3A3A; width: 18px; }"
        "QSpinBox::up-button:hover, QSpinBox::down-button:hover { background-color: #262626; }"
        "QSpinBox::up-arrow { image: none; width: 0; height: 0; border-left: 5px solid transparent; border-right: 5px solid transparent; border-bottom: 7px solid #8A8A8A; }"
        "QSpinBox::down-arrow { image: none; width: 0; height: 0; border-left: 5px solid transparent; border-right: 5px solid transparent; border-top: 7px solid #8A8A8A; }"

        // ✅ Combo popup hover + selected
        "QComboBox QAbstractItemView { background-color: #1E1E1E; border: 1px solid #2A2A2A; outline: 0; }"
        "QComboBox QAbstractItemView::item { padding: 6px 10px; }"
        "QComboBox QAbstractItemView::item:hover { background-color: #c77700; color: #fff3e6; }"
        "QComboBox QAbstractItemView::item:selected { background-color: #FF9800; color: #000000; }"

        "QCheckBox { spacing: 6px; color: #DDDDDD; }"
        "QCheckBox::indicator { width: 18px; height: 18px; margin: 2px; }"
        "QCheckBox::indicator:unchecked { border: 1px solid #FF9800; background-color: #121212; }"
        "QCheckBox::indicator:checked { background-color: #FF9800; border: 1px solid #FF9800; }"
        "QCheckBox::indicator:unchecked:hover { border: 1px solid #ffbf66; background-color: #1a1a1a; }"
        "QCheckBox::indicator:checked:hover { background-color: #ffbf66; border: 1px solid #ffbf66; }"
        "QCheckBox::indicator:disabled { border: 1px solid #3a3a3a; background-color: #161616; }"
        "QCheckBox:disabled { color: #777777; }"

        "QAbstractItemView::indicator { width: 18px; height: 18px; margin: 2px; }"
        "QAbstractItemView::indicator:unchecked { border: 1px solid #FF9800; background-color: #121212; }"
        "QAbstractItemView::indicator:checked { background-color: #FF9800; border: 1px solid #FF9800; }"
        "QAbstractItemView::indicator:unchecked:hover { border: 1px solid #ffbf66; background-color: #1a1a1a; }"
        "QAbstractItemView::indicator:checked:hover { background-color: #ffbf66; border: 1px solid #ffbf66; }"
        "QAbstractItemView::indicator:disabled { border: 1px solid #3a3a3a; background-color: #161616; }"

        "QListWidget { background-color: #181818; border: 1px solid #333333; }"
        "QListWidget::item { padding: 3px 6px; }"
        "QListWidget::item:selected { background-color: #FF9800; color: black; }"

        "QTreeView, QTableView, QTableWidget {"
        "   background-color: #121212;"
        "   alternate-background-color: #1A1A1A;"
        "   border: 1px solid #2A2A2A;"
        "   gridline-color: #2A2A2A;"
        "   selection-background-color: #E08600;"
        "   selection-color: #000000;"
        "   outline: 0;"
        "}"
        "QTreeView::item, QTableView::item, QTableWidget::item { padding: 2px 6px; }"
        "QTreeView::item:selected, QTableView::item:selected, QTableWidget::item:selected { background-color: #E08600; color: #000000; }"

        "QHeaderView::section { background-color: #1E1E1E; color: #DDDDDD; padding: 4px 8px; border: 0; border-right: 1px solid #2A2A2A; }"

        "QTabWidget::pane { border: 0px; }"
        "QTabBar::tab { height: 14px; padding: 4px 10px; background: #1a1208; color: #ffd6a0; border: 1px solid #3a240f; border-bottom: none; }"
        "QTabBar::tab:selected { background: #FF9800; color: black; }"
        "QTabBar::tab:hover { background: #c77700; color: #fff3e6; }"

        "QScrollBar:vertical { width: 12px; background: #121212; margin: 0; }"
        "QScrollBar::handle:vertical { background: #2A2A2A; min-height: 30px; border-radius: 6px; }"
        "QScrollBar::handle:vertical:hover { background: #3A3A3A; }"
        "QScrollBar::handle:vertical:pressed { background: #FF9800; }"
        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }"
        "QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: #121212; }"

        "QScrollBar:horizontal { height: 12px; background: #121212; margin: 0; }"
        "QScrollBar::handle:horizontal { background: #2A2A2A; min-width: 30px; border-radius: 6px; }"
        "QScrollBar::handle:horizontal:hover { background: #3A3A3A; }"
        "QScrollBar::handle:horizontal:pressed { background: #FF9800; }"
        "QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width: 0; }"
        "QScrollBar::add-page:horizontal, QScrollBar::sub-page:horizontal { background: #121212; }"
        // Menus (top menu dropdowns)
        "QMenu {"
        "   background-color: #1A1A1A;"
        "   color: #DDDDDD;"
        "   border: 1px solid #2A2A2A;"
        "   padding: 6px;"
        "}"
        "QMenu::separator {"
        "   height: 1px;"
        "   background: #2A2A2A;"
        "   margin: 6px 8px;"
        "}"
        "QMenu::item {"
        "   padding: 6px 18px;"
        "   border-radius: 6px;"
        "}"
        "QMenu::item:selected, QMenu::item:hover {"
        "   background-color: #FF9800;"
        "   color: #000000;"
        "}"
        "QMenu::item:disabled {"
        "   color: #777777;"
        "}"

    );
}

QString AppTheme::windowsBasic()
{
    return QStringLiteral(
        "QWidget { background-color: #F0F0F0; color: #000000; }"

        "QLabel, QPushButton, QLineEdit, QPlainTextEdit, QTextEdit,"
        "QComboBox, QSpinBox, QListWidget, QCheckBox,"
        "QMenuBar, QMenu, QStatusBar, QTabWidget, QTabBar { font-size: 14px; }"

        "QPushButton { background-color: #E1E1E1; border: 1px solid #A0A0A0; padding: 6px 10px; border-radius: 3px; }"
        "QPushButton:hover { background-color: #D6D6D6; }"
        "QPushButton:pressed { background-color: #C8C8C8; }"

        "QLineEdit, QTextEdit, QPlainTextEdit, QComboBox, QSpinBox { background-color: #FFFFFF; border: 1px solid #A0A0A0; padding: 4px; }"

        // ✅ Combo popup hover (classic already works, but keep consistent)
        "QComboBox QAbstractItemView { background-color: #FFFFFF; border: 1px solid #A0A0A0; outline: 0; }"
        "QComboBox QAbstractItemView::item { padding: 6px 10px; }"
        "QComboBox QAbstractItemView::item:hover { background-color: #E6F0FF; color: #000000; }"
        "QComboBox QAbstractItemView::item:selected { background-color: #3399FF; color: #FFFFFF; }"

        "QListWidget { background-color: #FFFFFF; border: 1px solid #B0B0B0; }"
        "QListWidget::item { padding: 3px 6px; }"
        "QListWidget::item:selected { background-color: #3399FF; color: #FFFFFF; }"

        "QTreeView, QTableView, QTableWidget {"
        "   background-color: #FFFFFF;"
        "   alternate-background-color: #F6F6F6;"
        "   border: 1px solid #B0B0B0;"
        "   selection-background-color: #3399FF;"
        "   selection-color: white;"
        "}"

        "QHeaderView::section { background-color: #E6E6E6; border: 1px solid #B0B0B0; padding: 4px 6px; }"

        "QTabBar::tab { background: #E6E6E6; border: 1px solid #B0B0B0; padding: 4px 10px; }"
        "QTabBar::tab:selected { background: #FFFFFF; border-bottom: 0; }"

        "QScrollBar { background: #F0F0F0; }"
        "QScrollBar::handle { background: #C0C0C0; border-radius: 4px; }"
        /* Menus (top menu dropdowns) */
        "QMenu {"
        "   background-color: #FFFFFF;"
        "   color: #000000;"
        "   border: 1px solid #B0B0B0;"
        "   padding: 6px;"
        "}"
        "QMenu::separator {"
        "   height: 1px;"
        "   background: #D0D0D0;"
        "   margin: 6px 8px;"
        "}"
        "QMenu::item {"
        "   padding: 6px 18px;"
        "}"
        "QMenu::item:selected, QMenu::item:hover {"
        "   background-color: #3399FF;"
        "   color: #FFFFFF;"
        "}"
        "QMenu::item:disabled {"
        "   color: #777777;"
        "}"

    );
}

QString AppTheme::neo()
{
    return QStringLiteral(
        "QWidget { background-color: #0B1020; color: #E9ECFF; }"

        "QLabel, QPushButton, QLineEdit, QPlainTextEdit, QTextEdit, "
        "QComboBox, QSpinBox, QListWidget, QCheckBox, "
        "QMenuBar, QMenu, QStatusBar, QTabWidget, QTabBar { font-size: 14px; }"

        "QPushButton { background-color: #121A33; border: 1px solid #8B5CF6; padding: 6px 10px; border-radius: 6px; }"
        "QPushButton:hover { background-color: #18214A; }"
        "QPushButton:pressed { background-color: #F472B6; color: #0B1020; }"

        "QLineEdit, QSpinBox, QComboBox, QTextEdit, QPlainTextEdit {"
        "   background-color: #0F1730;"
        "   border: 1px solid #24305C;"
        "   padding: 5px;"
        "   border-radius: 6px;"
        "   color: #F4F6FF;"
        "}"

        // ✅ Combo popup hover + selected
        "QComboBox QAbstractItemView { background-color: #0F1730; border: 1px solid #24305C; outline: 0; }"
        "QComboBox QAbstractItemView::item { padding: 6px 10px; }"
        "QComboBox QAbstractItemView::item:hover { background-color: #18214A; color: #F4F6FF; }"
        "QComboBox QAbstractItemView::item:selected { background-color: #8B5CF6; color: #0B1020; }"

        "QSpinBox::up-button { background-color: #121A33; border-left: 1px solid #24305C; border-top: 1px solid #24305C; width: 18px; }"
        "QSpinBox::down-button { background-color: #121A33; border-left: 1px solid #24305C; border-bottom: 1px solid #24305C; width: 18px; }"

        "QCheckBox { spacing: 6px; color: #E9ECFF; }"
        "QCheckBox::indicator { width: 18px; height: 18px; margin: 2px; }"
        "QCheckBox::indicator:unchecked { border: 1px solid #8B5CF6; background-color: #0B1020; }"
        "QCheckBox::indicator:checked { background-color: #8B5CF6; border: 1px solid #8B5CF6; }"
        "QCheckBox::indicator:unchecked:hover { border: 1px solid #a78bfa; background-color: #0f1730; }"
        "QCheckBox::indicator:checked:hover { background-color: #a78bfa; border: 1px solid #a78bfa; }"
        "QCheckBox::indicator:disabled { border: 1px solid #24305C; background-color: #0E1530; }"
        "QCheckBox:disabled { color: #6f7399; }"

        // ✅ ItemViews checkbox indicator (targets list)
        "QAbstractItemView::indicator { width: 18px; height: 18px; margin: 2px; }"
        "QAbstractItemView::indicator:unchecked { border: 1px solid #8B5CF6; background-color: #0B1020; }"
        "QAbstractItemView::indicator:checked { background-color: #8B5CF6; border: 1px solid #8B5CF6; }"
        "QAbstractItemView::indicator:unchecked:hover { border: 1px solid #a78bfa; background-color: #0f1730; }"
        "QAbstractItemView::indicator:checked:hover { background-color: #a78bfa; border: 1px solid #a78bfa; }"
        "QAbstractItemView::indicator:disabled { border: 1px solid #24305C; background-color: #0E1530; }"

        "QListWidget { background-color: #0E1530; border: 1px solid #24305C; }"
        "QListWidget::item { padding: 3px 6px; }"
        "QListWidget::item:selected { background-color: #8B5CF6; color: #0B1020; }"

        "QTreeView, QTableView, QTableWidget {"
        "   background-color: #0B1020;"
        "   alternate-background-color: #0E1530;"
        "   border: 1px solid #24305C;"
        "   gridline-color: #24305C;"
        "   selection-background-color: #F472B6;"
        "   selection-color: #0B1020;"
        "   outline: 0;"
        "}"
        "QTreeView::item, QTableView::item, QTableWidget::item { padding: 2px 6px; }"

        "QHeaderView::section { background-color: #121A33; color: #E9ECFF; padding: 4px 8px; border: 0; border-right: 1px solid #24305C; }"

        "QTabWidget::pane { border: 0px; }"
        "QTabBar::tab { height: 14px; padding: 4px 10px; background: #0E1530; color: #C7CBFF; border: 1px solid #24305C; border-bottom: none; }"
        "QTabBar::tab:selected { background: #8B5CF6; color: #0B1020; }"
        "QTabBar::tab:hover { background: #18214A; color: #F4F6FF; }"

        "QMenu { background-color: #0F1730; border: 1px solid #24305C; padding: 6px; }"
        "QMenu::item { padding: 6px 18px; border-radius: 6px; }"
        "QMenu::item:selected { background-color: #8B5CF6; color: #0B1020; }"

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
        return QColor("#0078D7");

    return QColor("#00FF99");
}
