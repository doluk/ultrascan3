//! \file us_master_password.cpp
#include "us_master_password.h"

#include <QCryptographicHash>
#include <QDebug>
#include <QObject>
#include <QPoint>
#include <QSharedMemory>
#include <QStringList>
#include <QTextStream>

#include <cstring>

#include <openssl/crypto.h>   // OPENSSL_cleanse

#include "us_global.h"
#include "us_os_keychain.h"
#include "us_settings.h"

#if defined( Q_OS_WIN )
#  include <io.h>
#  include <windows.h>
#else
#  include <termios.h>
#  include <unistd.h>
#endif

namespace
{
   // Set when the keychain was enabled but had no (valid) entry, so that the
   // next accepted password is written back.
   bool keychainStale = false;

   // One attachment for the life of the process. A temporary US_Global would
   // detach again at once, and when no other UltraScan program is attached
   // the next attachment starts from zeroed memory, losing the cache.
   US_Global& sharedCache( void )
   {
      static US_Global g;
      return g;
   }

   bool isTerminal( FILE* f )
   {
#if defined( Q_OS_WIN )
      return f != nullptr && _isatty( _fileno( f ) ) != 0;
#else
      return f != nullptr && isatty( fileno( f ) ) != 0;
#endif
   }

   // Switch terminal echo of \p f off (on == false) or back on.
   // Returns false if the stream is not a terminal.
   class EchoGuard
   {
     public:
      explicit EchoGuard( FILE* f ) : active( false )
      {
         if ( ! isTerminal( f ) ) return;
#if defined( Q_OS_WIN )
         handle = reinterpret_cast<HANDLE>( _get_osfhandle( _fileno( f ) ) );
         if ( handle != INVALID_HANDLE_VALUE && GetConsoleMode( handle, &saved ) )
            active = SetConsoleMode( handle, saved & ~ENABLE_ECHO_INPUT ) != 0;
#else
         fd = fileno( f );
         if ( tcgetattr( fd, &saved ) == 0 )
         {
            struct termios noecho = saved;
            noecho.c_lflag &= ~static_cast<tcflag_t>( ECHO );
            noecho.c_lflag |= ECHONL;
            active = tcsetattr( fd, TCSAFLUSH, &noecho ) == 0;
         }
#endif
      }
      ~EchoGuard()
      {
         if ( ! active ) return;
#if defined( Q_OS_WIN )
         SetConsoleMode( handle, saved );
#else
         tcsetattr( fd, TCSAFLUSH, &saved );
#endif
      }
      bool hidden() const { return active; }

     private:
      bool active;
#if defined( Q_OS_WIN )
      HANDLE handle = INVALID_HANDLE_VALUE;
      DWORD  saved  = 0;
#else
      int            fd = -1;
      struct termios saved;
#endif
   };

   // Read one line from \p in (without the line terminator).
   bool readLine( FILE* in, QByteArray& line )
   {
      line.clear();
      int c;
      bool any = false;
      while ( ( c = std::fgetc( in ) ) != EOF )
      {
         any = true;
         if ( c == '\n' ) break;
         if ( c != '\r' ) line.append( static_cast<char>( c ) );
      }
      return any;
   }

   void wipe( QByteArray& ba )
   {
      if ( ! ba.isEmpty() )
         OPENSSL_cleanse( ba.data(), static_cast<size_t>( ba.size() ) );
      ba.clear();
   }
} // namespace

const char* US_MasterPassword::envVarName( void )
{
   return "US3_MASTER_PASSWORD";
}

QByteArray US_MasterPassword::hash( const QString& password )
{
   return QCryptographicHash::hash( password.toLatin1(), QCryptographicHash::Sha1 );
}

bool US_MasterPassword::isSet( void )
{
   return ! US_Settings::UltraScanPW().isEmpty();
}

bool US_MasterPassword::verify( const QString& candidate )
{
   const QByteArray stored = US_Settings::UltraScanPW();
   if ( stored.isEmpty() || candidate.isEmpty() )
      return false;
   return hash( candidate ) == stored;
}

QString US_MasterPassword::cached( void )
{
   return sharedCache().passwd();
}

