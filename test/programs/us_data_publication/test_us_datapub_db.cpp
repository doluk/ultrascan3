// test_us_datapub_db.cpp - the publication catalog against a real database
//
// us_data_publication reads the record chain through US_DataCatalog, which
// prefers the catalog procedures added for it and falls back on the ones
// UltraScan has always had when a server does not have them.  An
// installation that has not run the new SQL still has to be able to
// publish its data, so these tests are meant to be run twice: against a
// database that has the new procedures, and against one that does not.
//
//   US3_TEST_DB_HOST=127.0.0.1:3306 US3_TEST_DB_NAME=us3test
//   US3_TEST_DB_USER=us3 US3_TEST_DB_PASS=...
//   US3_TEST_DB_PERSON_GUID=... US3_TEST_DB_PERSON_PW=...
//   US3_TEST_DB_PERSON_ID=2 ./bin/test_us_datapub
//
// The test about a changed investigator needs a second person to switch to,
// and an administrator login, since only an administrator may read another
// person's records.  Without these it skips:
//
//   US3_TEST_DB_ADMIN_GUID=... US3_TEST_DB_ADMIN_PW=...
//   US3_TEST_DB_OTHER_ID=3
//
// Without those they skip, so an ordinary build needs no database.
#include <gtest/gtest.h>

#include <QtCore>

#include "datapub_test_env.h"

#include "us_datapub_catalog.h"
#include "us_datapub_export.h"
#include "us_datapub_import.h"
#include "us_datapub_manifest.h"
#include "us_datapub_hash.h"
#include "us_datapub_bundle.h"
#include "us_db2.h"
#include "us_crypto.h"
#include "us_settings.h"
#include "us_time_state.h"
#include "us_analyte.h"

class DataPubDb : public ::testing::Test
{
   protected:
      US_DB2*     db = nullptr;
      QString     master;
      QStringList savedDB;
      int         savedInv = 0;
      bool        restore  = false;

      void SetUp() override
      {
         QString host = qEnvironmentVariable( "US3_TEST_DB_HOST" );
         QString name = qEnvironmentVariable( "US3_TEST_DB_NAME" );
         QString user = qEnvironmentVariable( "US3_TEST_DB_USER" );
         QString guid = qEnvironmentVariable( "US3_TEST_DB_PERSON_GUID" );

         if ( host.isEmpty()  ||  name.isEmpty()  ||
              user.isEmpty()  ||  guid.isEmpty() )
            GTEST_SKIP() << "no test database configured";

         master = "us3-datapub-test";

         QStringList dbCipher   = US_Crypto::encrypt(
               qEnvironmentVariable( "US3_TEST_DB_PASS" ), master );
         QStringList userCipher = US_Crypto::encrypt(
               qEnvironmentVariable( "US3_TEST_DB_PERSON_PW" ), master );

         QStringList entry;
         entry << "us3 test database" << user << name << host
               << dbCipher.at( 0 ) << dbCipher.at( 1 )
               << qEnvironmentVariable( "US3_TEST_DB_EMAIL" )
               << userCipher.at( 0 ) << userCipher.at( 1 ) << guid;

         savedDB  = US_Settings::defaultDB();
         savedInv = US_Settings::us_inv_ID();
         restore  = true;

         US_Settings::set_defaultDB( entry );
         US_Settings::set_us_inv_ID(
               qEnvironmentVariable( "US3_TEST_DB_PERSON_ID" ).toInt() );

         db = new US_DB2();
         QString error;

         if ( ! db->connect( master, error ) )
         {
            delete db;
            db = nullptr;
            US_Settings::set_defaultDB( savedDB );
            restore = false;
            GTEST_SKIP() << "cannot reach the test database: "
                         << error.toStdString();
         }
      }

      void TearDown() override
      {
         delete db;
         db = nullptr;

         if ( restore )
         {
            US_Settings::set_defaultDB ( savedDB  );
            US_Settings::set_us_inv_ID ( savedInv );
            restore = false;
         }
      }

      // True when the server has the procedures added for the catalog
      bool hasCatalogProcedures()
      {
         db->rawQuery( "CALL get_experiment_summary( '', '', 0 )" );

         return ! db->lastError().contains( "does not exist",
                                            Qt::CaseInsensitive );
      }

      // Log in as an administrator, who alone may ask the database for
      // another person's records, and say whether that was possible
      bool loginAsAdmin()
      {
         QString guid = qEnvironmentVariable( "US3_TEST_DB_ADMIN_GUID" );
         QString pass = qEnvironmentVariable( "US3_TEST_DB_ADMIN_PW"   );

         if ( guid.isEmpty() )  return false;

         QStringList cipher = US_Crypto::encrypt( pass, master );
         QStringList entry  = US_Settings::defaultDB();

         if ( entry.size() < 10 )  return false;

         entry.replace( 7, cipher.at( 0 ) );
         entry.replace( 8, cipher.at( 1 ) );
         entry.replace( 9, guid );

         US_Settings::set_defaultDB( entry );

         return true;
      }

      // The run of the store that has the most under it
      int busiestRun( US_DataPubCatalog& catalog,
                      QList< US_DataPubCatalog::Run >& runs, QString& error )
      {
         int found = -1;
         int most  = 0;

         for ( int ii = 0; ii < runs.size(); ii++ )
         {
            if ( ! catalog.loadRunDetails( runs[ ii ], error ) )  continue;

            int knt = 0;

            for ( int jj = 0; jj < runs[ ii ].raws.size(); jj++ )
               knt += 1 + runs[ ii ].raws[ jj ].edits.size();

            if ( knt <= most )  continue;

            most  = knt;
            found = ii;
         }

         return found;
      }
};

// Whatever the server has, the catalog lists the experiments
TEST_F( DataPubDb, TheExperimentsAreListed )
{
   US_DataPubCatalog catalog;
   QString           error;

   ASSERT_TRUE( catalog.open( true, master, error ) )
      << error.toStdString();

   QList< US_DataPubCatalog::Run > runs = catalog.runs( QString(), error );

   ASSERT_TRUE( error.isEmpty() ) << error.toStdString();
   ASSERT_GT  ( runs.size(), 0 );

   for ( int ii = 0; ii < runs.size(); ii++ )
   {
      EXPECT_FALSE( runs[ ii ].runID.isEmpty() );
      EXPECT_FALSE( runs[ ii ].id   .isEmpty() );
      EXPECT_NE   ( runs[ ii ].id.toStdString(), std::string( "-1" ) );
   }
}

// ... and reads the chain below one of them, procedures or no procedures
TEST_F( DataPubDb, TheChainOfOneExperimentIsRead )
{
   US_DataPubCatalog catalog;
   QString           error;

   ASSERT_TRUE( catalog.open( true, master, error ) );

   QList< US_DataPubCatalog::Run > runs = catalog.runs( QString(), error );
   ASSERT_GT( runs.size(), 0 );

   int index = busiestRun( catalog, runs, error );
   ASSERT_GE( index, 0 ) << "no experiment has any raw data";

   const US_DataPubCatalog::Run& run = runs[ index ];

   ASSERT_GT( run.raws.size(), 0 );

   int edits = 0;

   for ( int ii = 0; ii < run.raws.size(); ii++ )
   {
      EXPECT_FALSE( run.raws[ ii ].guid    .isEmpty() );
      EXPECT_FALSE( run.raws[ ii ].filename.isEmpty() );

      edits += run.raws[ ii ].edits.size();

      for ( int jj = 0; jj < run.raws[ ii ].edits.size(); jj++ )
      {
         EXPECT_FALSE( run.raws[ ii ].edits[ jj ].guid.isEmpty() );
         EXPECT_EQ   ( run.raws[ ii ].edits[ jj ].rawGUID.toStdString(),
                       run.raws[ ii ].guid.toStdString() );
      }
   }

   EXPECT_GT( edits, 0 );

   // The models and the noise of those edits come with it
   QList< US_DataPubCatalog::Run >   one;
   one << run;

   QList< US_DataPubCatalog::Model > models = catalog.models( one, error );
   ASSERT_TRUE( error.isEmpty() ) << error.toStdString();

   for ( int ii = 0; ii < models.size(); ii++ )
   {
      EXPECT_FALSE( models[ ii ].guid    .isEmpty() );
      EXPECT_FALSE( models[ ii ].editGUID.isEmpty() );
   }

   QList< US_DataPubCatalog::Noise > noises = catalog.noises( models, error );
   ASSERT_TRUE( error.isEmpty() ) << error.toStdString();

   for ( int ii = 0; ii < noises.size(); ii++ )
      EXPECT_FALSE( noises[ ii ].modelGUID.isEmpty() );
}

// One run looked up by its identifier, which is what an export does with
// the runs a user picked
TEST_F( DataPubDb, ARunIsFoundByItsIdentifier )
{
   US_DataPubCatalog catalog;
   QString           error;

   ASSERT_TRUE( catalog.open( true, master, error ) );

   QList< US_DataPubCatalog::Run > runs = catalog.runs( QString(), error );
   ASSERT_GT( runs.size(), 0 );

   US_DataPubCatalog::Run found;
   ASSERT_TRUE( catalog.runByID( runs[ 0 ].runID, found, error ) )
      << error.toStdString();

   EXPECT_EQ( found.runID.toStdString(), runs[ 0 ].runID.toStdString() );
   EXPECT_EQ( found.guid .toStdString(), runs[ 0 ].guid .toStdString() );

   // and one that is not there is an error, not a crash
   US_DataPubCatalog::Run missing;
   EXPECT_FALSE( catalog.runByID( "no-such-run-at-all", missing, error ) );
   EXPECT_FALSE( error.isEmpty() );
}

// The experiment XML an export needs, which on the database side is built
// from the experiment record rather than read from a file
TEST_F( DataPubDb, TheExperimentInformationIsRead )
{
   US_DataPubCatalog catalog;
   QString           error;

   ASSERT_TRUE( catalog.open( true, master, error ) );

   QList< US_DataPubCatalog::Run > runs = catalog.runs( QString(), error );
   ASSERT_GT( runs.size(), 0 );

   int index = busiestRun( catalog, runs, error );
   ASSERT_GE( index, 0 );

   US_DataPubCatalog::ExpInfo info;
   ASSERT_TRUE( catalog.expInfo( runs[ index ], info, error ) )
      << error.toStdString();

   EXPECT_EQ   ( info.runID.toStdString(),
                 runs[ index ].runID.toStdString() );
   EXPECT_FALSE( info.expGUID.isEmpty() );
}

// The whole point: the same chain comes back whether the server has the
// catalog procedures or not.  Turning them off is what an old server looks
// like to the catalog.
TEST_F( DataPubDb, TheOldProceduresGiveTheSameChain )
{
   QList< US_DataPubCatalog::Run > both[ 2 ];

   for ( int pass = 0; pass < 2; pass++ )
   {
      US_DataPubCatalog catalog;
      QString           error;

      ASSERT_TRUE( catalog.open( true, master, error ) );

      if ( pass == 1 )
         catalog.setBulkQueries( false );   // as if the server had none

      QList< US_DataPubCatalog::Run > listed = catalog.runs( QString(), error );
      ASSERT_TRUE( error.isEmpty() ) << error.toStdString();

      for ( int ii = 0; ii < listed.size(); ii++ )
      {
         // A run with no raw data cannot be published, and is refused the
         // same way whichever procedures answered
         if ( ! catalog.loadRunDetails( listed[ ii ], error ) )
         {
            EXPECT_FALSE( error.isEmpty() );
            continue;
         }

         both[ pass ] << listed[ ii ];
      }
   }

   ASSERT_EQ( both[ 1 ].size(), both[ 0 ].size() );
   ASSERT_GT( both[ 0 ].size(), 0 );

   for ( int ii = 0; ii < both[ 0 ].size(); ii++ )
   {
      const US_DataPubCatalog::Run& neu = both[ 0 ][ ii ];
      const US_DataPubCatalog::Run& old = both[ 1 ][ ii ];

      ASSERT_EQ( old.runID.toStdString(), neu.runID.toStdString() );
      EXPECT_EQ( old.guid .toStdString(), neu.guid .toStdString() );
      EXPECT_EQ( old.id   .toStdString(), neu.id   .toStdString() );
      EXPECT_EQ( old.projectGUID.toStdString(),
                 neu.projectGUID.toStdString() );
      ASSERT_EQ( old.raws.size(), neu.raws.size() ) << neu.runID.toStdString();

      for ( int jj = 0; jj < neu.raws.size(); jj++ )
      {
         EXPECT_EQ( old.raws[ jj ].guid.toStdString(),
                    neu.raws[ jj ].guid.toStdString() );
         EXPECT_EQ( old.raws[ jj ].filename.toStdString(),
                    neu.raws[ jj ].filename.toStdString() );
         ASSERT_EQ( old.raws[ jj ].edits.size(),
                    neu.raws[ jj ].edits.size() );

         for ( int kk = 0; kk < neu.raws[ jj ].edits.size(); kk++ )
            EXPECT_EQ( old.raws[ jj ].edits[ kk ].guid.toStdString(),
                       neu.raws[ jj ].edits[ kk ].guid.toStdString() );
      }
   }
}

// Say which of the two a run was measured against, so a run of this suite
// against an old server is recognisable in the log
TEST_F( DataPubDb, TheServerIsReported )
{
   bool present = hasCatalogProcedures();

   qDebug() << "DataPubDb: this server"
            << ( present ? "has" : "does NOT have" )
            << "the catalog procedures";

   SUCCEED();
}

// The end of it: a bundle really is written from the database.  This is
// the check that matters for an installation that has not run the new SQL
// -- the catalog falling back is only useful if an export comes out of it.
TEST_F( DataPubDb, ABundleIsExportedFromTheDatabase )
{
   US_DataPubCatalog catalog;
   QString           error;

   ASSERT_TRUE( catalog.open( true, master, error ) );

   QList< US_DataPubCatalog::Run > runs = catalog.runs( QString(), error );
   ASSERT_GT( runs.size(), 0 );

   // the smallest run that has anything in it, so the test stays quick
   int     index = -1;
   int     fewest = 0;
   QString chosen;

   for ( int ii = 0; ii < runs.size(); ii++ )
   {
      if ( ! catalog.loadRunDetails( runs[ ii ], error ) )  continue;

      int knt = runs[ ii ].raws.size();

      if ( knt < 1 )                          continue;
      if ( index >= 0  &&  knt >= fewest )    continue;

      fewest = knt;
      index  = ii;
      chosen = runs[ ii ].runID;
   }

   ASSERT_GE( index, 0 ) << "no experiment has any raw data";

   QString root = QDir::tempPath() + "/us_datapub_db_export_"
                  + QString::number( QCoreApplication::applicationPid() );
   QDir().mkpath( root );

   QString bundle = root + "/from_db.tar.gz";

   US_DataPubExporter            exporter;
   US_DataPubExporter::Selection selection;
   selection.fromDb     = true;
   selection.dbPassword = master;
   selection.scope      = US_DataPub::ScopeEdits;
   selection.runIDs << chosen;

   // An export that fails is a failure of this test, whatever the reason:
   // a run the catalog lists with raw data has to be exportable
   bool made = exporter.exportBundle( selection, bundle, error );

   if ( ! made )  QDir( root ).removeRecursively();

   ASSERT_TRUE( made ) << "this database cannot export "
                       << chosen.toStdString() << ": "
                       << error.toStdString();

   EXPECT_TRUE( QFile::exists( bundle ) );
   EXPECT_GT  ( QFileInfo( bundle ).size(), 0 );
   EXPECT_GT  ( exporter.manifest().count( US_DataPub::Experiment ), 0 );
   EXPECT_GT  ( exporter.manifest().count( US_DataPub::RawData    ), 0 );

   qDebug() << "DataPubDb: exported" << chosen
            << exporter.manifest().total() << "records from the database";

   QDir( root ).removeRecursively();
}

/* The fixture's run, exported whole.

   test/utils/data/us3_db_fixture.sql holds one run with everything a run
   has: its project, rotor calibration and centerpiece, the solution with
   its buffer (two components, an extinction profile) and analyte (an
   extinction profile) and a profile of its own, three triples, edits, a
   model with its noise and a time state.  Every record has to arrive, and
   the values the library files round have to arrive whole.  Skips on a
   database that is not the fixture.
*/
TEST_F( DataPubDb, TheFixtureRunIsExportedWithItsWholeChain )
{
   US_DataPubCatalog catalog;
   QString           error;

   ASSERT_TRUE( catalog.open( true, master, error ) );

   US_DataPubCatalog::Run run;

   if ( ! catalog.runByID( "MWL_demo_run", run, error ) )
      GTEST_SKIP() << "not the fixture database: " << error.toStdString();

   QString root = QDir::tempPath() + "/us_datapub_db_chain_"
                  + QString::number( QCoreApplication::applicationPid() );
   QDir().mkpath( root );

   US_DataPubExporter            exporter;
   US_DataPubExporter::Selection selection;
   selection.fromDb     = true;
   selection.dbPassword = master;
   selection.scope      = US_DataPub::ScopeNoise;
   selection.runIDs << "MWL_demo_run";

   bool made = exporter.exportBundle( selection, root + "/chain.tar.gz",
                                      error );

   if ( ! made )  QDir( root ).removeRecursively();

   ASSERT_TRUE( made ) << error.toStdString() << "\n"
                       << exporter.log().join( "\n" ).toStdString();

   const US_DataPubManifest& mani = exporter.manifest();
   SCOPED_TRACE( exporter.log().join( "\n" ).toStdString() );

   EXPECT_EQ( mani.count( US_DataPub::Project          ), 1 );
   EXPECT_EQ( mani.count( US_DataPub::RotorCalibration ), 1 );
   EXPECT_EQ( mani.count( US_DataPub::Centerpiece      ), 1 );
   EXPECT_EQ( mani.count( US_DataPub::Experiment       ), 1 );
   EXPECT_EQ( mani.count( US_DataPub::TimeState        ), 1 );
   EXPECT_EQ( mani.count( US_DataPub::Buffer           ), 1 );
   EXPECT_EQ( mani.count( US_DataPub::Analyte          ), 1 );
   EXPECT_EQ( mani.count( US_DataPub::Solution         ), 1 );
   EXPECT_EQ( mani.count( US_DataPub::RawData          ), 3 );
   EXPECT_EQ( mani.count( US_DataPub::EditedData       ), 3 );
   EXPECT_EQ( mani.count( US_DataPub::Model            ), 1 );
   EXPECT_EQ( mani.count( US_DataPub::Noise            ), 1 );

   US_DataPubBundle unpacked;
   ASSERT_TRUE( unpacked.unpack( root + "/chain.tar.gz", error ) )
      << error.toStdString();
   QString top = unpacked.rootPath() + "/";

   auto spectra = [ &top ]( const US_DataPubEntity& entity )
   {
      US_DataPubRecords::Spectra read;
      US_DataPubRecords::readSpectra(
            top + entity.attrs.value( "spectraPayload" ), read );
      return read.value( "extinction" );
   };

   // The values as the database holds them, not as the files round them
   ASSERT_EQ( mani.count( US_DataPub::Buffer ), 1 );
   US_DataPubEntity buffer = mani.section( US_DataPub::Buffer )[ 0 ];
   QMap< double, double > bext = spectra( buffer );
   EXPECT_EQ( bext.value( 260.0 ), 0.0123456789 );
   EXPECT_EQ( bext.value( 280.0 ), 0.00987654321 );

   QStringList     ids;
   QList< double > concs;
   ASSERT_TRUE( US_DataPubRecords::readBufferConcentrations(
                top + buffer.attrs.value( "componentsPayload" ), ids, concs ) );
   EXPECT_EQ( ids, QStringList() << "70" << "92" );
   // 0.000525 needs six decimals: a buffer file keeps five (0.00053)
   EXPECT_EQ( concs, QList< double >() << 150.0 << 0.000525 );

   // The time state is the one US_TimeState wrote, readable record by record
   ASSERT_EQ( mani.count( US_DataPub::TimeState ), 1 );
   QString why;
   EXPECT_TRUE( DataPubTestEnv::isTheTimeState(
                top + mani.section( US_DataPub::TimeState )[ 0 ].payload,
                why ) ) << why.toStdString();

   ASSERT_EQ( mani.count( US_DataPub::Analyte ), 1 );
   EXPECT_EQ( spectra( mani.section( US_DataPub::Analyte )[ 0 ] )
              .value( 280.0 ), 43824.1234567 );

   ASSERT_EQ( mani.count( US_DataPub::Solution ), 1 );
   EXPECT_EQ( spectra( mani.section( US_DataPub::Solution )[ 0 ] )
              .value( 280.0 ), 43824.987654321 );

   unpacked.cleanup();
   QDir( root ).removeRecursively();
}

// A run whose record names no rotor (NULL) is read without asking a strict
// server about the rotor ''
TEST_F( DataPubDb, ARunWithoutARotorIsRead )
{
   US_DataPubCatalog catalog;
   QString           error;

   ASSERT_TRUE( catalog.open( true, master, error ) );

   US_DataPubCatalog::Run run;

   if ( ! catalog.runByID( "Empty_run", run, error ) )
      GTEST_SKIP() << "not the fixture database: " << error.toStdString();

   US_DataPubCatalog::ExpInfo info;
   ASSERT_TRUE( catalog.expInfo( run, info, error ) ) << error.toStdString();
   EXPECT_TRUE( info.rotorID.isEmpty() );
   EXPECT_TRUE( info.calibrationID.isEmpty() );
}

/* Switching the investigator has to reach the queries.

   The investigator is chosen from a dialog while a catalog is open -- the
   run selection dialog offers one of its own -- and the database holds one
   person's records at a time.  A listing read for the person who was
   selected before is no answer about the run the user has just picked out
   of the dialog, which is how a run that is plainly there came back as
   "No run was found in the database".
*/
TEST_F( DataPubDb, SwitchingTheInvestigatorChangesWhatIsFound )
{
   if ( ! loginAsAdmin() )
      GTEST_SKIP() << "no administrator login configured";

   int first  = qEnvironmentVariable( "US3_TEST_DB_PERSON_ID" ).toInt();
   int second = qEnvironmentVariable( "US3_TEST_DB_OTHER_ID"  ).toInt();

   if ( second == 0  ||  second == first )
      GTEST_SKIP() << "no second investigator configured";

   US_Settings::set_us_inv_ID( first );

   US_DataPubCatalog catalog;
   QString           error;

   ASSERT_TRUE( catalog.open( true, master, error ) ) << error.toStdString();

   QList< US_DataPubCatalog::Run > mine = catalog.runs( QString(), error );

   ASSERT_TRUE( error.isEmpty() ) << error.toStdString();
   ASSERT_GT  ( mine.size(), 0 ) << "the first investigator has no runs";

   // That listing is what the catalog now has to hand
   US_DataPubCatalog::Run found;

   ASSERT_TRUE( catalog.runByID( mine[ 0 ].runID, found, error ) )
      << error.toStdString();

   // The user picks the other investigator in the dialog, which lists that
   // person's runs with a query of its own
   US_Settings::set_us_inv_ID( second );

   US_DataPubCatalog dialog;

   ASSERT_TRUE( dialog.open( true, master, error ) ) << error.toStdString();

   QList< US_DataPubCatalog::Run > theirs = dialog.runs( QString(), error );

   ASSERT_TRUE( error.isEmpty() ) << error.toStdString();
   ASSERT_GT  ( theirs.size(), 0 ) << "the second investigator has no runs";

   QStringList ids;

   for ( int ii = 0; ii < theirs.size(); ii++ )
      ids << theirs[ ii ].runID;

   ASSERT_FALSE( ids.contains( mine[ 0 ].runID ) )
      << "the two investigators share a run, so nothing is being told apart";

   // The run the dialog is showing is the one the open catalog has to find
   ASSERT_TRUE( catalog.runByID( theirs[ 0 ].runID, found, error ) )
      << error.toStdString();

   EXPECT_EQ( found.runID.toStdString(), theirs[ 0 ].runID.toStdString() );
   EXPECT_EQ( catalog.investigatorID(), second );

   // ... and what it lists is that person's work, not the previous one's
   QStringList listed;
   QList< US_DataPubCatalog::Run > now = catalog.runs( QString(), error );

   for ( int ii = 0; ii < now.size(); ii++ )
      listed << now[ ii ].runID;

   EXPECT_FALSE( listed.contains( mine[ 0 ].runID ) )
      << "the listing of the person selected before is still being shown";

   // A run of the person no longer selected is not theirs to publish
   EXPECT_FALSE( catalog.runByID( mine[ 0 ].runID, found, error ) );

   // ... and switching back brings the first person's work back
   US_Settings::set_us_inv_ID( first );

   ASSERT_TRUE( catalog.runByID( mine[ 0 ].runID, found, error ) )
      << error.toStdString();

   EXPECT_EQ( catalog.investigatorID(), first );
}

