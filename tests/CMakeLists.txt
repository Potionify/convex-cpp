add_executable(convex_tests
    value_json_test.cpp
    protocol_test.cpp
    base_client_test.cpp
)
target_link_libraries(convex_tests PRIVATE convex::convex GTest::gtest_main)
# Tests may exercise internal (src/detail) helpers directly.
target_include_directories(convex_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party
)
if(MSVC)
    target_compile_options(convex_tests PRIVATE /utf-8)
endif()

include(GoogleTest)
gtest_discover_tests(convex_tests)
