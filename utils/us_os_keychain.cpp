//! \file us_os_keychain.cpp
//! Native OS secret-store backends, no third-party libraries:
//! - macOS:   Security.framework generic-password items
//! - Windows: Credential Manager (CredWriteW / CredReadW, advapi32)
//! - Linux:   freedesktop.org Secret Service over Qt D-Bus
//!            (GNOME Keyring, KWallet >= 5.97, KeePassXC, ...)
#include "us_os_keychain.h"

#include <QByteArray>
#include <QObject>

#if defined( Q_OS_MACOS )
#  include <CoreFoundation/CoreFoundation.h>
#  include <Security/Security.h>
#elif defined( Q_OS_WIN )
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <wincred.h>
#elif defined( US3_HAVE_QTDBUS )
#  include <QCoreApplication>
#  include <QDBusArgument>
#  include <QDBusConnection>
#  include <QDBusMessage>
#  include <QDBusMetaType>
#  include <QDBusObjectPath>
#  include <QDBusVariant>
#  include <QEventLoop>
#  include <QList>
#  include <QMap>
#  include <QTimer>
#endif

#include <openssl/crypto.h>   // OPENSSL_cleanse

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
#if defined( Q_OS_MACOS ) || defined( Q_OS_WIN ) || defined( US3_HAVE_QTDBUS )
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

namespace
{
   Q_DECL_UNUSED QString notBuilt( void )
   {
      return QObject::tr( "UltraScan was built without OS keychain support" );
   }

   Q_DECL_UNUSED void wipe( QByteArray& ba )
   {
      if ( ! ba.isEmpty() )
         OPENSSL_cleanse( ba.data(), static_cast<size_t>( ba.size() ) );
      ba.clear();
   }
} // namespace

// ============================================================================
// macOS: Security.framework
// ============================================================================
#if defined( Q_OS_MACOS )
namespace
{
   struct CFRel
   {
      CFTypeRef ref;
      explicit CFRel( CFTypeRef r ) : ref( r ) {}
      ~CFRel() { if ( ref ) CFRelease( ref ); }
   };

   CFStringRef cfString( const QString& s )
   {
      const QByteArray u = s.toUtf8();
      return CFStringCreateWithBytes( kCFAllocatorDefault,
                                      reinterpret_cast<const UInt8*>( u.constData() ),
                                      u.size(), kCFStringEncodingUTF8, false );
   }

   QString osStatusText( OSStatus st )
   {
      CFStringRef msg = SecCopyErrorMessageString( st, nullptr );
      if ( msg == nullptr )
         return QObject::tr( "Keychain error %1" ).arg( st );
      char buf[ 512 ] = { 0 };
      CFStringGetCString( msg, buf, sizeof( buf ), kCFStringEncodingUTF8 );
      CFRelease( msg );
      return QString::fromUtf8( buf );
   }

   // Base query: generic password identified by service + account.
   CFMutableDictionaryRef baseQuery( const QString& key )
   {
      CFMutableDictionaryRef q = CFDictionaryCreateMutable(
            kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks,
            &kCFTypeDictionaryValueCallBacks );
      CFRel service( cfString( US_OsKeychain::serviceName() ) );
      CFRel account( cfString( key ) );
      CFDictionarySetValue( q, kSecClass, kSecClassGenericPassword );
      CFDictionarySetValue( q, kSecAttrService, service.ref );
      CFDictionarySetValue( q, kSecAttrAccount, account.ref );
      return q;
   }
} // namespace
#endif

// ============================================================================
// Windows: Credential Manager
// ============================================================================
#if defined( Q_OS_WIN )
namespace
{
   std::wstring targetName( const QString& key )
   {
      return ( US_OsKeychain::serviceName() + "/" + key ).toStdWString();
   }

   QString winErrorText( DWORD err )
   {
      return QObject::tr( "Credential Manager error %1" ).arg( err );
   }
} // namespace
#endif

