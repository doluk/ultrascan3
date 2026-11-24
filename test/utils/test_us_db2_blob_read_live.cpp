//! \file test_us_db2_blob_read_live.cpp
// US_DB2::readBlobFromDB against every shape of result a download procedure
// can give, on a live server:
//   - no status set (the procedure returned before it);
//   - the status set alone: a missing record, a refused one;
//   - the status and the data;
//   - an OK status and then no data set, an empty data set, NULL data, a
//     wrong checksum, or a failure part way.
// It read the status row and the data row without checking either, so a
// missing record crashed the program (a NULL result set).  Each case here
// must give its status and a reason, write no file on failure, and leave the
// connection working.
//
// The real US3 procedures (download_aucData, download_editData) and the
// caller US_ConvertIO::downloadRawDataBlob are checked for a missing ID and
// for a record of another investigator, where the US3 schema is there.
//
// They need a MariaDB or MySQL server and skip without one.  Set
//   US3_TEST_DB_HOST      host or host:port
//   US3_TEST_DB_NAME      a scratch database; the test creates a procedure
//   US3_TEST_DB_USER      needs CREATE ROUTINE on that database
//   US3_TEST_DB_PASSWORD
// and, for the US3 procedures, an investigator who is not an administrator:
//   US3_TEST_DB_PERSON_GUID, US3_TEST_DB_PERSON_PW, US3_TEST_DB_PERSON_ID

#include <gtest/gtest.h>

#include "us_convertio.h"
#include "us_db2.h"

#include <QFile>
#include <QString>
#include <QTemporaryDir>

namespace
{
#ifndef NO_DB
const char* const shapeProcedure = "us3_test_blob_shape";

class US_DB2_LiveBlobRead : public ::testing::Test
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

         // The US3 schema's status functions, made only where they are missing
         madeStatusFunctions = scalar( "SELECT COUNT(*) FROM information_schema.ROUTINES WHERE "
                                       "ROUTINE_SCHEMA = DATABASE() AND ROUTINE_NAME = 'last_errno'" ) == "0";
         if ( madeStatusFunctions )
         {
            db.rawQuery( "CREATE FUNCTION last_errno() RETURNS INT NO SQL RETURN @US3_LAST_ERRNO" );
            db.rawQuery( "CREATE FUNCTION last_error() RETURNS TEXT NO SQL RETURN @US3_LAST_ERROR" );
         }

