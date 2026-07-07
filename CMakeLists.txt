cmake_minimum_required(VERSION 3.21)
project(convex VERSION 0.1.0 LANGUAGES CXX)

if(PROJECT_IS_TOP_LEVEL)
    set(CONVEX_DEV_DEFAULT ON)
else()
    set(CONVEX_DEV_DEFAULT OFF)
endif()

option(CONVEX_BUILD_TESTS "Build convex unit tests" ${CONVEX_DEV_DEFAULT})
option(CONVEX_WITH_IXWEBSOCKET "Build the default IXWebSocket transport" OFF)
option(CONVEX_BUILD_INTEGRATION_TESTS "Build integration tests (require a live Convex backend)" OFF)
option(CONVEX_BUILD_EXAMPLES "Build example programs" OFF)

add_library(convex STATIC
    src/value.cpp
    src/detail/base64.cpp
    src/json_codec.cpp
    src/protocol.cpp
    src/base_client.cpp
)
add_library(convex::convex ALIAS convex)

target_compile_features(convex PUBLIC cxx_std_20)
set_target_properties(convex PROPERTIES CXX_EXTENSIONS OFF)
target_include_directories(convex
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

if(MSVC)
    target_compile_options(convex PRIVATE /W4 /utf-8 /permissive-)
else()
    target_compile_options(convex PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(CONVEX_BUILD_TESTS)
    enable_testing()
    include(FetchContent)
    FetchContent_Declare(
        googletest
        URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.zip
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    # Match the parent project's CRT on Windows.
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
    add_subdirectory(tests)
endif()
