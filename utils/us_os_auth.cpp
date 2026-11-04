//! \file us_os_auth.cpp
#include "us_os_auth.h"

#include <QFileInfo>
#include <QObject>
#include <QStringList>

#include <cstdlib>
#include <cstring>

#include <openssl/crypto.h>   // OPENSSL_cleanse

#if defined( Q_OS_WIN )
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  ifndef SECURITY_WIN32
#    define SECURITY_WIN32
#  endif
#  include <security.h>        // GetUserNameExW (Secur32)
#elif defined( Q_OS_MACOS )
#  include <unistd.h>
#  include <pwd.h>
#  include <CoreFoundation/CoreFoundation.h>
#  include <CFOpenDirectory/CFOpenDirectory.h>
#else
#  include <unistd.h>
#  include <pwd.h>
#  ifdef US3_HAVE_PAM
#    include <security/pam_appl.h>
#  endif
#endif

namespace
{
   // Wipe a byte buffer that held a password.
   Q_DECL_UNUSED void wipe( QByteArray& ba )
   {
      if ( ! ba.isEmpty() )
         OPENSSL_cleanse( ba.data(), static_cast<size_t>( ba.size() ) );
      ba.clear();
   }

#if !defined( Q_OS_WIN )
   QString posixUserName( void )
   {
      const struct passwd* pw = getpwuid( geteuid() );
      return ( pw != nullptr && pw->pw_name != nullptr )
             ? QString::fromLocal8Bit( pw->pw_name ) : QString();
   }
#endif

#if defined( US3_HAVE_PAM ) && !defined( Q_OS_WIN ) && !defined( Q_OS_MACOS )
   struct PamConvData
   {
      const char* password;
   };

   void freeResponses( struct pam_response* resp, int count )
   {
      if ( resp == nullptr ) return;
      for ( int i = 0; i < count; ++i )
      {
         if ( resp[ i ].resp != nullptr )
         {
            OPENSSL_cleanse( resp[ i ].resp, std::strlen( resp[ i ].resp ) );
            std::free( resp[ i ].resp );
         }
      }
      std::free( resp );
   }

   // Non-interactive conversation: answer every hidden prompt with the
   // supplied password, ignore informational messages, and refuse visible
   // prompts (e.g. a username or OTP request) instead of blocking.
   int pamConversation( int num_msg, const struct pam_message** msg,
                                   struct pam_response** resp, void* appdata )
   {
      if ( num_msg <= 0 || num_msg > PAM_MAX_NUM_MSG || appdata == nullptr )
         return PAM_CONV_ERR;

      const PamConvData* data = static_cast<const PamConvData*>( appdata );
      struct pam_response* reply = static_cast<struct pam_response*>(
            std::calloc( static_cast<size_t>( num_msg ), sizeof( struct pam_response ) ) );
      if ( reply == nullptr )
         return PAM_BUF_ERR;

      for ( int i = 0; i < num_msg; ++i )
      {
         switch ( msg[ i ]->msg_style )
         {
            case PAM_PROMPT_ECHO_OFF:
               reply[ i ].resp = strdup( data->password );
               if ( reply[ i ].resp == nullptr )
               {
                  freeResponses( reply, num_msg );
                  return PAM_BUF_ERR;
               }
               break;
            case PAM_ERROR_MSG:
            case PAM_TEXT_INFO:
               break;
            default:   // PAM_PROMPT_ECHO_ON and unknown styles
               freeResponses( reply, num_msg );
               return PAM_CONV_ERR;
         }
      }

      *resp = reply;
      return PAM_SUCCESS;
   }
#endif
} // namespace

bool US_OsAuth::isAvailable( void )
{
#if defined( Q_OS_WIN ) || defined( Q_OS_MACOS )
   return true;
#elif defined( US3_HAVE_PAM )
   return ! pamService().isEmpty();
#else
   return false;
#endif
}

QString US_OsAuth::backendName( void )
{
#if defined( Q_OS_WIN )
   return QObject::tr( "Windows account" );
#elif defined( Q_OS_MACOS )
   return QObject::tr( "macOS account" );
#elif defined( US3_HAVE_PAM )
   return QObject::tr( "PAM (%1)" ).arg( pamService() );
#else
   return QObject::tr( "none" );
#endif
}

QString US_OsAuth::pamService( void )
{
#if defined( US3_HAVE_PAM ) && !defined( Q_OS_WIN ) && !defined( Q_OS_MACOS )
   // An admin-provided "ultrascan3" service takes precedence; otherwise use
   // the distribution's shared auth stack.
   static const QStringList services = { "ultrascan3", "system-auth", "common-auth" };
   static const QStringList dirs     = { "/etc/pam.d", "/usr/lib/pam.d" };
   for ( const QString& svc : services )
      for ( const QString& dir : dirs )
         if ( QFileInfo::exists( dir + "/" + svc ) )
            return svc;
#endif
   return QString();
}

QString US_OsAuth::currentUser( void )
{
#if defined( Q_OS_WIN )
   ULONG size = 0;
   GetUserNameExW( NameSamCompatible, nullptr, &size );
   if ( size == 0 ) return QString();
   std::wstring buf( size, L'\0' );
   if ( ! GetUserNameExW( NameSamCompatible, &buf[ 0 ], &size ) )
      return QString();
   buf.resize( size );
   return QString::fromStdWString( buf );
#else
   return posixUserName();
#endif
}

