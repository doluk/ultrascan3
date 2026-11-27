//! \file test_us_db2_text.cpp
// What a latin1 column can hold, and what a user is told when it cannot.
//
// A US3 database on a server whose default character set is latin1
// (MariaDB's) stores most of its text as latin1.  Under a strict sql_mode
// the server then refuses a Greek letter (error 1366) and the whole
// statement fails.  US_DB2 turns that error into a message that names the
// character and the field, and its helpers let a program check text first.
// The live cases are in test_us_db2_roundtrip.cpp.

#include "mock_us_db2.h"
#include "qt_test_base.h"
#include "us_db2.h"

#include <QString>

using ::testing::_;
using ::testing::NiceMock;
using ::testing::Return;

namespace
{
const QString kAlpha   = QString::fromUtf8( "α" );
const QString kEmoji   = QString::fromUtf8( "\U0001F600" );
const QString kEuro    = QString::fromUtf8( "€" );

TEST( US_DB2_Latin1, AsciiAndLatin1Fit )
{
   EXPECT_TRUE( US_DB2::fitsLatin1( QString() ) );
   EXPECT_TRUE( US_DB2::fitsLatin1( "plain ASCII\t\r\n" ) );
   EXPECT_TRUE( US_DB2::fitsLatin1( QString::fromUtf8( "café 5 µM ÿ  " ) ) );
}

// The server's latin1 is Windows-1252: it holds the euro sign and curly
// quotes, and five C1 controls that 1252 leaves undefined, but no other C1
// control (checked against MariaDB 10.11 and MySQL 8.0)
TEST( US_DB2_Latin1, TheServersLatin1IsWindows1252 )
{
   EXPECT_TRUE( US_DB2::fitsLatin1( kEuro ) );
   EXPECT_TRUE( US_DB2::fitsLatin1( QString::fromUtf8( "‘’“”–—…™Ÿ" ) ) );
   for ( ushort code : { 0x81, 0x8D, 0x8F, 0x90, 0x9D } )
      EXPECT_TRUE( US_DB2::fitsLatin1( QString( QChar( code ) ) ) ) << code;
   for ( ushort code : { 0x80, 0x85, 0x9F } )
      EXPECT_FALSE( US_DB2::fitsLatin1( QString( QChar( code ) ) ) ) << code;
}

TEST( US_DB2_Latin1, GreekCjkAndEmojiDoNotFit )
{
   EXPECT_FALSE( US_DB2::fitsLatin1( kAlpha + "-helix" ) );
   EXPECT_FALSE( US_DB2::fitsLatin1( QString::fromUtf8( "中" ) ) );
   EXPECT_FALSE( US_DB2::fitsLatin1( kEmoji ) );
}

TEST( US_DB2_Latin1, NonLatin1CharactersListsEachOnceInOrder )
{
   const QString text = QString::fromUtf8( "β café αβ € " ) + kEmoji + kAlpha;
   EXPECT_EQ( QString::fromUtf8( "βα" ) + kEmoji, US_DB2::nonLatin1Characters( text ) );
   EXPECT_TRUE( US_DB2::nonLatin1Characters( "fine" ).isEmpty() );
}

TEST( US_DB2_Latin1, TextFitsCharsetByName )
{
   EXPECT_TRUE( US_DB2::textFitsCharset( kAlpha + kEmoji, "utf8mb4" ) );
   EXPECT_TRUE( US_DB2::textFitsCharset( kAlpha, "utf8mb3" ) );
   EXPECT_FALSE( US_DB2::textFitsCharset( kEmoji, "utf8mb3" ) );
   EXPECT_FALSE( US_DB2::textFitsCharset( kEmoji, "utf8" ) );
   EXPECT_FALSE( US_DB2::textFitsCharset( kAlpha, "latin1" ) );
   EXPECT_TRUE( US_DB2::textFitsCharset( kEuro, "LATIN1" ) );
   EXPECT_FALSE( US_DB2::textFitsCharset( QString::fromUtf8( "é" ), "ascii" ) );
}

TEST( US_DB2_Latin1, Latin1ProblemNamesTheFieldAndTheCharacters )
{
   EXPECT_TRUE( US_DB2::latin1Problem( "description", "fine" ).isEmpty() );

   const QString problem = US_DB2::latin1Problem( "description", kAlpha + "-helix " + kEmoji );
   EXPECT_TRUE( problem.contains( "Latin-1" ) ) << problem.toStdString();
   EXPECT_TRUE( problem.contains( "\"description\"" ) ) << problem.toStdString();
   EXPECT_TRUE( problem.contains( "'" + kAlpha + "' (U+03B1)" ) ) << problem.toStdString();
   EXPECT_TRUE( problem.contains( "'" + kEmoji + "' (U+1F600)" ) ) << problem.toStdString();
}

// The messages as MariaDB 10.11 and MySQL 8.0 give them for a procedure
// parameter and a column
TEST( US_DB2_Latin1, Error1366NamesTheCharacterAndTheField )
{
   const QString maria = US_DB2::charsetErrorText(
      1366, "Incorrect string value: '\\xCE\\xB1-hel...' for column ``.``.`p_description` at row 0" );
   EXPECT_TRUE( maria.contains( "Latin-1" ) ) << maria.toStdString();
   EXPECT_TRUE( maria.contains( "'" + kAlpha + "' (U+03B1)" ) ) << maria.toStdString();
   EXPECT_TRUE( maria.contains( "\"description\"" ) ) << maria.toStdString();

   const QString mysql = US_DB2::charsetErrorText(
      1366, "Incorrect string value: '\\xF0\\x9F\\x98\\x80' for column 'v' at row 1" );
   EXPECT_TRUE( mysql.contains( "'" + kEmoji + "' (U+1F600)" ) ) << mysql.toStdString();
   EXPECT_TRUE( mysql.contains( "\"v\"" ) ) << mysql.toStdString();

   const QString table = US_DB2::charsetErrorText(
      1366, "Incorrect string value: '\\xE4\\xB8\\xAD te...' for column `us3`.`analyte`.`description` at row 1" );
   EXPECT_TRUE( table.contains( QString::fromUtf8( "'中' (U+4E2D)" ) ) ) << table.toStdString();
   EXPECT_TRUE( table.contains( "\"description\"" ) ) << table.toStdString();

   // Bytes cut before a whole character: still a message, without it
   const QString cut = US_DB2::charsetErrorText(
      1366, "Incorrect string value: '\\xE4\\xB8' for column 'v' at row 1" );
   EXPECT_TRUE( cut.contains( "Latin-1" ) ) << cut.toStdString();
}

// US3 databases stay Latin-1: the messages ask for other text, and never for
// a conversion of the database
TEST( US_DB2_Latin1, MessagesDoNotAdviseConvertingTheDatabase )
{
   const QStringList messages = {
      US_DB2::latin1Problem( "description", kAlpha ),
      US_DB2::charsetErrorText( 1366, "Incorrect string value: '\\xCE\\xB1' for column 'v' at row 1" ),
      US_DB2::charsetErrorText( 1267, "Illegal mix of collations" ),
      US_DB2::charsetErrorText( 1300, "Invalid latin1 character string: 'CEB1'" ) };

   for ( const QString& message : messages )
   {
      EXPECT_FALSE( message.isEmpty() );
      EXPECT_FALSE( message.contains( "utf8mb4", Qt::CaseInsensitive ) ) << message.toStdString();
      EXPECT_FALSE( message.contains( "convert the database", Qt::CaseInsensitive ) ) << message.toStdString();
      EXPECT_FALSE( message.contains( "administrator", Qt::CaseInsensitive ) ) << message.toStdString();
      EXPECT_TRUE( message.contains( "cannot" ) ) << message.toStdString();
   }

   // 1300 quotes the bytes, and the message names the character
   EXPECT_TRUE( messages[ 3 ].contains( "'" + kAlpha + "' (U+03B1)" ) ) << messages[ 3 ].toStdString();
}

TEST( US_DB2_Latin1, OtherErrorsAreLeftAlone )
{
   // 1366 is given for a number too
   EXPECT_TRUE( US_DB2::charsetErrorText( 1366, "Incorrect integer value: 'abc' for column 'n' at row 1" )
                   .isEmpty() );
   EXPECT_TRUE( US_DB2::charsetErrorText( 1062, "Duplicate entry '1' for key 'PRIMARY'" ).isEmpty() );
   EXPECT_TRUE( US_DB2::charsetErrorText( 0, QString() ).isEmpty() );

   for ( int code : { 1267, 1271, 1300 } )
      EXPECT_TRUE( US_DB2::charsetErrorText( code, "Illegal mix of collations" ).contains( "Latin-1" ) ) << code;
}

// textFitsServer reads the database's and the column's character sets
TEST( US_DB2_Latin1, TextFitsServerChecksTheDatabaseAndTheColumn )
{
   NiceMock< US_DB2_Mock > db;
   ON_CALL( db, isConnected() ).WillByDefault( Return( true ) );
   EXPECT_CALL( db, rawQuery( _ ) ).Times( 3 );
   EXPECT_CALL( db, next() ).WillRepeatedly( Return( true ) );
   EXPECT_CALL( db, value( 0u ) )
      .WillOnce( Return( QVariant( "utf8mb4" ) ) )
      .WillOnce( Return( QVariant( "latin1" ) ) )
      .WillOnce( Return( QVariant( "utf8mb4" ) ) );
   EXPECT_CALL( db, value( 1u ) )
      .WillOnce( Return( QVariant( "utf8mb4" ) ) )
      .WillOnce( Return( QVariant( "utf8mb4" ) ) )
      .WillOnce( Return( QVariant( "latin1" ) ) );

   QString problem = "stale";
   EXPECT_TRUE( US_DB2::textFitsServer( &db, kAlpha, "analyte", "description", &problem ) );
   EXPECT_TRUE( problem.isEmpty() );

   // A procedure parameter in the database's latin1 refuses it too
   EXPECT_FALSE( US_DB2::textFitsServer( &db, kAlpha, "analyte", "description", &problem ) );
   EXPECT_TRUE( problem.contains( "'" + kAlpha + "'" ) ) << problem.toStdString();

   EXPECT_FALSE( US_DB2::textFitsServer( &db, kAlpha, "analyte", "description", &problem ) );
}

// Nothing known: latin1 is assumed, and nothing is sent for a name that is
// not an identifier
TEST( US_DB2_Latin1, TextFitsServerAssumesLatin1WhenItCannotTell )
{
   NiceMock< US_DB2_Mock > db;
   ON_CALL( db, isConnected() ).WillByDefault( Return( true ) );
   EXPECT_CALL( db, rawQuery( _ ) ).Times( 1 );
   EXPECT_CALL( db, next() ).WillOnce( Return( false ) );

   EXPECT_FALSE( US_DB2::textFitsServer( &db, kAlpha, "analyte", "description" ) );
   EXPECT_FALSE( US_DB2::textFitsServer( &db, kAlpha, "analyte'; DROP", "description" ) );
   EXPECT_TRUE( US_DB2::textFitsServer( nullptr, "plain", "analyte", "description" ) );
   EXPECT_FALSE( US_DB2::textFitsServer( nullptr, kAlpha, "analyte", "description" ) );
}
} // namespace
