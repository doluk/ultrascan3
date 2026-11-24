//! \file test_us_db2_roundtrip.cpp
// Values composed by US_DB2::composeQuery() reach the server unchanged.
//
// Each case sends a value, plus a guid and password that need escaping too,
// to a function that hands its three arguments straight back.  If the server
// returns what was sent, the escaping matches how it reads a literal.
//
// The UTF-8 cases send text beyond ASCII -- accented letters, the micro
// sign, Greek, CJK and an emoji -- through procedures and back: the
// test's own, whose parameters and column are utf8mb4, and the real US3
// login, project and blob paths, where a latin1 column (MariaDB's default)
// may hold only Latin-1.  Those need the US3 schema and skip without it.
//
// They need a MariaDB or MySQL server and skip without one.  Set
//   US3_TEST_DB_HOST      host or host:port
//   US3_TEST_DB_NAME      a scratch database; the test creates its own function
//   US3_TEST_DB_USER      needs CREATE ROUTINE on that database
//   US3_TEST_DB_PASSWORD
//
// US_DB2 always connects over TLS, with etc/mysql/ca-cert.pem as its CA and
// no client certificate, so the server must have TLS on.  Some client
// libraries also check that the server certificate chains to that CA.
// Without that the connect fails and the test fails rather than skip.

#include <gtest/gtest.h>

#include "us_crypto.h"
#include "us_db2.h"
#include "us_settings.h"

#include <QJsonArray>
#include <QTemporaryDir>
#include <QJsonDocument>
#include <QString>
#include <QStringList>

namespace
{
#ifndef NO_DB
const char* const echoFunction = "us3_test_echo";

class US_DB2_LiveRoundTrip : public ::testing::Test
{
   protected:
      void SetUp() override
      {
         const QString host     = qEnvironmentVariable( "US3_TEST_DB_HOST" );
         const QString name     = qEnvironmentVariable( "US3_TEST_DB_NAME" );
         const QString user     = qEnvironmentVariable( "US3_TEST_DB_USER" );
         const QString password = qEnvironmentVariable( "US3_TEST_DB_PASSWORD" );
         if ( host.isEmpty() || name.isEmpty() || user.isEmpty() )
         {
            GTEST_SKIP() << "US3_TEST_DB_HOST, US3_TEST_DB_NAME and US3_TEST_DB_USER not set";
         }

         QString error;
         ASSERT_TRUE( db.connect( host, name, user, password, error ) ) << error.toStdString();

         db.rawQuery( QString( "DROP FUNCTION IF EXISTS %1" ).arg( echoFunction ) );
         // utf8mb4 throughout, so the database's default character set
         // (latin1 on MariaDB) does not decide what can be echoed
         db.rawQuery( QString( "CREATE FUNCTION %1( g TEXT CHARACTER SET utf8mb4, p TEXT CHARACTER SET utf8mb4, "
                               "v LONGTEXT CHARACTER SET utf8mb4 ) RETURNS LONGTEXT CHARACTER SET utf8mb4 "
                               "RETURN JSON_ARRAY( g, p, v )" )
                         .arg( echoFunction ) );
      }

      void TearDown() override
      {
         if ( db.isConnected() )
         {
            db.rawQuery( QString( "DROP FUNCTION IF EXISTS %1" ).arg( echoFunction ) );
         }
      }

      //! The guid, password and value as the server received them.
      QStringList echoed( const QString& guid, const QString& password, const QString& value )
      {
         db.rawQuery( US_DB2::composeQuery( "SELECT", QStringList() << echoFunction << value, guid, password ) );
         if ( !db.next() )
         {
            ADD_FAILURE() << db.lastError().toStdString();
            return QStringList();
         }

         QStringList received;
         for ( const QJsonValue& item : QJsonDocument::fromJson( db.value( 0 ).toString().toUtf8() ).array() )
         {
            received << item.toString();
         }
         return received;
      }

      US_DB2 db;