         // One procedure, every shape: the ID picks it
         db.rawQuery( QString( "DROP PROCEDURE IF EXISTS %1" ).arg( shapeProcedure ) );
         db.rawQuery( QString(
            "CREATE PROCEDURE %1( g TEXT, p TEXT, id INT ) "
            "BEGIN "
            "  SET @US3_LAST_ERRNO = 0; SET @US3_LAST_ERROR = ''; "
            "  CASE id "
            "    WHEN 1 THEN SET @US3_LAST_ERRNO = 301; SET @US3_LAST_ERROR = 'shape: no status set'; "
            "    WHEN 2 THEN SET @US3_LAST_ERRNO = 301; SET @US3_LAST_ERROR = 'shape: status only'; "
            "                SELECT 301 AS status; "
            "    WHEN 3 THEN SELECT 0 AS status; SELECT 'hello\\0blob', MD5( 'hello\\0blob' ); "
            "    WHEN 4 THEN SELECT 0 AS status; SELECT 'x', MD5( 'x' ) FROM DUAL WHERE 1 = 0; "
            "    WHEN 5 THEN SELECT 0 AS status; SELECT NULL, MD5( NULL ); "
            "    WHEN 6 THEN SELECT 0 AS status; SELECT * FROM us3_test_no_such_table; "
            "    WHEN 7 THEN SELECT NULL AS status; "
            "    WHEN 8 THEN SELECT 0 AS status; "
            "    WHEN 9 THEN SELECT 0 AS status; SELECT 'hello', 'not the checksum'; "
            "    WHEN 10 THEN SET @US3_LAST_ERRNO = 0; "
            "    WHEN 11 THEN SELECT 0 AS status; SELECT '', MD5( '' ); "
            "    ELSE SELECT 999 AS status; "
            "  END CASE; "
            "END" ).arg( shapeProcedure ) );
         ASSERT_EQ( scalar( QString( "SELECT COUNT(*) FROM information_schema.ROUTINES WHERE "
                                     "ROUTINE_SCHEMA = DATABASE() AND ROUTINE_NAME = '%1'" ).arg( shapeProcedure ) ),
                    "1" ) << "the test user needs CREATE ROUTINE: " << db.lastError().toStdString();
      }

      void TearDown() override
      {
         if ( db.isConnected() )
         {
            db.rawQuery( QString( "DROP PROCEDURE IF EXISTS %1" ).arg( shapeProcedure ) );
            if ( madeStatusFunctions )
            {
               db.rawQuery( "DROP FUNCTION IF EXISTS last_errno" );
               db.rawQuery( "DROP FUNCTION IF EXISTS last_error" );
            }
         }
      }

      QString scalar( const QString& sql )
      {
         db.rawQuery( sql );
         return db.next() ? db.value( 0 ).toString() : QString();
      }

      //! The connection still answers
      static bool answers( US_DB2& conn )
      {
         conn.rawQuery( "SELECT 41 + 1" );
         return conn.next() && conn.value( 0 ).toInt() == 42;
      }

      US_DB2        db;
      QTemporaryDir scratch;
      bool          madeStatusFunctions = false;

      QString host     = qEnvironmentVariable( "US3_TEST_DB_HOST" );
      QString name     = qEnvironmentVariable( "US3_TEST_DB_NAME" );
      QString user     = qEnvironmentVariable( "US3_TEST_DB_USER" );
      QString password = qEnvironmentVariable( "US3_TEST_DB_PASSWORD" );
};

TEST_F( US_DB2_LiveBlobRead, EveryShapeOfResultGivesItsStatusAndLeavesTheConnectionWorking )
{
   struct Case
   {
      int     id;
      int     status;
      QString inError;   // part of lastError(); empty: no error
   };
   const QList< Case > cases = {
      { 1, IUS_DB2::NOROWS, "shape: no status set" },
      { 2, IUS_DB2::NOROWS, "shape: status only" },
      { 3, IUS_DB2::OK, "" },
      { 4, IUS_DB2::NOROWS, "no data row" },
      { 5, IUS_DB2::DBERROR, "NULL" },
      { 6, IUS_DB2::DBERROR, "us3_test_no_such_table" },
      { 7, IUS_DB2::DBERROR, "empty status" },
      { 8, IUS_DB2::DBERROR, "no data" },
      { 9, IUS_DB2::BAD_CHECKSUM, "checksum" },
      { 10, IUS_DB2::DBERROR, "no status" },
      { 11, IUS_DB2::OK, "" },
      { 12, 999, "status 999" },
   };

   for ( const Case& cc : cases )
   {
      SCOPED_TRACE( "shape " + std::to_string( cc.id ) );
      const QString path = scratch.filePath( QString( "shape%1.bin" ).arg( cc.id ) );

      EXPECT_EQ( db.readBlobFromDB( path, shapeProcedure, cc.id ), cc.status ) << db.lastError().toStdString();
      EXPECT_EQ( db.lastErrno(), cc.status );
      if ( cc.inError.isEmpty() )
      {
         EXPECT_TRUE( db.lastError().isEmpty() ) << db.lastError().toStdString();
      }
      else
      {
         EXPECT_TRUE( db.lastError().contains( cc.inError ) ) << db.lastError().toStdString();
      }
      EXPECT_EQ( QFile::exists( path ), cc.status == IUS_DB2::OK ) << "a file was written for a failed read";
      EXPECT_TRUE( db.isConnected() );
      EXPECT_TRUE( answers( db ) ) << db.lastError().toStdString();
   }

   // The data arrived whole, the NUL in it included
   QFile data( scratch.filePath( "shape3.bin" ) );
   ASSERT_TRUE( data.open( QIODevice::ReadOnly ) );
   EXPECT_EQ( data.readAll(), QByteArray( "hello\0blob", 10 ) );
   EXPECT_EQ( QFile( scratch.filePath( "shape11.bin" ) ).size(), 0 );

   // A procedure the server does not have is the server's error, and its number
   EXPECT_EQ( db.readBlobFromDB( scratch.filePath( "none.bin" ), "us3_test_no_such_procedure", 1 ), IUS_DB2::DBERROR );
   EXPECT_EQ( db.lastServerErrno(), int( IUS_DB2::serverNoSuchRoutine ) ) << db.lastError().toStdString();
   EXPECT_TRUE( answers( db ) );
}