bool US_OsAuth::verifyCurrentUser( const QString& password, QString* error )
{
   if ( password.isEmpty() )
   {
      if ( error ) *error = QObject::tr( "Empty password" );
      return false;
   }

   const QString user = currentUser();
   if ( user.isEmpty() )
   {
      if ( error ) *error = QObject::tr( "Cannot determine the current OS user" );
      return false;
   }

#if defined( Q_OS_WIN )
   // Split DOMAIN\user
   QString domain = ".";
   QString name   = user;
   const int bs   = user.indexOf( '\\' );
   if ( bs >= 0 )
   {
      domain = user.left( bs );
      name   = user.mid( bs + 1 );
   }

   std::wstring wpw = password.toStdWString();
   HANDLE token = nullptr;
   BOOL ok = LogonUserW( reinterpret_cast<LPCWSTR>( name.utf16() ),
                         reinterpret_cast<LPCWSTR>( domain.utf16() ),
                         wpw.c_str(), LOGON32_LOGON_NETWORK,
                         LOGON32_PROVIDER_DEFAULT, &token );
   DWORD err = ok ? 0 : GetLastError();
   if ( ! ok && err == ERROR_LOGON_TYPE_NOT_GRANTED )
   {
      ok  = LogonUserW( reinterpret_cast<LPCWSTR>( name.utf16() ),
                        reinterpret_cast<LPCWSTR>( domain.utf16() ),
                        wpw.c_str(), LOGON32_LOGON_INTERACTIVE,
                        LOGON32_PROVIDER_DEFAULT, &token );
      err = ok ? 0 : GetLastError();
   }
   SecureZeroMemory( &wpw[ 0 ], wpw.size() * sizeof( wchar_t ) );

   if ( ok )
   {
      CloseHandle( token );
      return true;
   }
   if ( error )
   {
      *error = ( err == ERROR_LOGON_FAILURE )
               ? QObject::tr( "Incorrect password" )
               : QObject::tr( "Windows logon failed (error %1)" ).arg( err );
   }
   return false;

#elif defined( Q_OS_MACOS )
   bool       result  = false;
   CFErrorRef cferr   = nullptr;
   QByteArray pwUtf8  = password.toUtf8();
   QByteArray usrUtf8 = user.toUtf8();

   CFStringRef cfUser = CFStringCreateWithCString( kCFAllocatorDefault,
                           usrUtf8.constData(), kCFStringEncodingUTF8 );
   CFStringRef cfPw   = CFStringCreateWithBytes( kCFAllocatorDefault,
                           reinterpret_cast<const UInt8*>( pwUtf8.constData() ),
                           pwUtf8.size(), kCFStringEncodingUTF8, false );
   wipe( pwUtf8 );

   ODNodeRef node = ODNodeCreateWithNodeType( kCFAllocatorDefault, kODSessionDefault,
                                              kODNodeTypeAuthentication, &cferr );
   ODRecordRef record = nullptr;
   if ( node != nullptr && cfUser != nullptr )
      record = ODNodeCopyRecord( node, kODRecordTypeUsers, cfUser, nullptr, &cferr );
   if ( record != nullptr && cfPw != nullptr )
      result = ODRecordVerifyPassword( record, cfPw, &cferr );

   if ( ! result && error )
   {
      if ( cferr != nullptr )
      {
         CFStringRef desc = CFErrorCopyDescription( cferr );
         char buf[ 512 ] = { 0 };
         if ( desc != nullptr )
         {
            CFStringGetCString( desc, buf, sizeof( buf ), kCFStringEncodingUTF8 );
            CFRelease( desc );
         }
         *error = QString::fromUtf8( buf );
      }
      else
         *error = QObject::tr( "Incorrect password" );
   }

   if ( cferr  ) CFRelease( cferr );
   if ( record ) CFRelease( record );
   if ( node   ) CFRelease( node );
   if ( cfPw   ) CFRelease( cfPw );
   if ( cfUser ) CFRelease( cfUser );
   return result;

#elif defined( US3_HAVE_PAM )
   const QString service = pamService();
   if ( service.isEmpty() )
   {
      if ( error ) *error = QObject::tr( "No usable PAM service found in /etc/pam.d" );
      return false;
   }

   QByteArray pwBytes   = password.toUtf8();
   QByteArray userBytes = user.toLocal8Bit();
   QByteArray svcBytes  = service.toLatin1();

   PamConvData     data { pwBytes.constData() };
   struct pam_conv conv { &pamConversation, &data };
   pam_handle_t*   handle = nullptr;

   int rc = pam_start( svcBytes.constData(), userBytes.constData(), &conv, &handle );
   if ( rc == PAM_SUCCESS )
      rc = pam_authenticate( handle, PAM_SILENT | PAM_DISALLOW_NULL_AUTHTOK );
   // Account checks (expiry, lock) only for the dedicated service, which is
   // guaranteed to contain an account stack; the shared *-auth files are not.
   if ( rc == PAM_SUCCESS && service == "ultrascan3" )
      rc = pam_acct_mgmt( handle, PAM_SILENT );

   if ( rc != PAM_SUCCESS && error )
      *error = ( handle != nullptr )
               ? QString::fromLocal8Bit( pam_strerror( handle, rc ) )
               : QObject::tr( "pam_start failed (%1)" ).arg( rc );

   if ( handle != nullptr )
      pam_end( handle, rc );
   wipe( pwBytes );
   return rc == PAM_SUCCESS;

#else
   if ( error ) *error = QObject::tr( "UltraScan was built without OS authentication support" );
   return false;
#endif
}
