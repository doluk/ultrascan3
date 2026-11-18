// test_us_data_merge.cpp - how us_manage_data merges, labels and judges records
//
// These tests feed US_DataModel records made up on the spot -- the kind the
// database side produces, which cannot be scanned here for want of a server
// -- and check the database paths against the IUS_DB2 mock.
#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include <QtCore>

#include "us_data_model.h"
#include "us_data_process.h"
#include "us_model.h"
#include "us_noise.h"
#include "us_util.h"
#include "mock_us_db2.h"

using ::testing::_;
using ::testing::An;
using ::testing::NiceMock;
using ::testing::Return;

namespace
{
   typedef US_DataModel::DataDesc Desc;

   const QString runID = "merge_run";

   Desc record( int recType, int state, const QString& guid,
                const QString& parentGUID, const QString& text )
   {
      Desc desc;
      desc.recordID    = ( state == US_DataModel::REC_DB ) ? 100 : -1;
      desc.recType     = recType;
      desc.parentID    = -1;
      desc.recState    = state;
      desc.subType     = "";
      desc.dataGUID    = guid;
      desc.parentGUID  = parentGUID;
      desc.filename    = ( state == US_DataModel::REC_LO )
                         ? "/data/" + guid + ".xml" : QString();
      desc.contents    = "";
      desc.label       = text;
      desc.description = text;
      return desc;
   }

   // The rows of a merge, by GUID
   QMap< QString, QList< Desc > > byGuid( const QVector< Desc >& descs )
   {
      QMap< QString, QList< Desc > > map;

      for ( int ii = 0; ii < descs.size(); ii++ )
         map[ descs[ ii ].dataGUID ] << descs[ ii ];

      return map;
   }
}

class TestUSDataMerge : public ::testing::Test
{
   protected:
      QString rawG, editG, modA, modB, modC, noiseG;

      void SetUp() override
      {
         rawG   = US_Util::new_guid();
         editG  = US_Util::new_guid();
         modA   = US_Util::new_guid();
         modB   = US_Util::new_guid();
         modC   = US_Util::new_guid();
         noiseG = US_Util::new_guid();
      }
};

// ------------------------------------------------------------ pairing

// A record in both places is one row, wherever either side lists it: the
// database holds a model the local disk does not, and the one model both
// hold is the second in one list and the first in the other.
TEST_F( TestUSDataMerge, RecordsArePairedByGuidNotByPosition )
{
   const int DB = US_DataModel::REC_DB;
   const int LO = US_DataModel::REC_LO;

   QVector< Desc > dbs;
   dbs << record( US_DataModel::RAW,   DB, rawG,  "", runID + ".1.A.280" )
       << record( US_DataModel::EDIT,  DB, editG, rawG, runID + ".e1" )
       << record( US_DataModel::MODEL, DB, modA,  editG, runID + ".a.model" )
       << record( US_DataModel::MODEL, DB, modB,  editG, runID + ".b.model" );

   QVector< Desc > los;
   los << record( US_DataModel::RAW,   LO, rawG,  "", runID + ".1.A.280" )
       << record( US_DataModel::EDIT,  LO, editG, rawG, runID + ".e1" )
       << record( US_DataModel::MODEL, LO, modB,  editG, runID + ".b.model" )
       << record( US_DataModel::MODEL, LO, modC,  editG, runID + ".c.model" )
       << record( US_DataModel::NOISE, LO, noiseG, modB, runID + ".b.ti_noise" );

   US_DataModel model;
   QVector< Desc > merged = model.merge_descs( dbs, los );
   QMap< QString, QList< Desc > > rows = byGuid( merged );

   ASSERT_EQ( merged.size(), 6 );          // one row per record

   for ( auto it = rows.begin(); it != rows.end(); ++it )
      EXPECT_EQ( it.value().size(), 1 ) << it.key().toStdString();

   EXPECT_EQ( rows[ rawG  ][ 0 ].recState & ( DB | LO ), DB | LO );
   EXPECT_EQ( rows[ editG ][ 0 ].recState & ( DB | LO ), DB | LO );
   EXPECT_EQ( rows[ modA  ][ 0 ].recState & ( DB | LO ), DB );
   EXPECT_EQ( rows[ modB  ][ 0 ].recState & ( DB | LO ), DB | LO );
   EXPECT_EQ( rows[ modC  ][ 0 ].recState & ( DB | LO ), LO );

   // the paired record knows its local file and keeps its DB id
   EXPECT_EQ( rows[ modB ][ 0 ].recordID, 100 );
   EXPECT_FALSE( rows[ modB ][ 0 ].filename.isEmpty() );

   // tree order: raw, edit, then each model followed by its noise
   ASSERT_EQ( merged[ 0 ].dataGUID.toStdString(), rawG.toStdString() );
   ASSERT_EQ( merged[ 1 ].dataGUID.toStdString(), editG.toStdString() );
   EXPECT_EQ( merged[ 2 ].dataGUID.toStdString(), modA.toStdString() );
   EXPECT_EQ( merged[ 3 ].dataGUID.toStdString(), modB.toStdString() );
   EXPECT_EQ( merged[ 4 ].dataGUID.toStdString(), noiseG.toStdString() );
   EXPECT_EQ( merged[ 5 ].dataGUID.toStdString(), modC.toStdString() );

   // a record knows where its parent is
   EXPECT_NE( rows[ modA ][ 0 ].recState & US_DataModel::PAR_DB, 0 );
   EXPECT_NE( rows[ modA ][ 0 ].recState & US_DataModel::PAR_LO, 0 );
}

