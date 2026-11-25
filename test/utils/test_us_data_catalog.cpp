// test_us_data_catalog.cpp - Unit tests for US_DataCatalog
#include "qt_test_base.h"

#include <QtCore>

#include "us_data_catalog.h"
#include "us_settings.h"
#include "us_util.h"
#include "us_dataIO.h"
#include "us_model.h"
#include "us_noise.h"

/**
 * A synthetic UltraScan3 local store with two runs.
 *
 * The first run has two triples; the first of those has two edits and the
 * older edit carries a model with one noise record.  The second run has one
 * triple and no edits at all, which is what an experiment looks like before
 * anyone has worked on it.
 */
class TestUSDataCatalog : public QtTestBase
{
   protected:
      QString root;
      QString savedWorkDir;

      QString runOne;
      QString runTwo;
      QString expOneGUID;
      QString rawOneGUID;
      QString rawTwoGUID;
      QString editOneGUID;
      QString editTwoGUID;
      QString modelGUID;
      QString noiseGUID;
      QString projectGUID;

      void SetUp() override
      {
         QtTestBase::SetUp();

         savedWorkDir = US_Settings::workBaseDir();

         root = QDir::tempPath() + "/us_catalog_test_"
                + QString::number( QCoreApplication::applicationPid() ) + "_"
                + QString::number( QDateTime::currentMSecsSinceEpoch() );
         QDir().mkpath( root );
         US_Settings::set_workBaseDir( root );

         runOne      = "Cat_run_one";
         runTwo      = "Cat.run.two";        // a run identifier with dots in it
         expOneGUID  = US_Util::new_guid();
         rawOneGUID  = US_Util::new_guid();
         rawTwoGUID  = US_Util::new_guid();
         editOneGUID = US_Util::new_guid();
         editTwoGUID = US_Util::new_guid();
         modelGUID   = US_Util::new_guid();
         noiseGUID   = US_Util::new_guid();
         projectGUID = US_Util::new_guid();

         QDir().mkpath( US_Settings::dataDir() + "/models" );
         QDir().mkpath( US_Settings::dataDir() + "/noises" );

         buildRunOne();
         buildRunTwo();
         buildModelAndNoise();
      }

      void TearDown() override
      {
         US_Settings::set_workBaseDir( savedWorkDir );
         QDir( root ).removeRecursively();

         QtTestBase::TearDown();
      }

      QString runDir( const QString& runID )
      {
         QString path = US_Settings::resultDir() + "/" + runID;
         QDir().mkpath( path );
         return path;
      }

      void writeAuc( const QString& path, const QString& guid,
                     const QString& description )
      {
         US_DataIO::RawData data;
         data.type[ 0 ]   = 'R';
         data.type[ 1 ]   = 'A';
         US_Util::uuid_parse( guid, (unsigned char*)data.rawGUID );
         data.cell        = 1;
         data.channel     = 'A';
         data.description = description;

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

      // An edit file as us_edit writes one, so the GUIDs sit where every
      // reader of an edit expects them
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
         xml.writeEndElement();               // identification
         xml.writeStartElement( "run" );
         xml.writeAttribute   ( "cell",       "1"   );
         xml.writeAttribute   ( "channel",    "A"   );
         xml.writeAttribute   ( "wavelength", "280" );
         xml.writeEndElement();
         xml.writeEndElement();               // experiment
         xml.writeEndDocument();
         file.close();
      }

      void writeExperimentXml( const QString& path, const QString& runID,
                               const QString& guid )
      {
         QFile file( path );
         ASSERT_TRUE( file.open( QIODevice::WriteOnly | QIODevice::Text ) );

         QXmlStreamWriter xml( &file );
         xml.setAutoFormatting( true );
         xml.writeStartDocument();
         xml.writeStartElement( "US_Scandata" );
         xml.writeStartElement( "experiment" );
         xml.writeAttribute( "id",    "31" );
         xml.writeAttribute( "guid",  guid );
         xml.writeAttribute( "type",  "velocity" );
         xml.writeAttribute( "runID", runID );
         xml.writeStartElement( "project" );
         xml.writeAttribute( "id",   "7" );
         xml.writeAttribute( "guid", projectGUID );
         xml.writeAttribute( "desc", "Catalog test project" );
         xml.writeEndElement();
         xml.writeEndElement();
         xml.writeEndElement();
         xml.writeEndDocument();
         file.close();
      }

      void buildRunOne()
      {
         QString dir = runDir( runOne );

         writeAuc( dir + "/" + runOne + ".RA.1.A.280.auc", rawOneGUID,
                   "triple one" );
         writeAuc( dir + "/" + runOne + ".RA.1.A.281.auc", rawTwoGUID,
                   "triple two" );

         writeEdit( dir + "/" + runOne + ".2401011200.RA.1.A.280.xml",
                    editOneGUID, rawOneGUID );
         writeEdit( dir + "/" + runOne + ".2401011300.RA.1.A.280.xml",
                    editTwoGUID, rawOneGUID );

         writeExperimentXml( dir + "/" + runOne + ".RA.xml", runOne,
                             expOneGUID );

         // files that are in a run directory but are not part of the chain
         QFile other( dir + "/" + runOne + ".RIProfile.xml" );
         other.open( QIODevice::WriteOnly );
         other.write( "<RIProfile/>" );
         other.close();
      }

      void buildRunTwo()
      {
         QString dir = runDir( runTwo );

         writeAuc( dir + "/" + runTwo + ".RA.2.B.280.auc", US_Util::new_guid(),
                   "second run triple" );
         writeExperimentXml( dir + "/" + runTwo + ".RA.xml", runTwo,
                             US_Util::new_guid() );
      }

      void buildModelAndNoise()
      {
         US_Model model;
         model.modelGUID   = modelGUID;
         model.editGUID    = editOneGUID;
         model.description = runOne + ".1A280.2dsa.model";
         model.write( US_Settings::dataDir() + "/models/M0000001.xml" );

         // a model of another edit entirely, which must not be attached here
         US_Model other;
         other.modelGUID   = US_Util::new_guid();
         other.editGUID    = US_Util::new_guid();
         other.description = "Somebody_elses_run.1A280.2dsa.model";
         other.write( US_Settings::dataDir() + "/models/M0000002.xml" );

         US_Noise noise;
         noise.noiseGUID   = noiseGUID;
         noise.modelGUID   = modelGUID;
         noise.description = runOne + ".1A280.2dsa.ti_noise";
         noise.type        = US_Noise::TI;
         noise.values << 0.001 << 0.002;
         noise.count  = 2;
         noise.write( US_Settings::dataDir() + "/noises/N0000001.xml" );
      }
};

TEST_F( TestUSDataCatalog, RunIdOfFileHandlesDottedRunIds )
{
   EXPECT_EQ( US_DataCatalog::runIdOfFile( "plain.RA.1.A.280.auc", false ),
              "plain" );
   EXPECT_EQ( US_DataCatalog::runIdOfFile( "a.b.c.RA.1.A.280.auc", false ),
              "a.b.c" );
   EXPECT_EQ( US_DataCatalog::runIdOfFile(
              "plain.2401011200.RA.1.A.280.xml", true ), "plain" );
   EXPECT_EQ( US_DataCatalog::runIdOfFile(
              "a.b.c.2401011200.RA.1.A.280.xml", true ), "a.b.c" );
}

