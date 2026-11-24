//! \file test_us_text_live.cpp
// Records whose text or IDs used to change on the way to a live server:
//   - an analyte sequence with CR LF line ends was stored with them in the
//     database while its disk copy, read back through XML, had LF;
//   - a latin1 database refused a Greek letter with nothing but the server's
//     "Incorrect string value: '\xCE\xB1...'";
//   - a buffer whose component the local component table lacked was saved
//     with an empty component ID.
//
// They need the US3 schema and an investigator of userlevel 2 or more:
//   US3_TEST_DB_HOST, US3_TEST_DB_NAME, US3_TEST_DB_USER, US3_TEST_DB_PASSWORD,
//   US3_TEST_DB_PERSON_GUID, US3_TEST_DB_PERSON_PW
// and skip without them.  They add an analyte and a buffer and remove them
// again.  Run them against a latin1 database (MariaDB's default) and a
// utf8mb4 one (MySQL 8's): which of the U3 checks applies depends on it.
//
// US_DB2 always connects over TLS, with etc/mysql/ca-cert.pem as its CA and
// no client certificate, so the server must have TLS on.

#include <gtest/gtest.h>

#include "us_analyte.h"
#include "us_buffer.h"
#include "us_db2.h"
#include "us_settings.h"

#include <QDir>
#include <QMap>
#include <QString>
#include <QStringList>

namespace
{
#ifndef NO_DB
class LiveUs3Text : public ::testing::Test
{
   protected:
      void SetUp() override
      {
         const QString host       = qEnvironmentVariable( "US3_TEST_DB_HOST" );
         const QString name       = qEnvironmentVariable( "US3_TEST_DB_NAME" );
         const QString user       = qEnvironmentVariable( "US3_TEST_DB_USER" );
         const QString password   = qEnvironmentVariable( "US3_TEST_DB_PASSWORD" );
         const QString personGuid = qEnvironmentVariable( "US3_TEST_DB_PERSON_GUID" );
         const QString personPw   = qEnvironmentVariable( "US3_TEST_DB_PERSON_PW" );
         if ( host.isEmpty() || name.isEmpty() || user.isEmpty() || personGuid.isEmpty() )
         {
            GTEST_SKIP() << "US3_TEST_DB_HOST, US3_TEST_DB_NAME, US3_TEST_DB_USER and US3_TEST_DB_PERSON_GUID not set";
         }

         QString error;
         ASSERT_TRUE( admin.connect( host, name, user, password, error ) ) << error.toStdString();
         admin.rawQuery( "SELECT COUNT(*) FROM information_schema.ROUTINES WHERE ROUTINE_SCHEMA = DATABASE() "
                         "AND ROUTINE_NAME IN ( 'new_analyte', 'get_analyteID', 'get_analyte_info', 'new_buffer', "
                         "'add_buffer_component', 'get_bufferID' )" );
         ASSERT_TRUE( admin.next() );
         if ( admin.value( 0 ).toInt() != 6 )
         {
            GTEST_SKIP() << "the US3 analyte and buffer procedures are not in " << name.toStdString();
         }
         admin.rawQuery( "SELECT personID FROM people WHERE personGUID = '" + personGuid + "'" );
         ASSERT_TRUE( admin.next() );
         savedInvestigator = US_Settings::us_inv_ID();
         US_Settings::set_us_inv_ID( admin.value( 0 ).toInt() );
         ASSERT_TRUE( person.connectAuthenticated( host, name, user, password, personGuid, personPw, error ) )
            << error.toStdString();
         removeTestRows();
         madeRows = true;
      }

      void TearDown() override
      {
         if ( madeRows )
         {
            removeTestRows();
            US_Settings::set_us_inv_ID( savedInvestigator );
         }
      }

      void removeTestRows()
      {
         admin.rawQuery( "DELETE ap FROM analytePerson ap JOIN analyte a ON a.analyteID = ap.analyteID "
                         "WHERE a.analyteGUID = '" + kAnalyteGuid + "'" );
         admin.rawQuery( "DELETE FROM analyte WHERE analyteGUID = '" + kAnalyteGuid + "'" );
         admin.rawQuery( "DELETE bl FROM bufferLink bl JOIN buffer b ON b.bufferID = bl.bufferID "
                         "WHERE b.bufferGUID = '" + kBufferGuid + "'" );
         admin.rawQuery( "DELETE bp FROM bufferPerson bp JOIN buffer b ON b.bufferID = bp.bufferID "
                         "WHERE b.bufferGUID = '" + kBufferGuid + "'" );
         admin.rawQuery( "DELETE FROM buffer WHERE bufferGUID = '" + kBufferGuid + "'" );
      }

