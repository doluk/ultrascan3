# =============================================================================
# Us3Install.cmake - install layout, options and helpers for UltraScan3
#
# Layout (relocatable, identical to the deploy_* staging trees):
#
#   <prefix>/
#     bin/                 us, us_*, SOMO programs, help files (Windows: + DLLs)
#     lib/                 libus_utils*, libus_gui*   (+ bundled deps, static libs)
#     lib/cmake/UltraScan3 CMake package files            (US3_INSTALL_DEVEL)
#     include/ultrascan3/  utils/ and gui/ headers        (US3_INSTALL_DEVEL)
#     etc/                 runtime data (US_Settings::appBaseDir() + "/etc")
#     somo/                SOMO data
#     license.txt, VERSION
#
# The directory names are fixed: the programs locate their data relative to
# their own bin/ directory, and the RPATHs point at ../lib.
#
# Install components:
#   Runtime        programs, shared libraries, bundled runtime dependencies
#   Data           etc/, somo/, license, VERSION
#   Documentation  Qt help files (manual.qch / manual.qhc)
#   SOMO           SOMO programs and helpers (US3_BUILD_SOMO)
#   Development    headers, static libraries, CMake package (US3_INSTALL_DEVEL)
#
#   cmake --install <build> [--prefix <dir>] [--component <name>]
# =============================================================================

option(US3_INSTALL_RUNTIME_DEPS
       "cmake --install also bundles Qt, Qwt, MariaDB, OpenSSL ... (runs the platform deploy script)"
       ON)
option(US3_INSTALL_DEVEL
       "Install headers, static libraries and the UltraScan3 CMake package"
       OFF)

set(US3_INSTALL_BINDIR     "bin")
set(US3_INSTALL_LIBDIR     "lib")
set(US3_INSTALL_INCLUDEDIR "include/ultrascan3")
set(US3_INSTALL_CMAKEDIR   "lib/cmake/UltraScan3")
set(US3_INSTALL_ETCDIR     "etc")
set(US3_INSTALL_SOMODIR    "somo")

# -----------------------------------------------------------------------------
# RPATH of installed binaries
#   Linux: $ORIGIN/../lib (set in the root CMakeLists.txt)
#   macOS: @loader_path/../lib for bare executables and dylibs
# Without bundled dependencies (or on macOS, where macdeployqt has to resolve
# the dependencies of the installed binaries) the linker search path is kept,
# so non-system libraries (vcpkg, Homebrew) are still found.
# -----------------------------------------------------------------------------
if(APPLE)
    set(CMAKE_INSTALL_RPATH "@loader_path/../${US3_INSTALL_LIBDIR}")
endif()
if(APPLE OR NOT US3_INSTALL_RUNTIME_DEPS)
    set(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)
endif()

# -----------------------------------------------------------------------------
# us3_install_program(<target>...)
#   Installs executables (macOS bundles included) into bin/.
# -----------------------------------------------------------------------------
function(us3_install_program)
    foreach(_tgt IN LISTS ARGN)
        get_target_property(_type ${_tgt} TYPE)
        if(NOT _type STREQUAL "EXECUTABLE")
            continue()
        endif()
        install(TARGETS ${_tgt}
                RUNTIME DESTINATION ${US3_INSTALL_BINDIR} COMPONENT Runtime
                BUNDLE  DESTINATION ${US3_INSTALL_BINDIR} COMPONENT Runtime)
    endforeach()
endfunction()

# -----------------------------------------------------------------------------
# us3_install_programs_in_dir(<dir>...)
#   Installs every executable defined directly in the given subdirectories.
# -----------------------------------------------------------------------------
function(us3_install_programs_in_dir)
    foreach(_dir IN LISTS ARGN)
        get_property(_targets DIRECTORY "${_dir}" PROPERTY BUILDSYSTEM_TARGETS)
        us3_install_program(${_targets})
    endforeach()
endfunction()

