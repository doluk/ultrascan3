# CMakeLists.txt for us_data_publication tests

project(TestUSDataPublication)

# Find Qt5 packages
find_package(Qt5 REQUIRED COMPONENTS Core Test)

# Google Test
if(TARGET gtest)
    set(GTEST_LIBRARIES gtest gtest_main)
    message(STATUS "Using Google Test for data publication tests")
else()
    find_package(GTest QUIET)
    if(GTest_FOUND)
        set(GTEST_LIBRARIES GTest::gtest GTest::gtest_main)
    else()
        set(GTEST_LIBRARIES gtest gtest_main)
    endif()
endif()

# Add test executable
add_executable(test_us_data_publication
    test_us_data_publication.cpp
)

target_include_directories(test_us_data_publication
    PRIVATE
        ${CMAKE_SOURCE_DIR}/programs/us_data_publication
        ${CMAKE_SOURCE_DIR}/utils
        ${CMAKE_SOURCE_DIR}/gui
)

target_link_libraries(test_us_data_publication
    PRIVATE
        Qt5::Core
        Qt5::Test
        ${GTEST_LIBRARIES}
)

# Register tests with CTest
include(GoogleTest)
gtest_discover_tests(test_us_data_publication
    PROPERTIES
        LABELS "gtest;unit;data_publication"
        TIMEOUT 60
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)
