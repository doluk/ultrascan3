//! \file us_os_auth.h
#ifndef US_OS_AUTH_H
#define US_OS_AUTH_H

#include <QString>

#include "us_extern.h"

/*! \brief Verify the password of the currently logged-in OS user.

    Used as the challenge for audit-trail / e-signature forms when the user
    opts out of the UltraScan master password. Only the account running the
    process can be verified; there is no way to authenticate another user.

    Backends:
    - Linux:   PAM (compiled in with US3_HAVE_PAM). The PAM service is the
               first one present of "ultrascan3", "system-auth"
               (RHEL/Fedora/Arch), "common-auth" (Debian/Ubuntu), searched in
               /etc/pam.d and /usr/lib/pam.d. Unprivileged processes can
               verify their own password through pam_unix' unix_chkpwd.
    - macOS:   Open Directory (ODRecordVerifyPassword).
    - Windows: LogonUserW for the current account (network logon, falling
               back to interactive logon).
*/
class US_UTIL_EXTERN US_OsAuth
{
  public:
    //! \brief True if an authentication backend was compiled in.
    static bool    isAvailable( void );

    //! \brief Human-readable backend name, e.g. "PAM".
    static QString backendName( void );

    //! \brief Login name of the current OS user (Windows: DOMAIN\\user).
    static QString currentUser( void );

    //! \brief Verify \p password for the current OS user.
    //! \param password Password typed by the user
    //! \param error    Optional reason on failure
    //! \retval true    The password is correct
    static bool    verifyCurrentUser( const QString& password,
                                      QString* error = nullptr );

    //! \brief PAM service that verifyCurrentUser() uses; empty if PAM is
    //!        not the backend or no suitable service file exists.
    static QString pamService( void );
};

#endif
