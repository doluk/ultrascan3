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
#include <QTreeWidget>

#include "us_data_model.h"
#include "us_data_process.h"
#include "us_data_tree.h"
#include "us_settings.h"
#include "us_util.h"
#include "us_dataIO.h"
#include "us_model.h"
#include "us_noise.h"

// The names of the files in a directory that match a pattern
static QStringList filesIn( const QString& dir, const QString& pattern )
{
   return QDir( dir ).entryList( QStringList( pattern ), QDir::Files );
}

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

   // The records are appended after every experiment row, so they are NOT
   // the rows that follow their experiment's row: what is under an
   // experiment is its own row range, and only that
   ASSERT_GE  ( entry.firstRow, 3 );
   EXPECT_EQ  ( entry.lastRow, model.recCount() - 1 );
   EXPECT_EQ  ( model.row_datadesc( entry.row + 1 ).recType,
                (int)US_DataModel::EXPERIMENT );

   QVector< int > below = model.descendant_rows( entry.row );
   ASSERT_EQ( below.size(), entry.lastRow - entry.firstRow + 1 );
   EXPECT_EQ( below.first(), entry.firstRow );
   EXPECT_EQ( below.last(),  entry.lastRow );

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

// -------------------------------------------------------- experiment ranges
//
// The experiment rows are rows 0..N-1 and each experiment's records are
// appended after all of them.  Everything that acts on "an experiment and
// what is under it" has to use the experiment's own row range: walking down
// from the experiment row reaches the next experiment instead (first and
// middle experiments), or every experiment's records (the last one).

TEST_F( TestUSDataModel, EachExperimentOwnsExactlyItsRowRange )
{
   // Give the first experiment an edit of its own too
   QString dir = runDir( runTwo );
   QString rawGUID = US_Util::new_guid();
   writeAuc ( dir + "/" + runTwo + ".RA.2.B.290.auc", rawGUID, "two" );
   writeEdit( dir + "/" + runTwo + ".2401021200.RA.2.B.290.xml",
              US_Util::new_guid(), rawGUID );

   US_DataModel model;
   prepare( model );
   model.scan_data();

   ASSERT_EQ( model.runCount(), 3 );

   int covered = 0;

   for ( int ii = 0; ii < model.runCount(); ii++ )
   {
      SCOPED_TRACE( "experiment " + std::to_string( ii ) );
      US_DataModel::RunEntry entry = model.run_entry( ii );

      ASSERT_TRUE( entry.loaded );
      ASSERT_GE  ( entry.firstRow, model.runCount() );

      QVector< int > rows = model.descendant_rows( entry.row );
      ASSERT_EQ( rows.size(), entry.lastRow - entry.firstRow + 1 );

      for ( int jj = 0; jj < rows.size(); jj++ )
      {
         EXPECT_EQ( rows[ jj ], entry.firstRow + jj );
         EXPECT_EQ( model.run_of_row( rows[ jj ] ), ii );
         EXPECT_NE( model.row_datadesc( rows[ jj ] ).recType,
                    (int)US_DataModel::EXPERIMENT );
      }

      EXPECT_EQ( model.run_of_row( entry.row ), ii );
      EXPECT_EQ( model.run_id_of_row( entry.firstRow ).toStdString(),
                 entry.runID.toStdString() );
      covered += rows.size();
   }

   // every record row belongs to exactly one experiment
   EXPECT_EQ( covered, model.recCount() - model.runCount() );
}

