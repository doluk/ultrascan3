// test_us_disk_scan.cpp - the local store read on a worker thread
//
// The worker owns its catalog: a caller asks for a job, waits for busy()
// to go false, and only then reads anything.  These tests hold it to that
// contract and check that what comes back is what reading the store on the
// calling thread gives.
#include "qt_test_base.h"

#include <QtCore>

#include "us_disk_scan.h"
#include "us_data_catalog.h"
#include "us_settings.h"
#include "us_util.h"
#include "us_dataIO.h"
#include "us_model.h"
#include "us_noise.h"

class TestUSDiskScan : public QtTestBase
{
   protected:
      QString root;
      QString savedWorkDir;
      QString runOne;
      QString runTwo;
      QString editOneGUID;
      QString modelGUID;

      void SetUp() override
      {
         QtTestBase::SetUp();

         savedWorkDir = US_Settings::workBaseDir();

         root = QDir::tempPath() + "/us_disk_scan_test_"
                + QString::number( QCoreApplication::applicationPid() ) + "_"
                + QString::number( QDateTime::currentMSecsSinceEpoch() );
         QDir().mkpath( root );
         US_Settings::set_workBaseDir( root );

         QDir().mkpath( US_Settings::dataDir() + "/models" );
         QDir().mkpath( US_Settings::dataDir() + "/noises" );

         runOne      = "Scan_run_one";
         runTwo      = "Scan.run.two";
         editOneGUID = US_Util::new_guid();
         modelGUID   = US_Util::new_guid();

         buildStore();
      }

      void TearDown() override
      {
         US_Settings::set_workBaseDir( savedWorkDir );
         QDir( root ).removeRecursively();

         QtTestBase::TearDown();
      }

      // Wait the way a window does: keep the event loop turning
      void settle( US_DiskScan& scan )
      {
         QElapsedTimer timer;
         timer.start();

         while ( scan.busy()  &&  timer.elapsed() < 20000 )
         {
            QCoreApplication::processEvents();
            QThread::msleep( 1 );
         }

         ASSERT_FALSE( scan.busy() ) << "the worker never finished";
      }

      void writeAuc( const QString& path, const QString& guid )
      {
         US_DataIO::RawData data;
         data.type[ 0 ]   = 'R';
         data.type[ 1 ]   = 'A';
         US_Util::uuid_parse( guid, (unsigned char*)data.rawGUID );
         data.cell        = 1;
         data.channel     = 'A';
         data.description = "scan test";

         for ( int ii = 0; ii < 8; ii++ )  data.xvalues << 5.8 + 0.01 * ii;

         US_DataIO::Scan scan;
         scan.temperature = 20.0;
         scan.rpm         = 45000.0;
         scan.seconds     = 100.0;
         scan.omega2t     = 1.0e10;
         scan.wavelength  = 280.0;
         scan.plateau     = 0.5;
         scan.delta_r     = 0.01;
         scan.nz_stddev   = false;

         for ( int ii = 0; ii < 8; ii++ )
         { scan.rvalues << 0.1 * ii; scan.stddevs << 0.0; }

         scan.interpolated = QByteArray( 1, char( 0 ) );
         data.scanData << scan;

         ASSERT_EQ( US_DataIO::writeRawData( path, data ), US_DataIO::OK );
      }

      void writeEdit( const QString& path, const QString& editGUID,
                      const QString& rawGUID )
      {
         QString runID = path.section( "/", -1, -1 ).section( ".", 0, -7 );
         QFile   file( path );
         ASSERT_TRUE( file.open( QIODevice::WriteOnly | QIODevice::Text ) );

         QXmlStreamWriter xml( &file );
         xml.setAutoFormatting( true );
         xml.writeStartDocument();
         xml.writeDTD         ( "<!DOCTYPE UltraScanEdits>" );
         xml.writeStartElement( "experiment" );
         xml.writeAttribute   ( "type", "velocity" );
         xml.writeStartElement( "identification" );
         xml.writeStartElement( "runid" );
         xml.writeAttribute   ( "value", runID );
         xml.writeEndElement();
         xml.writeStartElement( "editGUID" );
         xml.writeAttribute   ( "value", editGUID );
         xml.writeEndElement();
         xml.writeStartElement( "rawDataGUID" );
         xml.writeAttribute   ( "value", rawGUID );
         xml.writeEndElement();
         xml.writeEndElement();
         xml.writeStartElement( "run" );
         xml.writeAttribute   ( "cell",       "1"   );
         xml.writeAttribute   ( "channel",    "A"   );
         xml.writeAttribute   ( "wavelength", "280" );
         xml.writeEndElement();
         xml.writeEndElement();
         xml.writeEndDocument();
         file.close();
      }

