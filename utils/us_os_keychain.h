//! \file us_os_keychain.h
#ifndef US_OS_KEYCHAIN_H
#define US_OS_KEYCHAIN_H

#include <QString>

#include "us_extern.h"

/*! \brief Thin, blocking wrapper around the operating-system secret store.

    Native backends, no third-party library:
    - macOS:   Keychain via Security.framework (generic-password item)
    - Windows: Credential Manager (CredWriteW / CredReadW; DPAPI-protected)
    - Linux:   freedesktop.org Secret Service over Qt D-Bus (GNOME Keyring,
               KWallet >= 5.97, KeePassXC)

    The OS store is unlocked by the user's login and is re-protected by the
    OS when the user changes the login password, so secrets stored here
    survive a password change. It does not survive an administrative
    password reset (macOS, Windows) or a locked/missing Secret Service
    daemon (headless Linux). Callers must therefore always keep a fallback.

    On Linux, support is compiled in only when the Qt D-Bus module is
    available (macro US3_HAVE_QTDBUS). Otherwise isAvailable() returns
    false and every operation fails with an explanatory error.
*/
class US_UTIL_EXTERN US_OsKeychain
{
  public:
    //! \brief True if a keychain backend was compiled in.
    static bool    isAvailable( void );

    //! \brief Human-readable name of the backend for this platform.
    static QString backendName( void );

    //! \brief Store a secret under \p key (service "UltraScan3").
    //! \param key    Entry name
    //! \param secret Secret to store
    //! \param error  Optional error description on failure
    //! \retval true on success
    static bool    writeSecret ( const QString& key, const QString& secret,
                                 QString* error = nullptr );

    //! \brief Read the secret stored under \p key.
    //! \param key    Entry name
    //! \param secret Receives the secret on success
    //! \param error  Optional error description on failure
    //! \retval true on success (entry existed and was read)
    static bool    readSecret  ( const QString& key, QString& secret,
                                 QString* error = nullptr );

    //! \brief Remove the secret stored under \p key. Removing a
    //!        non-existent entry counts as success.
    static bool    deleteSecret( const QString& key, QString* error = nullptr );

    //! \brief Keychain entry name used for the UltraScan master password.
    static QString masterPasswordKey( void );

    //! \brief Service name under which all UltraScan entries are stored.
    static QString serviceName( void );
};

#endif