// Paired by GUID within the parent: the same model GUID under two different
// edits is two records
TEST_F( TestUSDataMerge, RecordsUnderDifferentParentsAreNotPaired )
{
   QString otherEdit = US_Util::new_guid();

   QVector< Desc > dbs;
   dbs << record( US_DataModel::RAW,   US_DataModel::REC_DB, rawG, "", "r" )
       << record( US_DataModel::EDIT,  US_DataModel::REC_DB, editG, rawG, "e1" )
       << record( US_DataModel::MODEL, US_DataModel::REC_DB, modA, editG, "m" );

   QVector< Desc > los;
   los << record( US_DataModel::RAW,   US_DataModel::REC_LO, rawG, "", "r" )
       << record( US_DataModel::EDIT,  US_DataModel::REC_LO, otherEdit, rawG, "e2" )
       << record( US_DataModel::MODEL, US_DataModel::REC_LO, modA, otherEdit, "m" );

   US_DataModel model;
   QMap< QString, QList< Desc > > rows = byGuid( model.merge_descs( dbs, los ) );

   EXPECT_EQ( rows[ rawG ].size(), 1 );
   EXPECT_EQ( rows[ modA ].size(), 2 );
}

// A record whose parent is missing hangs off gray fillers
TEST_F( TestUSDataMerge, AnOrphanGetsFillerParents )
{
   QVector< Desc > los;
   los << record( US_DataModel::NOISE, US_DataModel::REC_LO, noiseG, modA, "n" );

   US_DataModel model;
   QVector< Desc > merged = model.merge_descs( QVector< Desc >(), los );

   ASSERT_EQ( merged.size(), 4 );           // raw, edit, model fillers + noise

   EXPECT_EQ( merged[ 0 ].recType, (int)US_DataModel::RAW   );
   EXPECT_EQ( merged[ 1 ].recType, (int)US_DataModel::EDIT  );
   EXPECT_EQ( merged[ 2 ].recType, (int)US_DataModel::MODEL );
   EXPECT_EQ( merged[ 3 ].recType, (int)US_DataModel::NOISE );

   for ( int ii = 0; ii < 3; ii++ )
   {
      EXPECT_EQ( merged[ ii ].recState, (int)US_DataModel::NOSTAT );
      EXPECT_EQ( US_DataModel::sync_state( merged[ ii ] ),
                 US_DataModel::SYNC_DUMMY );
   }

   // the filler model carries the GUID the noise points at
   EXPECT_EQ( merged[ 2 ].dataGUID.toStdString(), modA.toStdString() );
   EXPECT_EQ( US_DataModel::sync_state( merged[ 3 ] ),
              US_DataModel::SYNC_LOCAL );
}