// ============================================================================
// Linux: Secret Service API over D-Bus
// https://specifications.freedesktop.org/secret-service-spec/latest/
// The "plain" transfer algorithm is used: the secret travels unencrypted over
// the per-user session bus, which only the same user (or root) can observe.
// ============================================================================
#if !defined( Q_OS_MACOS ) && !defined( Q_OS_WIN ) && defined( US3_HAVE_QTDBUS )

//! D-Bus struct Secret (oayays)
struct US_DBusSecret
{
   QDBusObjectPath session;
   QByteArray      parameters;
   QByteArray      value;
   QString         contentType;
};
Q_DECLARE_METATYPE( US_DBusSecret )

typedef QMap< QString, QString > US_DBusStringMap;
Q_DECLARE_METATYPE( US_DBusStringMap )

QDBusArgument& operator<<( QDBusArgument& arg, const US_DBusSecret& s )
{
   arg.beginStructure();
   arg << s.session << s.parameters << s.value << s.contentType;
   arg.endStructure();
   return arg;
}

const QDBusArgument& operator>>( const QDBusArgument& arg, US_DBusSecret& s )
{
   arg.beginStructure();
   arg >> s.session >> s.parameters >> s.value >> s.contentType;
   arg.endStructure();
   return arg;
}

namespace
{
   const char* SS_SERVICE    = "org.freedesktop.secrets";
   const char* SS_PATH       = "/org/freedesktop/secrets";
   const char* SS_IF_SERVICE = "org.freedesktop.Secret.Service";
   const char* SS_IF_COLL    = "org.freedesktop.Secret.Collection";
   const char* SS_IF_ITEM    = "org.freedesktop.Secret.Item";
   const char* SS_IF_PROMPT  = "org.freedesktop.Secret.Prompt";

   // Waits for org.freedesktop.Secret.Prompt.Completed of one prompt object.
   class PromptWaiter : public QObject
   {
      Q_OBJECT
     public:
      bool          dismissed = true;
      QDBusVariant  result;
      QEventLoop    loop;
     public slots:
      void completed( bool d, const QDBusVariant& r )
      {
         dismissed = d;
         result    = r;
         loop.quit();
      }
   };

   class SecretService
   {
     public:
      SecretService() : bus( QDBusConnection::sessionBus() )
      {
         static bool registered = false;
         if ( ! registered )
         {
            qDBusRegisterMetaType< US_DBusSecret >();
            qDBusRegisterMetaType< US_DBusStringMap >();
            registered = true;
         }
      }

      QString error;

      // Open a "plain" session.
      bool open( void )
      {
         if ( ! bus.isConnected() )
         {
            error = QObject::tr( "No D-Bus session bus: %1" ).arg( bus.lastError().message() );
            return false;
         }
         QDBusMessage reply = call( SS_PATH, SS_IF_SERVICE, "OpenSession",
               { QString( "plain" ), QVariant::fromValue( QDBusVariant( QString() ) ) } );
         if ( ! ok( reply, 2 ) ) return false;
         session = reply.arguments().at( 1 ).value< QDBusObjectPath >();
         return true;
      }

      void close( void )
      {
         if ( ! session.path().isEmpty() && session.path() != "/" )
            call( session.path(), "org.freedesktop.Secret.Session", "Close", {} );
         session = QDBusObjectPath();
      }

      static US_DBusStringMap attributes( const QString& key )
      {
         US_DBusStringMap a;
         a[ "xdg:schema"  ] = "org.ultrascan3.Password";
         a[ "application" ] = US_OsKeychain::serviceName();
         a[ "key"         ] = key;
         return a;
      }

      // Find the item for key; unlocks it if needed. Empty path: none.
      bool findItem( const QString& key, QDBusObjectPath& item )
      {
         item = QDBusObjectPath();
         QDBusMessage reply = call( SS_PATH, SS_IF_SERVICE, "SearchItems",
               { QVariant::fromValue( attributes( key ) ) } );
         if ( ! ok( reply, 2 ) ) return false;

         QList< QDBusObjectPath > unlocked = paths( reply.arguments().at( 0 ) );
         QList< QDBusObjectPath > locked   = paths( reply.arguments().at( 1 ) );
         if ( ! unlocked.isEmpty() )
         {
            item = unlocked.first();
            return true;
         }
         if ( locked.isEmpty() )
            return true;   // not found

         if ( ! unlock( { locked.first() } ) ) return false;
         item = locked.first();
         return true;
      }