TEST_F( TestUSDataCatalog, FirstLayerListsRunsWithCounts )
{
   US_DataCatalog catalog;
   QString        error;

   ASSERT_TRUE( catalog.open( US_DataCatalog::Disk, QString(), error ) )
      << error.toStdString();
   ASSERT_TRUE( catalog.loadRuns( error ) ) << error.toStdString();

   ASSERT_EQ( catalog.runCount(), 2 );

   int one = catalog.indexOfRun( runOne );
   ASSERT_GE( one, 0 );

   const US_DataCatalog::Run& run = catalog.run( one );
   EXPECT_EQ( run.guid,        expOneGUID );
   EXPECT_EQ( run.runType,     "RA" );
   EXPECT_EQ( run.projectGUID, projectGUID );
   EXPECT_EQ( run.projectDesc, "Catalog test project" );
   EXPECT_EQ( run.rawCount,    2 );
   EXPECT_EQ( run.editCount,   2 );
   EXPECT_EQ( run.modelCount,  1 ) << "the other run's model must not count";
   EXPECT_EQ( run.noiseCount,  1 );

   // The first layer says nothing about the chain below the experiment
   EXPECT_FALSE( run.isLoaded() );
   EXPECT_TRUE ( run.raws.isEmpty() );

   int two = catalog.indexOfRun( runTwo );
   ASSERT_GE( two, 0 ) << "a run identifier with dots must still be found";
   EXPECT_EQ( catalog.run( two ).rawCount,  1 );
   EXPECT_EQ( catalog.run( two ).editCount, 0 );

   // ... but it is not one UltraScan accepts any more, and says so
   EXPECT_TRUE ( run.usable ) << run.problem.toStdString();
   EXPECT_FALSE( catalog.run( two ).usable );
   EXPECT_FALSE( catalog.run( two ).problem.isEmpty() );
}

TEST_F( TestUSDataCatalog, SecondLayerLoadsTheWholeChain )
{
   US_DataCatalog catalog;
   QString        error;

   ASSERT_TRUE( catalog.open( US_DataCatalog::Disk, QString(), error ) );
   ASSERT_TRUE( catalog.loadRuns( error ) );

   int one = catalog.indexOfRun( runOne );
   ASSERT_GE( one, 0 );
   ASSERT_TRUE( catalog.loadRunDetail( one, error ) ) << error.toStdString();
   ASSERT_TRUE( catalog.isRunLoaded( one ) );

   const US_DataCatalog::Run& run = catalog.run( one );
   ASSERT_EQ( run.raws.size(), 2 );

   // the triples come back with the GUID and description from the .auc header
   const US_DataCatalog::Raw* first = nullptr;

   for ( int ii = 0; ii < run.raws.size(); ii++ )
      if ( run.raws[ ii ].triple == "1.A.280" )  first = &run.raws[ ii ];

   ASSERT_NE( first, nullptr );
   EXPECT_EQ( first->guid,        rawOneGUID );
   EXPECT_EQ( first->description, "triple one" );
   EXPECT_EQ( first->dataType,    "RA" );
   EXPECT_FALSE( first->checksum.isEmpty() );
   EXPECT_FALSE( first->size    .isEmpty() );

   ASSERT_EQ( first->edits.size(), 2 );
   EXPECT_EQ( first->edits[ 0 ].guid,    editOneGUID );
   EXPECT_EQ( first->edits[ 0 ].rawGUID, rawOneGUID );
   EXPECT_EQ( first->edits[ 0 ].editID,  "2401011200" );
   EXPECT_EQ( first->edits[ 1 ].guid,    editTwoGUID );

   // the model hangs off the edit it was fitted to, and only that one
   ASSERT_EQ( first->edits[ 0 ].models.size(), 1 );
   EXPECT_EQ( first->edits[ 0 ].models[ 0 ].guid,     modelGUID );
   EXPECT_EQ( first->edits[ 0 ].models[ 0 ].editGUID, editOneGUID );
   EXPECT_EQ( first->edits[ 1 ].models.size(), 0 );

   ASSERT_EQ( first->edits[ 0 ].models[ 0 ].noises.size(), 1 );
   EXPECT_EQ( first->edits[ 0 ].models[ 0 ].noises[ 0 ].guid, noiseGUID );
   EXPECT_EQ( first->edits[ 0 ].models[ 0 ].noises[ 0 ].modelGUID,
              modelGUID );

   // a triple with no edits is normal, not an error
   const US_DataCatalog::Raw* second = nullptr;

   for ( int ii = 0; ii < run.raws.size(); ii++ )
      if ( run.raws[ ii ].triple == "1.A.281" )  second = &run.raws[ ii ];

   ASSERT_NE( second, nullptr );
   EXPECT_EQ( second->guid, rawTwoGUID );
   EXPECT_EQ( second->edits.size(), 0 );
}

TEST_F( TestUSDataCatalog, LoadingIsIdempotent )
{
   US_DataCatalog catalog;
   QString        error;

   ASSERT_TRUE( catalog.open( US_DataCatalog::Disk, QString(), error ) );
   ASSERT_TRUE( catalog.loadRuns( error ) );

   int one = catalog.indexOfRun( runOne );
   ASSERT_TRUE( catalog.loadRunDetail( one, error ) );

   int raws = catalog.run( one ).raws.size();

   ASSERT_TRUE( catalog.loadRunDetail( one, error ) );
   EXPECT_EQ( catalog.run( one ).raws.size(), raws )
      << "loading an experiment twice must not double its contents";
}

TEST_F( TestUSDataCatalog, PendingQueueWorksThroughEveryRun )
{
   US_DataCatalog catalog;
   QString        error;

   ASSERT_TRUE( catalog.open( US_DataCatalog::Disk, QString(), error ) );
   ASSERT_TRUE( catalog.loadRuns( error ) );

   EXPECT_EQ( catalog.pendingCount(), 2 );

   int loaded = 0;
   int index  = -1;

   while ( catalog.loadNextPending( index, error ) )
   {
      EXPECT_GE( index, 0 );
      EXPECT_TRUE( catalog.isRunLoaded( index ) );
      loaded++;
   }

   EXPECT_TRUE( error.isEmpty() ) << error.toStdString();
   EXPECT_EQ( loaded, 2 );
   EXPECT_EQ( catalog.pendingCount(), 0 );
}

TEST_F( TestUSDataCatalog, RequestedRunIsLoadedNext )
{
   US_DataCatalog catalog;
   QString        error;

   ASSERT_TRUE( catalog.open( US_DataCatalog::Disk, QString(), error ) );
   ASSERT_TRUE( catalog.loadRuns( error ) );

   // Ask for whichever run the queue would have reached last
   int wanted = catalog.runCount() - 1;
   catalog.requestRun( wanted );

   int index = -1;
   ASSERT_TRUE( catalog.loadNextPending( index, error ) );
   EXPECT_EQ( index, wanted )
      << "a run the user opened must jump the queue";
}

TEST_F( TestUSDataCatalog, RequestingALoadedRunDoesNotQueueItAgain )
{
   US_DataCatalog catalog;
   QString        error;

   ASSERT_TRUE( catalog.open( US_DataCatalog::Disk, QString(), error ) );
   ASSERT_TRUE( catalog.loadRuns( error ) );

   int one = catalog.indexOfRun( runOne );
   ASSERT_TRUE( catalog.loadRunDetail( one, error ) );

   int before = catalog.pendingCount();
   catalog.requestRun( one );
   EXPECT_EQ( catalog.pendingCount(), before );
}

TEST_F( TestUSDataCatalog, ProjectFilterNarrowsTheList )
{
   US_DataCatalog catalog;
   QString        error;

   ASSERT_TRUE( catalog.open( US_DataCatalog::Disk, QString(), error ) );

   ASSERT_TRUE( catalog.loadRuns( projectGUID, error ) );
   EXPECT_EQ( catalog.runCount(), 2 );

   ASSERT_TRUE( catalog.loadRuns( US_Util::new_guid(), error ) );
   EXPECT_EQ( catalog.runCount(), 0 )
      << "no run belongs to a project that is not in the store";
}