# -----------------------------------------------------------------------------
# us3_install_library(<target> EXPORT_NAME <name>)
#   Shared library: runtime part -> Runtime, link part -> Development.
#   Static library: installed only with US3_INSTALL_DEVEL.
#   Both join the UltraScan3Targets export set as UltraScan3::<name>.
# -----------------------------------------------------------------------------
function(us3_install_library _tgt)
    cmake_parse_arguments(ARG "" "EXPORT_NAME" "" ${ARGN})
    if(ARG_EXPORT_NAME)
        set_target_properties(${_tgt} PROPERTIES EXPORT_NAME ${ARG_EXPORT_NAME})
    endif()
    get_target_property(_type ${_tgt} TYPE)
    if(_type STREQUAL "STATIC_LIBRARY")
        if(US3_INSTALL_DEVEL)
            install(TARGETS ${_tgt} EXPORT UltraScan3Targets
                    ARCHIVE DESTINATION ${US3_INSTALL_LIBDIR} COMPONENT Development)
        endif()
    else()
        install(TARGETS ${_tgt} EXPORT UltraScan3Targets
                RUNTIME DESTINATION ${US3_INSTALL_BINDIR} COMPONENT Runtime
                LIBRARY DESTINATION ${US3_INSTALL_LIBDIR} COMPONENT Runtime
                        NAMELINK_COMPONENT Development
                ARCHIVE DESTINATION ${US3_INSTALL_LIBDIR} COMPONENT Development)
    endif()
endfunction()

# -----------------------------------------------------------------------------
# us3_consumer_definitions(<target>)
#   The build sets the definitions the public headers depend on globally
#   (add_compile_definitions in the root CMakeLists.txt). Consumers of the
#   installed package get the same ones through the exported target.
# -----------------------------------------------------------------------------
function(us3_consumer_definitions _tgt)
    set(_defs "")
    if(US3_NO_DB)
        list(APPEND _defs NO_DB)
    endif()
    if(UNIX AND NOT APPLE)
        list(APPEND _defs LINUX "PLATFORM=\"intel\"" "TITLE=\"Intel\"")
    elseif(WIN32)
        list(APPEND _defs __LCC__ WIN32 "PLATFORM=\"intel\"" "TITLE=\"Intel\"")
        get_target_property(_type ${_tgt} TYPE)
        if(NOT _type STREQUAL "STATIC_LIBRARY")
            list(APPEND _defs US_MAKE_DLL)   # dllimport declarations
        endif()
    elseif(APPLE)
        list(APPEND _defs MAC OSX "PLATFORM=\"mac\"")
    endif()
    foreach(_d IN LISTS _defs)
        target_compile_definitions(${_tgt} INTERFACE "$<INSTALL_INTERFACE:${_d}>")
    endforeach()
endfunction()

# -----------------------------------------------------------------------------
# us3_install_headers(<subdir> <header>...)
#   -> include/ultrascan3/<subdir>  (US3_INSTALL_DEVEL)
# -----------------------------------------------------------------------------
function(us3_install_headers _subdir)
    if(US3_INSTALL_DEVEL)
        install(FILES ${ARGN}
                DESTINATION "${US3_INSTALL_INCLUDEDIR}/${_subdir}"
                COMPONENT Development)
    endif()
endfunction()