      bool getSecret( const QDBusObjectPath& item, QByteArray& value )
      {
         QDBusMessage reply = call( item.path(), SS_IF_ITEM, "GetSecret",
               { QVariant::fromValue( session ) } );
         if ( ! ok( reply, 1 ) ) return false;
         US_DBusSecret s;
         reply.arguments().at( 0 ).value< QDBusArgument >() >> s;
         value = s.value;
         wipe( s.value );
         return true;
      }

      bool createItem( const QString& key, const QByteArray& value )
      {
         QDBusObjectPath coll;
         if ( ! defaultCollection( coll ) ) return false;
         if ( ! unlock( { coll } ) ) return false;

         QVariantMap props;
         props[ "org.freedesktop.Secret.Item.Label" ] =
            QObject::tr( "UltraScan3 %1" ).arg( key );
         props[ "org.freedesktop.Secret.Item.Attributes" ] =
            QVariant::fromValue( attributes( key ) );

         US_DBusSecret s;
         s.session     = session;
         s.value       = value;
         s.contentType = "text/plain; charset=utf8";

         QDBusMessage reply = call( coll.path(), SS_IF_COLL, "CreateItem",
               { props, QVariant::fromValue( s ), true } );
         wipe( s.value );
         if ( ! ok( reply, 2 ) ) return false;
         return waitPrompt( reply.arguments().at( 1 ).value< QDBusObjectPath >() );
      }

      bool deleteItem( const QDBusObjectPath& item )
      {
         QDBusMessage reply = call( item.path(), SS_IF_ITEM, "Delete", {} );
         if ( ! ok( reply, 1 ) ) return false;
         return waitPrompt( reply.arguments().at( 0 ).value< QDBusObjectPath >() );
      }

     private:
      QDBusConnection bus;
      QDBusObjectPath session;

      QDBusMessage call( const QString& path, const char* iface, const char* method,
                         const QVariantList& args )
      {
         QDBusMessage msg = QDBusMessage::createMethodCall( SS_SERVICE, path, iface, method );
         msg.setArguments( args );
         // Unlocking may show a dialog; methods return at once with a prompt
         // object, so a short timeout suffices for the calls themselves.
         return bus.call( msg, QDBus::Block, 25000 );
      }

      bool ok( const QDBusMessage& reply, int nargs )
      {
         if ( reply.type() == QDBusMessage::ErrorMessage )
         {
            error = reply.errorName() == "org.freedesktop.DBus.Error.ServiceUnknown"
                    ? QObject::tr( "No Secret Service (e.g. gnome-keyring, KWallet) is running" )
                    : reply.errorMessage();
            return false;
         }
         if ( reply.arguments().size() < nargs )
         {
            error = QObject::tr( "Unexpected Secret Service reply" );
            return false;
         }
         return true;
      }

      static QList< QDBusObjectPath > paths( const QVariant& v )
      {
         QList< QDBusObjectPath > list;
         if ( v.canConvert< QDBusArgument >() )
            v.value< QDBusArgument >() >> list;
         else
            list = v.value< QList< QDBusObjectPath > >();
         return list;
      }

      bool defaultCollection( QDBusObjectPath& coll )
      {
         QDBusMessage reply = call( SS_PATH, SS_IF_SERVICE, "ReadAlias", { QString( "default" ) } );
         if ( ! ok( reply, 1 ) ) return false;
         coll = reply.arguments().at( 0 ).value< QDBusObjectPath >();
         if ( coll.path().isEmpty() || coll.path() == "/" )
         {
            // Fall back to the login keyring that most implementations provide.
            coll = QDBusObjectPath( "/org/freedesktop/secrets/aliases/default" );
         }
         return true;
      }

      bool unlock( const QList< QDBusObjectPath >& objects )
      {
         QDBusMessage reply = call( SS_PATH, SS_IF_SERVICE, "Unlock",
               { QVariant::fromValue( objects ) } );
         if ( ! ok( reply, 2 ) ) return false;
         return waitPrompt( reply.arguments().at( 1 ).value< QDBusObjectPath >() );
      }

