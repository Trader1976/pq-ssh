cmake_minimum_required(VERSION 3.16)
project(PQ_SSH LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# Qt5 Widgets
find_package(Qt5 REQUIRED COMPONENTS Widgets)

# pkg-config modules (libssh + qtermwidget5)
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBSSH REQUIRED libssh)
pkg_check_modules(QTERM  REQUIRED qtermwidget5)

add_executable(pq-ssh
        src/main.cpp
        src/MainWindow.cpp
        src/MainWindow.h

        src/SshClient.cpp
        src/SshClient.h

        src/SshShellWorker.cpp
        src/SshShellWorker.h

        src/TerminalView.cpp
        src/TerminalView.h

        src/CpunkTermWidget.cpp
        src/CpunkTermWidget.h
        src/AppTheme.cpp


        # Add these when they exist as real files:
        src/ProfileStore.cpp
        src/ProfileStore.h
        src/ProfilesEditorDialog.cpp
        src/ProfilesEditorDialog.h

        src/AppTheme.h
        src/SshProfile.h
        src/ShellManager.cpp
        src/ShellManager.h

        resources/pqssh_resources.qrc
        src/ThemeInstaller.cpp
        src/ThemeInstaller.h
        src/KeyGeneratorDialog.h
        src/KeyGeneratorDialog.cpp
        src/KeyMetadataUtils.h
        src/KeyMetadataUtils.cpp
        src/Logger.h
        src/Logger.cpp
        src/DilithiumKeyCrypto.h
        src/DilithiumKeyCrypto.cpp
        src/FilesTab.h
        src/FilesTab.cpp
        src/RemoteDropTable.h
        src/RemoteDropTable.cpp
)

target_include_directories(pq-ssh
        PRIVATE
        ${LIBSSH_INCLUDE_DIRS}
        ${QTERM_INCLUDE_DIRS}
)

target_compile_options(pq-ssh
        PRIVATE
        ${LIBSSH_CFLAGS_OTHER}
        ${QTERM_CFLAGS_OTHER}
)

target_link_libraries(pq-ssh
        PRIVATE
        Qt5::Widgets
        ${LIBSSH_LIBRARIES}
        ${QTERM_LIBRARIES}
)

set_target_properties(pq-ssh PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

find_package(PkgConfig REQUIRED)

pkg_check_modules(LIBSSH REQUIRED libssh)
pkg_check_modules(QTERM  REQUIRED qtermwidget5)
pkg_check_modules(SODIUM REQUIRED libsodium)

target_include_directories(pq-ssh PRIVATE
        ${LIBSSH_INCLUDE_DIRS}
        ${QTERM_INCLUDE_DIRS}
        ${SODIUM_INCLUDE_DIRS}
)

target_link_libraries(pq-ssh PRIVATE
        Qt5::Widgets
        ${LIBSSH_LIBRARIES}
        ${QTERM_LIBRARIES}
        ${SODIUM_LIBRARIES}
)
