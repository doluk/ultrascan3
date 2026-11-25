//! \file test_us_db2_transactions.cpp
// US_DB2 transaction outcomes that only a live server can produce.
//
// InnoDB answers a deadlock by rolling back the victim's whole transaction,
// which ends it just as an implicit commit does.  These tests force a real
// deadlock between two connections and check that the victim's commit and
// rollback name the deadlock instead of claiming its rows were kept, and that
// a transaction ended without any error to the client is never committed.
//
// They need a MariaDB or MySQL server and skip without one.  Set
//   US3_TEST_DB_HOST      host or host:port
//   US3_TEST_DB_NAME      a scratch database; the tests create their own
//                         table and procedures
//   US3_TEST_DB_USER      needs CREATE ROUTINE on that database, and PROCESS
//                         to read INNODB_TRX while setting up the deadlock
//   US3_TEST_DB_PASSWORD
//
// US_DB2 always connects over TLS, with etc/mysql/ca-cert.pem as its CA and
// no client certificate, so the server must have TLS on.  Some client
// libraries also check that the server certificate chains to that CA.
// Without that the connect fails and the tests fail rather than skip.

#include <gtest/gtest.h>

#include "us_db2.h"

#include <QFile>
#include <QString>
#include <QTemporaryDir>

#include <chrono>
#include <memory>
#include <thread>

namespace
{
#ifndef NO_DB
const char* const table = "us3_test_txn";

// Rolls back the caller's transaction without returning an error, as a
// procedure that catches a deadlock in a handler leaves it.
const char* const rollbackProcedure = "us3_test_txn_rollback";

// Returns a status row, then fails producing its data set.
const char* const failingProcedure = "us3_test_txn_fail";

// Records the status it is given in @US3_LAST_ERRNO and returns no status set,
// as US3 procedures do on an empty GUID or a failed verify_user.
const char* const silentProcedure = "us3_test_txn_silent";

// Returns a status set and a data set, as US3 procedures do, and commits the
// caller's transaction part way: after the status set ( p = 1 ) or after the
// data set, followed by one more set ( p = 2 ).  Then it waits for the lock
// on row 2, where a test can lose its connection while the client still has
// result sets to read.
const char* const committingProcedure = "us3_test_txn_commit";

// A table that DDL creates inside a transaction, to commit it implicitly
const char* const ddlTable = "us3_test_txn_ddl";

struct LiveDb
{
      QString host;
      QString name;
      QString user;
      QString password;

