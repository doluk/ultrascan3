# Standalone build of the Layer-1 generator.
#
# This deliberately does NOT go through the top-level UltraScan CMakeLists:
# that pulls in OpenGL, qwt, qwtplot3d and X11 for the GUI programs, none of
# which the generator needs.  We build only libus_utils in its no-database
# (HPC) configuration, which is the smallest subset containing US_Astfem_RSA,
# US_Model, US_SimulationParameters, US_DataIO and US_Noise.
#
#   cmake -S . -B build && cmake --build build -j
#
# The upstream toolchain image (ghcr.io/ehb54/us3-toolchain-ubuntu2404) pins
# a vcpkg-built Qt.  This file instead resolves Qt6 however CMake finds it, so
# it also works against a distribution Qt.  Record which one you used: they
# are not the same Qt version.

cmake_minimum_required(VERSION 3.22)
project(us3_moment_generate CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Xml Network)
find_package(OpenSSL REQUIRED)
find_package(LibArchive REQUIRED)

set(US3_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." CACHE PATH "UltraScan3 source root")
file(GLOB UTILS_SOURCES "${US3_ROOT}/utils/*.cpp")
# Instrument/database components excluded by the no-DB profile.
list(REMOVE_ITEM UTILS_SOURCES
    "${US3_ROOT}/utils/us_cfa_data.cpp"
    "${US3_ROOT}/utils/us_xpn_data.cpp"
    "${US3_ROOT}/utils/us_link_ssl.cpp"
)

add_library(us_utils_nodb STATIC ${UTILS_SOURCES})

# Mirrors the platform definitions the top-level build applies on Linux
# (CMakeLists.txt: add_compile_definitions(LINUX PLATFORM="intel" TITLE="Intel")).
# us_defines.h derives OS/OS_TITLE from LINUX; without it us_license_t.cpp
# does not compile.
target_compile_definitions(us_utils_nodb PUBLIC
    NO_DB QT_SHARED LINUX PLATFORM="intel" TITLE="Intel")

# Only needed when Qt is older than the pinned 6.9; see qt_compat.h.
target_compile_options(us_utils_nodb PUBLIC
    -include "${CMAKE_CURRENT_SOURCE_DIR}/qt_compat.h")
target_include_directories(us_utils_nodb PUBLIC "${US3_ROOT}/utils")
target_link_libraries(us_utils_nodb PUBLIC
    Qt6::Core Qt6::Xml Qt6::Network
    OpenSSL::SSL OpenSSL::Crypto
    LibArchive::LibArchive
)

add_executable(generate generate.cpp)
target_link_libraries(generate PRIVATE us_utils_nodb)