      // Run directories, each with one small .auc file, so that a scan
      // takes long enough to be caught part way
      void buildManyRuns( int count )
      {
         for ( int ii = 0; ii < count; ii++ )
         {
            QString runID = QString( "Many_%1" ).arg( ii, 4, 10, QChar( '0' ) );
            QString dir   = US_Settings::resultDir() + "/" + runID;
            QDir().mkpath( dir );
            writeAuc( dir + "/" + runID + ".RA.1.A.280.auc", US_Util::new_guid() );
         }
      }

      void buildStore()
      {
         QString rawOne = US_Util::new_guid();
         QString dir    = US_Settings::resultDir() + "/" + runOne;
         QDir().mkpath( dir );

         writeAuc ( dir + "/" + runOne + ".RA.1.A.280.auc", rawOne );
         writeEdit( dir + "/" + runOne + ".2401011200.RA.1.A.280.xml",
                    editOneGUID, rawOne );

         dir = US_Settings::resultDir() + "/" + runTwo;
         QDir().mkpath( dir );
         writeAuc( dir + "/" + runTwo + ".RA.2.B.280.auc",
                   US_Util::new_guid() );

         US_Model model;
         model.modelGUID   = modelGUID;
         model.editGUID    = editOneGUID;
         model.description = runOne + ".1A280.2dsa.model";
         model.write( US_Settings::dataDir() + "/models/M0000001.xml" );

         US_Noise noise;
         noise.noiseGUID   = US_Util::new_guid();
         noise.modelGUID   = modelGUID;
         noise.description = runOne + ".1A280.2dsa.ti_noise";
         noise.type        = US_Noise::TI;
         noise.values << 0.001 << 0.002;
         noise.count  = 2;
         noise.write( US_Settings::dataDir() + "/noises/N0000001.xml" );
      }
};

// The scanner goes idle only when the owning thread takes the result, so
// right after a request it is busy, whatever the worker has done by then:
// the catalog is not handed out and a second job is refused.
TEST_F( TestUSDiskScan, TheCatalogIsNotOfferedWhileAJobIsRunning )
{
   US_DiskScan scan;

   ASSERT_TRUE( scan.isRunning() );          // the worker waits for a job
   EXPECT_FALSE( scan.busy() );
   EXPECT_NE( scan.catalog(), nullptr );

   ASSERT_TRUE( scan.list() );

   EXPECT_TRUE ( scan.busy() );
   EXPECT_EQ   ( scan.catalog(), nullptr );
   EXPECT_FALSE( scan.ok() );
   EXPECT_FALSE( scan.readChain() ) << "a second job was taken while busy";
   EXPECT_FALSE( scan.verify( 0 ) );

   settle( scan );

   ASSERT_NE ( scan.catalog(), nullptr );
   EXPECT_TRUE( scan.ok() ) << scan.error().toStdString();
   EXPECT_TRUE( scan.readChain() ) << "idle again, the next job is taken";
   settle( scan );
}

// Whoever jobDone reaches finds the scanner idle and its catalog readable,
// however the slot is connected
TEST_F( TestUSDiskScan, JobDoneFindsTheScannerIdle )
{
   US_DiskScan scan;
   int         seen    = 0;
   bool        allIdle = true;

   QObject::connect( &scan, &US_DiskScan::jobDone, [ & ]( int, int )
   {
      seen++;
      allIdle = allIdle  &&  ! scan.busy()  &&  scan.catalog() != nullptr;
   } );

   ASSERT_TRUE( scan.list() );
   settle( scan );
   ASSERT_TRUE( scan.readChain() );
   settle( scan );

   EXPECT_EQ  ( seen, 2 );
   EXPECT_TRUE( allIdle );
}