TEST_F( TestUSDataModel, ParentsAndDescendantsStayInsideTheExperiment )
{
   US_DataModel model;
   prepare( model );
   model.scan_data();

   US_DataModel::RunEntry entry = model.run_entry( model.index_of_run( runOne ) );
   ASSERT_TRUE( entry.loaded );

   for ( int row = entry.firstRow; row <= entry.lastRow; row++ )
   {
      US_DataModel::DataDesc desc = model.row_datadesc( row );
      int prow = model.parent_row( row );

      if ( desc.recType == US_DataModel::RAW )
      {
         EXPECT_EQ( prow, entry.row );
         continue;
      }

      ASSERT_GE( prow, entry.firstRow );
      EXPECT_EQ( model.row_datadesc( prow ).recType, desc.recType - 1 );
      EXPECT_EQ( model.row_datadesc( prow ).dataGUID.toStdString(),
                 desc.parentGUID.toStdString() );
      EXPECT_TRUE( model.descendant_rows( prow ).contains( row ) );
   }

   // the raw with the edits has the edits, the model and the noise below it
   int nraw = 0;

   for ( int row = entry.firstRow; row <= entry.lastRow; row++ )
   {
      if ( model.row_datadesc( row ).dataGUID != rawOneGUID )   continue;

      nraw++;
      EXPECT_EQ( model.descendant_rows( row ).size(), 4 );
   }

   EXPECT_EQ( nraw, 1 );
}

// ------------------------------------------------------- edits in the tree

TEST_F( TestUSDataModel, AnEditIsLabelledAndTypedByItsFileName )
{
   US_DataModel model;
   prepare( model );
   model.scan_data();

   int nedit = 0;

   for ( int row = 0; row < model.recCount(); row++ )
   {
      US_DataModel::DataDesc desc = model.row_datadesc( row );

      if ( desc.recType != US_DataModel::EDIT )   continue;

      nedit++;
      EXPECT_EQ( desc.subType.toStdString(), "RA" );   // the run type
      EXPECT_TRUE( desc.label.startsWith( runOne + ".24010" ) )
         << desc.label.toStdString();
      EXPECT_TRUE( desc.label.endsWith( ".1.A.280" ) )
         << desc.label.toStdString();
   }

   EXPECT_EQ( nedit, 2 );
}

// A large local model is a custom grid even when the listing read no sizes
TEST_F( TestUSDataModel, ALargeLocalModelIsACustomGrid )
{
   US_Model big;
   big.modelGUID   = US_Util::new_guid();
   big.editGUID    = editTwoGUID;
   big.description = runOne + ".1A280.e2401011300_a2401011400_2DSA_i01.model";

   for ( int ii = 0; ii < 1000; ii++ )
   {
      US_Model::SimulationComponent comp;
      comp.s    = 1.0e-13 * ( ii + 1 );
      comp.f_f0 = 1.25;
      comp.name = QString( "component number %1" ).arg( ii );
      big.components << comp;
   }

   QString path = US_Settings::dataDir() + "/models/M0000009.xml";
   ASSERT_EQ( big.write( path ), US_DB2::OK );
   ASSERT_GT( QFileInfo( path ).size(), 65000 );

   US_DataModel model;
   prepare( model );
   model.scan_data();

   bool found = false;

   for ( int row = 0; row < model.recCount(); row++ )
   {
      US_DataModel::DataDesc desc = model.row_datadesc( row );

      if ( desc.dataGUID != big.modelGUID )   continue;

      found = true;
      EXPECT_TRUE( desc.contents.isEmpty() );        // no checksum was read
      EXPECT_EQ  ( desc.subType.toStdString(), "CUSTOMGRID" );
   }

   EXPECT_TRUE( found );
}

// -------------------------------------------------------- duplicates

TEST_F( TestUSDataModel, DuplicateGuidsAreFoundOnceOverTheWholeTree )
{
   // A second file claiming to be the same model
   US_Model dup;
   ASSERT_EQ( dup.load( US_Settings::dataDir() + "/models/M0000001.xml" ),
              US_DB2::OK );
   ASSERT_EQ( dup.write( US_Settings::dataDir() + "/models/M0000002.xml" ),
              US_DB2::OK );

   US_DataModel model;
   prepare( model );
   model.scan_data();                // no dialog in the middle of the scan

   // every experiment is still merged
   for ( int ii = 0; ii < model.runCount(); ii++ )
      EXPECT_TRUE( model.run_entry( ii ).loaded );

   QList< US_DataModel::Duplicate > dups = model.find_duplicates();
   ASSERT_EQ( dups.size(), 1 );
   EXPECT_EQ( dups[ 0 ].recType, (int)US_DataModel::MODEL );
   EXPECT_EQ( dups[ 0 ].guid.toStdString(), modelGUID.toStdString() );
   EXPECT_EQ( dups[ 0 ].rows.size(), 2 );

   // A record of no experiment with the GUID of a record in an experiment:
   // a duplicate across experiments
   US_DataModel::RunEntry first = model.run_entry( 0 );
   ASSERT_GE( first.firstRow, 0 );

   US_DataModel::DataDesc orphan = model.row_datadesc( first.firstRow );
   orphan.recState = US_DataModel::REC_LO;
   QVector< US_DataModel::DataDesc > orphans;
   orphans << orphan;
   ASSERT_GE( model.add_orphans( QVector< US_DataModel::DataDesc >(),
                                 orphans ), 0 );

   EXPECT_EQ( model.find_duplicates().size(), 2 );
}