void US_MasterPassword::clearCache( void )
{
   sharedCache().setPasswd( QString() );
}

bool US_MasterPassword::accept( const QString& candidate )
{
   if ( ! verify( candidate ) )
      return false;

   sharedCache().setPasswd( candidate );

   if ( keychainStale && US_Settings::masterPwInKeychain() && US_OsKeychain::isAvailable() )
   {
      QString err;
      if ( US_OsKeychain::writeSecret( US_OsKeychain::masterPasswordKey(), candidate, &err ) )
         keychainStale = false;
      else
         qWarning() << "Could not store the master password in the OS keychain:" << err;
   }
   return true;
}

QString US_MasterPassword::fromKeychain( QString* error )
{
   if ( ! isSet() || ! US_Settings::masterPwInKeychain() )
   {
      if ( error ) *error = QObject::tr( "OS keychain is not enabled" );
      return QString();
   }

   QString pw;
   QString err;
   if ( ! US_OsKeychain::readSecret( US_OsKeychain::masterPasswordKey(), pw, &err ) )
   {
      keychainStale = US_OsKeychain::isAvailable();
      if ( error ) *error = err;
      return QString();
   }

   if ( ! accept( pw ) )
   {
      keychainStale = true;
      if ( error ) *error = QObject::tr( "The master password in the OS keychain is outdated" );
      return QString();
   }
   return pw;
}

QString US_MasterPassword::fromEnvironment( QString* error )
{
   if ( ! qEnvironmentVariableIsSet( envVarName() ) )
   {
      if ( error ) *error = QObject::tr( "%1 is not set" ).arg( envVarName() );
      return QString();
   }

   const QString pw = qEnvironmentVariable( envVarName() );
   if ( ! accept( pw ) )
   {
      if ( error ) *error = QObject::tr( "%1 does not match the master password" )
                            .arg( envVarName() );
      return QString();
   }
   return pw;
}

QString US_MasterPassword::promptLoop( FILE* in, FILE* out, int attempts, QString* error )
{
   if ( in == nullptr || out == nullptr || attempts < 1 )
   {
      if ( error ) *error = QObject::tr( "No input stream" );
      return QString();
   }

   for ( int ii = 0; ii < attempts; ++ii )
   {
      std::fputs( "UltraScan master password: ", out );
      std::fflush( out );

      QByteArray line;
      bool       got;
      {
         EchoGuard guard( in );
         got = readLine( in, line );
      }

      if ( ! got )
      {
         if ( error ) *error = QObject::tr( "No password entered" );
         return QString();
      }

      QString pw = QString::fromLocal8Bit( line );
      wipe( line );
      if ( accept( pw ) )
         return pw;

      pw.fill( QChar( 0 ) );
      std::fputs( "Incorrect password.\n", out );
      std::fflush( out );
   }

   if ( error ) *error = QObject::tr( "The password is incorrect" );
   return QString();
}

QString US_MasterPassword::fromTerminal( QString* error, int attempts )
{
   if ( ! isTerminal( stdin ) )
   {
      if ( error ) *error = QObject::tr( "stdin is not a terminal" );
      return QString();
   }
   return promptLoop( stdin, stderr, attempts, error );
}

QString US_MasterPassword::obtainNonInteractive( QString* error )
{
   QString pw = cached();
   if ( ! pw.isEmpty() )
      return pw;

   if ( ! isSet() )
   {
      if ( error ) *error = QObject::tr( "The master password has not been set" );
      return QString();
   }

   return fromKeychain( error );
}

QString US_MasterPassword::obtainHeadless( QString* error )
{
   QStringList reasons;
   QString     err;

   QString pw = obtainNonInteractive( &err );
   if ( ! pw.isEmpty() )
      return pw;
   if ( ! isSet() )
   {
      if ( error ) *error = err;
      return QString();
   }
   reasons << err;

   err.clear();
   pw = fromEnvironment( &err );
   if ( ! pw.isEmpty() )
      return pw;
   reasons << err;

   err.clear();
   pw = fromTerminal( &err );
   if ( ! pw.isEmpty() )
      return pw;
   reasons << err;

   if ( error ) *error = reasons.join( "; " );
   return QString();
}
