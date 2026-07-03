# Optional headless driver for the ASTFVM instrumentation & convergence
# controls. Not built by default; opt in with:
#   cmake -DBUILD_ASTFVM_CONVERGENCE_DRIVER=ON ...
# Requires the us_utils target (see ../../CMakeLists.txt).

if(BUILD_ASTFVM_CONVERGENCE_DRIVER)
    find_package(Qt5 COMPONENTS Core REQUIRED)

    add_executable(astfvm_convergence_driver astfvm_convergence_driver.cpp)
    target_link_libraries(astfvm_convergence_driver PRIVATE us_utils Qt5::Core)
    target_include_directories(astfvm_convergence_driver PRIVATE
            ${CMAKE_SOURCE_DIR}/utils)
endif()
