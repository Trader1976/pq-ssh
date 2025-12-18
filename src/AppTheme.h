#pragma once
#include <QString>
#include <QColor>

class AppTheme
{
public:
    static QString dark();
    static QString orange();
    static QString windowsBasic();
    static QString neo();

    // Centralized accent color resolver (you already started this pattern)
    static QColor accent(const QString& themeId);
};
