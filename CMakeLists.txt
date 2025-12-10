cmake_minimum_required(VERSION 3.16)

project(PQ_SSH LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Qt automatic things
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# Find Qt6 Widgets
find_package(Qt6 REQUIRED COMPONENTS Widgets)

# Source files
add_executable(pq-ssh
    src/main.cpp
    src/MainWindow.cpp
    src/MainWindow.h
)

target_link_libraries(pq-ssh
    PRIVATE Qt6::Widgets
)

# Put binary into bin/
set_target_properties(pq-ssh PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
