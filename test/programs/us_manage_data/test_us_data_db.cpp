// test_us_data_db.cpp - us_manage_data against a database, through the mock
//
// The database half of the window -- listing and reading the store through
// the catalog, pairing database experiments and records with the local
// ones, comparing them, and the record actions -- runs here against a small
// fake server behind the IUS_DB2 mock.  It answers the stored procedures
// the catalog and the actions call from one data set, the way a server
// would, and keeps a log of what was asked, so a test can say both what the
// tree shows and what was never asked of the server.  The local half is a
// scratch results directory, as in test_us_data_model.cpp.
#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include <QtCore>
#include <QProgressBar>
#include <QLabel>
#include <QTreeWidget>

#include "mock_us_db2.h"
#include "us_data_model.h"
#include "us_data_process.h"
#include "us_data_tree.h"
#include "us_settings.h"
#include "us_util.h"
#include "us_dataIO.h"
#include "us_model.h"
#include "us_noise.h"

using ::testing::_;
using ::testing::An;
using ::testing::Invoke;
using ::testing::NiceMock;
using ::testing::Return;

typedef US_DataModel::DataDesc Desc;

namespace
{
   const int     kInv  = 5;
   const QString kDate = "2024-03-04 05:06:07";

   struct FExp   { QString id, guid, runID; };
   struct FRaw   { QString id, guid, file, expID; };
   struct FEdit  { QString id, guid, rawID, file; };
   struct FModel { QString id, guid, desc, editGUID, editID, xml; };
   struct FNoise { QString id, guid, editID, modelID, modelGUID, type,
                   desc, xml; };

   struct Answer
   {
      int                   status = IUS_DB2::OK;
      QString               error;
      QList< QVariantList > rows;
   };

   // The contents of a file, as text
   QString textOf( const QString& path )
   {
      QFile file( path );
      if ( ! file.open( QIODevice::ReadOnly ) )   return QString();
      return QString::fromUtf8( file.readAll() );
   }

   /* A server holding one investigator's store */
   class FakeServer
   {
      public:
         QList< FExp >   exps;
         QList< FRaw >   raws;
         QList< FEdit >  edits;
         QList< FModel > models;
         QList< FNoise > noises;

         QMap< QString, QString > digest;     // GUID -> "md5 size"
         QMap< int, QByteArray >  blobs;      // editedDataID -> contents
         QSet< QString >          failing;    // procedures that fail
         QStringList              calls;      // every query, by name
         QStringList              changes;    // every statusQuery
         int                      autoflow = 0;

         Answer current;
         int    row = -1;

         void wire( NiceMock< US_DB2_Mock >& db )
         {
            ON_CALL( db, isConnected() ).WillByDefault( Return( true ) );
            ON_CALL( db, query( An< const QStringList& >() ) )
               .WillByDefault( Invoke( [ this ]( const QStringList& q ) { call( q ); } ) );
            ON_CALL( db, next() )
               .WillByDefault( Invoke( [ this ]() { return ++row < current.rows.size(); } ) );
            ON_CALL( db, value( _ ) )
               .WillByDefault( Invoke( [ this ]( unsigned ii ) -> QVariant
               {
                  if ( row < 0  ||  row >= current.rows.size() )  return QVariant();
                  return current.rows[ row ].value( int( ii ) );
               } ) );
            ON_CALL( db, lastErrno() )
               .WillByDefault( Invoke( [ this ]() { return current.status; } ) );
            ON_CALL( db, lastError() )
               .WillByDefault( Invoke( [ this ]() { return current.error; } ) );
            ON_CALL( db, lastServerErrno() )
               .WillByDefault( Invoke( [ this ]() { return current.status == IUS_DB2::OK ? 0 : 2013; } ) );
            ON_CALL( db, statusQuery( An< const QStringList& >() ) )
               .WillByDefault( Invoke( [ this ]( const QStringList& q )
               {
                  changes << q.join( " " );
                  return 0;
               } ) );
            ON_CALL( db, functionQuery( _ ) )
               .WillByDefault( Invoke( [ this ]( const QStringList& q )
               {
                  calls << q.value( 0 );
                  return autoflow;
               } ) );
            ON_CALL( db, readBlobFromDB( _, _, _ ) )
               .WillByDefault( Invoke( [ this ]( const QString& path,
                                                 const QString& proc, int id )
               {
                  calls << proc;
                  QFile file( path );
                  if ( ! blobs.contains( id )  ||
                       ! file.open( QIODevice::WriteOnly ) )
                     return int( IUS_DB2::DBERROR );
                  file.write( blobs[ id ] );
                  return int( IUS_DB2::OK );
               } ) );
         }

         int count( const QString& name ) const { return calls.count( name ); }

         QString ck( const QString& guid, int part ) const
         {
            QString text = digest.value( guid.simplified(),
                                         "md5-" + guid.simplified() + " 100" );
            return text.section( " ", part, part );
         }

         void call( const QStringList& q )
         {
            const QString name = q.value( 0 );
            calls << name;

            current = Answer();
            row     = -1;

            if ( failing.contains( name ) )
            {
               current.status = IUS_DB2::DBERROR;
               current.error  = "MySQL error: Lost connection during " + name;
               return;
            }

            current.rows = rowsOf( name, q.mid( 1 ) );

            if ( current.rows.isEmpty() )
            {
               current.status = IUS_DB2::NOROWS;
               current.error  = "no rows";
            }
         }

