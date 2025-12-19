cmake_minimum_required(VERSION 3.16)
project(PQ_SSH LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# ----------------------------
# Options
# ----------------------------
# Path to a private libssh install prefix, e.g. /home/timo/opt/libssh-0.11
# Leave empty to use system libssh.
set(PQSSH_LIBSSH_PREFIX "" CACHE PATH "Optional private libssh prefix (contains include/ and lib/ and lib/pkgconfig/)")

# Where we will copy runtime libs for dev runs
set(PQSSH_RUNTIME_LIB_DIR "runtime-libs" CACHE STRING "Folder next to the built binary for bundled shared libraries")

# ----------------------------
# Qt
# ----------------------------
find_package(Qt5 REQUIRED COMPONENTS Widgets)
# ----------------------------
# OpenSSL (for PBKDF2/HKDF/SHA3 in Identity Manager)
# ----------------------------
find_package(OpenSSL REQUIRED)

# ----------------------------
# pkg-config
# ----------------------------
find_package(PkgConfig REQUIRED)

# If user provided a private libssh prefix, prefer its pkg-config
if(PQSSH_LIBSSH_PREFIX AND EXISTS "${PQSSH_LIBSSH_PREFIX}")
    message(STATUS "Using private libssh prefix: ${PQSSH_LIBSSH_PREFIX}")
    set(ENV{PKG_CONFIG_PATH}
            "${PQSSH_LIBSSH_PREFIX}/lib/pkgconfig:${PQSSH_LIBSSH_PREFIX}/lib/x86_64-linux-gnu/pkgconfig:$ENV{PKG_CONFIG_PATH}"
    )
endif()

pkg_check_modules(LIBSSH REQUIRED libssh)
pkg_check_modules(QTERM  REQUIRED qtermwidget5)
pkg_check_modules(SODIUM REQUIRED libsodium)

# ----------------------------
# Target
# ----------------------------
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
        src/AppTheme.h

        src/ProfileStore.cpp
        src/ProfileStore.h
        src/ProfilesEditorDialog.cpp
        src/ProfilesEditorDialog.h

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

        src/SettingsDialog.cpp

        src/IdentityManagerDialog.h
        src/IdentityManagerDialog.cpp

        src/IdentityDerivation.h
        src/IdentityDerivation.cpp

        src/OpenSshEd25519Key.h
        src/OpenSshEd25519Key.cpp
        src/IdentityManagerDialog.cpp
        src/IdentityDerivation.h
        src/IdentityDerivation.cpp
        src/IdentityDerivation.cpp
        src/OpenSshEd25519Key.h
        src/OpenSshEd25519Key.cpp
)
target_link_libraries(pq-ssh PRIVATE
        Qt5::Widgets
        ${LIBSSH_LIBRARIES}
        ${QTERM_LIBRARIES}
        ${SODIUM_LIBRARIES}
        OpenSSL::Crypto
)
target_include_directories(pq-ssh PRIVATE
        ${LIBSSH_INCLUDE_DIRS}
        ${QTERM_INCLUDE_DIRS}
        ${SODIUM_INCLUDE_DIRS}
)

target_compile_options(pq-ssh PRIVATE
        ${LIBSSH_CFLAGS_OTHER}
        ${QTERM_CFLAGS_OTHER}
        ${SODIUM_CFLAGS_OTHER}
)

target_link_libraries(pq-ssh PRIVATE
        Qt5::Widgets
        ${LIBSSH_LIBRARIES}
        ${QTERM_LIBRARIES}
        ${SODIUM_LIBRARIES}
)

# Put binary under build/bin like you had
set_target_properties(pq-ssh PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# ----------------------------
# Private libssh runtime setup (dev-friendly)
# ----------------------------
# We copy the private libssh .so next to the executable, and set RPATH to find it.
if(PQSSH_LIBSSH_PREFIX AND EXISTS "${PQSSH_LIBSSH_PREFIX}/lib")
    add_custom_command(TARGET pq-ssh POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:pq-ssh>/${PQSSH_RUNTIME_LIB_DIR}"
            COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${PQSSH_LIBSSH_PREFIX}/lib"
            "$<TARGET_FILE_DIR:pq-ssh>/${PQSSH_RUNTIME_LIB_DIR}"
            COMMENT "Copying private libs from ${PQSSH_LIBSSH_PREFIX}/lib to runtime folder"
    )

    # Ensure the loader searches our runtime-libs folder first
    set_target_properties(pq-ssh PROPERTIES
            BUILD_RPATH "\$ORIGIN/${PQSSH_RUNTIME_LIB_DIR}"
            INSTALL_RPATH "\$ORIGIN/${PQSSH_RUNTIME_LIB_DIR}"
    )
endif()

# Helpful debug print
message(STATUS "LIBSSH version (pkg-config): ${LIBSSH_VERSION}")
message(STATUS "LIBSSH libraries: ${LIBSSH_LIBRARIES}")
message(STATUS "LIBSSH include dirs: ${LIBSSH_INCLUDE_DIRS}")
