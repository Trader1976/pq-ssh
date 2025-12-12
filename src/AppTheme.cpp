// AppTheme.cpp
#include "AppTheme.h"

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
        "   border: 1px solid #00FF99;"
        "}"
        "QTabBar::tab {"
        "   background: #1E1E1E;"
        "   padding: 6px;"
        "   color: #DDDDDD;"
        "}"
        "QTabBar::tab:selected {"
        "   background: #00FF99;"
        "   color: black;"
        "}"
        "QTabBar::tab:hover {"
        "   background: #2A2A2A;"
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
