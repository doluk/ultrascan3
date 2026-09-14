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

   // and a fresh scan picks it up
   catalog.clear();
   ASSERT_TRUE( catalog.loadRuns( error ) );
   EXPECT_EQ( catalog.runCount(), 3 );
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