      // Run a prompt (e.g. "unlock keyring") and wait for it to complete.
      bool waitPrompt( const QDBusObjectPath& prompt )
      {
         if ( prompt.path().isEmpty() || prompt.path() == "/" )
            return true;   // no prompt needed

         if ( QCoreApplication::instance() == nullptr )
         {
            error = QObject::tr( "The keyring is locked and cannot be unlocked without an application" );
            return false;
         }

         PromptWaiter waiter;
         bus.connect( SS_SERVICE, prompt.path(), SS_IF_PROMPT, "Completed",
                      &waiter, SLOT( completed( bool, QDBusVariant ) ) );
         QDBusMessage reply = call( prompt.path(), SS_IF_PROMPT, "Prompt", { QString() } );
         if ( reply.type() == QDBusMessage::ErrorMessage )
         {
            error = reply.errorMessage();
            return false;
         }
         QTimer::singleShot( 300000, &waiter.loop, &QEventLoop::quit );   // 5 min
         waiter.loop.exec();
         bus.disconnect( SS_SERVICE, prompt.path(), SS_IF_PROMPT, "Completed",
                         &waiter, SLOT( completed( bool, QDBusVariant ) ) );
         if ( waiter.dismissed )
         {
            error = QObject::tr( "The keyring prompt was dismissed" );
            return false;
         }
         return true;
      }
   };
} // namespace
#endif

// ============================================================================
// Public API
// ============================================================================
bool US_OsKeychain::writeSecret( const QString& key, const QString& secret,
                                 QString* error )
{
#if defined( Q_OS_MACOS )
   QByteArray data = secret.toUtf8();
   CFMutableDictionaryRef q = baseQuery( key );
   CFRel qrel( q );
   CFRel value( CFDataCreate( kCFAllocatorDefault,
                              reinterpret_cast<const UInt8*>( data.constData() ),
                              data.size() ) );
   wipe( data );

   CFMutableDictionaryRef upd = CFDictionaryCreateMutable(
         kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks,
         &kCFTypeDictionaryValueCallBacks );
   CFRel urel( upd );
   CFDictionarySetValue( upd, kSecValueData, value.ref );

   OSStatus st = SecItemUpdate( q, upd );
   if ( st == errSecItemNotFound )
   {
      CFDictionarySetValue( q, kSecValueData, value.ref );
      CFDictionarySetValue( q, kSecAttrAccessible, kSecAttrAccessibleWhenUnlocked );
      st = SecItemAdd( q, nullptr );
   }
   if ( st != errSecSuccess )
   {
      if ( error ) *error = osStatusText( st );
      return false;
   }
   return true;

#elif defined( Q_OS_WIN )
   const std::wstring target  = targetName( key );
   const std::wstring account = key.toStdWString();
   QByteArray blob = secret.toUtf8();

   CREDENTIALW cred;
   ZeroMemory( &cred, sizeof( cred ) );
   cred.Type               = CRED_TYPE_GENERIC;
   cred.TargetName         = const_cast<LPWSTR>( target.c_str() );
   cred.UserName           = const_cast<LPWSTR>( account.c_str() );
   cred.CredentialBlobSize = static_cast<DWORD>( blob.size() );
   cred.CredentialBlob     = reinterpret_cast<LPBYTE>( blob.data() );
   cred.Persist            = CRED_PERSIST_LOCAL_MACHINE;   // this user, this machine

   const BOOL ok = CredWriteW( &cred, 0 );
   const DWORD err = ok ? 0 : GetLastError();
   wipe( blob );
   if ( ! ok )
   {
      if ( error ) *error = winErrorText( err );
      return false;
   }
   return true;

#elif defined( US3_HAVE_QTDBUS )
   SecretService ss;
   QByteArray value = secret.toUtf8();
   bool ok = ss.open() && ss.createItem( key, value );   // replaces existing
   wipe( value );
   ss.close();
   if ( ! ok && error ) *error = ss.error;
   return ok;

#else
   Q_UNUSED( key ); Q_UNUSED( secret );
   if ( error ) *error = notBuilt();
   return false;
#endif
}