      //! The character set of a column of the US3 schema
      QString columnCharset( const QString& table, const QString& column )
      {
         admin.rawQuery( "SELECT CHARACTER_SET_NAME FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = DATABASE() "
                         "AND TABLE_NAME = '" + table + "' AND COLUMN_NAME = '" + column + "'" );
         return admin.next() ? admin.value( 0 ).toString() : QString();
      }

      US_Analyte protein( const QString& sequence, const QString& description )
      {
         US_Analyte analyte;
         analyte.analyteGUID = kAnalyteGuid;
         analyte.type        = US_Analyte::PROTEIN;
         analyte.sequence    = sequence;
         analyte.description = description;
         analyte.vbar20      = 0.72;
         analyte.mw          = 15000;
         return analyte;
      }

      const QString kAnalyteGuid      = "bbbbbbbb-0000-4000-8000-0000000000f1";
      const QString kBufferGuid       = "bbbbbbbb-0000-4000-8000-0000000000f2";
      US_DB2        admin;
      US_DB2        person;
      int           savedInvestigator = 0;
      bool          madeRows          = false;
};

// U5: the database copy and the disk copy hold the same sequence, and
// reading either gives it
TEST_F( LiveUs3Text, AnAnalyteSequenceIsTheSameInTheDatabaseAndOnDisk )
{
   const QString entered = "  >sp|P69905|HBA_HUMAN test\r\n    1 MVLSPADKTN\tVKAAWGKVGA \r\n   21 HAGEYGAEAL\rERMF  ";
   const QString stored  = "  >sp|P69905|HBA_HUMAN test\n    1 MVLSPADKTN\tVKAAWGKVGA \n   21 HAGEYGAEAL\nERMF  ";

   US_Analyte analyte = protein( entered, "us3 test analyte (sequence)" );
   ASSERT_EQ( IUS_DB2::OK, analyte.write( true, QString(), &person ) )
      << analyte.message.toStdString() << " " << person.lastError().toStdString();

   admin.rawQuery( "SELECT HEX( sequence ) FROM analyte WHERE analyteGUID = '" + kAnalyteGuid + "'" );
   ASSERT_TRUE( admin.next() );
   EXPECT_EQ( QString::fromLatin1( stored.toUtf8().toHex().toUpper() ), admin.value( 0 ).toString() );

   US_Analyte fromDb;
   ASSERT_EQ( IUS_DB2::OK, fromDb.load( true, kAnalyteGuid, &person ) ) << fromDb.message.toStdString();
   EXPECT_EQ( stored, fromDb.sequence );

   US_Analyte fromDisk;
   ASSERT_EQ( IUS_DB2::OK, fromDisk.load( false, kAnalyteGuid ) ) << fromDisk.message.toStdString();
   EXPECT_EQ( stored, fromDisk.sequence );

   // A row written before, with CR LF, reads as the disk copy does
   admin.rawQuery( "UPDATE analyte SET sequence = CONCAT( 'MKV', CHAR( 13 ), CHAR( 10 ), 'AAG' ) "
                   "WHERE analyteGUID = '" + kAnalyteGuid + "'" );
   ASSERT_TRUE( admin.lastError().isEmpty() ) << admin.lastError().toStdString();
   US_Analyte old;
   ASSERT_EQ( IUS_DB2::OK, old.load( true, kAnalyteGuid, &person ) ) << old.message.toStdString();
   EXPECT_EQ( QString( "MKV\nAAG" ), old.sequence );
}

// U3: a latin1 database refuses a Greek letter, and says so in words; a
// utf8mb4 one stores it.  textFitsServer() tells the two apart beforehand.
TEST_F( LiveUs3Text, AGreekLetterIsStoredOrRefusedWithAMessageThatNamesIt )
{
   const QString alpha       = QString::fromUtf8( "α" );
   const QString description = "us3 test analyte " + alpha + "-helix";
   QString       problem;
   const bool    fits = US_DB2::textFitsServer( &person, description, "analyte", "description", &problem );
   const bool    wide = columnCharset( "analyte", "description" ).startsWith( "utf8" );
   admin.rawQuery( "SELECT @@character_set_database" );
   ASSERT_TRUE( admin.next() );
   const bool wideParameters = admin.value( 0 ).toString().startsWith( "utf8" );
   EXPECT_EQ( wide && wideParameters, fits ) << problem.toStdString();

   US_Analyte analyte = protein( "MKV", description );
   const int  status  = analyte.write( true, QString(), &person );
   if ( fits )
   {
      ASSERT_EQ( IUS_DB2::OK, status ) << person.lastError().toStdString();
      US_Analyte back;
      ASSERT_EQ( IUS_DB2::OK, back.load( true, kAnalyteGuid, &person ) );
      EXPECT_EQ( description, back.description );
      return;
   }

   EXPECT_TRUE( problem.contains( "'" + alpha + "' (U+03B1)" ) ) << problem.toStdString();
   EXPECT_NE( IUS_DB2::OK, status );
   EXPECT_EQ( 1366, person.lastServerErrno() );
   const QString error = person.lastError();
   EXPECT_TRUE( error.contains( "Latin-1" ) ) << error.toStdString();
   EXPECT_TRUE( error.contains( "'" + alpha + "' (U+03B1)" ) ) << error.toStdString();
   EXPECT_TRUE( error.contains( "\"description\"" ) ) << error.toStdString();
   EXPECT_TRUE( error.contains( "Incorrect string value" ) ) << "the server's own message is kept";

   admin.rawQuery( "SELECT COUNT(*) FROM analyte WHERE analyteGUID = '" + kAnalyteGuid + "'" );
   ASSERT_TRUE( admin.next() );
   EXPECT_EQ( 0, admin.value( 0 ).toInt() );
}

// U2: a buffer file names a component the local component table does not
// have.  The dialogs used to give it an empty component from the table,
// whose empty ID was then written; the ID in componentIDs is what counts.
TEST_F( LiveUs3Text, ABufferComponentTheLocalTableLacksIsSavedWithItsId )
{
   admin.rawQuery( "SELECT MAX( bufferComponentID ) FROM bufferComponent" );
   ASSERT_TRUE( admin.next() );
   const QString componentID = admin.value( 0 ).toString();
   ASSERT_FALSE( componentID.isEmpty() );

   US_Buffer written;
   written.GUID        = kBufferGuid;
   written.description = "us3 test buffer (component id)";
   written.density     = 1.0;
   written.viscosity   = 1.0;
   written.pH          = 7.0;
   written.componentIDs << componentID;
   written.concentration << 0.5;
   const QString dir = US_Settings::dataDir() + "/buffers";
   ASSERT_TRUE( QDir().mkpath( dir ) );
   const QString file = dir + "/component-id-test.xml";
   ASSERT_TRUE( written.writeToDisk( file ) );

   // The dialog as it was: QMap::operator[] of a table that lacks it
   US_Buffer                         buffer;
   QMap<QString, US_BufferComponent> table;
   ASSERT_TRUE( buffer.readFromDisk( file ) );
   QFile::remove( file );
   buffer.component.clear();
   for ( const QString& id : buffer.componentIDs )
      buffer.component << table[id];

   QString   error;
   const int id = buffer.saveToDB( &person, "0", &error );
   ASSERT_GT( id, 0 ) << error.toStdString();
   admin.rawQuery( "SELECT bufferComponentID, concentration FROM bufferLink WHERE bufferID = " + QString::number( id ) );
   ASSERT_TRUE( admin.next() );
   EXPECT_EQ( componentID, admin.value( 0 ).toString() );
   EXPECT_DOUBLE_EQ( 0.5, admin.value( 1 ).toDouble() );
   EXPECT_FALSE( admin.next() );

   // The dialog as it is: the component keeps its ID and the table is left
   // as it was
   // from the disk copy the save made
   bool          newFile = true;
   const QString copy    = US_Buffer::get_filename( dir, kBufferGuid, newFile );
   ASSERT_FALSE( newFile );
   US_Buffer again;
   ASSERT_TRUE( again.readFromDisk( copy ) );
   QFile::remove( copy );
   QMap<QString, US_BufferComponent> none;
   again.attachComponents( none );
   EXPECT_TRUE( none.isEmpty() );
   ASSERT_EQ( 1, again.component.size() );
   EXPECT_EQ( componentID, again.component[0].componentID );
   EXPECT_EQ( id, again.saveToDB( &person, "0", &error ) ) << error.toStdString();
   admin.rawQuery( "SELECT bufferComponentID FROM bufferLink WHERE bufferID = " + QString::number( id ) );
   ASSERT_TRUE( admin.next() );
   EXPECT_EQ( componentID, admin.value( 0 ).toString() );
}
#endif
} // namespace