// -------------------------------------------------------- re-entrancy
//
// The scan loop processes events between experiments, so the window can be
// used while it runs.  Anything that asks the model to read or compare an
// experiment meanwhile only queues it.

TEST_F( TestUSDataModel, AReadAskedForDuringTheScanIsOnlyQueued )
{
   US_DataModel model;
   prepare( model );

   int  last      = -1;
   int  calls     = 0;
   bool reentered = true;
   bool queued    = false;
   bool loadedThen = true;
   int  rowsThen  = -1;
   int  rowsAfterReset = -2;
   bool activeThen = false;

   QObject::connect( &model, &US_DataModel::runMerged, [ & ]( int )
   {
      if ( calls++ > 0 )   return;

      last       = model.runCount() - 1;
      loadedThen = model.run_entry( last ).loaded;
      rowsThen   = model.recCount();
      reentered  = model.scan_run( last )  ||  model.merge_run( last )  ||
                   model.verify_run( last );
      queued     = ( model.next_pending_run() == last );

      model.dummy_data();            // a Reset is refused under a scan too
      rowsAfterReset = model.recCount();
      activeThen     = model.scan_active();
   } );

   model.scan_data();

   EXPECT_FALSE( loadedThen );
   EXPECT_FALSE( reentered );
   EXPECT_TRUE ( queued );
   EXPECT_TRUE ( activeThen );
   EXPECT_EQ   ( rowsAfterReset, rowsThen );
   EXPECT_FALSE( model.scan_active() );

   // the queued experiment was read once, by the loop, and the ranges hold
   int covered = 0;

   for ( int ii = 0; ii < model.runCount(); ii++ )
   {
      US_DataModel::RunEntry entry = model.run_entry( ii );
      EXPECT_TRUE( entry.loaded );
      covered += model.descendant_rows( entry.row ).size();
   }

   EXPECT_EQ( covered, model.recCount() - model.runCount() );
   EXPECT_EQ( countOfType( model, US_DataModel::RAW ), 4 );
}

// A call that arrives while a merge is in progress -- from an event the
// merge let through -- is queued too, outside any scan
TEST_F( TestUSDataModel, AReadAskedForDuringAMergeIsOnlyQueued )
{
   US_DataModel model;
   prepare( model );

   model.scan_runs();
   ASSERT_TRUE( model.scan_all() );

   int  other     = model.runCount() - 1;
   bool reentered = false;
   bool queued    = false;
   bool fired     = false;

   QObject::connect( bar, &QProgressBar::valueChanged, [ & ]( int )
   {
      if ( fired  ||  ! model.is_busy() )   return;

      fired     = true;
      reentered = model.scan_run( other );
      queued    = ( model.next_pending_run() == other );
   } );

   ASSERT_TRUE( model.merge_run( 0 ) );

   EXPECT_TRUE ( fired );
   EXPECT_FALSE( reentered );
   EXPECT_TRUE ( queued );
   EXPECT_FALSE( model.run_entry( other ).loaded );
   EXPECT_FALSE( model.is_busy() );

   // and it is read when asked again with nothing in progress
   EXPECT_TRUE( model.scan_run( other ) );
   EXPECT_TRUE( model.run_entry( other ).loaded );
}