// The guid and password a blob read sends are text like any query's: a
// lone surrogate in them is refused before anything is sent, not sent as '?'
TEST_F( US_DB2_LiveBlobRead, ALoneSurrogateInThePasswordIsRefusedBeforeItIsSent )
{
   const QString lone = QString( "pw" ) + QChar( 0xDC00 );

   // The login is refused for the same reason, and leaves guid and password set
   US_DB2  person;
   QString error;
   EXPECT_FALSE( person.connectAuthenticated( host, name, user, password, "aaaaaaaa-0000-4000-8000-00000000ffff",
                                              lone, error ) );
   EXPECT_TRUE( error.contains( "surrogate" ) ) << error.toStdString();
   ASSERT_TRUE( person.isConnected() );

   const QString path = scratch.filePath( "lone.bin" );
   EXPECT_EQ( person.readBlobFromDB( path, "us3_test_no_such_procedure", 3 ), IUS_DB2::DBERROR );
   EXPECT_TRUE( person.lastError().contains( "surrogate" ) ) << person.lastError().toStdString();
   EXPECT_EQ( person.lastServerErrno(), 0 ) << "the server was asked";
   EXPECT_FALSE( QFile::exists( path ) );
   EXPECT_TRUE( answers( person ) );
}

// The real download procedures, as an investigator: a missing record and
// one of another investigator give their status and a reason, and the
// export's caller reports it instead of crashing
class US_DB2_LiveBlobReadUs3 : public US_DB2_LiveBlobRead
{
   protected:
      void SetUp() override
      {
         US_DB2_LiveBlobRead::SetUp();
         if ( IsSkipped() || HasFatalFailure() )
         {
            return;
         }
         if ( personGuid.isEmpty() || personID.isEmpty() )
         {
            GTEST_SKIP() << "US3_TEST_DB_PERSON_GUID, US3_TEST_DB_PERSON_PW and US3_TEST_DB_PERSON_ID not set";
         }
         if ( scalar( "SELECT COUNT(*) FROM information_schema.ROUTINES WHERE ROUTINE_SCHEMA = DATABASE() "
                      "AND ROUTINE_NAME IN ( 'download_aucData', 'download_editData', 'validate_user' )" ) != "3" )
         {
            GTEST_SKIP() << "the US3 procedures are not in " << name.toStdString();
         }
         if ( scalar( "SELECT userlevel FROM people WHERE personID = " + personID ).toInt() >= 3 )
         {
            GTEST_SKIP() << "US3_TEST_DB_PERSON_ID is an administrator; nothing is refused to it";
         }

         QString error;
         ASSERT_TRUE( person.connectAuthenticated( host, name, user, password, personGuid, personPw, error ) )
            << error.toStdString();

         missingRaw  = scalar( "SELECT COALESCE( MAX( rawDataID ), 0 ) + 1000 FROM rawData" ).toInt();
         missingEdit = scalar( "SELECT COALESCE( MAX( editedDataID ), 0 ) + 1000 FROM editedData" ).toInt();
         foreignRaw  = scalar( "SELECT MIN( r.rawDataID ) FROM rawData r WHERE NOT EXISTS ( SELECT 1 FROM "
                               "experimentPerson ep WHERE ep.experimentID = r.experimentID AND ep.personID = " +
                               personID + " )" ).toInt();
         ownRaw      = scalar( "SELECT MIN( r.rawDataID ) FROM rawData r JOIN experimentPerson ep "
                               "ON ep.experimentID = r.experimentID WHERE ep.personID = " + personID ).toInt();
      }

