//! \file test_us_db2.cpp
// US_DB2::composeQuery(): stored-procedure invocation and credential placement,
// and how a refused procedure name fails the query and the blob transfers.  The escaping itself is in
// test_us_db2_escaping.cpp.  US_DB2::writeBlobFile(): how a downloaded blob is written to its file.
// The rest of US_DB2 needs a live server.

#include "qt_test_base.h"
#include "us_db2.h"
#include "mock_us_db2.h"

#include <QFile>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QTemporaryFile>

#ifdef Q_OS_UNIX
#include <csignal>
#include <sys/resource.h>
#endif

namespace
{
const QString kGuid = "11111111-2222-3333-4444-555555555555";
const QString kPw   = "s3cret";

QString       call( const QStringList& arguments )
{
   return US_DB2::composeQuery( "CALL", arguments, kGuid, kPw );
}
}

TEST( UsDb2Query, CredentialsArePassedAsTheFirstTwoProcedureArguments )
{
   EXPECT_EQ( call( QStringList() << "get_user_info" ), QString( "CALL get_user_info('%1', '%2')" ).arg( kGuid ).arg( kPw ) );
}

TEST( UsDb2Query, CallerArgumentsFollowTheCredentialsInOrder )
{
   EXPECT_EQ( call( QStringList() << "get_experiment" << "42" << "RI" ),
              QString( "CALL get_experiment('%1', '%2', '42', 'RI')" ).arg( kGuid ).arg( kPw ) );
}

TEST( UsDb2Query, TheKeywordSelectsBetweenAProcedureCallAndAFunctionSelect )
{
   const QStringList arguments = QStringList() << "count_rows" << "7";

   EXPECT_TRUE( US_DB2::composeQuery( "CALL", arguments, kGuid, kPw ).startsWith( "CALL count_rows(" ) );
   EXPECT_TRUE( US_DB2::composeQuery( "SELECT", arguments, kGuid, kPw ).startsWith( "SELECT count_rows(" ) );
}

TEST( UsDb2Query, ArgumentsDifferingOnlyByKeywordAreOtherwiseIdentical )
{
   const QStringList arguments = QStringList() << "count_rows" << "7";
   const QString     asCall    = US_DB2::composeQuery( "CALL", arguments, kGuid, kPw );
   const QString     asSelect  = US_DB2::composeQuery( "SELECT", arguments, kGuid, kPw );

   EXPECT_EQ( asCall.mid( QString( "CALL" ).size() ), asSelect.mid( QString( "SELECT" ).size() ) );
}

TEST( UsDb2Query, EveryQuoteInAnArgumentIsEscapedNotJustTheFirst )
{
   EXPECT_EQ( call( QStringList() << "find_buffer" << "a'b'c" ),
              QString( "CALL find_buffer('%1', '%2', 'a\\'b\\'c')" ).arg( kGuid ).arg( kPw ) );
}

TEST( UsDb2Query, EachArgumentIsEscapedIndependently )
{
   EXPECT_EQ( call( QStringList() << "p" << "x'1" << "y'2" ),
              QString( "CALL p('%1', '%2', 'x\\'1', 'y\\'2')" ).arg( kGuid ).arg( kPw ) );
}

TEST( UsDb2Query, AProcedureNameThatIsNotAPlainIdentifierIsRefused )
{
   // The name is interpolated as a SQL identifier rather than a quoted
   // literal, so escaping cannot make it safe and it is rejected instead.
   EXPECT_TRUE( call( QStringList() << "od'd" ).isEmpty() );
   EXPECT_TRUE( call( QStringList() << "p; DROP TABLE users" ).isEmpty() );
   EXPECT_TRUE( call( QStringList() << "" ).isEmpty() );
   EXPECT_TRUE( call( QStringList() << "9starts_with_a_digit" ).isEmpty() );
}

TEST( UsDb2Query, OrdinaryProcedureNamesAreStillAccepted )
{
   // The guard must not reject the names the codebase actually uses.
   EXPECT_FALSE( call( QStringList() << "get_analyte_info" ).isEmpty() );
   EXPECT_FALSE( call( QStringList() << "_leading_underscore" ).isEmpty() );
   EXPECT_FALSE( call( QStringList() << "Mixed_Case9" ).isEmpty() );
}

TEST( UsDb2Query, AnEmptyArgumentListYieldsAnEmptyQuery )
{
   // The procedure name is arguments[ 0 ]; an empty list has no procedure to
   // call.
   EXPECT_TRUE( US_DB2::composeQuery( "CALL", QStringList(), kGuid, kPw ).isEmpty() );
}