// The point of the index is that the store is walked once per scan; a file
// that appears afterwards belongs to the next scan, not this one.
TEST_F( TestUSDataCatalog, TheStoreIsReadOncePerScan )
{
   US_DataCatalog catalog;
   QString        error;

   ASSERT_TRUE( catalog.open( US_DataCatalog::Disk, QString(), error ) );
   ASSERT_TRUE( catalog.loadRuns( error ) );
   ASSERT_EQ  ( catalog.runCount(), 2 );

   // a third run appears while the scan is being worked through
   QString dir = runDir( "Late_arrival" );
   writeAuc( dir + "/Late_arrival.RA.1.A.280.auc", US_Util::new_guid(),
             "late" );

   int one = catalog.indexOfRun( runOne );
   ASSERT_TRUE( catalog.loadRunDetail( one, error ) )
      << "the scan in progress still completes";
   EXPECT_EQ( catalog.runCount(), 2 );

   // and the next listing picks it up, with nothing cleared by hand
   ASSERT_TRUE( catalog.loadRuns( error ) );
   EXPECT_EQ( catalog.runCount(), 3 );
   EXPECT_GE( catalog.indexOfRun( "Late_arrival" ), 0 );
}

TEST_F( TestUSDataCatalog, NoiseOfOneModelIsFoundWithoutTheChain )
{
   US_DataCatalog catalog;
   QString        error;

   ASSERT_TRUE( catalog.open( US_DataCatalog::Disk, QString(), error ) );

   QList< US_DataCatalog::Noise > noises =
      catalog.noisesOfModel( modelGUID, error );

   ASSERT_TRUE( error.isEmpty() ) << error.toStdString();
   ASSERT_EQ  ( noises.size(), 1 );
   EXPECT_EQ  ( noises[ 0 ].guid     .toStdString(), noiseGUID.toStdString() );
   EXPECT_EQ  ( noises[ 0 ].modelGUID.toStdString(), modelGUID.toStdString() );
   EXPECT_FALSE( noises[ 0 ].checksum.isEmpty() );

   // a model nobody fitted noise to answers with nothing, not an error
   noises = catalog.noisesOfModel( US_Util::new_guid(), error );
   EXPECT_TRUE( error.isEmpty() );
   EXPECT_EQ  ( noises.size(), 0 );
}

TEST_F( TestUSDataCatalog, NoiseOfAnEditIsTheNoiseOfAllItsModels )
{
   // a second model of the same edit, with its own noise record
   QString  otherModel = US_Util::new_guid();
   QString  otherNoise = US_Util::new_guid();

   US_Model model;
   model.modelGUID   = otherModel;
   model.editGUID    = editOneGUID;
   model.description = runOne + ".1A280.2dsa-mc.model";
   model.write( US_Settings::dataDir() + "/models/M0000003.xml" );

   US_Noise noise;
   noise.noiseGUID   = otherNoise;
   noise.modelGUID   = otherModel;
   noise.description = runOne + ".1A280.2dsa-mc.ri_noise";
   noise.type        = US_Noise::RI;
   noise.values << 0.003;
   noise.count  = 1;
   noise.write( US_Settings::dataDir() + "/noises/N0000003.xml" );

   US_DataCatalog catalog;
   QString        error;

   ASSERT_TRUE( catalog.open( US_DataCatalog::Disk, QString(), error ) );

   QList< US_DataCatalog::Noise > noises =
      catalog.noisesOfEdit( editOneGUID, QString(), error );

   ASSERT_TRUE( error.isEmpty() ) << error.toStdString();
   ASSERT_EQ  ( noises.size(), 2 );

   QStringList guids;

   for ( int ii = 0; ii < noises.size(); ii++ )
      guids << noises[ ii ].guid;

   EXPECT_TRUE( guids.contains( noiseGUID  ) );
   EXPECT_TRUE( guids.contains( otherNoise ) );

   // an edit nobody fitted anything to answers with nothing, not an error
   noises = catalog.noisesOfEdit( US_Util::new_guid(), QString(), error );
   EXPECT_TRUE( error.isEmpty() );
   EXPECT_EQ  ( noises.size(), 0 );
}

// Listing the whole store in one go gives the same chain reading it one
// experiment at a time does.
TEST_F( TestUSDataCatalog, LoadAllGivesTheSameChainAsOneAtATime )
{
   US_DataCatalog oneAtATime;
   US_DataCatalog allAtOnce;
   QString        error;

   ASSERT_TRUE( oneAtATime.open( US_DataCatalog::Disk, QString(), error ) );
   ASSERT_TRUE( allAtOnce .open( US_DataCatalog::Disk, QString(), error ) );
   ASSERT_TRUE( oneAtATime.loadRuns( error ) );
   ASSERT_TRUE( allAtOnce .loadRuns( error ) );

   for ( int ii = 0; ii < oneAtATime.runCount(); ii++ )
      ASSERT_TRUE( oneAtATime.loadRunDetail( ii, error ) );

   ASSERT_TRUE( allAtOnce.loadAll( error ) ) << error.toStdString();

   ASSERT_EQ( allAtOnce.runCount(), oneAtATime.runCount() );
   EXPECT_EQ( allAtOnce.pendingCount(), 0 );

   for ( int ii = 0; ii < allAtOnce.runCount(); ii++ )
   {
      const US_DataCatalog::Run& one = oneAtATime.run( ii );
      const US_DataCatalog::Run& all = allAtOnce .run( ii );

      ASSERT_TRUE( all.isLoaded() );
      ASSERT_EQ  ( all.runID.toStdString(), one.runID.toStdString() );
      EXPECT_EQ  ( all.rawCount,   one.rawCount   );
      EXPECT_EQ  ( all.editCount,  one.editCount  );
      EXPECT_EQ  ( all.modelCount, one.modelCount );
      EXPECT_EQ  ( all.noiseCount, one.noiseCount );
      ASSERT_EQ  ( all.raws.size(), one.raws.size() );

      for ( int jj = 0; jj < all.raws.size(); jj++ )
      {
         EXPECT_EQ( all.raws[ jj ].guid.toStdString(),
                    one.raws[ jj ].guid.toStdString() );
         ASSERT_EQ( all.raws[ jj ].edits.size(),
                    one.raws[ jj ].edits.size() );

         for ( int kk = 0; kk < all.raws[ jj ].edits.size(); kk++ )
            EXPECT_EQ( all.raws[ jj ].edits[ kk ].models.size(),
                       one.raws[ jj ].edits[ kk ].models.size() );
      }
   }
}