// Orphans are shown under a stand-in experiment with a row range of its own
TEST_F( TestUSDataMerge, OrphansAreShownUnderAStandInExperiment )
{
   US_DataModel model;

   QVector< Desc > dbo;
   dbo << record( US_DataModel::MODEL, US_DataModel::REC_DB, modA, editG,
                  "orph.run.1A280.e1_a1_2DSA_i01.model" );
   QVector< Desc > loo;
   loo << record( US_DataModel::MODEL, US_DataModel::REC_LO, modA, editG,
                  "orph.run.1A280.e1_a1_2DSA_i01.model" )
       << record( US_DataModel::EDIT,  US_DataModel::REC_LO, editG, rawG,
                  "orph.run.2401011200.1.A.280" );

   EXPECT_EQ( model.add_orphans( QVector< Desc >(), QVector< Desc >() ), -1 );

   int index = model.add_orphans( dbo, loo );
   ASSERT_EQ( index, 0 );

   US_DataModel::RunEntry entry = model.run_entry( index );
   EXPECT_TRUE( entry.orphans );
   EXPECT_TRUE( entry.loaded );
   EXPECT_EQ  ( entry.row, 0 );
   EXPECT_EQ  ( model.row_datadesc( 0 ).recType, (int)US_DataModel::EXPERIMENT );

   // filler raw, the edit, the model (in both places)
   QVector< int > rows = model.descendant_rows( entry.row );
   ASSERT_EQ( rows.size(), 3 );
   EXPECT_EQ( model.row_datadesc( rows[ 0 ] ).recState,
              (int)US_DataModel::NOSTAT );
   EXPECT_EQ( model.row_datadesc( rows[ 2 ] ).recState
              & ( US_DataModel::REC_DB | US_DataModel::REC_LO ),
              US_DataModel::REC_DB | US_DataModel::REC_LO );

   // the run of an orphan is the one its description names
   EXPECT_EQ( model.run_id_of_row( rows[ 2 ] ).toStdString(), "orph.run" );
   EXPECT_EQ( entry.dbCount, 1 );
   EXPECT_EQ( entry.loCount, 2 );
}

// ------------------------------------------------------------ sync states

TEST_F( TestUSDataMerge, BothWithoutAComparisonIsUnverifiedNotInSync )
{
   Desc dbm = record( US_DataModel::MODEL, US_DataModel::REC_DB, modA, editG, "m" );
   Desc lom = record( US_DataModel::MODEL, US_DataModel::REC_LO, modA, editG, "m" );
   Desc dbe = record( US_DataModel::EDIT,  US_DataModel::REC_DB, editG, rawG, "e" );
   Desc loe = record( US_DataModel::EDIT,  US_DataModel::REC_LO, editG, rawG, "e" );
   dbm.contents = "abc 10";                 // one side only has a checksum

   US_DataModel model;
   QVector< Desc > merged = model.merge_descs( QVector< Desc >() << dbe << dbm,
                                               QVector< Desc >() << loe << lom );
   ASSERT_EQ( merged.size(), 2 + 1 );        // a filler raw, the edit, model

   for ( int ii = 1; ii < merged.size(); ii++ )
      EXPECT_EQ( US_DataModel::sync_state( merged[ ii ] ),
                 US_DataModel::SYNC_UNVERIFIED );

   // both checksums: compared
   dbm.contents = "abc 10";
   lom.contents = "abc 10";
   merged = model.merge_descs( QVector< Desc >() << dbe << dbm,
                               QVector< Desc >() << loe << lom );
   EXPECT_EQ( US_DataModel::sync_state( merged.last() ),
              US_DataModel::SYNC_INSYNC );

   lom.contents = "abd 11";
   merged = model.merge_descs( QVector< Desc >() << dbe << dbm,
                               QVector< Desc >() << loe << lom );
   EXPECT_EQ( US_DataModel::sync_state( merged.last() ),
              US_DataModel::SYNC_CONFLICT );
}