/* A time state is a pair of files, and the archive has to carry both.

   The binary readings mean nothing without the XML that says what the
   fields in them are, and US_TimeState looks for that XML beside the
   binary, under the same name.  A database export fetches the two into a
   working directory of its own; the path of the XML used to be built by
   rewriting ".tmst" wherever it appeared in the path of the binary, which
   rewrote the name of that working directory too, so the definitions were
   written into a directory that did not exist and never reached the
   archive.  An import then had nothing to rebuild the record from.
*/
TEST_F( DataPubDb, TheExportedTimeStateCarriesItsDefinitions )
{
   US_DataPubCatalog catalog;
   QString           error;

   ASSERT_TRUE( catalog.open( true, master, error ) ) << error.toStdString();

   QList< US_DataPubCatalog::Run > runs = catalog.runs( QString(), error );
   ASSERT_GT( runs.size(), 0 );

   QString chosen;

   for ( int ii = 0; ii < runs.size()  &&  chosen.isEmpty(); ii++ )
   {
      int       tmstID = 0;
      int       expID  = runs[ ii ].id.toInt();
      QString   fname;
      QString   xdefs;
      QString   cksum;
      QDateTime updated;

      US_TimeState::dbExamine( db, &tmstID, &expID, &fname, &xdefs, &cksum,
                               &updated );

      if ( tmstID > 0 )  chosen = runs[ ii ].runID;
   }

   if ( chosen.isEmpty() )
      GTEST_SKIP() << "no experiment of this database has a time state";

   QString root = QDir::tempPath() + "/us_datapub_db_tmst_"
                  + QString::number( QCoreApplication::applicationPid() );
   QDir().mkpath( root );

   QString bundle = root + "/time_state.tar.gz";

   US_DataPubExporter            exporter;
   US_DataPubExporter::Selection selection;
   selection.fromDb     = true;
   selection.dbPassword = master;
   selection.scope      = US_DataPub::ScopeRawData;
   selection.runIDs << chosen;

   bool made = exporter.exportBundle( selection, bundle, error );

   if ( ! made )  QDir( root ).removeRecursively();

   ASSERT_TRUE( made ) << "the run could not be exported: "
                       << error.toStdString();

   QList< US_DataPubEntity > states =
      exporter.manifest().section( US_DataPub::TimeState );

   ASSERT_EQ( states.size(), 1 );

   QString defs = states[ 0 ].attrs.value( "definitionsPayload" );

   EXPECT_FALSE( defs.isEmpty() )
      << "the manifest names no field definitions";

   US_DataPubBundle unpacked;

   ASSERT_TRUE( unpacked.unpack( bundle, error ) ) << error.toStdString();

   QString tmst = unpacked.rootPath() + "/" + states[ 0 ].payload;
   QString xdef = unpacked.rootPath() + "/" + defs;

   EXPECT_TRUE( QFile::exists( tmst ) ) << states[ 0 ].payload.toStdString();
   EXPECT_TRUE( QFile::exists( xdef ) ) << defs.toStdString();

   // US_TimeState::dbCreate() takes the path of the binary and reads the
   // definitions from beside it, so the two have to land together
   EXPECT_EQ( QFileInfo( xdef ).path().toStdString(),
              QFileInfo( tmst ).path().toStdString() );

   unpacked.cleanup();
   QDir( root ).removeRecursively();
}

/* Writing a whole bundle into a database.

   The store this exports is the synthetic one on disk, so the payloads are
   real files an import can rebuild records from -- which the read-only
   fixture in the database is not meant to be.  It writes, so it needs a
   scratch database of its own and skips without one:

     US3_TEST_DB_IMPORT_NAME=us3imp

   It has to be a database with the UltraScan3 schema and procedures, one
   person matching US3_TEST_DB_PERSON_GUID, and the lab, instrument, rotor
   and operator permit an experiment record needs.
*/
class DataPubDbImport : public DataPubTestEnv
{
   protected:
      QString     master;
      QStringList savedDB;
      int         savedInv  = 0;
      bool        restore   = false;
      US_DB2*     db        = nullptr;

      void SetUp() override
      {
         QString host = qEnvironmentVariable( "US3_TEST_DB_HOST" );
         QString name = qEnvironmentVariable( "US3_TEST_DB_IMPORT_NAME" );
         QString user = qEnvironmentVariable( "US3_TEST_DB_USER" );
         QString guid = qEnvironmentVariable( "US3_TEST_DB_PERSON_GUID" );

         if ( host.isEmpty()  ||  name.isEmpty()  ||
              user.isEmpty()  ||  guid.isEmpty() )
            GTEST_SKIP() << "no scratch import database configured";

         master = "us3-datapub-import";

         QStringList dbCipher   = US_Crypto::encrypt(
               qEnvironmentVariable( "US3_TEST_DB_PASS" ), master );
         QStringList userCipher = US_Crypto::encrypt(
               qEnvironmentVariable( "US3_TEST_DB_PERSON_PW" ), master );

         QStringList entry;
         entry << "us3 import database" << user << name << host
               << dbCipher.at( 0 ) << dbCipher.at( 1 )
               << qEnvironmentVariable( "US3_TEST_DB_EMAIL" )
               << userCipher.at( 0 ) << userCipher.at( 1 ) << guid;

         savedDB  = US_Settings::defaultDB();
         savedInv = US_Settings::us_inv_ID();
         restore  = true;

         US_Settings::set_defaultDB( entry );
         US_Settings::set_us_inv_ID(
               qEnvironmentVariable( "US3_TEST_DB_PERSON_ID" ).toInt() );

         db = new US_DB2();
         QString error;

         if ( ! db->connect( master, error ) )
         {
            delete db;
            db = nullptr;
            US_Settings::set_defaultDB( savedDB );
            restore = false;
            GTEST_SKIP() << "cannot reach the import database: "
                         << error.toStdString();
         }

         // The store on disk comes last: it moves workBaseDir, and the
         // settings above have to be in place whether or not it is built
         DataPubTestEnv::SetUp();
      }

      void TearDown() override
      {
         delete db;
         db = nullptr;

         if ( restore )
         {
            US_Settings::set_defaultDB ( savedDB  );
            US_Settings::set_us_inv_ID ( savedInv );
            restore = false;

            DataPubTestEnv::TearDown();
         }
      }
};

/* A time state comes back out of the database it was imported into.

   US_TimeState keeps the field definitions in a file beside the binary
   readings, and rebuilds the record from the pair.  The import copies one
   payload at a time into a work directory, so unless the definitions are
   put there too the pair is incomplete and the record cannot be written.
*/
TEST_F( DataPubDbImport, ATimeStateIsImportedWithItsDefinitions )
{
   ASSERT_TRUE( QFile::exists( timeStatePath( "tmst" ) ) );

   QString bundle = root + "/to_db.tar.gz";
   QString error;

   US_DataPubExporter            exporter;
   US_DataPubExporter::Selection selection;
   selection.fromDb = false;
   selection.scope  = US_DataPub::ScopeNoise;
   selection.runIDs << runID;

   ASSERT_TRUE( exporter.exportBundle( selection, bundle, error ) )
      << error.toStdString();

   ASSERT_EQ( exporter.manifest().count( US_DataPub::TimeState ), 1 );

   US_DataPubImporter importer;

   ASSERT_TRUE( importer.inspect( bundle, error ) ) << error.toStdString();

   US_DataPubImporter::Options options;
   options.target       = US_DataPub::TargetDb;
   options.dbPassword   = master;
   options.verifyHashes = true;
   options.dryRun       = false;
   options.policy       = US_DataPub::PolicyRename;

   ASSERT_TRUE( importer.runImport( options, error ) )
      << error.toStdString() << "\n" << importer.log().join( "\n" )
                                                      .toStdString();

   // Find what the experiment and the time state became in the database
   QList< US_DataPubImporter::Result > results = importer.results();
   QString expID;
   QString tmstID;

   for ( int ii = 0; ii < results.size(); ii++ )
   {
      if ( results[ ii ].type == US_DataPub::Experiment )
         expID  = results[ ii ].targetID;

      if ( results[ ii ].type == US_DataPub::TimeState )
         tmstID = results[ ii ].targetID;
   }

   ASSERT_FALSE( expID .isEmpty() ) << "the experiment was not imported";
   ASSERT_FALSE( tmstID.isEmpty() ) << "the time state was not imported";
   EXPECT_GT   ( tmstID.toInt(), 0 );

   // ... and that it is a whole record: the definitions came with it
   int       readID = 0;
   int       readExp = expID.toInt();
   QString   fname;
   QString   xdefs;
   QString   cksum;
   QDateTime updated;

   US_TimeState::dbExamine( db, &readID, &readExp, &fname, &xdefs, &cksum,
                            &updated );

   EXPECT_EQ   ( readID, tmstID.toInt() );
   EXPECT_FALSE( fname.isEmpty() );
   EXPECT_FALSE( xdefs.isEmpty() )
      << "the record holds no field definitions";
   EXPECT_TRUE ( xdefs.contains( "TimeState" ) ) << xdefs.toStdString();
}


/* Borrowed noise, through a real server.  A model with no noise of its own
   travels with the noise of its edit, which still belongs to the model it
   was fitted to; the bundle used to list it under the borrowing model, and
   every import refused it.  Exported as the export tab does it -- the later
   model and its borrowed noise ticked -- it now imports into a database,
   and the noise is stored with the model it was fitted to.
*/
TEST_F( DataPubDbImport, BorrowedNoiseIsImportedWithItsOwnModel )
{
   QString  laterGUID = US_Util::new_guid();
   US_Model later;
   later.modelGUID   = laterGUID;
   later.editGUID    = editGUID;
   later.description = "demo_run.1A280.2dsa-mc.model";
   later.write( US_Settings::dataDir() + "/models/M0000002.xml" );

   US_DataPubExporter            exporter;
   US_DataPubExporter::Selection selection;
   selection.fromDb = false;
   selection.scope  = US_DataPub::ScopeNoise;
   selection.runIDs << runID;
   selection.setModels( QStringList() << laterGUID );
   selection.setNoises( QStringList() << noiseGUID );

   QString bundle = root + "/borrowed.tar.gz";
   QString error;

   ASSERT_TRUE( exporter.exportBundle( selection, bundle, error ) )
      << error.toStdString();

   US_DataPubEntity noise;
   ASSERT_TRUE( exporter.manifest().entity( US_DataPub::Noise, noiseGUID,
                                            noise ) );
   EXPECT_EQ( noise.depend( US_DataPub::Model ), modelGUID );
   EXPECT_EQ( noise.attrs.value( "borrowedByModelGUID" ), laterGUID );

   US_DataPubImporter::Options options;
   options.target       = US_DataPub::TargetDb;
   options.dbPassword   = master;
   options.verifyHashes = true;
   options.dryRun       = false;
   options.policy       = US_DataPub::PolicyRename;

   US_DataPubImporter importer;
   ASSERT_TRUE( importer.inspect( bundle, error ) ) << error.toStdString();
   ASSERT_TRUE( importer.runImport( options, error ) )
      << error.toStdString() << "\n" << importer.log().join( "\n" )
                                                      .toStdString();

   // Both models are in the database, and the noise names its own
   US_Model stored;
   EXPECT_EQ( stored.load( true, modelGUID, db ), IUS_DB2::OK );
   EXPECT_EQ( stored.load( true, laterGUID, db ), IUS_DB2::OK );

   US_Noise record;
   ASSERT_EQ( record.load( true, noiseGUID, db ), IUS_DB2::OK );
   EXPECT_EQ( record.modelGUID, modelGUID );
}

/* A model's variance and meniscus are columns of its database row as well
   as attributes of its XML, and the XML the database stores need not have
   them -- the fixture's model does not.  Exported from the fixture database
   and imported into the scratch one, the model row keeps both: the bundle
   carries them in the manifest.  Skips on a database that is not the
   fixture.
*/
TEST_F( DataPubDbImport, AModelRowKeepsItsVarianceAndMeniscus )
{
   QString fixture = qEnvironmentVariable( "US3_TEST_DB_NAME" );

   if ( fixture.isEmpty() )
      GTEST_SKIP() << "no fixture database configured";

   // Export from the fixture database ...
   QStringList target = US_Settings::defaultDB();
   QStringList source = target;
   source.replace( 2, fixture );
   US_Settings::set_defaultDB( source );

   US_DataPubExporter            exporter;
   US_DataPubExporter::Selection selection;
   selection.fromDb     = true;
   selection.dbPassword = master;
   selection.scope      = US_DataPub::ScopeModels;
   selection.runIDs << "MWL_demo_run";

   QString bundle = root + "/fixture_models.tar.gz";
   QString error;
   bool    made   = exporter.exportBundle( selection, bundle, error );

   US_Settings::set_defaultDB( target );

   if ( ! made  &&  error.contains( "MWL_demo_run" ) )
      GTEST_SKIP() << "not the fixture database: " << error.toStdString();

   ASSERT_TRUE( made ) << error.toStdString();

   const QString modelGUID300 = "aaaaaaaa-0000-4000-8000-000000000300";
   US_DataPubEntity entity;
   ASSERT_TRUE( exporter.manifest().entity( US_DataPub::Model, modelGUID300,
                                            entity ) );
   EXPECT_EQ( entity.attrs.value( "variance" ).toDouble(), 0.001 );
   EXPECT_EQ( entity.attrs.value( "meniscus" ).toDouble(), 5.85 );

   // ... and into the scratch one, where an earlier run may have left the
   // model: it has to be written now, not reused
   QStringList query;
   query << "get_modelID" << modelGUID300;
   db->query( query );

   if ( db->lastErrno() == IUS_DB2::OK  &&  db->next() )
   {
      QString oldID = db->value( 0 ).toString();
      query.clear();
      query << "delete_model" << oldID;
      ASSERT_EQ( db->statusQuery( query ), IUS_DB2::OK )
         << db->lastError().toStdString();
   }

   US_DataPubImporter::Options options;
   options.target       = US_DataPub::TargetDb;
   options.dbPassword   = master;
   options.verifyHashes = true;
   options.dryRun       = false;
   options.policy       = US_DataPub::PolicyRename;

   US_DataPubImporter importer;
   ASSERT_TRUE( importer.inspect( bundle, error ) ) << error.toStdString();
   ASSERT_TRUE( importer.runImport( options, error ) )
      << error.toStdString() << "\n" << importer.log().join( "\n" )
                                                      .toStdString();

   bool created = false;

   for ( const US_DataPubImporter::Result& result : importer.results() )
      if ( result.type == US_DataPub::Model  &&  result.guid == modelGUID300 )
         created = ( result.resolution == US_DataPub::ResolvedCreated );

   EXPECT_TRUE( created ) << "the model was not written by this import";

   query.clear();
   query << "get_modelID" << modelGUID300;
   db->query( query );
   ASSERT_EQ( db->lastErrno(), IUS_DB2::OK ) << db->lastError().toStdString();
   ASSERT_TRUE( db->next() );
   QString modelID = db->value( 0 ).toString();

   query.clear();
   query << "get_model_info" << modelID;
   db->query( query );
   ASSERT_EQ( db->lastErrno(), IUS_DB2::OK ) << db->lastError().toStdString();
   ASSERT_TRUE( db->next() );
   EXPECT_EQ( db->value( 3 ).toString().toDouble(), 0.001 );
   EXPECT_EQ( db->value( 4 ).toString().toDouble(), 5.85 );
}

/* Against a real server: a dry run decides what the import then does, and
   a second import of the same bundle finds every record -- which takes the
   investigator's listings the import checks owners against
   (get_project_desc, all_solutionIDs, all_rawDataIDs, all_editedDataIDs,
   get_model_desc, get_noise_desc) in the columns the procedures return.
*/
TEST_F( DataPubDbImport, ADryRunAndASecondImportAgreeWithTheImport )
{
   QString bundle = root + "/twice.tar.gz";
   QString error;

   US_DataPubExporter            exporter;
   US_DataPubExporter::Selection selection;
   selection.fromDb = false;
   selection.scope  = US_DataPub::ScopeNoise;
   selection.runIDs << runID;

   ASSERT_TRUE( exporter.exportBundle( selection, bundle, error ) )
      << error.toStdString();

   US_DataPubImporter::Options options;
   options.target       = US_DataPub::TargetDb;
   options.dbPassword   = master;
   options.verifyHashes = true;
   options.policy       = US_DataPub::PolicyRename;

   US_DataPubImporter dry;
   options.dryRun = true;
   ASSERT_TRUE( dry.inspect( bundle, error ) ) << error.toStdString();
   ASSERT_TRUE( dry.runImport( options, error ) )
      << error.toStdString() << "\n" << dry.log().join( "\n" ).toStdString();

   US_DataPubImporter first;
   options.dryRun = false;
   ASSERT_TRUE( first.inspect( bundle, error ) ) << error.toStdString();
   ASSERT_TRUE( first.runImport( options, error ) )
      << error.toStdString() << "\n" << first.log().join( "\n" ).toStdString();

   QList< US_DataPubImporter::Result > would = dry.results();
   QList< US_DataPubImporter::Result > did   = first.results();
   ASSERT_EQ( would.size(), did.size() );

   for ( int ii = 0; ii < would.size(); ii++ )
      EXPECT_EQ( would[ ii ].resolution, did[ ii ].resolution )
         << US_DataPub::typeText( would[ ii ].type ).toStdString();

   US_DataPubImporter second;
   ASSERT_TRUE( second.inspect( bundle, error ) ) << error.toStdString();
   ASSERT_TRUE( second.runImport( options, error ) )
      << error.toStdString() << "\n" << second.log().join( "\n" ).toStdString();

   QList< US_DataPubImporter::Result > again = second.results();

   for ( int ii = 0; ii < again.size(); ii++ )
   {
      // Hardware the target lacks is stood in for or left out each time
      if ( again[ ii ].type == US_DataPub::Centerpiece  ||
           again[ ii ].type == US_DataPub::RotorCalibration )
         continue;

      EXPECT_EQ( again[ ii ].resolution, US_DataPub::ResolvedReused )
         << US_DataPub::typeText( again[ ii ].type ).toStdString() << " "
         << again[ ii ].name.toStdString();
   }
}

// ------------------------- the rules a real server enforces, on a real server
//
// These write into the scratch import database as well.  The calibration
// test needs an administrator login (US3_TEST_DB_ADMIN_GUID and _PW), the
// test of a second investigator one of that investigator:
//
//   US3_TEST_DB_OTHER_GUID=... US3_TEST_DB_OTHER_PW=... US3_TEST_DB_OTHER_ID=3
//
// The investigator of US3_TEST_DB_PERSON_GUID has to be at user level 2
// (enough to add buffer components, not to add rotor calibrations), and
// the database's user has to be allowed to read and change the channel
// table.  Without these they skip.

class DataPubDbRules : public DataPubDbImport
{
   protected:
      // Log in as another person of the database, importing for invID
      bool loginAs( const QString& guid, const QString& password, int invID )
      {
         if ( guid.isEmpty() )  return false;

         QStringList dbCipher   = US_Crypto::encrypt(
               qEnvironmentVariable( "US3_TEST_DB_PASS" ), master );
         QStringList userCipher = US_Crypto::encrypt( password, master );

         QStringList entry;
         entry << "us3 import database"
               << qEnvironmentVariable( "US3_TEST_DB_USER" )
               << qEnvironmentVariable( "US3_TEST_DB_IMPORT_NAME" )
               << qEnvironmentVariable( "US3_TEST_DB_HOST" )
               << dbCipher.at( 0 ) << dbCipher.at( 1 ) << ""
               << userCipher.at( 0 ) << userCipher.at( 1 ) << guid;

         US_Settings::set_defaultDB( entry );
         US_Settings::set_us_inv_ID( invID );

         return true;
      }

      // The fixture's store exported into a bundle
      QString exportStore( const QString& name )
      {
         QString bundle = root + "/" + name + ".tar.gz";
         QString error;
         QFile::remove( bundle );

         US_DataPubExporter            exporter;
         US_DataPubExporter::Selection selection;
         selection.fromDb = false;
         selection.scope  = US_DataPub::ScopeNoise;
         selection.runIDs << runID;

         EXPECT_TRUE( exporter.exportBundle( selection, bundle, error ) )
            << error.toStdString();

         return bundle;
      }

      US_DataPubImporter::Options options( bool dry = false )
      {
         US_DataPubImporter::Options opts;
         opts.target       = US_DataPub::TargetDb;
         opts.dbPassword   = master;
         opts.verifyHashes = true;
         opts.dryRun       = dry;
         opts.policy       = US_DataPub::PolicyRename;

         return opts;
      }

      const US_DataPubImporter::Result* resultOf(
            const QList< US_DataPubImporter::Result >& results,
            US_DataPub::EntityType type )
      {
         for ( int ii = 0; ii < results.size(); ii++ )
            if ( results[ ii ].type == type )  return &results[ ii ];

         return nullptr;
      }

      // A bundle's manifest changed, and the bundle packed again
      QString recraft( const QString& bundle,
                       std::function< void( US_DataPubManifest&,
                                            const QString& ) > change )
      {
         US_DataPubImporter source;
         QString            error;
         EXPECT_TRUE( source.inspect( bundle, error ) ) << error.toStdString();

         US_DataPubBundle out;
         EXPECT_TRUE( out.createStaging( error ) ) << error.toStdString();

         QString     stage = out.stagingPath();
         QDir        src( source.bundleRoot() );
         QDirIterator it( source.bundleRoot(), QDir::Files,
                          QDirIterator::Subdirectories );

         while ( it.hasNext() )
         {
            QString rel = src.relativeFilePath( it.next() );
            QDir().mkpath( QFileInfo( stage + "/" + rel ).path() );
            QFile::copy( source.bundleRoot() + "/" + rel, stage + "/" + rel );
         }

         US_DataPubManifest mani;
         QString mpath = stage + "/" + US_DataPubBundle::manifestName();
         EXPECT_TRUE( mani.read( mpath, error ) ) << error.toStdString();

         change( mani, stage );

         EXPECT_TRUE( mani.write( mpath, error ) ) << error.toStdString();

         QFile digest( stage + "/" + US_DataPubExporter::manifestDigestName() );
         digest.open( QIODevice::WriteOnly | QIODevice::Truncate );
         digest.write( ( US_DataPubHash::fileHash( mpath ) + "  "
                         + US_DataPubBundle::manifestName() + "\n" ).toUtf8() );
         digest.close();

         QString packed = bundle;
         packed.replace( ".tar.gz", "-crafted.tar.gz" );
         QFile::remove( packed );
         EXPECT_TRUE( out.pack( packed, error ) ) << error.toStdString();

         return packed;
      }

      // The store again under new GUIDs, but for its buffer and analyte
      void rebuildKeepingChemistry()
      {
         QDir( US_Settings::dataDir()   ).removeRecursively();
         QDir( US_Settings::resultDir() ).removeRecursively();
         QDir().mkpath( US_Settings::dataDir()   );
         QDir().mkpath( US_Settings::resultDir() );

         projectGUID  = US_Util::new_guid();
         solutionGUID = US_Util::new_guid();
         expGUID      = US_Util::new_guid();
         rawGUID      = US_Util::new_guid();
         editGUID     = US_Util::new_guid();
         modelGUID    = US_Util::new_guid();
         noiseGUID    = US_Util::new_guid();
         calGUID      = US_Util::new_guid();

         buildStore();
      }

      // The fixture's calibration, for a rotor of the target
      void calibrateTargetRotor( const QString& rotorGUID )
      {
         US_Rotor::RotorCalibration calibration;
         QString path = US_Settings::dataDir() + "/rotors/C0000001.xml";
         ASSERT_TRUE( US_DataPubRecords::readCalibration( path, calibration ) );
         calibration.rotorGUID = rotorGUID;

         // A label of its own, so that no calibration an earlier run of the
         // test left in the target is the same one
         calibration.label = "Demo calibration " + US_Util::new_guid().left( 8 );
         QFile::remove( path );
         ASSERT_TRUE( US_DataPubRecords::writeCalibration( calibration, path ) );
      }

      // The GUID of a rotor the target database has
      QString targetRotorGUID()
      {
         QVector< US_Rotor::Lab > labs;
         QStringList              q( "get_lab_names" );
         db->query( q );
         QStringList ids;

         while ( db->next() )  ids << db->value( 0 ).toString();

         for ( int ii = 0; ii < ids.size(); ii++ )
         {
            QVector< US_Rotor::Rotor > rotors;
            US_Rotor::readRotorsFromDB( rotors, ids[ ii ].toInt(), db );

            if ( ! rotors.isEmpty() )  return rotors[ 0 ].GUID;
         }

         return QString();
      }
};

/* A buffer component the target lacks is added, and the buffer linked to
   the row that was added.  The server's create_buffer_component inserts
   with an ID of its own and answers with LAST_INSERT_ID(), which an
   explicit ID does not set: the connection's previous insert ID came back
   (the project's, written just before) and the buffer was linked to that
   component instead. */
TEST_F( DataPubDbRules, ANewComponentIsTheOneTheBufferNames )
{
   QMap< QString, US_BufferComponent > table;
   US_BufferComponent::getAllFromHD( table );
   ASSERT_TRUE( table.contains( "1" ) ) << "no local component table";

   US_Buffer buffer;
   QString   bpath = US_Settings::dataDir() + "/buffers/B0000001.xml";
   ASSERT_TRUE( buffer.readFromDisk( bpath ) );
   buffer.component     << table.value( "1" );
   buffer.componentIDs  << "1";
   buffer.concentration << 150.0;
   ASSERT_TRUE( buffer.writeToDisk( bpath ) );

   // A component no target has
   QString unique = "Trisx " + US_Util::new_guid().left( 8 );
   QString bundle = recraft( exportStore( "component" ),
         [ & ]( US_DataPubManifest& m, const QString& stage )
   {
      QList< US_DataPubEntity > bufs = m.section( US_DataPub::Buffer );
      ASSERT_EQ( bufs.size(), 1 );
      QString cpath = stage + "/" + bufs[ 0 ].attrs.value( "componentsPayload" );
      QFile   file( cpath );
      ASSERT_TRUE( file.open( QIODevice::ReadOnly ) );
      QString text = QString::fromUtf8( file.readAll() );
      file.close();
      text.replace( "name=\"" + table.value( "1" ).name + "\"",
                    "name=\"" + unique + "\"" );
      ASSERT_TRUE( text.contains( unique ) );
      ASSERT_TRUE( file.open( QIODevice::WriteOnly | QIODevice::Truncate ) );
      file.write( text.toUtf8() );
      file.close();
      bufs[ 0 ].attrs.insert( US_DataPub::digestKeyOf( "componentsPayload" ),
                              US_DataPubHash::fileHash( cpath ) );
      m.setSection( US_DataPub::Buffer, bufs );
   } );

   US_DataPubImporter importer;
   QString            error;
   ASSERT_TRUE( importer.inspect( bundle, error ) ) << error.toStdString();
   ASSERT_TRUE( importer.runImport( options(), error ) )
      << error.toStdString() << "\n" << importer.log().join( "\n" ).toStdString();

   const US_DataPubImporter::Result* made =
         resultOf( importer.results(), US_DataPub::Buffer );
   ASSERT_NE( made, nullptr );

   US_Buffer stored;
   ASSERT_TRUE( stored.readFromDB( db, made->targetID ) );
   ASSERT_EQ  ( stored.component.size(), 1 );
   EXPECT_EQ  ( stored.component[ 0 ].name, unique )
      << "linked to component " << stored.componentIDs.value( 0 ).toStdString();
}

