//! \file us_db2.cpp
#include "us_db2.h"
#ifndef NO_DB
#include "errmsg.h"
#include "mysqld_error.h"
#endif
#include "us_settings.h"
#include "us_crypto.h"
#include "us_gzip.h"
#include "us_util.h"

#include <QRegularExpression>

#ifndef NO_DB
namespace
{
// Every string goes to the server as UTF-8 and comes back as UTF-8, so the
// connection must say so.  Left to the client library, the character set
// is whatever it was built with -- latin1 for some libmariadb builds,
// utf8mb4 for others -- and a query encoded one way and read the other
// fails on the first accented letter or stores a '?' in its place.
const char* const connectionCharset = "utf8mb4";

// Ask for the connection character set before connecting
bool request_utf8( MYSQL* conn, QString& err )
{
   if ( mysql_options( conn, MYSQL_SET_CHARSET_NAME, connectionCharset ) != 0 )
   {
      err = QString( "Error setting the connection character set to %1" ).arg( connectionCharset );
      return false;
   }
   return true;
}

// Put the character set on the session itself, and check that the server
// uses it.  The handshake only proposes one: a server started with
// skip-character-set-client-handshake, or one whose init_connect runs SET
// NAMES, keeps its own without saying so, and mysql_character_set_name()
// reports the client's setting, not the session's -- every query would then
// be read as Latin-1 and stored as mojibake.  mysql_set_character_set()
// sends SET NAMES, and the session variables say what the server will use
// to read a query, compare it, and send back results.
bool confirm_utf8( MYSQL* conn, QString& err )
{
   if ( mysql_set_character_set( conn, connectionCharset ) != 0 )
   {
      err = QString( "The server did not accept the %1 connection character set: %2" )
            .arg( connectionCharset, QString::fromUtf8( mysql_error( conn ) ) );
      return false;
   }

   const QByteArray check( "SELECT @@character_set_client, @@character_set_connection,"
                           " @@character_set_results" );
   if ( mysql_real_query( conn, check.constData(), static_cast< unsigned long >( check.size() ) ) != 0 )
   {
      err = QString( "The connection character set could not be checked: %1" )
            .arg( QString::fromUtf8( mysql_error( conn ) ) );
      return false;
   }

   QStringList found;
   MYSQL_RES*  result = mysql_store_result( conn );
   if ( result != nullptr )
   {
      MYSQL_ROW row = mysql_fetch_row( result );
      if ( row != nullptr  &&  mysql_num_fields( result ) == 3 )
      {
         for ( int ii = 0; ii < 3; ii++ )
            found << ( row[ ii ] != nullptr ? QString::fromUtf8( row[ ii ] ) : QString( "NULL" ) );
      }
      mysql_free_result( result );
   }

   // Nothing else is pending, but leave the connection ready all the same
   while ( mysql_next_result( conn ) == 0 )
   {
      MYSQL_RES* more = mysql_store_result( conn );
      if ( more != nullptr )
         mysql_free_result( more );
   }

   const QString wanted = QString::fromLatin1( connectionCharset );
   if ( found.size() == 3  &&  found.count( wanted ) == 3 )
      return true;

   err = QString( "The server does not use the %1 character set on this connection"
                  " (client, connection, results: %2), so text would be stored wrongly" )
         .arg( wanted, found.isEmpty() ? QString( "unknown" ) : found.join( ", " ) );
   return false;
}

// Run a statement given as text, as the UTF-8 the connection expects
int run_utf8( MYSQL* conn, const QString& statement )
{
   const QByteArray bytes = statement.toUtf8();
   return mysql_real_query( conn, bytes.constData(), static_cast< unsigned long >( bytes.size() ) );
}
}
#endif

#define CIPHER \
/* TLS 1.2 (Prioritize Galois/Counter Mode) */ \
"ECDHE-ECDSA-AES256-GCM-SHA384:" \
"ECDHE-RSA-AES256-GCM-SHA384:" \
"ECDHE-ECDSA-AES128-GCM-SHA256:" \
"ECDHE-RSA-AES128-GCM-SHA256:" \
/* TLS 1.2 (CCM) */ \
"AES256-CCM:" \
"AES128-CCM:" \
/* TLS 1.2 (CBC + Forward Secrecy) */ \
"ECDHE-ECDSA-AES256-SHA384:" \
"ECDHE-RSA-AES256-SHA384:" \
"ECDHE-ECDSA-AES128-SHA256:" \
"ECDHE-RSA-AES128-SHA256:" \
/* LEGACY FALLBACKS (For very old servers/SHA1) */ \
"ECDHE-ECDSA-AES256-SHA:" \
"ECDHE-RSA-AES256-SHA:" \
"ECDHE-ECDSA-AES128-SHA:" \
"ECDHE-RSA-AES128-SHA:" \
"AES256-GCM-SHA384:" \
"AES128-GCM-SHA256:" \
"AES256-SHA256:" \
"AES128-SHA256:" \
"AES256-SHA:" \
"AES128-SHA"

US_DB2::US_DB2()
{
#ifndef NO_DB
   caFile = US_Settings::appBaseDir() + QString( "/etc/mysql/ca-cert.pem" );

   connected  = false;
   result     = NULL;
   db         = mysql_init( NULL );
#endif
}

#ifdef NO_DB
US_DB2::US_DB2( const QString& ){}
#else
US_DB2::US_DB2( const QString& masterPW )
{
   caFile = US_Settings::appBaseDir() + QString( "/etc/mysql/ca-cert.pem" );

   connected  = false;
   result     = NULL;
   db         = mysql_init( NULL );

   QString err;
   if ( ! connect( masterPW, err ) )
   {
      db_errno = NOT_CONNECTED;
      error = "US_DB2 error: could not connect\n" + err;
      return;
   }

   db_errno  = OK;
   error     = "";
   connected = true;
}
#endif

US_DB2::~US_DB2()
{
#ifndef NO_DB
   // Closing would end the transaction anyway; roll it back explicitly so a
   // dangling one is undone by us, not by however the server treats the close.
   if ( inTransaction && connected )
   {
      drainResults();
      mysql_rollback( db );
   }

  mysql_free_result( result );
  mysql_close(db);

  result      = NULL;
  connected   = false;
#endif
}

QString US_DB2::lastError() {
    return error;
}

int US_DB2::lastErrno() {
    return db_errno;
}

int US_DB2::lastServerErrno() {
    return server_errno;
}

#ifdef NO_DB
bool US_DB2::test_db_connection( const QString&, const QString&,
      const QString&, const QString&, QString& ) { return false; }
#else
bool US_DB2::test_db_connection( 
        const QString& host, const QString& dbname, 
        const QString& user, const QString& password, 
        QString& error )
{
   error = "";

   MYSQL* conn = mysql_init( NULL );
   if ( ! conn )
   {
      error = QString( "Test connection allocation error" );
      return false;
   }

   if ( ! request_utf8( conn, error ) )
   {
      mysql_close( conn );
      return false;
   }

   QString uhost  = host.section( ":", 0, 0 ).simplified();
   int     uport  = host.section( ":", 1, 1 ).simplified().toInt();

   bool status = mysql_real_connect(
                 conn,
                 uhost   .toUtf8().constData(),
                 user    .toUtf8().constData(),
                 password.toUtf8().constData(),
                 dbname  .toUtf8().constData(),
                 uport, NULL, CLIENT_MULTI_STATEMENTS );
 
   if ( ! status )
      error = QString( "Test connection open error\n" ) + mysql_error( conn );

   else if ( ! confirm_utf8( conn, error ) )
      status = false;

   mysql_close( conn );

   return status;
}
#endif

#ifdef NO_DB
bool US_DB2::test_secure_connection( 
      const QString&, const QString&,
      const QString&, const QString&, 
      const QString&,  const QString&, 
      QString& ) { return false; }