// Hashing every file in a store is the expensive half of a scan and answers
// only whether a record that is in two places still matches, so a listing
// can leave it out and ask for it per experiment.
TEST_F( TestUSDataCatalog, ChecksumsAreLeftOutUntilAnExperimentIsVerified )
{
   US_DataCatalog catalog;
   QString        error;

   ASSERT_TRUE( catalog.open( US_DataCatalog::Disk, QString(), error ) );
   ASSERT_TRUE( catalog.checksums() );          // on unless a caller says no

   catalog.setChecksums( false );
   EXPECT_FALSE( catalog.checksums() );

   ASSERT_TRUE( catalog.loadRuns( error ) );
   ASSERT_TRUE( catalog.loadAll ( error ) ) << error.toStdString();

   int index = catalog.indexOfRun( runOne );
   ASSERT_GE( index, 0 );

   const US_DataCatalog::Run& run = catalog.run( index );
   ASSERT_GT( run.raws.size(), 0 );

   EXPECT_FALSE( catalog.isRunVerified( index ) );
   EXPECT_TRUE ( run.raws[ 0 ].checksum.isEmpty() );
   EXPECT_TRUE ( run.raws[ 0 ].size    .isEmpty() );

   ASSERT_TRUE( catalog.verifyRun( index, error ) ) << error.toStdString();
   EXPECT_TRUE( catalog.isRunVerified( index ) );

   const US_DataCatalog::Run& done = catalog.run( index );
   EXPECT_FALSE( done.raws[ 0 ].checksum.isEmpty() );
   EXPECT_FALSE( done.raws[ 0 ].size    .isEmpty() );

   ASSERT_GT   ( done.raws[ 0 ].edits.size(), 0 );
   EXPECT_FALSE( done.raws[ 0 ].edits[ 0 ].checksum.isEmpty() );

   // the checksum is the one the file really has
   QString expect = US_Util::md5sum_file( done.raws[ 0 ].path );
   EXPECT_EQ( done.raws[ 0 ].checksum.toStdString(),
              expect.section( " ", 0, 0 ).toStdString() );
}

TEST_F( TestUSDataCatalog, TheVerifyQueueTakesTheRequestedExperimentFirst )
{
   US_DataCatalog catalog;
   QString        error;

   ASSERT_TRUE( catalog.open( US_DataCatalog::Disk, QString(), error ) );
   catalog.setChecksums( false );
   ASSERT_TRUE( catalog.loadRuns( error ) );
   ASSERT_TRUE( catalog.loadAll ( error ) );

   QList< int > all;

   for ( int ii = 0; ii < catalog.runCount(); ii++ )  all << ii;

   catalog.queueVerify( all );
   ASSERT_EQ( catalog.verifyPendingCount(), catalog.runCount() );

   int last = catalog.runCount() - 1;
   ASSERT_GT( last, 0 );

   catalog.requestVerify( last );

   int index = -1;
   ASSERT_TRUE( catalog.verifyNextPending( index, error ) )
      << error.toStdString();
   EXPECT_EQ  ( index, last );
   EXPECT_TRUE( catalog.isRunVerified( last ) );

   int knt = 1;

   while ( catalog.verifyNextPending( index, error ) )  knt++;

   EXPECT_TRUE( error.isEmpty() ) << error.toStdString();
   EXPECT_EQ  ( knt, catalog.runCount() );
   EXPECT_EQ  ( catalog.verifyPendingCount(), 0 );
}

TEST_F( TestUSDataCatalog, DiskCatalogHasNoDatabaseConnection )
{
   US_DataCatalog catalog;
   QString        error;

   ASSERT_TRUE( catalog.open( US_DataCatalog::Disk, QString(), error ) );

   EXPECT_TRUE ( catalog.isOpen() );
   EXPECT_FALSE( catalog.isDb() );
   EXPECT_EQ   ( catalog.db(), nullptr );
   EXPECT_EQ   ( catalog.source(), US_DataCatalog::Disk );
}

/* The investigator is chosen from a dialog any window may open, so the one
   a catalog was built with can be out of date by the time the next query
   goes out.  A local store holds the same work whoever is selected, so the
   catalog picks the change up without calling the listing stale.
*/
TEST_F( TestUSDataCatalog, TheSelectedInvestigatorIsPickedUp )
{
   // Put the investigator back however the test ends, a failed ASSERT too
   struct Restore
   {
      int saved = US_Settings::us_inv_ID();
      ~Restore() { US_Settings::set_us_inv_ID( saved ); }
   } restore;

   US_Settings::set_us_inv_ID( 11 );

   US_DataCatalog catalog;
   QString        error;

   ASSERT_TRUE( catalog.open( US_DataCatalog::Disk, QString(), error ) );
   EXPECT_EQ  ( catalog.investigatorID(), 11 );
   EXPECT_FALSE( catalog.refreshInvestigator() );

   US_Settings::set_us_inv_ID( 12 );

   EXPECT_FALSE( catalog.refreshInvestigator() );
   EXPECT_EQ   ( catalog.investigatorID(), 12 );
}

// ------------------------------------------------------- run directories

// Two directories whose files carry the same run ID are listed apart.  Keyed
// by run ID they used to collapse into one, and the triples of one were
// shown as the other's.  The one named after the run ID is the run and
// stays usable; the other is a copy of it, flagged, and ignored.  (Until
// round 7 both were refused, so a stray copy blocked the real run.)
TEST_F( TestUSDataCatalog, TwoDirectoriesWithTheSameRunIdAreTwoRuns )
{
   QString copyGUID = US_Util::new_guid();
   QString dir      = runDir( "Backup_" + runOne );
   writeAuc( dir + "/" + runOne + ".RA.1.A.280.auc", copyGUID, "the copy" );

   US_DataCatalog catalog;
   QString        error;
   QStringList    messages;
   QObject::connect( &catalog, &US_DataCatalog::message,
                     [ &messages ]( const QString& text )
                     { messages << text; } );

   ASSERT_TRUE( catalog.open( US_DataCatalog::Disk, QString(), error ) );
   ASSERT_TRUE( catalog.loadRuns( error ) );
   ASSERT_EQ  ( catalog.runCount(), 3 );

   QList< int > same;

   for ( int ii = 0; ii < catalog.runCount(); ii++ )
      if ( catalog.run( ii ).runID == runOne )  same << ii;

   ASSERT_EQ( same.size(), 2 );

   QStringList rawGUIDs;
   int         realIx = -1;

   for ( int ii : same )
   {
      const US_DataCatalog::Run& run = catalog.run( ii );

      if ( run.dirPath.endsWith( "/Backup_" + runOne ) )
      {  // the copy: flagged, naming the directory that is the run
         EXPECT_FALSE( run.usable );
         EXPECT_EQ   ( run.copyOf, runDir( runOne ) );
         EXPECT_TRUE ( run.problem.contains( "Backup_" + runOne ) )
            << run.problem.toStdString();
         EXPECT_TRUE ( run.problem.contains( "whose directory is " + runOne ) )
            << run.problem.toStdString();
      }
      else
      {  // the run itself
         realIx = ii;
         EXPECT_TRUE ( run.usable ) << run.problem.toStdString();
         EXPECT_TRUE ( run.problem.isEmpty() ) << run.problem.toStdString();
         EXPECT_TRUE ( run.copyOf.isEmpty() );
      }

      ASSERT_TRUE( catalog.loadRunDetail( ii, error ) ) << error.toStdString();

      for ( const US_DataCatalog::Raw& raw : catalog.run( ii ).raws )
         rawGUIDs << raw.guid;
   }

   // each directory gives its own triples
   EXPECT_EQ( rawGUIDs.size(), 3 );
   EXPECT_TRUE( rawGUIDs.contains( copyGUID ) );
   EXPECT_TRUE( rawGUIDs.contains( rawOneGUID ) );

   // the run ID finds the run, though the copy sorts before it
   ASSERT_GE( realIx, 0 );
   EXPECT_LT( same.first(), realIx );
   EXPECT_EQ( catalog.indexOfRun( runOne ), realIx );

   // and the listing warns about the copy
   QString warned = messages.filter( "copy of run " + runOne ).join( "\n" );
   EXPECT_TRUE( warned.contains( QDir::toNativeSeparators( dir ) ) )
      << messages.join( "\n" ).toStdString();
}