/* add_rotor_calibration takes an administrator, and -1 for "no calibration
   experiment" (0 is an ID it looks for).  An investigator's import leaves
   the calibration out with a warning -- and its dry run says the same; an
   administrator's creates it, with no experiment. */
TEST_F( DataPubDbRules, ACalibrationIsAddedByAnAdministratorOnly )
{
   QString rotor = targetRotorGUID();
   ASSERT_FALSE( rotor.isEmpty() ) << "the target database has no rotor";

   calibrateTargetRotor( rotor );
   QString bundle = exportStore( "calibration" );
   QString error;

   // ---- an investigator ----------------------------------------------------
   {
      US_DataPubImporter dry;
      ASSERT_TRUE( dry.inspect( bundle, error ) ) << error.toStdString();
      ASSERT_TRUE( dry.runImport( options( true ), error ) )
         << error.toStdString();

      US_DataPubImporter importer;
      ASSERT_TRUE( importer.inspect( bundle, error ) ) << error.toStdString();
      ASSERT_TRUE( importer.runImport( options(), error ) )
         << error.toStdString() << "\n"
         << importer.log().join( "\n" ).toStdString();

      for ( const US_DataPubImporter* run : { &dry, &importer } )
      {
         const US_DataPubImporter::Result* cal =
               resultOf( run->results(), US_DataPub::RotorCalibration );
         ASSERT_NE( cal, nullptr );
         EXPECT_EQ( cal->resolution, US_DataPub::ResolvedOmitted );
         EXPECT_TRUE( run->warnings().join( "\n" ).contains( "administrator" ) );
      }

      // Strict about hardware, the import refuses to leave it out
      US_DataPubImporter          strict;
      US_DataPubImporter::Options opts = options();
      opts.strictHardware = true;
      ASSERT_TRUE ( strict.inspect( bundle, error ) ) << error.toStdString();
      EXPECT_FALSE( strict.runImport( opts, error ) );
      EXPECT_TRUE ( error.contains( "administrator" ) ) << error.toStdString();
      EXPECT_EQ   ( strict.outcome(), QString( "Nothing was written" ) );
   }

   // ---- an administrator, for the same investigator -------------------------
   if ( ! loginAs( qEnvironmentVariable( "US3_TEST_DB_ADMIN_GUID" ),
                   qEnvironmentVariable( "US3_TEST_DB_ADMIN_PW" ),
                   qEnvironmentVariable( "US3_TEST_DB_PERSON_ID" ).toInt() ) )
      GTEST_SKIP() << "no administrator login configured";

   rebuildKeepingChemistry();
   calibrateTargetRotor( rotor );
   QString adminBundle = exportStore( "admin" );

   US_DataPubImporter admin;
   ASSERT_TRUE( admin.inspect( adminBundle, error ) ) << error.toStdString();
   ASSERT_TRUE( admin.runImport( options(), error ) )
      << error.toStdString() << "\n" << admin.log().join( "\n" ).toStdString();

   const US_DataPubImporter::Result* cal =
         resultOf( admin.results(), US_DataPub::RotorCalibration );
   ASSERT_NE( cal, nullptr );
   ASSERT_TRUE( cal->resolution == US_DataPub::ResolvedCreated  ||
                cal->resolution == US_DataPub::ResolvedRenamed );

   US_Rotor::RotorCalibration stored;
   ASSERT_EQ( stored.readDB( cal->targetID.toInt(), db ), US_Rotor::ROTOR_OK );
   EXPECT_EQ( stored.GUID, calGUID );
   EXPECT_EQ( stored.calibrationExperimentID, -1 );
}

/* A second investigator imports a bundle whose buffer and analyte the first
   one's import put in the database.  The analyte is not theirs to use (the
   server refuses it in new_solutionAnalyte, half way through the import)
   and the buffer is the first one's private buffer: both are copied under
   GUIDs of their own, and the dry run says what the import does. */
TEST_F( DataPubDbRules, AnotherInvestigatorsChemistryIsCopied )
{
   QString otherGUID = qEnvironmentVariable( "US3_TEST_DB_OTHER_GUID" );
   QString otherPW   = qEnvironmentVariable( "US3_TEST_DB_OTHER_PW" );
   int     otherID   = qEnvironmentVariable( "US3_TEST_DB_OTHER_ID" ).toInt();

   if ( otherGUID.isEmpty()  ||  otherID < 1 )
      GTEST_SKIP() << "no second investigator configured";

   QString error;

   // The first investigator's import
   QString first = exportStore( "first" );
   US_DataPubImporter mine;
   ASSERT_TRUE( mine.inspect( first, error ) ) << error.toStdString();
   ASSERT_TRUE( mine.runImport( options(), error ) )
      << error.toStdString() << "\n" << mine.log().join( "\n" ).toStdString();

   // Another run of the same chemistry, imported by the second investigator
   rebuildKeepingChemistry();
   QString second = exportStore( "second" );
   ASSERT_TRUE( loginAs( otherGUID, otherPW, otherID ) );

   US_DataPubImporter dry;
   ASSERT_TRUE( dry.inspect( second, error ) ) << error.toStdString();
   ASSERT_TRUE( dry.runImport( options( true ), error ) )
      << error.toStdString() << "\n" << dry.log().join( "\n" ).toStdString();

   US_DataPubImporter theirs;
   ASSERT_TRUE( theirs.inspect( second, error ) ) << error.toStdString();
   ASSERT_TRUE( theirs.runImport( options(), error ) )
      << error.toStdString() << "\n" << theirs.log().join( "\n" ).toStdString();

   for ( US_DataPub::EntityType type : { US_DataPub::Buffer,
                                         US_DataPub::Analyte } )
   {
      const US_DataPubImporter::Result* would = resultOf( dry.results(), type );
      const US_DataPubImporter::Result* did   = resultOf( theirs.results(),
                                                          type );
      ASSERT_NE( would, nullptr );
      ASSERT_NE( did,   nullptr );
      EXPECT_EQ( would->resolution, did->resolution );
      // A copy (renamed when an earlier run of the test left one of that
      // name), or that earlier copy -- never the first investigator's record
      EXPECT_TRUE( did->resolution == US_DataPub::ResolvedCreated  ||
                   did->resolution == US_DataPub::ResolvedRenamed  ||
                   did->resolution == US_DataPub::ResolvedReused )
         << US_DataPub::typeText( type ).toStdString();
      EXPECT_NE( did->targetGUID, did->guid )
         << US_DataPub::typeText( type ).toStdString();
   }

   // ... and their solution names the copies
   const US_DataPubImporter::Result* sol =
         resultOf( theirs.results(), US_DataPub::Solution );
   ASSERT_NE( sol, nullptr );

   // Read as the second investigator, whose solution it is
   US_DB2 asThem;
   ASSERT_TRUE( asThem.connect( master, error ) ) << error.toStdString();

   US_Solution solution;
   ASSERT_EQ( solution.readFromDB( sol->targetID.toInt(), &asThem ),
              IUS_DB2::OK );
   EXPECT_EQ( solution.buffer.GUID,
              resultOf( theirs.results(), US_DataPub::Buffer )->targetGUID );
   ASSERT_EQ( solution.analyteInfo.size(), 1 );
   EXPECT_EQ( solution.analyteInfo[ 0 ].analyte.analyteGUID,
              resultOf( theirs.results(), US_DataPub::Analyte )->targetGUID );
}

/* The raw data and solution rows name channel 1, which a fresh schema may
   not have: the dry run finds it, and the import fails before it writes
   anything, instead of on the server's foreign key half way through. */
TEST_F( DataPubDbRules, AMissingChannelFailsBeforeTheImport )
{
   db->rawQuery( "CREATE TEMPORARY TABLE fx_channel_saved"
                 " SELECT * FROM channel WHERE channelID = 1" );
   db->rawQuery( "SET FOREIGN_KEY_CHECKS = 0" );
   db->rawQuery( "DELETE FROM channel WHERE channelID = 1" );
   db->rawQuery( "SET FOREIGN_KEY_CHECKS = 1" );

   int left = db->statusQuery(
         "SELECT IF( COUNT(*) > 0, 0, 301 ) FROM channel WHERE channelID = 1" );

   if ( left != IUS_DB2::NOROWS )
   {
      db->rawQuery( "INSERT IGNORE INTO channel SELECT * FROM fx_channel_saved" );
      GTEST_SKIP() << "the channel table cannot be changed here";
   }

   QString bundle = exportStore( "channel" );
   QString error;
   QString dryError;
   QString realError;
   QString outcome;

   {
      US_DataPubImporter dry;
      EXPECT_TRUE ( dry.inspect( bundle, error ) ) << error.toStdString();
      EXPECT_FALSE( dry.runImport( options( true ), dryError ) );

      US_DataPubImporter importer;
      EXPECT_TRUE ( importer.inspect( bundle, error ) ) << error.toStdString();
      EXPECT_FALSE( importer.runImport( options(), realError ) );
      outcome = importer.outcome();
   }

   db->rawQuery( "INSERT IGNORE INTO channel SELECT * FROM fx_channel_saved" );
   db->rawQuery( "DROP TEMPORARY TABLE fx_channel_saved" );

   EXPECT_TRUE( dryError .contains( "channel 1" ) ) << dryError.toStdString();
   EXPECT_TRUE( realError.contains( "channel 1" ) ) << realError.toStdString();
   EXPECT_EQ  ( outcome, QString( "Nothing was written" ) );
   EXPECT_EQ  ( db->statusQuery( "SELECT IF( COUNT(*) > 0, 0, 301 ) FROM"
                                 " channel WHERE channelID = 1" ),
                int( IUS_DB2::OK ) ) << "channel 1 was not put back";
}

// The store's analyte with another description and sequence
static void rewriteStoreAnalyte( const QString& description,
                                 const QString& sequence )
{
   QString    path = US_Settings::dataDir() + "/analytes/A0000001.xml";
   US_Analyte analyte;
   ASSERT_EQ( analyte.readFromFile( path ), IUS_DB2::OK );
   analyte.description = description;
   analyte.sequence    = sequence;
   QFile::remove( path );
   ASSERT_EQ( analyte.write( false, path ), IUS_DB2::OK );
}

/* Text the target's latin1 columns and procedure parameters cannot hold
   fails the dry run, as it fails the import -- before anything is
   written, naming the record, field and character.  Text latin1 holds
   goes in, and comes back as it was. */
TEST_F( DataPubDbRules, TextTheTargetCannotStoreFailsTheDryRun )
{
   db->rawQuery( "SELECT CHARACTER_SET_NAME FROM information_schema.PARAMETERS"
                 " WHERE SPECIFIC_SCHEMA = DATABASE()"
                 " AND SPECIFIC_NAME = 'new_analyte'"
                 " AND PARAMETER_NAME = 'p_description'" );
   QString charset = db->next() ? db->value( 0 ).toString() : QString();

   if ( charset != "latin1" )
      GTEST_SKIP() << "the target's analyte text is not latin1 ("
                   << charset.toStdString() << ")";

   QString alpha = QString::fromUtf8( "Prot\xC3\xA9ine \xCE\xB1" );
   QString acute = QString::fromUtf8( "Prot\xC3\xA9ine \xC3\xA9 " )
                   + US_Util::new_guid().left( 8 );

   rewriteStoreAnalyte( alpha, QString() );
   QString bundle = exportStore( "alpha" );
   QString error;

   for ( int dry = 1; dry >= 0; dry-- )
   {
      US_DataPubImporter importer;
      ASSERT_TRUE ( importer.inspect( bundle, error ) ) << error.toStdString();
      EXPECT_FALSE( importer.runImport( options( dry == 1 ), error ) )
         << ( dry ? "dry run" : "import" );
      EXPECT_TRUE ( error.contains( QString::fromUtf8( "\xCE\xB1" ) )  &&
                    error.contains( "description" )                    &&
                    error.contains( "latin1" ) ) << error.toStdString();

      if ( dry == 0 )
      {
         EXPECT_EQ( importer.outcome(), QString( "Nothing was written" ) );
      }
   }

   QStringList q;
   q << "get_analyteID" << analyteGUID;
   db->query( q );
   EXPECT_FALSE( db->next() ) << "the analyte was written";

   // ... and text latin1 holds
   rewriteStoreAnalyte( acute, QString() );
   bundle = exportStore( "acute" );

   US_DataPubImporter dry;
   ASSERT_TRUE( dry.inspect( bundle, error ) ) << error.toStdString();
   ASSERT_TRUE( dry.runImport( options( true ), error ) )
      << error.toStdString();

   US_DataPubImporter importer;
   ASSERT_TRUE( importer.inspect( bundle, error ) ) << error.toStdString();
   ASSERT_TRUE( importer.runImport( options(), error ) )
      << error.toStdString() << "\n"
      << importer.log().join( "\n" ).toStdString();

   const US_DataPubImporter::Result* ana = resultOf( importer.results(),
                                                     US_DataPub::Analyte );
   ASSERT_NE( ana, nullptr );
   q.clear();
   q << "get_analyte_info" << ana->targetID;
   db->query( q );
   ASSERT_TRUE( db->next() );
   EXPECT_EQ( db->value( 4 ).toString(), acute );
}

/* A sequence is stored exactly as the bundle holds it -- FASTA header,
   residue numbers, line breaks -- in a database as on disk, and the same
   bundle imported again finds the stored analyte the same one. */
TEST_F( DataPubDbRules, ASequenceIsStoredAsTheBundleHoldsIt )
{
   QString fasta =
      ">sp|P02768|ALBU_HUMAN Serum albumin OS=Homo sapiens\n"
      "1 MKWVTFISLL FLFSSAYSRG VFRRDAHKSE VAHRFKDLGE ENFKALVLIA FAQYLQQCPF\n"
      "61 EDHVKLVNEV TEFAKTCVAD ESAENCDKSL HTLFGDKLCT VATLRETYGE MADCCAKQEP\n";

   rewriteStoreAnalyte( "Fasta analyte " + US_Util::new_guid().left( 8 ),
                        fasta );
   QString bundle = exportStore( "fasta" );
   QString error;

   US_DataPubImporter importer;
   ASSERT_TRUE( importer.inspect( bundle, error ) ) << error.toStdString();
   ASSERT_TRUE( importer.runImport( options(), error ) )
      << error.toStdString() << "\n"
      << importer.log().join( "\n" ).toStdString();

   const US_DataPubImporter::Result* ana = resultOf( importer.results(),
                                                     US_DataPub::Analyte );
   ASSERT_NE( ana, nullptr );
   QStringList q;
   q << "get_analyte_info" << ana->targetID;
   db->query( q );
   ASSERT_TRUE( db->next() );
   EXPECT_EQ( db->value( 2 ).toString().toUtf8(), fasta.toUtf8() );

   // On disk the same
   US_DataPubImporter          disk;
   US_DataPubImporter::Options dopts = options();
   dopts.target    = US_DataPub::TargetDisk;
   dopts.outputDir = root + "/fasta_disk";
   QDir().mkpath( dopts.outputDir );
   ASSERT_TRUE( disk.inspect( bundle, error ) ) << error.toStdString();
   ASSERT_TRUE( disk.runImport( dopts, error ) ) << error.toStdString();

   QStringList files = QDir( dopts.outputDir + "/data/analytes" )
                       .entryList( QStringList( "*.xml" ), QDir::Files );
   ASSERT_EQ( files.size(), 1 );
   US_Analyte onDisk;
   ASSERT_EQ( onDisk.readFromFile( dopts.outputDir + "/data/analytes/"
                                   + files[ 0 ] ), IUS_DB2::OK );
   EXPECT_EQ( onDisk.sequence.toUtf8(), fasta.toUtf8() );

   // ... and the stored analyte is the bundle's to a second import
   US_DataPubImporter again;
   ASSERT_TRUE( again.inspect( bundle, error ) ) << error.toStdString();
   ASSERT_TRUE( again.runImport( options(), error ) )
      << error.toStdString() << "\n"
      << again.log().join( "\n" ).toStdString();
   const US_DataPubImporter::Result* reused = resultOf( again.results(),
                                                        US_DataPub::Analyte );
   ASSERT_NE( reused, nullptr );
   EXPECT_EQ( reused->resolution, US_DataPub::ResolvedReused );
   EXPECT_EQ( reused->targetID,   ana->targetID );
}

// ------------------------------------------- importing into a mock database
//
// These run without a server: the importer is handed an IUS_DB2 that the
// fake in datapub_fake_db.h answers.  They pin down what the import does to
// a database -- one transaction, committed once at the end and rolled back
// on any failure; raw data and edits written through US_ConvertIO; nothing
// written to the local disk store; a second import of the same bundle
// writing nothing at all.

#include "datapub_fake_db.h"

class DataPubMockImport : public DataPubTestEnv
{
   protected:
      QString       bundlePath;
      int           savedInv = 0;
      DataPubFakeDb fake;

      void SetUp() override
      {
         DataPubTestEnv::SetUp();

         savedInv = US_Settings::us_inv_ID();
         US_Settings::set_us_inv_ID( 2 );

         addSpeedSteps();

         bundlePath = root + "/to_mock.tar.gz";

         US_DataPubExporter            exporter;
         US_DataPubExporter::Selection selection;
         selection.fromDb = false;
         selection.scope  = US_DataPub::ScopeNoise;
         selection.runIDs << runID;

         QString error;
         ASSERT_TRUE( exporter.exportBundle( selection, bundlePath, error ) )
            << error.toStdString();

         fake.addHardware( "DEMO-1" );
      }

      void TearDown() override
      {
         US_Settings::set_us_inv_ID( savedInv );
         DataPubTestEnv::TearDown();
      }

      // Two speed steps in the run's experiment XML
      void addSpeedSteps()
      {
         QString path = runDir() + "/" + runID + "." + runType + ".xml";
         QFile   file( path );
         ASSERT_TRUE( file.open( QIODevice::ReadOnly ) );
         QString text = QString::fromUtf8( file.readAll() );
         file.close();

         QString steps =
            "<speedstep rotorspeed=\"20000\" duration_hrs=\"1\" scans=\"10\"/>"
            "<speedstep rotorspeed=\"45000\" duration_hrs=\"5\" scans=\"40\"/>"
            "<runTemp";
         text.replace( "<runTemp", steps );

         ASSERT_TRUE( file.open( QIODevice::WriteOnly | QIODevice::Truncate ) );
         file.write( text.toUtf8() );
         file.close();
      }

      US_DataPubImporter::Options dbOptions()
      {
         US_DataPubImporter::Options options;
         options.target       = US_DataPub::TargetDb;
         options.verifyHashes = true;
         options.policy       = US_DataPub::PolicyReuse;

         return options;
      }

      bool importOnce( US_DataPubImporter& importer, QString& error,
                       const US_DataPubImporter::Options& options )
      {
         importer.setDatabase( fake.db() );

         if ( ! importer.inspect( bundlePath, error ) )  return false;

         return importer.runImport( options, error );
      }

      static int filesUnder( const QString& dir )
      {
         int          knt = 0;
         QDirIterator it( dir, QDir::Files, QDirIterator::Subdirectories );

         while ( it.hasNext() )
         {
            it.next();
            knt++;
         }

         return knt;
      }

      /* Every file under a directory, with its digest and modification
         time.  The times are first set back to a fixed moment, so that a
         file written again -- even with the same content, even within the
         same second -- shows up as changed.
      */
      static QMap< QString, QString > snapshot( const QString& dir )
      {
         static const QDateTime past( QDate( 2001, 2, 3 ), QTime( 4, 5, 6 ),
                                      Qt::UTC );
         QMap< QString, QString > files;
         QDirIterator it( dir, QDir::Files, QDirIterator::Subdirectories );

         while ( it.hasNext() )
         {
            QString path = it.next();
            QFile   file( path );

            if ( file.open( QIODevice::ReadWrite ) )
            {
               file.setFileTime( past, QFileDevice::FileModificationTime );
               file.close();
            }

            files.insert( QDir( dir ).relativeFilePath( path ),
                          state( path ) );
         }

         return files;
      }

      // The digest and modification time of every file, as they are now
      static QMap< QString, QString > current( const QString& dir )
      {
         QMap< QString, QString > files;
         QDirIterator it( dir, QDir::Files, QDirIterator::Subdirectories );

         while ( it.hasNext() )
         {
            QString path = it.next();
            files.insert( QDir( dir ).relativeFilePath( path ), state( path ) );
         }

         return files;
      }

      static QString state( const QString& path )
      {
         return US_DataPubHash::fileHash( path ) + " "
                + QString::number( QFileInfo( path ).lastModified()
                                   .toMSecsSinceEpoch() );
      }

      // The files of the local disk store that an import changed or added
      static QStringList changedFiles( const QMap< QString, QString >& before,
                                       const QMap< QString, QString >& after )
      {
         QStringList changed;
         QList< QString > keys = after.keys();

         for ( int ii = 0; ii < keys.size(); ii++ )
            if ( before.value( keys[ ii ] ) != after.value( keys[ ii ] ) )
               changed << keys[ ii ];

         keys = before.keys();

         for ( int ii = 0; ii < keys.size(); ii++ )
            if ( ! after.contains( keys[ ii ] ) )  changed << keys[ ii ];

         return changed;
      }

      US_DataPub::Resolution resolutionOf(
            const QList< US_DataPubImporter::Result >& results,
            US_DataPub::EntityType type )
      {
         for ( int ii = 0; ii < results.size(); ii++ )
            if ( results[ ii ].type == type )  return results[ ii ].resolution;

         return US_DataPub::ResolvedFailed;
      }
};

TEST_F( DataPubMockImport, TheWholeImportIsOneTransactionCommittedAtTheEnd )
{
   // Every file of the local disk store, by content and time
   QMap< QString, QString > dataBefore   = snapshot( US_Settings::dataDir() );
   QMap< QString, QString > resultBefore = snapshot( US_Settings::resultDir() );

   US_DataPubImporter importer;
   QString            error;

   ASSERT_TRUE( importOnce( importer, error, dbOptions() ) )
      << error.toStdString() << "\n"
      << importer.log().join( "\n" ).toStdString();

   EXPECT_EQ( fake.begins,    1 );
   EXPECT_EQ( fake.commits,   1 );
   EXPECT_EQ( fake.rollbacks, 0 );

   // Nothing was written after the commit
   EXPECT_EQ( fake.commitAt, fake.log.size() );

   // Every record went in: the run, its raw data through US_ConvertIO's
   // writer (which clears the run's rows first and writes the cell rows),
   // its edit, time state, model and noise
   EXPECT_EQ( fake.callsOf( "new_experiment" ).size(), 1 );
   EXPECT_EQ( fake.callsOf( "new_rawData"    ).size(), 1 );
   EXPECT_EQ( fake.callsOf( "delete_rawData" ).size(), 1 );
   EXPECT_EQ( fake.callsOf( "new_cell_experiment"     ).size(), 1 );
   EXPECT_EQ( fake.callsOf( "new_experiment_solution" ).size(), 1 );
   EXPECT_EQ( fake.callsOf( "upload_aucData"  ).size(), 1 );
   EXPECT_EQ( fake.callsOf( "new_editedData"  ).size(), 1 );
   EXPECT_EQ( fake.callsOf( "upload_editData" ).size(), 1 );
   EXPECT_EQ( fake.callsOf( "new_timestate"   ).size(), 1 );
   EXPECT_EQ( fake.callsOf( "upload_timestate" ).size(), 1 );
   EXPECT_EQ( fake.callsOf( "new_model" ).size(), 1 );
   EXPECT_EQ( fake.callsOf( "new_noise" ).size(), 1 );

   // The buffer and the analyte went in through their procedures
   EXPECT_EQ( fake.callsOf( "new_buffer"  ).size(), 1 );
   EXPECT_EQ( fake.callsOf( "new_analyte" ).size(), 1 );

   // An edit is stored as us_edit and us_import_run store one: no comment
   ASSERT_EQ( fake.callsOf( "new_editedData" ).size(), 1 );
   EXPECT_EQ( fake.callsOf( "new_editedData" )[ 0 ].value( 5 ), QString() );

   // The speed profile of the run came along
   EXPECT_EQ( fake.callsOf( "new_speedstep" ).size(), 2 );

   // The experiment and the raw data carry their own GUIDs, and the
   // solution its run
   ASSERT_EQ( fake.callsOf( "new_experiment" ).size(), 1 );
   EXPECT_EQ( fake.callsOf( "new_experiment" )[ 0 ].value( 1 ), expGUID );
   EXPECT_EQ( fake.callsOf( "new_rawData" )[ 0 ].value( 1 ), rawGUID );

   QString expID = QString::number(
                   fake.registry[ "experiment" ].value( expGUID ) );
   ASSERT_EQ( fake.callsOf( "new_solution" ).size(), 1 );
   EXPECT_EQ( fake.callsOf( "new_solution" )[ 0 ].value( 6 ), expID );

   // The local disk store was left alone: no file was added, removed or
   // written again, not even with the same content
   EXPECT_EQ( changedFiles( dataBefore, current( US_Settings::dataDir() ) ),
              QStringList() );
   EXPECT_EQ( changedFiles( resultBefore,
                            current( US_Settings::resultDir() ) ),
              QStringList() );
   EXPECT_FALSE( fake.log.contains( "get_lab_names" )
                 &&  QFile::exists( US_Settings::dataDir()
                                    + "/rotors/labs.xml" ) );

   EXPECT_TRUE( importer.outcome().contains( "written" ) )
      << importer.outcome().toStdString();
}

