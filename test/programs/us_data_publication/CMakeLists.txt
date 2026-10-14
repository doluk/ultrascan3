# CMakeLists.txt for us_data_publication tests

project(TestUSDataPublication)

# Use Qt version from parent (supports both Qt5 and Qt6)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Core Test)

# Google Test - only link gtest (not gtest_main) since tests define their own main()
if(TARGET gtest)
    set(GTEST_LIBRARIES gtest)
    message(STATUS "Using Google Test for data publication tests")
else()
    find_package(GTest QUIET)
    if(GTest_FOUND)
        set(GTEST_LIBRARIES GTest::gtest)
    else()
        set(GTEST_LIBRARIES gtest)
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
        Qt${QT_VERSION_MAJOR}::Core
        Qt${QT_VERSION_MAJOR}::Test
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
