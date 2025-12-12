#include "AppTheme.h"

QString AppTheme::dark()
{
    return QStringLiteral(R"(
        QWidget {
            background-color: #1e1e1e;
            color: #d4d4d4;
        }

        QLineEdit, QPlainTextEdit, QTextEdit {
            background-color: #252526;
            border: 1px solid #3c3c3c;
            padding: 4px;
            selection-background-color: #007acc;
        }

        QPushButton {
            background-color: #2d2d30;
            border: 1px solid #3c3c3c;
            padding: 6px 10px;
        }

        QPushButton:hover {
            background-color: #3e3e42;
        }

        QPushButton:pressed {
            background-color: #007acc;
        }

        QListWidget {
            background-color: #252526;
            border: 1px solid #3c3c3c;
        }

        QMenuBar {
            background-color: #2d2d30;
        }

        QMenuBar::item:selected {
            background-color: #007acc;
        }

        QStatusBar {
            background-color: #2d2d30;
        }
    )");
}
