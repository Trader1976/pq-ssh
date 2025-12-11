cmake_minimum_required(VERSION 3.16)

project(PQ_SSH LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Qt automatic things
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# libssh
find_package(LIBSSH REQUIRED)

# Qt5 Widgets (NOT Qt6)
find_package(Qt5 REQUIRED COMPONENTS Widgets)

# QTermWidget via pkg-config (Qt5-based)
find_package(PkgConfig REQUIRED)
pkg_check_modules(QTERM REQUIRED qtermwidget5)

# Source files
add_executable(pq-ssh
    src/main.cpp
    src/MainWindow.cpp
    src/MainWindow.h
    src/SshShellWorker.cpp
    src/TerminalView.cpp
)

# QTermWidget include dirs
target_include_directories(pq-ssh
    PRIVATE
        ${QTERM_INCLUDE_DIRS}
)

# Link Qt5 + libssh + QTermWidget
target_link_libraries(pq-ssh
    PRIVATE
        Qt5::Widgets
        ssh                 # from find_package(LIBSSH)
        ${QTERM_LIBRARIES}  # from pkg_check_modules(QTERM ...)
)

# Put binary into bin/
set_target_properties(pq-ssh PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