// The window's loop and the layers driven by hand give the same tree
TEST_F( TestUSDataModel, TheScanLoopGivesTheSameRowsAsTheLayers )
{
   US_DataModel looped;
   US_DataModel layered;
   prepare( looped  );
   prepare( layered );

   QList< int > merged;
   QObject::connect( &looped, &US_DataModel::runMerged,
                     [ & ]( int index ) { merged << index; } );

   looped.scan_data();

   layered.scan_runs();
   ASSERT_TRUE( layered.scan_all() );

   for ( int ii = 0; ii < layered.runCount(); ii++ )
      ASSERT_TRUE( layered.merge_run( ii ) );

   EXPECT_EQ( merged.size(), looped.runCount() );
   ASSERT_EQ( looped.recCount(), layered.recCount() );

   for ( int row = 0; row < looped.recCount(); row++ )
      EXPECT_EQ( looped .row_datadesc( row ).dataGUID.toStdString(),
                 layered.row_datadesc( row ).dataGUID.toStdString() );

   EXPECT_TRUE( looped.scan_errors().isEmpty() );
   EXPECT_FALSE( looped.db_listing_failed() );
}

// -------------------------------------------------------- the selectors

TEST_F( TestUSDataModel, TheSelectorsListRunsAndTriplesFromTheDirectory )
{
   QStringList runs = US_DataModel::local_run_ids();

   EXPECT_TRUE( runs.contains( runOne ) );
   EXPECT_TRUE( runs.contains( runTwo ) );     // a run ID with dots, whole
   EXPECT_TRUE( runs.contains( runThree ) );

   QStringList trips = US_DataModel::local_triples( runOne );
   trips.sort();
   ASSERT_EQ( trips.size(), 2 );
   EXPECT_EQ( trips[ 0 ].toStdString(), "1.A.280" );
   EXPECT_EQ( trips[ 1 ].toStdString(), "1.A.281" );

   trips = US_DataModel::local_triples( runTwo );
   ASSERT_EQ( trips.size(), 1 );
   EXPECT_EQ( trips[ 0 ].toStdString(), "2.B.280" );
}

// -------------------------------------------------------- tree actions
//
// A context menu action on an experiment acts on that experiment's records
// and nothing else -- whichever position the experiment has.

