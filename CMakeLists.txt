cmake_minimum_required(VERSION 3.20)

project(sakura_wx_prototype VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/wxWidgets/CMakeLists.txt")
    message(FATAL_ERROR "wxWidgets submodule is missing. Run: git submodule update --init --recursive")
endif()

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/libtsm/src/tsm/libtsm.h")
    message(FATAL_ERROR "libtsm submodule is missing. Run: git submodule update --init --recursive")
endif()

# Build only the wxWidgets libraries needed by this prototype.
set(wxBUILD_SHARED OFF CACHE BOOL "" FORCE)
set(wxBUILD_SAMPLES OFF CACHE BOOL "" FORCE)
set(wxBUILD_TESTS OFF CACHE BOOL "" FORCE)
set(wxBUILD_DEMOS OFF CACHE BOOL "" FORCE)
set(wxBUILD_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/wxWidgets EXCLUDE_FROM_ALL)

# libtsm currently uses Meson upstream. Its small core is built directly here
# so the prototype has one CMake build and does not need a separate install.
set(LIBTSM_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/libtsm")
add_library(sakura_libtsm STATIC
    "${LIBTSM_ROOT}/src/shared/shl-htable.c"
    "${LIBTSM_ROOT}/external/wcwidth/wcwidth.c"
    "${LIBTSM_ROOT}/src/tsm/tsm-render.c"
    "${LIBTSM_ROOT}/src/tsm/tsm-screen.c"
    "${LIBTSM_ROOT}/src/tsm/tsm-selection.c"
    "${LIBTSM_ROOT}/src/tsm/tsm-unicode.c"
    "${LIBTSM_ROOT}/src/tsm/tsm-vte-charsets.c"
    "${LIBTSM_ROOT}/src/tsm/tsm-vte.c"
)
target_include_directories(sakura_libtsm
    PUBLIC
        "${LIBTSM_ROOT}/src"
    PRIVATE
        "${LIBTSM_ROOT}/src/tsm"
        "${LIBTSM_ROOT}/src/shared"
        "${LIBTSM_ROOT}/external"
        "${LIBTSM_ROOT}/external/wcwidth"
)
target_compile_definitions(sakura_libtsm PRIVATE
    _GNU_SOURCE
    _POSIX_C_SOURCE=200809L
)
target_compile_features(sakura_libtsm PRIVATE c_std_99)

add_executable(sakura-wx-prototype
    src/main.cpp
    src/pty_session.cpp
)
target_include_directories(sakura-wx-prototype PRIVATE src)
target_link_libraries(sakura-wx-prototype PRIVATE
    wx::core
    sakura_libtsm
    Threads::Threads
)

if(UNIX)
    target_compile_definitions(sakura-wx-prototype PRIVATE SAKURA_WX_POSIX=1)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # forkpty() lives in libutil on Linux.
    target_link_libraries(sakura-wx-prototype PRIVATE util)
endif()

if(MSVC)
    target_compile_options(sakura-wx-prototype PRIVATE /W4)
else()
    target_compile_options(sakura-wx-prototype PRIVATE -Wall -Wextra -Wpedantic)
endif()
