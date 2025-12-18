// Logger.h
#pragma once

#include <QString>

namespace Logger {

    // Initializes logging (call once early in main()).
    void install(const QString& appName);

    // Absolute path to active log file.
    QString logFilePath();

    // 0=Errors only, 1=Normal, 2=Debug
    void setLogLevel(int level);
    int  logLevel();

} // namespace Logger