// When no directory is named after the run ID, which one is the run cannot
// be told: every one of them is refused, and none is called a copy
TEST_F( TestUSDataCatalog, ARunIdInDirectoriesNoneNamedAfterItIsRefusedInAll )
{
   QString dirA = runDir( "Backup_Lone_run" );
   QString dirB = runDir( "Lone_run_old" );
   writeAuc( dirA + "/Lone_run.RA.1.A.280.auc", US_Util::new_guid(), "a" );
   writeAuc( dirB + "/Lone_run.RA.1.A.280.auc", US_Util::new_guid(), "b" );

   US_DataCatalog catalog;
   QString        error;

   ASSERT_TRUE( catalog.open( US_DataCatalog::Disk, QString(), error ) );
   ASSERT_TRUE( catalog.loadRuns( error ) );

   int count = 0;

   for ( int ii = 0; ii < catalog.runCount(); ii++ )
   {
      const US_DataCatalog::Run& run = catalog.run( ii );

      if ( run.runID != "Lone_run" )  continue;

      count++;
      EXPECT_FALSE( run.usable );
      EXPECT_TRUE ( run.copyOf.isEmpty() );
      EXPECT_TRUE ( run.problem.contains( "also in" ) )
         << run.problem.toStdString();
      EXPECT_TRUE ( run.problem.contains( "no directory is named after it" ) )
         << run.problem.toStdString();
   }

   EXPECT_EQ( count, 2 );
}

// A run ID that is a prefix of another followed by a dot does not count
// the other run's models as its own
TEST_F( TestUSDataCatalog, ModelCountsDoNotSpillIntoARunWithALongerId )
{
   QString longer = runOne + ".b";
   QString dir    = runDir( longer );
   writeAuc( dir + "/" + longer + ".RA.1.A.280.auc", US_Util::new_guid(), "longer" );

   US_Model model;
   model.modelGUID   = US_Util::new_guid();
   model.editGUID    = US_Util::new_guid();
   model.description = longer + ".1A280.2dsa.model";
   model.write( US_Settings::dataDir() + "/models/M0000009.xml" );

   US_DataCatalog catalog;
   QString        error;

   ASSERT_TRUE( catalog.open( US_DataCatalog::Disk, QString(), error ) );
   ASSERT_TRUE( catalog.loadRuns( error ) );

   int one = catalog.indexOfRun( runOne );
   int two = catalog.indexOfRun( longer );
   ASSERT_GE( one, 0 );
   ASSERT_GE( two, 0 );

   EXPECT_EQ( catalog.run( one ).modelCount, 1 );
   EXPECT_EQ( catalog.run( two ).modelCount, 1 );
}

// --------------------------------------------------------- empty GUIDs

// A record written without a GUID must not become the parent of every
// other record written without one
TEST_F( TestUSDataCatalog, AnEmptyGuidJoinsNothing )
{
   QString dir = runDir( runOne );
   writeEdit( dir + "/" + runOne + ".2401011500.RA.1.A.281.xml", "", rawTwoGUID );

   US_Model model;
   model.modelGUID   = "";
   model.editGUID    = "";
   model.description = runOne + ".1A281.nothing.model";
   model.write( US_Settings::dataDir() + "/models/M0000005.xml" );

   US_Noise noise;
   noise.noiseGUID   = US_Util::new_guid();
   noise.modelGUID   = "";
   noise.description = runOne + ".1A281.nothing.ti_noise";
   noise.type        = US_Noise::TI;
   noise.values << 0.1;
   noise.count  = 1;
   noise.write( US_Settings::dataDir() + "/noises/N0000005.xml" );

   US_DataCatalog catalog;
   QString        error;

   ASSERT_TRUE( catalog.open( US_DataCatalog::Disk, QString(), error ) );
   ASSERT_TRUE( catalog.loadRuns( error ) );
   ASSERT_TRUE( catalog.loadAll ( error ) ) << error.toStdString();

   const US_DataCatalog::Run& run = catalog.run( catalog.indexOfRun( runOne ) );

   for ( const US_DataCatalog::Raw& raw : run.raws )
      for ( const US_DataCatalog::Edit& edit : raw.edits )
      {
         if ( ! edit.guid.isEmpty() )  continue;

         EXPECT_TRUE( edit.models.isEmpty() )
            << "a model with no editGUID was attached to an edit with none";
      }

   EXPECT_TRUE( catalog.noisesOfModel( "", error ).isEmpty() );
   EXPECT_TRUE( catalog.noisesOfEdit ( "", QString(), error ).isEmpty() );

   // both are listed as what they are: records with no parent
   bool modelFound = false;

   for ( const US_DataCatalog::Model& mm : catalog.orphans().models )
      if ( mm.description == runOne + ".1A281.nothing.model" )
      {
         modelFound = true;
         EXPECT_TRUE( mm.noises.isEmpty() );
      }

   EXPECT_TRUE( modelFound );

   bool noiseFound = false;

   for ( const US_DataCatalog::Noise& nn : catalog.orphans().noises )
      if ( nn.guid == noise.noiseGUID )  noiseFound = true;

   EXPECT_TRUE( noiseFound );
}

// ------------------------------------------------------------- orphans

TEST_F( TestUSDataCatalog, RecordsWithNoParentAreKeptAsOrphans )
{
   // an edit of a triple with no .auc file, with a model and its noise
   QString lostEdit  = US_Util::new_guid();
   QString lostModel = US_Util::new_guid();
   QString lostNoise = US_Util::new_guid();
   writeEdit( runDir( runOne ) + "/" + runOne + ".2401011600.RA.1.A.290.xml",
              lostEdit, US_Util::new_guid() );

   US_Model model;
   model.modelGUID   = lostModel;
   model.editGUID    = lostEdit;
   model.description = runOne + ".1A290.2dsa.model";
   model.write( US_Settings::dataDir() + "/models/M0000006.xml" );

   US_Noise noise;
   noise.noiseGUID   = lostNoise;
   noise.modelGUID   = lostModel;
   noise.description = runOne + ".1A290.2dsa.ti_noise";
   noise.type        = US_Noise::TI;
   noise.values << 0.1;
   noise.count  = 1;
   noise.write( US_Settings::dataDir() + "/noises/N0000006.xml" );

   // a run directory with an edit and no .auc file at all
   QString bareEdit = US_Util::new_guid();
   writeEdit( runDir( "Bare_run" ) + "/Bare_run.2401011700.RA.1.A.280.xml",
              bareEdit, US_Util::new_guid() );

   // noise of a model that is nowhere
   US_Noise stray;
   stray.noiseGUID   = US_Util::new_guid();
   stray.modelGUID   = US_Util::new_guid();
   stray.description = "Nowhere.1A280.2dsa.ti_noise";
   stray.type        = US_Noise::TI;
   stray.values << 0.1;
   stray.count  = 1;
   stray.write( US_Settings::dataDir() + "/noises/N0000007.xml" );

   US_DataCatalog catalog;
   QString        error;

   ASSERT_TRUE( catalog.open( US_DataCatalog::Disk, QString(), error ) );
   ASSERT_TRUE( catalog.loadRuns( error ) );
   EXPECT_EQ  ( catalog.runCount(), 2 ) << "a directory with no .auc is not a run";
   EXPECT_TRUE( catalog.orphans().isEmpty() );

   ASSERT_TRUE( catalog.loadAll( error ) ) << error.toStdString();

   const US_DataCatalog::Orphans& orphans = catalog.orphans();

   EXPECT_TRUE( orphans.raws.isEmpty() );

   // both loose edits, the first carrying its model and that model's noise
   ASSERT_EQ( orphans.edits.size(), 2 );
   const US_DataCatalog::Edit* lost = nullptr;

   for ( const US_DataCatalog::Edit& ed : orphans.edits )
   {
      if ( ed.guid == lostEdit )  lost = &ed;
      EXPECT_TRUE( ed.guid == lostEdit  ||  ed.guid == bareEdit );
   }

   ASSERT_NE( lost, nullptr );
   EXPECT_EQ( lost->runID,  runOne );
   EXPECT_EQ( lost->triple, QString( "1.A.290" ) );
   ASSERT_EQ( lost->models.size(), 1 );
   EXPECT_EQ( lost->models[ 0 ].guid, lostModel );
   ASSERT_EQ( lost->models[ 0 ].noises.size(), 1 );
   EXPECT_EQ( lost->models[ 0 ].noises[ 0 ].guid, lostNoise );

   // the fixture's model of somebody else's edit, and the stray noise
   ASSERT_EQ( orphans.models.size(), 1 );
   EXPECT_EQ( orphans.models[ 0 ].description,
              QString( "Somebody_elses_run.1A280.2dsa.model" ) );
   ASSERT_EQ( orphans.noises.size(), 1 );
   EXPECT_EQ( orphans.noises[ 0 ].guid, stray.noiseGUID );

   // and nothing in the chain is listed again as an orphan
   for ( const US_DataCatalog::Model& mm : orphans.models )
      EXPECT_NE( mm.guid, modelGUID );
}

