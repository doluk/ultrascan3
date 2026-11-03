# Installing a CMake build

`cmake --install` produces the same relocatable tree as the `deploy_*`
targets (`admin/cmake/Us3Install.cmake`):

    <prefix>/
      bin/                   us, us_*, SOMO programs, manual.qch/.qhc (Windows: + DLLs)
      lib/                   libus_utils*, libus_gui* (+ bundled dependencies)
      plugins/               Qt and MariaDB plugins          (bundled dependencies)
      etc/  somo/            runtime data
      license.txt  VERSION
      lib/cmake/UltraScan3/  CMake package                   (US3_INSTALL_DEVEL)
      include/ultrascan3/    utils/ and gui/ headers         (US3_INSTALL_DEVEL)

The directory names are fixed: the programs find `etc/` and `somo/` relative
to their own `bin/` (`US_Settings::appBaseDir()`), and the RPATHs point to
`../lib`.

## Commands

    cmake --preset linux-release-qt6
    cmake --build --preset linux-release-qt6
    cmake --install build/linux-release-qt6                      # -> install/linux-release-qt6
    cmake --install build/linux-release-qt6 --prefix /opt/UltraScan3
    cmake --install build/linux-release-qt6 --component Runtime

The presets set the default prefix to `install/<preset>`. Build first:
`cmake --install` does not build.

## Options (configure time)

| Option | Default | Effect |
|---|---|---|
| `US3_INSTALL_RUNTIME_DEPS` | `ON` | Run the platform deploy script (`admin/cmake/packaging/*/…Deploy.cmake`) on the install prefix: Qt libraries and plugins, Qwt, MariaDB, OpenSSL, Qt Assistant, launchers. `OFF` installs only UltraScan3's own files; the binaries then keep the linker search path as RPATH. |
| `US3_INSTALL_DEVEL` | `OFF` | Also install headers, static libraries, qwtplot3d (in-tree build) and the CMake package `UltraScan3`. |

At install time, `-DUS3_SKIP_RUNTIME_DEPS=ON` (or the environment variable
`US3_SKIP_RUNTIME_DEPS=1`) skips the deploy script once.

## Components

| Component | Content |
|---|---|
| `Runtime` | programs, shared libraries, bundled runtime dependencies |
| `Data` | `etc/`, `somo/`, `license.txt`, `VERSION` |
| `Documentation` | Qt help files |
| `SOMO` | SOMO programs and helpers (`-DUS3_BUILD_SOMO=ON`) |
| `Development` | headers, static libraries, CMake package (`US3_INSTALL_DEVEL`) |

## Using the installed libraries

    find_package(UltraScan3 4 CONFIG REQUIRED)        # CMAKE_PREFIX_PATH=<prefix>
    target_link_libraries(app PRIVATE UltraScan3::Gui) # or UltraScan3::Utils

Targets: `UltraScan3::Utils`, `UltraScan3::Gui`, and, when the build made
them, `UltraScan3::Utils_Static`, `UltraScan3::Gui_Static`,
`UltraScan3::Resources` (link it as a whole archive). The package finds the
same Qt major version, OpenSSL, LibArchive, MariaDB, Qwt and qwtplot3d that the
build used.

## Packaging

`deploy_*` / `package_*` targets are unchanged and still stage into
`build/<preset>/_stage`. On Linux the CPack `TGZ` fallback (`cpack`) now
packs the install tree. The Windows NSIS package still packs the
`deploy_windows` stage (`CPACK_INSTALLED_DIRECTORIES`).
