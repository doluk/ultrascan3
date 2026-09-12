// test/programs/us_data_publication/test_us_datapub_records.cpp
#include <gtest/gtest.h>

#include <QtCore>

#include "us_datapub_records.h"
#include "us_datapub_defs.h"
#include "us_datapub_hash.h"

class DataPubRecords : public ::testing::Test
{
   protected:
      QString dir;

      void SetUp() override
      {
         dir = QDir::tempPath() + "/us_datapub_records_"
               + QString::number( QCoreApplication::applicationPid() );
         QDir().mkpath( dir );
      }

      void TearDown() override
      {
         QDir( dir ).removeRecursively();
      }
};

TEST_F( DataPubRecords, CalibrationRoundTrip )
{
   US_Rotor::RotorCalibration written;
   written.ID          = 21;
   written.GUID        = "33333333-3333-3333-3333-333333333333";
   written.rotorID     = 2;
   written.rotorGUID   = "44444444-4444-4444-4444-444444444444";
   written.coeff1      = 1.25e-5;
   written.coeff2      = 3.5e-10;
   written.label       = "Demo calibration";
   written.report      = "Stretch measured on a demo rotor";
   written.lastUpdated = QDate( 2024, 1, 1 );
   written.omega2t     = 1.5e12;

   QString path = dir + "/C0000001.xml";
   ASSERT_TRUE( US_DataPubRecords::writeCalibration( written, path ) );

   US_Rotor::RotorCalibration read;
   ASSERT_TRUE( US_DataPubRecords::readCalibration( path, read ) );

   EXPECT_EQ( read.ID,        written.ID );
   EXPECT_EQ( read.GUID,      written.GUID );
   EXPECT_EQ( read.rotorID,   written.rotorID );
   EXPECT_EQ( read.rotorGUID, written.rotorGUID );
   EXPECT_EQ( read.label,     written.label );
   EXPECT_EQ( read.report,    written.report );
   EXPECT_EQ( read.lastUpdated, written.lastUpdated );
   EXPECT_NEAR( read.coeff1, written.coeff1, 1.0e-12 );
   EXPECT_NEAR( read.coeff2, written.coeff2, 1.0e-16 );
}

TEST_F( DataPubRecords, CenterpieceRoundTrip )
{
   US_AbstractCenterpiece written;
   written.serial_number = 2;
   written.guid          = "55555555-5555-5555-5555-555555555555";
   written.name          = "Epon 2 channel standard";
   written.material      = "Epon";
   written.channels      = 3;
   written.shape         = "sector";
   written.maxRPM        = 60000.0;
   written.angle         = 2.5;
   written.width         = 0.0;
   written.path_length     << 1.2 << 1.2 << 1.2;
   written.bottom_position << 7.2 << 7.2 << 7.2;

   QString path = dir + "/centerpiece_2.xml";
   ASSERT_TRUE( US_DataPubRecords::writeCenterpiece( written, path ) );

   US_AbstractCenterpiece read;
   ASSERT_TRUE( US_DataPubRecords::readCenterpiece( path, read ) );

   EXPECT_EQ( read.serial_number, written.serial_number );
   EXPECT_EQ( read.guid,          written.guid );
   EXPECT_EQ( read.name,          written.name );
   EXPECT_EQ( read.material,      written.material );
   EXPECT_EQ( read.channels,      written.channels );
   EXPECT_EQ( read.path_length.size(),     3 );
   EXPECT_EQ( read.bottom_position.size(), 3 );
   EXPECT_NEAR( read.maxRPM, written.maxRPM, 1.0e-6 );
}

TEST_F( DataPubRecords, RenameAnAttributeBackedName )
{
   QString path = dir + "/M0000001.xml";
   QFile   file( path );
   ASSERT_TRUE( file.open( QIODevice::WriteOnly | QIODevice::Text ) );
   file.write( "<ModelData version=\"1.0\">\n"
               "  <model description=\"old name\""
               " modelGUID=\"66666666-6666-6666-6666-666666666666\""
               " editGUID=\"77777777-7777-7777-7777-777777777777\"/>\n"
               "</ModelData>\n" );
   file.close();

   EXPECT_EQ( US_DataPubRecords::recordName( path, US_DataPub::Model ),
              "old name" );

   ASSERT_TRUE( US_DataPubRecords::setRecordName( path, US_DataPub::Model,
                                                  "new name" ) );

   EXPECT_EQ( US_DataPubRecords::recordName( path, US_DataPub::Model ),
              "new name" );

   // The GUID is what ties the record to its dependants, so renaming must
   // leave it alone.
   QFile check( path );
   ASSERT_TRUE( check.open( QIODevice::ReadOnly ) );
   QString content = QString::fromUtf8( check.readAll() );
   check.close();

   EXPECT_TRUE( content.contains(
                "66666666-6666-6666-6666-666666666666" ) );
   EXPECT_TRUE( content.contains(
                "77777777-7777-7777-7777-777777777777" ) );
   EXPECT_FALSE( content.contains( "old name" ) );
}

TEST_F( DataPubRecords, RenameAnElementBackedName )
{
   QString path = dir + "/P0000001.xml";
   QFile   file( path );
   ASSERT_TRUE( file.open( QIODevice::WriteOnly | QIODevice::Text ) );
   file.write( "<ProjectData version=\"1.0\">\n"
               "  <project id=\"7\""
               " guid=\"88888888-8888-8888-8888-888888888888\">\n"
               "    <goals>keep me</goals>\n"
               "    <description>old project</description>\n"
               "  </project>\n"
               "</ProjectData>\n" );
   file.close();

   EXPECT_EQ( US_DataPubRecords::recordName( path, US_DataPub::Project ),
              "old project" );

   ASSERT_TRUE( US_DataPubRecords::setRecordName( path, US_DataPub::Project,
                                                  "new project" ) );

   EXPECT_EQ( US_DataPubRecords::recordName( path, US_DataPub::Project ),
              "new project" );

   QFile check( path );
   ASSERT_TRUE( check.open( QIODevice::ReadOnly ) );
   QString content = QString::fromUtf8( check.readAll() );
   check.close();

   EXPECT_TRUE ( content.contains( "keep me" ) );
   EXPECT_TRUE ( content.contains(
                 "88888888-8888-8888-8888-888888888888" ) );
   EXPECT_FALSE( content.contains( "old project" ) );
}

TEST_F( DataPubRecords, RenameLeavesRecordsWithoutANameAlone )
{
   QString path = dir + "/raw.auc";
   QFile   file( path );
   ASSERT_TRUE( file.open( QIODevice::WriteOnly ) );
   file.write( QByteArray( "UCDA binary", 11 ) );
   file.close();

   EXPECT_FALSE( US_DataPubRecords::setRecordName( path, US_DataPub::RawData,
                                                   "whatever" ) );
}