#else
bool US_DB2::test_secure_connection( 
        const QString& host,  const QString& dbname, 
        const QString& user,  const QString& password, 
        const QString& email, const QString& pw, 
        QString&       err )
{
   error = "";

   if ( ! db )
   {
      error = QString( "Test secure connection allocation error" );
      err   = error;
      return false;
   }

   const bool config_status = configure_ssl( err );
   if ( ! config_status ) {
      err = error;
      return false;
   }

   if ( ! request_utf8( db, error ) ) {
      err = error;
      return false;
   }

   QString uhost  = host.section( ":", 0, 0 ).simplified();
   int     uport  = host.section( ":", 1, 1 ).simplified().toInt();

   bool status = mysql_real_connect(
                 db,
                 uhost   .toUtf8().constData(),
                 user    .toUtf8().constData(),
                 password.toUtf8().constData(),
                 dbname  .toUtf8().constData(),
                 uport, NULL, CLIENT_MULTI_STATEMENTS );
 
   if ( ! status )
   {
      error = QString( "Test secure connection open error\n" ) +
              mysql_error( db );
      err   = mysql_error( db );
      return false;
   }

   if ( ! confirm_utf8( db, error ) )
   {
      err = error;
      return false;
   }

   // Let's see if the user can log in
   status    = false;
   // validate_user takes (guid, email, password): no guid yet, and the email
   // goes in the slot composeQuery reserves for the password.
   QString q = composeQuery( "CALL", QStringList() << "validate_user" << pw, QString(), email );

   this->query( q );

   if ( db_errno != OK )
   {
      error = mysql_error( db );
      err   = error;
      return false;
   }
   
   next();  // Setup for calling routine to just use value()

   return true;
}
#endif

#ifdef NO_DB
bool US_DB2::connect( const QString&, QString& ){ return false; }
#else
bool US_DB2::connect( const QString& masterPW, QString& err )
{
   if ( connected ) return true;

   resetLostHandle();

   QStringList defaultDB = US_Settings::defaultDB();
   if ( defaultDB.size() < 6 )
   {
       db_errno = NOT_CONNECTED;
       error = "US_DB2 error: DB not configured";
       err = error;
       return false;
   }

   QString user     = defaultDB.at( 1 );
   QString dbname   = defaultDB.at( 2 );
   QString host     = defaultDB.at( 3 );
   QString cipher   = defaultDB.at( 4 );
   QString iv       = defaultDB.at( 5 );  // Initialization vector

   QString password = US_Crypto::decrypt( cipher, masterPW, iv );
   const bool config_status = configure_ssl( err ) && request_utf8( db, err );
   if ( ! config_status ) {
      db_errno = NOT_CONNECTED;
      error = QString( "Connect open error:\n" ) + err;
      err = error;
      return false;
   }
   try
   {
      // The CLIENT_MULTI_STATEMENTS flag allows for multiple queries and
      //   multiple result sets from a single stored procedure. It is required
      //   for any stored procedure that returns result sets.
      QString uhost  = host.section( ":", 0, 0 ).simplified();
      int     uport  = host.section( ":", 1, 1 ).simplified().toInt();
//qDebug() << "DB: connect uport" << uport << "uhost" << uhost;

      connected = mysql_real_connect(
                  db,
                  uhost   .toUtf8().constData(),
                  user    .toUtf8().constData(),
                  password.toUtf8().constData(),
                  dbname  .toUtf8().constData(),
                  uport, NULL, CLIENT_MULTI_STATEMENTS );
 
   }

   catch ( std::exception &e )
   {
      db_errno = NOT_CONNECTED;
      error = e.what();
      err = "US_DB2: uncaught exception " + error;
      return false;
   }

   db_errno = OK;
   error = "";
   inTransaction        = false;
   rolledBackByDeadlock = false;
   lostCommitError.clear();
   server_version       = connected ? QString::fromUtf8( mysql_get_server_info( db ) ) : QString();

   if ( ! connected )
   {
      db_errno = NOT_CONNECTED;
      error = QString( "Connect open error: " ) + mysql_error( db );
      err = error;
      return false;
   }

   if ( ! confirm_utf8( db, err ) )
   {
      connected = false;
      db_errno  = NOT_CONNECTED;
      error     = QString( "Connect open error: " ) + err;
      err       = error;
      return false;
   }

   email  = defaultDB.at( 6 );  // Save for later

   // DB Internal PW
   cipher = defaultDB.at( 7 );
   iv     = defaultDB.at( 8 );
   userPW = US_Crypto::decrypt( cipher, masterPW, iv );
   guid   = defaultDB.at( 9 );

   // validate_user takes (guid, email, password), so the password goes in the
   // first caller-argument slot and the empty email in the password slot.
   QString q = composeQuery( "CALL", QStringList() << "validate_user" << userPW, guid, QString() );

   this->query( q );

   if ( db_errno != OK )
   {
      err = error;
      return false;
   }
   
   next();  // Setup for calling routine to just use value()

   // See if email was changed in the database
   if ( email != this->value( 1 ).toString() )
   {
      QList< QStringList > dbinfo = US_Settings::databases();

      for ( int i = 0; i < dbinfo.size(); i++ )
      {
         QStringList info = dbinfo.at( i );
         
         if ( info.at( 9 ) == guid )  // Found it
         {
            email = this->value( 1 ).toString();
            info.replace( 6, email );
            US_Settings::set_defaultDB( info );  // Update the current DB 
            dbinfo.replace( i, info );
            break;
         }
      }

      US_Settings::set_databases( dbinfo );  // Update the full DB list
   }

   return connected;
}
#endif

#ifdef NO_DB
bool US_DB2::connect( const QString&, const QString&, const QString&,
                      const QString&, QString& ){ return false; }
#else
bool US_DB2::connect( 
        const QString& host,  const QString& dbname, 
        const QString& user,  const QString& password, 
        QString& err )
{
  if ( connected ) return true;

  resetLostHandle();

  const bool config_status = configure_ssl( err ) && request_utf8( db, err );
  if ( ! config_status ) {
     db_errno = NOT_CONNECTED;
     error = QString( "Connect open error:\n" ) + err;
     err = error;
     return false;
  }
   try
   {
      // The CLIENT_MULTI_STATEMENTS flag allows for multiple queries and
      //   multiple result sets from a single stored procedure. It is required
      //   for any stored procedure that returns result sets.
      QString uhost  = host.section( ":", 0, 0 ).simplified();
      int     uport  = host.section( ":", 1, 1 ).simplified().toInt();

      connected = mysql_real_connect(
                  db,
                  uhost   .toUtf8().constData(),
                  user    .toUtf8().constData(),
                  password.toUtf8().constData(),
                  dbname  .toUtf8().constData(),
                  uport, NULL, CLIENT_MULTI_STATEMENTS );
   }

   catch ( std::exception &e )
   {
      db_errno = NOT_CONNECTED;
      error = e.what();
      err = "US_DB2: uncaught exception " + error;
      return false;
   }

   db_errno = OK;
   error = "";
   inTransaction        = false;
   rolledBackByDeadlock = false;
   lostCommitError.clear();
   server_version       = connected ? QString::fromUtf8( mysql_get_server_info( db ) ) : QString();
   if ( ! connected )
   {
      db_errno = NOT_CONNECTED;
      error = QString( "Connect open error: " ) + mysql_error( db );
      err = error;
      return false;
   }

   if ( ! confirm_utf8( db, err ) )
   {
      connected = false;
      db_errno  = NOT_CONNECTED;
      error     = QString( "Connect open error: " ) + err;
      err       = error;
      return false;
   }

  return connected;
}
#endif

