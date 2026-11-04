//! \file us_master_password.h
#ifndef US_MASTER_PASSWORD_H
#define US_MASTER_PASSWORD_H

#include <QByteArray>
#include <QString>

#include <cstdio>

#include "us_extern.h"

/*! \brief GUI-free master-password logic.

    The UltraScan master password encrypts the database passwords stored in
    the user's settings (see \ref US_Crypto). This class holds everything
    that does not need a widget:

    - checking a candidate against the hash in \ref US_Settings::UltraScanPW
    - the per-user shared-memory cache (\ref US_Global)
    - the optional OS keychain copy (\ref US_OsKeychain)
    - non-interactive sources for command-line tools: the environment
      variable \c US3_MASTER_PASSWORD and a no-echo terminal prompt

    \ref US_Passwd (GUI library) only adds the dialog on top of this.

    Lookup order of obtainHeadless(): cache, OS keychain (if enabled),
    environment variable, terminal prompt (if stdin is a terminal).
    The password is never accepted on the command line, which would expose
    it in process listings and shell history.
*/
class US_UTIL_EXTERN US_MasterPassword
{
  public:
    //! \brief Name of the environment variable read by fromEnvironment().
    static const char* envVarName( void );

    //! \brief The hash format stored in the settings (SHA-1 of Latin-1).
    static QByteArray  hash( const QString& password );

    //! \brief True if a master password has been defined.
    static bool        isSet( void );

    //! \brief True if \p candidate matches the stored hash.
    static bool        verify( const QString& candidate );

    //! \brief The password cached in shared memory, or empty.
    static QString     cached( void );

    //! \brief Verify \p candidate and, if correct, cache it in shared memory
    //!        and refresh the OS keychain copy when that is enabled but was
    //!        found missing or outdated.
    //! \retval true if accepted
    static bool        accept( const QString& candidate );

    //! \brief Clear the shared-memory cache.
    static void        clearCache( void );

    //! \brief Read the password from the OS keychain if enabled.
    //!        A verified password is cached and returned; otherwise empty.
    static QString     fromKeychain( QString* error = nullptr );

    //! \brief Read the password from \c US3_MASTER_PASSWORD.
    //!        A verified password is cached and returned; otherwise empty.
    static QString     fromEnvironment( QString* error = nullptr );

    //! \brief Prompt on the controlling terminal without echo.
    //!        Fails immediately if stdin is not a terminal.
    //! \param attempts Number of tries before giving up
    static QString     fromTerminal( QString* error = nullptr, int attempts = 3 );

    //! \brief Prompt loop on arbitrary streams; echo is suppressed when
    //!        \p in is a terminal. Used by fromTerminal() and unit tests.
    static QString     promptLoop( FILE* in, FILE* out, int attempts,
                                   QString* error = nullptr );

    //! \brief Cache, then OS keychain. Never prompts.
    static QString     obtainNonInteractive( QString* error = nullptr );

    //! \brief Cache, OS keychain, environment, then terminal prompt.
    //!        For command-line tools without a GUI.
    static QString     obtainHeadless( QString* error = nullptr );
};

#endif
