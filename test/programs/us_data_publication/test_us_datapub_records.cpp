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

// Coefficients and geometry come back as the very same doubles
TEST_F( DataPubRecords, NumbersKeepTheirFullPrecision )
{
   US_Rotor::RotorCalibration cal;
   cal.GUID    = "33333333-3333-3333-3333-333333333333";
   cal.coeff1  = 1.2345678901234567e-5;
   cal.coeff2  = 3.1415926535897931e-10;
   cal.omega2t = 1.2345678901234e12;

   QString path = dir + "/C0000002.xml";
   ASSERT_TRUE( US_DataPubRecords::writeCalibration( cal, path ) );

   US_Rotor::RotorCalibration read;
   ASSERT_TRUE( US_DataPubRecords::readCalibration( path, read ) );
   EXPECT_EQ( read.coeff1,  cal.coeff1 );
   EXPECT_EQ( read.coeff2,  cal.coeff2 );
   EXPECT_EQ( read.omega2t, cal.omega2t );

   US_AbstractCenterpiece cp;
   cp.serial_number = 4;
   cp.maxRPM        = 59999.123456789;
   cp.angle         = 2.4999999999;
   cp.width         = 0.123456789012;
   cp.path_length     << 1.2000000001;
   cp.bottom_position << 7.1999999999;

   path = dir + "/centerpiece_4.xml";
   ASSERT_TRUE( US_DataPubRecords::writeCenterpiece( cp, path ) );

   US_AbstractCenterpiece cread;
   ASSERT_TRUE( US_DataPubRecords::readCenterpiece( path, cread ) );
   EXPECT_EQ( cread.maxRPM, cp.maxRPM );
   EXPECT_EQ( cread.angle,  cp.angle );
   EXPECT_EQ( cread.width,  cp.width );
   ASSERT_EQ( cread.path_length.size(), 1 );
   EXPECT_EQ( cread.path_length[ 0 ],     cp.path_length[ 0 ] );
   EXPECT_EQ( cread.bottom_position[ 0 ], cp.bottom_position[ 0 ] );
}

TEST_F( DataPubRecords, AWriteThatCannotHappenFails )
{
   US_Rotor::RotorCalibration cal;
   US_AbstractCenterpiece     cp;

   EXPECT_FALSE( US_DataPubRecords::writeCalibration( cal,
                 dir + "/no/such/dir/C.xml" ) );
   EXPECT_FALSE( US_DataPubRecords::writeCenterpiece( cp,
                 dir + "/no/such/dir/cp.xml" ) );
}

// A calibration or a centerpiece without a GUID gets one from what it is:
// the same record gives the same GUID, two records that only share their
// local numbers do not
TEST_F( DataPubRecords, DerivedGuidsFollowTheContent )
{
   US_Rotor::RotorCalibration cal;
   cal.ID          = 3;
   cal.rotorGUID   = "44444444-4444-4444-4444-444444444444";
   cal.coeff1      = 1.25e-5;
   cal.coeff2      = 3.5e-10;
   cal.lastUpdated = QDate( 2024, 1, 1 );

   QString one = US_DataPubRecords::calibrationGuid( cal );
   EXPECT_EQ( one, US_DataPubRecords::calibrationGuid( cal ) );
   EXPECT_FALSE( QUuid( one ).isNull() );

   US_Rotor::RotorCalibration other = cal;
   other.coeff1 = 1.2500000000001e-5;
   EXPECT_NE( one, US_DataPubRecords::calibrationGuid( other ) );

   other = cal;
   other.lastUpdated = QDate( 2025, 1, 1 );
   EXPECT_NE( one, US_DataPubRecords::calibrationGuid( other ) );

   other = cal;
   other.calibrationExperimentGUID = "66666666-6666-6666-6666-666666666666";
   EXPECT_NE( one, US_DataPubRecords::calibrationGuid( other ) );

   // A record's own GUID always wins
   other.GUID = "77777777-7777-7777-7777-777777777777";
   EXPECT_EQ( US_DataPubRecords::calibrationGuid( other ), other.GUID );

   US_AbstractCenterpiece cp;
   cp.serial_number = 2;
   cp.name          = "Epon";
   cp.path_length     << 1.2;
   cp.bottom_position << 7.2;

   QString cpOne = US_DataPubRecords::centerpieceGuid( cp );
   EXPECT_EQ( cpOne, US_DataPubRecords::centerpieceGuid( cp ) );

   US_AbstractCenterpiece cpOther = cp;
   cpOther.path_length[ 0 ] = 0.3;
   EXPECT_NE( cpOne, US_DataPubRecords::centerpieceGuid( cpOther ) );

   cpOther = cp;
   cpOther.channels = cp.channels + 1;
   EXPECT_NE( cpOne, US_DataPubRecords::centerpieceGuid( cpOther ) );
}

TEST_F( DataPubRecords, OnlyAReadableRiProfileIsUsable )
{
   QByteArray good( "<?xml version=\"1.0\"?>\n<!DOCTYPE US_RIProfile>\n"
                    "<US_RIProfile version=\"1.0\">"
                    "<RI value=\"0.5\"/><RI value=\"0.6\"/>"
                    "</US_RIProfile>\n" );
   EXPECT_TRUE( US_DataPubRecords::riProfileIsUsable( good ) );

   QString why;
   EXPECT_FALSE( US_DataPubRecords::riProfileIsUsable( QByteArray(), &why ) );
   EXPECT_FALSE( why.isEmpty() );
   EXPECT_FALSE( US_DataPubRecords::riProfileIsUsable( "  \n" ) );
   EXPECT_FALSE( US_DataPubRecords::riProfileIsUsable(
                 "<US_RIProfile><RI value=\"0.5\"/>" ) );      // cut short
   EXPECT_FALSE( US_DataPubRecords::riProfileIsUsable(
                 "<US_RIProfile version=\"1.0\"/>" ) );        // no values
   EXPECT_FALSE( US_DataPubRecords::riProfileIsUsable(
                 "<Other><RI value=\"0.5\"/></Other>" ) );     // not a profile
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