#ifdef NO_DB
bool US_DB2::connectAuthenticated( const QString&,
                                   const QString&,
                                   const QString&,
                                   const QString&,
                                   const QString&,
                                   const QString&,
                                   QString& err )
{
   err = "Database support is disabled";
   return false;
}
#else
bool US_DB2::connectAuthenticated( const QString& host,
                                   const QString& dbname,
                                   const QString& user,
                                   const QString& password,
                                   const QString& personGUID,
                                   const QString& personPassword,
                                   QString&       err )
{
   if ( !connect( host, dbname, user, password, err ) )
   {
      return false;
   }

   guid   = personGUID;
   userPW = personPassword;

   // validate_user takes (guid, email, password), so the password goes in the
   // first caller-argument slot and the empty email in the password slot.
   query( composeQuery( "CALL", QStringList() << "validate_user" << userPW, guid, QString() ) );

   if ( db_errno != OK )
   {
      err = error;
      return false;
   }

   // Leave the row where the profile connect leaves it, so callers that read
   // value() straight after connecting behave the same either way.
   next();
   email = value( 1 ).toString();

   return connected;
}
#endif

#ifdef NO_DB
bool US_DB2::beginTransaction( QString& transaction_error )
{
   transaction_error = QObject::tr( notConnectedMessage ).arg( "beginTransaction" );
   return false;
}
bool US_DB2::commitTransaction( QString& transaction_error )
{
   transaction_error = QObject::tr( notConnectedMessage ).arg( "commitTransaction" );
   return false;
}
bool US_DB2::rollbackTransaction( QString& transaction_error )
{
   transaction_error = QObject::tr( notConnectedMessage ).arg( "rollbackTransaction" );
   return false;
}
#else
bool US_DB2::beginTransaction( QString& transaction_error )
{
   if ( !connected )
   {
      transaction_error = QObject::tr( notConnectedMessage ).arg( "beginTransaction" );
      return false;
   }

   if ( inTransaction )
   {
      transaction_error = QObject::tr( transactionOpenMessage ).arg( "beginTransaction" );
      return false;
   }

   // Whatever ran before this call may have left results pending.
   drainResults();

   // START TRANSACTION rather than turning autocommit off: the server restores
   // autocommit itself at the COMMIT or ROLLBACK, so there is no second step
   // that can fail after the transaction has already ended.
   if ( mysql_query( db, "START TRANSACTION" ) != 0 )
   {
      transaction_error = mysql_error( db );
      noteConnectionLoss();
      return false;
   }

   inTransaction        = true;
   rolledBackByDeadlock = false;
   lostCommitError.clear();
   transaction_error.clear();
   return true;
}

bool US_DB2::commitTransaction( QString& transaction_error )
{
   if ( !connected )
   {
      // A statement of the transaction lost the connection, and the server
      // rolled the transaction back when it ended; nothing was committed.
      transaction_error = inTransaction && connection_lost
                             ? transactionEndedText()
                             : QObject::tr( notConnectedMessage ).arg( "commitTransaction" );
      return false;
   }

   if ( !inTransaction )
   {
      transaction_error = QObject::tr( noTransactionMessage ).arg( "commitTransaction" );
      return false;
   }

   // The last query of the transaction left its results on the connection, and
   // a CALL left a status set behind them.  mysql_commit() talks to the server
   // directly rather than through rawQuery(), so without this it is answered
   // with "Commands out of sync" and the whole transaction is lost at the last
   // step -- with the rollback that follows failing for the same reason.
   drainResults();

   // A COMMIT after the transaction has ended succeeds as a no-op, so check
   // first or lost writes would be reported as committed.  The client does
   // not always see what ended it: a procedure can catch a deadlock itself,
   // or roll back on its own.
   if ( transactionEndedEarly() )
   {
      transaction_error = transactionEndedText();
      return false;
   }

   if ( mysql_commit( db ) != 0 )
   {
      // The server may have committed before the connection dropped; only a
      // lost connection leaves that open, so only then is the outcome unknown.
      transaction_error =
         commitErrorText( int( mysql_errno( db ) ), QString::fromUtf8( mysql_error( db ) ), server_version );
      noteConnectionLoss();

      // The rollback that follows must not claim that nothing was kept
      if ( connection_lost )
         lostCommitError = transaction_error;

      // Still open as far as the caller is concerned: it rolls back next.
      return false;
   }

   inTransaction = false;
   transaction_error.clear();
   return true;
}

bool US_DB2::rollbackTransaction( QString& transaction_error )
{
   if ( !connected )
   {
      // After a lost connection the server has rolled the transaction back
      // itself; there is nothing left to roll back, and nothing was kept.
      transaction_error = inTransaction && connection_lost
                             ? transactionEndedText()
                             : QObject::tr( notConnectedMessage ).arg( "rollbackTransaction" );
      inTransaction     = false;
      return false;
   }

   if ( !inTransaction )
   {
      transaction_error = QObject::tr( noTransactionMessage ).arg( "rollbackTransaction" );
      return false;
   }

   // Same reason as commitTransaction(), and it matters more here: a rollback
   // that cannot run is a partial load left in place.
   drainResults();

   inTransaction = false;

   // If the server no longer has the transaction open, something ended it
   // before this call and the rollback below undoes nothing, so a quiet
   // success here would be a lie.
   const bool ended_early = transactionEndedEarly();

   // The check found the connection gone: the server rolls back a
   // transaction whose connection ends, so that is the outcome, and the
   // ROLLBACK below could not be sent anyway
   if ( ended_early && connection_lost )
   {
      transaction_error = transactionEndedText();
      return false;
   }

   if ( mysql_rollback( db ) != 0 )
   {
      transaction_error = mysql_error( db );
      noteConnectionLoss();
      if ( connection_lost )
         transaction_error = transactionEndedText();
      return false;
   }

   if ( ended_early )
   {
      transaction_error = transactionEndedText();
      return false;
   }

   transaction_error.clear();
   return true;
}
#endif

#ifndef NO_DB
bool US_DB2::transactionEndedEarly()
{
   // The status comes from the last statement that succeeded; an error packet
   // does not carry one, so it may predate a rollback by the server.  One
   // empty statement brings it up to date.  A connection that cannot run even
   // that has lost the transaction with it.
   if ( mysql_query( db, "DO 0" ) != 0 )
   {
      noteConnectionLoss();
      return true;
   }

   // The C API has no accessor for the status flags; MYSQL documents the field.
   return ( db->server_status & SERVER_STATUS_IN_TRANS ) == 0;
}

void US_DB2::noteStatementError()
{
   // InnoDB always answers a deadlock by rolling back the whole transaction,
   // so that error alone says what ended it.  A lock wait timeout does so only
   // when innodb_rollback_on_timeout is set, and is not recorded.  Remember
   // the error while it is still on the connection; the next statement
   // replaces it.
   if ( inTransaction && mysql_errno( db ) == ER_LOCK_DEADLOCK )
   {
      rolledBackByDeadlock = true;
   }

   noteConnectionLoss();
}

void US_DB2::noteConnectionLoss()
{
   // Left "connected", a caller would go on sending into a dead connection
   // and read each failure as a problem with its own statement.
   if ( isConnectionLoss( int( mysql_errno( db ) ), server_version ) )
   {
      connected       = false;
      connection_lost = true;
   }
}

void US_DB2::resetLostHandle()
{
   if ( ! connection_lost )
      return;

   // The old handle may still hold a result, and its connection state is
   // whatever the library left at the loss; start from a fresh one
   if ( result )
      mysql_free_result( result );
   result      = NULL;
   row         = NULL;
   row_lengths = nullptr;
   mysql_close( db );
   db              = mysql_init( NULL );
   connection_lost = false;
   inTransaction   = false;
}

QString US_DB2::transactionEndedText() const
{
   // A lost connection is checked first.  Lost before the COMMIT was sent,
   // the server rolled the whole transaction back when the connection ended;
   // lost during the COMMIT, whether it took effect is unknown.
   if ( connection_lost )
   {
      return lostCommitError.isEmpty() ? QObject::tr( connectionLostRollbackMessage ) : lostCommitError;
   }
   return QObject::tr( rolledBackByDeadlock ? serverRollbackMessage : transactionEndedMessage );
}
#endif