         const FRaw* rawById( const QString& id ) const
         {
            for ( const FRaw& rr : raws )  if ( rr.id == id )  return &rr;
            return nullptr;
         }

         QString expOfEdit( const QString& editID ) const
         {
            for ( const FEdit& ed : edits )
               if ( ed.id == editID )
                  return rawById( ed.rawID ) ? rawById( ed.rawID )->expID : QString();
            return QString();
         }

         QString rawGUIDOf( const FEdit& ed ) const
         {
            return rawById( ed.rawID ) ? rawById( ed.rawID )->guid : QString();
         }

         QVariantList rawRow( const FRaw& rr, bool hashed ) const
         {
            return QVariantList() << rr.id << rr.guid << "label" << "C:\\data\\" + rr.file
                                  << "comment" << rr.expID << "1" << kInv << kDate
                                  << ( hashed ? ck( rr.guid, 0 ) : QString() )
                                  << ( hashed ? ck( rr.guid, 1 ) : QString() );
         }

         QVariantList editRow( const FEdit& ed, bool hashed ) const
         {
            return QVariantList() << ed.id << ed.rawID << rawGUIDOf( ed ) << ed.guid
                                  << "label" << ed.file << "comment" << kDate
                                  << ( hashed ? ck( ed.guid, 0 ) : QString() )
                                  << ( hashed ? ck( ed.guid, 1 ) : QString() );
         }

         QVariantList modelRow( const FModel& mm ) const
         {
            return QVariantList() << mm.id << mm.guid << mm.desc << "" << ""
                                  << mm.editGUID << mm.editID << kDate
                                  << ck( mm.guid, 0 ) << ck( mm.guid, 1 );
         }

         QVariantList noiseRow( const FNoise& nn ) const
         {
            return QVariantList() << nn.id << nn.guid << nn.editID << nn.modelID
                                  << nn.type << nn.modelGUID << kDate
                                  << ck( nn.guid, 0 ) << ck( nn.guid, 1 ) << nn.desc;
         }

         QList< QVariantList > rowsOf( const QString& name, const QStringList& a ) const
         {
            QList< QVariantList > rows;

            if ( name == "get_experiment_summary" )
               for ( const FExp& ee : exps )
               {
                  int nr = 0, ne = 0, nm = 0, nn = 0;
                  for ( const FRaw& rr : raws )   if ( rr.expID == ee.id )  nr++;
                  for ( const FEdit& ed : edits ) if ( rawById( ed.rawID ) && rawById( ed.rawID )->expID == ee.id ) ne++;
                  for ( const FModel& mm : models ) if ( expOfEdit( mm.editID ) == ee.id ) nm++;
                  for ( const FNoise& no : noises ) if ( expOfEdit( no.editID ) == ee.id ) nn++;
                  rows << ( QVariantList() << ee.id << ee.guid << ee.runID << "velocity" << "RA"
                                           << ee.runID + " label" << "1" << ""
                                           << "project" << kDate << nr << ne << nm << nn );
               }

            else if ( name == "get_rawData_by_person" )
               for ( const FRaw& rr : raws )  rows << rawRow( rr, false );

            else if ( name == "get_editedData_by_person" )
               for ( const FEdit& ed : edits )  rows << editRow( ed, false );

            else if ( name == "get_rawData_by_experiment" )
            {
               for ( const FRaw& rr : raws )
                  if ( rr.expID == a.value( 0 ) )  rows << rawRow( rr, true );
            }

            else if ( name == "get_editedData_by_experiment" )
            {
               for ( const FEdit& ed : edits )
                  if ( rawById( ed.rawID ) && rawById( ed.rawID )->expID == a.value( 0 ) )
                     rows << editRow( ed, true );
            }

            else if ( name == "get_model_desc" )
               for ( const FModel& mm : models )  rows << modelRow( mm );

            else if ( name == "get_noise_desc" )
               for ( const FNoise& nn : noises )  rows << noiseRow( nn );

            else if ( name == "get_model_desc_by_experiment" )
            {
               for ( const FModel& mm : models )
                  if ( expOfEdit( mm.editID ) == a.value( 1 ) )  rows << modelRow( mm );
            }

            else if ( name == "get_noise_desc_by_experiment" )
            {
               for ( const FNoise& nn : noises )
                  if ( expOfEdit( nn.editID ) == a.value( 1 ) )  rows << noiseRow( nn );
            }

            else if ( name == "get_rawData" )
            {
               if ( const FRaw* rr = rawById( a.value( 0 ) ) )
                  rows << ( QVariantList() << rr->guid << "label" << rr->file << "comment"
                                           << rr->expID << "1" << "" << kDate
                                           << ck( rr->guid, 0 ) << ck( rr->guid, 1 ) );
            }

            else if ( name == "get_editedData" )
            {
               for ( const FEdit& ed : edits )
                  if ( ed.id == a.value( 0 ) )
                     rows << ( QVariantList() << ed.rawID << ed.guid << "label" << ed.file
                                              << "comment" << kDate << ck( ed.guid, 0 )
                                              << ck( ed.guid, 1 ) );
            }

            else if ( name == "get_model_info" )
            {
               for ( const FModel& mm : models )
                  if ( mm.id == a.value( 0 ) )
                     rows << ( QVariantList() << mm.guid << mm.desc << mm.xml << 0.0
                                              << mm.editGUID << "" << kDate );
            }

            else if ( name == "get_noise_info" )
            {
               for ( const FNoise& nn : noises )
                  if ( nn.id == a.value( 0 ) )
                     rows << ( QVariantList() << nn.guid << nn.desc << nn.modelGUID
                                              << nn.type << nn.modelID << nn.xml );
            }

            return rows;
         }
   };
}