// ------------------------------------------------------------- checksums

TEST_F( TestUSDataCatalog, NoiseOfAModelHonoursTheChecksumSetting )
{
   US_DataCatalog catalog;
   QString        error;

   ASSERT_TRUE( catalog.open( US_DataCatalog::Disk, QString(), error ) );
   catalog.setChecksums( false );

   QList< US_DataCatalog::Noise > noises = catalog.noisesOfModel( modelGUID, error );
   ASSERT_EQ  ( noises.size(), 1 );
   EXPECT_TRUE( noises[ 0 ].checksum.isEmpty() );
   EXPECT_EQ  ( noises[ 0 ].editGUID, editOneGUID );
   EXPECT_FALSE( noises[ 0 ].lastUpdated.isEmpty() );

   noises = catalog.noisesOfEdit( editOneGUID, QString(), error );
   ASSERT_EQ( noises.size(), 1 );
   EXPECT_EQ( noises[ 0 ].editGUID, editOneGUID );
}

// Reading the whole store keeps what is known about an experiment already
// read, rather than reading it again with less
TEST_F( TestUSDataCatalog, LoadAllKeepsAnExperimentAlreadyVerified )
{
   US_DataCatalog catalog;
   QString        error;

   ASSERT_TRUE( catalog.open( US_DataCatalog::Disk, QString(), error ) );
   ASSERT_TRUE( catalog.loadRuns( error ) );

   int one = catalog.indexOfRun( runOne );
   ASSERT_TRUE( catalog.loadRunDetail( one, error ) );
   ASSERT_TRUE( catalog.isRunVerified( one ) );

   catalog.setChecksums( false );
   ASSERT_TRUE( catalog.loadAll( error ) );

   EXPECT_TRUE ( catalog.isRunVerified( one ) );
   EXPECT_FALSE( catalog.run( one ).raws[ 0 ].checksum.isEmpty() );

   int two = catalog.indexOfRun( runTwo );
   EXPECT_TRUE ( catalog.isRunLoaded  ( two ) );
   EXPECT_FALSE( catalog.isRunVerified( two ) );
}

// Every date is in the form the database's dates are given in
TEST_F( TestUSDataCatalog, DatesAreInTheCatalogsOneForm )
{
   US_DataCatalog catalog;
   QString        error;

   ASSERT_TRUE( catalog.open( US_DataCatalog::Disk, QString(), error ) );
   ASSERT_TRUE( catalog.loadRuns( error ) );
   ASSERT_TRUE( catalog.loadAll ( error ) );

   QRegularExpression form( "^\\d{4}-\\d\\d-\\d\\d \\d\\d:\\d\\d:\\d\\d UTC$" );

   const US_DataCatalog::Run& run = catalog.run( catalog.indexOfRun( runOne ) );
   EXPECT_TRUE( form.match( run.date ).hasMatch() ) << run.date.toStdString();
   EXPECT_TRUE( form.match( run.raws[ 0 ].lastUpdated ).hasMatch() )
      << run.raws[ 0 ].lastUpdated.toStdString();
   EXPECT_TRUE( form.match( run.raws[ 0 ].edits[ 0 ].models[ 0 ].lastUpdated ).hasMatch() );
}

// ------------------------------------------------- the second review

// A store with no run in it is still read by loadAll: its edits, models and
// noise records are all orphans, and loadAll used to return at once
TEST_F( TestUSDataCatalog, AStoreWithNoRunIsAllOrphans )
{
   QDir( runDir( runOne ) ).removeRecursively();
   QDir( runDir( runTwo ) ).removeRecursively();

   QString bareEdit = US_Util::new_guid();
   writeEdit( runDir( "Bare_run" ) + "/Bare_run.2401011700.RA.1.A.280.xml",
              bareEdit, US_Util::new_guid() );

   US_DataCatalog catalog;
   QString        error;
   ASSERT_TRUE( catalog.open( US_DataCatalog::Disk, QString(), error ) );
   ASSERT_TRUE( catalog.loadRuns( error ) );
   ASSERT_EQ  ( catalog.runCount(), 0 );

   ASSERT_TRUE( catalog.loadAll( error ) ) << error.toStdString();

   const US_DataCatalog::Orphans& orphans = catalog.orphans();
   ASSERT_EQ( orphans.edits.size(), 1 );
   EXPECT_EQ( orphans.edits[ 0 ].guid, bareEdit );
   EXPECT_EQ( orphans.models.size(), 2 );     // the fixture's two models
   EXPECT_TRUE( orphans.noises.isEmpty() );   // its noise is under its model

   int noises = 0;
   for ( const US_DataCatalog::Model& mm : orphans.models )
      noises += mm.noises.size();
   EXPECT_EQ( noises, 1 );

   // and with nothing in the store at all, there is nothing
   QDir( US_Settings::dataDir()   ).removeRecursively();
   QDir( US_Settings::resultDir() ).removeRecursively();
   QDir().mkpath( US_Settings::resultDir() );
   ASSERT_TRUE( catalog.loadRuns( error ) );
   ASSERT_TRUE( catalog.loadAll ( error ) ) << error.toStdString();
   EXPECT_TRUE( catalog.orphans().isEmpty() );
}