      QString host     = qEnvironmentVariable( "US3_TEST_DB_HOST" );
      QString name     = qEnvironmentVariable( "US3_TEST_DB_NAME" );
      QString user     = qEnvironmentVariable( "US3_TEST_DB_USER" );
      QString password = qEnvironmentVariable( "US3_TEST_DB_PASSWORD" );
};

// Text beyond ASCII, from Latin-1 to outside the Basic Multilingual Plane
const QStringList kUnicode = {
   QString::fromUtf8( "L\u00f6sung caf\u00e9" ),
   QString::fromUtf8( "5 \u00b5M NaCl" ),
   QString::fromUtf8( "\u0394G = -3 kcal" ),
   QString::fromUtf8( "\u4e2d\u6587\u6837\u54c1" ),
   QString::fromUtf8( "run \U0001F600 done" ),
   QString::fromUtf8( "\u00e9\u00b5\u0394\u4e2d\U0001F600'\\" ),
};

// The character set of every part of a connection, as the server sees it
QString connectionCharsets( US_DB2& conn )
{
   conn.rawQuery( "SELECT CONCAT_WS( ' ', @@character_set_client, @@character_set_connection, "
                  "@@character_set_results )" );
   return conn.next() ? conn.value( 0 ).toString() : conn.lastError();
}

const QString kUtf8mb4Everywhere = "utf8mb4 utf8mb4 utf8mb4";

TEST_F( US_DB2_LiveRoundTrip, EscapedValuesArriveAsSent )
{
   const QString     guid     = "guid'\\";
   const QString     password = "pa\\ss'word\\";

   const QStringList values = {
      "O'Brien",
      "C:\\data\\new\\run1.auc",
      "a\\'b",
      "trailing\\",
      "line1\nline2\ttab\r",
      "100% _under_score",
      QString( "nul" ) + QChar( '\0' ) + "inside",
      "<model desc='x' path=\"C:\\runs\\demo\"><v n='-0.2'/></model>",
   };

   for ( const QString& value : values )
   {
      EXPECT_EQ( echoed( guid, password, value ), QStringList() << guid << password << value ) << "value: " << value.toStdString();
   }
}

TEST_F( US_DB2_LiveRoundTrip, UnicodeArrivesAsSentInEveryArgument )
{
   EXPECT_EQ( kUtf8mb4Everywhere, connectionCharsets( db ) );

   for ( const QString& value : kUnicode )
   {
      EXPECT_EQ( echoed( value, value + "pw", value ), QStringList() << value << value + "pw" << value )
         << "value: " << value.toStdString();
   }
}

// A procedure that stores what it is given and reads it back, as the US3
// write and read procedures do: a status set, then the data
TEST_F( US_DB2_LiveRoundTrip, UnicodeIsStoredAndReadBackThroughAProcedure )
{
   db.rawQuery( "DROP PROCEDURE IF EXISTS us3_test_utf8_store" );
   db.rawQuery( "DROP TABLE IF EXISTS us3_test_utf8" );
   db.rawQuery( "CREATE TABLE us3_test_utf8 ( id INT AUTO_INCREMENT PRIMARY KEY, "
                "v TEXT CHARACTER SET utf8mb4 ) DEFAULT CHARSET = utf8mb4" );
   ASSERT_TRUE( db.lastError().isEmpty() ) << db.lastError().toStdString();
   db.rawQuery( "CREATE PROCEDURE us3_test_utf8_store( g TEXT CHARACTER SET utf8mb4, p TEXT CHARACTER SET utf8mb4, "
                "v TEXT CHARACTER SET utf8mb4 ) "
                "BEGIN "
                "  INSERT INTO us3_test_utf8 ( v ) VALUES ( v ); "
                "  SELECT 0 AS status; "
                "  SELECT id, v, HEX( v ), CHAR_LENGTH( v ) FROM us3_test_utf8 WHERE id = LAST_INSERT_ID(); "
                "END" );
   ASSERT_TRUE( db.lastError().isEmpty() ) << db.lastError().toStdString();

   for ( const QString& value : kUnicode )
   {
      SCOPED_TRACE( value.toStdString() );
      db.query( QStringList() << "us3_test_utf8_store" << value );
      ASSERT_EQ( IUS_DB2::OK, db.lastErrno() ) << db.lastError().toStdString();
      ASSERT_TRUE( db.next() );
      // What the server stored is the text, not its bytes read as Latin-1
      EXPECT_EQ( value, db.value( 1 ).toString() );
      EXPECT_EQ( QString::fromLatin1( value.toUtf8().toHex().toUpper() ), db.value( 2 ).toString() );
      EXPECT_EQ( value.toUcs4().size(), db.value( 3 ).toInt() );
   }

   db.rawQuery( "DROP PROCEDURE IF EXISTS us3_test_utf8_store" );
   db.rawQuery( "DROP TABLE IF EXISTS us3_test_utf8" );
}

/* The real US3 paths, as an investigator whose password is not ASCII: the
   login (validate_user), a project written with new_project and read with
   get_project_info, and a blob written with writeBlobToDB.

   The person is made here and removed again.  Its password hash is made by
   the server the way check_user makes it -- MD5 of a VARCHAR(80) in the
   database's character set -- so it matches whatever that character set is.
*/
class US_DB2_LiveUnicodeUs3 : public US_DB2_LiveRoundTrip
{
   protected:
      void SetUp() override
      {
         US_DB2_LiveRoundTrip::SetUp();
         if ( IsSkipped() || HasFatalFailure() )
         {
            return;
         }

         db.rawQuery( "SELECT COUNT(*) FROM information_schema.ROUTINES WHERE ROUTINE_SCHEMA = DATABASE() "
                      "AND ROUTINE_NAME IN ( 'validate_user', 'new_project', 'get_project_info', 'delete_project' )" );
         ASSERT_TRUE( db.next() );
         if ( db.value( 0 ).toInt() != 4 )
         {
            GTEST_SKIP() << "the US3 procedures are not in " << name.toStdString();
         }

         cleanUp();
         db.rawQuery( "CREATE FUNCTION us3_test_md5( p VARCHAR(80) ) RETURNS CHAR(32) RETURN MD5( p )" );
         ASSERT_TRUE( db.lastError().isEmpty() ) << db.lastError().toStdString();
         db.rawQuery( "INSERT INTO people ( personGUID, fname, lname, address, city, state, zip, phone, email, "
                      "organization, password, activated, signup, lastLogin, userlevel, account_enabled, "
                      "authenticatePAM, userNamePAM ) VALUES ( '" + kPersonGuid + "', '" + kFirstName +
                      "', 'Test', 'x', 'x', 'TX', '00000', '000', 'utf8-test@example.org', 'Lab', "
                      "us3_test_md5( '" + kPersonPassword + "' ), 1, NOW(), NOW(), 2, 1, 0, 'us3_test_utf8' )" );
         ASSERT_TRUE( db.lastError().isEmpty() ) << db.lastError().toStdString();
         personMade = true;
         db.rawQuery( "SELECT personID FROM people WHERE personGUID = '" + kPersonGuid + "'" );
         ASSERT_TRUE( db.next() );
         personID = db.value( 0 ).toInt();
      }

