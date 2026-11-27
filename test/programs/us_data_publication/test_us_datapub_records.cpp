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
   other.coeff1 = 1.26e-5;
   EXPECT_NE( one, US_DataPubRecords::calibrationGuid( other ) );

   other = cal;
   other.coeff2 = 3.6e-10;
   EXPECT_NE( one, US_DataPubRecords::calibrationGuid( other ) );

   other = cal;
   other.ID = 4;
   EXPECT_NE( one, US_DataPubRecords::calibrationGuid( other ) );

   other = cal;
   other.rotorGUID = "55555555-5555-5555-5555-555555555555";
   EXPECT_NE( one, US_DataPubRecords::calibrationGuid( other ) );

   // The date is when the row or the file was last written, not part of
   // what was measured
   other = cal;
   other.lastUpdated = QDate( 2025, 1, 1 );
   EXPECT_EQ( one, US_DataPubRecords::calibrationGuid( other ) );

   // A record's own GUID always wins ...
   other.GUID = "77777777-7777-7777-7777-777777777777";
   EXPECT_EQ( US_DataPubRecords::calibrationGuid( other ), other.GUID );

   // ... when it is one: a legacy value that is not stands for none
   other = cal;
   other.GUID = "legacy-7";
   EXPECT_EQ( US_DataPubRecords::calibrationGuid( other ), one );
}

// The same calibration has the same identity whether it is read from the
// database or from its local copy: the database reads the coefficients as
// floats and returns no calibration experiment GUID, the local file keeps
// six digits of each coefficient
TEST_F( DataPubRecords, ACalibrationGuidIsTheSameFromTheDatabaseAndFromDisk )
{
   US_Rotor::RotorCalibration fromDb;
   fromDb.ID          = 3;
   fromDb.rotorGUID   = "44444444-4444-4444-4444-444444444444";
   fromDb.coeff1      = QString( "1.23456789e-05" ).toFloat();
   fromDb.coeff2      = QString( "3.45678912e-10" ).toFloat();
   fromDb.lastUpdated = QDate( 2024, 1, 1 );

   US_Rotor::RotorCalibration fromDisk = fromDb;
   fromDisk.coeff1 = QString::number( fromDb.coeff1 ).toFloat();
   fromDisk.coeff2 = QString::number( fromDb.coeff2 ).toFloat();
   fromDisk.calibrationExperimentGUID = "66666666-6666-6666-6666-666666666666";

   // the database's date is when the row was last written, the file's
   // when it was copied
   fromDisk.lastUpdated = QDate( 2025, 6, 30 );

   ASSERT_NE( fromDb.coeff1, fromDisk.coeff1 );
   EXPECT_EQ( US_DataPubRecords::calibrationGuid( fromDb ),
              US_DataPubRecords::calibrationGuid( fromDisk ) );
}

// A derived identity must not change between builds or installations: a
// known input gives a known UUID (version 5, RFC 4122 variant)
TEST_F( DataPubRecords, DerivedGuidsAreKnownValues )
{
   QString guid = US_DataPub::derivedGuid( "timeState:"
                                           "11111111-1111-1111-1111-111111111111" );
   QUuid   uuid( guid );

   EXPECT_EQ( uuid.version(), QUuid::Sha1 );
   EXPECT_EQ( uuid.variant(), QUuid::DCE );
   // Worked out with Python's uuid.uuid5() from the namespace UUID
   EXPECT_EQ( guid, QString( "0bf44af6-71ae-54e2-a5e8-ce490b1c52ee" ) );

   US_Rotor::RotorCalibration cal;
   cal.ID          = 3;
   cal.rotorGUID   = "44444444-4444-4444-4444-444444444444";
   cal.coeff1      = 1.25e-5;
   cal.coeff2      = 3.5e-10;
   cal.lastUpdated = QDate( 2024, 1, 1 );

   // uuid5( ns, "rotorCalibration:<rotorGUID>:3:1.25000e-05:3.50000e-10" ),
   // with no date in it
   EXPECT_EQ( US_DataPubRecords::calibrationGuid( cal ),
              QString( "116899ce-6150-5f54-bdf6-795320a03ae9" ) );
}