bool US_OsKeychain::readSecret( const QString& key, QString& secret, QString* error )
{
   secret.clear();
#if defined( Q_OS_MACOS )
   CFMutableDictionaryRef q = baseQuery( key );
   CFRel qrel( q );
   CFDictionarySetValue( q, kSecReturnData, kCFBooleanTrue );
   CFDictionarySetValue( q, kSecMatchLimit, kSecMatchLimitOne );

   CFTypeRef result = nullptr;
   const OSStatus st = SecItemCopyMatching( q, &result );
   CFRel rrel( result );
   if ( st != errSecSuccess || result == nullptr )
   {
      if ( error ) *error = ( st == errSecItemNotFound )
                            ? QObject::tr( "No keychain entry" ) : osStatusText( st );
      return false;
   }
   CFDataRef data = static_cast<CFDataRef>( result );
   QByteArray bytes( reinterpret_cast<const char*>( CFDataGetBytePtr( data ) ),
                     static_cast<int>( CFDataGetLength( data ) ) );
   secret = QString::fromUtf8( bytes );
   wipe( bytes );
   return true;

#elif defined( Q_OS_WIN )
   const std::wstring target = targetName( key );
   PCREDENTIALW cred = nullptr;
   if ( ! CredReadW( target.c_str(), CRED_TYPE_GENERIC, 0, &cred ) )
   {
      const DWORD err = GetLastError();
      if ( error ) *error = ( err == ERROR_NOT_FOUND )
                            ? QObject::tr( "No keychain entry" ) : winErrorText( err );
      return false;
   }
   QByteArray bytes( reinterpret_cast<const char*>( cred->CredentialBlob ),
                     static_cast<int>( cred->CredentialBlobSize ) );
   SecureZeroMemory( cred->CredentialBlob, cred->CredentialBlobSize );
   CredFree( cred );
   secret = QString::fromUtf8( bytes );
   wipe( bytes );
   return true;

#elif defined( US3_HAVE_QTDBUS )
   SecretService   ss;
   QDBusObjectPath item;
   QByteArray      value;
   bool ok = ss.open() && ss.findItem( key, item );
   if ( ok && item.path().isEmpty() )
   {
      ss.error = QObject::tr( "No keychain entry" );
      ok = false;
   }
   ok = ok && ss.getSecret( item, value );
   ss.close();
   if ( ! ok )
   {
      if ( error ) *error = ss.error;
      return false;
   }
   secret = QString::fromUtf8( value );
   wipe( value );
   return true;

#else
   Q_UNUSED( key );
   if ( error ) *error = notBuilt();
   return false;
#endif
}

bool US_OsKeychain::deleteSecret( const QString& key, QString* error )
{
#if defined( Q_OS_MACOS )
   CFMutableDictionaryRef q = baseQuery( key );
   CFRel qrel( q );
   const OSStatus st = SecItemDelete( q );
   if ( st == errSecSuccess || st == errSecItemNotFound )
      return true;
   if ( error ) *error = osStatusText( st );
   return false;

#elif defined( Q_OS_WIN )
   const std::wstring target = targetName( key );
   if ( CredDeleteW( target.c_str(), CRED_TYPE_GENERIC, 0 ) )
      return true;
   const DWORD err = GetLastError();
   if ( err == ERROR_NOT_FOUND )
      return true;
   if ( error ) *error = winErrorText( err );
   return false;

#elif defined( US3_HAVE_QTDBUS )
   SecretService   ss;
   QDBusObjectPath item;
   bool ok = ss.open() && ss.findItem( key, item );
   if ( ok && ! item.path().isEmpty() )
      ok = ss.deleteItem( item );
   ss.close();
   if ( ! ok && error ) *error = ss.error;
   return ok;

#else
   Q_UNUSED( key );
   if ( error ) *error = notBuilt();
   return false;
#endif
}

#if !defined( Q_OS_MACOS ) && !defined( Q_OS_WIN ) && defined( US3_HAVE_QTDBUS )
#  include "us_os_keychain.moc"
#endif
