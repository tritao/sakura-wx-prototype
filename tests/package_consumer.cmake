cmake_minimum_required(VERSION 3.20)

foreach(required_variable IN ITEMS
        SAKURA_SOURCE_DIR
        SAKURA_BUILD_DIR
        SAKURA_CONSUMER_SOURCE_DIR
        SAKURA_CONSUMER_BINARY_DIR
        SAKURA_INSTALL_PREFIX
        SAKURA_GENERATOR)
    if(NOT DEFINED ${required_variable} OR
       "${${required_variable}}" STREQUAL "")
        message(FATAL_ERROR "${required_variable} is required")
    endif()
endforeach()

file(REMOVE_RECURSE
    "${SAKURA_CONSUMER_BINARY_DIR}"
    "${SAKURA_INSTALL_PREFIX}")

set(install_command
    "${CMAKE_COMMAND}"
    --install "${SAKURA_BUILD_DIR}"
    --prefix "${SAKURA_INSTALL_PREFIX}")
if(DEFINED SAKURA_CONFIG AND NOT "${SAKURA_CONFIG}" STREQUAL "")
    list(APPEND install_command --config "${SAKURA_CONFIG}")
endif()
execute_process(
    COMMAND ${install_command}
    RESULT_VARIABLE install_result
    OUTPUT_VARIABLE install_output
    ERROR_VARIABLE install_error
)
if(NOT install_result EQUAL 0)
    message(FATAL_ERROR
        "Package install failed (${install_result})\n"
        "${install_output}\n${install_error}")
endif()

set(configure_command
    "${CMAKE_COMMAND}"
    -S "${SAKURA_CONSUMER_SOURCE_DIR}"
    -B "${SAKURA_CONSUMER_BINARY_DIR}"
    -G "${SAKURA_GENERATOR}"
    "-DCMAKE_PREFIX_PATH=${SAKURA_INSTALL_PREFIX}"
    "-DSAKURA_SOURCE_DIR=${SAKURA_SOURCE_DIR}")
if(DEFINED SAKURA_CONFIG AND NOT "${SAKURA_CONFIG}" STREQUAL "")
    list(APPEND configure_command
        "-DCMAKE_BUILD_TYPE=${SAKURA_CONFIG}")
endif()
if(DEFINED SAKURA_GENERATOR_PLATFORM AND
   NOT "${SAKURA_GENERATOR_PLATFORM}" STREQUAL "")
    list(APPEND configure_command
        -A "${SAKURA_GENERATOR_PLATFORM}")
endif()
if(DEFINED SAKURA_GENERATOR_TOOLSET AND
   NOT "${SAKURA_GENERATOR_TOOLSET}" STREQUAL "")
    list(APPEND configure_command
        -T "${SAKURA_GENERATOR_TOOLSET}")
endif()
execute_process(
    COMMAND ${configure_command}
    RESULT_VARIABLE configure_result
    OUTPUT_VARIABLE configure_output
    ERROR_VARIABLE configure_error
)
if(NOT configure_result EQUAL 0)
    message(FATAL_ERROR
        "Package consumer configure failed (${configure_result})\n"
        "${configure_output}\n${configure_error}")
endif()

set(build_command
    "${CMAKE_COMMAND}"
    --build "${SAKURA_CONSUMER_BINARY_DIR}")
if(DEFINED SAKURA_CONFIG AND NOT "${SAKURA_CONFIG}" STREQUAL "")
    list(APPEND build_command --config "${SAKURA_CONFIG}")
endif()
execute_process(
    COMMAND ${build_command}
    RESULT_VARIABLE build_result
    OUTPUT_VARIABLE build_output
    ERROR_VARIABLE build_error
)
if(NOT build_result EQUAL 0)
    message(FATAL_ERROR
        "Package consumer build failed (${build_result})\n"
        "${build_output}\n${build_error}")
endif()

message(STATUS "Installed package consumer built successfully")