// A failure part way through undoes everything: nothing is committed
TEST_F( DataPubMockImport, AFailureMidImportRollsBack )
{
   fake.failOn = "upload_aucData";

   US_DataPubImporter importer;
   QString            error;

   EXPECT_FALSE( importOnce( importer, error, dbOptions() ) );

   EXPECT_EQ( fake.begins,    1 );
   EXPECT_EQ( fake.commits,   0 );
   EXPECT_EQ( fake.rollbacks, 1 );
   EXPECT_GT( fake.callsOf( "new_experiment" ).size(), 0 )
      << "the failure came after the first writes";
   EXPECT_TRUE( importer.outcome().contains( "rolled back" ) )
      << importer.outcome().toStdString();
}

TEST_F( DataPubMockImport, AFailedNoiseWriteRollsBackTheWholeRun )
{
   fake.failOn = "new_noise";

   US_DataPubImporter importer;
   QString            error;

   EXPECT_FALSE( importOnce( importer, error, dbOptions() ) );
   EXPECT_EQ   ( fake.commits,   0 );
   EXPECT_EQ   ( fake.rollbacks, 1 );
}

// A lookup that fails is not taken for "not there": that would create a
// duplicate of a record the target has
TEST_F( DataPubMockImport, AFailedLookupFailsTheImport )
{
   fake.failOn = "get_bufferID";

   US_DataPubImporter importer;
   QString            error;

   EXPECT_FALSE( importOnce( importer, error, dbOptions() ) );
   EXPECT_TRUE ( error.contains( "could not be asked" ) ) << error.toStdString();
   EXPECT_EQ   ( fake.callsOf( "new_buffer" ).size(), 0 );
   EXPECT_EQ   ( fake.commits, 0 );
}

// The same bundle imported twice: the second import recognises every
// record, the experiment included, and writes nothing
TEST_F( DataPubMockImport, ASecondImportWritesNothing )
{
   QString error;

   {
      US_DataPubImporter first;
      ASSERT_TRUE( importOnce( first, error, dbOptions() ) )
         << error.toStdString();
   }

   int writes = fake.writes();

   US_DataPubImporter second;
   ASSERT_TRUE( importOnce( second, error, dbOptions() ) )
      << error.toStdString();

   EXPECT_EQ( fake.writes(), writes )
      << fake.log.mid( fake.commitAt ).join( " " ).toStdString();

   QList< US_DataPubImporter::Result > results = second.results();
   QList< US_DataPub::EntityType > types;
   types << US_DataPub::Project << US_DataPub::Buffer << US_DataPub::Analyte
         << US_DataPub::Solution << US_DataPub::Experiment
         << US_DataPub::RawData << US_DataPub::EditedData
         << US_DataPub::TimeState << US_DataPub::Model << US_DataPub::Noise;

   for ( int ii = 0; ii < types.size(); ii++ )
      EXPECT_EQ( resolutionOf( results, types[ ii ] ),
                 US_DataPub::ResolvedReused )
         << US_DataPub::typeText( types[ ii ] ).toStdString();
}

// Hardware the target lacks is stood in for, and said so -- or, when the
// import is strict about it, the import fails and nothing is kept
TEST_F( DataPubMockImport, HardwareSubstitutionIsAWarningOrAFailure )
{
   QString error;

   {
      US_DataPubImporter importer;
      ASSERT_TRUE( importOnce( importer, error, dbOptions() ) )
         << error.toStdString();

      // The calibration's rotor, the centerpiece and the operator are not
      // in the target
      EXPECT_FALSE( importer.warnings().isEmpty() );
      EXPECT_EQ( resolutionOf( importer.results(),
                               US_DataPub::RotorCalibration ),
                 US_DataPub::ResolvedOmitted );

      for ( int ii = 0; ii < importer.results().size(); ii++ )
      {
         US_DataPubImporter::Result result = importer.results()[ ii ];

         if ( result.type != US_DataPub::Centerpiece  &&
              result.type != US_DataPub::RotorCalibration )
            continue;

         // Hardware is matched or stood in for, never reported as created
         EXPECT_NE( result.resolution, US_DataPub::ResolvedCreated )
            << result.name.toStdString();
      }
   }
}

TEST_F( DataPubMockImport, AStrictImportRefusesSubstitutes )
{
   US_DataPubImporter          importer;
   US_DataPubImporter::Options options = dbOptions();
   options.strictHardware = true;
   QString                     error;

   EXPECT_FALSE( importOnce( importer, error, options ) );
   EXPECT_TRUE ( error.contains( "substitutions are not allowed" ) )
      << error.toStdString();
   EXPECT_EQ   ( fake.commits, 0 );
}

// The calibration whose rotor the target has is created under that rotor,
// and the run records the target's rotor and lab, not the source's
TEST_F( DataPubMockImport, AKnownRotorTakesTheCalibration )
{
   US_DataPubImporter probe;
   QString            error;
   ASSERT_TRUE( probe.inspect( bundlePath, error ) );

   QList< US_DataPubEntity > cals =
         probe.manifest().section( US_DataPub::RotorCalibration );
   ASSERT_EQ( cals.size(), 1 );
   fake.registry[ "rotor" ].insert( cals[ 0 ].attrs.value( "rotorGUID" ), 3 );

   // add_rotor_calibration takes an administrator
   fake.admin = true;

   US_DataPubImporter importer;
   ASSERT_TRUE( importOnce( importer, error, dbOptions() ) )
      << error.toStdString();

   // ... and -1 for "no calibration experiment": 0 is an experiment ID it
   // looks for, and refuses
   ASSERT_EQ( fake.callsOf( "add_rotor_calibration" ).size(), 1 );
   EXPECT_EQ( fake.callsOf( "add_rotor_calibration" )[ 0 ].value( 1 ), "3" );
   EXPECT_EQ( fake.callsOf( "add_rotor_calibration" )[ 0 ].value( 7 ), "-1" );

   QStringList exp = fake.callsOf( "new_experiment" ).value( 0 );
   EXPECT_EQ( exp.value( 4 ), "1" );    // the target's lab
   EXPECT_EQ( exp.value( 5 ), "5" );    // its instrument, by serial
   EXPECT_EQ( exp.value( 7 ), "3" );    // its rotor
   EXPECT_EQ( exp.value( 8 ),
              QString::number( fake.registry[ "calibration" ]
                               .value( cals[ 0 ].guid ) ) );
}

// Anyone but an administrator is refused a new calibration by the server:
// it is left out with a warning -- decided in the preflight, so a dry run
// says the same and a strict import fails before anything is written
TEST_F( DataPubMockImport, OnlyAnAdministratorAddsACalibration )
{
   US_DataPubImporter probe;
   QString            error;
   ASSERT_TRUE( probe.inspect( bundlePath, error ) );

   QList< US_DataPubEntity > cals =
         probe.manifest().section( US_DataPub::RotorCalibration );
   ASSERT_EQ( cals.size(), 1 );
   fake.registry[ "rotor" ].insert( cals[ 0 ].attrs.value( "rotorGUID" ), 3 );

   US_DataPubImporter          dry;
   US_DataPubImporter::Options options = dbOptions();
   options.dryRun = true;
   ASSERT_TRUE( importOnce( dry, error, options ) ) << error.toStdString();
   EXPECT_EQ  ( resolutionOf( dry.results(), US_DataPub::RotorCalibration ),
                US_DataPub::ResolvedOmitted );
   EXPECT_TRUE( dry.warnings().join( "\n" ).contains( "administrator" ) )
      << dry.warnings().join( "\n" ).toStdString();

   US_DataPubImporter importer;
   ASSERT_TRUE( importOnce( importer, error, dbOptions() ) )
      << error.toStdString();
   EXPECT_EQ( resolutionOf( importer.results(), US_DataPub::RotorCalibration ),
              US_DataPub::ResolvedOmitted );
   EXPECT_EQ( fake.callsOf( "add_rotor_calibration" ).size(), 0 );
   EXPECT_EQ( fake.commits, 1 );

   DataPubFakeDb strictDb;
   strictDb.addHardware( "DEMO-1" );
   strictDb.registry[ "rotor" ].insert( cals[ 0 ].attrs.value( "rotorGUID" ),
                                         3 );

   US_DataPubImporter strict;
   strict.setDatabase( strictDb.db() );
   options = dbOptions();
   options.strictHardware = true;
   ASSERT_TRUE ( strict.inspect( bundlePath, error ) );
   EXPECT_FALSE( strict.runImport( options, error ) );
   EXPECT_TRUE ( error.contains( "administrator" ) ) << error.toStdString();
   EXPECT_EQ   ( strictDb.begins,   0 );
   EXPECT_EQ   ( strictDb.writes(), 0 );
}

// A chosen project has to be the current investigator's
TEST_F( DataPubMockImport, AProjectOfAnotherInvestigatorIsRefused )
{
   QString theirs = US_Util::new_guid();
   fake.registry[ "project" ].insert( theirs, 77 );
   fake.canned.insert( "get_project_desc", QList< QVariantList >()
                       << ( QVariantList() << 12 << "Mine" ) );

   US_DataPubImporter          importer;
   US_DataPubImporter::Options options = dbOptions();
   options.projectGUID = theirs;
   QString                     error;

   EXPECT_FALSE( importOnce( importer, error, options ) );
   EXPECT_TRUE ( error.contains( "does not belong" ) ) << error.toStdString();
   EXPECT_EQ   ( fake.begins, 0 );
}

// A dry run reads, and neither opens a transaction nor writes
TEST_F( DataPubMockImport, ADryRunWritesNothing )
{
   US_DataPubImporter          importer;
   US_DataPubImporter::Options options = dbOptions();
   options.dryRun = true;
   QString                     error;

   ASSERT_TRUE( importOnce( importer, error, options ) ) << error.toStdString();
   EXPECT_EQ( fake.begins,   0 );
   EXPECT_EQ( fake.writes(), 0 );
}

// A run ID the target already uses for another run: the run is imported
// under a usable new ID, and its raw data and edit are filed under it
TEST_F( DataPubMockImport, ARunOfATakenIdIsRenamedWithItsData )
{
   fake.addExperiment( 50, US_Util::new_guid(), runID );

   US_DataPubImporter importer;
   QString            error;

   ASSERT_TRUE( importOnce( importer, error, dbOptions() ) )
      << error.toStdString();

   QString newRun = runID + "-imported";

   ASSERT_EQ( fake.callsOf( "new_experiment" ).size(), 1 );
   EXPECT_EQ( fake.callsOf( "new_experiment" )[ 0 ].value( 3 ), newRun );

   ASSERT_EQ( fake.callsOf( "new_rawData" ).size(), 1 );
   EXPECT_EQ( fake.callsOf( "new_rawData" )[ 0 ].value( 3 ),
              newRun + rawFile.mid( runID.size() ) );

   ASSERT_EQ( fake.callsOf( "new_editedData" ).size(), 1 );
   EXPECT_EQ( fake.callsOf( "new_editedData" )[ 0 ].value( 3 ), newRun );
   EXPECT_EQ( fake.callsOf( "new_editedData" )[ 0 ].value( 4 ),
              newRun + editFile.mid( runID.size() ) );

   ASSERT_EQ( fake.callsOf( "new_timestate" ).size(), 1 );
   EXPECT_TRUE( fake.callsOf( "new_timestate" )[ 0 ].value( 2 )
                .startsWith( newRun + "." ) );

   // and the run that was there is not the one the import used
   EXPECT_EQ( resolutionOf( importer.results(), US_DataPub::Experiment ),
              US_DataPub::ResolvedRenamed );
}

// The same run under the same GUID is recognised although no procedure
// looks an experiment up by its GUID
TEST_F( DataPubMockImport, TheSameRunIsRecognisedByItsGuid )
{
   fake.addExperiment( 50, expGUID, QString( "renamed_before" ) );

   US_DataPubImporter importer;
   QString            error;

   // Its raw data is not in this target, and the import does not add data
   // to a run it did not create
   EXPECT_FALSE( importOnce( importer, error, dbOptions() ) );
   EXPECT_TRUE ( error.contains( "did not create" ) ) << error.toStdString();
   EXPECT_EQ   ( fake.callsOf( "new_experiment" ).size(), 0 );
   EXPECT_EQ   ( fake.rollbacks, 1 );
}

// ------------------------------- buffer components and the RI profile, in DB

class DataPubMockExtras : public DataPubMockImport
{
   protected:
      US_BufferComponent component;

      // The fixture's buffer gets local component 1, and the bundle is
      // made again
      void exportWithComponent()
      {
         QMap< QString, US_BufferComponent > table;
         US_BufferComponent::getAllFromHD( table );
         ASSERT_TRUE( table.contains( "1" ) ) << "no local component table";
         component = table.value( "1" );

         US_Buffer buffer;
         QString   bpath = US_Settings::dataDir() + "/buffers/B0000001.xml";
         ASSERT_TRUE( buffer.readFromDisk( bpath ) );
         buffer.component     << component;
         buffer.componentIDs  << "1";
         buffer.concentration << 150.0;
         ASSERT_TRUE( buffer.writeToDisk( bpath ) );

         reexport();
      }

      void reexport()
      {
         US_DataPubExporter            exporter;
         US_DataPubExporter::Selection selection;
         selection.fromDb = false;
         selection.scope  = US_DataPub::ScopeNoise;
         selection.runIDs << runID;

         QString error;
         ASSERT_TRUE( exporter.exportBundle( selection, bundlePath, error ) )
            << error.toStdString();
      }

      // The component as the database would list it
      QVariantList componentRow( const US_BufferComponent& comp )
      {
         QStringList dens;
         QStringList visc;

         for ( int ii = 0; ii < 6; ii++ )
         {
            dens << QString::number( comp.dens_coeff[ ii ], 'f', 5 );
            visc << QString::number( comp.visc_coeff[ ii ], 'f', 5 );
         }

         return QVariantList() << comp.unit << comp.name << visc.join( " " )
                               << dens.join( " " ) << comp.range << "0";
      }
};

// A component the database has under another ID is linked by that ID
TEST_F( DataPubMockExtras, AKnownComponentIsLinkedByTheTargetsId )
{
   exportWithComponent();

   fake.canned.insert( "get_buffer_component_desc", QList< QVariantList >()
                       << ( QVariantList() << 42 ) );
   fake.canned.insert( "get_buffer_component_info", QList< QVariantList >()
                       << componentRow( component ) );

   US_DataPubImporter importer;
   QString            error;
   ASSERT_TRUE( importOnce( importer, error, dbOptions() ) )
      << error.toStdString();

   EXPECT_EQ( fake.callsOf( "create_buffer_component" ).size(), 0 );
   ASSERT_EQ( fake.callsOf( "add_buffer_component" ).size(), 1 );
   EXPECT_EQ( fake.callsOf( "add_buffer_component" )[ 0 ].value( 2 ), "42" );
}

// One the database lacks is created, and the buffer linked to the new row.
// The procedure inserts with an ID of its own (the row count plus one) and
// answers with LAST_INSERT_ID(), which that does not set: the connection's
// previous insert ID comes back -- here the project's.  The row is found
// again by what it is, never by that ID.
TEST_F( DataPubMockExtras, AnUnknownComponentIsCreated )
{
   exportWithComponent();

   fake.addComponent( 1, "Sodium chloride", "mM", "", "0 0 0 0 0 0",
                      "0 0 0 0 0 0" );
   fake.addComponent( 2, "Cadmium chloride", "mM", "", "0 0 0 0 0 0",
                      "0 0 0 0 0 0" );

   US_DataPubImporter importer;
   QString            error;
   ASSERT_TRUE( importOnce( importer, error, dbOptions() ) )
      << error.toStdString();

   ASSERT_EQ( fake.callsOf( "create_buffer_component" ).size(), 1 );
   EXPECT_EQ( fake.callsOf( "create_buffer_component" )[ 0 ].value( 2 ),
              component.name );
   ASSERT_EQ( fake.state.components.size(), 3 );
   EXPECT_EQ( fake.state.components.value( 3 ).value( 1 ).toString(),
              component.name );

   // Linked to the new row, 3 -- not to the stale insert ID the procedure
   // answered with, nor to another component
   ASSERT_EQ( fake.callsOf( "add_buffer_component" ).size(), 1 );
   EXPECT_EQ( fake.callsOf( "add_buffer_component" )[ 0 ].value( 2 ), "3" );
   EXPECT_EQ( fake.commits, 1 );
}

// A server whose procedure picks a taken ID fails the creation; the import
// says so before it starts, and is rolled back when it fails
TEST_F( DataPubMockExtras, AComponentIdThatIsTakenIsForeseen )
{
   exportWithComponent();

   // Two rows, IDs 1 and 3: the next is "3" again
   fake.addComponent( 1, "Sodium chloride", "mM", "", "0 0 0 0 0 0",
                      "0 0 0 0 0 0" );
   fake.addComponent( 3, "Cadmium chloride", "mM", "", "0 0 0 0 0 0",
                      "0 0 0 0 0 0" );

   US_DataPubImporter          importer;
   US_DataPubImporter::Options options = dbOptions();
   options.dryRun = true;
   QString                     error;
   ASSERT_TRUE( importOnce( importer, error, options ) )
      << error.toStdString();
   EXPECT_TRUE( importer.warnings().join( "\n" ).contains( "IDs 3" ) )
      << importer.warnings().join( "\n" ).toStdString();

   US_DataPubImporter real;
   EXPECT_FALSE( importOnce( real, error, dbOptions() ) );
   EXPECT_TRUE ( error.contains( "could not be added" ) ) << error.toStdString();
   EXPECT_EQ   ( fake.commits,   0 );
   EXPECT_EQ   ( fake.rollbacks, 1 );
   EXPECT_EQ   ( fake.state.components.size(), 2 );
}

// Adding a component takes user level 2: below it the import is refused
// before anything is written, and the dry run says so too
TEST_F( DataPubMockExtras, AComponentNeedsLevelTwo )
{
   exportWithComponent();
   fake.userLevel = 1;

   for ( int dry = 0; dry < 2; dry++ )
   {
      US_DataPubImporter          importer;
      US_DataPubImporter::Options options = dbOptions();
      options.dryRun = ( dry == 1 );
      QString                     error;

      EXPECT_FALSE( importOnce( importer, error, options ) );
      EXPECT_TRUE ( error.contains( "user level 2" ) ) << error.toStdString();
   }

   EXPECT_EQ( fake.begins,   0 );
   EXPECT_EQ( fake.writes(), 0 );
}

// An RI run's intensity profile goes into the experiment row
TEST_F( DataPubMockExtras, AnRiProfileIsStoredWithTheRun )
{
   QString xml = runDir() + "/" + runID + "." + runType + ".xml";
   QFile   file( xml );
   ASSERT_TRUE( file.open( QIODevice::ReadOnly ) );
   QString text = QString::fromUtf8( file.readAll() );
   file.close();
   text.replace( "<opticalSystem value=\"RA\"", "<opticalSystem value=\"RI\"" );
   ASSERT_TRUE( file.open( QIODevice::WriteOnly | QIODevice::Truncate ) );
   file.write( text.toUtf8() );
   file.close();

   QFile rip( runDir() + "/" + runID + ".RIProfile.xml" );
   ASSERT_TRUE( rip.open( QIODevice::WriteOnly ) );
   rip.write( "<?xml version=\"1.0\"?>\n<!DOCTYPE US_RIProfile>\n"
              "<US_RIProfile version=\"1.0\"><RI value=\"1.5\"/>"
              "<RI value=\"1.25\"/></US_RIProfile>\n" );
   rip.close();

   reexport();

   US_DataPubImporter importer;
   QString            error;
   ASSERT_TRUE( importOnce( importer, error, dbOptions() ) )
      << error.toStdString();

   ASSERT_EQ( fake.callsOf( "new_experiment" ).size(), 1 );
   QString stored = fake.callsOf( "new_experiment" )[ 0 ].value( 11 );
   EXPECT_TRUE( stored.contains( "<RI value=\"1.5\"" ) ) << stored.toStdString();
   EXPECT_TRUE( stored.contains( "<RI value=\"1.25\"" ) ) << stored.toStdString();
}

// A row of the target's component table that does not parse has nothing to
// do with this bundle's buffers: it is skipped, not taken for a failure
TEST_F( DataPubMockExtras, AnUnparseableComponentRowIsSkipped )
{
   exportWithComponent();

   QVariantList bad = componentRow( component );
   bad[ 2 ] = QString( "1.0 2.0 3.0 4.0 5.0" );    // five viscosity terms

   fake.canned.insert( "get_buffer_component_desc", QList< QVariantList >()
                       << ( QVariantList() << 41 ) << ( QVariantList() << 42 ) );
   fake.cannedFor.insert( "get_buffer_component_info:41",
                          QList< QVariantList >() << bad );
   fake.cannedFor.insert( "get_buffer_component_info:42",
                          QList< QVariantList >() << componentRow( component ) );

   US_DataPubImporter importer;
   QString            error;
   ASSERT_TRUE( importOnce( importer, error, dbOptions() ) )
      << error.toStdString();

   EXPECT_EQ( fake.callsOf( "create_buffer_component" ).size(), 0 );
   ASSERT_EQ( fake.callsOf( "add_buffer_component" ).size(), 1 );
   EXPECT_EQ( fake.callsOf( "add_buffer_component" )[ 0 ].value( 2 ), "42" );
   EXPECT_TRUE( importer.log().join( "\n" ).contains( "component 41" ) )
      << importer.log().join( "\n" ).toStdString();
}

// The extinction spectra of a buffer and an analyte go in with them.  The
// library writers store a spectrum only when told it is new, which one read
// from a file is not, so the import writes the profiles itself.
TEST_F( DataPubMockExtras, ExtinctionSpectraGoInWithTheirRecords )
{
   US_Buffer buffer;
   QString   bpath = US_Settings::dataDir() + "/buffers/B0000001.xml";
   ASSERT_TRUE( buffer.readFromDisk( bpath ) );
   buffer.extinction[ 280.0 ] = 0.25;
   buffer.extinction[ 230.0 ] = 1.5;
   ASSERT_TRUE( buffer.writeToDisk( bpath ) );

   US_Analyte analyte;
   QString    apath = US_Settings::dataDir() + "/analytes/A0000001.xml";
   ASSERT_EQ( analyte.readFromFile( apath ), IUS_DB2::OK );
   analyte.extinction[ 280.0 ] = 43824.0;
   ASSERT_EQ( analyte.write( false, apath ), IUS_DB2::OK );

   reexport();

   US_DataPubImporter importer;
   QString            error;
   ASSERT_TRUE( importOnce( importer, error, dbOptions() ) )
      << error.toStdString();

   QString bufID = QString::number( fake.registry[ "buffer"  ].value( bufferGUID ) );
   QString anaID = QString::number( fake.registry[ "analyte" ].value( analyteGUID ) );
   QList< QStringList > profiles = fake.callsOf( "new_eprofile" );

   ASSERT_EQ( profiles.size(), 2 );

   QMap< QString, QStringList > byType;

   for ( int ii = 0; ii < profiles.size(); ii++ )
      byType.insert( profiles[ ii ].value( 2 ), profiles[ ii ] );

   ASSERT_TRUE( byType.contains( "Buffer"  ) );
   ASSERT_TRUE( byType.contains( "Analyte" ) );
   EXPECT_EQ  ( byType[ "Buffer"  ].value( 1 ), bufID );
   EXPECT_EQ  ( byType[ "Analyte" ].value( 1 ), anaID );
   EXPECT_TRUE( byType[ "Buffer"  ].value( 4 ).contains( "230" ) )
      << byType[ "Buffer" ].value( 4 ).toStdString();
   EXPECT_TRUE( byType[ "Analyte" ].value( 4 ).contains( "280" ) )
      << byType[ "Analyte" ].value( 4 ).toStdString();

   // ... inside the one transaction
   EXPECT_EQ( fake.commits, 1 );
   EXPECT_EQ( fake.callsInTransaction( "new_eprofile" ), 2 );
}

// The runs are looked up before the transaction opens: by run ID first,
// and the investigator's whole experiment list only for a run that leaves
// open -- never one query per experiment inside the transaction
TEST_F( DataPubMockImport, TheRunsAreLookedUpBeforeTheTransaction )
{
   // Some runs of the investigator's own, and one of someone else's
   for ( int ii = 0; ii < 5; ii++ )
      fake.addExperiment( 60 + ii, US_Util::new_guid(),
                          QString( "other_run_%1" ).arg( ii ) );
   fake.addExperiment( 70, US_Util::new_guid(), "foreign_run", 9 );

   QStringList lookups;
   lookups << "get_experiment_desc" << "get_experiment_info"
           << "get_experiment_info_by_runID";

   US_DataPubImporter first;
   QString            error;
   ASSERT_TRUE( importOnce( first, error, dbOptions() ) )
      << error.toStdString();

   // Inside the transaction only the last check that the new run's ID is
   // still free, right before its row is written
   for ( int ii = 0; ii < lookups.size(); ii++ )
      EXPECT_EQ( fake.callsInTransaction( lookups[ ii ] ),
                 lookups[ ii ] == "get_experiment_info_by_runID" ? 1 : 0 )
         << lookups[ ii ].toStdString();

   // A new run is not in the list: it was read once, before the writes
   EXPECT_EQ( fake.callsOf( "get_experiment_desc" ).size(), 1 );
   EXPECT_EQ( fake.callsOf( "get_experiment_info" ).size(), 5 );

   // The second time the run ID finds the run: no listing at all
   int start = fake.log.size();

   US_DataPubImporter second;
   ASSERT_TRUE( importOnce( second, error, dbOptions() ) )
      << error.toStdString();

   QStringList after = fake.log.mid( start );
   EXPECT_EQ( after.count( "get_experiment_info_by_runID" ), 1 );
   EXPECT_EQ( after.count( "get_experiment_desc" ), 0 );
   EXPECT_EQ( after.count( "get_experiment_info" ), 0 );

   for ( int ii = 0; ii < lookups.size(); ii++ )
      EXPECT_EQ( fake.callsInTransaction( lookups[ ii ] ), 0 )
         << lookups[ ii ].toStdString();

   EXPECT_EQ( resolutionOf( second.results(), US_DataPub::Experiment ),
              US_DataPub::ResolvedReused );
}

