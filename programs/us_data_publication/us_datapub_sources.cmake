# The us_data_publication sources that do not depend on the GUI library.
#
# Included by the program and by its unit tests
# (test/programs/us_data_publication), which compile exactly this list; the
# test tree is also built in profiles that do not add the program, so the
# list lives here rather than in a variable of the program's CMakeLists.

set(US_DATAPUB_SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR})

set(US_DATAPUB_CORE_SOURCES
    ${US_DATAPUB_SOURCE_DIR}/us_datapub_defs.cpp
    ${US_DATAPUB_SOURCE_DIR}/us_datapub_manifest.cpp
    ${US_DATAPUB_SOURCE_DIR}/us_datapub_hash.cpp
    ${US_DATAPUB_SOURCE_DIR}/us_datapub_bundle.cpp
    ${US_DATAPUB_SOURCE_DIR}/us_datapub_records.cpp
    ${US_DATAPUB_SOURCE_DIR}/us_datapub_catalog.cpp
    ${US_DATAPUB_SOURCE_DIR}/us_datapub_export.cpp
    ${US_DATAPUB_SOURCE_DIR}/us_datapub_import.cpp
    ${US_DATAPUB_SOURCE_DIR}/us_datapub_cli.cpp
)