TEST_F( TestUSDiskScan, TheWorkerReadsWhatTheCallingThreadWouldRead )
{
   US_DiskScan scan;
   settle( scan );

   scan.list();
   settle( scan );
   ASSERT_TRUE( scan.ok() ) << scan.error().toStdString();

   scan.readChain();
   settle( scan );
   ASSERT_TRUE( scan.ok() ) << scan.error().toStdString();

   US_DataCatalog* worker = scan.catalog();
   ASSERT_NE( worker, nullptr );

   US_DataCatalog here;
   QString        error;
   here.setChecksums( false );
   ASSERT_TRUE( here.open( US_DataCatalog::Disk, QString(), error ) );
   ASSERT_TRUE( here.loadRuns( error ) );
   ASSERT_TRUE( here.loadAll ( error ) );

   ASSERT_EQ( worker->runCount(), here.runCount() );
   ASSERT_GT( worker->runCount(), 0 );

   for ( int ii = 0; ii < worker->runCount(); ii++ )
   {
      const US_DataCatalog::Run& wr = worker->run( ii );
      const US_DataCatalog::Run& hr = here   .run( ii );

      ASSERT_EQ  ( wr.runID.toStdString(), hr.runID.toStdString() );
      ASSERT_TRUE( wr.isLoaded() );
      EXPECT_EQ  ( wr.rawCount,   hr.rawCount   );
      EXPECT_EQ  ( wr.editCount,  hr.editCount  );
      EXPECT_EQ  ( wr.modelCount, hr.modelCount );
      EXPECT_EQ  ( wr.noiseCount, hr.noiseCount );
      ASSERT_EQ  ( wr.raws.size(), hr.raws.size() );

      for ( int jj = 0; jj < wr.raws.size(); jj++ )
         EXPECT_EQ( wr.raws[ jj ].guid.toStdString(),
                    hr.raws[ jj ].guid.toStdString() );
   }
}

TEST_F( TestUSDiskScan, AVerifyJobHashesOneExperimentsFiles )
{
   US_DiskScan scan;
   settle( scan );

   scan.list();
   settle( scan );
   scan.readChain();
   settle( scan );

   int index = scan.catalog()->indexOfRun( runOne );
   ASSERT_GE( index, 0 );

   EXPECT_FALSE( scan.catalog()->isRunVerified( index ) );
   EXPECT_TRUE ( scan.catalog()->run( index ).raws[ 0 ].checksum.isEmpty() );

   scan.verify( index );
   settle( scan );
   ASSERT_TRUE( scan.ok() ) << scan.error().toStdString();

   const US_DataCatalog::Run& run = scan.catalog()->run( index );

   EXPECT_TRUE ( scan.catalog()->isRunVerified( index ) );
   ASSERT_FALSE( run.raws[ 0 ].checksum.isEmpty() );

   QString expect = US_Util::md5sum_file( run.raws[ 0 ].path );
   EXPECT_EQ( run.raws[ 0 ].checksum.toStdString(),
              expect.section( " ", 0, 0 ).toStdString() );

   // the other experiment was not touched
   int other = scan.catalog()->indexOfRun( runTwo );
   ASSERT_GE ( other, 0 );
   EXPECT_FALSE( scan.catalog()->isRunVerified( other ) );
}

TEST_F( TestUSDiskScan, JobsRunOneAfterAnotherInTheOrderTheyWereAsked )
{
   US_DiskScan scan;
   QSignalSpy  spy( &scan, &US_DiskScan::jobDone );

   settle( scan );

   scan.list();
   settle( scan );
   scan.readChain();
   settle( scan );

   int index = scan.catalog()->indexOfRun( runOne );
   ASSERT_GE( index, 0 );

   scan.verify( index );
   settle( scan );

   ASSERT_EQ( spy.count(), 3 );
   EXPECT_EQ( spy.at( 0 ).at( 0 ).toInt(), int( US_DiskScan::JobList   ) );
   EXPECT_EQ( spy.at( 1 ).at( 0 ).toInt(), int( US_DiskScan::JobChain  ) );
   EXPECT_EQ( spy.at( 2 ).at( 0 ).toInt(), int( US_DiskScan::JobVerify ) );
   EXPECT_EQ( spy.at( 2 ).at( 1 ).toInt(), index );
}

// Destroying the scanner while it is working must not crash or hang: the
// job is cancelled at the next file and the thread is joined.  The
// scanner is certainly busy when it is destroyed here, since it goes idle
// only on this thread.
TEST_F( TestUSDiskScan, TearingItDownWhileItWorksIsSafe )
{
   buildManyRuns( 200 );

   QElapsedTimer timer;
   timer.start();

   {
      US_DiskScan scan;
      ASSERT_TRUE( scan.list() );
      ASSERT_TRUE( scan.busy() );
   }

   EXPECT_LT( timer.elapsed(), 20000 );
}