#ifdef NO_DB
void US_DB2::drainResults()
{
}
#else
void US_DB2::drainResults()
{
   row_lengths = nullptr;

   if ( result )
      mysql_free_result( result );

   while ( mysql_next_result( db ) == 0 )
   {
      result = mysql_store_result( db );
      mysql_free_result( result );
   }

   // A procedure that failed part way reports it on the result set after the
   // last good one, which ends the loop above.
   noteStatementError();

   result = NULL;
}
#endif

#ifdef NO_DB
void US_DB2::rawQuery( const QString& )
{
}
#else
void US_DB2::rawQuery( const QString& sqlQuery )
{
   // Make sure that we clear out any unused
   //   result sets
   drainResults();

   if ( refuseUnsendable( sqlQuery ) )
   {
      return;
   }

   if ( run_utf8( db, sqlQuery ) != 0 )
   {
      error = serverErrorText();
      noteStatementError();
   }

   else
   {
      result = mysql_store_result( db );

      // A statement without a result set gives NULL too; only an error code
      // says that reading the result failed.
      if ( result == NULL && mysql_errno( db ) != 0 )
      {
         error = serverErrorText();
         noteStatementError();
      }
   }
}
#endif

#ifdef NO_DB
int US_DB2::statusQuery( const QString& ){ return 0; }
#else
int US_DB2::statusQuery( const QString& sqlQuery )
{
   if ( refuseUnsendable( sqlQuery ) )
   {
      return db_errno;
   }

   db_errno = DBERROR;

   this->rawQuery( sqlQuery );

   // What the server said about the statement itself; the reads of the US3
   // status below are statements too, and must not overwrite it
   server_errno = int( mysql_errno( db ) );

   // A statement the server refused itself -- a duplicate key, a wrong
   // argument count -- has its reason in the MySQL error rawQuery() kept.
   // last_error() holds only what a procedure recorded, possibly for an
   // earlier call, so it is asked only when the statement ran.
   if ( mysql_errno( db ) != 0 )
   {
      return db_errno;
   }

   if ( result )
   {
      row       = mysql_fetch_row( result );
      db_errno     = atoi( row[ 0 ] );
      mysql_free_result( result );
      result = NULL;
   }

   if ( db_errno != 0 )
   {
      this->rawQuery( "SELECT last_error()" );
      if ( result )
      {
         row       = mysql_fetch_row( result );
         error     = QString::fromUtf8( row[ 0 ] );
         mysql_free_result( result );
         result = NULL;
      }
   }

   return db_errno;
}
#endif

int US_DB2::statusQuery( const QStringList& arguments )
{
   const QString composed = buildQuery( arguments );
   if ( composed.isEmpty() )
   {
      return refuseArguments( arguments );
   }

   return statusQuery( composed );
}

int US_DB2::functionQuery( const QStringList& arguments )
{
   const QString composed = buildQuerySelect( arguments );
   if ( composed.isEmpty() )
   {
      return refuseArguments( arguments );
   }

   return statusQuery( composed );
}

int US_DB2::refuseArguments( const QStringList& arguments )
{
   // Nothing is sent, so clear what the last query left for next() to read.
   drainResults();
   db_errno     = DBERROR;
   server_errno = 0;
   error    = QObject::tr( invalidProcedureMessage ).arg( arguments.value( 0 ) );
   return db_errno;
}

#ifdef NO_DB
void US_DB2::query( const QString& ) {}
#else
void US_DB2::query( const QString& sqlQuery )
{
   if ( refuseUnsendable( sqlQuery ) )
   {
      return;
   }

   this->rawQuery( sqlQuery );

   // What the server said about the statement itself; the reads of the US3
   // status below are statements too, and must not overwrite it
   server_errno = int( mysql_errno( db ) );

   // A statement the server refused outright leaves no result set, and
   // rawQuery() reports that only through `error`.  Without this, db_errno
   // still holds whatever the previous query returned, so lastErrno() answers
   // OK for a query that never ran -- a lost connection, a deadlock, a
   // lock-wait timeout.  statusQuery() has always defaulted to DBERROR for the
   // same reason; this brings query() in line with it.
   if ( mysql_errno( db ) != 0 )
   {
      db_errno = DBERROR;
      return;
   }

   if ( result )
   {
      // This is a 2-set result: status, then data
      row    = mysql_fetch_row( result );
      db_errno  = atoi( row[ 0 ] );        // status
      mysql_free_result( result );
      result = NULL;

      const int next = mysql_next_result( db );

      // The procedure failed after its status row, while producing its data.
      if ( next > 0 )
      {
         error        = serverErrorText();
         db_errno     = DBERROR;
         server_errno = int( mysql_errno( db ) );
         noteStatementError();
         return;
      }

      else if ( next == 0 ) // get the result data
      {
         result = mysql_store_result( db );
         if ( ! result )
         {
            if ( mysql_field_count( db ) == 0 )
            {} // We are here as the result of an INSERT, UPDATE or DELETE

            else
            {} // Error retrieving result set
         }
      }
   }

   // A procedure that returns before its status set -- an empty GUID, a
   // failed verify_user -- still records its status in @US3_LAST_ERRNO.  Read
   // it, or db_errno keeps the previous query's status, usually OK.
   else if ( sqlQuery.trimmed().startsWith( "CALL", Qt::CaseInsensitive ) )
   {
      db_errno = DBERROR;
      this->rawQuery( "SELECT last_errno()" );

      if ( result )
      {
         row = mysql_fetch_row( result );
         if ( row != NULL && row[0] != NULL )
         {
            db_errno = atoi( row[0] );
         }
         mysql_free_result( result );
         result = NULL;
      }
   }

   if ( db_errno != 0 )
   {
      this->rawQuery( "SELECT last_error()" );

      if ( result )
      {
         row       = mysql_fetch_row( result );
         error     = QString::fromUtf8( row[ 0 ] );
         mysql_free_result( result );
         result = NULL;
      }
   }

   else
      error     = "";
}
#endif

void US_DB2::query( const QStringList& arguments )
{
   const QString composed = buildQuery( arguments );
   if ( composed.isEmpty() )
   {
      refuseArguments( arguments );
      return;
   }

   query( composed );
}

QString US_DB2::escapeLiteral( QString value )
{
   // Backslash first:  escaping the quotes first would then double the
   // backslashes this step inserts.  A NUL would end the query string early.
   // This assumes the server's default sql_mode, without NO_BACKSLASH_ESCAPES.
   value.replace( "\\", "\\\\" );
   value.replace( "'", "\\'" );
   value.replace( QChar( '\0' ), "\\0" );
   return value;
}

bool US_DB2::isProcedureName( const QString& name )
{
   // The name is a SQL identifier, not a quoted literal, so escaping cannot
   // make it safe -- it is interpolated as written.  Accept only a plain
   // identifier so anything else is refused rather than emitted.
   static const QRegularExpression procName( "\\A[A-Za-z_][A-Za-z0-9_]*\\z" );
   return procName.match( name ).hasMatch();
}

QString US_DB2::composeQuery( const QString& keyword, const QStringList& arguments, const QString& guid, const QString& password )
{
   // The procedure name is arguments[ 0 ], so an empty list has nothing to call.
   if ( arguments.isEmpty() )
   {
      return QString();
   }

   if ( !isProcedureName( arguments[0] ) )
   {
      return QString();
   }

   // The guid and password are quoted literals like every other argument and
   // are escaped the same way.
   QString newquery = keyword + " " + arguments[0] + "('" + escapeLiteral( guid ) + "', '" + escapeLiteral( password ) + "'";

   for ( int i = 1; i < arguments.size(); i++ )
   {
      newquery += ", '" + escapeLiteral( arguments[i] ) + "'";
   }

   newquery += ")";

   return newquery;
}

QString US_DB2::buildQuery( const QStringList& arguments )
{
   return composeQuery( "CALL", arguments, guid, userPW );
}

QString US_DB2::buildQuerySelect( const QStringList& arguments )
{
   return composeQuery( "SELECT", arguments, guid, userPW );
}

