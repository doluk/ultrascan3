//! \file test_us_db2_limits_live.cpp
// What US_DB2 does at the edges of what a connection carries:
//   - a column holding a NUL is read whole, not up to the NUL;
//   - text with a lone UTF-16 surrogate is refused, not stored as '?';
//   - a blob too large for the server's max_allowed_packet is refused before
//     it is sent, with a message that says so, and the connection stays up;
//   - a blob just below that limit arrives byte for byte, also where a
//     multi-byte UTF-8 sequence straddles the pieces it is escaped in;
//   - a connection the server dropped is no longer "connected", and
//     connect() opens a new one.
//
// They need a MariaDB or MySQL server and skip without one.  Set
//   US3_TEST_DB_HOST      host or host:port
//   US3_TEST_DB_NAME      a scratch database; the test creates a procedure
//   US3_TEST_DB_USER      needs CREATE ROUTINE on that database
//   US3_TEST_DB_PASSWORD
//
// US_DB2 always connects over TLS, with etc/mysql/ca-cert.pem as its CA and
// no client certificate, so the server must have TLS on.

#include <gtest/gtest.h>

#include "us_db2.h"

#include <QCryptographicHash>
#include <QFile>
#include <QRandomGenerator>
#include <QString>
#include <QTemporaryDir>
#include <QThread>

namespace
{
#ifndef NO_DB
const char* const blobProcedure = "us3_test_blob_md5";

class US_DB2_LiveLimits : public ::testing::Test
{
   protected:
      void SetUp() override
      {
         if ( host.isEmpty() || name.isEmpty() || user.isEmpty() )
         {
            GTEST_SKIP() << "US3_TEST_DB_HOST, US3_TEST_DB_NAME and US3_TEST_DB_USER not set";
         }

         QString error;
         ASSERT_TRUE( db.connect( host, name, user, password, error ) ) << error.toStdString();
         ASSERT_TRUE( scratch.isValid() );
      }

      void TearDown() override
      {
         if ( db.isConnected() )
         {
            db.rawQuery( QString( "DROP PROCEDURE IF EXISTS %1" ).arg( blobProcedure ) );
         }
      }

      //! A procedure with the blob upload's parameters, that answers 0 when
      //! the data's MD5 is the checksum sent and 106 (BAD_CHECKSUM) otherwise
      void createBlobProcedure( void )
      {
         db.rawQuery( QString( "DROP PROCEDURE IF EXISTS %1" ).arg( blobProcedure ) );
         db.rawQuery( QString( "CREATE PROCEDURE %1( g TEXT, p TEXT, id INT, d LONGBLOB, c TEXT ) "
                               "SELECT IF( MD5( d ) = c, 0, 106 ) AS status" )
                         .arg( blobProcedure ) );
         ASSERT_TRUE( db.lastError().isEmpty() || !db.lastError().startsWith( "MySQL error" ) )
            << db.lastError().toStdString();
      }

      qint64 maxAllowedPacket( void )
      {
         db.rawQuery( "SELECT @@max_allowed_packet" );
         return db.next() ? db.value( 0 ).toLongLong() : 0;
      }

      //! The connection still answers
      bool answers( US_DB2& conn )
      {
         conn.rawQuery( "SELECT 41 + 1" );
         return conn.next() && conn.value( 0 ).toInt() == 42;
      }

      QString writeFile( const QString& fileName, const QByteArray& data )
      {
         const QString path = scratch.filePath( fileName );
         QFile         file( path );
         if ( !file.open( QIODevice::WriteOnly ) || file.write( data ) != data.size() )
         {
            ADD_FAILURE() << "could not write " << path.toStdString();
         }
         return path;
      }

      US_DB2        db;
      QTemporaryDir scratch;

