cmake_minimum_required(VERSION 3.20)

project(sakura_wx_prototype VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)
include(CTest)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

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
set_target_properties(sakura_libtsm PROPERTIES EXPORT_NAME LibTsm)
target_include_directories(sakura_libtsm
    PUBLIC
        $<BUILD_INTERFACE:${LIBTSM_ROOT}/src>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
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
target_compile_definitions(sakura_libtsm PUBLIC TSM_STATIC)
target_compile_features(sakura_libtsm PRIVATE c_std_99)

add_library(sakura_terminal_core STATIC
    src/terminal_core.cpp
    src/tsm_scroll_observer.c
)
set_target_properties(sakura_terminal_core PROPERTIES EXPORT_NAME TerminalCoreC)
target_include_directories(sakura_terminal_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        "${LIBTSM_ROOT}/src/tsm"
        "${LIBTSM_ROOT}/src/shared"
        "${LIBTSM_ROOT}/external"
        "${LIBTSM_ROOT}/external/wcwidth"
)
target_link_libraries(sakura_terminal_core PUBLIC sakura_libtsm)

add_library(sakura_transport STATIC
    src/conpty_session.cpp
    src/pty_session.cpp
    src/transport_factory.cpp
)
set_target_properties(sakura_transport PROPERTIES EXPORT_NAME TerminalTransport)
target_include_directories(sakura_transport
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_link_libraries(sakura_transport PUBLIC Threads::Threads)

add_library(sakura_wx_terminal STATIC
    src/wx/terminal_ctrl.cpp
)
set_target_properties(sakura_wx_terminal PROPERTIES EXPORT_NAME WxTerminal)
get_target_property(SAKURA_WX_CORE_INCLUDES wxcore INTERFACE_INCLUDE_DIRECTORIES)
get_target_property(SAKURA_WX_CORE_DEFINITIONS wxcore INTERFACE_COMPILE_DEFINITIONS)
target_include_directories(sakura_wx_terminal
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${SAKURA_WX_CORE_INCLUDES}
        "${LIBTSM_ROOT}/external"
)
target_compile_definitions(sakura_wx_terminal PRIVATE ${SAKURA_WX_CORE_DEFINITIONS})
target_link_libraries(sakura_wx_terminal
    PUBLIC
        sakura_terminal_core
        $<BUILD_INTERFACE:wx::core>
)

add_library(Sakura::TerminalCoreC ALIAS sakura_terminal_core)
add_library(Sakura::TerminalTransport ALIAS sakura_transport)
add_library(Sakura::WxTerminal ALIAS sakura_wx_terminal)

if(UNIX)
    target_compile_definitions(sakura_transport PRIVATE SAKURA_WX_POSIX=1)
endif()

if(WIN32)
    target_compile_definitions(sakura_transport PRIVATE
        _WIN32_WINNT=0x0A00
        WINVER=0x0A00
    )
    target_link_libraries(sakura_transport PUBLIC kernel32)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # forkpty() lives in libutil on Linux.
    target_link_libraries(sakura_transport PUBLIC util)
endif()

add_executable(sakura-wx-prototype
    src/main.cpp
)
if(WIN32)
    set_target_properties(sakura-wx-prototype PROPERTIES
        WIN32_EXECUTABLE TRUE
    )
endif()
target_include_directories(sakura-wx-prototype PRIVATE
    src
    "${LIBTSM_ROOT}/external"
)
target_link_libraries(sakura-wx-prototype PRIVATE
    wx::core
    sakura_wx_terminal
    sakura_transport
)

if(MSVC)
    target_compile_options(sakura-wx-prototype PRIVATE /W4)
else()
    target_compile_options(sakura-wx-prototype PRIVATE -Wall -Wextra -Wpedantic)
endif()

option(SAKURA_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

option(SAKURA_BUILD_BENCHMARKS "Build terminal rendering benchmarks" OFF)
if(SAKURA_BUILD_BENCHMARKS)
    add_executable(sakura-terminal-core-benchmark
        benchmarks/terminal_core_benchmark.cpp
    )
    target_link_libraries(sakura-terminal-core-benchmark PRIVATE
        sakura_terminal_core
    )
    target_compile_features(sakura-terminal-core-benchmark PRIVATE cxx_std_17)
    if(MSVC)
        target_compile_options(sakura-terminal-core-benchmark PRIVATE /W4)
    else()
        target_compile_options(sakura-terminal-core-benchmark PRIVATE -Wall -Wextra -Wpedantic)
    endif()

    add_executable(sakura-tsm-render-benchmark
        benchmarks/tsm_render_benchmark.cpp
    )
    target_link_libraries(sakura-tsm-render-benchmark PRIVATE sakura_libtsm)
    target_compile_features(sakura-tsm-render-benchmark PRIVATE cxx_std_17)
    if(MSVC)
        target_compile_options(sakura-tsm-render-benchmark PRIVATE /W4)
    else()
        target_compile_options(sakura-tsm-render-benchmark PRIVATE -Wall -Wextra -Wpedantic)
    endif()

    add_executable(sakura-wx-paint-benchmark
        benchmarks/wx_paint_benchmark.cpp
    )
    target_link_libraries(sakura-wx-paint-benchmark PRIVATE
        wx::core
        sakura_wx_terminal
    )
    target_compile_features(sakura-wx-paint-benchmark PRIVATE cxx_std_17)
    if(MSVC)
        target_compile_options(sakura-wx-paint-benchmark PRIVATE /W4)
    else()
        target_compile_options(sakura-wx-paint-benchmark PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endif()

if(SAKURA_ENABLE_SANITIZERS AND NOT MSVC)
    function(sakura_enable_sanitizers target)
        target_compile_options(${target} PRIVATE -fsanitize=address,undefined
                                                  -fno-omit-frame-pointer)
        target_link_options(${target} PRIVATE -fsanitize=address,undefined)
    endfunction()

    sakura_enable_sanitizers(sakura_libtsm)
    sakura_enable_sanitizers(sakura_terminal_core)
    sakura_enable_sanitizers(sakura_transport)
    sakura_enable_sanitizers(sakura_wx_terminal)
    sakura_enable_sanitizers(sakura-wx-prototype)
    if(TARGET sakura-terminal-core-benchmark)
        sakura_enable_sanitizers(sakura-terminal-core-benchmark)
    endif()
endif()

install(TARGETS sakura_libtsm sakura_terminal_core sakura_transport
                sakura_wx_terminal
    EXPORT SakuraTerminalTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT SakuraTerminalTargets
    FILE SakuraTerminalTargets.cmake
    NAMESPACE Sakura::
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/SakuraTerminal"
)

configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/SakuraTerminalConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/SakuraTerminalConfig.cmake"
    INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/SakuraTerminal"
)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/SakuraTerminalConfigVersion.cmake"
    COMPATIBILITY SameMajorVersion
)
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/SakuraTerminalConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/SakuraTerminalConfigVersion.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/SakuraTerminal"
)

if(BUILD_TESTING)
    add_executable(sakura_terminal_core_test
        tests/terminal_core_test.cpp
    )
    target_include_directories(sakura_terminal_core_test PRIVATE
        "${LIBTSM_ROOT}/external"
    )
    target_link_libraries(sakura_terminal_core_test PRIVATE sakura_terminal_core)
    add_test(NAME terminal_core COMMAND sakura_terminal_core_test)

    add_executable(sakura-vt-replay
        tests/vt_replay.cpp
    )
    target_link_libraries(sakura-vt-replay PRIVATE sakura_terminal_core)
    add_test(NAME vt_replay
        COMMAND sakura-vt-replay
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/basic.vtlog"
    )
    set_tests_properties(vt_replay PROPERTIES TIMEOUT 10)

    add_executable(sakura_library_consumer_test
        tests/library_consumer_test.cpp
    )
    target_link_libraries(sakura_library_consumer_test PRIVATE
        Sakura::WxTerminal
        Sakura::TerminalTransport
    )
    add_test(NAME library_consumer COMMAND sakura_library_consumer_test)

    add_executable(sakura_c_core_consumer_test
        tests/c_core_consumer_test.c
    )
    set_target_properties(sakura_c_core_consumer_test PROPERTIES
        LINKER_LANGUAGE CXX
    )
    target_include_directories(sakura_c_core_consumer_test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    )
    target_link_libraries(sakura_c_core_consumer_test PRIVATE
        sakura_terminal_core
    )
    add_test(NAME c_core_consumer COMMAND sakura_c_core_consumer_test)

    add_executable(sakura_transport_smoke_test
        tests/transport_smoke_test.cpp
    )
    target_link_libraries(sakura_transport_smoke_test PRIVATE sakura_transport)
    add_test(NAME transport_smoke COMMAND sakura_transport_smoke_test)
    set_tests_properties(transport_smoke PROPERTIES TIMEOUT 15)

    add_executable(sakura_transport_lifecycle_test
        tests/transport_lifecycle_test.cpp
    )
    target_link_libraries(sakura_transport_lifecycle_test PRIVATE sakura_transport)
    add_test(NAME transport_lifecycle COMMAND sakura_transport_lifecycle_test)
    set_tests_properties(transport_lifecycle PROPERTIES TIMEOUT 10)

    if(UNIX)
        add_executable(sakura_pty_stress_test
            tests/pty_stress_test.cpp
        )
        target_link_libraries(sakura_pty_stress_test PRIVATE sakura_transport)
        add_test(NAME pty_stress COMMAND sakura_pty_stress_test)
        set_tests_properties(pty_stress PROPERTIES TIMEOUT 15)
    endif()

    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        add_test(NAME wx_smoke
            COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/wx_smoke_test.sh"
                    "$<TARGET_FILE:sakura-wx-prototype>"
        )
        # GTK/Pango/Fontconfig retain process-global caches during shutdown;
        # the smoke test is for window construction and event-loop health.
        set_tests_properties(wx_smoke PROPERTIES
            TIMEOUT 10
            ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0"
        )
        add_test(NAME wx_ux_scenario
            COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/wx_smoke_test.sh"
                    "$<TARGET_FILE:sakura-wx-prototype>"
        )
        set_tests_properties(wx_ux_scenario PROPERTIES
            TIMEOUT 10
            ENVIRONMENT "SAKURA_WX_SMOKE_TEST=1;SAKURA_WX_SCENARIO_TEST=1;ASAN_OPTIONS=detect_leaks=0"
        )
        if(TARGET sakura-wx-paint-benchmark)
            add_test(NAME wx_paint_regression
                COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/wx_smoke_test.sh"
                        "$<TARGET_FILE:sakura-wx-paint-benchmark>"
                        --json
            )
            set_tests_properties(wx_paint_regression PROPERTIES
                TIMEOUT 30
                ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0"
            )
        endif()
    endif()

    if(SAKURA_ENABLE_SANITIZERS AND NOT MSVC)
        sakura_enable_sanitizers(sakura_terminal_core_test)
        sakura_enable_sanitizers(sakura-vt-replay)
        if(TARGET sakura_pty_stress_test)
            sakura_enable_sanitizers(sakura_pty_stress_test)
        endif()
        sakura_enable_sanitizers(sakura_transport_smoke_test)
        sakura_enable_sanitizers(sakura_transport_lifecycle_test)
        sakura_enable_sanitizers(sakura_library_consumer_test)
        sakura_enable_sanitizers(sakura_c_core_consumer_test)
    endif()
endif()