      US_DB2  person;
      QString personGuid = qEnvironmentVariable( "US3_TEST_DB_PERSON_GUID" );
      QString personPw   = qEnvironmentVariable( "US3_TEST_DB_PERSON_PW" );
      QString personID   = qEnvironmentVariable( "US3_TEST_DB_PERSON_ID" );
      int     missingRaw  = 0;
      int     missingEdit = 0;
      int     foreignRaw  = 0;
      int     ownRaw      = 0;
};

TEST_F( US_DB2_LiveBlobReadUs3, AMissingRecordIsNoRowsNotACrash )
{
   const QString path = scratch.filePath( "missing.auc" );

   EXPECT_EQ( person.readBlobFromDB( path, "download_aucData", missingRaw ), IUS_DB2::NOROWS );
   EXPECT_FALSE( person.lastError().isEmpty() );
   EXPECT_FALSE( QFile::exists( path ) );
   EXPECT_TRUE( answers( person ) );

   const int editStatus = person.readBlobFromDB( path, "download_editData", missingEdit );
   EXPECT_NE( editStatus, IUS_DB2::OK );
   EXPECT_NE( editStatus, IUS_DB2::DBERROR ) << "the procedure's own status: " << person.lastError().toStdString();
   EXPECT_FALSE( person.lastError().isEmpty() );
   EXPECT_FALSE( QFile::exists( path ) );
   EXPECT_TRUE( answers( person ) );

   // What the export runs through
   QString   error;
   const int status = US_ConvertIO::downloadRawDataBlob( &person, missingRaw, path, error );
   EXPECT_EQ( status, IUS_DB2::NOROWS );
   EXPECT_TRUE( error.contains( QString::number( missingRaw ) ) ) << error.toStdString();
   EXPECT_FALSE( QFile::exists( path ) );
   EXPECT_TRUE( answers( person ) );
}

TEST_F( US_DB2_LiveBlobReadUs3, ARecordOfAnotherInvestigatorIsRefusedNotACrash )
{
   if ( foreignRaw < 1 )
   {
      GTEST_SKIP() << "no raw data of another investigator in " << name.toStdString();
   }

   const QString path = scratch.filePath( "foreign.auc" );
   const int     status = person.readBlobFromDB( path, "download_aucData", foreignRaw );
   EXPECT_NE( status, IUS_DB2::OK );
   EXPECT_NE( status, IUS_DB2::DBERROR ) << "the procedure's own status: " << person.lastError().toStdString();
   EXPECT_FALSE( person.lastError().isEmpty() );
   EXPECT_FALSE( QFile::exists( path ) );
   EXPECT_TRUE( answers( person ) );

   QString error;
   EXPECT_EQ( US_ConvertIO::downloadRawDataBlob( &person, foreignRaw, path, error ), status );
   EXPECT_FALSE( error.isEmpty() );
   EXPECT_FALSE( QFile::exists( path ) );

   // An own record still arrives
   if ( ownRaw > 0 )
   {
      EXPECT_EQ( person.readBlobFromDB( path, "download_aucData", ownRaw ), IUS_DB2::OK )
         << person.lastError().toStdString();
      EXPECT_TRUE( QFile::exists( path ) );
   }
}
#endif
} // namespace