TEST_F( TestUSDataMerge, SyncStatesFollowTheComparison )
{
   const int BOTH = US_DataModel::REC_DB | US_DataModel::REC_LO;
   Desc desc = record( US_DataModel::NOISE, BOTH, noiseG, modA, "n" );

   desc.recState = US_DataModel::REC_LO;
   EXPECT_EQ( US_DataModel::sync_state( desc ), US_DataModel::SYNC_LOCAL );

   desc.recState = US_DataModel::REC_DB;
   EXPECT_EQ( US_DataModel::sync_state( desc ), US_DataModel::SYNC_DB );

   desc.recState = US_DataModel::NOSTAT;
   EXPECT_EQ( US_DataModel::sync_state( desc ), US_DataModel::SYNC_DUMMY );

   desc.recState = BOTH;                    // not compared yet
   desc.contents = "";
   EXPECT_EQ( US_DataModel::sync_state( desc ),
              US_DataModel::SYNC_UNVERIFIED );

   desc.recState = BOTH | US_DataModel::HV_DET;
   desc.contents = "abc 10 abc 10";
   EXPECT_EQ( US_DataModel::sync_state( desc ), US_DataModel::SYNC_INSYNC );

   desc.contents = "abc 10 abd 10";
   EXPECT_EQ( US_DataModel::sync_state( desc ), US_DataModel::SYNC_CONFLICT );

   // compared, and one side had no checksum: a conflict, not in sync
   desc.contents = "abc 10 - -";
   EXPECT_EQ( US_DataModel::sync_state( desc ), US_DataModel::SYNC_CONFLICT );
   desc.contents = "- - abc 10";
   EXPECT_EQ( US_DataModel::sync_state( desc ), US_DataModel::SYNC_CONFLICT );
   desc.contents = "";
   EXPECT_EQ( US_DataModel::sync_state( desc ), US_DataModel::SYNC_CONFLICT );
}

// ------------------------------------------------------------ names

TEST_F( TestUSDataMerge, EditSubTypeIsTheRunType )
{
   EXPECT_EQ( US_DataModel::edit_subtype(
              "run1.2401011200.RA.1.A.280.xml" ).toStdString(), "RA" );
   EXPECT_EQ( US_DataModel::edit_subtype(
              "/r/run.with.dots.2401011200.RI.2.B.260.xml" ).toStdString(),
              "RI" );
}

TEST_F( TestUSDataMerge, EditLabelIsRunEditAndTriple )
{
   EXPECT_EQ( US_DataModel::edit_label(
              "run1", "run1.2401011200.RA.1.A.280.xml" ).toStdString(),
              "run1.2401011200.1.A.280" );
   EXPECT_EQ( US_DataModel::edit_label(
              "run.with.dots",
              "run.with.dots.2401011200.RI.2.B.260.xml" ).toStdString(),
              "run.with.dots.2401011200.2.B.260" );
}

TEST_F( TestUSDataMerge, ModelSubType )
{
   EXPECT_EQ( US_DataModel::model_subtype(
              "r.1A280.e1_a1_2DSA-IT_i01.model", "100" ).toStdString(),
              "2DSA-IT" );
   EXPECT_EQ( US_DataModel::model_subtype(
              "r.1A280.e1_a1_2DSA_i01.model", "70000" ).toStdString(),
              "CUSTOMGRID" );
   EXPECT_EQ( US_DataModel::model_subtype(
              "r.1A280.e1_a1_2DSA_i01_mc0001.model", "70000" ).toStdString(),
              "2DSA" );
   EXPECT_EQ( US_DataModel::model_subtype( "manual", "" ).toStdString(),
              "MANUAL" );
}