TEST_F( DataPubRecords, CenterpieceGuidsFollowTheContent )
{
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

   // A GUID column holding something else is no identity
   cpOther = cp;
   cpOther.guid = "{not-a-guid}";
   EXPECT_EQ( US_DataPubRecords::centerpieceGuid( cpOther ), cpOne );
}

// Spectra go into the bundle as the doubles they are: no rounding of the
// value, and no merging of wavelengths closer than 0.05 nm
TEST_F( DataPubRecords, SpectraRoundTripAtFullPrecision )
{
   US_DataPubRecords::Spectra written;
   written[ "extinction" ].insert( 280.0,  1.2345678901234567 );
   written[ "extinction" ].insert( 280.02, 1.0e-7 / 3.0 );
   written[ "refraction" ].insert( 589.3,  1.3329987654321 );
   written[ "fluorescence" ];                   // empty: left out

   QString path = dir + "/spectra.xml";
   ASSERT_TRUE( US_DataPubRecords::writeSpectra( written, path ) );

   US_DataPubRecords::Spectra read;
   QString                    why;
   ASSERT_TRUE( US_DataPubRecords::readSpectra( path, read, &why ) )
      << why.toStdString();

   EXPECT_FALSE( read.contains( "fluorescence" ) );
   ASSERT_EQ( read.value( "extinction" ).size(), 2 );
   EXPECT_EQ( read[ "extinction" ].value( 280.0  ), 1.2345678901234567 );
   EXPECT_EQ( read[ "extinction" ].value( 280.02 ), 1.0e-7 / 3.0 );
   EXPECT_EQ( read[ "refraction" ].value( 589.3  ), 1.3329987654321 );

   // What the library writer would have made of it
   EXPECT_NE( QString::number( 1.0e-7 / 3.0, 'e', 4 ).toDouble(),
              1.0e-7 / 3.0 );

   // Only the known spectra are written
   US_DataPubRecords::Spectra odd;
   odd[ "absorbance" ].insert( 280.0, 1.0 );
   EXPECT_FALSE( US_DataPubRecords::writeSpectra( odd, dir + "/odd.xml" ) );
}

TEST_F( DataPubRecords, BrokenSpectraAreRefused )
{
   auto refused = [ this ]( const QByteArray& xml )
   {
      QFile file( dir + "/bad.xml" );
      file.open( QIODevice::WriteOnly | QIODevice::Truncate );
      file.write( xml );
      file.close();

      US_DataPubRecords::Spectra spectra;
      QString                    why;
      bool ok = US_DataPubRecords::readSpectra( dir + "/bad.xml", spectra,
                                                &why );
      return ! ok  &&  ! why.isEmpty()  &&  spectra.isEmpty();
   };

   EXPECT_TRUE( refused( "<buffer/>" ) );
   EXPECT_TRUE( refused( "<US_DataPubSpectra><spectrum type=\"absorbance\"/>"
                         "</US_DataPubSpectra>" ) );
   EXPECT_TRUE( refused( "<US_DataPubSpectra><spectrum type=\"extinction\">"
                         "<point wavelength=\"280\" value=\"x\"/></spectrum>"
                         "</US_DataPubSpectra>" ) );
   EXPECT_TRUE( refused( "<US_DataPubSpectra><spectrum type=\"extinction\">"
                         "<point wavelength=\"280\" value=\"inf\"/></spectrum>"
                         "</US_DataPubSpectra>" ) );
   EXPECT_TRUE( refused( "<US_DataPubSpectra><spectrum type=\"extinction\">"
                         "<point wavelength=\"280\" value=\"1\"/>"
                         "<point wavelength=\"280\" value=\"2\"/></spectrum>"
                         "</US_DataPubSpectra>" ) );
   EXPECT_TRUE( refused( "<US_DataPubSpectra><spectrum type=\"extinction\"/>"
                         "<spectrum type=\"extinction\"/></US_DataPubSpectra>" ) );
   EXPECT_TRUE( refused( "<US_DataPubSpectra><spectrum type=\"extinction\">" ) );
}

