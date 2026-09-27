# Auto-generated CMakeLists.txt for us_2dsa

set(src_dir ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(us_2dsa
    ${src_dir}/us_plot_control_2d.cpp
    ${src_dir}/us_2dsa.cpp
    ${src_dir}/us_adv_analysis_2d.cpp
    ${src_dir}/us_worker_calcnorm.cpp
    ${src_dir}/us_2dsa_process.cpp
    ${src_dir}/us_resplot_2d.cpp
    ${src_dir}/us_worker_2d.cpp
    ${src_dir}/us_show_norm.cpp
    ${src_dir}/us_grid_view_2d.cpp
    ${src_dir}/us_optimality_2d.cpp
    ${src_dir}/us_sim_cache_2d.cpp
    ${src_dir}/us_analysis_control_2d.cpp
)

target_link_libraries(us_2dsa
    PRIVATE
        us_program_common
)

target_include_directories(us_2dsa
    PRIVATE
        ${src_dir}
)

# Headless benchmark of subgrid partitions (see bench/README.md)
option(US3_BUILD_2DSA_BENCH "Build the us_2dsa_bench subgrid benchmark" OFF)
if(US3_BUILD_2DSA_BENCH)
    set(astfem_sim_dir ${CMAKE_CURRENT_SOURCE_DIR}/../us_astfem_sim)

    add_executable(us_2dsa_bench
        ${src_dir}/bench/us_2dsa_bench.cpp
        ${src_dir}/us_2dsa_process.cpp
        ${src_dir}/us_worker_2d.cpp
        ${src_dir}/us_sim_cache_2d.cpp
        ${astfem_sim_dir}/us_astfem_sim.cpp
        ${astfem_sim_dir}/us_clipdata.cpp
    )

    # us_astfem_sim's class is used for simulations; its own main() is renamed
    set_source_files_properties(${astfem_sim_dir}/us_astfem_sim.cpp
        PROPERTIES COMPILE_DEFINITIONS "main=us_astfem_sim_main")

    target_link_libraries(us_2dsa_bench
        PRIVATE
            us_program_common
    )

    target_include_directories(us_2dsa_bench
        PRIVATE
            ${src_dir}
            ${astfem_sim_dir}
    )
endif()