TEST_F( TestUSDataMerge, DatesOfEitherSourceAreRead )
{
   QDateTime a = US_DataModel::parse_date( "2024-01-02 03:04:05 UTC" );
   QDateTime b = US_DataModel::parse_date( "2024-01-02T03:04:05Z" );
   QDateTime c = US_DataModel::parse_date( "2024-01-02T03:04:06" );

   ASSERT_TRUE( a.isValid() );
   ASSERT_TRUE( b.isValid() );
   ASSERT_TRUE( c.isValid() );
   EXPECT_EQ  ( a, b );
   EXPECT_LT  ( a, c );

   EXPECT_FALSE( US_DataModel::parse_date( "" ).isValid() );
   EXPECT_FALSE( US_DataModel::parse_date( "Tue Jan 2 2024" ).isValid() );
}

// ------------------------------------------------------------ repairs

class TestUSDataRepair : public TestUSDataMerge
{
   protected:
      QTemporaryDir tmp;

      QString xmlOf( US_Model& model )
      {
         QString path = tmp.path() + "/m.xml";
         EXPECT_EQ( model.write( path ), US_DB2::OK );
         QFile file( path );
         EXPECT_TRUE( file.open( QIODevice::ReadOnly ) );
         return QString::fromUtf8( file.readAll() );
      }

      QString xmlOf( US_Noise& noise )
      {
         QString path = tmp.path() + "/n.xml";
         EXPECT_EQ( noise.write( path ), US_DB2::OK );
         QFile file( path );
         EXPECT_TRUE( file.open( QIODevice::ReadOnly ) );
         return QString::fromUtf8( file.readAll() );
      }
};

TEST_F( TestUSDataRepair, ACutModelDescriptionIsReadInFull )
{
   QString full = runID + ".1A280.e2401011200_a2401011300_2DSA-FM-IT_"
                  "with_a_description_long_enough_to_be_cut_i01.model";
   ASSERT_GT( full.length(), 80 );

   US_Model stored;
   stored.modelGUID   = modA;
   stored.editGUID    = editG;
   stored.description = full;
   QString xml = xmlOf( stored );

   Desc mdesc = record( US_DataModel::MODEL, US_DataModel::REC_DB, modA,
                        editG, full.left( 80 ) );
   mdesc.recordID = 42;
   Desc ndesc = record( US_DataModel::NOISE, US_DataModel::REC_DB, noiseG,
                        modA, "" );
   ndesc.recordID = 43;
   ndesc.subType  = "TI";

   QVector< Desc > descs;
   descs << mdesc << ndesc;

   NiceMock< US_DB2_Mock > db;
   EXPECT_CALL( db, query( An< const QStringList& >() ) )
      .WillOnce( [ & ]( const QStringList& q )
      {
         EXPECT_EQ( q, QStringList() << "get_model_info" << "42" );
      } );
   ON_CALL( db, lastErrno() ).WillByDefault( Return( IUS_DB2::OK ) );
   ON_CALL( db, next() ).WillByDefault( Return( true ) );
   ON_CALL( db, value( 2 ) ).WillByDefault( Return( QVariant( xml ) ) );
   ON_CALL( db, value( _ ) ).WillByDefault( Return( QVariant( QString() ) ) );
   ON_CALL( db, value( 2 ) ).WillByDefault( Return( QVariant( xml ) ) );

   US_DataModel::repair_descriptions( descs, &db );

   EXPECT_EQ( descs[ 0 ].description.toStdString(), full.toStdString() );
   EXPECT_LE( descs[ 0 ].label.length(), 40 );

   // the empty noise description is its model's, as a noise
   QString expect = QString( full ).replace( ".model", ".ti_noise" );
   EXPECT_EQ( descs[ 1 ].description.toStdString(), expect.toStdString() );
}

