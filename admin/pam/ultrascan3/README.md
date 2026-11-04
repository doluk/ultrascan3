# OS integration of the UltraScan master password

Both options are set in **us_config → Change Master Password** and are off by
default; the classic master password keeps working unchanged.

| Option | Effect | Backend |
|---|---|---|
| Unlock with OS login | The master password is stored in the OS secret store and read from there, so it is not typed again. If the store is unavailable (admin password reset, no Secret Service daemon, new machine) UltraScan asks for the master password and re-stores it. | macOS Keychain, Windows Credential Manager, Linux Secret Service / KWallet (QtKeychain) |
| Confirm audit-trail forms with the OS account password | GMP audit-trail and e-signature forms ask for the password of the logged-in OS account instead of the master password. The password is not returned to the caller. | Linux PAM, macOS Open Directory, Windows `LogonUserW` |

## Command-line tools

With `--errors-cl` the simulator CLIs have no GUI. Database access then takes
the master password, in this order, from:

1. the shared-memory cache of a running, unlocked UltraScan,
2. the OS keychain (if enabled),
3. the environment variable `US3_MASTER_PASSWORD`,
4. a no-echo prompt on the terminal (if stdin is a terminal).

The password is never accepted as a command-line argument.

## Build

* CMake: `US3_WITH_OS_KEYCHAIN` and `US3_WITH_OS_AUTH` (both `ON`). A missing
  QtKeychain (`Qt5Keychain`/`Qt6Keychain`) or PAM (`libpam0g-dev`,
  `pam-devel`) only disables the respective option.
* qmake: set `QTKEYCHAIN_INC` / `QTKEYCHAIN_LIB` in `local.pri`; PAM is used if
  `/usr/include/security/pam_appl.h` exists.

## Linux PAM service

See `ultrascan3` in this directory for an optional dedicated PAM service.
