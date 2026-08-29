# us_3dsa_cli -- headless 3DSA fit driver and test-case generator.
#
# Deliberately linked against usutils and Qt Core only, not against
# us_program_common: it carries no GUI dependency, so it builds in the
# headless HPC profile as well as the full application build.

set(src_dir ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(us_3dsa_cli
    ${src_dir}/us_3dsa_cli.cpp
    ${src_dir}/us_3dsa_cli.h
)

target_include_directories(us_3dsa_cli
    PRIVATE
        ${src_dir}
        ${CMAKE_SOURCE_DIR}/utils
        ${CMAKE_BINARY_DIR}
)

if(US3_PREFER_STATIC AND TARGET UltraScan3::Utils_Static)
    target_link_libraries(us_3dsa_cli PRIVATE UltraScan3::Utils_Static)
elseif(TARGET UltraScan3::Utils)
    target_link_libraries(us_3dsa_cli PRIVATE UltraScan3::Utils)
else()
    message(FATAL_ERROR "[us_3dsa_cli] no UltraScan3 Utils target found")
endif()

target_link_libraries(us_3dsa_cli
    PRIVATE
        Qt::Core
        Qt::Network
        Qt::Xml
        Qt::Sql
)

if(UNIX AND NOT APPLE)
    target_link_libraries(us_3dsa_cli PRIVATE OpenSSL::Crypto)
endif()

if(TARGET UltraScan3::Resources)
    if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.24)
        target_link_libraries(us_3dsa_cli
                PRIVATE $<LINK_LIBRARY:WHOLE_ARCHIVE,UltraScan3::Resources>)
    else()
        target_link_libraries(us_3dsa_cli PRIVATE UltraScan3::Resources)
    endif()
endif()