TEST_F( TestUSDataModel, AnActionOnAnExperimentActsOnItsOwnRecordsOnly )
{
   // Records under the first and the last experiment as well
   QString dirTwo   = runDir( runTwo );
   QString dirThree = runDir( runThree );
   QString rawGUID  = US_Util::new_guid();
   writeAuc ( dirTwo + "/" + runTwo + ".RA.2.B.290.auc", rawGUID, "two" );
   writeEdit( dirTwo + "/" + runTwo + ".2401021200.RA.2.B.290.xml",
              US_Util::new_guid(), rawGUID );
   rawGUID          = US_Util::new_guid();
   writeAuc ( dirThree + "/" + runThree + ".RA.3.A.290.auc", rawGUID, "three" );
   writeEdit( dirThree + "/" + runThree + ".2401031200.RA.3.A.290.xml",
              US_Util::new_guid(), rawGUID );

   US_DataModel   model;
   prepare( model );

   QTreeWidget    tw;
   tw.setSelectionMode( QAbstractItemView::ExtendedSelection );
   US_DataProcess proc( &model );
   US_DataTree    tree( &model, &tw );
   model.setSiblings( &proc, &tree );

   model.scan_data();
   tree.build_dtree();

   ASSERT_EQ( model.runCount(), 3 );
   ASSERT_EQ( tw.topLevelItemCount(), 3 );

   // first, middle and last:  the action rows are the experiment's own
   for ( int ii = 0; ii < model.runCount(); ii++ )
   {
      SCOPED_TRACE( "experiment " + std::to_string( ii ) );
      US_DataModel::RunEntry entry = model.run_entry( ii );

      QVector< int > expected;

      for ( int row = entry.firstRow; row >= 0  &&  row <= entry.lastRow; row++ )
         if ( model.row_datadesc( row ).recType > US_DataModel::RAW )
            expected << row;

      ASSERT_FALSE( expected.isEmpty() );

      tw.clearSelection();
      tw.topLevelItem( ii )->setSelected( true );

      EXPECT_EQ( tree.action_row_list(), expected );
   }

   // An experiment and one of its own records selected together: each
   // record is acted on once
   int middle = model.index_of_run( runOne );
   ASSERT_EQ( middle, 1 );
   US_DataModel::RunEntry mentry = model.run_entry( middle );
   QTreeWidgetItem* mitem = tw.topLevelItem( middle );
   ASSERT_GT( mitem->childCount(), 0 );

   tw.clearSelection();
   mitem->setSelected( true );
   mitem->child( 0 )->setSelected( true );
   QVector< int > both = tree.action_row_list();
   QVector< int > uniq = both;
   std::sort( uniq.begin(), uniq.end() );
   uniq.erase( std::unique( uniq.begin(), uniq.end() ), uniq.end() );
   EXPECT_EQ( both, uniq );

   // Remove the middle experiment's local records:  its edits, model and
   // noise go, nobody else's do
   tw.clearSelection();
   mitem->setSelected( true );

   EXPECT_EQ( tree.act_on_selection( "Local disk only", "Local-only remove" ),
              0 ) << proc.lastError().toStdString();

   EXPECT_TRUE ( filesIn( runDir( runOne ), "*.xml" ).isEmpty() );
   EXPECT_EQ   ( filesIn( runDir( runOne ), "*.auc" ).size(), 2 );
   EXPECT_FALSE( QFile::exists( US_Settings::dataDir() + "/models/M0000001.xml" ) );
   EXPECT_FALSE( QFile::exists( US_Settings::dataDir() + "/noises/N0000001.xml" ) );
   EXPECT_EQ   ( filesIn( dirTwo,   "*.xml" ).size(), 1 );
   EXPECT_EQ   ( filesIn( dirThree, "*.xml" ).size(), 1 );

   for ( int row = mentry.firstRow; row <= mentry.lastRow; row++ )
   {
      US_DataModel::DataDesc desc = model.row_datadesc( row );

      if ( desc.recType > US_DataModel::RAW )
      {
         EXPECT_EQ( desc.recState, (int)US_DataModel::NOSTAT );
      }
   }

   // Asked again, there is nothing left to do -- and that is not "Success"
   EXPECT_EQ( tree.act_on_selection( "Local disk only", "Local-only remove" ),
              US_DataTree::ACT_NOTHING );

   // Nothing of a local store is in the database
   tw.clearSelection();
   tw.topLevelItem( 0 )->setSelected( true );
   EXPECT_EQ( tree.act_on_selection( "Database only", "DB-only remove" ),
              US_DataTree::ACT_NOTHING );
   EXPECT_EQ( filesIn( dirTwo, "*.xml" ).size(), 1 );
}

// Opening an experiment while a scan runs only queues it
TEST_F( TestUSDataModel, OpeningAnExperimentDuringTheScanOnlyQueuesIt )
{
   US_DataModel   model;
   prepare( model );

   QTreeWidget    tw;
   US_DataProcess proc( &model );
   US_DataTree    tree( &model, &tw );
   model.setSiblings( &proc, &tree );

   QObject::connect( &model, &US_DataModel::runsListed,
                     [ & ]() { tree.build_dtree(); } );

   int  calls      = 0;
   int  last       = -1;
   bool loadedThen = true;
   bool queued     = false;
   int  childThen  = -1;

   QObject::connect( &model, &US_DataModel::runMerged, [ & ]( int index )
   {
      tree.fill_run( index );

      if ( calls++ > 0 )   return;

      last = model.runCount() - 1;
      tree.item_opened( tw.topLevelItem( last ) );

      loadedThen = model.run_entry( last ).loaded;
      childThen  = tw.topLevelItem( last )->childCount();
      queued     = ( model.next_pending_run() == last );
   } );

   model.scan_data();

   EXPECT_FALSE( loadedThen );
   EXPECT_EQ   ( childThen, 0 );
   EXPECT_TRUE ( queued );
   EXPECT_TRUE ( model.run_entry( last ).loaded );
   EXPECT_GT   ( tw.topLevelItem( last )->childCount(), 0 );
}