// A run that another investigator holds is not imported again beside it,
// and nothing is attached to their records
TEST_F( DataPubMockImport, ARunOfAnotherInvestigatorIsRefused )
{
   fake.addExperiment( 50, expGUID, runID, 9 );
   fake.addRecord( "rawData", rawGUID, 51, 9 );

   US_DataPubImporter importer;
   QString            error;

   EXPECT_FALSE( importOnce( importer, error, dbOptions() ) );
   EXPECT_TRUE ( error.contains( "another investigator" ) )
      << error.toStdString();
   EXPECT_EQ   ( fake.begins, 0 );
   EXPECT_EQ   ( fake.callsOf( "new_experiment" ).size(), 0 );
   EXPECT_EQ   ( fake.writes(), 0 );
}

// One the import cannot see at all still never gets a second experiment of
// its GUID: the database refuses it and the whole import is rolled back
TEST_F( DataPubMockImport, AnotherInvestigatorsRunIsNeverDuplicated )
{
   fake.addExperiment( 50, expGUID, "their_name", 9 );

   US_DataPubImporter importer;
   QString            error;

   EXPECT_FALSE( importOnce( importer, error, dbOptions() ) );
   EXPECT_EQ   ( fake.rollbacks, 1 );
   EXPECT_EQ   ( fake.state.experiments.size(), 1 );
   EXPECT_EQ   ( fake.state.experiments.value( 50 ).owner, 9 );
   EXPECT_FALSE( fake.registry[ "buffer" ].contains( bufferGUID ) )
      << "the rollback left a buffer behind";
}

// A run the target already has is not added to: not even a solution
TEST_F( DataPubMockImport, ASolutionIsNotAddedToARunTheTargetHad )
{
   fake.addExperiment( 50, expGUID, runID );

   US_DataPubImporter importer;
   QString            error;

   EXPECT_FALSE( importOnce( importer, error, dbOptions() ) );
   EXPECT_TRUE ( error.contains( "does not add a solution" ) )
      << error.toStdString();
   EXPECT_EQ   ( fake.callsOf( "new_solution" ).size(), 0 );
   EXPECT_EQ   ( fake.commits, 0 );

   // ... and a dry run says so too, rather than promise a solution
   US_DataPubImporter          dry;
   US_DataPubImporter::Options options = dbOptions();
   options.dryRun = true;

   EXPECT_FALSE( importOnce( dry, error, options ) );
   EXPECT_TRUE ( error.contains( "does not add a solution" ) )
      << error.toStdString();
}

// A listing that fails is a failed lookup, never an empty one
TEST_F( DataPubMockImport, AFailedHardwareListingFailsTheImport )
{
   QStringList procs;
   procs << "get_rotor_calibration_profiles" << "get_rotor_names"
         << "get_abstractCenterpiece_names" << "get_abstractCenterpiece_info"
         << "get_instrument_names";

   for ( int ii = 0; ii < procs.size(); ii++ )
   {
      DataPubFakeDb broken;
      broken.addHardware( "DEMO-1" );
      broken.failOn = procs[ ii ];

      US_DataPubImporter importer;
      importer.setDatabase( broken.db() );
      QString            error;

      ASSERT_TRUE ( importer.inspect( bundlePath, error ) );
      EXPECT_FALSE( importer.runImport( dbOptions(), error ) )
         << procs[ ii ].toStdString();
      EXPECT_TRUE ( error.contains( "could not be asked" ) )
         << procs[ ii ].toStdString() << ": " << error.toStdString();
      EXPECT_EQ   ( broken.commits, 0 ) << procs[ ii ].toStdString();
   }
}

// A failed import leaves nothing in the database, and what was uploaded in
// a successful one is the bytes of the bundle's files
TEST_F( DataPubMockImport, TheDatabaseHoldsWhatWasCommittedAndNothingElse )
{
   {
      DataPubFakeDb broken;
      broken.addHardware( "DEMO-1" );
      broken.failOn = "new_noise";

      US_DataPubImporter importer;
      importer.setDatabase( broken.db() );
      QString            error;

      ASSERT_TRUE ( importer.inspect( bundlePath, error ) );
      EXPECT_FALSE( importer.runImport( dbOptions(), error ) );
      EXPECT_GT   ( broken.callsOf( "new_buffer" ).size(), 0 );
      EXPECT_TRUE ( broken.registry.value( "buffer" ).isEmpty() );
      EXPECT_TRUE ( broken.registry.value( "rawData" ).isEmpty() );
      EXPECT_TRUE ( broken.state.experiments.isEmpty() );
      EXPECT_TRUE ( broken.state.blobs.isEmpty() );
   }

   US_DataPubImporter importer;
   QString            error;
   ASSERT_TRUE( importOnce( importer, error, dbOptions() ) )
      << error.toStdString();

   QFile raw( runDir() + "/" + rawFile );
   ASSERT_TRUE( raw.open( QIODevice::ReadOnly ) );
   QByteArray bytes = raw.readAll();
   raw.close();

   QString key = "upload_aucData:" + QString::number(
                 fake.registry[ "rawData" ].value( rawGUID ) );
   ASSERT_TRUE( fake.state.blobs.contains( key ) ) << key.toStdString();
   EXPECT_EQ  ( fake.state.blobs.value( key ), bytes );
}

// ------------------------------- whose records an import may use, in DB
//
// The GUID lookups of edits, models and noise (and, for an admin, of
// projects, solutions and raw data) find a record whoever owns it, so the
// import checks against the investigator's own listings before it reuses a
// record or attaches one to it.

#include "us_datapub_bundle.h"

class DataPubMockOwners : public DataPubMockImport
{
   protected:
      typedef std::function< void( US_DataPubManifest&, const QString& ) >
              Change;

      static bool copyTree( const QString& from, const QString& to )
      {
         QDir src( from );
         QDir().mkpath( to );

         QFileInfoList list = src.entryInfoList(
               QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot );

         for ( int ii = 0; ii < list.size(); ii++ )
         {
            QString target = to + "/" + list[ ii ].fileName();

            if ( list[ ii ].isDir() )
            {
               if ( ! copyTree( list[ ii ].absoluteFilePath(), target ) )
                  return false;
            }

            else if ( ! QFile::copy( list[ ii ].absoluteFilePath(), target ) )
               return false;
         }

         return true;
      }

      // The fixture's bundle with its manifest changed, packed again with a
      // matching manifest digest
      void craft( Change change )
      {
         US_DataPubImporter source;
         QString            error;
         ASSERT_TRUE( source.inspect( bundlePath, error ) ) << error.toStdString();

         US_DataPubBundle out;
         ASSERT_TRUE( out.createStaging( error ) ) << error.toStdString();

         QString stage = out.stagingPath();
         ASSERT_TRUE( copyTree( source.bundleRoot(), stage ) );

         US_DataPubManifest mani;
         QString mpath = stage + "/" + US_DataPubBundle::manifestName();
         ASSERT_TRUE( mani.read( mpath, error ) ) << error.toStdString();

         change( mani, stage );

         ASSERT_TRUE( mani.write( mpath, error ) ) << error.toStdString();

         QFile digest( stage + "/" + US_DataPubExporter::manifestDigestName() );
         ASSERT_TRUE( digest.open( QIODevice::WriteOnly | QIODevice::Truncate ) );
         digest.write( ( US_DataPubHash::fileHash( mpath ) + "  "
                         + US_DataPubBundle::manifestName() + "\n" ).toUtf8() );
         digest.close();

         bundlePath = root + "/crafted.tar.gz";
         QFile::remove( bundlePath );
         ASSERT_TRUE( out.pack( bundlePath, error ) ) << error.toStdString();
      }

      // Leave a record type out of a manifest, and every dependency on it
      static void leaveOut( US_DataPubManifest& mani,
                            US_DataPub::EntityType gone )
      {
         mani.setSection( gone, QList< US_DataPubEntity >() );

         QList< US_DataPubEntity > all = mani.all();
         QSet< int >               done;

         for ( int ii = 0; ii < all.size(); ii++ )
         {
            if ( done.contains( int( all[ ii ].type ) ) )  continue;

            done << int( all[ ii ].type );
            QList< US_DataPubEntity > list = mani.section( all[ ii ].type );

            for ( int jj = 0; jj < list.size(); jj++ )
            {
               QList< QString > keys = list[ jj ].depends.keys();

               for ( int kk = 0; kk < keys.size(); kk++ )
                  if ( US_DataPubEntity::dependType( keys[ kk ] ) == gone )
                     list[ jj ].depends.remove( keys[ kk ] );
            }

            mani.setSection( all[ ii ].type, list );
         }
      }

      // The import is refused before anything is written
      void expectRefused( DataPubFakeDb& db, const QString& what )
      {
         US_DataPubImporter importer;
         importer.setDatabase( db.db() );
         QString            error;

         ASSERT_TRUE ( importer.inspect( bundlePath, error ) )
            << error.toStdString();
         EXPECT_FALSE( importer.runImport( dbOptions(), error ) );
         EXPECT_TRUE ( error.contains( "another investigator" ) )
            << error.toStdString();
         EXPECT_TRUE ( error.contains( what ) ) << error.toStdString();
         EXPECT_EQ   ( db.begins,   0 );
         EXPECT_EQ   ( db.writes(), 0 );
      }

      // A fresh database with the fixture's hardware, logged in as an admin
      // or not, holding one record of investigator 9
      void expectRefusedFor( const QString& family, const QString& guid,
                             bool admin )
      {
         DataPubFakeDb db;
         db.addHardware( "DEMO-1" );
         db.admin = admin;
         db.addRecord( family, guid, 300, 9 );

         expectRefused( db, guid );
      }
};

// A record of the bundle that the target holds for someone else is not
// reused -- and nothing of the bundle is hung from it.  The lookups of
// edits, models and noise find anyone's record; a project's lookup refuses
// someone else's for a user and finds it for an admin.
TEST_F( DataPubMockOwners, NoRecordOfAnotherInvestigatorIsReused )
{
   for ( int admin = 0; admin < 2; admin++ )
   {
      expectRefusedFor( "model",   modelGUID,   admin );
      expectRefusedFor( "edit",    editGUID,    admin );
      expectRefusedFor( "noise",   noiseGUID,   admin );
      expectRefusedFor( "project", projectGUID, admin );
   }
}

// A noise record whose model the bundle does not carry is attached only to
// the investigator's own model of that GUID
TEST_F( DataPubMockOwners, ANoiseIsNotAttachedToAnotherInvestigatorsModel )
{
   craft( []( US_DataPubManifest& m, const QString& )
          { leaveOut( m, US_DataPub::Model ); } );

   expectRefusedFor( "model", modelGUID, false );
}

TEST_F( DataPubMockOwners, ANoiseGoesOnTheInvestigatorsOwnModel )
{
   craft( []( US_DataPubManifest& m, const QString& )
          { leaveOut( m, US_DataPub::Model ); } );

   // The investigator's own model, fitted to an edit of theirs
   QString theirEdit = US_Util::new_guid();
   fake.addRecord( "edit",  theirEdit, 250 );
   fake.addRecord( "model", modelGUID, 300, 2, "their model" );
   fake.state.modelEdits.insert( modelGUID, theirEdit );

   US_DataPubImporter importer;
   QString            error;

   ASSERT_TRUE( importOnce( importer, error, dbOptions() ) )
      << error.toStdString();

   // Written with the model's ID and the edit it was fitted to, without the
   // lookups US_Noise::write() would make inside the transaction
   ASSERT_EQ( fake.callsOf( "new_noise" ).size(), 1 );
   EXPECT_EQ( fake.callsOf( "new_noise" )[ 0 ].value( 1 ), noiseGUID );
   EXPECT_EQ( fake.callsOf( "new_noise" )[ 0 ].value( 2 ), "250" );
   EXPECT_EQ( fake.callsOf( "new_noise" )[ 0 ].value( 3 ), "300" );
   EXPECT_EQ( fake.callsInTransaction( "get_model_desc" ), 0 );
   EXPECT_EQ( fake.callsInTransaction( "get_modelID" ), 0 );
   EXPECT_EQ( fake.commits, 1 );
}

// A model whose edit the bundle does not carry is attached only to the
// investigator's own edit
TEST_F( DataPubMockOwners, AModelIsNotAttachedToAnotherInvestigatorsEdit )
{
   craft( []( US_DataPubManifest& m, const QString& )
          { leaveOut( m, US_DataPub::EditedData ); } );

   expectRefusedFor( "edit", editGUID, false );
}

// A triple's solution that the bundle does not carry has to be the
// investigator's: someone else's is refused by the lookup (for a user) or
// by the listing (for an admin)
TEST_F( DataPubMockOwners, ATripleIsNotGivenAnotherInvestigatorsSolution )
{
   craft( []( US_DataPubManifest& m, const QString& )
          { leaveOut( m, US_DataPub::Solution ); } );

   expectRefusedFor( "solution", solutionGUID, false );
   expectRefusedFor( "solution", solutionGUID, true  );
}

// An admin importing for an investigator gets the same answer about
// someone else's run as the investigator would
TEST_F( DataPubMockOwners, AnAdminIsRefusedAnotherInvestigatorsRunToo )
{
   DataPubFakeDb db;
   db.addHardware( "DEMO-1" );
   db.admin = true;
   db.addExperiment( 50, expGUID, runID, 9 );
   db.addRecord( "rawData", rawGUID, 51, 9 );

   expectRefused( db, runID );
}

// ----------------------------- what runs inside the transaction, in DB

class DataPubMockTxn : public DataPubMockExtras
{
};

// Every lookup that can be made before the writes is made before them: the
// transaction runs the writes, the library writers' own queries, and the
// last check of a new run's ID -- no listing, hardware read or GUID lookup
// the preflight could have made
TEST_F( DataPubMockTxn, TheTransactionRunsNoLookupThatCouldComeFirst )
{
   exportWithComponent();

   // A name conflict, so the fingerprint of the target's record is read
   QString other = US_Util::new_guid();
   fake.addRecord( "analyte", other, 40, 2, "Demo analyte" );

   US_DataPubImporter importer;
   QString            error;
   ASSERT_TRUE( importOnce( importer, error, dbOptions() ) )
      << error.toStdString();

   QStringList early;
   early << "get_lab_names" << "get_lab_info" << "get_instrument_names"
         << "get_instrument_info_new" << "get_rotor_names" << "get_rotor_info"
         << "get_rotor_calibration_profiles" << "get_rotor_calibration_info"
         << "get_abstractCenterpiece_names" << "get_abstractCenterpiece_info"
         << "get_project_desc" << "get_buffer_desc" << "get_analyte_desc"
         << "all_solutionIDs" << "all_rawDataIDs" << "all_editedDataIDs"
         << "get_model_desc" << "get_noise_desc" << "get_experiment_desc"
         << "get_experiment_info" << "get_rotorID_from_GUID"
         << "get_personID_from_GUID" << "get_projectID_from_GUID"
         << "get_bufferID" << "get_analyteID" << "get_analyte_info"
         << "get_modelID" << "get_noiseID" << "get_editID";

   QStringList inside = fake.transactionLog();

   for ( int ii = 0; ii < early.size(); ii++ )
      EXPECT_EQ( inside.count( early[ ii ] ), 0 )
         << early[ ii ].toStdString() << " in: "
         << inside.join( " " ).toStdString();

   // The component table is read in the transaction only around a
   // component being added (US_BufferComponent::saveToDB()), to find the
   // new row
   int added = inside.indexOf( "create_buffer_component" );
   ASSERT_GE( added, 1 );
   EXPECT_EQ( inside.count( "get_buffer_component_desc" ), 2 );
   EXPECT_EQ( inside.indexOf( "get_buffer_component_desc" ), added - 1 );
   EXPECT_EQ( inside.lastIndexOf( "get_buffer_component_desc" ), added + 1 );

   // ... and the analyte was renamed, after comparing it with the other one
   EXPECT_EQ( fake.callsOf( "new_analyte" ).value( 0 ).value( 5 ),
              QString( "Demo analyte (imported)" ) );
   EXPECT_EQ( fake.writesBeforeBegin(), 0 );
}

// The run ID is checked once more right before the run is written, inside
// the transaction: a run of that ID that appeared since the preflight
// fails the import instead of being duplicated
TEST_F( DataPubMockTxn, ARunIdTakenDuringTheImportFailsIt )
{
   fake.onBegin = [ this ]()
   {
      fake.addExperiment( 50, US_Util::new_guid(), runID );
   };

   US_DataPubImporter importer;
   QString            error;

   EXPECT_FALSE( importOnce( importer, error, dbOptions() ) );
   EXPECT_TRUE ( error.contains( "was taken" ) ) << error.toStdString();
   EXPECT_EQ   ( fake.callsOf( "new_experiment" ).size(), 0 );
   EXPECT_EQ   ( fake.rollbacks, 1 );
   EXPECT_EQ   ( fake.commits,   0 );
}

// (The two implicit-commit cases exercise the fake's implicit-commit path
// and pin the importer's report down; that report was right before.)
// A procedure that commits by itself ends the transaction: the import can
// no longer undo what was written before it, and says so instead of
// reporting a clean rollback
TEST_F( DataPubMockTxn, AnImplicitCommitIsNotReportedAsARollback )
{
   fake.implicitCommitOn = "new_buffer";
   fake.failOn           = "new_noise";

   US_DataPubImporter importer;
   QString            error;

   EXPECT_FALSE( importOnce( importer, error, dbOptions() ) );
   EXPECT_EQ   ( fake.commits, 0 );
   EXPECT_TRUE ( importer.outcome().contains( "may have been kept" ) )
      << importer.outcome().toStdString();
   EXPECT_FALSE( importer.outcome().contains( "Nothing was imported" ) )
      << importer.outcome().toStdString();

   // What the procedure committed is there
   EXPECT_TRUE ( fake.registry.value( "buffer" ).contains( bufferGUID ) );
}

// ... and an import that otherwise succeeds fails at the commit
TEST_F( DataPubMockTxn, AnImplicitCommitFailsTheCommit )
{
   fake.implicitCommitOn = "new_analyte";

   US_DataPubImporter importer;
   QString            error;

   EXPECT_FALSE( importOnce( importer, error, dbOptions() ) );
   EXPECT_TRUE ( error.contains( "commit" ) ) << error.toStdString();
   EXPECT_TRUE ( importer.outcome().contains( "may have been kept" ) )
      << importer.outcome().toStdString();
}

// A manually entered buffer is stored without its "  [M]" marker, so it is
// compared and renamed under the name it will have
TEST_F( DataPubMockTxn, AManualBufferIsRenamedUnderItsStoredName )
{
   US_Buffer buffer;
   QString   bpath = US_Settings::dataDir() + "/buffers/B0000001.xml";
   ASSERT_TRUE( buffer.readFromDisk( bpath ) );
   buffer.description = "Demo buffer  [M]";
   buffer.manual      = true;
   ASSERT_TRUE( buffer.writeToDisk( bpath ) );
   reexport();

   // The target has another buffer of that stored name
   fake.addRecord( "buffer", US_Util::new_guid(), 40, 2, "Demo buffer" );

   US_DataPubImporter importer;
   QString            error;
   ASSERT_TRUE( importOnce( importer, error, dbOptions() ) )
      << error.toStdString();

   ASSERT_EQ( fake.callsOf( "new_buffer" ).size(), 1 );
   EXPECT_EQ( fake.callsOf( "new_buffer" )[ 0 ].value( 2 ),
              QString( "Demo buffer (imported)" ) );
   EXPECT_EQ( fake.callsOf( "new_buffer" )[ 0 ].value( 7 ), QString( "1" ) );
}

// ------------------------------------------------- a dry run into a DB

// A dry run decides what the import would: it leaves out a calibration
// whose rotor the target lacks, as the import does, and resolves the run's
// hardware with the same substitutions and warnings
TEST_F( DataPubMockTxn, ADryRunMakesTheImportsDecisions )
{
   US_DataPubImporter          dry;
   US_DataPubImporter::Options options = dbOptions();
   options.dryRun = true;
   QString                     error;

   ASSERT_TRUE( importOnce( dry, error, options ) ) << error.toStdString();
   EXPECT_EQ  ( fake.begins,   0 );
   EXPECT_EQ  ( fake.writes(), 0 );

   US_DataPubImporter real;
   ASSERT_TRUE( importOnce( real, error, dbOptions() ) ) << error.toStdString();

   // The same resolution for every record, and the same warnings
   QList< US_DataPubImporter::Result > would = dry.results();
   QList< US_DataPubImporter::Result > did   = real.results();
   ASSERT_EQ( would.size(), did.size() );

   for ( int ii = 0; ii < would.size(); ii++ )
   {
      EXPECT_EQ( would[ ii ].type, did[ ii ].type );
      EXPECT_EQ( would[ ii ].resolution, did[ ii ].resolution )
         << US_DataPub::typeText( would[ ii ].type ).toStdString();
   }

   EXPECT_EQ( resolutionOf( would, US_DataPub::RotorCalibration ),
              US_DataPub::ResolvedOmitted );
   EXPECT_EQ( dry.warnings().size(), real.warnings().size() )
      << dry.warnings().join( "\n" ).toStdString() << "\n--\n"
      << real.warnings().join( "\n" ).toStdString();
}

// ... and a run's triple whose solution is in neither the bundle nor the
// target fails the dry run, not only the import
TEST_F( DataPubMockTxn, ADryRunChecksTheStagedRun )
{
   QString xml = runDir() + "/" + runID + "." + runType + ".xml";
   QFile   file( xml );
   ASSERT_TRUE( file.open( QIODevice::ReadOnly ) );
   QString text = QString::fromUtf8( file.readAll() );
   file.close();

   // The experiment XML describes a triple the bundle has no raw data for
   ASSERT_TRUE( text.contains( rawGUID ) );
   text.replace( rawGUID, US_Util::new_guid() );
   ASSERT_TRUE( file.open( QIODevice::WriteOnly | QIODevice::Truncate ) );
   file.write( text.toUtf8() );
   file.close();
   reexport();

   US_DataPubImporter          importer;
   US_DataPubImporter::Options options = dbOptions();
   options.dryRun = true;
   QString                     error;

   EXPECT_FALSE( importOnce( importer, error, options ) );
   EXPECT_TRUE ( error.contains( "is not described" )  ||
                 error.contains( "could not be read" ) ) << error.toStdString();
   EXPECT_EQ   ( fake.writes(), 0 );
}
// An analyte's profile holds the extinction values its bundle carries, and
// nothing the reader computes: US_Analyte's reader adds the 280 nm value of
// a protein's sequence, which must not become a stored profile point.  The
// payloads are crafted so that they hold exactly what the test says, in
// this bundle format (a spectra payload, absent without spectra) and in the
// older one (the values in the analyte XML).
class DataPubMockSpectra : public DataPubMockOwners
{
   protected:
      // The analyte with a protein sequence put in and its extinction
      // values replaced
      void craftAnalyte( const QMap< double, double >& values,
                         bool spectraPayload )
      {
         QString extinction;

         if ( ! spectraPayload )
            for ( auto it = values.constBegin(); it != values.constEnd(); ++it )
               extinction += QString( "<extinction frequency=\"%1\""
                                      " value=\"%2\"/>" )
                             .arg( it.key() ).arg( it.value() );

         craft( [ & ]( US_DataPubManifest& m, const QString& stage )
         {
            QList< US_DataPubEntity > list = m.section( US_DataPub::Analyte );
            ASSERT_EQ( list.size(), 1 );

            QString path = stage + "/" + list[ 0 ].payload;
            QFile   file( path );
            ASSERT_TRUE( file.open( QIODevice::ReadOnly ) );
            QString text = QString::fromUtf8( file.readAll() );
            file.close();

            text.remove( QRegularExpression( "<extinction [^>]*/>" ) );
            text.replace( QRegularExpression( "<sequence>\\s*</sequence>" ),
                          "<sequence>\nMKWVTFISLLLLFSSAYSRGVFRRDTHKSEIAHRFKD\n"
                          "</sequence>" + extinction );
            ASSERT_TRUE( text.contains( "MKWV" ) ) << text.toStdString();

            ASSERT_TRUE( file.open( QIODevice::WriteOnly | QIODevice::Truncate ) );
            file.write( text.toUtf8() );
            file.close();

            list[ 0 ].sha256 = US_DataPubHash::fileHash( path );
            list[ 0 ].attrs.remove( "spectraPayload" );
            list[ 0 ].attrs.remove( US_DataPub::digestKeyOf( "spectraPayload" ) );

            if ( spectraPayload  &&  ! values.isEmpty() )
            {
               QString rel = US_DataPub::payloadDir( US_DataPub::Analyte ) + "/"
                             + list[ 0 ].guid + ".crafted.spectra.xml";
               US_DataPubRecords::Spectra spectra;
               spectra.insert( "extinction", values );
               ASSERT_TRUE( US_DataPubRecords::writeSpectra(
                               spectra, stage + "/" + rel ) );
               list[ 0 ].attrs.insert( "spectraPayload", rel );
               list[ 0 ].attrs.insert(
                     US_DataPub::digestKeyOf( "spectraPayload" ),
                     US_DataPubHash::fileHash( stage + "/" + rel ) );
            }

            m.setSection( US_DataPub::Analyte, list );
         } );
      }

      QStringList analyteProfiles()
      {
         QList< QStringList > profiles = fake.callsOf( "new_eprofile" );
         QStringList          stored;

         for ( int ii = 0; ii < profiles.size(); ii++ )
            if ( profiles[ ii ].value( 2 ) == "Analyte" )
               stored << profiles[ ii ].value( 4 );

         return stored;
      }

      void expectNoProfile( bool spectraPayload )
      {
         craftAnalyte( QMap< double, double >(), spectraPayload );

         US_DataPubImporter importer;
         QString            error;
         ASSERT_TRUE( importOnce( importer, error, dbOptions() ) )
            << error.toStdString();

         EXPECT_EQ( analyteProfiles(), QStringList() );
         EXPECT_EQ( fake.callsOf( "new_analyte" ).value( 0 ).value( 6 ),
                    QString() );
      }