// Where the file system does not tell "RunA" from "runA" neither does the
// catalog: the directory is the run's, and the files are found whatever
// the case of their suffix.  Where it does, the names must match.
TEST_F( TestUSDataCatalog, FileNamesCompareAsTheFileSystemDoes )
{
   const QString runID = "Case_run";
   QString       dir   = runDir( "case_RUN" );
   QString       raw   = US_Util::new_guid();

   writeAuc ( dir + "/" + runID + ".RA.1.A.280.AUC", raw, "upper case suffix" );
   writeEdit( dir + "/" + runID + ".2401011200.RA.1.A.280.XML",
              US_Util::new_guid(), raw );

   for ( Qt::CaseSensitivity cs : { Qt::CaseInsensitive, Qt::CaseSensitive } )
   {
      SCOPED_TRACE( cs == Qt::CaseInsensitive ? "insensitive" : "sensitive" );

      US_DataCatalog catalog;
      QString        error;
      catalog.setFileNameCase( cs );
      EXPECT_EQ( catalog.fileNameCase(), cs );
      ASSERT_TRUE( catalog.open( US_DataCatalog::Disk, QString(), error ) );
      ASSERT_TRUE( catalog.loadRuns( error ) );

      int index = catalog.indexOfRun( runID );

      if ( cs == Qt::CaseSensitive )
      {  // ".AUC" is not ".auc" here, so there is no run in that directory
         EXPECT_LT( index, 0 );
         continue;
      }

      ASSERT_GE  ( index, 0 );
      EXPECT_TRUE( catalog.run( index ).usable ) << catalog.run( index ).problem.toStdString();

      ASSERT_TRUE( catalog.loadRunDetail( index, error ) ) << error.toStdString();
      ASSERT_EQ  ( catalog.run( index ).raws.size(), 1 );
      EXPECT_EQ  ( catalog.run( index ).raws[ 0 ].edits.size(), 1 );
   }

   // with exact names, a directory that differs from its run only in case
   // is still not the run's own
   QString dir2 = runDir( "other_RUN" );
   writeAuc( dir2 + "/Other_run.RA.1.A.280.auc", US_Util::new_guid(), "x" );

   US_DataCatalog exact;
   QString        error;
   exact.setFileNameCase( Qt::CaseSensitive );
   ASSERT_TRUE( exact.open( US_DataCatalog::Disk, QString(), error ) );
   ASSERT_TRUE( exact.loadRuns( error ) );
   ASSERT_GE  ( exact.indexOfRun( "Other_run" ), 0 );
   EXPECT_FALSE( exact.run( exact.indexOfRun( "Other_run" ) ).usable );

   US_DataCatalog folded;
   folded.setFileNameCase( Qt::CaseInsensitive );
   ASSERT_TRUE( folded.open( US_DataCatalog::Disk, QString(), error ) );
   ASSERT_TRUE( folded.loadRuns( error ) );
   ASSERT_GE  ( folded.indexOfRun( "Other_run" ), 0 );
   EXPECT_TRUE( folded.run( folded.indexOfRun( "Other_run" ) ).usable );

   // and a run is found by its ID as the file system compares names
   EXPECT_EQ( folded.indexOfRun( "OTHER_RUN" ), folded.indexOfRun( "Other_run" ) );
   EXPECT_LT( exact .indexOfRun( "OTHER_RUN" ), 0 );
}

// A listing for one project says nothing about the runs of another: their
// copies are not listed, so no warning names them
TEST_F( TestUSDataCatalog, CopiesOutsideTheProjectAreNotWarnedAbout )
{
   const QString foreign = "Foreign_run";
   const QString mine    = projectGUID;
   projectGUID           = US_Util::new_guid();   // the other project's

   for ( const QString& name : { foreign, "Copy_" + foreign } )
   {
      QString dir = runDir( name );
      writeAuc( dir + "/" + foreign + ".RA.1.A.280.auc", US_Util::new_guid(), name );
      writeExperimentXml( dir + "/" + foreign + ".RA.xml", foreign, US_Util::new_guid() );
   }
   const QString other = projectGUID;
   projectGUID         = mine;

   for ( const QString& filter : { mine, other } )
   {
      SCOPED_TRACE( filter == mine ? "this project" : "the copy's project" );

      US_DataCatalog catalog;
      QString        error;
      QStringList    messages;
      QObject::connect( &catalog, &US_DataCatalog::message,
                        [ &messages ]( const QString& text )
                        { messages << text; } );

      ASSERT_TRUE( catalog.open( US_DataCatalog::Disk, QString(), error ) );
      ASSERT_TRUE( catalog.loadRuns( filter, error ) ) << error.toStdString();

      const QStringList warned = messages.filter( "copy of run " + foreign );

      if ( filter == mine )
      {
         EXPECT_LT( catalog.indexOfRun( foreign ), 0 );
         EXPECT_TRUE( warned.isEmpty() ) << warned.join( "\n" ).toStdString();
      }
      else
      {  // listed, the copy is still warned about
         EXPECT_GE( catalog.indexOfRun( foreign ), 0 );
         EXPECT_EQ( warned.size(), 1 ) << messages.join( "\n" ).toStdString();
      }
   }
}

// Where file names compare without case, so do run IDs: files of "FOLD_RUN"
// in another directory are a copy of run "Fold_run", whose directory is
// there, and the run ID in any case finds the run, not the copy
TEST_F( TestUSDataCatalog, ACopyInAnotherCaseIsACopyWhereCaseIsNotTold )
{
   const QString runID = "Fold_run";
   writeAuc( runDir( runID ) + "/" + runID + ".RA.1.A.280.auc", US_Util::new_guid(), "the run" );
   writeAuc( runDir( "Copy_fold" ) + "/FOLD_RUN.RA.1.A.280.auc", US_Util::new_guid(), "the copy" );

   US_DataCatalog catalog;
   QString        error;
   catalog.setFileNameCase( Qt::CaseInsensitive );
   ASSERT_TRUE( catalog.open( US_DataCatalog::Disk, QString(), error ) );
   ASSERT_TRUE( catalog.loadRuns( error ) );

   int realIx = -1;
   int copyIx = -1;

   for ( int ii = 0; ii < catalog.runCount(); ii++ )
   {
      if ( catalog.run( ii ).dirPath == runDir( runID ) )      realIx = ii;
      if ( catalog.run( ii ).dirPath == runDir( "Copy_fold" ) ) copyIx = ii;
   }

   ASSERT_GE( realIx, 0 );
   ASSERT_GE( copyIx, 0 );

   EXPECT_TRUE ( catalog.run( realIx ).usable ) << catalog.run( realIx ).problem.toStdString();
   EXPECT_FALSE( catalog.run( copyIx ).usable );
   EXPECT_EQ   ( catalog.run( copyIx ).copyOf, runDir( runID ) );
   EXPECT_TRUE ( catalog.run( copyIx ).problem.contains( "copy of run" ) )
      << catalog.run( copyIx ).problem.toStdString();

   // the copy's own spelling finds the run as well
   EXPECT_EQ( catalog.indexOfRun( runID ), realIx );
   EXPECT_EQ( catalog.indexOfRun( "FOLD_RUN" ), realIx );
   EXPECT_EQ( catalog.indexOfRun( "fold_run" ), realIx );
}

// Where file names compare without case, files of "Spell_run" and of
// "SPELL_RUN" in one directory are one run, not two; where they do not,
// the directory holds two runs
TEST_F( TestUSDataCatalog, FilesOfOneRunInTwoSpellingsAreOneRunWhereCaseIsNotTold )
{
   const QString runID = "Spell_run";
   const QString dir   = runDir( runID );
   writeAuc( dir + "/" + runID + ".RA.1.A.280.auc", US_Util::new_guid(), "own spelling" );
   writeAuc( dir + "/SPELL_RUN.RA.1.A.281.auc", US_Util::new_guid(), "other spelling" );
   writeExperimentXml( dir + "/" + runID + ".RA.xml", runID, US_Util::new_guid() );

   for ( Qt::CaseSensitivity cs : { Qt::CaseInsensitive, Qt::CaseSensitive } )
   {
      SCOPED_TRACE( cs == Qt::CaseInsensitive ? "insensitive" : "sensitive" );

      US_DataCatalog catalog;
      QString        error;
      catalog.setFileNameCase( cs );
      ASSERT_TRUE( catalog.open( US_DataCatalog::Disk, QString(), error ) );
      ASSERT_TRUE( catalog.loadRuns( error ) ) << error.toStdString();

      const int index = catalog.indexOfRun( runID );
      ASSERT_GE( index, 0 );
      const US_DataCatalog::Run& run = catalog.run( index );
      EXPECT_EQ( run.rawCount, 2 );

      if ( cs == Qt::CaseInsensitive )
      {
         EXPECT_TRUE ( run.usable ) << run.problem.toStdString();
         EXPECT_FALSE( run.problem.contains( "more than one run" ) ) << run.problem.toStdString();
      }
      else
      {
         EXPECT_FALSE( run.usable );
         EXPECT_TRUE ( run.problem.contains( "more than one run" ) ) << run.problem.toStdString();
      }
   }
}

