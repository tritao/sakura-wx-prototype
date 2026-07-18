cmake_minimum_required(VERSION 3.20)

project(SakuraTerminalPackageConsumer LANGUAGES CXX)

if(NOT DEFINED SAKURA_SOURCE_DIR OR "${SAKURA_SOURCE_DIR}" STREQUAL "")
    message(FATAL_ERROR "SAKURA_SOURCE_DIR is required")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(SakuraTerminal CONFIG REQUIRED)

add_executable(sakura-wx-installed-consumer
    "${SAKURA_SOURCE_DIR}/src/main.cpp"
)
target_link_libraries(sakura-wx-installed-consumer PRIVATE
    Sakura::WxTerminal
    Sakura::TerminalTransport
)

if(WIN32)
    set_target_properties(sakura-wx-installed-consumer PROPERTIES
        WIN32_EXECUTABLE TRUE
    )
endif()
