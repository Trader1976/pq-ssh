#pragma once
#include <QString>

namespace Logger {
    void install(const QString& appName);

    // 0=Errors only, 1=Normal, 2=Debug
    void setLogLevel(int level);
    int  logLevel();

    QString logFilePath();

    // NEW
    void setLogFilePathOverride(const QString& absoluteFilePath);  // empty => use default
    QString logDirPath();  // convenience: parent directory of current log file
}