// The listing's model and noise counts compare run IDs as the directories
// do: where case is not told, a record whose description spells the run
// in another case is the run's, and a copy in another case does not split
// the count
TEST_F( TestUSDataCatalog, ModelAndNoiseCountsCompareRunIdsAsTheFileSystemDoes )
{
   const QString runID = "Tally_run";
   writeAuc( runDir( runID ) + "/" + runID + ".RA.1.A.280.auc", US_Util::new_guid(), "the run" );
   writeAuc( runDir( "Copy_tally" ) + "/TALLY_RUN.RA.1.A.280.auc", US_Util::new_guid(), "the copy" );

   int file = 10;
   for ( const QString& spelling : { runID, QString( "TALLY_RUN" ), QString( "tally_run" ) } )
   {
      US_Model model;
      model.modelGUID   = US_Util::new_guid();
      model.editGUID    = US_Util::new_guid();
      model.description = spelling + ".1A280.e1_a1_2DSA_i01.model";
      model.write( US_Settings::dataDir() + QString( "/models/M%1.xml" ).arg( file, 7, 10, QChar( '0' ) ) );

      US_Noise noise;
      noise.noiseGUID   = US_Util::new_guid();
      noise.modelGUID   = model.modelGUID;
      noise.description = spelling + ".1A280.2dsa.ti_noise";
      noise.type        = US_Noise::TI;
      noise.values << 0.001 << 0.002;
      noise.count  = 2;
      noise.write( US_Settings::dataDir() + QString( "/noises/N%1.xml" ).arg( file, 7, 10, QChar( '0' ) ) );
      ++file;
   }

   for ( Qt::CaseSensitivity cs : { Qt::CaseInsensitive, Qt::CaseSensitive } )
   {
      SCOPED_TRACE( cs == Qt::CaseInsensitive ? "insensitive" : "sensitive" );

      US_DataCatalog catalog;
      QString        error;
      catalog.setFileNameCase( cs );
      ASSERT_TRUE( catalog.open( US_DataCatalog::Disk, QString(), error ) );
      ASSERT_TRUE( catalog.loadRuns( error ) ) << error.toStdString();

      int realIx = -1;
      int copyIx = -1;
      for ( int ii = 0; ii < catalog.runCount(); ii++ )
      {
         if ( catalog.run( ii ).dirPath == runDir( runID ) )        realIx = ii;
         if ( catalog.run( ii ).dirPath == runDir( "Copy_tally" ) ) copyIx = ii;
      }
      ASSERT_GE( realIx, 0 );
      ASSERT_GE( copyIx, 0 );

      // All three spellings, or only the exact one each
      const int expected = cs == Qt::CaseInsensitive ? 3 : 1;
      EXPECT_EQ( catalog.run( realIx ).modelCount, expected );
      EXPECT_EQ( catalog.run( realIx ).noiseCount, expected );
      EXPECT_EQ( catalog.run( copyIx ).modelCount, expected );
      EXPECT_EQ( catalog.run( copyIx ).noiseCount, expected );
   }
}

// A stray file of another run that sorts first does not make a run's own
// directory a copy of that other run: the directory is still its run, not
// usable for holding two runs, and nothing is called a copy
TEST_F( TestUSDataCatalog, AStrayFileDoesNotMakeARunACopyOfAnother )
{
   const QString stray = "AAA_run";
   const QString runID = "Mixed_run";
   writeAuc( runDir( stray ) + "/" + stray + ".RA.1.A.280.auc", US_Util::new_guid(), "stray's own" );
   writeAuc( runDir( runID ) + "/" + stray + ".RA.1.A.280.auc", US_Util::new_guid(), "stray" );
   writeAuc( runDir( runID ) + "/" + runID + ".RA.1.A.280.auc", US_Util::new_guid(), "own" );
   writeExperimentXml( runDir( runID ) + "/" + runID + ".RA.xml", runID, US_Util::new_guid() );

   US_DataCatalog catalog;
   QString        error;
   QStringList    messages;
   QObject::connect( &catalog, &US_DataCatalog::message,
                     [ &messages ]( const QString& text )
                     { messages << text; } );

   ASSERT_TRUE( catalog.open( US_DataCatalog::Disk, QString(), error ) );
   ASSERT_TRUE( catalog.loadRuns( error ) );

   const int index = catalog.indexOfRun( runID );
   ASSERT_GE( index, 0 ) << "the run dropped out of the listing";

   const US_DataCatalog::Run& run = catalog.run( index );
   EXPECT_EQ   ( run.dirPath, runDir( runID ) );
   EXPECT_TRUE ( run.copyOf.isEmpty() ) << run.copyOf.toStdString();
   EXPECT_EQ   ( run.runType, QString( "RA" ) );
   EXPECT_FALSE( run.usable );
   EXPECT_TRUE ( run.problem.contains( "more than one run" ) ) << run.problem.toStdString();
   EXPECT_FALSE( run.problem.contains( "copy" ) ) << run.problem.toStdString();

   // and the stray's own run is untouched
   const int strayIx = catalog.indexOfRun( stray );
   ASSERT_GE  ( strayIx, 0 );
   EXPECT_EQ  ( catalog.run( strayIx ).dirPath, runDir( stray ) );
   EXPECT_TRUE( catalog.run( strayIx ).usable ) << catalog.run( strayIx ).problem.toStdString();

   EXPECT_TRUE( messages.filter( "copy of run" ).isEmpty() ) << messages.join( "\n" ).toStdString();
}

// A file that cannot be read has no checksum, and saying it has checksum
// "0" would make it look like a copy that differs.  The read fails instead,
// and the experiment stays as it was.
TEST_F( TestUSDataCatalog, AFileThatCannotBeHashedFailsTheRead )
{
   for ( const QString& gone : { QString( "/noises/N0000001.xml" ),
                                 QString( "/models/M0000001.xml" ) } )
   {
      SCOPED_TRACE( gone.toStdString() );

      US_DataCatalog catalog;
      QString        error;
      ASSERT_TRUE( catalog.open( US_DataCatalog::Disk, QString(), error ) );
      ASSERT_TRUE( catalog.loadRuns( error ) );

      int one = catalog.indexOfRun( runOne );
      ASSERT_GE( one, 0 );

      // listed, and indexed, while it was there; gone before it is hashed
      const QString path = US_Settings::dataDir() + gone;
      const QString keep = path + ".kept";
      ASSERT_TRUE( QFile::rename( path, keep ) );

      EXPECT_FALSE( catalog.loadRunDetail( one, error ) );
      EXPECT_TRUE ( error.contains( path ) ) << error.toStdString();
      EXPECT_FALSE( catalog.isRunLoaded( one ) );

      if ( gone.startsWith( "/noises" ) )
      {
         EXPECT_TRUE( catalog.noisesOfModel( modelGUID, error ).isEmpty() );
         EXPECT_TRUE( error.contains( path ) ) << error.toStdString();
      }

      // without checksums nothing is hashed, so nothing fails
      catalog.setChecksums( false );
      EXPECT_TRUE( catalog.loadRunDetail( one, error ) ) << error.toStdString();

      // and a verify that cannot hash the file fails, and changes nothing
      EXPECT_FALSE( catalog.verifyRun( one, error ) );
      EXPECT_TRUE ( error.contains( path ) ) << error.toStdString();
      EXPECT_FALSE( catalog.isRunVerified( one ) );

      ASSERT_TRUE( QFile::rename( keep, path ) );
   }
}