      void expectOnlyItsPoint( bool spectraPayload )
      {
         QMap< double, double > values;
         values.insert( 230.0, 150000.0 );
         craftAnalyte( values, spectraPayload );

         US_DataPubImporter importer;
         QString            error;
         ASSERT_TRUE( importOnce( importer, error, dbOptions() ) )
            << error.toStdString();

         QStringList stored = analyteProfiles();
         ASSERT_EQ   ( stored.size(), 1 );
         EXPECT_TRUE ( stored[ 0 ].contains( "230" ) ) << stored[ 0 ].toStdString();
         EXPECT_FALSE( stored[ 0 ].contains( "280" ) ) << stored[ 0 ].toStdString();
         EXPECT_TRUE ( fake.callsOf( "new_analyte" ).value( 0 ).value( 6 )
                       .startsWith( "230/" ) )
            << fake.callsOf( "new_analyte" ).value( 0 ).value( 6 ).toStdString();
      }
};

TEST_F( DataPubMockSpectra, AProteinWithoutASpectrumGetsNoProfile )
{
   expectNoProfile( true );
}

TEST_F( DataPubMockSpectra, AProteinWithoutASpectrumGetsNoProfileFromAnOldBundle )
{
   expectNoProfile( false );
}

TEST_F( DataPubMockSpectra, AMeasuredSpectrumGainsNoComputedPoint )
{
   expectOnlyItsPoint( true );
}

TEST_F( DataPubMockSpectra, AMeasuredSpectrumGainsNoComputedPointFromAnOldBundle )
{
   expectOnlyItsPoint( false );
}

// ------------------------------------------- spectra, export to import

// The stored profile of a component type, read back from new_eprofile
static QMap< double, double > stored_profile( const DataPubFakeDb& fake,
                                              const QString& compType )
{
   QList< QStringList >   profiles = fake.callsOf( "new_eprofile" );
   QMap< double, double > values;

   for ( int ii = 0; ii < profiles.size(); ii++ )
   {
      if ( profiles[ ii ].value( 2 ) != compType )  continue;

      QXmlStreamReader xml( profiles[ ii ].value( 4 ) );

      while ( ! xml.atEnd() )
      {
         xml.readNext();

         if ( ! xml.isStartElement()  ||
              xml.name().toString() != "extinction" )  continue;

         QXmlStreamAttributes a = xml.attributes();
         values.insert( a.value( "wavelength" ).toString().toDouble(),
                        a.value( "value"      ).toString().toDouble() );
      }
   }

   return values;
}

// The spectra of a buffer and an analyte, and a buffer's component
// concentrations, go from the source's files through a bundle into the
// database at full precision: the library's own files round them
class DataPubMockRoundTrip : public DataPubMockExtras
{
   protected:
      QMap< double, double > storedProfile( const QString& compType )
      {
         return stored_profile( fake, compType );
      }
};

TEST_F( DataPubMockRoundTrip, SpectraAndConcentrationsKeepTheirDigits )
{
   // Two buffer points 0.01 nm apart, and values with every digit in use
   QMap< double, double > bufSpectrum;
   bufSpectrum.insert( 230.01, 1.2345678901234567 );
   bufSpectrum.insert( 230.02, 0.000123456789012345 );

   QMap< double, double > anaSpectrum;
   anaSpectrum.insert( 278.123456789, 43824.123456789 );
   anaSpectrum.insert( 280.0,         45678.987654321 );

   exportWithComponent();

   US_Buffer buffer;
   QString   bpath = US_Settings::dataDir() + "/buffers/B0000001.xml";
   ASSERT_TRUE( buffer.readFromDisk( bpath ) );
   buffer.extinction    = bufSpectrum;
   buffer.concentration = QList< double >() << 0.000005;     // 5 uM in mM
   ASSERT_TRUE( buffer.writeToDisk( bpath ) );

   US_Analyte analyte;
   QString    apath = US_Settings::dataDir() + "/analytes/A0000001.xml";
   ASSERT_EQ( analyte.readFromFile( apath ), IUS_DB2::OK );
   analyte.extinction = anaSpectrum;
   ASSERT_EQ( analyte.write( false, apath ), IUS_DB2::OK );

   reexport();

   US_DataPubImporter importer;
   QString            error;
   ASSERT_TRUE( importOnce( importer, error, dbOptions() ) )
      << error.toStdString();

   QMap< double, double > storedBuf = storedProfile( "Buffer" );
   QMap< double, double > storedAna = storedProfile( "Analyte" );

   ASSERT_EQ( storedBuf.size(), 2 ) << "two points 0.01 nm apart stay two";
   EXPECT_EQ( storedBuf, bufSpectrum );
   EXPECT_EQ( storedAna, anaSpectrum );

   ASSERT_EQ( fake.callsOf( "add_buffer_component" ).size(), 1 );
   EXPECT_EQ( fake.callsOf( "add_buffer_component" )[ 0 ].value( 3 )
              .toDouble(), 0.000005 )
      << fake.callsOf( "add_buffer_component" )[ 0 ].value( 3 ).toStdString();
}

// A solution's own profile goes in with it, as US_Solution::saveToDB()
// stores one: component type "Sample"
TEST_F( DataPubMockSpectra, ASolutionsOwnProfileGoesInWithIt )
{
   QString error;

   // The disk store keeps no solution profile, so it is given to the bundle
   // the way a database export gives it: as the solution's spectra payload
   craft( [ & ]( US_DataPubManifest& m, const QString& stage )
   {
      QList< US_DataPubEntity > list = m.section( US_DataPub::Solution );
      ASSERT_FALSE( list.isEmpty() );

      QString rel = US_DataPub::payloadDir( US_DataPub::Solution ) + "/"
                    + list[ 0 ].guid + ".spectra.xml";
      US_DataPubRecords::Spectra spectra;
      spectra[ "extinction" ].insert( 260.5, 0.123456789012345678 );
      spectra[ "extinction" ].insert( 280.25, 9.87654321098765 );
      ASSERT_TRUE( US_DataPubRecords::writeSpectra( spectra,
                                                    stage + "/" + rel ) );
      list[ 0 ].attrs.insert( "spectraPayload", rel );
      list[ 0 ].attrs.insert( US_DataPub::digestKeyOf( "spectraPayload" ),
                              US_DataPubHash::fileHash( stage + "/" + rel ) );
      m.setSection( US_DataPub::Solution, list );
   } );

   US_DataPubImporter importer;
   ASSERT_TRUE( importOnce( importer, error, dbOptions() ) )
      << error.toStdString();

   QMap< double, double > want;
   want.insert( 260.5, 0.123456789012345678 );
   want.insert( 280.25, 9.87654321098765 );

   QList< QStringList > profiles = fake.callsOf( "new_eprofile" );
   QString              solID    = QString::number(
                                   fake.registry[ "solution" ].value( solutionGUID ) );
   bool                 found    = false;

   for ( int ii = 0; ii < profiles.size(); ii++ )
      if ( profiles[ ii ].value( 2 ) == "Sample" )
      {
         found = true;
         EXPECT_EQ( profiles[ ii ].value( 1 ), solID );
      }

   EXPECT_TRUE( found );
   EXPECT_EQ  ( stored_profile( fake, "Sample" ), want );
   EXPECT_EQ  ( fake.callsInTransaction( "new_eprofile" ),
                fake.callsOf( "new_eprofile" ).size() );
}

// End to end: a protein the source holds without a spectrum gets no
// profile in the target: the export carries only the spectra the analyte
// file holds, not the 280 nm value US_Analyte's disk reader computes from
// the sequence; the import side is covered by DataPubMockSpectra above.
TEST_F( DataPubMockRoundTrip, AProteinWithoutASpectrumStaysWithout )
{
   US_DataPubImporter importer;
   QString            error;
   ASSERT_TRUE( importOnce( importer, error, dbOptions() ) )
      << error.toStdString();

   EXPECT_TRUE( storedProfile( "Analyte" ).isEmpty() );
}

// A target record that was read but cannot be written out to be compared
// fails the import before anything is written; it is not taken for a
// record that differs (and the bundle's renamed past it)
TEST_F( DataPubMockTxn, ATargetRecordThatCannotBeComparedFailsTheImport )
{
   QString other = US_Util::new_guid();
   fake.addRecord( "buffer", other, 40, 2, "Demo buffer" );
   fake.cannedFor.insert( "get_buffer_info:40", QList< QVariantList >()
      << ( QVariantList() << other << "Demo buffer" << "0.0" << "7.0"
                          << "1.002" << "1.0" << "0" << 2 ) );

   US_DataPubImporter importer;
   importer.setDatabase( fake.db() );
   QString            error;
   ASSERT_TRUE( importer.inspect( bundlePath, error ) ) << error.toStdString();

   // Where the target's buffer would be written out, a directory
   QString where = QFileInfo( importer.bundleRoot() ).path() + "/work/target/"
                   + US_DataPub::typeKey( US_DataPub::Buffer ) + ".xml";
   ASSERT_TRUE( QDir().mkpath( where ) );

   EXPECT_FALSE( importer.runImport( dbOptions(), error ) );
   EXPECT_TRUE ( error.contains( "written out to be compared" ) )
      << error.toStdString();
   EXPECT_EQ   ( fake.writes(), 0 );
}

// ------------------------- what the server would refuse, refused up front
//
// The fake refuses what the real procedures refuse (datapub_fake_db.h), so
// an import that would fail half way on a server fails half way here too.
// These pin down that the import -- and its dry run -- find it before the
// transaction opens, or make the record so the server takes it.

class DataPubMockRules : public DataPubMockSpectra
{
   protected:
      // The import is refused before anything is written, a dry run too
      void expectRefusedEarly( DataPubFakeDb& db, const QString& what )
      {
         for ( int dry = 0; dry < 2; dry++ )
         {
            US_DataPubImporter          importer;
            US_DataPubImporter::Options options = dbOptions();
            options.dryRun = ( dry == 1 );
            QString                     error;

            importer.setDatabase( db.db() );
            ASSERT_TRUE ( importer.inspect( bundlePath, error ) )
               << error.toStdString();
            EXPECT_FALSE( importer.runImport( options, error ) )
               << ( dry ? "dry run" : "import" );
            EXPECT_TRUE ( error.contains( what ) ) << error.toStdString();
         }

         EXPECT_EQ( db.begins,   0 );
         EXPECT_EQ( db.writes(), 0 );
      }

      QString resultName( const QList< US_DataPubImporter::Result >& results,
                          US_DataPub::EntityType type )
      {
         for ( int ii = 0; ii < results.size(); ii++ )
            if ( results[ ii ].type == type )  return results[ ii ].targetName;

         return QString();
      }

      // The analyte's payload, with its XML text changed
      void editAnalyteXml( std::function< void( QString& ) > change,
                           bool dropSpectra )
      {
         craft( [ & ]( US_DataPubManifest& m, const QString& stage )
         {
            QList< US_DataPubEntity > list = m.section( US_DataPub::Analyte );
            ASSERT_EQ( list.size(), 1 );

            QString path = stage + "/" + list[ 0 ].payload;
            QFile   file( path );
            ASSERT_TRUE( file.open( QIODevice::ReadOnly ) );
            QString text = QString::fromUtf8( file.readAll() );
            file.close();

            change( text );

            ASSERT_TRUE( file.open( QIODevice::WriteOnly | QIODevice::Truncate ) );
            file.write( text.toUtf8() );
            file.close();

            list[ 0 ].sha256 = US_DataPubHash::fileHash( path );

            if ( dropSpectra )
            {
               list[ 0 ].attrs.remove( "spectraPayload" );
               list[ 0 ].attrs.remove(
                     US_DataPub::digestKeyOf( "spectraPayload" ) );
            }

            m.setSection( US_DataPub::Analyte, list );
         } );
      }
};

// Someone else's analyte of the bundle's GUID is not hung from the
// investigator's solution -- new_solutionAnalyte would refuse it half way
// through the import.  The import makes a copy of the bundle's analyte
// under a GUID of its own, and a dry run says so too.
TEST_F( DataPubMockRules, AnotherInvestigatorsAnalyteIsCopied )
{
   fake.addRecord( "analyte", analyteGUID, 300, 9, "Demo analyte" );

   US_DataPubImporter          dry;
   US_DataPubImporter::Options options = dbOptions();
   options.dryRun = true;
   QString                     error;
   ASSERT_TRUE( importOnce( dry, error, options ) ) << error.toStdString();
   EXPECT_EQ  ( resolutionOf( dry.results(), US_DataPub::Analyte ),
                US_DataPub::ResolvedCreated );

   US_DataPubImporter importer;
   ASSERT_TRUE( importOnce( importer, error, dbOptions() ) )
      << error.toStdString() << "\n"
      << importer.log().join( "\n" ).toStdString();

   ASSERT_EQ( fake.callsOf( "new_analyte" ).size(), 1 );
   QString copy = fake.callsOf( "new_analyte" )[ 0 ].value( 1 );
   EXPECT_NE( copy, analyteGUID );
   EXPECT_TRUE( US_DataPubImporter::guidIsUsable( copy ) );

   ASSERT_EQ( fake.callsOf( "new_solutionAnalyte" ).size(), 1 );
   EXPECT_EQ( fake.callsOf( "new_solutionAnalyte" )[ 0 ].value( 3 ), copy );
   EXPECT_EQ( resolutionOf( importer.results(), US_DataPub::Analyte ),
              US_DataPub::ResolvedCreated );
   EXPECT_EQ( fake.commits, 1 );
}

// An administrator may use any analyte (verify_analyte_permission): the
// target's is reused as it is
TEST_F( DataPubMockRules, AnAdministratorReusesAnyAnalyte )
{
   fake.admin = true;
   fake.addRecord( "analyte", analyteGUID, 300, 9, "Demo analyte" );

   US_DataPubImporter importer;
   QString            error;
   ASSERT_TRUE( importOnce( importer, error, dbOptions() ) )
      << error.toStdString();

   EXPECT_EQ( fake.callsOf( "new_analyte" ).size(), 0 );
   ASSERT_EQ( fake.callsOf( "new_solutionAnalyte" ).size(), 1 );
   EXPECT_EQ( fake.callsOf( "new_solutionAnalyte" )[ 0 ].value( 3 ),
              analyteGUID );
   EXPECT_EQ( resolutionOf( importer.results(), US_DataPub::Analyte ),
              US_DataPub::ResolvedReused );
}

// Another investigator's analyte that the bundle does not carry cannot be
// copied: the import is refused before it starts
TEST_F( DataPubMockRules, AnotherInvestigatorsAnalyteTheBundleLacksIsRefused )
{
   craft( []( US_DataPubManifest& m, const QString& )
          { leaveOut( m, US_DataPub::Analyte ); } );

   fake.addRecord( "analyte", analyteGUID, 300, 9, "Demo analyte" );

   expectRefusedEarly( fake, "another investigator's analyte" );
}

// Another investigator's private buffer is not linked to the solution
// (no procedure would stop that): the import makes a copy of its own.  A
// public one is shared, and reused.
TEST_F( DataPubMockRules, AnotherInvestigatorsPrivateBufferIsCopied )
{
   fake.addRecord( "buffer", bufferGUID, 300, 9, "Demo buffer" );

   US_DataPubImporter importer;
   QString            error;
   ASSERT_TRUE( importOnce( importer, error, dbOptions() ) )
      << error.toStdString();

   ASSERT_EQ( fake.callsOf( "new_buffer" ).size(), 1 );
   QString copy = fake.callsOf( "new_buffer" )[ 0 ].value( 1 );
   EXPECT_NE( copy, bufferGUID );
   ASSERT_EQ( fake.callsOf( "new_solutionBuffer" ).size(), 1 );
   EXPECT_EQ( fake.callsOf( "new_solutionBuffer" )[ 0 ].value( 3 ), copy );

   DataPubFakeDb shared;
   shared.addHardware( "DEMO-1" );
   shared.addRecord( "buffer", bufferGUID, 300, 9, "Demo buffer" );
   shared.state.publicBuffers << bufferGUID;

   US_DataPubImporter second;
   second.setDatabase( shared.db() );
   ASSERT_TRUE( second.inspect( bundlePath, error ) );
   ASSERT_TRUE( second.runImport( dbOptions(), error ) ) << error.toStdString();
   EXPECT_EQ  ( shared.callsOf( "new_buffer" ).size(), 0 );
   ASSERT_EQ  ( shared.callsOf( "new_solutionBuffer" ).size(), 1 );
   EXPECT_EQ  ( shared.callsOf( "new_solutionBuffer" )[ 0 ].value( 3 ),
                bufferGUID );
}

// A target without channel 1, which every raw data and solution row names,
// is found before anything is written; a dry run says so too
TEST_F( DataPubMockRules, AMissingChannelIsFoundBeforeTheImport )
{
   fake.channelRow1 = false;

   expectRefusedEarly( fake, "channel 1" );
}

// A noise record whose edit the target cannot give is refused in the
// preflight, not written with edit 0 for the server to refuse
TEST_F( DataPubMockRules, ANoiseWithoutAnEditIsRefusedBeforeTheImport )
{
   craft( []( US_DataPubManifest& m, const QString& )
          { leaveOut( m, US_DataPub::Model ); } );

   // The investigator's own model, fitted to no edit the target has
   fake.addRecord( "model", modelGUID, 300, 2, "their model" );

   expectRefusedEarly( fake, "edit" );
}

// A project the import makes from what a run names is reported with the
// records, by the dry run as by the import
TEST_F( DataPubMockRules, AProjectMadeFromARunIsReported )
{
   craft( []( US_DataPubManifest& m, const QString& )
          { leaveOut( m, US_DataPub::Project ); } );

   for ( int dry = 1; dry >= 0; dry-- )
   {
      US_DataPubImporter          importer;
      US_DataPubImporter::Options options = dbOptions();
      options.dryRun = ( dry == 1 );
      QString                     error;
      ASSERT_TRUE( importOnce( importer, error, options ) )
         << error.toStdString();

      QList< US_DataPubImporter::Result > results = importer.results();
      int                                 found   = 0;

      for ( int ii = 0; ii < results.size(); ii++ )
      {
         if ( results[ ii ].type != US_DataPub::Project )  continue;

         found++;
         EXPECT_EQ( results[ ii ].guid, projectGUID );
         EXPECT_EQ( results[ ii ].resolution, US_DataPub::ResolvedCreated );
         EXPECT_TRUE( results[ ii ].detail.contains( runID ) )
            << results[ ii ].detail.toStdString();
      }

      EXPECT_EQ( found, 1 ) << ( dry ? "dry run" : "import" );
   }

   EXPECT_EQ( fake.callsOf( "new_project2" ).size(), 1 );
}

// The raw data, edit and time state of a renamed run are reported under
// the names they have in the target
TEST_F( DataPubMockRules, ARenamedRunsFilesAreReportedUnderTheirNewNames )
{
   fake.addExperiment( 50, US_Util::new_guid(), runID );

   US_DataPubImporter importer;
   QString            error;
   ASSERT_TRUE( importOnce( importer, error, dbOptions() ) )
      << error.toStdString();

   QString newRun = runID + "-imported";
   QList< US_DataPubImporter::Result > results = importer.results();

   EXPECT_EQ( resultName( results, US_DataPub::RawData ),
              newRun + rawFile.mid( runID.size() ) );
   EXPECT_EQ( resultName( results, US_DataPub::EditedData ),
              newRun + editFile.mid( runID.size() ) );
   EXPECT_TRUE( resultName( results, US_DataPub::TimeState )
                .startsWith( newRun + "." ) )
      << resultName( results, US_DataPub::TimeState ).toStdString();

   QString log = importer.log().join( "\n" );
   EXPECT_FALSE( log.contains( "\"" + rawFile + "\"" ) ) << log.toStdString();
}

// The investigator's own raw data under another of their runs is not said
// to be someone else's
TEST_F( DataPubMockRules, OwnRawDataUnderAnotherRunIsNotCalledForeign )
{
   fake.addRecord( "rawData", rawGUID, 51, 2 );

   US_DataPubImporter importer;
   QString            error;
   EXPECT_FALSE( importOnce( importer, error, dbOptions() ) );
   EXPECT_TRUE ( error.contains( "same investigator" ) ) << error.toStdString();
   EXPECT_FALSE( error.contains( "another investigator" ) )
      << error.toStdString();
   EXPECT_EQ   ( fake.begins, 0 );
}

// An old bundle's extinction value that is not a finite number refuses
// the bundle in the preflight
TEST_F( DataPubMockRules, AnOldBundlesSpectrumHasToBeFinite )
{
   editAnalyteXml( []( QString& text )
   {
      text.remove( QRegularExpression( "<extinction [^>]*/>" ) );
      text.replace( "</sequence>", "</sequence><extinction frequency=\"230\""
                                   " value=\"nan\"/>" );
   }, true );

   expectRefusedEarly( fake, "finite" );
}

// An old bundle's protein carries the 280 nm value US_Analyte computed
// from its sequence; a lone point at 280 nm is that, and not stored as if
// it were measured
TEST_F( DataPubMockRules, AnOldBundlesComputedPointIsNotStored )
{
   editAnalyteXml( []( QString& text )
   {
      text.remove( QRegularExpression( "<extinction [^>]*/>" ) );
      text.replace( QRegularExpression( "<sequence>\\s*</sequence>" ),
                    "<sequence>\nMKWVTFISLLLLFSSAYS\n</sequence>"
                    "<extinction frequency=\"280\" value=\"5960\"/>" );
   }, true );

   US_DataPubImporter importer;
   QString            error;
   ASSERT_TRUE( importOnce( importer, error, dbOptions() ) )
      << error.toStdString();

   EXPECT_EQ( analyteProfiles(), QStringList() );
   EXPECT_EQ( fake.callsOf( "new_analyte" ).value( 0 ).value( 6 ), QString() );
}

// A sequence goes in exactly as the bundle holds it -- a FASTA header,
// residue numbers and line breaks are the analyte's own -- as a disk
// import keeps it; and the target's copy is the same analyte to a second
// import of the bundle
TEST_F( DataPubMockRules, ASequenceGoesInAsTheBundleHoldsIt )
{
   QString fasta =
      ">sp|P02768|ALBU_HUMAN Serum albumin OS=Homo sapiens\n"
      "1 MKWVTFISLL FLFSSAYSRG VFRRDAHKSE VAHRFKDLGE ENFKALVLIA FAQYLQQCPF\n"
      "61 EDHVKLVNEV TEFAKTCVAD ESAENCDKSL HTLFGDKLCT VATLRETYGE MADCCAKQEP\n";
   QString escaped = fasta;
   escaped.replace( ">", "&gt;" );

   editAnalyteXml( [ & ]( QString& text )
   {
      text.replace( QRegularExpression( "<sequence>\\s*</sequence>" ),
                    "<sequence>" + escaped + "</sequence>" );
   }, false );

   US_DataPubImporter importer;
   QString            error;
   ASSERT_TRUE( importOnce( importer, error, dbOptions() ) )
      << error.toStdString();
   ASSERT_EQ( fake.callsOf( "new_analyte" ).size(), 1 );
   EXPECT_EQ( fake.callsOf( "new_analyte" )[ 0 ].value( 3 ), fasta );

   // A second import finds the stored row, sequence and all, the same
   US_DataPubImporter probe;
   ASSERT_TRUE( probe.inspect( bundlePath, error ) );
   US_Analyte source;
   ASSERT_EQ( source.readFromFile( probe.bundleRoot() + "/"
                 + probe.manifest().section( US_DataPub::Analyte )
                   .value( 0 ).payload ), IUS_DB2::OK );

   DataPubFakeDb other;
   other.addHardware( "DEMO-1" );
   QString copy = US_Util::new_guid();
   other.addRecord( "analyte", copy, 40, 2, "Demo analyte" );
   other.cannedFor.insert( "get_analyte_info:40", QList< QVariantList >()
      << ( QVariantList() << copy << "Protein" << fasta
                          << QString::number( source.vbar20, 'g', 17 )
                          << "Demo analyte" << ""
                          << QString::number( source.mw, 'g', 17 ) << "0" ) );

   US_DataPubImporter second;
   second.setDatabase( other.db() );
   ASSERT_TRUE( second.inspect( bundlePath, error ) );
   ASSERT_TRUE( second.runImport( dbOptions(), error ) ) << error.toStdString();
   EXPECT_EQ( other.callsOf( "new_analyte" ).size(), 0 );
   EXPECT_EQ( resolutionOf( second.results(), US_DataPub::Analyte ),
              US_DataPub::ResolvedReused );
}

// Text a latin1 target cannot store is refused by the preflight -- by the
// dry run as by the import, before anything is written -- naming the
// record, the field and the character; text latin1 holds goes in
TEST_F( DataPubMockRules, TextTheTargetCannotStoreIsRefusedUpFront )
{
   QString alpha = QString::fromUtf8( "Prot\xC3\xA9ine \xCE\xB1" );
   QString acute = QString::fromUtf8( "Prot\xC3\xA9ine \xC3\xA9" );

   editAnalyteXml( [ & ]( QString& text )
   {
      text.replace( "description=\"Demo analyte\"",
                    "description=\"" + alpha + "\"" );
   }, false );

   for ( bool shown : { true, false } )
   {
      SCOPED_TRACE( shown ? "information_schema" : "the schema's default" );
      DataPubFakeDb latin;
      latin.addHardware( "DEMO-1" );
      latin.charset    = "latin1";
      latin.infoSchema = shown;

      for ( int dry = 1; dry >= 0; dry-- )
      {
         US_DataPubImporter          importer;
         US_DataPubImporter::Options options = dbOptions();
         options.dryRun = ( dry == 1 );
         QString                     error;

         importer.setDatabase( latin.db() );
         ASSERT_TRUE ( importer.inspect( bundlePath, error ) );
         EXPECT_FALSE( importer.runImport( options, error ) )
            << ( dry ? "dry run" : "import" );
         EXPECT_TRUE ( error.contains( QString::fromUtf8( "\xCE\xB1" ) )  &&
                       error.contains( "U+03B1" )                         &&
                       error.contains( "description" )                    &&
                       error.contains( "nalyte" )                         &&
                       error.contains( "latin1" ) ) << error.toStdString();
      }

      EXPECT_EQ( latin.begins,   0 );
      EXPECT_EQ( latin.writes(), 0 );
   }

   // A target that holds it takes it
   US_DataPubImporter wide;
   QString            error;
   ASSERT_TRUE( importOnce( wide, error, dbOptions() ) ) << error.toStdString();
   EXPECT_EQ( fake.callsOf( "new_analyte" ).value( 0 ).value( 5 ), alpha );

   // ... and latin1 holds an acute accent
   editAnalyteXml( [ & ]( QString& text )
   {
      text.replace( "description=\"" + alpha + "\"",
                    "description=\"" + acute + "\"" );
   }, false );

   DataPubFakeDb latin;
   latin.addHardware( "DEMO-1" );
   latin.charset = "latin1";

   US_DataPubImporter plain;
   plain.setDatabase( latin.db() );
   ASSERT_TRUE( plain.inspect( bundlePath, error ) );
   ASSERT_TRUE( plain.runImport( dbOptions(), error ) ) << error.toStdString();
   EXPECT_EQ( latin.callsOf( "new_analyte" ).value( 0 ).value( 5 ), acute );
}

