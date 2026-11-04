# UltraScan3 Utils Library
# Based on library.pri configuration

# Auto-discover source files
file(GLOB UTILS_SOURCES "*.cpp")
file(GLOB UTILS_HEADERS "*.h")

# Exclude instrument-specific DB importers when building without database support.
# US_CfaData (SQLite) and US_XpnData (PostgreSQL) are data-acquisition components
# that are not needed for the HPC compute-only profile.
# US_LinkSsl manages SSL connections to the Optima instrument — also GUI/DB only.
if(US3_NO_DB)
    list(REMOVE_ITEM UTILS_SOURCES
        "${CMAKE_CURRENT_SOURCE_DIR}/us_cfa_data.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/us_xpn_data.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/us_link_ssl.cpp"
    )
    list(REMOVE_ITEM UTILS_HEADERS
        "${CMAKE_CURRENT_SOURCE_DIR}/us_cfa_data.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/us_xpn_data.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/us_link_ssl.h"
    )
    # OS keychain / OS authentication only serve the interactive DB login
    list(REMOVE_ITEM UTILS_SOURCES
        "${CMAKE_CURRENT_SOURCE_DIR}/us_os_keychain.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/us_os_auth.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/us_master_password.cpp"
    )
    list(REMOVE_ITEM UTILS_HEADERS
        "${CMAKE_CURRENT_SOURCE_DIR}/us_os_keychain.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/us_os_auth.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/us_master_password.h"
    )
endif()

# -----------------------------------------------------------------------------
# Optional OS integration for the master password (see us_os_keychain.h and
# us_os_auth.h). Missing dependencies only disable the feature; the classic
# master password always works.
# -----------------------------------------------------------------------------
set(US3_OS_INTEGRATION_LIBS "")
set(US3_OS_INTEGRATION_DEFS "")
set(US3_OS_INTEGRATION_INCS "")
if(NOT US3_NO_DB)
    if(US3_WITH_OS_KEYCHAIN)
        if(QT_VERSION_MAJOR EQUAL 6)
            find_package(Qt6Keychain CONFIG QUIET)
            set(_us3_keychain_target qt6keychain)
        else()
            find_package(Qt5Keychain CONFIG QUIET)
            set(_us3_keychain_target qt5keychain)
        endif()
        if(TARGET ${_us3_keychain_target})
            message(STATUS "OS keychain support: ON (${_us3_keychain_target})")
            list(APPEND US3_OS_INTEGRATION_LIBS ${_us3_keychain_target})
            list(APPEND US3_OS_INTEGRATION_DEFS US3_HAVE_QTKEYCHAIN)
        else()
            message(STATUS "OS keychain support: OFF (QtKeychain for Qt${QT_VERSION_MAJOR} not found)")
        endif()
    endif()

    if(US3_WITH_OS_AUTH)
        if(APPLE)
            list(APPEND US3_OS_INTEGRATION_LIBS "-framework OpenDirectory" "-framework CoreFoundation")
            message(STATUS "OS authentication support: ON (Open Directory)")
        elseif(WIN32)
            list(APPEND US3_OS_INTEGRATION_LIBS advapi32 secur32)
            message(STATUS "OS authentication support: ON (LogonUser)")
        else()
            find_path(PAM_INCLUDE_DIR security/pam_appl.h)
            find_library(PAM_LIBRARY NAMES pam)
            if(PAM_INCLUDE_DIR AND PAM_LIBRARY)
                message(STATUS "OS authentication support: ON (PAM: ${PAM_LIBRARY})")
                list(APPEND US3_OS_INTEGRATION_LIBS ${PAM_LIBRARY})
                list(APPEND US3_OS_INTEGRATION_DEFS US3_HAVE_PAM)
                list(APPEND US3_OS_INTEGRATION_INCS ${PAM_INCLUDE_DIR})
            else()
                message(STATUS "OS authentication support: OFF (PAM headers/library not found)")
            endif()
        endif()
    endif()
endif()

function(us3_configure_os_integration target)
    if(US3_OS_INTEGRATION_LIBS)
        target_link_libraries(${target} PRIVATE ${US3_OS_INTEGRATION_LIBS})
    endif()
    if(US3_OS_INTEGRATION_DEFS)
        target_compile_definitions(${target} PRIVATE ${US3_OS_INTEGRATION_DEFS})
    endif()
    if(US3_OS_INTEGRATION_INCS)
        target_include_directories(${target} PRIVATE ${US3_OS_INTEGRATION_INCS})
    endif()
endfunction()

# Create the utils library
add_library(us_utils ${UTILS_SOURCES} ${UTILS_HEADERS})
us3_configure_os_integration(us_utils)

if(APPLE)
    # Force all symbols to be exported at link time
    target_link_options(us_utils PRIVATE
            "LINKER:-export_dynamic"
            "LINKER:-all_load"
            )
    # Override any inherited visibility settings
    target_compile_options(us_utils PRIVATE -fvisibility=default)
elseif(UNIX)
    # Force all symbols to be exported at link time
    target_link_options(us_utils PRIVATE
            "LINKER:--export-dynamic"
    )
    # Override any inherited visibility settings
    target_compile_options(us_utils PRIVATE -fvisibility=default)
endif()

# Set library properties - MERGED with visibility settings
set_target_properties(us_utils PROPERTIES
        VERSION ${PROJECT_VERSION}
        SOVERSION ${PROJECT_VERSION_MAJOR}
        OUTPUT_NAME "us_utils${PROJECT_VERSION_MAJOR}"
        CXX_VISIBILITY_PRESET default
        C_VISIBILITY_PRESET default
        VISIBILITY_INLINES_HIDDEN OFF
        POSITION_INDEPENDENT_CODE ON
        )

