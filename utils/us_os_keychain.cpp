//! \file us_os_keychain.cpp
#include "us_os_keychain.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QObject>

#ifdef US3_HAVE_QTKEYCHAIN
#  if QT_VERSION >= QT_VERSION_CHECK( 6, 0, 0 )
#    include <qt6keychain/keychain.h>
#  else
#    include <qt5keychain/keychain.h>
#  endif
#endif

QString US_OsKeychain::serviceName( void )
{
   return QStringLiteral( "UltraScan3" );
}

QString US_OsKeychain::masterPasswordKey( void )
{
   return QStringLiteral( "MasterPassword" );
}

bool US_OsKeychain::isAvailable( void )
{
#ifdef US3_HAVE_QTKEYCHAIN
   return true;
#else
   return false;
#endif
}

QString US_OsKeychain::backendName( void )
{
#if defined( Q_OS_MACOS )
   return QObject::tr( "macOS Keychain" );
#elif defined( Q_OS_WIN )
   return QObject::tr( "Windows Credential Manager" );
#else
   return QObject::tr( "Secret Service (GNOME Keyring / KWallet)" );
#endif
}

#ifdef US3_HAVE_QTKEYCHAIN
namespace
{
   // QtKeychain jobs are asynchronous; run one to completion in a local
   // event loop. Requires a QCoreApplication instance.
   bool runJob( QKeychain::Job& job, QString* error )
   {
      if ( QCoreApplication::instance() == nullptr )
      {
         if ( error ) *error = QObject::tr( "No application instance" );
         return false;
      }

      job.setAutoDelete( false );
      job.setInsecureFallback( false );   // never store secrets in plain text

      QEventLoop loop;
      QObject::connect( &job, &QKeychain::Job::finished, &loop, &QEventLoop::quit );
      job.start();
      loop.exec();

      if ( job.error() != QKeychain::NoError )
      {
         if ( error ) *error = job.errorString();
         return false;
      }
      return true;
   }
} // namespace
#endif

bool US_OsKeychain::writeSecret( const QString& key, const QString& secret,
                                 QString* error )
{
#ifdef US3_HAVE_QTKEYCHAIN
   QKeychain::WritePasswordJob job( serviceName() );
   job.setKey( key );
   job.setTextData( secret );
   return runJob( job, error );
#else
   Q_UNUSED( key ); Q_UNUSED( secret );
   if ( error ) *error = QObject::tr( "UltraScan was built without OS keychain support" );
   return false;
#endif
}

bool US_OsKeychain::readSecret( const QString& key, QString& secret, QString* error )
{
   secret.clear();
#ifdef US3_HAVE_QTKEYCHAIN
   QKeychain::ReadPasswordJob job( serviceName() );
   job.setKey( key );
   if ( ! runJob( job, error ) )
      return false;
   secret = job.textData();
   return true;
#else
   Q_UNUSED( key );
   if ( error ) *error = QObject::tr( "UltraScan was built without OS keychain support" );
   return false;
#endif
}

bool US_OsKeychain::deleteSecret( const QString& key, QString* error )
{
#ifdef US3_HAVE_QTKEYCHAIN
   QKeychain::DeletePasswordJob job( serviceName() );
   job.setKey( key );
   QString err;
   if ( runJob( job, &err ) )
      return true;
   // Deleting an entry that does not exist is not an error for callers.
   if ( job.error() == QKeychain::EntryNotFound )
      return true;
   if ( error ) *error = err;
   return false;
#else
   Q_UNUSED( key );
   if ( error ) *error = QObject::tr( "UltraScan was built without OS keychain support" );
   return false;
#endif
}