#ifdef NO_DB
bool US_DB2::next( void ){ return false; }
#else
bool US_DB2::next( void )
{ 
   row         = NULL;
   row_lengths = nullptr;
   if ( result )
   {
      if ( ( row = mysql_fetch_row( result ) ) != NULL )
      {
         row_lengths = mysql_fetch_lengths( result );
         return true;
      }
   }

   return false;
}
#endif

#ifdef NO_DB
QVariant US_DB2::value( unsigned ){ return QVariant(); }
#else
QVariant US_DB2::value( unsigned index )
{
   if ( row && ( index < mysql_field_count( db ) ) )
   {
      // A column is not a C string: it may hold NUL, and only its length
      // says where it ends.  A NULL column is a null string, as it was.
      if ( row[ index ] == NULL )
         return QVariant( QString() );
      if ( row_lengths != nullptr )
         return QVariant( QString::fromUtf8( row[ index ], int( row_lengths[ index ] ) ) );
      return QVariant( QString::fromUtf8( row[ index ] ) );
   }

   return QVariant();
}
#endif

bool US_DB2::isConnected ( void )
{
   return connected;
}

#ifdef NO_DB
int US_DB2::numRows( void ){ return 0; }
#else
int US_DB2::numRows( void )
{ 
   return ( result )? ( (int) mysql_num_rows( result ) ) : -1;
}
#endif

#ifdef NO_DB
int US_DB2::writeBlobToDB( const QString& , const QString& , const int ) { return 0; }
#else
int US_DB2::writeBlobToDB( const QString& filename, 
    const QString& procedure, const int tableID )
{
   // The procedure name is pasted into the query below, so refuse it before
   // reading the file.
   if ( !isProcedureName( procedure ) )
   {
      return refuseArguments( QStringList() << procedure );
   }

   if ( tableID == 0 )
   {
      error = QString( "writeBlob: don't know which record data belongs to in " ) + filename;
      db_errno = DBERROR;
      return DBERROR;
   }

   // The text around the data is UTF-8 like every other query; its length
   // in bytes is not its length in characters once the guid or password
   // holds anything beyond ASCII.  The escaped data has no NUL left in it,
   // and _binary keeps the server from reading it as UTF-8 text.
   const QString queryPart1 = "CALL " + procedure + "('" + escapeLiteral( guid ) + "', '" + escapeLiteral( userPW ) + "', " +
                              QString::number( tableID ) + ", _binary'";
   if ( refuseUnsendable( queryPart1 ) )
   {
      return db_errno;
   }
   const QByteArray stmtHead = queryPart1.toUtf8();
   const int        stmtTail = 4 + 32 + 2;   // "', '", the MD5 in hex, "')"

   QFile fin( filename );

   if ( ! fin.open( QIODevice::ReadOnly ) )
   {
      error = QString( "writeBlob: cannot open file " ) + filename;
      db_errno = DBERROR;
      return DBERROR;
   }

   // The server takes a statement of at most max_allowed_packet bytes, and
   // answers a larger one by closing the connection.  So a file that cannot
   // fit is refused here, before it is read, and the statement is checked
   // again as it is built: escaping makes it longer.  The limit also keeps
   // every size below in range of an int, since the server's own maximum is
   // 1 GiB.
   const qint64 fileSize = fin.size();
   const qint64 packet   = maxAllowedPacket();
   if ( packet <= 0 )
   {
      fin.close();
      error    = QString( "writeBlob: the server's max_allowed_packet could not be read, so nothing was sent" ) +
                 ( error.isEmpty() ? QString() : ": " + error );
      db_errno = DBERROR;
      return DBERROR;
   }
   const qint64 limit    = qMin( packet, qint64( 0x40000000 ) );

   // What the server measures is the packet: the command byte (COM_QUERY)
   // and the statement after it.  A packet of max_allowed_packet bytes or
   // more is refused with 1153 and the connection closed, so the longest
   // statement that is sent is limit - 2 bytes, on MySQL and MariaDB alike.
   const int    commandByte = 1;
   const qint64 longest     = limit - 1 - commandByte;
   const auto   overLimit   = [ & ]( qint64 statement ) { return statement > longest; };
   const auto   tooLarge = [ & ]( qint64 statement )
   {
      error = QString( "writeBlob: %1 is %2 bytes, and the statement that sends it would be at least %3 bytes; "
                       "the server accepts statements of at most %4 bytes (max_allowed_packet %5, less 2), so "
                       "nothing was sent.  The database administrator can raise max_allowed_packet." )
                 .arg( filename ).arg( fileSize ).arg( statement ).arg( longest ).arg( packet );
      db_errno     = DBERROR;
      server_errno = 0;
      return DBERROR;
   };

   if ( overLimit( stmtHead.size() + fileSize + stmtTail ) )
   {
      fin.close();
      return tooLarge( stmtHead.size() + fileSize + stmtTail );
   }

   QByteArray blobData = fin.readAll();
   fin.close();

   if ( blobData.size() < 1 )
   {
      error = QString( "writeBlob: no data in file " ) + filename;
      db_errno = DBERROR;
      return DBERROR;
   }

   // Calculate a checksum
   const QByteArray checksum =
        QCryptographicHash::hash( blobData, QCryptographicHash::Md5 ).toHex();

   // Escape the data straight into the statement, a piece at a time, so
   // that only the data and the statement are held at once -- not a second
   // escaped copy as well, nor room for every byte to be escaped.  The
   // connection's character set is utf8mb4, in which no byte of a
   // multi-byte character is one that needs escaping, so where the pieces
   // are cut does not change what the server reads.
   const int  piece = 4 * 1024 * 1024;
   QByteArray sqlQuery;
   const qint64 expected = qint64( stmtHead.size() ) + blobData.size() + blobData.size() / 16 + stmtTail;
   sqlQuery.reserve( int( qMin( limit, expected ) + 2 * piece + 1 ) );
   sqlQuery.append( stmtHead );

   for ( int offset = 0; offset < blobData.size(); offset += piece )
   {
      const int length = qMin( piece, blobData.size() - offset );
      const int at     = sqlQuery.size();
      sqlQuery.resize( at + 2 * length + 1 );
      const unsigned long written = mysql_real_escape_string( db, sqlQuery.data() + at, blobData.constData() + offset,
                                                              static_cast< unsigned long >( length ) );
      sqlQuery.resize( at + int( written ) );

      if ( overLimit( qint64( sqlQuery.size() ) + stmtTail ) )
      {
         return tooLarge( qint64( sqlQuery.size() ) + stmtTail );
      }
   }
   blobData = QByteArray();

   sqlQuery.append( "', '" );
   sqlQuery.append( checksum );
   sqlQuery.append( "')" );

   // We can't use rawQuery() since it takes a QString
   drainResults();

   if ( mysql_real_query( db, sqlQuery.constData(), static_cast< unsigned long >( sqlQuery.size() ) ) != 0 )
   {
     error = serverErrorText();
     noteStatementError();

     db_errno = DBERROR;
     return DBERROR;
   }

   result   = mysql_store_result( db );
   row      = result != NULL ? mysql_fetch_row( result ) : NULL;
   if ( row == NULL || row[ 0 ] == NULL )
   {
      error = QString( "writeBlob: %1 returned no status" ).arg( procedure );
      if ( mysql_errno( db ) != 0 )
      {
         error += ": " + serverErrorText();
         noteStatementError();
      }
      db_errno = DBERROR;
      return DBERROR;
   }
   db_errno = atoi( row[ 0 ] );

   mysql_free_result( result );
   result = NULL;

   if ( db_errno == BAD_CHECKSUM )
   {
      error = QString( "writeBlob: data transmission error (MD5 checksum)" ) ;

      return BAD_CHECKSUM;
   }

   else if ( db_errno != OK )
   {
      error = QString( "MySQL error: " ) + lastError();

      return db_errno;
   }

   return db_errno;
}
#endif