// A job that is cancelled stops, and says why.  The cancel is made from
// the worker itself, at the catalog's first note -- before it reads the
// first file -- so the worker cannot finish first.
TEST_F( TestUSDiskScan, ACancelledJobFailsAndSaysSo )
{
   US_DiskScan scan;
   ASSERT_TRUE( scan.list() );
   settle( scan );
   ASSERT_TRUE( scan.ok() ) << scan.error().toStdString();
   const int listed = scan.catalog()->runCount();
   ASSERT_EQ( listed, 2 );

   QMetaObject::Connection gate = QObject::connect(
      scan.catalog(), &US_DataCatalog::message, &scan,
      [ &scan ]( const QString& ) { scan.cancel(); }, Qt::DirectConnection );

   ASSERT_TRUE( scan.list() );
   settle( scan );
   QObject::disconnect( gate );

   EXPECT_FALSE( scan.ok() );
   EXPECT_TRUE ( scan.error().contains( "cancelled" ) ) << scan.error().toStdString();

   // a stopped read leaves the catalog as it was
   EXPECT_EQ( scan.catalog()->runCount(), listed );
   EXPECT_GE( scan.catalog()->indexOfRun( runOne ), 0 );

   // and the next job is not cancelled by the last one's request
   ASSERT_TRUE( scan.list() );
   settle( scan );
   EXPECT_TRUE( scan.ok() ) << scan.error().toStdString();
   EXPECT_EQ  ( scan.catalog()->runCount(), listed );
}

// A listing that fails keeps the last one: the store going away (a network
// share dropping, say) does not empty what the window shows
TEST_F( TestUSDiskScan, AFailedListingKeepsTheLastOne )
{
   US_DiskScan scan;
   ASSERT_TRUE( scan.list() );
   settle( scan );
   ASSERT_TRUE( scan.ok() ) << scan.error().toStdString();
   ASSERT_EQ  ( scan.catalog()->runCount(), 2 );

   US_Settings::set_workBaseDir( root + "/nothing-here" );

   ASSERT_TRUE( scan.list() );
   settle( scan );
   US_Settings::set_workBaseDir( root );

   EXPECT_FALSE( scan.ok() );
   EXPECT_FALSE( scan.error().isEmpty() );
   EXPECT_EQ   ( scan.catalog()->runCount(), 2 );
   EXPECT_GE   ( scan.catalog()->indexOfRun( runOne ), 0 );
}

// The cancel flag, checked by the catalog itself, deterministically
TEST_F( TestUSDiskScan, ACatalogStopsWhenItsCancelFlagIsSet )
{
   QAtomicInt     flag;
   US_DataCatalog catalog;
   QString        error;

   catalog.setCancelFlag( &flag );
   ASSERT_TRUE( catalog.open( US_DataCatalog::Disk, QString(), error ) );
   ASSERT_TRUE( catalog.loadRuns( error ) ) << error.toStdString();
   ASSERT_EQ  ( catalog.runCount(), 2 );

   flag.storeRelease( 1 );

   EXPECT_FALSE( catalog.loadAll( error ) );
   EXPECT_TRUE ( error.contains( "cancelled" ) ) << error.toStdString();
   EXPECT_FALSE( catalog.isRunLoaded( 0 ) );

   EXPECT_FALSE( catalog.loadRuns( error ) );
   EXPECT_EQ   ( catalog.runCount(), 2 ) << "a cancelled listing keeps the last one";

   EXPECT_FALSE( catalog.verifyRun( 0, error ) );
   EXPECT_FALSE( catalog.isRunVerified( 0 ) );

   flag.storeRelease( 0 );
   EXPECT_TRUE( catalog.loadAll( error ) ) << error.toStdString();
}

// A store that is not there is an answer, not a crash
TEST_F( TestUSDiskScan, AMissingStoreIsReportedNotFatal )
{
   US_Settings::set_workBaseDir( root + "/nothing-here" );

   US_DiskScan scan;
   settle( scan );

   scan.list();
   settle( scan );

   EXPECT_FALSE( scan.ok() );
   EXPECT_FALSE( scan.error().isEmpty() );
}
