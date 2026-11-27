# CMakeLists.txt for us_data_publication
#
# The program is one executable with two front ends: the window that opens
# when it is started without arguments, and the command line that runs when
# --mode, --no-ui or --help is given.  The records-and-bundles half of the
# program has no GUI dependency, so the unit tests under
# test/programs/us_data_publication compile the same sources directly.

set(src_dir ${CMAKE_CURRENT_SOURCE_DIR})

# Sources that do not depend on the GUI library, shared with the tests
include(${src_dir}/us_datapub_sources.cmake)

add_executable(us_data_publication
    ${src_dir}/us_data_publication.cpp
    ${src_dir}/us_datapub_export_pane.cpp
    ${src_dir}/us_datapub_import_pane.cpp
    ${US_DATAPUB_CORE_SOURCES}
)

target_link_libraries(us_data_publication
    PRIVATE
        us_program_common
)

target_include_directories(us_data_publication
    PRIVATE
        ${src_dir}
)