#ifdef NO_DB
qint64 US_DB2::maxAllowedPacket() { return 0; }
#else
qint64 US_DB2::maxAllowedPacket()
{
   error.clear();
   rawQuery( "SELECT @@max_allowed_packet" );
   qint64 packet = 0;
   if ( result != NULL )
   {
      MYSQL_ROW limit = mysql_fetch_row( result );
      if ( limit != NULL && limit[ 0 ] != NULL )
         packet = QByteArray( limit[ 0 ] ).toLongLong();
      mysql_free_result( result );
      result = NULL;
   }
   return packet;
}
#endif

QString US_DB2::commitErrorText( int serverErrno, const QString& serverMessage, const QString& serverVersion )
{
   // The server may have committed before the connection dropped; only a
   // lost connection leaves that open, so only then is the outcome unknown.
   // Any error isConnectionLoss() knows is one: 2055, which MySQL's client
   // library gives for a lost connection, and 2026, which a TLS connection
   // gives, as well as 2006 and 2013.
   if ( isConnectionLoss( serverErrno, serverVersion ) )
      return QObject::tr( commitUnknownMessage ).arg( serverMessage );
   return serverMessage;
}

QString US_DB2::serverVersion()
{
   return server_version;
}

QString US_DB2::loneSurrogateProblem( const QString& text )
{
   for ( int ii = 0; ii < text.size(); ii++ )
   {
      if ( text[ ii ].isHighSurrogate() && ii + 1 < text.size() && text[ ii + 1 ].isLowSurrogate() )
      {
         ii++;
         continue;
      }

      if ( text[ ii ].isSurrogate() )
      {
         return QObject::tr( "The text holds half of a UTF-16 surrogate pair (U+%1) on its own, which is not a "
                             "character and cannot be sent to the database as UTF-8, so nothing was sent.  The text "
                             "was probably cut in the middle of a character, or read from a malformed file." )
            .arg( QString::number( text[ ii ].unicode(), 16 ).toUpper() );
      }
   }
   return QString();
}

bool US_DB2::refuseUnsendable( const QString& statement )
{
   const QString problem = loneSurrogateProblem( statement );
   if ( problem.isEmpty() )
      return false;

   // Nothing is sent, so clear what the last query left for next() to read
   drainResults();
   db_errno     = DBERROR;
   server_errno = 0;
   error        = problem;
   return true;
}

int US_DB2::writeBlobFile( const QString& filename, const QByteArray& data, QString& error )
{
   QFile fout( filename );
   if ( !fout.open( QIODevice::WriteOnly ) )
   {
      error = QString( "readBlob: could not write file " ) + filename;
      return DBERROR;
   }

   const qint64  written = fout.write( data );
   const bool    flushed = fout.flush();
   const bool    failed  = written != data.size() || !flushed || fout.error() != QFileDevice::NoError;
   const QString reason  = fout.errorString();
   fout.close();

   if ( failed )
   {
      error = QString( "readBlob: could not write all %1 bytes to %2: %3" ).arg( data.size() ).arg( filename, reason );
      QFile::remove( filename );
      return DBERROR;
   }
   return OK;
}

// ---------------------------------------------- text a latin1 column holds

namespace
{
// The characters the server's latin1 (Windows-1252) has at 0x80-0x9F
bool cp1252_extra( ushort code )
{
   switch ( code )
   {
      case 0x20AC: case 0x201A: case 0x0192: case 0x201E: case 0x2026:
      case 0x2020: case 0x2021: case 0x02C6: case 0x2030: case 0x0160:
      case 0x2039: case 0x0152: case 0x017D: case 0x2018: case 0x2019:
      case 0x201C: case 0x201D: case 0x2022: case 0x2013: case 0x2014:
      case 0x02DC: case 0x2122: case 0x0161: case 0x203A: case 0x0153:
      case 0x017E: case 0x0178:
      // Undefined in Windows-1252; the server maps these bytes to themselves
      case 0x0081: case 0x008D: case 0x008F: case 0x0090: case 0x009D:
         return true;
      default:
         return false;
   }
}

bool latin1_char( QChar ch )
{
   const ushort code = ch.unicode();
   if ( code < 0x80 || ( code >= 0xA0 && code <= 0xFF ) )
      return true;
   return cp1252_extra( code );
}

// Each character of the text, as a string: a pair of surrogates is one
QStringList characters_of( const QString& text )
{
   QStringList chars;
   for ( int ii = 0; ii < text.size(); ii++ )
   {
      if ( text[ ii ].isHighSurrogate() && ii + 1 < text.size() && text[ ii + 1 ].isLowSurrogate() )
      {
         chars << text.mid( ii, 2 );
         ii++;
      }
      else
         chars << QString( text[ ii ] );
   }
   return chars;
}

// How a character is shown in a message: itself, and its code point
QString shown_char( const QString& ch )
{
   const QVector< uint > codes = ch.toUcs4();
   const uint            code  = codes.isEmpty() ? 0 : codes[ 0 ];
   return QString( "'%1' (U+%2)" ).arg( ch, QString::number( code, 16 ).toUpper().rightJustified( 4, '0' ) );
}

const char* const latin1Advice =
   "Replace the characters outside Latin-1 (a Greek letter, for example, can be spelled out: alpha), "
   "or have the database administrator convert the database to utf8mb4.";
}

bool US_DB2::fitsLatin1( const QString& text )
{
   for ( const QChar& ch : text )
   {
      if ( !latin1_char( ch ) )
         return false;
   }
   return true;
}

QString US_DB2::nonLatin1Characters( const QString& text )
{
   QString found;
   for ( const QString& ch : characters_of( text ) )
   {
      if ( ( ch.size() > 1 || !latin1_char( ch[ 0 ] ) ) && !found.contains( ch ) )
         found += ch;
   }
   return found;
}

bool US_DB2::textFitsCharset( const QString& text, const QString& charset )
{
   const QString name = charset.trimmed().toLower();
   if ( name == "latin1" )
      return fitsLatin1( text );
   if ( name == "ascii" )
   {
      for ( const QChar& ch : text )
      {
         if ( ch.unicode() >= 0x80 )
            return false;
      }
      return true;
   }
   if ( name == "utf8" || name == "utf8mb3" )
   {  // Three bytes at most: nothing beyond the Basic Multilingual Plane
      for ( const QChar& ch : text )
      {
         if ( ch.isSurrogate() )
            return false;
      }
      return true;
   }
   return true;
}

QString US_DB2::latin1Problem( const QString& field, const QString& text )
{
   const QString bad = nonLatin1Characters( text );
   if ( bad.isEmpty() )
      return QString();

   QStringList shown;
   for ( const QString& ch : characters_of( bad ) )
      shown << shown_char( ch );

   return QObject::tr( "This database stores text as Latin-1 and cannot hold %1 in \"%2\".  %3" )
      .arg( shown.join( ", " ), field, QObject::tr( latin1Advice ) );
}

bool US_DB2::textFitsServer( IUS_DB2* db, const QString& text, const QString& table,
                             const QString& column, QString* problem )
{
   if ( problem != nullptr )
      problem->clear();

   QStringList charsets;
   static const QRegularExpression identifier( "^[A-Za-z0-9_]+$" );
   if ( db != nullptr && db->isConnected() && identifier.match( table ).hasMatch() &&
        identifier.match( column ).hasMatch() )
   {
      db->rawQuery( "SELECT @@character_set_database, ( SELECT CHARACTER_SET_NAME FROM information_schema.COLUMNS"
                    " WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = '" + table +
                    "' AND COLUMN_NAME = '" + column + "' )" );
      if ( db->next() )
      {
         const QString database = db->value( 0 ).toString();
         const QString stored   = db->value( 1 ).toString();
         if ( !database.isEmpty() && !stored.isEmpty() )
            charsets << database << stored;
      }
   }

   // Not known: the narrowest a US3 schema uses
   if ( charsets.isEmpty() )
      charsets << "latin1";

   for ( const QString& charset : charsets )
   {
      if ( textFitsCharset( text, charset ) )
         continue;

      if ( problem != nullptr )
      {
         if ( charset.toLower() == "latin1" )
            *problem = latin1Problem( column, text );
         else
            *problem = QObject::tr( "This database stores \"%1\" as %2, which cannot hold all of the text given." )
                          .arg( column, charset );
      }
      return false;
   }
   return true;
}