// A number that is not finite is refused by the preflight of a database
// import too
TEST_F( DataPubMockRules, ANumberThatIsNotFiniteIsRefusedUpFront )
{
   editAnalyteXml( []( QString& text )
   {
      text.replace( QRegularExpression( "vbar20=\"[^\"]*\"" ),
                    "vbar20=\"inf\"" );
   }, false );

   expectRefusedEarly( fake, "vbar20" );
}

// A protein the bundle carries from a disk store holds the 280 nm value
// US_Analyte computed; the same analyte in the database holds no such
// point.  They are the same analyte, and the target's is reused.
TEST_F( DataPubMockRules, ADiskProteinMatchesItsDatabaseCopy )
{
   QString seq = "MKWVTFISLLLLFSSAYSRGVFRRDTHKSEIAHRFKD";

   editAnalyteXml( [ & ]( QString& text )
   {
      text.remove( QRegularExpression( "<extinction [^>]*/>" ) );
      text.replace( QRegularExpression( "<sequence>\\s*</sequence>" ),
                    "<sequence>\n" + seq + "\n</sequence>"
                    "<extinction frequency=\"280\" value=\"5960\"/>" );
   }, false );

   US_DataPubImporter probe;
   QString            error;
   ASSERT_TRUE( probe.inspect( bundlePath, error ) );
   QString     analytePath = probe.bundleRoot() + "/"
                             + probe.manifest().section( US_DataPub::Analyte )
                               .value( 0 ).payload;
   US_Analyte  source;
   ASSERT_EQ( source.readFromFile( analytePath ), IUS_DB2::OK );

   // The same analyte in the database, under another GUID
   QString other = US_Util::new_guid();
   fake.addRecord( "analyte", other, 40, 2, "Demo analyte" );
   fake.cannedFor.insert( "get_analyte_info:40", QList< QVariantList >()
      << ( QVariantList() << other << "Protein" << seq
                          << QString::number( source.vbar20, 'g', 17 )
                          << "Demo analyte" << ""
                          << QString::number( source.mw, 'g', 17 ) << "0" ) );

   US_DataPubImporter importer;
   ASSERT_TRUE( importOnce( importer, error, dbOptions() ) )
      << error.toStdString();

   EXPECT_EQ( fake.callsOf( "new_analyte" ).size(), 0 );
   EXPECT_EQ( resolutionOf( importer.results(), US_DataPub::Analyte ),
              US_DataPub::ResolvedReused );
}

// A target record of the name that cannot be read is a failure, not a
// different record: a renamed duplicate would be made of it
TEST_F( DataPubMockRules, ATargetRecordThatCannotBeReadFailsTheImport )
{
   QString other = US_Util::new_guid();
   fake.addRecord( "buffer", other, 40, 2, "Demo buffer" );
   fake.failOn = "get_buffer_components";

   US_DataPubImporter importer;
   QString            error;
   EXPECT_FALSE( importOnce( importer, error, dbOptions() ) );
   EXPECT_TRUE ( error.contains( "could not be read" ) ) << error.toStdString();
   EXPECT_EQ   ( fake.callsOf( "new_buffer" ).size(), 0 );
   EXPECT_EQ   ( fake.writes(), 0 );
}

// A model's variance and meniscus are columns of its row: a bundle that
// carries them beside an XML without them has them stored -- with every
// digit the manifest gives, not the six of US_Model's XML
TEST_F( DataPubMockRules, AModelsVarianceAndMeniscusComeFromTheManifest )
{
   const double variance = 1.0 / 3.0e5;
   const double meniscus = 5.8712345678901;

   craft( [ & ]( US_DataPubManifest& m, const QString& )
   {
      QList< US_DataPubEntity > list = m.section( US_DataPub::Model );
      ASSERT_EQ( list.size(), 1 );
      list[ 0 ].attrs.insert( "variance", QString::number( variance, 'g', 17 ) );
      list[ 0 ].attrs.insert( "meniscus", QString::number( meniscus, 'g', 17 ) );
      m.setSection( US_DataPub::Model, list );
   } );

   US_DataPubImporter importer;
   QString            error;
   ASSERT_TRUE( importOnce( importer, error, dbOptions() ) )
      << error.toStdString();

   ASSERT_EQ( fake.callsOf( "new_model" ).size(), 1 );
   EXPECT_EQ( fake.callsOf( "new_model" )[ 0 ].value( 4 ).toDouble(), variance )
      << fake.callsOf( "new_model" )[ 0 ].value( 4 ).toStdString();
   EXPECT_EQ( fake.callsOf( "new_model" )[ 0 ].value( 5 ).toDouble(), meniscus )
      << fake.callsOf( "new_model" )[ 0 ].value( 5 ).toStdString();
}

// The manifest's values are the source row's, at full precision; the XML's
// are US_Model's six digits of them, and do not win
TEST_F( DataPubMockRules, TheManifestsVarianceWinsOverTheXmls )
{
   const double variance = 2.0 / 3.0e4;

   craft( [ & ]( US_DataPubManifest& m, const QString& stage )
   {
      QList< US_DataPubEntity > list = m.section( US_DataPub::Model );
      ASSERT_EQ( list.size(), 1 );

      // The XML says 6.66667e-05
      QString path = stage + "/" + list[ 0 ].payload;
      QFile   file( path );
      ASSERT_TRUE( file.open( QIODevice::ReadOnly ) );
      QString text = QString::fromUtf8( file.readAll() );
      file.close();
      ASSERT_TRUE( text.contains( "<model " ) );
      text.remove( QRegularExpression( " variance=\"[^\"]*\"" ) );
      text.replace( "<model ", "<model variance=\"" + QString::number( variance )
                               + "\" " );
      ASSERT_TRUE( file.open( QIODevice::WriteOnly | QIODevice::Truncate ) );
      file.write( text.toUtf8() );
      file.close();
      list[ 0 ].sha256 = US_DataPubHash::fileHash( path );

      list[ 0 ].attrs.insert( "variance", QString::number( variance, 'g', 17 ) );
      m.setSection( US_DataPub::Model, list );
   } );

   US_DataPubImporter importer;
   QString            error;
   ASSERT_TRUE( importOnce( importer, error, dbOptions() ) )
      << error.toStdString();

   ASSERT_EQ( fake.callsOf( "new_model" ).size(), 1 );
   EXPECT_EQ( fake.callsOf( "new_model" )[ 0 ].value( 4 ).toDouble(), variance )
      << fake.callsOf( "new_model" )[ 0 ].value( 4 ).toStdString();
}

// The concentrations a bundle carries for a buffer's components are
// matched by component ID, not by their order in the definitions file
class DataPubMockComponents : public DataPubMockExtras
{
};

TEST_F( DataPubMockComponents, ConcentrationsAreMatchedByComponentId )
{
   QMap< QString, US_BufferComponent > table;
   US_BufferComponent::getAllFromHD( table );
   ASSERT_TRUE( table.contains( "1" )  &&  table.contains( "2" ) )
      << "no local component table";

   US_Buffer buffer;
   QString   bpath = US_Settings::dataDir() + "/buffers/B0000001.xml";
   ASSERT_TRUE( buffer.readFromDisk( bpath ) );
   buffer.component     << table.value( "1" ) << table.value( "2" );
   buffer.componentIDs  << "1" << "2";
   buffer.concentration << 150.0 << 25.0;
   ASSERT_TRUE( buffer.writeToDisk( bpath ) );
   reexport();

   // The definitions file with its two components the other way round
   US_DataPubImporter source;
   QString            error;
   ASSERT_TRUE( source.inspect( bundlePath, error ) ) << error.toStdString();

   US_DataPubBundle out;
   ASSERT_TRUE( out.createStaging( error ) );
   QString stage = out.stagingPath();

   QDir        src( source.bundleRoot() );
   QStringList files;
   QDirIterator it( source.bundleRoot(), QDir::Files,
                    QDirIterator::Subdirectories );

   while ( it.hasNext() )  files << src.relativeFilePath( it.next() );

   for ( int ii = 0; ii < files.size(); ii++ )
   {
      QDir().mkpath( QFileInfo( stage + "/" + files[ ii ] ).path() );
      ASSERT_TRUE( QFile::copy( source.bundleRoot() + "/" + files[ ii ],
                                stage + "/" + files[ ii ] ) );
   }

   US_DataPubManifest mani;
   QString            mpath = stage + "/" + US_DataPubBundle::manifestName();
   ASSERT_TRUE( mani.read( mpath, error ) ) << error.toStdString();

   QList< US_DataPubEntity > bufs = mani.section( US_DataPub::Buffer );
   ASSERT_EQ( bufs.size(), 1 );
   QString cpath = stage + "/" + bufs[ 0 ].attrs.value( "componentsPayload" );

   QMap< QString, US_BufferComponent > defs;
   ASSERT_TRUE( US_DataPubRecords::readBufferComponents( cpath, defs ) );
   ASSERT_EQ  ( defs.size(), 2 );

   QList< US_BufferComponent > reversed;
   reversed << defs.value( "2" ) << defs.value( "1" );
   QFile::remove( cpath );
   ASSERT_TRUE( US_DataPubRecords::writeBufferComponents(
                   reversed, QList< double >() << 25.0 << 150.0, cpath ) );

   bufs[ 0 ].attrs.insert( US_DataPub::digestKeyOf( "componentsPayload" ),
                           US_DataPubHash::fileHash( cpath ) );
   mani.setSection( US_DataPub::Buffer, bufs );
   ASSERT_TRUE( mani.write( mpath, error ) ) << error.toStdString();

   QFile digest( stage + "/" + US_DataPubExporter::manifestDigestName() );
   ASSERT_TRUE( digest.open( QIODevice::WriteOnly | QIODevice::Truncate ) );
   digest.write( ( US_DataPubHash::fileHash( mpath ) + "  "
                   + US_DataPubBundle::manifestName() + "\n" ).toUtf8() );
   digest.close();

   bundlePath = root + "/reversed.tar.gz";
   QFile::remove( bundlePath );
   ASSERT_TRUE( out.pack( bundlePath, error ) ) << error.toStdString();

   US_DataPubImporter importer;
   ASSERT_TRUE( importOnce( importer, error, dbOptions() ) )
      << error.toStdString();

   // Each component with its own concentration, whatever IDs the target
   // gave them
   QList< QStringList > adds = fake.callsOf( "add_buffer_component" );
   ASSERT_EQ( adds.size(), 2 );

   QMap< QString, QString > concOf;   // component name -> concentration

   for ( int ii = 0; ii < adds.size(); ii++ )
   {
      int id = adds[ ii ].value( 2 ).toInt();
      ASSERT_TRUE( fake.state.components.contains( id ) );
      concOf.insert( fake.state.components.value( id ).value( 1 ).toString(),
                     adds[ ii ].value( 3 ) );
   }

   EXPECT_EQ( concOf.value( table.value( "1" ).name ).toDouble(), 150.0 );
   EXPECT_EQ( concOf.value( table.value( "2" ).name ).toDouble(),  25.0 );
}

// ------------------------------------------------ staging directories

#if defined( Q_OS_UNIX )
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

// A staging directory whose importer is gone is swept; a live importer's
// is not, whatever its age
TEST_F( DataPubMockImport, StaleStagingDirectoriesAreSwept )
{
   US_DataPubImporter live;
   QString            error;
   ASSERT_TRUE( live.inspect( bundlePath, error ) ) << error.toStdString();
   QString liveDir = QFileInfo( live.bundleRoot() ).path();
   ASSERT_TRUE( QFile::exists( liveDir + "/import.lock" ) );

   QString parent = US_Settings::tmpDir() + "/us_datapub";
   ASSERT_TRUE( QDir().mkpath( parent + "/bundle_dead" ) );

   // The lock of a process that took it and was killed
   fflush( nullptr );
   pid_t pid = fork();
   ASSERT_GE( pid, 0 );

   if ( pid == 0 )
   {
      QLockFile held( parent + "/bundle_dead/import.lock" );
      held.tryLock( 0 );
      _exit( held.isLocked() ? 0 : 1 );    // Without unlocking it
   }

   int status = 0;
   ASSERT_EQ( waitpid( pid, &status, 0 ), pid );
   ASSERT_TRUE( WIFEXITED( status )  &&  WEXITSTATUS( status ) == 0 );
   ASSERT_TRUE( QFile::exists( parent + "/bundle_dead/import.lock" ) );

   // A directory without a lock that has not changed for days
   ASSERT_TRUE( QDir().mkpath( parent + "/bundle_old" ) );
   QDateTime past = QDateTime::currentDateTimeUtc().addDays( -3 );
   struct timeval times[ 2 ];
   times[ 0 ].tv_sec  = time_t( past.toSecsSinceEpoch() );
   times[ 0 ].tv_usec = 0;
   times[ 1 ]         = times[ 0 ];
   ASSERT_EQ( utimes( QFile::encodeName( parent + "/bundle_old" ).constData(),
                      times ), 0 );

   // ... and one that changed a moment ago
   ASSERT_TRUE( QDir().mkpath( parent + "/bundle_new" ) );

   EXPECT_EQ  ( US_DataPubImporter::sweepStaleStaging(), 2 );
   EXPECT_FALSE( QFile::exists( parent + "/bundle_old" ) );
   EXPECT_TRUE ( QFile::exists( parent + "/bundle_new" ) );
   EXPECT_FALSE( QFile::exists( parent + "/bundle_dead" ) );
   EXPECT_TRUE ( QFile::exists( liveDir ) );

   // ... and once the importer is gone, so is its directory
   live.inspect( bundlePath, error );
   EXPECT_FALSE( QFile::exists( liveDir ) );
}

// An importer stopped by a signal leaves its staging directory to the
// sweep: no handler removes it (a signal handler cannot do that safely),
// the signal ends the process as it would anyway, and the directory keeps
// the lock of a process that is gone, so the next import removes it
TEST_F( DataPubMockImport, AStoppedImportersDirectoryIsSweptNextTime )
{
   QString     parent = US_Settings::tmpDir() + "/us_datapub";
   QStringList before = QDir( parent ).entryList( QStringList( "bundle_*" ),
                                                  QDir::Dirs
                                                  | QDir::NoDotAndDotDot );
   fflush( nullptr );
   pid_t pid = fork();
   ASSERT_GE( pid, 0 );

   if ( pid == 0 )
   {
      US_DataPubImporter importer;
      QString            error;

      if ( ! importer.inspect( bundlePath, error ) )  _exit( 2 );

      raise( SIGTERM );
      _exit( 3 );                     // Not reached
   }

   int status = 0;
   ASSERT_EQ( waitpid( pid, &status, 0 ), pid );
   ASSERT_TRUE( WIFSIGNALED( status ) ) << "exit " << WEXITSTATUS( status );
   EXPECT_EQ( WTERMSIG( status ), SIGTERM );

   QStringList after = QDir( parent ).entryList( QStringList( "bundle_*" ),
                                                 QDir::Dirs
                                                 | QDir::NoDotAndDotDot );
   QStringList left;

   for ( const QString& dir : after )
      if ( ! before.contains( dir ) )  left << dir;

   ASSERT_EQ( left.size(), 1 );
   QString stage = parent + "/" + left[ 0 ];
   EXPECT_TRUE( QFile::exists( stage + "/import.lock" ) );

   EXPECT_GE  ( US_DataPubImporter::sweepStaleStaging(), 1 );
   EXPECT_FALSE( QFile::exists( stage ) );
}

// The lock is in the staging directory from the moment it is made -- a
// sweep never finds a live directory without it -- and it is the last
// thing removed
TEST_F( DataPubMockImport, TheStagingDirectoryIsLockedFromTheStart )
{
   US_DataPubBundle bundle;
   QString          error;
   bundle.setStagingLock( "import.lock" );
   ASSERT_TRUE( bundle.createStaging( error ) ) << error.toStdString();

   QString base = QFileInfo( bundle.stagingPath() ).path();
   EXPECT_TRUE( bundle.stagingLocked() );
   ASSERT_TRUE( QFile::exists( base + "/import.lock" ) );

   // Held: a probe cannot take it, and the sweep leaves the directory
   QLockFile probe( base + "/import.lock" );
   probe.setStaleLockTime( 0 );
   EXPECT_FALSE( probe.tryLock( 0 ) );

   // ... even one that has not changed for days
   QDateTime past = QDateTime::currentDateTimeUtc().addDays( -3 );
   struct timeval times[ 2 ];
   times[ 0 ].tv_sec  = time_t( past.toSecsSinceEpoch() );
   times[ 0 ].tv_usec = 0;
   times[ 1 ]         = times[ 0 ];
   ASSERT_EQ( utimes( QFile::encodeName( base ).constData(), times ), 0 );
   US_DataPubImporter::sweepStaleStaging();
   EXPECT_TRUE( QFile::exists( bundle.stagingPath() ) );

   bundle.cleanup();
   EXPECT_FALSE( bundle.stagingLocked() );
   EXPECT_FALSE( QFile::exists( base ) );

   // A bundle that fails to unpack leaves nothing either
   QString junk = root + "/junk.tar.gz";
   QFile   file( junk );
   ASSERT_TRUE( file.open( QIODevice::WriteOnly ) );
   file.write( "not an archive" );
   file.close();

   QStringList before = QDir( US_Settings::tmpDir() + "/us_datapub" )
                        .entryList( QDir::Dirs | QDir::NoDotAndDotDot );
   US_DataPubImporter importer;
   EXPECT_FALSE( importer.inspect( junk, error ) );
   EXPECT_EQ( QDir( US_Settings::tmpDir() + "/us_datapub" )
              .entryList( QDir::Dirs | QDir::NoDotAndDotDot ), before );
}
#endif

// new_experiment refuses an operator without a permit for the instrument:
// the import records one who holds a permit instead, with a warning -- or,
// when no one does, refuses the run, in the dry run too
TEST_F( DataPubMockRules, TheOperatorHoldsAPermitForTheInstrument )
{
   fake.operators = QList< int >() << 5;

   US_DataPubImporter importer;
   QString            error;
   ASSERT_TRUE( importOnce( importer, error, dbOptions() ) )
      << error.toStdString();

   ASSERT_EQ( fake.callsOf( "new_experiment" ).size(), 1 );
   EXPECT_EQ( fake.callsOf( "new_experiment" )[ 0 ].value( 6 ), "5" );
   EXPECT_TRUE( importer.warnings().join( "\n" ).contains( "operator" ) )
      << importer.warnings().join( "\n" ).toStdString();

   DataPubFakeDb none;
   none.addHardware( "DEMO-1" );
   none.operators.clear();

   US_DataPubImporter          dry;
   US_DataPubImporter::Options options = dbOptions();
   options.dryRun = true;
   dry.setDatabase( none.db() );
   ASSERT_TRUE ( dry.inspect( bundlePath, error ) );
   EXPECT_FALSE( dry.runImport( options, error ) );
   EXPECT_TRUE ( error.contains( "permit" ) ) << error.toStdString();
}

// ------------------------------------------------- the minors of round six

// The warning of a component ID that is taken is one of the import's
// warnings: a strict import fails on it, in the dry run already
TEST_F( DataPubMockExtras, AComponentIdThatIsTakenIsAWarningOfTheImport )
{
   exportWithComponent();

   fake.addComponent( 1, "Sodium chloride", "mM", "", "0 0 0 0 0 0",
                      "0 0 0 0 0 0" );
   fake.addComponent( 3, "Cadmium chloride", "mM", "", "0 0 0 0 0 0",
                      "0 0 0 0 0 0" );

   US_DataPubImporter          importer;
   US_DataPubImporter::Options options = dbOptions();
   options.dryRun         = true;
   options.strictHardware = true;
   QString                     error;
   EXPECT_FALSE( importOnce( importer, error, options ) );
   EXPECT_TRUE ( error.contains( "IDs 3" ) ) << error.toStdString();
   EXPECT_EQ   ( fake.writes(), 0 );
}

// A server that says its create_buffer_component counts no rows -- by its
// catalog version marker, or by a definition the login may read -- is not
// warned of; one whose readable definition counts rows still is, and so is
// one that cannot be told apart
TEST_F( DataPubMockExtras, AnUpdatedServerIsNotWarnedOfComponentIds )
{
   exportWithComponent();

   fake.patchedComponents = true;
   fake.addComponent( 1, "Sodium chloride", "mM", "", "0 0 0 0 0 0",
                      "0 0 0 0 0 0" );
   fake.addComponent( 3, "Cadmium chloride", "mM", "", "0 0 0 0 0 0",
                      "0 0 0 0 0 0" );

   const QString updated =
      "BEGIN DECLARE l_componentID INT; SELECT COALESCE( MAX("
      " bufferComponentID ), 0 ) + 1 INTO l_componentID FROM bufferComponent;"
      " END";
   const QString counting =
      "BEGIN DECLARE count_bufferscomps INT; SELECT COUNT(*) INTO"
      " count_bufferscomps FROM bufferComponent; END";

   struct Case
   {
      bool    marker;
      QString definition;
      bool    warned;
   };

   const Case cases[] = { { true,  QString(), false },
                          { false, updated,   false },
                          { false, counting,  true  },
                          { false, QString(), true  } };

   for ( const Case& cc : cases )
   {
      fake.routines.clear();
      fake.routines.insert( "create_buffer_component", cc.definition );
      fake.catalogVersion = cc.marker ? 1 : 0;

      US_DataPubImporter          dry;
      US_DataPubImporter::Options options = dbOptions();
      options.dryRun = true;
      QString                     error;
      ASSERT_TRUE( importOnce( dry, error, options ) ) << error.toStdString();
      EXPECT_EQ  ( dry.warnings().join( "\n" ).contains( "row count" ),
                   cc.warned ) << dry.warnings().join( "\n" ).toStdString();
   }

   // ... and the import goes through, linked to the row the updated
   // procedure added (the highest ID plus one)
   US_DataPubImporter importer;
   QString            error;
   ASSERT_TRUE( importOnce( importer, error, dbOptions() ) )
      << error.toStdString();
   ASSERT_EQ( fake.callsOf( "add_buffer_component" ).size(), 1 );
   EXPECT_EQ( fake.callsOf( "add_buffer_component" )[ 0 ].value( 2 ), "4" );
}

// Of several rows of the same definition, the buffer is linked to the one
// of the highest ID: the rule of US_BufferComponent::matchingID(), which
// the library's own lookups follow too
TEST_F( DataPubMockExtras, OfIdenticalComponentsTheHighestIdIsTaken )
{
   exportWithComponent();

   fake.canned.insert( "get_buffer_component_desc", QList< QVariantList >()
                       << ( QVariantList() << 41 ) << ( QVariantList() << 42 ) );
   fake.canned.insert( "get_buffer_component_info", QList< QVariantList >()
                       << componentRow( component ) );

   US_DataPubImporter importer;
   QString            error;
   ASSERT_TRUE( importOnce( importer, error, dbOptions() ) )
      << error.toStdString();

   EXPECT_EQ( fake.callsOf( "create_buffer_component" ).size(), 0 );
   ASSERT_EQ( fake.callsOf( "add_buffer_component" ).size(), 1 );
   EXPECT_EQ( fake.callsOf( "add_buffer_component" )[ 0 ].value( 2 ), "42" );
}

// A rotor calibration without a label is named by its GUID in what the
// import says, never as ""
TEST_F( DataPubMockExtras, ACalibrationWithoutALabelIsNamedByItsGuid )
{
   QString path = US_Settings::dataDir() + "/rotors/C0000001.xml";
   US_Rotor::RotorCalibration calibration;
   ASSERT_TRUE( US_DataPubRecords::readCalibration( path, calibration ) );
   calibration.label.clear();
   ASSERT_TRUE( US_DataPubRecords::writeCalibration( calibration, path ) );
   reexport();

   US_DataPubImporter probe;
   QString            error;
   ASSERT_TRUE( probe.inspect( bundlePath, error ) );
   QList< US_DataPubEntity > cals =
         probe.manifest().section( US_DataPub::RotorCalibration );
   ASSERT_EQ( cals.size(), 1 );
   fake.registry[ "rotor" ].insert( cals[ 0 ].attrs.value( "rotorGUID" ), 3 );

   US_DataPubImporter importer;
   ASSERT_TRUE( importOnce( importer, error, dbOptions() ) )
      << error.toStdString();

   QString said = importer.warnings().join( "\n" );
   EXPECT_TRUE ( said.contains( "Rotor calibration " + cals[ 0 ].guid ) )
      << said.toStdString();
   EXPECT_FALSE( said.contains( "\"\"" ) ) << said.toStdString();
}