class TestUSDataDb : public ::testing::Test
{
   protected:
      NiceMock< US_DB2_Mock > db;
      FakeServer              server;
      QString                 root;
      QString                 savedWorkDir;
      int                     savedInv;
      QProgressBar*           bar;
      QLabel*                 label;

      // The store both places share: RunA, with one triple and an edit, a
      // model and its noise, each the same record in both places
      const QString runA    = "RunA";
      QString       rawG, editG, modelG, noiseG, expG;
      QString       aucPath, editPath, modelPath, noisePath;

      void SetUp() override
      {
         savedWorkDir = US_Settings::workBaseDir();
         savedInv     = US_Settings::us_inv_ID();
         US_Settings::set_us_inv_ID( kInv );

         root = QDir::tempPath() + "/us_data_db_test_"
                + QString::number( QCoreApplication::applicationPid() ) + "_"
                + QString::number( QDateTime::currentMSecsSinceEpoch() );
         QDir().mkpath( root );
         US_Settings::set_workBaseDir( root );
         QDir().mkpath( US_Settings::dataDir() + "/models" );
         QDir().mkpath( US_Settings::dataDir() + "/noises" );

         server.wire( db );

         rawG   = US_Util::new_guid();
         editG  = US_Util::new_guid();
         modelG = US_Util::new_guid();
         noiseG = US_Util::new_guid();
         expG   = US_Util::new_guid();

         QString dir = runDir( runA );
         aucPath   = dir + "/" + runA + ".RA.1.A.280.auc";
         editPath  = dir + "/" + runA + ".2401011200.RA.1.A.280.xml";
         modelPath = US_Settings::dataDir() + "/models/M0000001.xml";
         noisePath = US_Settings::dataDir() + "/noises/N0000001.xml";

         writeAuc ( aucPath, rawG );
         writeEdit( editPath, editG, rawG );
         writeModel( modelPath, modelG, editG, runA + ".1A280.e2401011200_a1_2DSA_i01.model" );
         writeNoise( noisePath, noiseG, modelG, runA + ".1A280.e2401011200_a1_2DSA_i01.ti_noise" );

         server.exps   << FExp  { "1", expG, runA };
         server.raws   << FRaw  { "11", rawG, runA + ".RA.1.A.280.auc", "1" };
         server.edits  << FEdit { "21", editG, "11", runA + ".2401011200.RA.1.A.280.xml" };
         server.models << FModel{ "31", modelG, runA + ".1A280.e2401011200_a1_2DSA_i01.model",
                                  editG, "21", textOf( modelPath ) };
         server.noises << FNoise{ "41", noiseG, "21", "31", modelG, "ti_noise",
                                  runA + ".1A280.e2401011200_a1_2DSA_i01.ti_noise",
                                  textOf( noisePath ) };

         // The server's copies are the local ones
         server.digest[ rawG   ] = US_Util::md5sum_file( aucPath );
         server.digest[ editG  ] = US_Util::md5sum_file( editPath );
         server.digest[ modelG ] = US_Util::md5sum_file( modelPath );
         server.digest[ noiseG ] = US_Util::md5sum_file( noisePath );

         bar   = new QProgressBar();
         label = new QLabel();
      }

      void TearDown() override
      {
         delete bar;
         delete label;

         US_Settings::set_us_inv_ID( savedInv );
         US_Settings::set_workBaseDir( savedWorkDir );
         QDir( root ).removeRecursively();
      }

      QString runDir( const QString& runID )
      {
         QString path = US_Settings::resultDir() + "/" + runID;
         QDir().mkpath( path );
         return path;
      }

