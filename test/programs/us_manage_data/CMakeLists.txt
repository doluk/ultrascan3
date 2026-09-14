# Unit tests for us_manage_data
#
# The tests compile the program's own data model, so they exercise the very
# sources the program ships.  US_DataModel reaches into us_gui for the
# widgets it reports progress on, so the tests link against it.

project(TestUSManageData)

set(manage_dir ${CMAKE_SOURCE_DIR}/programs/us_manage_data)

if(TARGET gtest)
    set(GTEST_LIBRARIES gtest gtest_main gmock gmock_main)
else()
    find_package(GTest QUIET)
    if(GTest_FOUND)
        set(GTEST_LIBRARIES GTest::gtest GTest::gtest_main GTest::gmock GTest::gmock_main)
    else()
        set(GTEST_LIBRARIES gtest gtest_main gmock gmock_main)
    endif()
endif()

set(MANAGE_SOURCES
        ${manage_dir}/us_data_model.cpp
        ${manage_dir}/us_data_model.h
        ${manage_dir}/us_data_process.cpp
        ${manage_dir}/us_data_process.h
        ${manage_dir}/us_data_tree.cpp
        ${manage_dir}/us_data_tree.h
        ${manage_dir}/us_sync_exper.cpp
        ${manage_dir}/us_sync_exper.h
        )

file(GLOB MANAGE_TEST_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/test_us_*.cpp")
list(REMOVE_ITEM MANAGE_TEST_SOURCES
        "${CMAKE_CURRENT_SOURCE_DIR}/test_us_manage_data_main.cpp")

add_executable(test_us_manage_data
        ${CMAKE_CURRENT_SOURCE_DIR}/test_us_manage_data_main.cpp
        ${MANAGE_TEST_SOURCES}
        ${MANAGE_SOURCES}
        )

target_include_directories(test_us_manage_data
        PRIVATE
        ${manage_dir}
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/test/common
        ${CMAKE_SOURCE_DIR}/utils
        ${CMAKE_SOURCE_DIR}/gui
        ${CMAKE_BINARY_DIR}
        )

if(TARGET UltraScan3::Utils_Static)
    set(US_UTILS_LINK_TARGET UltraScan3::Utils_Static)
else()
    set(US_UTILS_LINK_TARGET UltraScan3::Utils)
endif()

if(TARGET UltraScan3::Gui_Static)
    set(US_GUI_LINK_TARGET UltraScan3::Gui_Static)
else()
    set(US_GUI_LINK_TARGET UltraScan3::Gui)
endif()

target_link_libraries(test_us_manage_data
        PRIVATE
        ${US_GUI_LINK_TARGET}
        ${US_UTILS_LINK_TARGET}
        Qt::Core
        Qt::Gui
        Qt::Network
        Qt::Sql
        Qt::Widgets
        Qt::Xml
        ${GTEST_LIBRARIES}
        )

if(TARGET MySQL::Client AND NOT US3_NO_DB)
    target_link_libraries(test_us_manage_data PRIVATE MySQL::Client)
endif()

include(GoogleTest)
gtest_discover_tests(test_us_manage_data
        PROPERTIES
        LABELS "gtest;unit;programs;us_manage_data"
        TIMEOUT 120
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        )

add_custom_target(test_manage_data
        COMMAND ${CMAKE_CTEST_COMMAND} -L us_manage_data --output-on-failure
        DEPENDS test_us_manage_data
        COMMENT "Running us_manage_data tests only"
        )