TEST( UsDb2Query, ARefusedProcedureNameFailsTheQueryWithItsOwnError )
{
   // Nothing is sent, so no connection is needed to see the refusal.
   US_DB2        db;
   const QString name = "p; DROP TABLE users";

   EXPECT_EQ( db.statusQuery( QStringList() << name << "1" ), IUS_DB2::DBERROR );
   EXPECT_EQ( db.lastError(), QObject::tr( US_DB2::invalidProcedureMessage ).arg( name ) );

   EXPECT_EQ( db.functionQuery( QStringList() << "od'd" ), IUS_DB2::DBERROR );
   EXPECT_EQ( db.lastError(), QObject::tr( US_DB2::invalidProcedureMessage ).arg( "od'd" ) );

   db.query( QStringList() << "" );
   EXPECT_EQ( db.lastErrno(), IUS_DB2::DBERROR );
   EXPECT_FALSE( db.next() );
}

// Arguments refused before anything is sent never reach a server, so there
// is no server error to report.
TEST( UsDb2Query, ARefusedProcedureNameHasNoServerError )
{
   US_DB2 db;

   EXPECT_EQ( db.lastServerErrno(), 0 );
   EXPECT_EQ( db.statusQuery( QStringList() << "p; DROP" ), IUS_DB2::DBERROR );
   EXPECT_EQ( db.lastServerErrno(), 0 );
}

#ifndef NO_DB
// A statement the client cannot send at all -- here because the connection
// was never made -- is reported with the client's own error number, and
// with the reason in lastError(), by both kinds of query.  (That the US3
// status read after a refused statement does not replace the reason needs
// a server that refuses one statement and answers the next; the database
// suite checks it with a procedure the server does not have.)
TEST( UsDb2Query, AStatementThatCannotBeSentReportsTheServerError )
{
   US_DB2 db;

   db.query( QString( "CALL no_such_procedure()" ) );
   EXPECT_EQ( db.lastErrno(), IUS_DB2::DBERROR );
   EXPECT_NE( db.lastServerErrno(), 0 );
   EXPECT_TRUE( db.lastError().startsWith( "MySQL error:" ) )
      << db.lastError().toStdString();

   EXPECT_EQ( db.statusQuery( QString( "CALL no_such_procedure()" ) ),
              IUS_DB2::DBERROR );
   EXPECT_NE( db.lastServerErrno(), 0 );
   EXPECT_TRUE( db.lastError().startsWith( "MySQL error:" ) )
      << db.lastError().toStdString();
}
#endif

// NO_DB compiles blob transfers as no-op stubs; this validates the DB adapter.
#ifndef NO_DB
TEST( UsDb2Query, TheBlobTransfersRefuseAnInvalidProcedureName )
{
   // A readable, non-empty file, so only the procedure name can be refused.
   QTemporaryFile file;
   ASSERT_TRUE( file.open() );
   file.write( "blob" );
   file.close();

   US_DB2        db;
   const QString name = "p; DROP TABLE users";

   EXPECT_EQ( db.writeBlobToDB( file.fileName(), name, 1 ), IUS_DB2::DBERROR );
   EXPECT_EQ( db.lastError(), QObject::tr( US_DB2::invalidProcedureMessage ).arg( name ) );

   EXPECT_EQ( db.readBlobFromDB( file.fileName(), "od'd", 1 ), IUS_DB2::DBERROR );
   EXPECT_EQ( db.lastError(), QObject::tr( US_DB2::invalidProcedureMessage ).arg( "od'd" ) );
}

#endif

// Text with half a surrogate pair has no UTF-8 form; Qt would send '?' in
// its place.  Every query refuses it, before anything is sent -- so an
// adapter that is not even connected says so, and not that it could not
// reach the server.
TEST( UsDb2Text, ALoneSurrogateIsNamed )
{
   EXPECT_TRUE( US_DB2::loneSurrogateProblem( QString() ).isEmpty() );
   EXPECT_TRUE( US_DB2::loneSurrogateProblem( QString::fromUtf8( "café \U0001F600 中" ) ).isEmpty() );

   const QString high = QString( "a" ) + QChar( 0xD83D ) + "b";
   const QString low  = QString( "a" ) + QChar( 0xDE00 );
   const QString cut  = QString::fromUtf8( "\U0001F600" ).left( 1 );
   const QString swap = QString( QChar( 0xDE00 ) ) + QChar( 0xD83D );
   for ( const QString& text : { high, low, cut, swap } )
   {
      const QString problem = US_DB2::loneSurrogateProblem( text );
      EXPECT_TRUE( problem.contains( "surrogate" ) ) << problem.toStdString();
      EXPECT_TRUE( problem.contains( "nothing was sent" ) ) << problem.toStdString();
   }
   EXPECT_TRUE( US_DB2::loneSurrogateProblem( high ).contains( "U+D83D" ) );
   EXPECT_TRUE( US_DB2::loneSurrogateProblem( low ).contains( "U+DE00" ) );
}