// -------------------------------------------------------- orphans
//
// Records whose parent is missing are not in any experiment's chain; the
// scan shows them under a stand-in experiment, each under gray fillers for
// its missing parents.

TEST_F( TestUSDataModel, OrphansAreShownAfterTheScan )
{
   QString lostEdit   = US_Util::new_guid();
   QString orphModel  = US_Util::new_guid();
   QString orphNoise  = US_Util::new_guid();
   QString globModel  = US_Util::new_guid();

   US_Model lost;                     // its edit is gone
   lost.modelGUID   = orphModel;
   lost.editGUID    = lostEdit;
   lost.description = runOne + ".1A280.e2301011200_a2301011300_2DSA_i01.model";
   ASSERT_EQ( lost.write( US_Settings::dataDir() + "/models/M0000005.xml" ),
              US_DB2::OK );

   US_Noise noise;                    // under the orphan model
   noise.noiseGUID   = orphNoise;
   noise.modelGUID   = orphModel;
   noise.description = runOne + ".1A280.e2301011200_a2301011300_2DSA_i01.ti_noise";
   noise.type        = US_Noise::TI;
   noise.values << 0.001 << 0.002;
   noise.count       = 2;
   ASSERT_EQ( noise.write( US_Settings::dataDir() + "/noises/N0000005.xml" ),
              US_DB2::OK );

   US_Model global;                   // a global model has no edit at all
   global.modelGUID   = globModel;
   global.description = "Global-run.global.e2301011200_a2301011300_2DSA-GL_i01.model";
   ASSERT_EQ( global.write( US_Settings::dataDir() + "/models/M0000006.xml" ),
              US_DB2::OK );

   US_DataModel   model;
   prepare( model );

   QTreeWidget    tw;
   US_DataProcess proc( &model );
   US_DataTree    tree( &model, &tw );
   model.setSiblings( &proc, &tree );

   int added = -1;
   QObject::connect( &model, &US_DataModel::runsListed,
                     [ & ]() { tree.build_dtree(); } );
   QObject::connect( &model, &US_DataModel::runMerged,
                     [ & ]( int index ) { tree.fill_run( index ); } );
   QObject::connect( &model, &US_DataModel::orphansAdded,
                     [ & ]( int index ) { added = index; tree.add_run( index ); } );

   model.scan_data();

   ASSERT_EQ( added, model.runCount() - 1 );
   US_DataModel::RunEntry entry = model.run_entry( added );
   EXPECT_TRUE( entry.orphans );
   EXPECT_EQ  ( tw.topLevelItemCount(), model.runCount() );
   EXPECT_GT  ( tw.topLevelItem( added )->childCount(), 0 );

   QVector< int > rows = model.descendant_rows( entry.row );
   ASSERT_FALSE( rows.isEmpty() );

   QMap< QString, int > rowOf;
   int nfill = 0;

   for ( int jj = 0; jj < rows.size(); jj++ )
   {
      US_DataModel::DataDesc desc = model.row_datadesc( rows[ jj ] );
      rowOf[ desc.dataGUID ] = rows[ jj ];

      if ( desc.recState == US_DataModel::NOSTAT )
         nfill++;
   }

   ASSERT_TRUE( rowOf.contains( orphModel ) );
   ASSERT_TRUE( rowOf.contains( orphNoise ) );
   ASSERT_TRUE( rowOf.contains( globModel ) );
   EXPECT_GT  ( nfill, 0 );

   // the orphan model hangs off a filler edit with its edit's GUID
   int prow = model.parent_row( rowOf[ orphModel ] );
   ASSERT_GE( prow, 0 );
   EXPECT_EQ( model.row_datadesc( prow ).recState, (int)US_DataModel::NOSTAT );
   EXPECT_EQ( model.row_datadesc( prow ).dataGUID.toStdString(),
              lostEdit.toStdString() );

   // and its noise hangs off it
   EXPECT_EQ( model.parent_row( rowOf[ orphNoise ] ), rowOf[ orphModel ] );

   // the experiments keep their own records; nothing is in two places
   int covered = 0;

   for ( int ii = 0; ii < model.runCount(); ii++ )
      covered += model.descendant_rows( model.run_entry( ii ).row ).size();

   EXPECT_EQ( covered, model.recCount() - model.runCount() );
   EXPECT_TRUE( model.find_duplicates().isEmpty() );

   // a stand-in row's records can be acted on like any other's
   tw.clearSelection();
   tw.topLevelItem( added )->setSelected( true );
   EXPECT_EQ( tree.act_on_selection( "Local disk only", "Local-only remove" ),
              0 ) << proc.lastError().toStdString();
   EXPECT_FALSE( QFile::exists( US_Settings::dataDir() + "/models/M0000005.xml" ) );
   EXPECT_FALSE( QFile::exists( US_Settings::dataDir() + "/noises/N0000005.xml" ) );
   EXPECT_TRUE ( QFile::exists( US_Settings::dataDir() + "/models/M0000001.xml" ) );
}