// Experiment 1 is the ID verify_experiment_permission lets anyone use, so
// the raw data of whoever's first run it became is found for everyone.
// Found is not one's own: the run is another investigator's
TEST_F( DataPubMockImport, TheRunOfExperimentOneIsAnotherInvestigators )
{
   fake.addExperiment( 1, expGUID, runID, 9 );
   fake.addRecord( "rawData", rawGUID, 51, 9 );
   fake.state.rawExperiments.insert( rawGUID, 1 );

   US_DataPubImporter importer;
   QString            error;

   EXPECT_FALSE( importOnce( importer, error, dbOptions() ) );
   EXPECT_TRUE ( error.contains( "another investigator's run" ) )
      << error.toStdString();
   EXPECT_FALSE( error.contains( "same investigator" ) ) << error.toStdString();
   EXPECT_EQ   ( fake.writes(), 0 );
}

// What the manifest carries of the source's rows beyond their payloads is
// written as it was: a buffer's private flag, an edit's comment, the raw
// data's label and comment, an analyte's spectrum column.  Without it,
// what the importer always wrote.
class DataPubMockFields : public DataPubMockRules
{
   protected:
      void setAttr( US_DataPubManifest& m, US_DataPub::EntityType type,
                    const QString& key, const QString& value )
      {
         QList< US_DataPubEntity > list = m.section( type );
         ASSERT_FALSE( list.isEmpty() );

         for ( int ii = 0; ii < list.size(); ii++ )
            list[ ii ].attrs.insert( key, value );

         m.setSection( type, list );
      }

      void exportAgain( const QString& name )
      {
         US_DataPubExporter            exporter;
         US_DataPubExporter::Selection selection;
         selection.fromDb = false;
         selection.scope  = US_DataPub::ScopeNoise;
         selection.runIDs << runID;
         bundlePath = root + "/" + name + ".tar.gz";
         QString error;
         ASSERT_TRUE( exporter.exportBundle( selection, bundlePath, error ) )
            << error.toStdString();
      }
};

TEST_F( DataPubMockFields, WithoutTheFieldsTheDefaultsAreWritten )
{
   US_DataPubImporter importer;
   QString            error;
   ASSERT_TRUE( importOnce( importer, error, dbOptions() ) )
      << error.toStdString();

   ASSERT_EQ( fake.callsOf( "new_buffer" ).size(), 1 );
   EXPECT_EQ( fake.callsOf( "new_buffer" )[ 0 ].value( 8 ), "1" );
   ASSERT_EQ( fake.callsOf( "new_editedData" ).size(), 1 );
   EXPECT_EQ( fake.callsOf( "new_editedData" )[ 0 ].value( 5 ), QString() );
   ASSERT_EQ( fake.callsOf( "new_rawData" ).size(), 1 );
   EXPECT_EQ( fake.callsOf( "new_rawData" )[ 0 ].value( 2 ),
              fake.callsOf( "new_experiment" ).value( 0 ).value( 13 ) );
}

TEST_F( DataPubMockFields, TheSourcesFieldsAreWritten )
{
   craft( [ & ]( US_DataPubManifest& m, const QString& )
   {
      setAttr( m, US_DataPub::Buffer,     "private",  "0" );
      setAttr( m, US_DataPub::EditedData, "comment",  "edited by hand" );
      setAttr( m, US_DataPub::RawData,    "label",    "raw label" );
      setAttr( m, US_DataPub::RawData,    "comment",  "raw comment" );
      setAttr( m, US_DataPub::Analyte,    "spectrum", "" );
   } );

   US_DataPubImporter importer;
   QString            error;
   ASSERT_TRUE( importOnce( importer, error, dbOptions() ) )
      << error.toStdString();

   ASSERT_EQ( fake.callsOf( "new_buffer" ).size(), 1 );
   EXPECT_EQ( fake.callsOf( "new_buffer" )[ 0 ].value( 8 ), "0" );
   EXPECT_TRUE( fake.state.publicBuffers.contains( bufferGUID ) );

   ASSERT_EQ( fake.callsOf( "new_editedData" ).size(), 1 );
   EXPECT_EQ( fake.callsOf( "new_editedData" )[ 0 ].value( 5 ),
              "edited by hand" );

   ASSERT_EQ( fake.callsOf( "new_rawData" ).size(), 1 );
   EXPECT_EQ( fake.callsOf( "new_rawData" )[ 0 ].value( 2 ), "raw label" );
   EXPECT_EQ( fake.callsOf( "new_rawData" )[ 0 ].value( 4 ), "raw comment" );

   // ... while the run keeps its own label
   EXPECT_NE( fake.callsOf( "new_experiment" ).value( 0 ).value( 13 ),
              "raw label" );

   ASSERT_EQ( fake.callsOf( "new_analyte" ).size(), 1 );
   EXPECT_EQ( fake.callsOf( "new_analyte" )[ 0 ].value( 6 ), QString() );
}

// A spectrum column the source filled is kept as it was, not made again
// from the extinction profile
TEST_F( DataPubMockFields, AnAnalytesSpectrumColumnIsKept )
{
   craft( [ & ]( US_DataPubManifest& m, const QString& )
   { setAttr( m, US_DataPub::Analyte, "spectrum", "260/1.5" ); } );

   US_DataPubImporter importer;
   QString            error;
   ASSERT_TRUE( importOnce( importer, error, dbOptions() ) )
      << error.toStdString();

   ASSERT_EQ( fake.callsOf( "new_analyte" ).size(), 1 );
   EXPECT_EQ( fake.callsOf( "new_analyte" )[ 0 ].value( 6 ), "260/1.5" );
}

// Raw data records of one run that disagree on their label are written
// with the run's, and the import says so
TEST_F( DataPubMockFields, RawDataThatDisagreeKeepTheRunsLabel )
{
   addSecondTriple();
   ASSERT_TRUE( secondTriple );
   exportAgain( "two_triples" );

   craft( [ & ]( US_DataPubManifest& m, const QString& )
   {
      QList< US_DataPubEntity > list = m.section( US_DataPub::RawData );
      ASSERT_EQ( list.size(), 2 );
      list[ 0 ].attrs.insert( "label", "one" );
      list[ 1 ].attrs.insert( "label", "two" );
      m.setSection( US_DataPub::RawData, list );
   } );

   US_DataPubImporter importer;
   QString            error;
   ASSERT_TRUE( importOnce( importer, error, dbOptions() ) )
      << error.toStdString();

   ASSERT_EQ( fake.callsOf( "new_rawData" ).size(), 2 );
   QString runLabel = fake.callsOf( "new_experiment" ).value( 0 ).value( 13 );
   EXPECT_EQ( fake.callsOf( "new_rawData" )[ 0 ].value( 2 ), runLabel );
   EXPECT_EQ( fake.callsOf( "new_rawData" )[ 1 ].value( 2 ), runLabel );
   EXPECT_TRUE( importer.log().join( "\n" ).contains( "do not share one label" ) )
      << importer.log().join( "\n" ).toStdString();
}

// A public buffer stays public, so another investigator's import of the
// same chemistry uses it instead of copying it
TEST_F( DataPubMockFields, APublicBufferIsUsedByAnotherInvestigator )
{
   craft( [ & ]( US_DataPubManifest& m, const QString& )
   { setAttr( m, US_DataPub::Buffer, "private", "0" ); } );

   US_DataPubImporter first;
   QString            error;
   ASSERT_TRUE( importOnce( first, error, dbOptions() ) )
      << error.toStdString();
   ASSERT_EQ( fake.callsOf( "new_buffer" ).size(), 1 );

   // Another run of the same buffer, by investigator 3
   QDir( US_Settings::dataDir()   ).removeRecursively();
   QDir( US_Settings::resultDir() ).removeRecursively();
   QDir().mkpath( US_Settings::dataDir()   );
   QDir().mkpath( US_Settings::resultDir() );
   projectGUID  = US_Util::new_guid();
   analyteGUID  = US_Util::new_guid();
   solutionGUID = US_Util::new_guid();
   expGUID      = US_Util::new_guid();
   rawGUID      = US_Util::new_guid();
   editGUID     = US_Util::new_guid();
   modelGUID    = US_Util::new_guid();
   noiseGUID    = US_Util::new_guid();
   calGUID      = US_Util::new_guid();
   buildStore();
   exportAgain( "second" );

   craft( [ & ]( US_DataPubManifest& m, const QString& )
   { setAttr( m, US_DataPub::Buffer, "private", "0" ); } );

   fake.person = 3;
   fake.operators << 3;
   US_Settings::set_us_inv_ID( 3 );

   US_DataPubImporter second;
   ASSERT_TRUE( importOnce( second, error, dbOptions() ) )
      << error.toStdString() << "\n" << second.log().join( "\n" ).toStdString();

   EXPECT_EQ( fake.callsOf( "new_buffer" ).size(), 1 );

   for ( const US_DataPubImporter::Result& result : second.results() )
      if ( result.type == US_DataPub::Buffer )
      {
         EXPECT_EQ( result.resolution, US_DataPub::ResolvedReused );
         EXPECT_EQ( result.targetGUID, bufferGUID );
      }
}

// ------------------------------------- the minors of round six, on a server

class DataPubDbFields : public DataPubDbRules
{
   protected:
      // The first column of the first row of a statement, as text
      QString sqlValue( const QString& sql )
      {
         db->rawQuery( sql );

         return db->next() ? db->value( 0 ).toString() : QString();
      }

      // The store again, every record under a new GUID
      void renewAll()
      {
         QDir( US_Settings::dataDir()   ).removeRecursively();
         QDir( US_Settings::resultDir() ).removeRecursively();
         QDir().mkpath( US_Settings::dataDir()   );
         QDir().mkpath( US_Settings::resultDir() );

         projectGUID  = US_Util::new_guid();
         bufferGUID   = US_Util::new_guid();
         analyteGUID  = US_Util::new_guid();
         solutionGUID = US_Util::new_guid();
         expGUID      = US_Util::new_guid();
         rawGUID      = US_Util::new_guid();
         editGUID     = US_Util::new_guid();
         modelGUID    = US_Util::new_guid();
         noiseGUID    = US_Util::new_guid();
         calGUID      = US_Util::new_guid();

         buildStore();
      }

      static void setAttr( US_DataPubManifest& m, US_DataPub::EntityType type,
                           const QString& key, const QString& value )
      {
         QList< US_DataPubEntity > list = m.section( type );
         ASSERT_FALSE( list.isEmpty() );

         for ( int ii = 0; ii < list.size(); ii++ )
            list[ ii ].attrs.insert( key, value );

         m.setSection( type, list );
      }
};

/* What the manifest carries of the source's rows beyond their payloads is
   stored as the source had it: a buffer's private flag, an edit's comment,
   the raw data's label and comment, an analyte's spectrum column, and a
   model's variance and meniscus with all their digits. */
TEST_F( DataPubDbFields, TheSourcesFieldsAreStored )
{
   renewAll();

   const double variance = 1.0 / 3.0e5;
   const double meniscus = 5.8712345678901;

   QString bundle = recraft( exportStore( "fields" ),
         [ & ]( US_DataPubManifest& m, const QString& )
   {
      setAttr( m, US_DataPub::Buffer,     "private",  "0" );
      setAttr( m, US_DataPub::EditedData, "comment",  "edited by hand" );
      setAttr( m, US_DataPub::RawData,    "label",    "raw label" );
      setAttr( m, US_DataPub::RawData,    "comment",  "raw comment" );
      setAttr( m, US_DataPub::Analyte,    "spectrum", "260/1.5" );
      setAttr( m, US_DataPub::Model,      "variance",
               QString::number( variance, 'g', 17 ) );
      setAttr( m, US_DataPub::Model,      "meniscus",
               QString::number( meniscus, 'g', 17 ) );
   } );

   US_DataPubImporter importer;
   QString            error;
   ASSERT_TRUE( importer.inspect( bundle, error ) ) << error.toStdString();
   ASSERT_TRUE( importer.runImport( options(), error ) )
      << error.toStdString() << "\n" << importer.log().join( "\n" ).toStdString();

   EXPECT_EQ( sqlValue( "SELECT bp.private FROM buffer b, bufferPerson bp"
                        " WHERE b.bufferID = bp.bufferID AND b.bufferGUID = '"
                        + bufferGUID + "'" ), QString( "0" ) );
   EXPECT_EQ( sqlValue( "SELECT comment FROM editedData WHERE editGUID = '"
                        + editGUID + "'" ), QString( "edited by hand" ) );
   EXPECT_EQ( sqlValue( "SELECT label FROM rawData WHERE rawDataGUID = '"
                        + rawGUID + "'" ), QString( "raw label" ) );
   EXPECT_EQ( sqlValue( "SELECT comment FROM rawData WHERE rawDataGUID = '"
                        + rawGUID + "'" ), QString( "raw comment" ) );
   EXPECT_EQ( sqlValue( "SELECT spectrum FROM analyte WHERE analyteGUID = '"
                        + analyteGUID + "'" ), QString( "260/1.5" ) );

   db->rawQuery( "SELECT variance, meniscus FROM model WHERE modelGUID = '"
                 + modelGUID + "'" );
   ASSERT_TRUE( db->next() );
   EXPECT_EQ( db->value( 0 ).toDouble(), variance )
      << db->value( 0 ).toString().toStdString();
   EXPECT_EQ( db->value( 1 ).toDouble(), meniscus )
      << db->value( 1 ).toString().toStdString();
}

/* A public buffer stays public, so another investigator's import of the
   same chemistry uses it instead of copying it. */
TEST_F( DataPubDbFields, APublicBufferIsUsedByAnotherInvestigator )
{
   QString otherGUID = qEnvironmentVariable( "US3_TEST_DB_OTHER_GUID" );
   QString otherPW   = qEnvironmentVariable( "US3_TEST_DB_OTHER_PW" );
   int     otherID   = qEnvironmentVariable( "US3_TEST_DB_OTHER_ID" ).toInt();

   if ( otherGUID.isEmpty()  ||  otherID < 1 )
      GTEST_SKIP() << "no second investigator configured";

   renewAll();

   auto makePublic = [ & ]( US_DataPubManifest& m, const QString& )
   { setAttr( m, US_DataPub::Buffer, "private", "0" ); };

   QString            error;
   QString            first = recraft( exportStore( "public1" ), makePublic );
   US_DataPubImporter mine;
   ASSERT_TRUE( mine.inspect( first, error ) ) << error.toStdString();
   ASSERT_TRUE( mine.runImport( options(), error ) )
      << error.toStdString() << "\n" << mine.log().join( "\n" ).toStdString();

   rebuildKeepingChemistry();
   QString second = recraft( exportStore( "public2" ), makePublic );
   ASSERT_TRUE( loginAs( otherGUID, otherPW, otherID ) );

   US_DataPubImporter theirs;
   ASSERT_TRUE( theirs.inspect( second, error ) ) << error.toStdString();
   ASSERT_TRUE( theirs.runImport( options(), error ) )
      << error.toStdString() << "\n" << theirs.log().join( "\n" ).toStdString();

   const US_DataPubImporter::Result* buf =
         resultOf( theirs.results(), US_DataPub::Buffer );
   ASSERT_NE( buf, nullptr );
   EXPECT_EQ( buf->resolution, US_DataPub::ResolvedReused );
   EXPECT_EQ( buf->targetGUID, bufferGUID );
}

/* Experiment 1 is the ID verify_experiment_permission lets anyone use: the
   raw data of whoever's first run became experiment 1 is found for every
   investigator.  Another investigator importing that run is told it is
   someone else's, not that it is their own under another GUID.  It needs
   experiment 1 to have been made by an import of this suite (the first
   one into a fresh target does), and skips otherwise. */
TEST_F( DataPubDbFields, TheRunOfExperimentOneIsAnotherInvestigators )
{
   QString otherGUID = qEnvironmentVariable( "US3_TEST_DB_OTHER_GUID" );
   QString otherPW   = qEnvironmentVariable( "US3_TEST_DB_OTHER_PW" );
   int     otherID   = qEnvironmentVariable( "US3_TEST_DB_OTHER_ID" ).toInt();
   int     selfID    = qEnvironmentVariable( "US3_TEST_DB_PERSON_ID" ).toInt();

   if ( otherGUID.isEmpty()  ||  otherID < 1 )
      GTEST_SKIP() << "no second investigator configured";

   QString error;
   QString one = "SELECT experimentGUID FROM experiment WHERE experimentID = 1";

   if ( sqlValue( one ).isEmpty() )
   {
      if ( sqlValue( "SELECT COUNT(*) FROM experiment" ) != "0" )
         GTEST_SKIP() << "the target has no experiment 1 and is not fresh";

      US_DataPubImporter first;
      QString            bundle = exportStore( "first_run" );
      ASSERT_TRUE( first.inspect( bundle, error ) ) << error.toStdString();
      ASSERT_TRUE( first.runImport( options(), error ) )
         << error.toStdString() << "\n" << first.log().join( "\n" ).toStdString();
   }

   QString guid  = sqlValue( one );
   QString run   = sqlValue( "SELECT runID FROM experiment"
                             " WHERE experimentID = 1" );
   QString raw   = sqlValue( "SELECT rawDataGUID FROM rawData"
                             " WHERE experimentID = 1 LIMIT 1" );
   int     owner = sqlValue( "SELECT personID FROM experimentPerson"
                             " WHERE experimentID = 1 LIMIT 1" ).toInt();

   if ( guid.isEmpty()  ||  raw.isEmpty()  ||
        ! US_DataPubImporter::runIdIsUsable( run ) )
      GTEST_SKIP() << "experiment 1 has no raw data this test can stand for";

   // That run as a bundle: the store again with its GUIDs and run ID
   renewAll();
   QDir( US_Settings::dataDir()   ).removeRecursively();
   QDir( US_Settings::resultDir() ).removeRecursively();
   QDir().mkpath( US_Settings::dataDir()   );
   QDir().mkpath( US_Settings::resultDir() );
   expGUID = guid;
   rawGUID = raw;
   runID   = run;
   buildStore();
   QString bundle = exportStore( "experiment_one" );

   // ... imported by an investigator who does not own it
   if ( owner == otherID )
      ASSERT_TRUE( loginAs( qEnvironmentVariable( "US3_TEST_DB_PERSON_GUID" ),
                            qEnvironmentVariable( "US3_TEST_DB_PERSON_PW" ),
                            selfID ) );
   else
      ASSERT_TRUE( loginAs( otherGUID, otherPW, otherID ) );

   US_DataPubImporter importer;
   ASSERT_TRUE ( importer.inspect( bundle, error ) ) << error.toStdString();
   EXPECT_FALSE( importer.runImport( options(), error ) );
   EXPECT_TRUE ( error.contains( "another investigator's run" ) )
      << error.toStdString();
   EXPECT_FALSE( error.contains( "same investigator" ) ) << error.toStdString();
   EXPECT_EQ   ( importer.outcome(), QString( "Nothing was written" ) );
}

/* A target whose component table has a gap: the procedure servers have run
   for years gives a new component the row count plus one, which is taken.
   The dry run says so unless the server says its procedure counts nothing
   (us3_catalog_version() answers 1 or more); the import then either fails
   on it and is
   rolled back (that procedure), or goes through with the buffer linked to
   the new row (the updated one). */
TEST_F( DataPubDbFields, AComponentIdTheTableHasIsForeseen )
{
   int  count  = sqlValue( "SELECT COUNT(*) FROM bufferComponent" ).toInt();
   int  top    = sqlValue( "SELECT COALESCE( MAX( bufferComponentID ), 0 )"
                           " FROM bufferComponent" ).toInt();
   bool marker = sqlValue( "SELECT us3_catalog_version()" ).toInt() >= 1;
   int  gapID  = 0;

   if ( top == count )
   {  // Make one: a row two past the last, so the count plus one is taken
      gapID = top + 2;
      db->rawQuery( QString( "INSERT INTO bufferComponent ( bufferComponentID,"
                             " description, units, viscosity, density, c_range,"
                             " gradientForming ) VALUES ( %1, 'Gap %1', 'mM',"
                             " '0 0 0 0 0 0', '0 0 0 0 0 0', '', 0 )" )
                    .arg( gapID ) );
      ASSERT_EQ( db->lastErrno(), IUS_DB2::OK ) << db->lastError().toStdString();
   }

   QMap< QString, US_BufferComponent > table;
   US_BufferComponent::getAllFromHD( table );
   ASSERT_TRUE( table.contains( "1" ) ) << "no local component table";

   renewAll();
   US_Buffer buffer;
   QString   bpath = US_Settings::dataDir() + "/buffers/B0000001.xml";
   ASSERT_TRUE( buffer.readFromDisk( bpath ) );
   buffer.component     << table.value( "1" );
   buffer.componentIDs  << "1";
   buffer.concentration << 150.0;
   ASSERT_TRUE( buffer.writeToDisk( bpath ) );

   QString unique = "Gapx " + US_Util::new_guid().left( 8 );
   QString bundle = recraft( exportStore( "gap" ),
         [ & ]( US_DataPubManifest& m, const QString& stage )
   {
      QList< US_DataPubEntity > bufs = m.section( US_DataPub::Buffer );
      ASSERT_EQ( bufs.size(), 1 );
      QString cpath = stage + "/" + bufs[ 0 ].attrs.value( "componentsPayload" );
      QFile   file( cpath );
      ASSERT_TRUE( file.open( QIODevice::ReadOnly ) );
      QString text = QString::fromUtf8( file.readAll() );
      file.close();
      text.replace( "name=\"" + table.value( "1" ).name + "\"",
                    "name=\"" + unique + "\"" );
      ASSERT_TRUE( file.open( QIODevice::WriteOnly | QIODevice::Truncate ) );
      file.write( text.toUtf8() );
      file.close();
      bufs[ 0 ].attrs.insert( US_DataPub::digestKeyOf( "componentsPayload" ),
                              US_DataPubHash::fileHash( cpath ) );
      m.setSection( US_DataPub::Buffer, bufs );
   } );

   QString            error;
   US_DataPubImporter dry;
   ASSERT_TRUE( dry.inspect( bundle, error ) ) << error.toStdString();
   ASSERT_TRUE( dry.runImport( options( true ), error ) ) << error.toStdString();
   EXPECT_EQ  ( dry.warnings().join( "\n" ).contains( "row count" ), ! marker )
      << dry.warnings().join( "\n" ).toStdString();

   US_DataPubImporter importer;
   ASSERT_TRUE( importer.inspect( bundle, error ) ) << error.toStdString();
   bool done = importer.runImport( options(), error );

   if ( done )
   {
      const US_DataPubImporter::Result* made =
            resultOf( importer.results(), US_DataPub::Buffer );
      ASSERT_NE( made, nullptr );
      US_Buffer stored;
      ASSERT_TRUE( stored.readFromDB( db, made->targetID ) );
      ASSERT_EQ  ( stored.component.size(), 1 );
      EXPECT_EQ  ( stored.component[ 0 ].name, unique );
   }

   else
   {
      EXPECT_FALSE( marker ) << error.toStdString();
      EXPECT_TRUE ( error.contains( "could not be added" ) ) << error.toStdString();
      EXPECT_TRUE ( importer.outcome().startsWith( "Nothing was imported" ) )
         << importer.outcome().toStdString();
      EXPECT_EQ   ( sqlValue( "SELECT COUNT(*) FROM bufferComponent"
                              " WHERE description = '" + unique + "'" ),
                    QString( "0" ) );
   }

   if ( gapID > 0 )
      db->rawQuery( QString( "DELETE FROM bufferComponent"
                             " WHERE bufferComponentID = %1" ).arg( gapID ) );
}

/* DB -> bundle -> disk -> bundle: the fixture's run, imported into a disk
   store, comes out of it again with its rotor calibration -- which a run
   naming calibration -1 lost. */
TEST_F( DataPubDbImport, ARunKeepsItsCalibrationThroughADiskStore )
{
   QString fixture = qEnvironmentVariable( "US3_TEST_DB_NAME" );

   if ( fixture.isEmpty() )
      GTEST_SKIP() << "no fixture database configured";

   QStringList target = US_Settings::defaultDB();
   QStringList source = target;
   source.replace( 2, fixture );
   US_Settings::set_defaultDB( source );

   US_DataPubExporter            exporter;
   US_DataPubExporter::Selection selection;
   selection.fromDb     = true;
   selection.dbPassword = master;
   selection.scope      = US_DataPub::ScopeSolutions;
   selection.runIDs << "MWL_demo_run";

   QString bundle = root + "/fixture_run.tar.gz";
   QString error;
   bool    made   = exporter.exportBundle( selection, bundle, error );

   US_Settings::set_defaultDB( target );

   if ( ! made  &&  error.contains( "MWL_demo_run" ) )
      GTEST_SKIP() << "not the fixture database: " << error.toStdString();

   ASSERT_TRUE( made ) << error.toStdString();

   QList< US_DataPubEntity > cals =
         exporter.manifest().section( US_DataPub::RotorCalibration );
   ASSERT_EQ( cals.size(), 1 ) << "the fixture run has no calibration";

   QString store = root + "/disk_target";
   QDir().mkpath( store );

   US_DataPubImporter::Options options;
   options.target       = US_DataPub::TargetDisk;
   options.outputDir    = store;
   options.verifyHashes = true;
   options.dryRun       = false;
   options.policy       = US_DataPub::PolicyReuse;

   US_DataPubImporter importer;
   ASSERT_TRUE( importer.inspect( bundle, error ) ) << error.toStdString();
   ASSERT_TRUE( importer.runImport( options, error ) )
      << error.toStdString() << "\n" << importer.log().join( "\n" ).toStdString();

   US_Settings::set_workBaseDir( store );

   US_DataPubExporter            again;
   US_DataPubExporter::Selection fromDisk;
   fromDisk.fromDb = false;
   fromDisk.scope  = US_DataPub::ScopeSolutions;
   fromDisk.runIDs << "MWL_demo_run";
   QString second = root + "/from_disk.tar.gz";
   ASSERT_TRUE( again.exportBundle( fromDisk, second, error ) )
      << error.toStdString();

   QList< US_DataPubEntity > back =
         again.manifest().section( US_DataPub::RotorCalibration );
   ASSERT_EQ( back.size(), 1 );
   EXPECT_EQ( back[ 0 ].guid, cals[ 0 ].guid );
}