#ifndef NO_DB
TEST( UsDb2Text, EveryQueryRefusesALoneSurrogateBeforeSending )
{
   US_DB2        db;
   const QString lone = QString( "x" ) + QChar( 0xDC00 );

   db.query( QString( "SELECT '" ) + lone + "'" );
   EXPECT_EQ( db.lastErrno(), IUS_DB2::DBERROR );
   EXPECT_EQ( db.lastServerErrno(), 0 );
   EXPECT_TRUE( db.lastError().contains( "surrogate" ) ) << db.lastError().toStdString();

   db.query( QStringList() << "get_person_info" << lone );
   EXPECT_TRUE( db.lastError().contains( "surrogate" ) ) << db.lastError().toStdString();

   EXPECT_EQ( db.statusQuery( QStringList() << "new_person" << lone ), IUS_DB2::DBERROR );
   EXPECT_TRUE( db.lastError().contains( "surrogate" ) ) << db.lastError().toStdString();

   EXPECT_EQ( db.functionQuery( QStringList() << "count_people" << lone ), IUS_DB2::DBERROR );
   EXPECT_TRUE( db.lastError().contains( "surrogate" ) ) << db.lastError().toStdString();

   db.rawQuery( QString( "SELECT '" ) + lone + "'" );
   EXPECT_TRUE( db.lastError().contains( "surrogate" ) ) << db.lastError().toStdString();
   EXPECT_FALSE( db.next() );
}
#endif

// The errors after which a connection is gone, and some after which it is not
TEST( UsDb2Connection, TheErrorsOfALostConnectionAreKnown )
{
   const QString mysql   = "8.0.46-0ubuntu0.24.04.5";
   const QString mariadb = "10.11.14-MariaDB-0ubuntu0.24.04.1";
   for ( const QString& version : { mysql, mariadb, QString() } )
   {
      for ( int code : { 2006, 2013, 2026, 2027, 2055, 5013, 5014, 1053, 1153, 1927 } )
         EXPECT_TRUE( US_DB2::isConnectionLoss( code, version ) ) << code << " " << version.toStdString();
      for ( int code : { 0, 1062, 1205, 1213, 1305, 1366, 1451, 2000, 2014 } )
         EXPECT_FALSE( US_DB2::isConnectionLoss( code, version ) ) << code << " " << version.toStdString();
   }
   EXPECT_TRUE( US_DB2::isConnectionLoss( 2013 ) );
   EXPECT_FALSE( US_DB2::isConnectionLoss( 2014 ) );
}

// 4031 is MySQL's idle timeout, sent as it closes the connection; MariaDB
// uses the number for a statement error (ER_REFERENCED_TRG_DOES_NOT_EXIST)
// after which the connection is fine
TEST( UsDb2Connection, Error4031IsALossOnlyOnMySQL )
{
   EXPECT_TRUE ( US_DB2::isConnectionLoss( 4031, "8.0.46-0ubuntu0.24.04.5" ) );
   EXPECT_TRUE ( US_DB2::isConnectionLoss( 4031, "8.4.3" ) );
   EXPECT_FALSE( US_DB2::isConnectionLoss( 4031, "10.11.14-MariaDB-0ubuntu0.24.04.1" ) );
   EXPECT_FALSE( US_DB2::isConnectionLoss( 4031, "5.5.5-10.4.6-MariaDB-log" ) );
   EXPECT_FALSE( US_DB2::isConnectionLoss( 4031, QString() ) ) << "an unknown server";
   EXPECT_FALSE( US_DB2::isConnectionLoss( 4031 ) ) << "an unknown server";

   EXPECT_TRUE ( US_DB2::serverIsMariaDB( "10.11.14-MariaDB-0ubuntu0.24.04.1" ) );
   EXPECT_FALSE( US_DB2::serverIsMariaDB( "8.0.46" ) );
   EXPECT_FALSE( US_DB2::serverIsMySQL( "" ) );

   // The commit that failed with it: unknown on MySQL, the server's error on MariaDB
   EXPECT_EQ( US_DB2::commitErrorText( 4031, "boom", "8.0.46" ),
              QObject::tr( US_DB2::commitUnknownMessage ).arg( "boom" ) );
   EXPECT_EQ( US_DB2::commitErrorText( 4031, "boom", "10.11.14-MariaDB" ), QString( "boom" ) );
}