      bool    configured() const
      {
         return !host.isEmpty() && !name.isEmpty() && !user.isEmpty();
      }
};

LiveDb liveDb()
{
   return { qEnvironmentVariable( "US3_TEST_DB_HOST" ),
            qEnvironmentVariable( "US3_TEST_DB_NAME" ),
            qEnvironmentVariable( "US3_TEST_DB_USER" ),
            qEnvironmentVariable( "US3_TEST_DB_PASSWORD" ) };
}

// Runs a statement and returns the first column of its first row.
QString scalar( US_DB2& db, const QString& sql )
{
   db.rawQuery( sql );
   return db.next() ? db.value( 0 ).toString() : QString();
}

class US_DB2_LiveTransaction : public ::testing::Test
{
   protected:
      void SetUp() override
      {
         const LiveDb live = liveDb();
         if ( !live.configured() )
         {
            GTEST_SKIP() << "US3_TEST_DB_HOST, US3_TEST_DB_NAME and US3_TEST_DB_USER not set";
         }

         first  = std::make_unique<US_DB2>();
         second = std::make_unique<US_DB2>();

         QString error;
         ASSERT_TRUE( first->connect( live.host, live.name, live.user, live.password, error ) ) << error.toStdString();
         ASSERT_TRUE( second->connect( live.host, live.name, live.user, live.password, error ) ) << error.toStdString();

         first->rawQuery( QString( "CREATE TABLE IF NOT EXISTS %1 ( id INT PRIMARY KEY, v INT ) ENGINE=InnoDB" ).arg( table ) );
         first->rawQuery( QString( "DELETE FROM %1" ).arg( table ) );
         first->rawQuery( QString( "INSERT INTO %1 VALUES ( 1, 0 ), ( 2, 0 )" ).arg( table ) );
         ASSERT_EQ( scalar( *first, QString( "SELECT COUNT(*) FROM %1" ).arg( table ) ), "2" );

         first->rawQuery( QString( "DROP PROCEDURE IF EXISTS %1" ).arg( rollbackProcedure ) );
         first->rawQuery( QString( "CREATE PROCEDURE %1() BEGIN ROLLBACK; END" ).arg( rollbackProcedure ) );
         first->rawQuery( QString( "DROP PROCEDURE IF EXISTS %1" ).arg( failingProcedure ) );
         first->rawQuery(
            QString( "CREATE PROCEDURE %1() BEGIN SELECT 0; SELECT * FROM us3_test_no_such_table; END" ).arg( failingProcedure ) );
         first->rawQuery( QString( "DROP PROCEDURE IF EXISTS %1" ).arg( silentProcedure ) );
         first->rawQuery( QString( "CREATE PROCEDURE %1( p INT ) BEGIN SET @US3_LAST_ERRNO = p; "
                                   "SET @US3_LAST_ERROR = CONCAT( 'silent status ', p ); END" )
                             .arg( silentProcedure ) );
         first->rawQuery( QString( "DROP PROCEDURE IF EXISTS %1" ).arg( committingProcedure ) );
         first->rawQuery( QString( "CREATE PROCEDURE %1( p INT ) BEGIN SELECT 0 AS status; "
                                   "IF p = 1 THEN UPDATE %2 SET v = v + 1 WHERE id = 1; COMMIT; END IF; "
                                   "SELECT 1 AS data; "
                                   "IF p = 2 THEN UPDATE %2 SET v = v + 1 WHERE id = 1; COMMIT; SELECT 2; END IF; "
                                   "SELECT v FROM %2 WHERE id = 2 FOR UPDATE; SELECT 3; END" )
                             .arg( committingProcedure, table ) );

         // The US3 schema's status functions, created only where they are missing.
         createdStatusFunctions = scalar( *first,
                                          "SELECT COUNT(*) FROM information_schema.ROUTINES WHERE "
                                          "ROUTINE_SCHEMA = DATABASE() AND ROUTINE_NAME = 'last_errno'" ) == "0";
         if ( createdStatusFunctions )
         {
            first->rawQuery( "CREATE FUNCTION last_errno() RETURNS INT NO SQL RETURN @US3_LAST_ERRNO" );
            first->rawQuery( "CREATE FUNCTION last_error() RETURNS TEXT NO SQL RETURN @US3_LAST_ERROR" );
         }

         ASSERT_EQ( scalar( *first,
                            QString( "SELECT COUNT(*) FROM information_schema.ROUTINES WHERE ROUTINE_SCHEMA = DATABASE() "
                                     "AND ROUTINE_NAME IN ( '%1', '%2', '%3', '%4', 'last_errno', 'last_error' )" )
                               .arg( rollbackProcedure, failingProcedure, silentProcedure, committingProcedure ) ),
                    "6" )
            << "the test user needs CREATE ROUTINE";
      }

      void TearDown() override
      {
         if ( first )
         {
            first->rawQuery( QString( "DROP TABLE IF EXISTS %1" ).arg( table ) );
            first->rawQuery( QString( "DROP PROCEDURE IF EXISTS %1" ).arg( rollbackProcedure ) );
            first->rawQuery( QString( "DROP PROCEDURE IF EXISTS %1" ).arg( failingProcedure ) );
            first->rawQuery( QString( "DROP PROCEDURE IF EXISTS %1" ).arg( silentProcedure ) );
            first->rawQuery( QString( "DROP PROCEDURE IF EXISTS %1" ).arg( committingProcedure ) );
            first->rawQuery( QString( "DROP TABLE IF EXISTS %1" ).arg( ddlTable ) );
            if ( createdStatusFunctions )
            {
               first->rawQuery( "DROP FUNCTION IF EXISTS last_errno" );
               first->rawQuery( "DROP FUNCTION IF EXISTS last_error" );
            }
         }
      }

