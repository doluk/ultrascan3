# CMakeLists.txt for us_data_publication

set(src_dir ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(us_data_publication
    ${src_dir}/us_data_publication_main.cpp
    ${src_dir}/us_data_publication.cpp
)

target_link_libraries(us_data_publication
    PRIVATE
        us_program_common
)

target_include_directories(us_data_publication
    PRIVATE
        ${src_dir}
)