TEST_F( TestUSDataRepair, ACutNoiseDescriptionIsReadInFull )
{
   QString full = runID + ".1A280.e2401011200_a2401011300_2DSA-FM-IT_"
                  "with_a_description_long_enough_to_be_cut_i01.ri_noise";

   US_Noise stored;
   stored.noiseGUID   = noiseG;
   stored.modelGUID   = modA;
   stored.description = full;
   stored.type        = US_Noise::RI;
   stored.values << 0.1 << 0.2;
   stored.count       = 2;
   QString xml = xmlOf( stored );

   Desc ndesc = record( US_DataModel::NOISE, US_DataModel::REC_DB, noiseG,
                        modA, full.left( 80 ) );
   ndesc.recordID = 7;
   ndesc.subType  = "RI";

   QVector< Desc > descs;
   descs << ndesc;

   NiceMock< US_DB2_Mock > db;
   EXPECT_CALL( db, query( An< const QStringList& >() ) )
      .WillOnce( [ & ]( const QStringList& q )
      {
         EXPECT_EQ( q, QStringList() << "get_noise_info" << "7" );
      } );
   ON_CALL( db, lastErrno() ).WillByDefault( Return( IUS_DB2::OK ) );
   ON_CALL( db, next() ).WillByDefault( Return( true ) );
   ON_CALL( db, value( _ ) ).WillByDefault( Return( QVariant( QString() ) ) );
   ON_CALL( db, value( 5 ) ).WillByDefault( Return( QVariant( xml ) ) );

   US_DataModel::repair_descriptions( descs, &db );

   EXPECT_EQ( descs[ 0 ].description.toStdString(), full.toStdString() );
}

// Without a database nothing is read, and nothing breaks
TEST_F( TestUSDataRepair, WithoutADatabaseOnlyTheNoiseIsRepaired )
{
   QString cut = QString( 80, 'x' );
   Desc mdesc = record( US_DataModel::MODEL, US_DataModel::REC_DB, modA,
                        editG, runID + ".1A280.e1_a1_2DSA_i01.model" );
   Desc ndesc = record( US_DataModel::NOISE, US_DataModel::REC_DB, noiseG,
                        modA, "" );
   ndesc.subType = "RI";
   Desc cmdesc = record( US_DataModel::MODEL, US_DataModel::REC_DB, modB,
                         editG, cut );

   QVector< Desc > descs;
   descs << mdesc << ndesc << cmdesc;

   US_DataModel::repair_descriptions( descs, NULL );

   EXPECT_EQ( descs[ 1 ].description.toStdString(),
              ( runID + ".1A280.e1_a1_2DSA_i01.ri_noise" ).toStdString() );
   EXPECT_EQ( descs[ 2 ].description.toStdString(), cut.toStdString() );
}

// ------------------------------------------------------------ the selectors

TEST_F( TestUSDataMerge, TheRunSelectorIsOneQuery )
{
   NiceMock< US_DB2_Mock > db;
   QStringList rows;
   rows << "run.one" << "run_two" << "run.one";
   int at = -1;

   EXPECT_CALL( db, query( An< const QStringList& >() ) )
      .WillOnce( [ & ]( const QStringList& q )
      {
         EXPECT_EQ( q, QStringList() << "get_experiment_desc" << "12" );
      } );
   ON_CALL( db, lastErrno() ).WillByDefault( Return( IUS_DB2::OK ) );
   ON_CALL( db, next() ).WillByDefault( [ & ]() { return ++at < rows.size(); } );
   ON_CALL( db, value( 1 ) ).WillByDefault(
      [ & ]( unsigned ) { return QVariant( rows[ at ] ); } );

   QString     error;
   QStringList ids = US_DataModel::db_run_ids( &db, 12, error );

   EXPECT_TRUE( error.isEmpty() );
   EXPECT_EQ  ( ids, QStringList() << "run.one" << "run_two" );
}