      void TearDown() override
      {
         if ( personMade )
         {
            cleanUp();
         }
         US_DB2_LiveRoundTrip::TearDown();
      }

      void cleanUp()
      {
         db.rawQuery( "DELETE pp FROM projectPerson pp JOIN project p ON p.projectID = pp.projectID "
                      "WHERE p.projectGUID = '" + kProjectGuid + "'" );
         db.rawQuery( "DELETE FROM project WHERE projectGUID = '" + kProjectGuid + "'" );
         db.rawQuery( "DELETE FROM people WHERE personGUID = '" + kPersonGuid + "'" );
         db.rawQuery( "DROP FUNCTION IF EXISTS us3_test_md5" );
         db.rawQuery( "DROP PROCEDURE IF EXISTS us3_test_blob_store" );
         db.rawQuery( "DROP TABLE IF EXISTS us3_test_blob" );
      }

      //! The character set of a column of the US3 schema
      QString columnCharset( const QString& table, const QString& column )
      {
         db.rawQuery( "SELECT CHARACTER_SET_NAME FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = DATABASE() "
                      "AND TABLE_NAME = '" + table + "' AND COLUMN_NAME = '" + column + "'" );
         return db.next() ? db.value( 0 ).toString() : QString();
      }

      const QString kPersonGuid     = "bbbbbbbb-0000-4000-8000-0000000000e9";
      const QString kProjectGuid    = "bbbbbbbb-0000-4000-8000-0000000000ea";
      const QString kPersonPassword = QString::fromUtf8( "pässwört µ" );
      const QString kFirstName      = QString::fromUtf8( "Zoë" );
      bool          personMade      = false;
      int           personID        = 0;
};

TEST_F( US_DB2_LiveUnicodeUs3, AnInvestigatorWithANonAsciiPasswordLogsInWritesAndReads )
{
   US_DB2  person;
   QString error;
   ASSERT_TRUE( person.connectAuthenticated( host, name, user, password, kPersonGuid, kPersonPassword, error ) )
      << error.toStdString();
   EXPECT_EQ( kUtf8mb4Everywhere, connectionCharsets( person ) );

   // What the description column can hold: everything when it is utf8mb4,
   // Latin-1 when it is latin1, and then anything else is refused rather
   // than stored as '?'
   const bool wide = columnCharset( "project", "description" ).startsWith( "utf8" );
   db.rawQuery( "SELECT @@GLOBAL.sql_mode" );
   ASSERT_TRUE( db.next() );
   const bool        strict = db.value( 0 ).toString().contains( "STRICT" );
   const QStringList fits   = wide ? kUnicode : QStringList( kUnicode.mid( 0, 2 ) );

   for ( const QString& description : fits )
   {
      SCOPED_TRACE( description.toStdString() );
      person.statusQuery( QStringList() << "new_project" << kProjectGuid << "g" << "m" << "p" << "e" << "b" << "s"
                                        << "q" << "n" << description << "submitted" << QString::number( personID ) );
      ASSERT_EQ( IUS_DB2::OK, person.lastErrno() ) << person.lastError().toStdString();
      const int projectID = person.lastInsertID();
      ASSERT_GT( projectID, 0 );

      person.query( QStringList() << "get_project_info" << QString::number( projectID ) );
      ASSERT_EQ( IUS_DB2::OK, person.lastErrno() ) << person.lastError().toStdString();
      ASSERT_TRUE( person.next() );
      EXPECT_EQ( description, person.value( 10 ).toString() );

      person.statusQuery( QStringList() << "delete_project" << QString::number( projectID ) );
      ASSERT_EQ( IUS_DB2::OK, person.lastErrno() ) << person.lastError().toStdString();
   }

   if ( !wide && strict )
   {
      person.statusQuery( QStringList() << "new_project" << kProjectGuid << "g" << "m" << "p" << "e" << "b" << "s"
                                        << "q" << "n" << kUnicode[3] << "submitted" << QString::number( personID ) );
      EXPECT_NE( IUS_DB2::OK, person.lastErrno() ) << "a latin1 column took " << kUnicode[3].toStdString();
      db.rawQuery( "SELECT COUNT(*) FROM project WHERE projectGUID = '" + kProjectGuid + "'" );
      ASSERT_TRUE( db.next() );
      EXPECT_EQ( 0, db.value( 0 ).toInt() ) << "the description was stored with '?' for what it could not hold";
   }

   // A blob, with the person's password in front of it: the query is
   // assembled from bytes, and its text part is UTF-8 like any other
   db.rawQuery( "CREATE TABLE us3_test_blob ( id INT PRIMARY KEY, p TEXT CHARACTER SET utf8mb4, data LONGBLOB )" );
   ASSERT_TRUE( db.lastError().isEmpty() ) << db.lastError().toStdString();
   db.rawQuery( "CREATE PROCEDURE us3_test_blob_store( g CHAR(36), p VARCHAR(80) CHARACTER SET utf8mb4, i INT, "
                "d LONGBLOB, c CHAR(32) ) "
                "BEGIN "
                "  REPLACE INTO us3_test_blob VALUES ( i, p, d ); "
                "  SELECT IF( MD5( d ) = c, 0, -3 ) AS status; "
                "END" );
   ASSERT_TRUE( db.lastError().isEmpty() ) << db.lastError().toStdString();

   QTemporaryDir dir;
   ASSERT_TRUE( dir.isValid() );
   QByteArray blob;
   for ( int ii = 0; ii < 4096; ii++ )
   {
      blob.append( char( ii % 256 ) );
   }
   blob.append( "\xc3\xa9 not text '\\" );
   QFile file( dir.filePath( "blob.bin" ) );
   ASSERT_TRUE( file.open( QIODevice::WriteOnly ) );
   file.write( blob );
   file.close();

   EXPECT_EQ( IUS_DB2::OK, person.writeBlobToDB( file.fileName(), "us3_test_blob_store", 7 ) )
      << person.lastError().toStdString();
   db.rawQuery( "SELECT p, HEX( data ) FROM us3_test_blob WHERE id = 7" );
   ASSERT_TRUE( db.next() );
   EXPECT_EQ( kPersonPassword, db.value( 0 ).toString() );
   EXPECT_EQ( QString::fromLatin1( blob.toHex().toUpper() ), db.value( 1 ).toString() );
}

// Every way of connecting asks for utf8mb4, the profile connection and the
// test connection of the database dialog too.  A client library whose own
// default is latin1 would otherwise read every query as Latin-1.
TEST_F( US_DB2_LiveUnicodeUs3, EveryConnectPathUsesUtf8mb4 )
{
   const QString     master   = "utf8 master password";
   const QStringList dbCipher = US_Crypto::encrypt( password, master );
   const QStringList pwCipher = US_Crypto::encrypt( kPersonPassword, master );
   const QStringList entry{ "utf8 test", user,        name,        host,       dbCipher[0],
                            dbCipher[1], "utf8-test@example.org", pwCipher[0], pwCipher[1], kPersonGuid };
   const QStringList saved = US_Settings::defaultDB();
   US_Settings::set_defaultDB( entry );
   {
      US_DB2 profile( master );
      EXPECT_EQ( IUS_DB2::OK, profile.lastErrno() ) << profile.lastError().toStdString();
      EXPECT_EQ( kUtf8mb4Everywhere, connectionCharsets( profile ) );
   }
   US_Settings::set_defaultDB( saved );

   US_DB2  tester;
   QString error;
   EXPECT_TRUE( tester.test_secure_connection( host, name, user, password, "utf8-test@example.org", kPersonPassword,
                                               error ) )
      << error.toStdString();
   EXPECT_EQ( kUtf8mb4Everywhere, connectionCharsets( tester ) );

   US_DB2 plain;
   EXPECT_TRUE( plain.connect( host, name, user, password, error ) ) << error.toStdString();
   EXPECT_EQ( kUtf8mb4Everywhere, connectionCharsets( plain ) );

   // test_db_connection() asks for it too, but connects without TLS, which
   // an account that requires TLS refuses, so it is not tried here
}

/* A server that ignores the character set the client proposes: started
   with skip-character-set-client-handshake (or with an init_connect that
   runs SET NAMES), it keeps its own, latin1 here, and the client library
   still reports the utf8mb4 it asked for.  Every query was then read as
   Latin-1, and text stored as mojibake.  US_DB2 now sets the character set
   on the session and checks it.

   The shared test servers honour the handshake, so this needs a server of
   its own, set by
     US3_TEST_DB_NOHANDSHAKE_HOST   host:port of such a server
   with a database of US3_TEST_DB_NAME that US3_TEST_DB_USER may change.
*/
TEST( US_DB2_LiveHandshake, AServerThatIgnoresTheHandshakeStillReadsUtf8 )
{
   const QString host     = qEnvironmentVariable( "US3_TEST_DB_NOHANDSHAKE_HOST" );
   const QString name     = qEnvironmentVariable( "US3_TEST_DB_NAME" );
   const QString user     = qEnvironmentVariable( "US3_TEST_DB_USER" );
   const QString password = qEnvironmentVariable( "US3_TEST_DB_PASSWORD" );
   if ( host.isEmpty() || name.isEmpty() || user.isEmpty() )
   {
      GTEST_SKIP() << "US3_TEST_DB_NOHANDSHAKE_HOST, US3_TEST_DB_NAME and US3_TEST_DB_USER not set";
   }

   US_DB2  db;
   QString error;
   ASSERT_TRUE( db.connect( host, name, user, password, error ) ) << error.toStdString();
   EXPECT_EQ( kUtf8mb4Everywhere, connectionCharsets( db ) );

   db.rawQuery( "DROP TABLE IF EXISTS us3_test_handshake" );
   db.rawQuery( "CREATE TABLE us3_test_handshake ( id INT AUTO_INCREMENT PRIMARY KEY, "
                "v TEXT CHARACTER SET utf8mb4 ) DEFAULT CHARSET = utf8mb4" );
   ASSERT_TRUE( db.lastError().isEmpty() ) << db.lastError().toStdString();

   for ( const QString& value : kUnicode )
   {
      SCOPED_TRACE( value.toStdString() );
      db.rawQuery( "DELETE FROM us3_test_handshake" );
      QString literal = value;
      literal.replace( "\\", "\\\\" ).replace( "'", "\\'" );
      db.rawQuery( "INSERT INTO us3_test_handshake ( v ) VALUES ( '" + literal + "' )" );
      ASSERT_TRUE( db.lastError().isEmpty() ) << db.lastError().toStdString();
      db.rawQuery( "SELECT v, HEX( v ) FROM us3_test_handshake" );
      ASSERT_TRUE( db.next() );
      // The bytes stored are the text's UTF-8, not its UTF-8 read as Latin-1
      EXPECT_EQ( QString::fromLatin1( value.toUtf8().toHex().toUpper() ), db.value( 1 ).toString() );
      EXPECT_EQ( value, db.value( 0 ).toString() );
   }

   db.rawQuery( "DROP TABLE IF EXISTS us3_test_handshake" );
}
#endif
} // namespace
