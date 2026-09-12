# Unit tests for us_data_publication
#
# The records-and-bundles half of us_data_publication has no GUI
# dependency, so it is compiled straight into the test binary.  That also
# keeps the tests honest: they exercise the very sources the program ships.

project(TestUSDataPublication)

set(datapub_dir ${CMAKE_SOURCE_DIR}/programs/us_data_publication)

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

set(DATAPUB_SOURCES
        ${datapub_dir}/us_datapub_defs.cpp
        ${datapub_dir}/us_datapub_manifest.cpp
        ${datapub_dir}/us_datapub_hash.cpp
        ${datapub_dir}/us_datapub_bundle.cpp
        ${datapub_dir}/us_datapub_records.cpp
        ${datapub_dir}/us_datapub_catalog.cpp
        ${datapub_dir}/us_datapub_export.cpp
        ${datapub_dir}/us_datapub_import.cpp
        ${datapub_dir}/us_datapub_cli.cpp
        )

file(GLOB DATAPUB_TEST_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/test_us_datapub_*.cpp")
list(REMOVE_ITEM DATAPUB_TEST_SOURCES
        "${CMAKE_CURRENT_SOURCE_DIR}/test_us_datapub_main.cpp")

add_executable(test_us_datapub
        ${CMAKE_CURRENT_SOURCE_DIR}/test_us_datapub_main.cpp
        ${DATAPUB_TEST_SOURCES}
        ${DATAPUB_SOURCES}
        )

target_include_directories(test_us_datapub
        PRIVATE
        ${datapub_dir}
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/test/common
        ${CMAKE_SOURCE_DIR}/utils
        ${CMAKE_BINARY_DIR}
        )

if(TARGET UltraScan3::Utils_Static)
    set(US_UTILS_LINK_TARGET UltraScan3::Utils_Static)
else()
    set(US_UTILS_LINK_TARGET UltraScan3::Utils)
endif()

target_link_libraries(test_us_datapub
        PRIVATE
        ${US_UTILS_LINK_TARGET}
        Qt::Core
        Qt::Network
        Qt::Sql
        Qt::Xml
        ${GTEST_LIBRARIES}
        )

if(TARGET MySQL::Client AND NOT US3_NO_DB)
    target_link_libraries(test_us_datapub PRIVATE MySQL::Client)
endif()

if(UNIX AND NOT APPLE)
    target_link_libraries(test_us_datapub PRIVATE OpenSSL::Crypto)
endif()

include(GoogleTest)
gtest_discover_tests(test_us_datapub
        PROPERTIES
        LABELS "gtest;unit;programs;us_data_publication"
        TIMEOUT 120
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        )

add_custom_target(test_data_publication
        COMMAND ${CMAKE_CTEST_COMMAND} -L us_data_publication --output-on-failure
        DEPENDS test_us_datapub
        COMMENT "Running us_data_publication tests only"
        )