QString US_DB2::charsetErrorText( int serverErrno, const QString& serverMessage )
{
   if ( serverErrno == 1366 )
   {  // ER_TRUNCATED_WRONG_VALUE_FOR_FIELD is given for numbers too
      static const QRegularExpression wrongString(
         "Incorrect string value: '((?:\\\\x[0-9A-Fa-f]{2})*)[^']*' for column (.*) at row" );
      const QRegularExpressionMatch match = wrongString.match( serverMessage );
      if ( !match.hasMatch() )
         return QString();

      // The character: the bytes the server quotes, read as UTF-8 up to the
      // end of the first whole character
      QByteArray    bytes;
      const QString escapes = match.captured( 1 );
      for ( int ii = 0; ii + 4 <= escapes.size(); ii += 4 )
         bytes.append( char( escapes.mid( ii + 2, 2 ).toUInt( nullptr, 16 ) ) );
      QString character;
      for ( int len = 1; len <= bytes.size() && character.isEmpty(); len++ )
      {
         const QString decoded = QString::fromUtf8( bytes.left( len ) );
         if ( !decoded.contains( QChar( QChar::ReplacementCharacter ) ) )
            character = decoded;
      }

      // The column: the last name of `db`.`table`.`column` or 'column', and
      // a procedure parameter without its p_
      QString                         field = match.captured( 2 ).trimmed();
      static const QRegularExpression lastName( "[`']([^`']*)[`']$" );
      const QRegularExpressionMatch   name  = lastName.match( field );
      if ( name.hasMatch() )
         field = name.captured( 1 );
      if ( field.startsWith( "p_" ) )
         field = field.mid( 2 );

      return QObject::tr( "This database stores text as Latin-1 and cannot hold %1 in \"%2\".  %3" )
         .arg( character.isEmpty() ? QObject::tr( "a character" ) : shown_char( character ),
               field.isEmpty() ? QObject::tr( "a text field" ) : field, QObject::tr( latin1Advice ) );
   }

   if ( serverErrno == 1267 || serverErrno == 1271 || serverErrno == 1300 )
   {
      return QObject::tr( "This database stores text as Latin-1, and the text given holds characters "
                          "outside Latin-1 that it cannot compare or convert.  %1" )
         .arg( QObject::tr( latin1Advice ) );
   }

   return QString();
}

#ifndef NO_DB
QString US_DB2::serverErrorText()
{
   const QString message  = QString::fromUtf8( mysql_error( db ) );
   const QString friendly = charsetErrorText( int( mysql_errno( db ) ), message );
   if ( friendly.isEmpty() )
      return QString( "MySQL error: " ) + message;
   return QString( "MySQL error: " ) + friendly + "\n(" + message + ")";
}
#endif

#ifdef NO_DB
int US_DB2::readBlobFromDB( const QString& , const QString& , const int ) { return 0; }
#else
int US_DB2::readBlobFromDB( const QString& filename, 
    const QString& procedure, const int tableID )
{
   // First let's build the query
   const QString sqlQuery = composeQuery( "CALL", QStringList() << procedure << QString::number( tableID ), guid, userPW );
   if ( sqlQuery.isEmpty() )
   {
      return refuseArguments( QStringList() << procedure );
   }

   // The guid and password are text like any other query's
   if ( refuseUnsendable( sqlQuery ) )
   {
      return db_errno;
   }

   // We can't use standard methods because the
   // binary data doesn't all transfer

   // Make sure that we clear out any unused
   //   result sets
   drainResults();
   error.clear();
   row = NULL;

   // Fails the read with the server's error for the statement
   const auto serverFailure = [ & ]( void )
   {
      error        = serverErrorText();
      server_errno = int( mysql_errno( db ) );
      noteStatementError();
      db_errno     = DBERROR;
      return DBERROR;
   };

   // Fails the read with a reason of its own
   const auto failure = [ & ]( int status, const QString& reason )
   {
      if ( result )
         mysql_free_result( result );
      result   = NULL;
      row      = NULL;
      error    = QString( "readBlob: %1 for ID %2 %3" ).arg( procedure ).arg( tableID ).arg( reason );
      db_errno = status;
      return status;
   };

   if ( run_utf8( db, sqlQuery ) != 0 )
   {
      return serverFailure();
   }
   server_errno = 0;

   // First result set is status.  A procedure that returned before it --
   // an empty GUID, a failed login -- sent none, and recorded its status
   // in @US3_LAST_ERRNO instead.
   result = mysql_store_result( db );
   if ( result == NULL )
   {
      if ( mysql_errno( db ) != 0 )
         return serverFailure();

      int status = DBERROR;
      rawQuery( "SELECT last_errno()" );
      if ( result != NULL )
      {
         MYSQL_ROW recorded = mysql_fetch_row( result );
         if ( recorded != NULL && recorded[ 0 ] != NULL )
            status = atoi( recorded[ 0 ] );
         mysql_free_result( result );
         result = NULL;
      }
      if ( status == OK )
         status = DBERROR;   // and yet it sent nothing

      const QString recordedError = recordedErrorText();
      failure( status, QString( "returned no status" ) );
      if ( !recordedError.isEmpty() )
         error = recordedError;
      return status;
   }

   row = mysql_fetch_row( result );
   if ( row == NULL || mysql_num_fields( result ) < 1 || row[ 0 ] == NULL )
   {
      return failure( DBERROR, "returned an empty status" );
   }
   const int status = atoi( row[ 0 ] );
   mysql_free_result( result );
   result = NULL;
   row    = NULL;

   // A missing record or a refused one: the status set is all there is,
   // and the procedure recorded why in @US3_LAST_ERROR
   if ( status != OK )
   {
      const QString recordedError = recordedErrorText();
      failure( status, QString( "returned status %1" ).arg( status ) );
      if ( !recordedError.isEmpty() )
         error = recordedError;
      return status;
   }

   // Now get the result data.  After the status set comes either the data
   // set or, from a procedure that sent none, only the CALL's own result.
   const int next = mysql_next_result( db );
   if ( next > 0 )
   {
      return serverFailure();
   }

   result = ( next == 0 ) ? mysql_store_result( db ) : NULL;
   if ( result == NULL )
   {
      if ( next == 0 && mysql_errno( db ) != 0 )
         return serverFailure();
      return failure( DBERROR, "returned no data" );
   }

   // Make sure we get the data and the right number of bytes
   row = mysql_fetch_row( result );
   if ( row == NULL )
   {
      // The record went between the procedure's check and its read
      return failure( NOROWS, "returned no data row" );
   }

   const unsigned long* lengths = mysql_fetch_lengths( result );
   if ( mysql_num_fields( result ) < 2 || lengths == nullptr )
   {
      return failure( DBERROR, "returned no data and checksum" );
   }

   if ( row[ 0 ] == NULL )
   {
      return failure( DBERROR, "holds no data (NULL)" );
   }

   // Now convert
   QByteArray aucData( row[ 0 ], int( lengths[ 0 ] ) );
   QByteArray checksum = row[ 1 ] != NULL ? QByteArray( row[ 1 ], int( lengths[ 1 ] ) ) : QByteArray();
   QByteArray calculated = 
        QCryptographicHash::hash( aucData, QCryptographicHash::Md5 ).toHex();

   mysql_free_result( result );
   result = NULL;
   row    = NULL;

   // Since we got data, let's write it out
   if ( checksum != calculated )
   {
      error = QString( "readBlob: data transmission error (MD5 checksum)" ) ;

      db_errno = BAD_CHECKSUM;
   }

   else
   {
      db_errno = OK;
      const int written = writeBlobFile( filename, aucData, error );
      if ( written != OK )
      {
         db_errno = written;
      }
   }

   return db_errno;
}
#endif