      void writeAuc( const QString& path, const QString& guid )
      {
         US_DataIO::RawData data;
         data.type[ 0 ]   = 'R';
         data.type[ 1 ]   = 'A';
         US_Util::uuid_parse( guid, (unsigned char*)data.rawGUID );
         data.cell        = 1;
         data.channel     = 'A';
         data.description = "triple";

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

      void writeModel( const QString& path, const QString& guid,
                       const QString& editGUID, const QString& desc )
      {
         US_Model model;
         model.modelGUID   = guid;
         model.editGUID    = editGUID;
         model.description = desc;
         ASSERT_EQ( model.write( path ), US_DB2::OK );
      }

      void writeNoise( const QString& path, const QString& guid,
                       const QString& modelGUID, const QString& desc )
      {
         US_Noise noise;
         noise.noiseGUID   = guid;
         noise.modelGUID   = modelGUID;
         noise.description = desc;
         noise.type        = US_Noise::TI;
         noise.values << 0.001 << 0.002;
         noise.count       = 2;
         ASSERT_EQ( noise.write( path ), US_DB2::OK );
      }

      void prepare( US_DataModel& model, const QString& run = "ALL" )
      {
         model.setDatabase( &db );
         model.setProgress( bar, label );
         model.setFilters ( run, "ALL", "ALL" );
      }

      // The row of the record with a GUID, in an experiment's range or in
      // the whole tree
      int rowOf( US_DataModel& model, const QString& guid, int runIndex = -1 )
      {
         for ( int row = 0; row < model.recCount(); row++ )
         {
            if ( model.row_datadesc( row ).dataGUID != guid )          continue;
            if ( runIndex >= 0  &&  model.run_of_row( row ) != runIndex ) continue;
            return row;
         }

         return -1;
      }

      int stateOf( US_DataModel& model, const QString& guid )
      {
         int row = rowOf( model, guid );
         return ( row < 0 ) ? -1 : model.row_datadesc( row ).recState;
      }

      static QTreeWidgetItem* itemOfRow( QTreeWidget& tw, int row )
      {
         QList< QTreeWidgetItem* > stack;

         for ( int ii = 0; ii < tw.topLevelItemCount(); ii++ )
            stack << tw.topLevelItem( ii );

         while ( ! stack.isEmpty() )
         {
            QTreeWidgetItem* item = stack.takeFirst();

            if ( item->type() - (int)QTreeWidgetItem::UserType == row )
               return item;

            for ( int ii = 0; ii < item->childCount(); ii++ )
               stack << item->child( ii );
         }

         return nullptr;
      }

      int standIn( US_DataModel& model, bool globals )
      {
         for ( int ii = 0; ii < model.runCount(); ii++ )
         {
            US_DataModel::RunEntry entry = model.run_entry( ii );
            if ( entry.orphans  &&  entry.globals == globals )  return ii;
         }

         return -1;
      }
};

// A tree, a processor and the prompts they would have shown
struct Window
{
   QTreeWidget    tw;
   US_DataProcess proc;
   US_DataTree    tree;
   QStringList    prompts;
   bool           answer = true;

   explicit Window( US_DataModel& model )
      : proc( &model ), tree( &model, &tw )
   {
      tw.setSelectionMode( QAbstractItemView::ExtendedSelection );
      model.setSiblings( &proc, &tree );
      tree.setPrompter( [ this ]( int, const QString& title, const QString& text )
                        { prompts << title + ": " + text; return answer; } );
   }