      // Waits until the connection with this ID waits for a row lock; a
      // connection killed before may still be listed as waiting.  InnoDB
      // refreshes what INNODB_TRX shows only after 0.1 s without a read of
      // it, so polling faster than that would read the same snapshot forever.
      void awaitLockWait( US_DB2& observer, const QString& connectionId )
      {
         for ( int tries = 0; tries < 50; ++tries )
         {
            if ( scalar( observer, QString( "SELECT COUNT(*) FROM information_schema.INNODB_TRX WHERE "
                                            "trx_state = 'LOCK WAIT' AND trx_mysql_thread_id = %1" )
                                      .arg( connectionId ) ) != "0" )
            {
               return;
            }
            std::this_thread::sleep_for( std::chrono::milliseconds( 150 ) );
         }
         ADD_FAILURE() << "connection " << connectionId.toStdString() << " did not wait for a lock: "
                       << scalar( observer, "SELECT GROUP_CONCAT( trx_mysql_thread_id, ' ', trx_state, ' ', "
                                            "IFNULL( trx_query, '-' ) SEPARATOR '; ' ) FROM information_schema.INNODB_TRX" )
                             .toStdString()
                       << " / " << observer.lastError().toStdString();
      }

      // Each connection locks one row, then asks for the other's.  The server
      // breaks the cycle by rolling one of them back; that one is returned.
      US_DB2* deadlock()
      {
         const QString firstId = scalar( *first, "SELECT CONNECTION_ID()" );
         QString       error;
         EXPECT_TRUE( first->beginTransaction( error ) ) << error.toStdString();
         EXPECT_TRUE( second->beginTransaction( error ) ) << error.toStdString();

         first->rawQuery( QString( "UPDATE %1 SET v = 1 WHERE id = 1" ).arg( table ) );
         second->rawQuery( QString( "UPDATE %1 SET v = 2 WHERE id = 2" ).arg( table ) );

         // This one blocks on the second connection's lock, so it runs aside.
         std::thread blocked( [this] { first->rawQuery( QString( "UPDATE %1 SET v = 1 WHERE id = 2" ).arg( table ) ); } );

         // Wait until the server shows it waiting, so the next update closes the
         // cycle rather than racing it.
         awaitLockWait( *second, firstId );

         second->rawQuery( QString( "UPDATE %1 SET v = 2 WHERE id = 1" ).arg( table ) );
         blocked.join();

         const bool first_lost  = first->lastError().contains( "Deadlock" );
         const bool second_lost = second->lastError().contains( "Deadlock" );
         EXPECT_NE( first_lost, second_lost ) << "exactly one connection should be the deadlock victim";

         return first_lost ? first.get() : second.get();
      }

      US_DB2* survivor( US_DB2* victim )
      {
         return victim == first.get() ? second.get() : first.get();
      }