#ifndef NO_DB
QString US_DB2::recordedErrorText()
{
   QString text;
   rawQuery( "SELECT last_error()" );
   if ( result != NULL )
   {
      MYSQL_ROW recorded = mysql_fetch_row( result );
      if ( recorded != NULL && recorded[ 0 ] != NULL )
         text = QString::fromUtf8( recorded[ 0 ] );
      mysql_free_result( result );
      result = NULL;
   }
   return text;
}
#endif

#ifdef NO_DB
int US_DB2::lastInsertID( void ){ return 0; }
#else
int US_DB2::lastInsertID( void )
{
   int ID = -1;

   this->rawQuery( "SELECT last_insertID()" );
   
   if ( result )
   {
      row = mysql_fetch_row( result );
      ID = atoi( row[ 0 ] );
      mysql_free_result( result );
      result = NULL;
   }

   return ID;
}
#endif

#ifdef NO_DB
QString US_DB2::lastDebug( void ){ return ""; }
#else
QString US_DB2::lastDebug( void )
{
   QString debug = "";

   // Let's see if there is a debug message available
   this->rawQuery( "SELECT last_debug()" );
   if ( result )
   {
      row       = mysql_fetch_row( result );
      debug     = QString::fromUtf8( row[ 0 ] );
      mysql_free_result( result );
      result = NULL;
   }

   return ( debug );
}
#endif

#ifdef NO_DB
unsigned long US_DB2::mysqlEscapeString( QByteArray& , QByteArray& , unsigned long  ) { return 0; }
#else
unsigned long US_DB2::mysqlEscapeString( QByteArray& to, QByteArray& from, unsigned long length )
{
   // Size the QByteArray properly
   // worst case every character has to be escaped plus a trailing \0
   to.resize( length * 2 + 1 );

   const char* fromPtr = from.constData();
   char* toPtr         = to.data();

   // to_length is the length of the QByteArray without the \0, but according to the docs
   // the trailing \0 is still written for the mariadb-/mysql-connector
   ulong to_length = mysql_real_escape_string( db, toPtr, fromPtr, length );

   // Size string appropriately and return new length
   // The trailing '\0' shouldn't be included in the size
   to.resize( to_length );
   return to_length;
}
#endif

#ifdef NO_DB
int US_DB2::writeAucToDB( const QString&, int ) { return 0; }
#else
int US_DB2::writeAucToDB( const QString& filename, int tableID ) 
{
   // First compress th file
   US_Gzip gz;
   int     retCode = gz.gzip( filename );
   
   QString fn = filename;

   if ( retCode == 0 )
   {  
      fn  += ".gz";  // gzip renames the file
      retCode = writeBlobToDB( fn, "upload_aucData", tableID );

      // Now uncompress the file
      gz.gzip( fn );
   }

   return retCode;
}
#endif

#ifdef NO_DB
int US_DB2::readAucFromDB( const QString&, int ) { return 0; }
#else
int US_DB2::readAucFromDB( const QString& filename, int tableID ) 
{
   QString fn = filename + ".gz";

   int retCode = readBlobFromDB( fn, "download_aucData", tableID );

   if ( retCode == OK )
   {
      // Check to see if it is a gzipped file
      char  buf[ 2 ];
      QFile t( fn );
      t.open( QIODevice::ReadOnly );
      t.peek( buf, 2 );
      t.close();

      // Look for gzip magic number
      if ( buf[ 0 ] == '\037'  &&  buf[ 1 ] == '\213' )
      {
         US_Gzip gz;
         retCode = gz.gunzip( fn );
      }
      else // not a gz file, jsut rename it
      {
         QFile::rename( fn, filename ); 
      }
   }

   return retCode;
}
#endif

#ifdef NO_DB
bool US_DB2::configure_ssl( QString& err )
{
   error = "";
   err = error;
   return true;
}
#else
bool US_DB2::configure_ssl( QString& err )
{
   int success = -1;
   error = "";
   err = error;
   // Set connection to use ssl encryption.
   // No client certificate is sent.  No database account requires one, and a
   // server whose TLS library refuses SHA-1 certificate signatures rejected the
   // handshake when the shared one formerly in etc/mysql was sent.  TLS stays
   // on: libmariadb enables it when a CA file is set, libmysql through
   // MYSQL_OPT_SSL_MODE below.
   success = mysql_options( db, MYSQL_OPT_SSL_CA, QFile::encodeName( caFile ).constData() );
   if ( success != 0 ) {
      error = QString( "Error setting SSL CA file: " ) + caFile;
      err = error;
      return false;
   }
   success = mysql_options( db, MYSQL_OPT_SSL_CAPATH, nullptr );
   if ( success != 0 ) {
      error = QString( "Error setting SSL CA path " );
      err = error;
      return false;
   }
   // Due to legacy certs and ciphers, security policies must be relaxed
   if ( US_Settings::debug_match( "DB_ENFORCE_DEFAULT_SSL_SECURITY" ) ) {
#if !defined(__MINGW64__)
         // set default SSL ciphers
         success = mysql_options( db, MYSQL_OPT_SSL_CIPHER, CIPHER );
         if ( success != 0 ) {
            error = QString( "Error setting SSL cipher: " ) + CIPHER;
            err = error;
            return false;
         }
#endif
      // the user enforces default security
      return true;
   }
#if !defined(__MINGW64__)
   // apply :@SECLEVEL=0 to the cipher
   const QString cipher = QString::asprintf( "%s:@SECLEVEL=0", CIPHER );
   success = mysql_options( db, MYSQL_OPT_SSL_CIPHER, cipher.toLatin1().constData() );
   if ( success != 0 ) {
      error = QString( "Error setting SSL cipher: " ) + cipher;
      err = error;
      return false;
   }
#else
   // MSYS2/64 (MINGW64) Windows builds link a Schannel-backed libmariadb,
   // which fails to match any suite from the OpenSSL-style CIPHER list
   // above (TLS SEC_E_ALGORITHM_MISMATCH, "no cipher match"). Skip setting
   // MYSQL_OPT_SSL_CIPHER here and let the backend negotiate its own
   // default suite set instead. Does not affect MSVC/vcpkg builds
   // (__MINGW64__ is not defined under MSVC). See ultrascan-tickets#940.
#endif
   // force a minimum of TLS 1.2
   const QString tls_versions ("TLSv1.2,TLSv1.3");
#if defined(LIBMARIADB)
   // for libmariadb it is MARIADB_OPT_TLS_VERSION
   success = mysql_options( db, MARIADB_OPT_TLS_VERSION, tls_versions.toLatin1().constData() );
#else
   // for libmysql it is MYSQL_OPT_TLS_VERSION
   success = mysql_options( db, MYSQL_OPT_TLS_VERSION, tls_versions.toLatin1().constData() );
#endif
   if ( success != 0 ) {
      error = QString( "Error setting TLS version: " ) + tls_versions;
      err = error;
      return false;
   }
   // disable verification of ssl server cert (hostname)
#if defined(LIBMARIADB)
   // for libmariadb disable MYSQL_OPT_SSL_VERIFY_SERVER_CERT
   unsigned long verify = 0; // 0 = disable, 1 = enable
   success = mysql_options( db, MYSQL_OPT_SSL_VERIFY_SERVER_CERT, &verify );
#else
   // for libmysql set MYSQL_OPT_SSL_MODE to SSL_MODE_REQUIRED
   unsigned int ssl_mode = SSL_MODE_REQUIRED; // SSL on, but no CA/hostname verification
   success = mysql_options( db, MYSQL_OPT_SSL_MODE, &ssl_mode );
#endif
   if ( success != 0 ) {
      error = QString( "Error disabling SSL verify server cert." );
      err = error;
      return false;
   }
   return true;
}
#endif
