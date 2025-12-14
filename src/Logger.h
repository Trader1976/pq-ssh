#pragma once
#include <QString>

namespace Logger {
    void install(const QString& appName);
    QString logFilePath();
}