      std::unique_ptr<US_DB2> first;
      std::unique_ptr<US_DB2> second;
      bool                    createdStatusFunctions = false;
};

// Any statement a caller runs after a failed query, such as an error lookup,
// refreshes the server status, which no longer shows a transaction.  The
// rollback must not read that as an implicit commit.
TEST_F( US_DB2_LiveTransaction, RollbackAfterDeadlockNamesTheDeadlock )
{
   US_DB2* victim = deadlock();
   scalar( *victim, "SELECT 1" );

   QString error;
   EXPECT_FALSE( victim->rollbackTransaction( error ) );
   EXPECT_TRUE( error.contains( "deadlock" ) ) << error.toStdString();
   EXPECT_FALSE( error.contains( "may have been kept" ) ) << error.toStdString();

   EXPECT_TRUE( survivor( victim )->commitTransaction( error ) ) << error.toStdString();
}

// With nothing run after the deadlock, the status still predates it, and a
// COMMIT would succeed as a no-op.  The commit must be refused all the same.
TEST_F( US_DB2_LiveTransaction, CommitAfterDeadlockIsRefused )
{
   US_DB2* victim = deadlock();

   QString error;
   EXPECT_FALSE( victim->commitTransaction( error ) );
   EXPECT_TRUE( error.contains( "deadlock" ) ) << error.toStdString();

   // The caller rolls back after a failed commit, as the contract asks.
   EXPECT_FALSE( victim->rollbackTransaction( error ) );
   EXPECT_TRUE( error.contains( "deadlock" ) ) << error.toStdString();

   US_DB2* winner = survivor( victim );
   EXPECT_TRUE( winner->commitTransaction( error ) ) << error.toStdString();

   // Only the survivor's writes are in the table.
   const QString expected = winner == first.get() ? "1" : "2";
   EXPECT_EQ( scalar( *winner, QString( "SELECT v FROM %1 WHERE id = 1" ).arg( table ) ), expected );
   EXPECT_EQ( scalar( *winner, QString( "SELECT v FROM %1 WHERE id = 2" ).arg( table ) ), expected );
}

// A transaction ended by a statement that commits implicitly, with no error
// to say so, is reported without guessing which way it ended.
TEST_F( US_DB2_LiveTransaction, RollbackAfterImplicitCommitDoesNotClaimACause )
{
   QString error;
   ASSERT_TRUE( first->beginTransaction( error ) ) << error.toStdString();
   first->rawQuery( QString( "UPDATE %1 SET v = 1 WHERE id = 1" ).arg( table ) );
   first->rawQuery( QString( "ANALYZE TABLE %1" ).arg( table ) );

   EXPECT_FALSE( first->rollbackTransaction( error ) );
   EXPECT_TRUE( error.contains( "may have been kept" ) ) << error.toStdString();
   EXPECT_FALSE( error.contains( "deadlock" ) ) << error.toStdString();
   EXPECT_EQ( scalar( *first, QString( "SELECT v FROM %1 WHERE id = 1" ).arg( table ) ), "1" );
}

// The transaction ends with no error the client could record, so only the
// server status shows it.  A COMMIT now would succeed as a no-op.
TEST_F( US_DB2_LiveTransaction, CommitAfterSilentRollbackIsRefused )
{
   QString error;
   ASSERT_TRUE( first->beginTransaction( error ) ) << error.toStdString();
   first->rawQuery( QString( "UPDATE %1 SET v = 1 WHERE id = 1" ).arg( table ) );
   first->rawQuery( QString( "CALL %1()" ).arg( rollbackProcedure ) );

   EXPECT_FALSE( first->commitTransaction( error ) );
   EXPECT_TRUE( error.contains( "may have been kept" ) ) << error.toStdString();

   EXPECT_FALSE( first->rollbackTransaction( error ) );
   EXPECT_EQ( scalar( *first, QString( "SELECT v FROM %1 WHERE id = 1" ).arg( table ) ), "0" );
}

// With innodb_rollback_on_timeout off, the default, a timeout undoes only the
// statement; the transaction and the writes before it are still there.
TEST_F( US_DB2_LiveTransaction, CommitAfterLockWaitTimeoutKeepsEarlierWrites )
{
   if ( scalar( *first, "SELECT @@innodb_rollback_on_timeout" ) != "0" )
   {
      GTEST_SKIP() << "innodb_rollback_on_timeout is set on this server";
   }

   QString error;
   ASSERT_TRUE( first->beginTransaction( error ) ) << error.toStdString();
   ASSERT_TRUE( second->beginTransaction( error ) ) << error.toStdString();
   second->rawQuery( QString( "UPDATE %1 SET v = 2 WHERE id = 2" ).arg( table ) );

   first->rawQuery( "SET SESSION innodb_lock_wait_timeout = 1" );
   first->rawQuery( QString( "UPDATE %1 SET v = 1 WHERE id = 1" ).arg( table ) );
   first->rawQuery( QString( "UPDATE %1 SET v = 1 WHERE id = 2" ).arg( table ) );
   ASSERT_TRUE( first->lastError().contains( "Lock wait timeout" ) ) << first->lastError().toStdString();

   EXPECT_TRUE( first->commitTransaction( error ) ) << error.toStdString();
   EXPECT_TRUE( second->rollbackTransaction( error ) ) << error.toStdString();
   EXPECT_EQ( scalar( *first, QString( "SELECT v FROM %1 WHERE id = 1" ).arg( table ) ), "1" );
   EXPECT_EQ( scalar( *first, QString( "SELECT v FROM %1 WHERE id = 2" ).arg( table ) ), "0" );
}

// A transaction whose connection is lost is rolled back by the server: that
// is known, and the commit and rollback say so instead of "some rows may
// have been kept".  Three ways the client learns of it: at the commit, at
// the rollback, or from a statement before either.
TEST_F( US_DB2_LiveTransaction, ALostConnectionSaysTheServerRolledTheTransactionBack )
{
   enum Notice { AtCommit, AtRollback, AtStatement };
   for ( Notice notice : { AtCommit, AtRollback, AtStatement } )
   {
      SCOPED_TRACE( "noticed " + std::to_string( int( notice ) ) );
      const LiveDb live = liveDb();
      US_DB2       lost;
      QString      error;
      ASSERT_TRUE( lost.connect( live.host, live.name, live.user, live.password, error ) ) << error.toStdString();

      ASSERT_TRUE( lost.beginTransaction( error ) ) << error.toStdString();
      lost.rawQuery( QString( "UPDATE %1 SET v = 7 WHERE id = 1" ).arg( table ) );
      ASSERT_FALSE( lost.lastError().startsWith( "MySQL error" ) ) << lost.lastError().toStdString();
      const QString id = scalar( lost, "SELECT CONNECTION_ID()" );

      second->rawQuery( "KILL " + id );
      ASSERT_FALSE( second->lastError().startsWith( "MySQL error" ) ) << second->lastError().toStdString();

      if ( notice == AtStatement )
      {
         lost.query( "SELECT 41 + 1" );
         EXPECT_FALSE( lost.isConnected() ) << lost.lastServerErrno() << " " << lost.lastError().toStdString();
      }

      if ( notice != AtRollback )
      {
         EXPECT_FALSE( lost.commitTransaction( error ) );
         EXPECT_EQ( error, QObject::tr( US_DB2::connectionLostRollbackMessage ) ) << error.toStdString();
      }

      EXPECT_FALSE( lost.rollbackTransaction( error ) );
      EXPECT_EQ( error, QObject::tr( US_DB2::connectionLostRollbackMessage ) ) << error.toStdString();
      EXPECT_FALSE( error.contains( "may have been kept" ) ) << error.toStdString();
      EXPECT_FALSE( lost.isConnected() );
      EXPECT_FALSE( lost.transactionActive() );

      // And it was: the row is as it was
      EXPECT_EQ( scalar( *second, QString( "SELECT v FROM %1 WHERE id = 1" ).arg( table ) ), "0" );
   }
}

TEST_F( US_DB2_LiveTransaction, UnmatchedTransactionCallsAreRefused )
{
   QString error;
   EXPECT_FALSE( first->commitTransaction( error ) );
   EXPECT_EQ( error, QObject::tr( US_DB2::noTransactionMessage ).arg( "commitTransaction" ) );
   EXPECT_FALSE( first->rollbackTransaction( error ) );
   EXPECT_EQ( error, QObject::tr( US_DB2::noTransactionMessage ).arg( "rollbackTransaction" ) );

   ASSERT_TRUE( first->beginTransaction( error ) ) << error.toStdString();
   EXPECT_FALSE( first->beginTransaction( error ) );
   EXPECT_EQ( error, QObject::tr( US_DB2::transactionOpenMessage ).arg( "beginTransaction" ) );

   // The refused begin left the first transaction open.
   EXPECT_TRUE( first->commitTransaction( error ) ) << error.toStdString();
}

// query() reads a CALL as a status set followed by a data set.  A failure on
// the data set is the query's result, not the status row's OK.
TEST_F( US_DB2_LiveTransaction, QueryReportsAFailureAfterTheStatusRow )
{
   first->query( QString( "CALL %1()" ).arg( failingProcedure ) );

   EXPECT_EQ( first->lastErrno(), IUS_DB2::DBERROR );
   EXPECT_TRUE( first->lastError().contains( "us3_test_no_such_table" ) ) << first->lastError().toStdString();
   EXPECT_FALSE( first->next() );
}

// A procedure that returns without its status set reports the status it
// recorded, not the previous query's (ultrascan-tickets#1018).
TEST_F( US_DB2_LiveTransaction, QueryReadsTheStatusOfAProcedureThatReturnsNoSet )
{
   first->query( "SELECT 0" );
   ASSERT_EQ( first->lastErrno(), IUS_DB2::OK );

   first->query( QString( "CALL %1( %2 )" ).arg( silentProcedure ).arg( IUS_DB2::EMPTY ) );
   EXPECT_EQ( first->lastErrno(), IUS_DB2::EMPTY );
   EXPECT_EQ( first->lastError(), QString( "silent status %1" ).arg( IUS_DB2::EMPTY ) );
   EXPECT_FALSE( first->next() );

   first->query( QString( "CALL %1( 0 )" ).arg( silentProcedure ) );
   EXPECT_EQ( first->lastErrno(), IUS_DB2::OK );
   EXPECT_TRUE( first->lastError().isEmpty() ) << first->lastError().toStdString();
}

// A CALL the server refuses itself reports the server's reason, not what
// last_error() still holds from an earlier procedure.
TEST_F( US_DB2_LiveTransaction, StatusQueryKeepsTheServersReasonForARefusedCall )
{
   const QString earlier = QString( "silent status %1" ).arg( IUS_DB2::EMPTY );
   first->query( QString( "CALL %1( %2 )" ).arg( silentProcedure ).arg( IUS_DB2::EMPTY ) );
   ASSERT_EQ( scalar( *first, "SELECT last_error()" ), earlier );

   // The procedure takes one argument.
   EXPECT_EQ( first->statusQuery( QString( "CALL %1()" ).arg( silentProcedure ) ), IUS_DB2::DBERROR );
   EXPECT_EQ( first->lastErrno(), IUS_DB2::DBERROR );
   EXPECT_TRUE( first->lastError().contains( silentProcedure ) ) << first->lastError().toStdString();
   EXPECT_NE( first->lastError(), earlier );
}

// A transaction that ended before the connection was lost -- an implicit
// commit, here ANALYZE TABLE -- kept what it wrote, and what ran after it
// autocommitted.  The loss must not be reported as "nothing was kept".
TEST_F( US_DB2_LiveTransaction, ALostConnectionAfterAnImplicitCommitSaysRowsMayHaveBeenKept )
{
   enum Notice { AtCommit, AtRollback, AtStatement };
   for ( Notice notice : { AtCommit, AtRollback, AtStatement } )
   {
      SCOPED_TRACE( "noticed " + std::to_string( int( notice ) ) );
      first->rawQuery( QString( "UPDATE %1 SET v = 0" ).arg( table ) );

      const LiveDb live = liveDb();
      US_DB2       lost;
      QString      error;
      ASSERT_TRUE( lost.connect( live.host, live.name, live.user, live.password, error ) ) << error.toStdString();

      ASSERT_TRUE( lost.beginTransaction( error ) ) << error.toStdString();
      lost.rawQuery( QString( "UPDATE %1 SET v = 7 WHERE id = 1" ).arg( table ) );
      lost.rawQuery( QString( "ANALYZE TABLE %1" ).arg( table ) );
      lost.rawQuery( QString( "UPDATE %1 SET v = 8 WHERE id = 2" ).arg( table ) );
      ASSERT_FALSE( lost.lastError().startsWith( "MySQL error" ) ) << lost.lastError().toStdString();
      const QString id = scalar( lost, "SELECT CONNECTION_ID()" );

      second->rawQuery( "KILL " + id );
      ASSERT_FALSE( second->lastError().startsWith( "MySQL error" ) ) << second->lastError().toStdString();

      if ( notice == AtStatement )
      {
         lost.query( "SELECT 41 + 1" );
         EXPECT_FALSE( lost.isConnected() );
      }

      if ( notice != AtRollback )
      {
         EXPECT_FALSE( lost.commitTransaction( error ) );
         EXPECT_EQ( error, QObject::tr( US_DB2::transactionEndedMessage ) ) << error.toStdString();
      }

      EXPECT_FALSE( lost.rollbackTransaction( error ) );
      EXPECT_EQ( error, QObject::tr( US_DB2::transactionEndedMessage ) ) << error.toStdString();
      EXPECT_FALSE( lost.transactionActive() );

      // And they were kept: the row before the commit and the one after it
      EXPECT_EQ( scalar( *second, QString( "SELECT v FROM %1 WHERE id = 1" ).arg( table ) ), "7" );
      EXPECT_EQ( scalar( *second, QString( "SELECT v FROM %1 WHERE id = 2" ).arg( table ) ), "8" );
   }
}

// A deadlock, then a lost connection: the deadlock rolled the transaction
// back, which the message says, rather than the loss
TEST_F( US_DB2_LiveTransaction, ALostConnectionAfterADeadlockNamesTheDeadlock )
{
   US_DB2*       victim = deadlock();
   const QString id     = scalar( *victim, "SELECT CONNECTION_ID()" );
   ASSERT_FALSE( id.isEmpty() );

   const LiveDb live = liveDb();
   US_DB2       killer;
   QString      error;
   ASSERT_TRUE( killer.connect( live.host, live.name, live.user, live.password, error ) ) << error.toStdString();
   killer.rawQuery( "KILL " + id );
   ASSERT_FALSE( killer.lastError().startsWith( "MySQL error" ) ) << killer.lastError().toStdString();

   victim->query( "SELECT 41 + 1" );
   EXPECT_FALSE( victim->isConnected() );

   EXPECT_FALSE( victim->commitTransaction( error ) );
   EXPECT_EQ( error, QObject::tr( US_DB2::serverRollbackMessage ) ) << error.toStdString();
   EXPECT_FALSE( victim->rollbackTransaction( error ) );
   EXPECT_EQ( error, QObject::tr( US_DB2::serverRollbackMessage ) ) << error.toStdString();

   EXPECT_TRUE( survivor( victim )->commitTransaction( error ) ) << error.toStdString();
}

// The error of the last statement belongs to that connection: a new one
// starts without it, so lastErrorIsConnectionLoss() no longer says the
// connection is gone
TEST_F( US_DB2_LiveTransaction, AReconnectClearsTheLostConnectionError )
{
   const LiveDb live = liveDb();
   US_DB2       db;
   QString      error;
   ASSERT_TRUE( db.connect( live.host, live.name, live.user, live.password, error ) ) << error.toStdString();
   const QString id = scalar( db, "SELECT CONNECTION_ID()" );

   second->rawQuery( "KILL " + id );
   db.query( "SELECT 41 + 1" );
   ASSERT_FALSE( db.isConnected() );
   ASSERT_TRUE( db.lastErrorIsConnectionLoss() ) << db.lastServerErrno();

   ASSERT_TRUE( db.connect( live.host, live.name, live.user, live.password, error ) ) << error.toStdString();
   EXPECT_EQ( db.lastServerErrno(), 0 );
   EXPECT_FALSE( db.lastErrorIsConnectionLoss() );
}

// rawQuery() and writeBlobToDB() say how the server refused a statement, as
// query() and statusQuery() do, and a statement it accepted clears it
TEST_F( US_DB2_LiveTransaction, RawQueryAndWriteBlobSetTheServerError )
{
   first->rawQuery( "SELECT * FROM us3_test_no_such_table" );
   EXPECT_EQ( first->lastServerErrno(), 1146 );   // ER_NO_SUCH_TABLE
   first->rawQuery( "SELECT 1" );
   EXPECT_EQ( first->lastServerErrno(), 0 );

   QTemporaryDir dir;
   ASSERT_TRUE( dir.isValid() );
   const QString path = dir.filePath( "blob.bin" );
   QFile         file( path );
   ASSERT_TRUE( file.open( QIODevice::WriteOnly ) );
   file.write( "some bytes" );
   file.close();

   EXPECT_EQ( first->writeBlobToDB( path, "us3_test_no_such_procedure", 1 ), IUS_DB2::DBERROR );
   EXPECT_EQ( first->lastServerErrno(), 1305 );   // ER_SP_DOES_NOT_EXIST

   // A connection lost before the blob went out says so too
   const LiveDb live = liveDb();
   US_DB2       db;
   QString      error;
   ASSERT_TRUE( db.connect( live.host, live.name, live.user, live.password, error ) ) << error.toStdString();
   second->rawQuery( "KILL " + scalar( db, "SELECT CONNECTION_ID()" ) );
   EXPECT_EQ( db.writeBlobToDB( path, "us3_test_no_such_procedure", 1 ), IUS_DB2::DBERROR );
   EXPECT_TRUE( db.lastErrorIsConnectionLoss() ) << db.lastServerErrno() << " " << db.lastError().toStdString();
}

// DDL commits the open transaction implicitly; what follows runs outside it.
// A deadlock after that -- here in a transaction a procedure would have
// started -- rolls back only what ran since, so it must not be reported as
// "nothing written in it was kept": the row written before the DDL was.
TEST_F( US_DB2_LiveTransaction, ADeadlockAfterAnImplicitCommitSaysRowsMayHaveBeenKept )
{
   const QString firstId = scalar( *first, "SELECT CONNECTION_ID()" );
   QString       error;
   ASSERT_TRUE( first->beginTransaction( error ) ) << error.toStdString();
   first->rawQuery( QString( "UPDATE %1 SET v = 5 WHERE id = 1" ).arg( table ) );
   first->rawQuery( QString( "CREATE TABLE %1 ( id INT PRIMARY KEY ) ENGINE=InnoDB" ).arg( ddlTable ) );
   ASSERT_EQ( first->lastServerErrno(), 0 ) << first->lastError().toStdString();
   first->rawQuery( "START TRANSACTION" );

   // The second transaction writes more, so the server picks the first one
   // as the deadlock victim
   ASSERT_TRUE( second->beginTransaction( error ) ) << error.toStdString();
   for ( int id = 100; id < 150; ++id )
      second->rawQuery( QString( "INSERT INTO %1 VALUES ( %2, 0 )" ).arg( table ).arg( id ) );

   first->rawQuery( QString( "UPDATE %1 SET v = 1 WHERE id = 1" ).arg( table ) );
   second->rawQuery( QString( "UPDATE %1 SET v = 2 WHERE id = 2" ).arg( table ) );
   std::thread blocked( [this] { first->rawQuery( QString( "UPDATE %1 SET v = 1 WHERE id = 2" ).arg( table ) ); } );
   awaitLockWait( *second, firstId );
   second->rawQuery( QString( "UPDATE %1 SET v = 2 WHERE id = 1" ).arg( table ) );
   blocked.join();
   ASSERT_TRUE( first->lastError().contains( "Deadlock" ) ) << first->lastError().toStdString();
   ASSERT_EQ( second->lastServerErrno(), 0 ) << second->lastError().toStdString();

   EXPECT_FALSE( first->commitTransaction( error ) );
   EXPECT_EQ( error, QObject::tr( US_DB2::transactionEndedMessage ) ) << error.toStdString();
   EXPECT_FALSE( first->rollbackTransaction( error ) );
   EXPECT_EQ( error, QObject::tr( US_DB2::transactionEndedMessage ) ) << error.toStdString();

   ASSERT_TRUE( second->rollbackTransaction( error ) ) << error.toStdString();

   // The write before the DDL was kept
   EXPECT_EQ( scalar( *second, QString( "SELECT v FROM %1 WHERE id = 1" ).arg( table ) ), "5" );
}

// A procedure commits part way, and the connection is lost while the client
// still reads its later result sets.  The set sent after the COMMIT showed
// the transaction ended, so the loss is not "the server rolled it back":
// the procedure's write was kept.  Commits after the status set (read by
// query()) and after the data set (read when the next statement clears the
// connection) are both covered.
TEST_F( US_DB2_LiveTransaction, ALostConnectionAfterAProcedureCommittedSaysRowsMayHaveBeenKept )
{
   for ( int stage : { 1, 2 } )
   {
      SCOPED_TRACE( "commit at stage " + std::to_string( stage ) );
      first->rawQuery( QString( "UPDATE %1 SET v = 0" ).arg( table ) );

      const LiveDb live = liveDb();
      US_DB2       lost;
      QString      error;
      ASSERT_TRUE( lost.connect( live.host, live.name, live.user, live.password, error ) ) << error.toStdString();
      const QString id = scalar( lost, "SELECT CONNECTION_ID()" );

      // Holds row 2, where the procedure waits after its commit
      ASSERT_TRUE( second->beginTransaction( error ) ) << error.toStdString();
      second->rawQuery( QString( "UPDATE %1 SET v = 9 WHERE id = 2" ).arg( table ) );

      ASSERT_TRUE( lost.beginTransaction( error ) ) << error.toStdString();
      lost.rawQuery( QString( "UPDATE %1 SET v = 5 WHERE id = 1" ).arg( table ) );
      lost.query( QString( "CALL %1( %2 )" ).arg( committingProcedure ).arg( stage ) );
      ASSERT_EQ( lost.lastErrno(), IUS_DB2::OK ) << lost.lastError().toStdString();
      ASSERT_TRUE( lost.next() );
      EXPECT_EQ( lost.value( 0 ).toString(), "1" );

      awaitLockWait( *second, id );
      second->rawQuery( "KILL " + id );
      ASSERT_EQ( second->lastServerErrno(), 0 ) << second->lastError().toStdString();
      ASSERT_TRUE( second->rollbackTransaction( error ) ) << error.toStdString();

      EXPECT_FALSE( lost.rollbackTransaction( error ) );
      EXPECT_FALSE( lost.isConnected() );
      EXPECT_EQ( error, QObject::tr( US_DB2::transactionEndedMessage ) ) << error.toStdString();

      // And it was kept: the client's write and the procedure's, committed together
      EXPECT_EQ( scalar( *second, QString( "SELECT v FROM %1 WHERE id = 1" ).arg( table ) ), "6" );
   }
}
#endif
} // namespace
