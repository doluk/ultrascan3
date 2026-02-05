# Add the subdirectory for the test utilities
add_subdirectory(utils)

# Add data publication tests if directory exists
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/programs/us_data_publication/CMakeLists.txt")
    add_subdirectory(programs/us_data_publication)
endif()