// Narrowed to one run, nothing is an orphan of that run
TEST_F( TestUSDataModel, ARunFilterShowsNoOrphans )
{
   US_Model lost;
   lost.modelGUID   = US_Util::new_guid();
   lost.editGUID    = US_Util::new_guid();
   lost.description = runOne + ".1A280.e1_a1_2DSA_i01.model";
   ASSERT_EQ( lost.write( US_Settings::dataDir() + "/models/M0000005.xml" ),
              US_DB2::OK );

   US_DataModel model;
   model.setProgress( bar, label );
   model.setFilters ( runOne, "ALL", "Local Only" );

   int added = -1;
   QObject::connect( &model, &US_DataModel::orphansAdded,
                     [ & ]( int index ) { added = index; } );

   model.scan_data();

   EXPECT_EQ( added, -1 );
   EXPECT_EQ( model.runCount(), 1 );
}

// -------------------------------------------------------- run problems

TEST_F( TestUSDataModel, ARunWithAProblemSaysSo )
{
   // A directory named for one run, holding another's data
   QString dir = US_Settings::resultDir() + "/Misnamed_dir";
   QDir().mkpath( dir );
   writeAuc( dir + "/Other_run.RA.4.A.280.auc", US_Util::new_guid(), "four" );

   US_DataModel   model;
   prepare( model );

   QTreeWidget    tw;
   US_DataProcess proc( &model );
   US_DataTree    tree( &model, &tw );
   model.setSiblings( &proc, &tree );

   model.scan_data();
   tree.build_dtree();

   int other = model.index_of_run( "Other_run" );
   int dots  = model.index_of_run( runTwo );
   int fine  = model.index_of_run( runOne );
   ASSERT_GE( other, 0 );
   ASSERT_GE( dots,  0 );
   ASSERT_GE( fine,  0 );

   EXPECT_FALSE( model.run_entry( other ).problem.isEmpty() );
   EXPECT_FALSE( model.run_entry( dots  ).problem.isEmpty() );   // legacy ID
   EXPECT_TRUE ( model.run_entry( fine  ).problem.isEmpty() );

   EXPECT_EQ( model.run_problems().size(), 2 );

   // the tree row says why
   EXPECT_FALSE( tw.topLevelItem( dots )->toolTip( 1 ).isEmpty() );
   EXPECT_TRUE ( tw.topLevelItem( dots )->text( 1 ).endsWith( "(!)" ) );
   EXPECT_TRUE ( tw.topLevelItem( fine )->toolTip( 1 ).isEmpty() );
}

// The dates the catalog writes are the ones the upload check reads
TEST_F( TestUSDataModel, CatalogDatesAreRead )
{
   QDateTime when( QDate( 2024, 5, 6 ), QTime( 7, 8, 9 ), QTimeZone::utc() );
   QString   text = US_DataCatalog::utcText( QVariant( when ) );

   EXPECT_EQ( text.toStdString(), "2024-05-06 07:08:09 UTC" );
   EXPECT_EQ( US_DataModel::parse_date( text ), when );
}
