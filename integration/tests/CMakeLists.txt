add_executable(convex_integration_tests
    integration_test.cpp
)
target_link_libraries(convex_integration_tests PRIVATE
    convex::convex
    convex::ixwebsocket
    GTest::gtest_main
)
if(MSVC)
    target_compile_options(convex_integration_tests PRIVATE /utf-8)
endif()
# Not registered with ctest by default: these need a live backend
# (integration/backend: docker compose up -d).