// The concentrations travel with the component definitions, in buffer
// order, at full precision; 5 uM is not 0.00001 mM
TEST_F( DataPubRecords, BufferConcentrationsRoundTripAtFullPrecision )
{
   US_BufferComponent salt;
   salt.componentID = "12";
   salt.name        = "sodium chloride";
   US_BufferComponent tris;
   tris.componentID = "7";
   tris.name        = "Tris";

   QList< US_BufferComponent > comps;
   comps << salt << tris;
   QList< double > concs;
   concs << 0.005 << 1.0 / 3.0;

   QString path = dir + "/components.xml";
   ASSERT_TRUE( US_DataPubRecords::writeBufferComponents( comps, concs, path ) );

   QStringList     ids;
   QList< double > read;
   QString         why;
   ASSERT_TRUE( US_DataPubRecords::readBufferConcentrations( path, ids, read,
                                                             &why ) )
      << why.toStdString();
   EXPECT_EQ( ids, QStringList() << "12" << "7" );
   ASSERT_EQ( read.size(), 2 );
   EXPECT_EQ( read[ 0 ], 0.005 );
   EXPECT_EQ( read[ 1 ], 1.0 / 3.0 );
   EXPECT_NE( QString::number( 1.0 / 3.0, 'f', 5 ).toDouble(), 1.0 / 3.0 );

   // The definitions are still read as before
   QMap< QString, US_BufferComponent > defs;
   ASSERT_TRUE( US_DataPubRecords::readBufferComponents( path, defs ) );
   EXPECT_EQ( defs.value( "12" ).name, QString( "sodium chloride" ) );

   // A list that does not match the components is refused
   EXPECT_FALSE( US_DataPubRecords::writeBufferComponents( comps,
                 QList< double >() << 1.0, dir + "/short.xml" ) );

   // A file of an older bundle carries none: no concentrations, no error
   ASSERT_TRUE( US_DataPubRecords::writeBufferComponents( comps,
                dir + "/old.xml" ) );
   EXPECT_FALSE( US_DataPubRecords::readBufferConcentrations( dir + "/old.xml",
                 ids, read, &why ) );
   EXPECT_TRUE( ids.isEmpty() );
   EXPECT_TRUE( why.isEmpty() ) << why.toStdString();
}

// A date is written as "yyyy-MM-dd" in ASCII digits.  (Qt 5 formats a
// date with the system locale, which a test cannot change once the process
// runs, so this pins the format rather than reproducing a foreign locale.)
TEST_F( DataPubRecords, DatesAreWrittenWithAsciiDigits )
{
   EXPECT_EQ( US_DataPubRecords::isoDate( QDate( 2024, 3, 5 ) ),
              QString( "2024-03-05" ) );
   EXPECT_TRUE( US_DataPubRecords::isoDate( QDate() ).isEmpty() );

   US_Rotor::RotorCalibration written;
   written.ID          = 1;
   written.GUID        = "11111111-2222-3333-4444-555555555555";
   written.lastUpdated = QDate( 2024, 3, 5 );
   ASSERT_TRUE( US_DataPubRecords::writeCalibration( written,
                dir + "/C.xml" ) );

   QFile file( dir + "/C.xml" );
   ASSERT_TRUE( file.open( QIODevice::ReadOnly ) );
   EXPECT_TRUE( file.readAll().contains( "lastUpdated=\"2024-03-05\"" ) );
   file.close();

   US_Rotor::RotorCalibration read;
   ASSERT_TRUE( US_DataPubRecords::readCalibration( dir + "/C.xml", read ) );
   EXPECT_EQ( read.lastUpdated, QDate( 2024, 3, 5 ) );
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