// The interface asks the connection: its last error, on its server
TEST( UsDb2Connection, TheInterfaceReadsTheLastErrorForItsServer )
{
   ::testing::NiceMock< US_DB2_Mock > db;
   ON_CALL( db, lastServerErrno() ).WillByDefault( ::testing::Return( 4031 ) );
   ON_CALL( db, serverVersion() ).WillByDefault( ::testing::Return( QString( "10.11.14-MariaDB" ) ) );
   EXPECT_FALSE( db.lastErrorIsConnectionLoss() );
   ON_CALL( db, serverVersion() ).WillByDefault( ::testing::Return( QString( "8.0.46" ) ) );
   EXPECT_TRUE( db.lastErrorIsConnectionLoss() );
   ON_CALL( db, lastServerErrno() ).WillByDefault( ::testing::Return( 2014 ) );
   EXPECT_FALSE( db.lastErrorIsConnectionLoss() );
}

// A COMMIT that fails with the connection may have taken effect: every
// error of a lost connection says the outcome is unknown, 2055 (MySQL's
// client library) and 2026 (TLS) as well as 2006 and 2013
TEST( UsDb2Connection, ACommitLostWithTheConnectionHasAnUnknownOutcome )
{
   const QString unknown = QObject::tr( US_DB2::commitUnknownMessage ).arg( "boom" );
   for ( int code : { 2006, 2013, 2055, 2026 } )
      EXPECT_EQ( US_DB2::commitErrorText( code, "boom" ), unknown ) << code;
   for ( int code : { 1213, 1205, 1180 } )
      EXPECT_EQ( US_DB2::commitErrorText( code, "boom" ), QString( "boom" ) ) << code;
}

TEST( UsDb2Query, AnEmptyArgumentIsStillPassedAsAnEmptyLiteral )
{
   EXPECT_EQ( call( QStringList() << "p" << "" << "z" ), QString( "CALL p('%1', '%2', '', 'z')" ).arg( kGuid ).arg( kPw ) );
}

TEST( UsDb2Query, DisconnectedAdapterDoesNotOwnATransaction )
{
   US_DB2 db;
   EXPECT_FALSE( db.transactionActive() );
   QString error;
   EXPECT_FALSE( db.beginTransaction( error ) );
   EXPECT_FALSE( db.transactionActive() );
}

// writeBlobFile():  how readBlobFromDB() writes a download (ultrascan-tickets#1251).
TEST( UsDb2BlobFile, WritesEveryByte )
{
   QTemporaryDir dir;
   ASSERT_TRUE( dir.isValid() );
   const QString    path = dir.filePath( "run.auc" );
   const QByteArray data( 1 << 20, 'x' );
   QString          error;
   EXPECT_EQ( US_DB2::writeBlobFile( path, data, error ), IUS_DB2::OK );
   EXPECT_TRUE( error.isEmpty() );
   QFile file( path );
   ASSERT_TRUE( file.open( QIODevice::ReadOnly ) );
   EXPECT_EQ( file.readAll(), data );
}

TEST( UsDb2BlobFile, AFileThatCannotBeOpenedIsAnError )
{
   QTemporaryDir dir;
   ASSERT_TRUE( dir.isValid() );
   const QString path = dir.filePath( "missing-folder/run.auc" );
   QString       error;
   EXPECT_EQ( US_DB2::writeBlobFile( path, QByteArray( "data" ), error ), IUS_DB2::DBERROR );
   EXPECT_TRUE( error.contains( path ) );
   EXPECT_FALSE( QFile::exists( path ) );
}

#ifdef Q_OS_UNIX
// A short write, as on a full disk:  the process's file size limit stops the
// write partway.  The error is reported and the partial file removed.
TEST( UsDb2BlobFile, AShortWriteIsAnErrorAndLeavesNoPartialFile )
{
   QTemporaryDir dir;
   ASSERT_TRUE( dir.isValid() );
   const QString path = dir.filePath( "run.auc" );
   QString       error;
   int           status = IUS_DB2::OK;

   struct rlimit saved;
   ASSERT_EQ( getrlimit( RLIMIT_FSIZE, &saved ), 0 );
   void ( *savedHandler )( int ) = signal( SIGXFSZ, SIG_IGN ); // EFBIG instead of the signal
   struct rlimit limited         = saved;
   limited.rlim_cur              = 4096;
   ASSERT_EQ( setrlimit( RLIMIT_FSIZE, &limited ), 0 );
   status = US_DB2::writeBlobFile( path, QByteArray( 1 << 16, 'x' ), error );
   setrlimit( RLIMIT_FSIZE, &saved );
   signal( SIGXFSZ, savedHandler );

   EXPECT_EQ( status, IUS_DB2::DBERROR );
   EXPECT_TRUE( error.contains( "65536" ) ) << error.toStdString();
   EXPECT_FALSE( QFile::exists( path ) );
}
#endif
