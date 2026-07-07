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
    src/client.cpp
    src/http_client.cpp
    src/file_storage.cpp
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

if(CONVEX_WITH_IXWEBSOCKET)
    include(FetchContent)

    # MbedTLS from source so the transport is self-contained (no system TLS).
    set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
    set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
    set(GEN_FILES OFF CACHE BOOL "" FORCE)
    set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        mbedtls
        URL https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.2/mbedtls-3.6.2.tar.bz2
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(mbedtls)

    # IXWebSocket picks these up instead of running its FindMbedTLS module.
    set(USE_TLS ON CACHE BOOL "" FORCE)
    set(USE_MBED_TLS ON CACHE BOOL "" FORCE)
    set(USE_ZLIB OFF CACHE BOOL "" FORCE)
    set(IXWEBSOCKET_INSTALL OFF CACHE BOOL "" FORCE)
    set(MBEDTLS_FOUND TRUE)
    set(MBEDTLS_INCLUDE_DIRS ${mbedtls_SOURCE_DIR}/include)
    set(MBEDTLS_LIBRARIES mbedtls mbedx509 mbedcrypto)
    FetchContent_Declare(
        ixwebsocket
        URL https://github.com/machinezone/IXWebSocket/archive/refs/tags/v11.4.6.tar.gz
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(ixwebsocket)
    # Set manually because we bypassed FindMbedTLS (mbedtls 3.x API differs).
    target_compile_definitions(ixwebsocket PRIVATE IXWEBSOCKET_USE_MBED_TLS_MIN_VERSION_3)

    add_library(convex_ixwebsocket STATIC
        transports/ixwebsocket/ixwebsocket_transport.cpp
    )
    add_library(convex::ixwebsocket ALIAS convex_ixwebsocket)
    target_link_libraries(convex_ixwebsocket PUBLIC convex::convex PRIVATE ixwebsocket)
    target_include_directories(convex_ixwebsocket
        PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/transports>
    )
    if(MSVC)
        target_compile_options(convex_ixwebsocket PRIVATE /W4 /utf-8 /permissive-)
    endif()
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

if(CONVEX_BUILD_EXAMPLES)
    if(NOT CONVEX_WITH_IXWEBSOCKET)
        message(FATAL_ERROR "CONVEX_BUILD_EXAMPLES requires CONVEX_WITH_IXWEBSOCKET")
    endif()
    add_executable(chat_demo examples/chat_demo.cpp)
    target_link_libraries(chat_demo PRIVATE convex::convex convex::ixwebsocket)
    if(MSVC)
        target_compile_options(chat_demo PRIVATE /utf-8)
    endif()
endif()

if(CONVEX_BUILD_INTEGRATION_TESTS)
    if(NOT CONVEX_WITH_IXWEBSOCKET OR NOT CONVEX_BUILD_TESTS)
        message(FATAL_ERROR "CONVEX_BUILD_INTEGRATION_TESTS requires CONVEX_WITH_IXWEBSOCKET and CONVEX_BUILD_TESTS")
    endif()
    add_subdirectory(integration/tests)
endif()
