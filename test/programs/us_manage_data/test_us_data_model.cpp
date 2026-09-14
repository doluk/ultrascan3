// test_us_data_model.cpp - the layered scan behind us_manage_data
//
// The window is driven in two layers: scan_runs() lists the experiments and
// scan_run() reads the chain of one of them.  These tests build a synthetic
// local store and check that the layers add up, that reading one experiment
// leaves the rows of the others where they were, and that an experiment the
// user opens jumps the queue.
#include <gtest/gtest.h>

#include <QtCore>
#include <QProgressBar>
#include <QLabel>

#include "us_data_model.h"
#include "us_settings.h"
#include "us_util.h"
#include "us_dataIO.h"
#include "us_model.h"
#include "us_noise.h"

class TestUSDataModel : public ::testing::Test
{
   protected:
      QString       root;
      QString       savedWorkDir;
      QProgressBar* bar;
      QLabel*       label;

      QString runOne;
      QString runTwo;
      QString runThree;
      QString rawOneGUID;
      QString rawTwoGUID;
      QString editOneGUID;
      QString editTwoGUID;
      QString modelGUID;
      QString noiseGUID;

      void SetUp() override
      {
         savedWorkDir = US_Settings::workBaseDir();

         root = QDir::tempPath() + "/us_data_model_test_"
                + QString::number( QCoreApplication::applicationPid() ) + "_"
                + QString::number( QDateTime::currentMSecsSinceEpoch() );
         QDir().mkpath( root );
         US_Settings::set_workBaseDir( root );

         QDir().mkpath( US_Settings::dataDir() + "/models" );
         QDir().mkpath( US_Settings::dataDir() + "/noises" );

         runOne      = "Model_run_one";
         runTwo      = "Model.run.two";     // a run identifier with dots in it
         runThree    = "Model_run_three";
         rawOneGUID  = US_Util::new_guid();
         rawTwoGUID  = US_Util::new_guid();
         editOneGUID = US_Util::new_guid();
         editTwoGUID = US_Util::new_guid();
         modelGUID   = US_Util::new_guid();
         noiseGUID   = US_Util::new_guid();

         buildStore();

         bar   = new QProgressBar();
         label = new QLabel();
      }