# Platform-specific properties
if(APPLE)
    set_target_properties(us_utils PROPERTIES
            MACOSX_RPATH ON
            INSTALL_RPATH "@loader_path;@loader_path/../lib"
            )
elseif(UNIX)
    set_target_properties(us_utils PROPERTIES
            INSTALL_RPATH "$ORIGIN:$ORIGIN/../lib"
            )
elseif(WIN32)
    # Windows handles DLLs differently
    set_target_properties(us_utils PROPERTIES
            WINDOWS_EXPORT_ALL_SYMBOLS OFF  # We'll use explicit exports
            )
    # Use BigObj format for large files
    if(MSVC)
        target_compile_options(us_utils PRIVATE "/bigobj")
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(us_utils PRIVATE "-Wa,-mbig-obj")
    endif()
endif()

# Ensure dynamic Qt linking - ALL PLATFORMS
target_compile_definitions(us_utils
        PUBLIC
        QT_SHARED
        PRIVATE
        QT_PLUGIN
        )

## Symbol visibility flags for GCC/Clang (Linux, macOS)
#if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
#    target_compile_options(us_utils PRIVATE
#            -fvisibility=hidden
#            -fvisibility-inlines-hidden
#            )
#endif()

# Include directories
target_include_directories(us_utils
        PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_BINARY_DIR}  # For generated headers
        )

# Core Qt libraries + OpenSSL (always needed — us_crypto.cpp uses EVP directly)
target_link_libraries(us_utils
        PUBLIC
        Qt::Core
        Qt::Xml
        Qt::Network
        OpenSSL::SSL
        OpenSSL::Crypto
        LibArchive::LibArchive
        )

# Database support (APP/TEST profiles only)
if(NOT US3_NO_DB)
    target_link_libraries(us_utils
            PUBLIC
            Qt::Sql
            MySQL::Client
            )
endif()

# Platform-specific libraries
if(UNIX AND NOT APPLE)
    # Linux-specific libraries
    if(X11_FOUND)
        target_link_libraries(us_utils PUBLIC ${X11_LIBRARIES})
        target_include_directories(us_utils PUBLIC ${X11_INCLUDE_DIR})
    endif()
elseif(WIN32)
    # Windows system libraries
    target_link_libraries(us_utils PUBLIC ${WIN32_SYSTEM_LIBS})
    target_compile_definitions(us_utils PRIVATE US_MAKE_DLL US_MAKE_UTIL_DLL)
endif()

# Compiler-specific options for debugging
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    if(MSVC)
        target_compile_options(us_utils PRIVATE /Zi /Od)
    else()
        target_compile_options(us_utils PRIVATE -g -O0)
    endif()
endif()

# Export this target for use by other parts of the project
add_library(UltraScan3::Utils ALIAS us_utils)

# =============================================================================
# OPTIONAL: Build static library for unit testing
# =============================================================================
if(BUILD_TESTING OR US3_PREFER_STATIC)
    message(STATUS "Building static utils library for testing")

    # Create STATIC library specifically for testing
    add_library(us_utils_static STATIC ${UTILS_SOURCES} ${UTILS_HEADERS})
    us3_configure_os_integration(us_utils_static)

    # Set properties for STATIC library
    set_target_properties(us_utils_static PROPERTIES
            OUTPUT_NAME "us_utils${PROJECT_VERSION_MAJOR}_static"
            POSITION_INDEPENDENT_CODE ON
            )

    # Apply same compile definitions
    target_compile_definitions(us_utils_static
            PUBLIC
            QT_SHARED
            US_UTIL_STATIC
            )

    # Apply same include directories
    target_include_directories(us_utils_static
            PUBLIC
            ${CMAKE_CURRENT_SOURCE_DIR}
            ${CMAKE_CURRENT_BINARY_DIR}
            )

    # Core Qt libraries + OpenSSL (always needed)
    target_link_libraries(us_utils_static
            PUBLIC
            Qt::Core
            Qt::Xml
            Qt::Network
            OpenSSL::SSL
            OpenSSL::Crypto
            LibArchive::LibArchive
            )

    # Database support (APP/TEST profiles only)
    if(NOT US3_NO_DB)
        target_link_libraries(us_utils_static
                PUBLIC
                Qt::Sql
                MySQL::Client
                )
    endif()

    # Platform-specific libraries
    if(UNIX AND NOT APPLE)
        if(X11_FOUND)
            target_link_libraries(us_utils_static PUBLIC ${X11_LIBRARIES})
            target_include_directories(us_utils_static PUBLIC ${X11_INCLUDE_DIR})
        endif()
    elseif(WIN32)
        target_link_libraries(us_utils_static PUBLIC ${WIN32_SYSTEM_LIBS})
        # Use BigObj format for large files
        if(MSVC)
            target_compile_options(us_utils_static PRIVATE "/bigobj")
        elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
            target_compile_options(us_utils_static PRIVATE "-Wa,-mbig-obj")
        endif()
    endif()

    # Debug options
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        if(MSVC)
            target_compile_options(us_utils_static PRIVATE /Zi /Od)
        else()
            target_compile_options(us_utils_static PRIVATE -g -O0)
        endif()
    endif()

    # Export static library alias
    add_library(UltraScan3::Utils_Static ALIAS us_utils_static)

else()
    message(STATUS "Skipping static utils library (testing disabled)")
endif()
# END: Build static library for unit testing

# Installation - only install the shared library
install(TARGETS us_utils
        EXPORT UltraScan3Targets
        LIBRARY DESTINATION lib
        ARCHIVE DESTINATION lib
        RUNTIME DESTINATION bin
        )