TEST_F( TestUSDataMerge, TheRunSelectorSaysWhenTheQueryFails )
{
   NiceMock< US_DB2_Mock > db;
   ON_CALL( db, lastErrno() ).WillByDefault( Return( IUS_DB2::DBERROR ) );
   ON_CALL( db, lastError() ).WillByDefault( Return( QString( "gone" ) ) );

   QString     error;
   QStringList ids = US_DataModel::db_run_ids( &db, 12, error );

   EXPECT_TRUE( ids.isEmpty() );
   EXPECT_TRUE( error.contains( "gone" ) );
}

TEST_F( TestUSDataMerge, TheTripleSelectorListsRawDataNames )
{
   NiceMock< US_DB2_Mock > db;
   int  step = 0;               // 0: experiment lookup, 1: raw data listing
   int  at   = -1;
   QStringList files;
   files << "run.dots.RA.1.A.280.auc" << "run.dots.RA.2.B.260.auc"
         << "other.RA.3.A.280.auc"   << "run.dots.RA.1.A.280.auc";

   EXPECT_CALL( db, query( An< const QStringList& >() ) )
      .WillOnce( [ & ]( const QStringList& q )
      {
         EXPECT_EQ( q, QStringList() << "get_experiment_info_by_runID"
                                     << "run.dots" << "5" );
      } )
      .WillOnce( [ & ]( const QStringList& q )
      {
         EXPECT_EQ( q, QStringList() << "get_rawDataIDs" << "77" );
         step = 1;
         at   = -1;
      } );
   ON_CALL( db, lastErrno() ).WillByDefault( Return( IUS_DB2::OK ) );
   ON_CALL( db, next() ).WillByDefault( [ & ]()
      { return ( step == 0 ) ? ( ++at == 0 ) : ( ++at < files.size() ); } );
   ON_CALL( db, value( _ ) ).WillByDefault( [ & ]( unsigned col )
      {
         if ( step == 0 )   return QVariant( col == 1 ? "77" : "" );
         return QVariant( col == 2 ? files[ at ] : QString() );
      } );

   QString     error;
   QStringList trips = US_DataModel::db_triples( &db, "run.dots", 5, error );

   EXPECT_TRUE( error.isEmpty() );
   EXPECT_EQ  ( trips, QStringList() << "1.A.280" << "2.B.260" );
}

// ------------------------------------------------------------ upload guard

// A local copy older than the database copy is not uploaded over it.  Both
// sources write their dates as "yyyy-MM-dd hh:mm:ss UTC", which the old
// check could not read, so it never refused anything.
TEST_F( TestUSDataMerge, AnOlderLocalCopyIsNotUploadedOverTheDatabase )
{
   const int BOTH = US_DataModel::REC_DB | US_DataModel::REC_LO;

   Desc raw  = record( US_DataModel::RAW,  BOTH, rawG, "", runID + ".1.A.280" );
   Desc edit = record( US_DataModel::EDIT, BOTH, editG, rawG,
                       runID + ".2401011200.1.A.280" );
   edit.recordID    = 5;
   edit.filename    = "/nowhere/" + runID + ".2401011200.RA.1.A.280.xml";
   edit.lastmodDate = "2024-03-01 10:00:00 UTC";     // DB copy
   edit.filemodDate = "2024-02-01 10:00:00 UTC";     // local copy, older

   US_DataModel model;
   ASSERT_EQ( model.add_orphans( QVector< Desc >(),
                                 QVector< Desc >() << raw << edit ), 0 );

   int erow = -1;

   for ( int row = 0; row < model.recCount(); row++ )
      if ( model.row_datadesc( row ).dataGUID == editG )
         erow = row;

   ASSERT_GT( erow, 0 );

   US_DataProcess proc( &model );

   EXPECT_EQ( proc.record_upload( erow ), 3061 );
   EXPECT_TRUE( proc.lastError().contains( "older" ) )
      << proc.lastError().toStdString();
}