      void TearDown() override
      {
         delete bar;
         delete label;

         US_Settings::set_workBaseDir( savedWorkDir );
         QDir( root ).removeRecursively();
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

      void buildStore()
      {
         QString dir = runDir( runOne );
         writeAuc ( dir + "/" + runOne + ".RA.1.A.280.auc", rawOneGUID, "one" );
         writeAuc ( dir + "/" + runOne + ".RA.1.A.281.auc", rawTwoGUID, "two" );
         writeEdit( dir + "/" + runOne + ".2401011200.RA.1.A.280.xml",
                    editOneGUID, rawOneGUID );
         writeEdit( dir + "/" + runOne + ".2401011300.RA.1.A.280.xml",
                    editTwoGUID, rawOneGUID );

         dir = runDir( runTwo );
         writeAuc ( dir + "/" + runTwo + ".RA.2.B.280.auc",
                    US_Util::new_guid(), "second run" );

         dir = runDir( runThree );
         writeAuc ( dir + "/" + runThree + ".RA.3.A.280.auc",
                    US_Util::new_guid(), "third run" );

         US_Model model;
         model.modelGUID   = modelGUID;
         model.editGUID    = editOneGUID;
         model.description = runOne + ".1A280.2dsa.model";
         model.write( US_Settings::dataDir() + "/models/M0000001.xml" );

         US_Noise noise;
         noise.noiseGUID   = noiseGUID;
         noise.modelGUID   = modelGUID;
         noise.description = runOne + ".1A280.2dsa.ti_noise";
         noise.type        = US_Noise::TI;
         noise.values << 0.001 << 0.002;
         noise.count  = 2;
         noise.write( US_Settings::dataDir() + "/noises/N0000001.xml" );
      }

      void prepare( US_DataModel& model )
      {
         model.setProgress( bar, label );
         model.setFilters ( "ALL", "ALL", "Local Only" );
      }

      int countOfType( US_DataModel& model, int recType )
      {
         int knt = 0;

         for ( int ii = 0; ii < model.recCount(); ii++ )
            if ( model.row_datadesc( ii ).recType == recType )   knt++;

         return knt;
      }
};

TEST_F( TestUSDataModel, FirstLayerIsOneRowPerExperiment )
{
   US_DataModel model;
   prepare( model );

   model.scan_runs();

   EXPECT_EQ( model.runCount(), 3 );
   EXPECT_EQ( model.recCount(), 3 );   // nothing below the experiments yet

   for ( int ii = 0; ii < model.runCount(); ii++ )
   {
      US_DataModel::RunEntry entry = model.run_entry( ii );

      EXPECT_FALSE( entry.loaded );
      EXPECT_EQ   ( entry.row, ii );
      EXPECT_EQ   ( model.row_datadesc( entry.row ).recType,
                    (int)US_DataModel::EXPERIMENT );
      EXPECT_EQ   ( model.row_datadesc( entry.row ).label.toStdString(),
                    entry.runID.toStdString() );
   }

   // a run identifier with dots in it survives the scan intact
   EXPECT_GE( model.index_of_run( runTwo ), 0 );
   EXPECT_EQ( model.pending_runs(), 3 );
}

TEST_F( TestUSDataModel, TheFirstLayerCountsWhatIsUnderAnExperiment )
{
   US_DataModel model;
   prepare( model );

   model.scan_runs();

   int index = model.index_of_run( runOne );
   ASSERT_GE( index, 0 );

   // two triples, two edits, one model, one noise
   EXPECT_EQ( model.run_entry( index ).loCount, 6 );
   EXPECT_EQ( model.run_entry( index ).dbCount, 0 );

   index = model.index_of_run( runThree );
   ASSERT_GE( index, 0 );
   EXPECT_EQ( model.run_entry( index ).loCount, 1 );
}

TEST_F( TestUSDataModel, SecondLayerAppendsTheChainOfOneExperiment )
{
   US_DataModel model;
   prepare( model );

   model.scan_runs();

   int index = model.index_of_run( runOne );
   ASSERT_GE( index, 0 );

   ASSERT_TRUE( model.scan_run( index ) );

   US_DataModel::RunEntry entry = model.run_entry( index );

   ASSERT_TRUE( entry.loaded );
   ASSERT_GE  ( entry.firstRow, 3 );          // appended after every header
   EXPECT_EQ  ( entry.lastRow, model.recCount() - 1 );

   EXPECT_EQ( countOfType( model, US_DataModel::RAW   ), 2 );
   EXPECT_EQ( countOfType( model, US_DataModel::EDIT  ), 2 );
   EXPECT_EQ( countOfType( model, US_DataModel::MODEL ), 1 );
   EXPECT_EQ( countOfType( model, US_DataModel::NOISE ), 1 );

   // the chain is in tree order: a raw, then its edits, then their models
   int    depth = 0;
   QString first;

   for ( int row = entry.firstRow; row <= entry.lastRow; row++ )
   {
      US_DataModel::DataDesc desc = model.row_datadesc( row );

      if ( row == entry.firstRow )
      {
         EXPECT_EQ( desc.recType, (int)US_DataModel::RAW );
         first = desc.dataGUID;
      }

      EXPECT_LE( desc.recType, depth + 1 );   // never skips a level
      depth = desc.recType;

      EXPECT_EQ( desc.recState, (int)US_DataModel::REC_LO | desc.recState );
   }

   EXPECT_EQ( first.toStdString(), rawOneGUID.toStdString() );
}

TEST_F( TestUSDataModel, ReadingOneExperimentLeavesTheOtherRowsAlone )
{
   US_DataModel model;
   prepare( model );

   model.scan_runs();

   QVector< int >     rows;
   QVector< QString > labels;

   for ( int ii = 0; ii < model.runCount(); ii++ )
   {
      rows   << model.run_entry( ii ).row;
      labels << model.row_datadesc( model.run_entry( ii ).row ).label;
   }

   for ( int ii = 0; ii < model.runCount(); ii++ )
   {
      ASSERT_TRUE( model.scan_run( ii ) );

      // every experiment row is still where the tree put it
      for ( int jj = 0; jj < model.runCount(); jj++ )
      {
         EXPECT_EQ( model.run_entry( jj ).row, rows[ jj ] );
         EXPECT_EQ( model.row_datadesc( rows[ jj ] ).label.toStdString(),
                    labels[ jj ].toStdString() );
      }
   }

   EXPECT_EQ( model.pending_runs(), 0 );
   EXPECT_EQ( countOfType( model, US_DataModel::RAW ), 4 );
   EXPECT_EQ( model.recCountLoc(), model.recCount() - model.runCount() );
}

TEST_F( TestUSDataModel, AnOpenedExperimentJumpsTheQueue )
{
   US_DataModel model;
   prepare( model );

   model.scan_runs();

   int last = model.runCount() - 1;
   ASSERT_GT( last, 0 );
   ASSERT_NE( model.next_pending_run(), last );

   model.request_run( last );
   EXPECT_EQ( model.next_pending_run(), last );

   ASSERT_TRUE( model.scan_run( last ) );
   EXPECT_NE  ( model.next_pending_run(), last );
   EXPECT_EQ  ( model.pending_runs(), model.runCount() - 1 );

   // asking again for an experiment already read does not queue it
   model.request_run( last );
   EXPECT_EQ( model.pending_runs(), model.runCount() - 1 );
}

TEST_F( TestUSDataModel, ReadingAnExperimentTwiceChangesNothing )
{
   US_DataModel model;
   prepare( model );

   model.scan_runs();
   ASSERT_TRUE( model.scan_run( 0 ) );

   int rows = model.recCount();

   ASSERT_TRUE( model.scan_run( 0 ) );
   EXPECT_EQ  ( model.recCount(), rows );
}

TEST_F( TestUSDataModel, TheRunFilterNarrowsTheScanToOneExperiment )
{
   US_DataModel model;
   model.setProgress( bar, label );
   model.setFilters ( runTwo, "ALL", "Local Only" );

   model.scan_runs();

   ASSERT_EQ( model.runCount(), 1 );
   EXPECT_EQ( model.run_entry( 0 ).runID.toStdString(), runTwo.toStdString() );

   ASSERT_TRUE( model.scan_run( 0 ) );
   EXPECT_EQ  ( countOfType( model, US_DataModel::RAW ), 1 );
}

TEST_F( TestUSDataModel, ExcludingLocalOnlyTreesLeavesNothingOfALocalStore )
{
   US_DataModel model;
   model.setProgress( bar, label );
   model.setFilters ( "ALL", "ALL", "Exclude Local-Only Trees" );

   model.scan_runs();

   EXPECT_EQ( model.runCount(), 0 );
   EXPECT_EQ( model.recCount(), 0 );
}

// -------------------------------------------------------- the bulk second layer

// Reading the whole store at once has to put the same rows in the tree as
// reading one experiment at a time does.
TEST_F( TestUSDataModel, ScanAllGivesTheSameRowsAsOneAtATime )
{
   US_DataModel oneAtATime;
   US_DataModel allAtOnce;

   prepare( oneAtATime );
   prepare( allAtOnce  );

   oneAtATime.scan_runs();
   allAtOnce .scan_runs();

   for ( int ii = 0; ii < oneAtATime.runCount(); ii++ )
      ASSERT_TRUE( oneAtATime.scan_run( ii ) );

   ASSERT_TRUE( allAtOnce.scan_all() );

   for ( int ii = 0; ii < allAtOnce.runCount(); ii++ )
      ASSERT_TRUE( allAtOnce.merge_run( ii ) );

   ASSERT_EQ( allAtOnce.runCount(), oneAtATime.runCount() );
   ASSERT_EQ( allAtOnce.recCount(), oneAtATime.recCount() );

   for ( int row = 0; row < allAtOnce.recCount(); row++ )
   {
      US_DataModel::DataDesc all = allAtOnce .row_datadesc( row );
      US_DataModel::DataDesc one = oneAtATime.row_datadesc( row );

      EXPECT_EQ( all.recType, one.recType );
      EXPECT_EQ( all.label.toStdString(), one.label.toStdString() );
      EXPECT_EQ( all.dataGUID.toStdString(), one.dataGUID.toStdString() );
   }

   EXPECT_EQ( allAtOnce.recCountLoc(), oneAtATime.recCountLoc() );
}

// A local-only store has nothing to compare against, so there is nothing
// worth verifying and no file is read to find that out.
TEST_F( TestUSDataModel, NothingIsVerifiedWhenOnlyOneSourceHoldsTheData )
{
   US_DataModel model;
   prepare( model );

   model.scan_runs();
   ASSERT_TRUE( model.scan_all() );

   for ( int ii = 0; ii < model.runCount(); ii++ )
      ASSERT_TRUE( model.merge_run( ii ) );

   EXPECT_EQ( model.queue_verifies(), 0 );
   EXPECT_EQ( model.pending_verifies(), 0 );

   for ( int ii = 0; ii < model.runCount(); ii++ )
   {
      EXPECT_FALSE( model.run_verified( ii ) );
      EXPECT_FALSE( model.run_entry( ii ).inBoth() );
   }

   // and asking for one anyway is refused rather than wasted
   model.request_verify( 0 );
   EXPECT_EQ( model.next_pending_verify(), -1 );
}

// Listing a store does not read its files, so a row carries no checksum
// until its experiment has been verified.
TEST_F( TestUSDataModel, ListingCarriesNoChecksums )
{
   US_DataModel model;
   prepare( model );

   model.scan_runs();
   ASSERT_TRUE( model.scan_all() );

   for ( int ii = 0; ii < model.runCount(); ii++ )
      ASSERT_TRUE( model.merge_run( ii ) );

   int rows = 0;

   for ( int row = 0; row < model.recCount(); row++ )
   {
      US_DataModel::DataDesc desc = model.row_datadesc( row );

      if ( desc.recType == US_DataModel::EXPERIMENT )  continue;

      EXPECT_TRUE( desc.contents.isEmpty() )
         << desc.label.toStdString() << ": " << desc.contents.toStdString();
      rows++;
   }

   EXPECT_GT( rows, 0 );
}