      QString host     = qEnvironmentVariable( "US3_TEST_DB_HOST" );
      QString name     = qEnvironmentVariable( "US3_TEST_DB_NAME" );
      QString user     = qEnvironmentVariable( "US3_TEST_DB_USER" );
      QString password = qEnvironmentVariable( "US3_TEST_DB_PASSWORD" );
};

// value() used to read a column as a C string, so it ended at the first NUL
TEST_F( US_DB2_LiveLimits, AColumnHoldingANulIsReadWhole )
{
   db.rawQuery( "SELECT 'before\\0after', CONCAT( 'x', CHAR( 0 USING utf8mb4 ), '\xce\xb1' ), NULL" );
   ASSERT_TRUE( db.next() ) << db.lastError().toStdString();

   const QString first = db.value( 0 ).toString();
   EXPECT_EQ( first.size(), 12 );
   EXPECT_EQ( first, QString( "before" ) + QChar( '\0' ) + "after" );

   const QString second = db.value( 1 ).toString();
   EXPECT_EQ( second, QString( "x" ) + QChar( '\0' ) + QChar( 0x03B1 ) );

   EXPECT_TRUE( db.value( 2 ).toString().isNull() ) << "a NULL column is still a null string";
}

// Qt writes a lone surrogate as '?' in UTF-8, so the server stored a '?'
// where the text had something else, and said nothing
TEST_F( US_DB2_LiveLimits, TextWithALoneSurrogateIsRefusedNotStoredAsAQuestionMark )
{
   const QString lone = QString( "ab" ) + QChar( 0xD83D ) + "cd";

   db.rawQuery( "SELECT '" + lone + "'" );
   EXPECT_EQ( db.lastErrno(), IUS_DB2::DBERROR );
   EXPECT_TRUE( db.lastError().contains( "surrogate" ) ) << db.lastError().toStdString();
   EXPECT_FALSE( db.next() ) << "something was sent: " << db.value( 0 ).toString().toStdString();

   db.query( QStringList() << "us3_no_such_procedure" << lone );
   EXPECT_EQ( db.lastErrno(), IUS_DB2::DBERROR );
   EXPECT_TRUE( db.lastError().contains( "surrogate" ) ) << db.lastError().toStdString();
   EXPECT_EQ( db.lastServerErrno(), 0 ) << "the server was asked";

   EXPECT_EQ( db.statusQuery( QStringList() << "us3_no_such_procedure" << lone ), IUS_DB2::DBERROR );
   EXPECT_TRUE( db.lastError().contains( "surrogate" ) ) << db.lastError().toStdString();

   // A whole pair is a character like any other
   db.rawQuery( QString( "SELECT HEX( '" ) + QString::fromUtf8( "\U0001F600" ) + "' )" );
   ASSERT_TRUE( db.next() ) << db.lastError().toStdString();
   EXPECT_EQ( db.value( 0 ).toString(), QString( "F09F9880" ) );
   EXPECT_TRUE( answers( db ) );
}

// The server closes the connection on a statement over max_allowed_packet,
// and the upload then failed with "server has gone away" -- on a connection
// that still said it was connected
TEST_F( US_DB2_LiveLimits, ABlobOverMaxAllowedPacketIsRefusedBeforeItIsSent )
{
   createBlobProcedure();
   const qint64 packet = maxAllowedPacket();
   ASSERT_GT( packet, 0 );
   ASSERT_LE( packet, qint64( 256 ) * 1024 * 1024 ) << "too large a limit to test here";

   // Exactly the limit: with the statement around it, more than the limit
   const QString path = writeFile( "big.bin", QByteArray( int( packet ), 'A' ) );
   EXPECT_EQ( db.writeBlobToDB( path, blobProcedure, 1 ), IUS_DB2::DBERROR );
   EXPECT_TRUE( db.lastError().contains( "max_allowed_packet" ) ) << db.lastError().toStdString();
   EXPECT_TRUE( db.lastError().contains( QString::number( packet ) ) ) << db.lastError().toStdString();
   EXPECT_TRUE( db.isConnected() );
   EXPECT_TRUE( answers( db ) ) << "the connection was lost: " << db.lastError().toStdString();

   // Small enough as a file, too large once escaped: every byte is a quote
   const QString quoted = writeFile( "quotes.bin", QByteArray( int( packet * 3 / 4 ), '\'' ) );
   EXPECT_EQ( db.writeBlobToDB( quoted, blobProcedure, 1 ), IUS_DB2::DBERROR );
   EXPECT_TRUE( db.lastError().contains( "max_allowed_packet" ) ) << db.lastError().toStdString();
   EXPECT_TRUE( db.isConnected() );
   EXPECT_TRUE( answers( db ) ) << "the connection was lost: " << db.lastError().toStdString();
}

// The server measures the packet -- a command byte and the statement -- and
// refuses one of max_allowed_packet bytes or more, closing the connection.
// So the longest statement it takes is max_allowed_packet - 2 bytes.  The
// check let a statement of max_allowed_packet - 1 bytes through, and that
// one killed the connection
TEST_F( US_DB2_LiveLimits, ABlobStatementAtTheExactLimitArrivesAndOneByteMoreIsRefused )
{
   createBlobProcedure();
   const qint64 packet = maxAllowedPacket();
   ASSERT_GT( packet, 0 );
   ASSERT_LE( packet, qint64( 256 ) * 1024 * 1024 ) << "too large a limit to test here";

   // The statement around the data, with no guid or password: the head
   // writeBlobToDB() builds, then "', '", the MD5 in hex, "')"
   const qint64 around  = QString( "CALL %1('', '', 1, _binary'" ).arg( blobProcedure ).toUtf8().size() + 4 + 32 + 2;
   const qint64 longest = packet - 2;

   // Bytes that need no escaping, so the statement is exactly this long
   const QString atLimit = writeFile( "at.bin", QByteArray( int( longest - around ), 'A' ) );
   EXPECT_EQ( db.writeBlobToDB( atLimit, blobProcedure, 1 ), IUS_DB2::OK ) << db.lastError().toStdString();
   EXPECT_TRUE( db.isConnected() );
   EXPECT_TRUE( answers( db ) ) << "the connection was lost: " << db.lastError().toStdString();

   const QString over = writeFile( "over.bin", QByteArray( int( longest - around + 1 ), 'A' ) );
   EXPECT_EQ( db.writeBlobToDB( over, blobProcedure, 1 ), IUS_DB2::DBERROR );
   EXPECT_TRUE( db.lastError().contains( "max_allowed_packet" ) ) << db.lastError().toStdString();
   EXPECT_TRUE( db.lastError().contains( QString( "at most %1 bytes" ).arg( longest ) ) )
      << db.lastError().toStdString();
   EXPECT_EQ( db.lastServerErrno(), 0 ) << "it was sent: " << db.lastError().toStdString();
   EXPECT_TRUE( db.isConnected() );
   EXPECT_TRUE( answers( db ) ) << "the connection was lost: " << db.lastError().toStdString();

   // One byte that escapes to two, at the limit: refused while it is escaped
   QByteArray escaped( int( longest - around ), 'A' );
   escaped[ 0 ] = '\'';
   const QString grown = writeFile( "grown.bin", escaped );
   EXPECT_EQ( db.writeBlobToDB( grown, blobProcedure, 1 ), IUS_DB2::DBERROR );
   EXPECT_TRUE( db.lastError().contains( "max_allowed_packet" ) ) << db.lastError().toStdString();
   EXPECT_TRUE( answers( db ) ) << "the connection was lost: " << db.lastError().toStdString();
}

// The connection knows its server, as the server names itself
TEST_F( US_DB2_LiveLimits, TheConnectionKnowsItsServer )
{
   db.rawQuery( "SELECT VERSION()" );
   ASSERT_TRUE( db.next() );
   const QString version = db.value( 0 ).toString();
   EXPECT_EQ( db.serverVersion(), version );
   EXPECT_NE( US_DB2::serverIsMariaDB( db.serverVersion() ), US_DB2::serverIsMySQL( db.serverVersion() ) );
}

// MariaDB uses 4031 for a statement error, after which the connection is
// fine; it used to be taken for MySQL's idle timeout, and the connection
// for gone
TEST_F( US_DB2_LiveLimits, Error4031OnMariaDBKeepsTheConnection )
{
   if ( !US_DB2::serverIsMariaDB( db.serverVersion() ) )
   {
      GTEST_SKIP() << "on MySQL 4031 is the idle timeout; see the next test";
   }

   db.query( "SIGNAL SQLSTATE '45000' SET MYSQL_ERRNO = 4031" );
   EXPECT_EQ( db.lastErrno(), IUS_DB2::DBERROR );
   EXPECT_EQ( db.lastServerErrno(), 4031 ) << db.lastError().toStdString();
   EXPECT_FALSE( db.lastErrorIsConnectionLoss() );
   EXPECT_TRUE( db.isConnected() );
   EXPECT_TRUE( answers( db ) ) << db.lastError().toStdString();
}

// An idle connection the server timed out is gone on either server:
// MySQL 8 says so with 4031, MariaDB with a lost connection
TEST_F( US_DB2_LiveLimits, AConnectionTheServerTimedOutIsNoLongerConnected )
{
   db.rawQuery( "SET SESSION wait_timeout = 1" );
   ASSERT_FALSE( db.lastError().startsWith( "MySQL error" ) ) << db.lastError().toStdString();
   QThread::msleep( 3000 );

   db.query( "SELECT 41 + 1" );
   EXPECT_EQ( db.lastErrno(), IUS_DB2::DBERROR );
   EXPECT_TRUE( db.lastErrorIsConnectionLoss() ) << db.lastServerErrno() << " " << db.lastError().toStdString();
   EXPECT_FALSE( db.isConnected() ) << db.lastServerErrno() << " " << db.lastError().toStdString();

   QString error;
   ASSERT_TRUE( db.connect( host, name, user, password, error ) ) << error.toStdString();
   EXPECT_TRUE( answers( db ) ) << db.lastError().toStdString();
}

// Just below the limit everything arrives: random bytes, each special byte,
// and a four-byte UTF-8 character across every 4 MiB boundary the data is
// escaped at
TEST_F( US_DB2_LiveLimits, ABlobJustBelowTheLimitArrivesByteForByte )
{
   createBlobProcedure();
   const qint64 packet = maxAllowedPacket();
   ASSERT_GT( packet, 0 );
   ASSERT_LE( packet, qint64( 256 ) * 1024 * 1024 ) << "too large a limit to test here";

   // Random bytes need escaping about one in 37; leave room for that
   const int  size = int( packet * 9 / 10 );
   QByteArray data( size, '\0' );
   QRandomGenerator random( 20261124 );
   for ( int ii = 0; ii + 4 <= size; ii += 4 )
   {
      const quint32 word = random.generate();
      memcpy( data.data() + ii, &word, 4 );
   }
   const QByteArray special( "\0\n\r\\'\"\x1a", 7 );
   data.replace( 0, special.size(), special );
   const QByteArray emoji( "\xf0\x9f\x98\x80" );
   for ( int boundary = 4 * 1024 * 1024; boundary + 2 < size; boundary += 4 * 1024 * 1024 )
   {
      data.replace( boundary - 2, emoji.size(), emoji );
   }

   const QString path = writeFile( "near.bin", data );
   EXPECT_EQ( db.writeBlobToDB( path, blobProcedure, 1 ), IUS_DB2::OK ) << db.lastError().toStdString();
   EXPECT_TRUE( answers( db ) ) << db.lastError().toStdString();
}

// A connection the server dropped answers nothing again; it used to go on
// saying it was connected
TEST_F( US_DB2_LiveLimits, AConnectionTheServerDroppedIsNoLongerConnected )
{
   db.rawQuery( "SELECT CONNECTION_ID()" );
   ASSERT_TRUE( db.next() ) << db.lastError().toStdString();
   const QString id = db.value( 0 ).toString();

   US_DB2  killer;
   QString error;
   ASSERT_TRUE( killer.connect( host, name, user, password, error ) ) << error.toStdString();
   killer.rawQuery( "KILL " + id );
   ASSERT_FALSE( killer.lastError().startsWith( "MySQL error" ) ) << killer.lastError().toStdString();

   EXPECT_TRUE( db.isConnected() ) << "nothing has told it yet";
   db.query( "SELECT 41 + 1" );
   EXPECT_FALSE( db.next() );
   EXPECT_FALSE( db.isConnected() ) << db.lastError().toStdString() << " errno " << db.lastServerErrno();

   QString transactionError;
   EXPECT_FALSE( db.beginTransaction( transactionError ) );

   // Connecting again makes a new connection
   ASSERT_TRUE( db.connect( host, name, user, password, error ) ) << error.toStdString();
   EXPECT_TRUE( db.isConnected() );
   EXPECT_TRUE( answers( db ) ) << db.lastError().toStdString();
   ASSERT_TRUE( db.beginTransaction( transactionError ) ) << transactionError.toStdString();
   EXPECT_TRUE( db.rollbackTransaction( transactionError ) ) << transactionError.toStdString();
}
#endif
}