   void select( QTreeWidgetItem* item )
   {
      tw.clearSelection();
      ASSERT_NE( item, nullptr );
      item->setSelected( true );
   }
};

// ------------------------------------------------------------ the scan

// One experiment in both places: its records pair by GUID, the scan reads
// the store in its whole-store queries, and compares the experiment
TEST_F( TestUSDataDb, RecordsInBothPlacesArePairedAndCompared )
{
   US_DataModel model;
   prepare( model );

   model.scan_data();

   ASSERT_EQ( model.runCount(), 1 );
   US_DataModel::RunEntry entry = model.run_entry( 0 );
   EXPECT_TRUE( entry.inBoth() );
   EXPECT_TRUE( entry.loaded );
   EXPECT_TRUE( entry.verified );
   EXPECT_TRUE( model.scan_errors().isEmpty() )
      << model.scan_errors().join( "\n" ).toStdString();
   EXPECT_FALSE( model.db_listing_failed() );

   // the whole store in its own queries, not one experiment at a time
   EXPECT_EQ( server.count( "get_rawData_by_person" ), 1 );
   EXPECT_EQ( server.count( "get_model_desc" ), 1 );
   EXPECT_EQ( server.count( "get_model_desc_by_experiment" ), 0 );

   for ( const QString& guid : { rawG, editG, modelG, noiseG } )
   {
      int row = rowOf( model, guid );
      ASSERT_GE( row, 0 );
      EXPECT_EQ( model.run_of_row( row ), 0 );
      EXPECT_EQ( US_DataModel::sync_state( model.row_datadesc( row ) ),
                 US_DataModel::SYNC_INSYNC )
         << model.row_datadesc( row ).contents.toStdString();
   }

   EXPECT_EQ( US_DataModel::sync_state( model.row_datadesc( entry.row ) ),
              US_DataModel::SYNC_INSYNC );
   EXPECT_TRUE( model.find_duplicates().isEmpty() );
   EXPECT_EQ( model.recCountDB(),  4 );
   EXPECT_EQ( model.recCountLoc(), 4 );
}

// Copies that differ are a conflict, and the experiment row says so
TEST_F( TestUSDataDb, CopiesThatDifferAreAConflict )
{
   server.digest[ editG ] = "0123456789abcdef0123456789abcdef 999";

   US_DataModel model;
   prepare( model );
   model.scan_data();

   EXPECT_EQ( US_DataModel::sync_state( model.row_datadesc( rowOf( model, editG ) ) ),
              US_DataModel::SYNC_CONFLICT );
   EXPECT_EQ( US_DataModel::sync_state( model.row_datadesc( rowOf( model, rawG ) ) ),
              US_DataModel::SYNC_INSYNC );
   EXPECT_EQ( US_DataModel::sync_state( model.row_datadesc( model.run_entry( 0 ).row ) ),
              US_DataModel::SYNC_CONFLICT );
}

// Two database experiments may share a run ID: each is an experiment of its
// own, and the local run joins the first
TEST_F( TestUSDataDb, DatabaseExperimentsSharingARunIdAreKeptApart )
{
   server.exps << FExp{ "2", US_Util::new_guid(), runA };

   US_DataModel model;
   prepare( model );
   model.scan_data();

   ASSERT_EQ( model.runCount(), 2 );
   US_DataModel::RunEntry first  = model.run_entry( 0 );
   US_DataModel::RunEntry second = model.run_entry( 1 );

   EXPECT_EQ( first .runID.toStdString(), runA.toStdString() );
   EXPECT_EQ( second.runID.toStdString(), runA.toStdString() );
   EXPECT_TRUE ( first.inBoth() );
   EXPECT_FALSE( second.inBoth() );
   EXPECT_GE   ( second.dbIndex, 0 );
   EXPECT_NE   ( first.dbIndex, second.dbIndex );

   // the records are the first one's; the second has none
   EXPECT_EQ( model.run_of_row( rowOf( model, editG ) ), 0 );
   EXPECT_TRUE( model.descendant_rows( second.row ).isEmpty() );
}

// A run filter reads that run alone: no whole-store query goes out
TEST_F( TestUSDataDb, AFilteredScanReadsOnlyTheFilteredRun )
{
   QString rawB = US_Util::new_guid();
   server.exps << FExp{ "2", US_Util::new_guid(), "RunB" };
   server.raws << FRaw{ "12", rawB, "RunB.RA.1.A.280.auc", "2" };

   US_DataModel model;
   prepare( model, runA );
   model.scan_data();

   ASSERT_EQ( model.runCount(), 1 );
   EXPECT_EQ( model.run_entry( 0 ).runID.toStdString(), runA.toStdString() );

   for ( const char* proc : { "get_rawData_by_person", "get_editedData_by_person",
                              "get_model_desc", "get_noise_desc",
                              "all_rawDataIDs", "all_editedDataIDs" } )
      EXPECT_EQ( server.count( proc ), 0 ) << proc;

   EXPECT_GE( server.count( "get_rawData_by_experiment" ), 1 );
   EXPECT_EQ( rowOf( model, rawB ), -1 );

   // and the run is still compared
   EXPECT_TRUE( model.run_entry( 0 ).verified );
   EXPECT_EQ( US_DataModel::sync_state( model.row_datadesc( rowOf( model, editG ) ) ),
              US_DataModel::SYNC_INSYNC );
}

// When the database cannot be listed the window says so, and nothing else
// is asked of it: every record would read as an orphan
TEST_F( TestUSDataDb, AFailedListingIsReportedAndReadsNothingMore )
{
   server.failing << "get_experiment_summary" << "get_experiment_desc";

   US_DataModel model;
   prepare( model );
   model.scan_data();

   EXPECT_TRUE ( model.db_listing_failed() );
   EXPECT_FALSE( model.scan_errors().isEmpty() );
   EXPECT_EQ   ( server.count( "get_rawData_by_person" ), 0 );
   EXPECT_EQ   ( server.count( "get_model_desc" ), 0 );

   ASSERT_EQ( model.runCount(), 1 );
   EXPECT_FALSE( model.run_entry( 0 ).inBoth() );
   EXPECT_EQ( stateOf( model, editG ) & US_DataModel::REC_DB, 0 );
   EXPECT_LT( standIn( model, false ), 0 );
}

// ------------------------------------------------------------ orphans

// A database record whose parent is gone, and a global model, which has no
// edit by design, are each under a stand-in of their own
TEST_F( TestUSDataDb, DatabaseOrphansAndGlobalModelsHaveStandIns )
{
   QString lost = US_Util::new_guid();
   QString glob = US_Util::new_guid();
   server.models << FModel{ "32", lost, runA + ".1A280.lost.model",
                            US_Util::new_guid(), "99", "" };
   server.models << FModel{ "33", glob, "Global.global.e1_a1_2DSA-GL_i01.model",
                            "", "", "" };

   US_DataModel model;
   prepare( model );

   QList< int > added;
   QObject::connect( &model, &US_DataModel::orphansAdded,
                     [ & ]( int index ) { added << index; } );

   model.scan_data();

   int orph = standIn( model, false );
   int glbs = standIn( model, true );
   ASSERT_GE( orph, 0 );
   ASSERT_GE( glbs, 0 );
   EXPECT_EQ( added, QList< int >() << orph << glbs );

   EXPECT_EQ( model.run_of_row( rowOf( model, lost ) ), orph );
   EXPECT_EQ( model.run_of_row( rowOf( model, glob ) ), glbs );
   EXPECT_EQ( stateOf( model, lost ) & US_DataModel::REC_DB, (int)US_DataModel::REC_DB );

   // stand-ins are not experiments
   EXPECT_EQ( model.runCount(), 3 );
   EXPECT_EQ( model.experimentCount(), 1 );
}

// A local copy of a database record whose parent is in the database only
// pairs with the database record, under that parent -- not as an orphan,
// and not as a duplicate
TEST_F( TestUSDataDb, ALocalCopyOfARecordWithADatabaseOnlyParentPairsWithIt )
{
   QString edit2  = US_Util::new_guid();
   QString model2 = US_Util::new_guid();
   QString mpath  = US_Settings::dataDir() + "/models/M0000002.xml";
   writeModel( mpath, model2, edit2, runA + ".1A280.e2401021200_a1_2DSA_i01.model" );

   server.edits  << FEdit { "22", edit2, "11", runA + ".2401021200.RA.1.A.280.xml" };
   server.models << FModel{ "34", model2, runA + ".1A280.e2401021200_a1_2DSA_i01.model",
                            edit2, "22", textOf( mpath ) };
   server.digest[ model2 ] = US_Util::md5sum_file( mpath );

   US_DataModel model;
   prepare( model );
   model.scan_data();

   int row = rowOf( model, model2 );
   ASSERT_GE( row, 0 );
   EXPECT_EQ( model.run_of_row( row ), 0 );
   EXPECT_EQ( model.row_datadesc( row ).recState
              & ( US_DataModel::REC_DB | US_DataModel::REC_LO ),
              US_DataModel::REC_DB | US_DataModel::REC_LO );
   EXPECT_EQ( model.row_datadesc( model.parent_row( row ) ).dataGUID.toStdString(),
              edit2.toStdString() );

   // one row for the record, nothing left as an orphan, no false alarm
   int nrows = 0;
   for ( int rr = 0; rr < model.recCount(); rr++ )
      nrows += ( model.row_datadesc( rr ).dataGUID == model2 ) ? 1 : 0;
   EXPECT_EQ( nrows, 1 );
   EXPECT_LT( standIn( model, false ), 0 );
   EXPECT_TRUE( model.find_duplicates().isEmpty() );

   // and it is compared like any other
   EXPECT_EQ( US_DataModel::sync_state( model.row_datadesc( row ) ),
              US_DataModel::SYNC_INSYNC )
      << model.row_datadesc( row ).contents.toStdString();
}

// The records of a stand-in that are in both places are compared too, and
// the stand-in row is no record of either place
TEST_F( TestUSDataDb, AStandInsPairedRecordsAreCompared )
{
   QString lostEdit = US_Util::new_guid();
   QString lost     = US_Util::new_guid();
   QString mpath    = US_Settings::dataDir() + "/models/M0000009.xml";
   writeModel( mpath, lost, lostEdit, runA + ".1A280.e9_a9_2DSA_i01.model" );
   server.models << FModel{ "39", lost, runA + ".1A280.e9_a9_2DSA_i01.model",
                            lostEdit, "99", textOf( mpath ) };
   server.digest[ lost ] = US_Util::md5sum_file( mpath );

   US_DataModel model;
   prepare( model );
   Window       win( model );
   model.scan_data();
   win.tree.build_dtree();

   int orph = standIn( model, false );
   ASSERT_GE( orph, 0 );
   int row = rowOf( model, lost, orph );
   ASSERT_GE( row, 0 );

   EXPECT_TRUE( model.run_entry( orph ).verified );
   EXPECT_EQ( US_DataModel::sync_state( model.row_datadesc( row ) ),
              US_DataModel::SYNC_INSYNC )
      << model.row_datadesc( row ).contents.toStdString();
   EXPECT_FALSE( model.needs_verify( orph ) );

   // neutral: neither "DB" nor "Local" nor a sync state
   EXPECT_EQ( win.tw.topLevelItem( orph )->text( 3 ).toStdString(), "--" );
}

// ------------------------------------------------------------ comparisons

// An experiment asked for while the chain is read is compared first
TEST_F( TestUSDataDb, AnExperimentAskedForIsComparedFirst )
{
   QString rawB = US_Util::new_guid();
   QString aucB = runDir( "RunB" ) + "/RunB.RA.1.A.280.auc";
   writeAuc( aucB, rawB );
   server.exps << FExp{ "2", US_Util::new_guid(), "RunB" };
   server.raws << FRaw{ "12", rawB, "RunB.RA.1.A.280.auc", "2" };
   server.digest[ rawB ] = US_Util::md5sum_file( aucB );

   US_DataModel model;
   prepare( model );

   bool asked = false;
   QList< int > compared;
   QObject::connect( &model, &US_DataModel::runMerged, [ & ]( int )
   {
      if ( asked )   return;
      asked = true;
      model.request_verify( model.index_of_run( "RunB" ) );
   } );
   QObject::connect( &model, &US_DataModel::runCompared,
                     [ & ]( int index ) { compared << index; } );

   model.scan_data();

   ASSERT_EQ( compared.size(), 2 );
   EXPECT_EQ( compared.first(), model.index_of_run( "RunB" ) );
}

// The checksums are found whatever whitespace the server leaves around a
// GUID: the rows carry it simplified
TEST_F( TestUSDataDb, ChecksumsAreFoundByTheSimplifiedGuid )
{
   server.models[ 0 ].guid = modelG + "  ";
   server.noises[ 0 ].modelGUID = modelG + "  ";
   server.failing << "get_model_info";     // nothing to fall back on

   US_DataModel model;
   prepare( model );
   model.scan_data();

   int row = rowOf( model, modelG );
   ASSERT_GE( row, 0 );
   EXPECT_EQ( US_DataModel::sync_state( model.row_datadesc( row ) ),
              US_DataModel::SYNC_INSYNC )
      << model.row_datadesc( row ).contents.toStdString();
}

// A model whose two copies differ only in how they are written out shows in
// sync, and so does its experiment
TEST_F( TestUSDataDb, TheExperimentRowReadsAsItsRecordsDo )
{
   server.models[ 0 ].xml += "\n\n";
   QByteArray xml = server.models[ 0 ].xml.toUtf8();
   server.digest[ modelG ] = QString( QCryptographicHash::hash(
                                xml, QCryptographicHash::Md5 ).toHex() )
                             + " " + QString::number( xml.size() );

   US_DataModel model;
   prepare( model );
   Window       win( model );
   model.scan_data();
   win.tree.build_dtree();

   int row = rowOf( model, modelG );
   ASSERT_GE( row, 0 );
   EXPECT_EQ( US_DataModel::sync_state( model.row_datadesc( row ) ),
              US_DataModel::SYNC_CONFLICT );            // the bytes differ

   QTreeWidgetItem* item = itemOfRow( win.tw, row );
   ASSERT_NE( item, nullptr );
   EXPECT_EQ( item->text( 3 ).toStdString(), "In Sync" );   // the model does not
   EXPECT_EQ( win.tw.topLevelItem( 0 )->text( 3 ).toStdString(), "In Sync" );
}

// ------------------------------------------------------------ actions

// A record downloaded is compared again, on its own, in the background
TEST_F( TestUSDataDb, ADownloadedRecordIsComparedAgain )
{
   QString edit3 = US_Util::new_guid();
   QString file3 = runA + ".2401031200.RA.1.A.280.xml";
   QString tmp   = root + "/blob.xml";
   writeEdit( tmp, edit3, rawG );
   server.edits << FEdit{ "23", edit3, "11", file3 };
   server.blobs[ 23 ]      = textOf( tmp ).toUtf8();
   server.digest[ edit3 ]  = US_Util::md5sum_file( tmp );

   US_DataModel model;
   prepare( model );
   Window       win( model );
   model.scan_data();
   win.tree.build_dtree();

   ASSERT_TRUE( model.run_entry( 0 ).verified );
   int row = rowOf( model, edit3 );
   ASSERT_GE( row, 0 );
   EXPECT_EQ( model.row_datadesc( row ).recState & US_DataModel::REC_LO, 0 );

   int asked = 0;
   QObject::connect( &model, &US_DataModel::workRequested, [ & ]() { asked++; } );

   win.select( itemOfRow( win.tw, row ) );
   EXPECT_EQ( win.tree.act_on_selection( "Database only", "Local file create" ), 0 )
      << win.proc.lastError().toStdString();

   EXPECT_TRUE( QFile::exists( runDir( runA ) + "/" + file3 ) );
   EXPECT_EQ( US_DataModel::sync_state( model.row_datadesc( row ) ),
              US_DataModel::SYNC_UNVERIFIED );
   EXPECT_FALSE( model.run_entry( 0 ).verified );
   EXPECT_EQ( win.tw.topLevelItem( 0 )->text( 3 ).toStdString(), "Unverified" );
   EXPECT_GT( asked, 0 );
   EXPECT_EQ( model.next_pending_verify(), 0 );

   // the background pass compares that record, and only that record
   int before = server.count( "get_editedData_by_experiment" );
   EXPECT_TRUE( model.pump_once() );
   EXPECT_EQ( server.count( "get_editedData_by_experiment" ), before );
   EXPECT_EQ( server.count( "get_editedData" ), 1 );

   EXPECT_EQ( US_DataModel::sync_state( model.row_datadesc( row ) ),
              US_DataModel::SYNC_INSYNC );
   EXPECT_TRUE( model.run_entry( 0 ).verified );
   EXPECT_FALSE( model.pump_once() );
}

// A database replace of a copy the database already holds writes nothing
TEST_F( TestUSDataDb, UploadingWhatTheDatabaseHoldsIsNothingToDo )
{
   US_DataModel model;
   prepare( model );
   Window       win( model );
   model.scan_data();
   win.tree.build_dtree();

   int row = rowOf( model, editG );
   ASSERT_EQ( US_DataModel::sync_state( model.row_datadesc( row ) ),
              US_DataModel::SYNC_INSYNC );

   win.select( itemOfRow( win.tw, row ) );
   EXPECT_EQ( win.tree.act_on_selection( "both DB and Local", "DB replace" ),
              US_DataTree::ACT_IN_SYNC );
   EXPECT_EQ( server.count( "update_editedData" ), 0 );
   EXPECT_EQ( server.count( "new_editedData" ), 0 );

   // Not compared yet, and the database copy is newer -- it came from this
   // very file: the contents decide, not the dates
   Desc desc = model.row_datadesc( row );
   desc.recState   &= ~US_DataModel::HV_DET;
   desc.contents    = QString();
   desc.lastmodDate = "2030-01-01 00:00:00 UTC";
   desc.filemodDate = "2020-01-01 00:00:00 UTC";
   model.change_datadesc( desc, row );

   EXPECT_EQ( win.proc.record_upload( row ), (int)US_DataProcess::IN_SYNC );
   EXPECT_EQ( server.count( "get_editedData" ), 1 );     // compared on its own

   // When they differ, an older local copy does not replace the database's
   QFile file( editPath );
   ASSERT_TRUE( file.open( QIODevice::Append ) );
   file.write( "\n" );
   file.close();

   EXPECT_EQ( win.proc.record_upload( row ), 3061 );
   EXPECT_TRUE( win.proc.lastError().contains( "older" ) );
   EXPECT_EQ( server.count( "update_editedData" ), 0 );
}

// Removing an edit from the database removes what hangs off it there too;
// those rows are marked, not deleted again
TEST_F( TestUSDataDb, RemovingFromTheDatabaseCascades )
{
   US_DataModel model;
   prepare( model );
   Window       win( model );
   model.scan_data();
   win.tree.build_dtree();

   win.select( itemOfRow( win.tw, rowOf( model, editG ) ) );
   EXPECT_EQ( win.tree.act_on_selection( "Database only", "DB-only remove" ), 0 )
      << win.proc.lastError().toStdString();

   EXPECT_EQ( server.changes, QStringList() << "delete_editedData 21" );

   for ( const QString& guid : { editG, modelG, noiseG } )
   {
      Desc desc = model.row_datadesc( rowOf( model, guid ) );
      EXPECT_EQ( desc.recState & US_DataModel::REC_DB, 0 );
      EXPECT_NE( desc.recState & US_DataModel::REC_LO, 0 );
      EXPECT_EQ( US_DataModel::sync_state( desc ), US_DataModel::SYNC_LOCAL );
   }

   EXPECT_NE( stateOf( model, rawG ) & US_DataModel::REC_DB, 0 );
}

// Nothing the GMP framework needs is removed, and nothing is asked to be
TEST_F( TestUSDataDb, GmpDataIsNotRemoved )
{
   server.autoflow = 1;

   US_DataModel model;
   prepare( model );
   Window       win( model );
   model.scan_data();
   win.tree.build_dtree();

   win.select( win.tw.topLevelItem( 0 ) );
   EXPECT_EQ( win.tree.act_on_selection( "Database only", "DB-only remove" ),
              US_DataTree::ACT_REFUSED );
   EXPECT_TRUE( server.changes.isEmpty() );
   ASSERT_EQ( win.prompts.size(), 1 );
   EXPECT_TRUE( win.prompts[ 0 ].contains( "GMP" ) );
   EXPECT_NE( stateOf( model, editG ) & US_DataModel::REC_DB, 0 );
}

// Removing everything under the global models asks first, with the count
TEST_F( TestUSDataDb, RemovingAllGlobalModelsAsksFirst )
{
   server.models << FModel{ "51", US_Util::new_guid(), "G.global.e1_a1_2DSA-GL_i01.model", "", "", "" };
   server.models << FModel{ "52", US_Util::new_guid(), "G.global.e1_a1_2DSA-GL_i02.model", "", "", "" };

   US_DataModel model;
   prepare( model );
   Window       win( model );
   model.scan_data();
   win.tree.build_dtree();

   int glbs = standIn( model, true );
   ASSERT_GE( glbs, 0 );
   win.select( win.tw.topLevelItem( glbs ) );

   win.answer = false;
   EXPECT_EQ( win.tree.act_on_selection( "Database only", "DB-only remove" ),
              US_DataTree::ACT_CANCELLED );
   EXPECT_TRUE( server.changes.isEmpty() );
   ASSERT_EQ( win.prompts.size(), 1 );
   EXPECT_TRUE( win.prompts[ 0 ].contains( "all 2 record(s)" ) )
      << win.prompts[ 0 ].toStdString();

   win.answer = true;
   EXPECT_EQ( win.tree.act_on_selection( "Database only", "DB-only remove" ), 0 );
   EXPECT_EQ( server.changes, QStringList() << "delete_model 51" << "delete_model 52" );

   // the experiment's own model is not among them
   EXPECT_NE( stateOf( model, modelG ) & US_DataModel::REC_DB, 0 );
}

// A download that would land in another directory than the run's is refused
TEST_F( TestUSDataDb, NothingIsWrittenForARunInTheWrongDirectory )
{
   QDir().rename( runDir( runA ), US_Settings::resultDir() + "/Other_dir" );
   QString edit3 = US_Util::new_guid();
   server.edits << FEdit{ "23", edit3, "11", runA + ".2401031200.RA.1.A.280.xml" };
   server.blobs[ 23 ] = "x";

   US_DataModel model;
   prepare( model );
   Window       win( model );
   model.scan_data();
   win.tree.build_dtree();

   ASSERT_EQ( model.runCount(), 1 );
   EXPECT_TRUE( model.run_entry( 0 ).unsafe );

   win.select( itemOfRow( win.tw, rowOf( model, edit3 ) ) );
   EXPECT_EQ( win.tree.act_on_selection( "Database only", "Local file create" ),
              US_DataTree::ACT_REFUSED );
   EXPECT_FALSE( QDir( runDir( runA ) ).exists( runA + ".2401031200.RA.1.A.280.xml" ) );
   EXPECT_EQ( server.count( "download_editData" ), 0 );

   // and the processor refuses on its own too
   EXPECT_EQ( win.proc.record_download( rowOf( model, edit3 ) ),
              (int)US_DataProcess::UNSAFE_RUN );
}

// A model is not downloaded where its edit is not
TEST_F( TestUSDataDb, AChildIsNotDownloadedWithoutItsParent )
{
   QString edit3  = US_Util::new_guid();
   QString model3 = US_Util::new_guid();
   server.edits  << FEdit { "23", edit3, "11", runA + ".2401031200.RA.1.A.280.xml" };
   server.models << FModel{ "35", model3, runA + ".1A280.e2401031200_a1_2DSA_i01.model",
                            edit3, "23", "<xml/>" };

   US_DataModel model;
   prepare( model );
   Window       win( model );
   model.scan_data();

   int row = rowOf( model, model3 );
   ASSERT_GE( row, 0 );
   EXPECT_EQ( win.proc.record_download( row ), (int)US_DataProcess::NO_PARENT );
   EXPECT_EQ( server.count( "get_model_info" ), 0 );
}