# -----------------------------------------------------------------------------
# us3_install_deploy_step(NAME <n> SCRIPT <file> STAGE_VAR <var> ARGS <-D...>...)
#   Runs a packaging deploy script (admin/cmake/packaging/...) at install time
#   with <var> = $DESTDIR<prefix>, i.e. it fills the install tree exactly as it
#   fills the _stage/ tree of the deploy_* targets: Qt libraries and plugins,
#   Qwt, MariaDB (+ auth plugins), OpenSSL, Qt Assistant, launchers.
#   Skipped when US3_INSTALL_RUNTIME_DEPS is OFF, or at install time with
#   -DUS3_SKIP_RUNTIME_DEPS=ON / environment variable US3_SKIP_RUNTIME_DEPS.
# -----------------------------------------------------------------------------
function(us3_install_deploy_step)
    cmake_parse_arguments(ARG "" "NAME;SCRIPT;STAGE_VAR" "ARGS" ${ARGN})
    if(NOT US3_INSTALL_RUNTIME_DEPS)
        message(STATUS "Install: runtime dependencies not bundled (US3_INSTALL_RUNTIME_DEPS=OFF)")
        return()
    endif()
    set(_args "")
    foreach(_a IN LISTS ARG_ARGS)
        string(APPEND _args " [==[${_a}]==]")
    endforeach()
    install(CODE "
set(_us3_stage \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}\")
if(US3_SKIP_RUNTIME_DEPS OR \"\$ENV{US3_SKIP_RUNTIME_DEPS}\")
    message(STATUS \"[US3] Runtime dependency deployment skipped (US3_SKIP_RUNTIME_DEPS)\")
else()
    message(STATUS \"[US3] Deploying runtime dependencies (${ARG_NAME}) into \${_us3_stage}\")
    execute_process(
        COMMAND [==[${CMAKE_COMMAND}]==] \"-D${ARG_STAGE_VAR}=\${_us3_stage}\"
                -DUS3_DEPLOY_INTO_INSTALL_TREE=ON ${_args}
                -P [==[${ARG_SCRIPT}]==]
        RESULT_VARIABLE _us3_rc)
    if(NOT _us3_rc EQUAL 0)
        message(FATAL_ERROR \"[US3] ${ARG_NAME} deployment failed (\${_us3_rc})\")
    endif()
endif()
" COMPONENT Runtime)
    message(STATUS "Install: runtime dependencies bundled by ${ARG_NAME} (US3_INSTALL_RUNTIME_DEPS=ON)")
endfunction()

# -----------------------------------------------------------------------------
# Package config support (US3_INSTALL_DEVEL)
#
# Dependencies that were not found as CMake packages (system Qwt / MariaDB via
# pkg-config, GL2PS) exist only as imported targets created in this project.
# us3_imported_target_code() writes them out so that the installed package
# re-creates the same targets for consumers.
# -----------------------------------------------------------------------------
function(us3_imported_target_code _out _tgt)
    set(_code "")
    get_target_property(_real ${_tgt} ALIASED_TARGET)
    if(NOT _real)
        set(_real ${_tgt})
    endif()
    get_target_property(_type ${_real} TYPE)
    if(_type STREQUAL "INTERFACE_LIBRARY")
        set(_kind INTERFACE)
    else()
        set(_kind UNKNOWN)
    endif()
    string(APPEND _code "if(NOT TARGET ${_real})\n"
                        "    add_library(${_real} ${_kind} IMPORTED)\n")
    foreach(_prop IN ITEMS IMPORTED_LOCATION INTERFACE_INCLUDE_DIRECTORIES
                           INTERFACE_LINK_LIBRARIES INTERFACE_LINK_DIRECTORIES
                           INTERFACE_COMPILE_DEFINITIONS INTERFACE_COMPILE_OPTIONS)
        if(_kind STREQUAL "INTERFACE" AND _prop STREQUAL "IMPORTED_LOCATION")
            continue()
        endif()
        get_target_property(_val ${_real} ${_prop})
        if(_val)
            string(APPEND _code "    set_property(TARGET ${_real} PROPERTY ${_prop} [==[${_val}]==])\n")
        endif()
    endforeach()
    string(APPEND _code "endif()\n")
    set(${_out} "${_code}" PARENT_SCOPE)
endfunction()

# Qwt: vcpkg config package or a locally created target
function(us3_qwt_dependency_code _out)
    if(unofficial-qwt_FOUND)
        set(${_out} "find_dependency(unofficial-qwt CONFIG)\n" PARENT_SCOPE)
    elseif(TARGET Qwt::Qwt)
        us3_imported_target_code(_c Qwt::Qwt)
        set(${_out} "${_c}" PARENT_SCOPE)
    else()
        set(${_out} "" PARENT_SCOPE)
    endif()
endfunction()

# -----------------------------------------------------------------------------
# us3_install_package()
#   Installs the UltraScan3Targets export set and UltraScan3Config.cmake.
#   Call once, after all libraries are defined.
# -----------------------------------------------------------------------------
function(us3_install_package)
    if(NOT US3_INSTALL_DEVEL)
        return()
    endif()
    include(CMakePackageConfigHelpers)

    # Qt components used by the exported libraries
    set(_qt_components Core Network Xml)
    if(NOT US3_NO_DB)
        list(APPEND _qt_components Sql)
    endif()
    set(_dep_code "")
    if(TARGET us_gui)
        list(APPEND _qt_components Gui Widgets Svg OpenGL PrintSupport)
        if(USE_QT6)
            list(APPEND _qt_components OpenGLWidgets)
        endif()
        string(APPEND _dep_code "find_dependency(OpenGL)\n")
        if(UNIX AND NOT APPLE)
            string(APPEND _dep_code "find_dependency(X11)\n")
        endif()
        us3_qwt_dependency_code(_qwt)
        string(APPEND _dep_code "${_qwt}")
        if(TARGET GL2PS::GL2PS)
            us3_imported_target_code(_gl2ps GL2PS::GL2PS)
            string(APPEND _dep_code "${_gl2ps}")
        endif()
        if(qwtplot3d_FOUND)
            string(APPEND _dep_code "find_dependency(qwtplot3d CONFIG)\n")
        else()
            string(APPEND _dep_code
                "find_dependency(qwtplot3d CONFIG HINTS \"\${CMAKE_CURRENT_LIST_DIR}/../qwtplot3d\")\n")
        endif()
    endif()
    if(NOT US3_NO_DB)
        if(unofficial-libmariadb_FOUND)
            string(APPEND _dep_code "find_dependency(unofficial-libmariadb CONFIG)\n")
        elseif(TARGET MySQL::Client)
            us3_imported_target_code(_mysql MySQL::Client)
            string(APPEND _dep_code "${_mysql}")
        endif()
    endif()

    set(US3_PKG_QT_PACKAGE "Qt${QT_VERSION_MAJOR}")
    string(REPLACE ";" " " US3_PKG_QT_COMPONENTS "${_qt_components}")
    set(US3_PKG_DEPENDENCIES "${_dep_code}")
    set(US3_PKG_NO_DB "${US3_NO_DB}")

    configure_package_config_file(
        "${PROJECT_SOURCE_DIR}/admin/cmake/UltraScan3Config.cmake.in"
        "${PROJECT_BINARY_DIR}/UltraScan3Config.cmake"
        INSTALL_DESTINATION "${US3_INSTALL_CMAKEDIR}")
    write_basic_package_version_file(
        "${PROJECT_BINARY_DIR}/UltraScan3ConfigVersion.cmake"
        VERSION "${PROJECT_VERSION}"
        COMPATIBILITY SameMajorVersion)

    install(EXPORT UltraScan3Targets
            NAMESPACE UltraScan3::
            FILE UltraScan3Targets.cmake
            DESTINATION "${US3_INSTALL_CMAKEDIR}"
            COMPONENT Development)
    install(FILES
            "${PROJECT_BINARY_DIR}/UltraScan3Config.cmake"
            "${PROJECT_BINARY_DIR}/UltraScan3ConfigVersion.cmake"
            DESTINATION "${US3_INSTALL_CMAKEDIR}"
            COMPONENT Development)
    message(STATUS "Install: CMake package UltraScan3 -> ${US3_INSTALL_CMAKEDIR} (US3_INSTALL_DEVEL=ON)")
endfunction()
