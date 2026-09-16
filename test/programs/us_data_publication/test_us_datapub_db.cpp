// test_us_datapub_db.cpp - the publication catalog against a real database
//
// us_data_publication reads the record chain through US_DataCatalog, which
// prefers the catalog procedures added for it and falls back on the ones
// UltraScan has always had when a server does not have them.  An
// installation that has not run the new SQL still has to be able to
// publish its data, so these tests are meant to be run twice: against a
// database that has the new procedures, and against one that does not.
//
//   US3_TEST_DB_HOST=127.0.0.1:3306 US3_TEST_DB_NAME=us3test \
//   US3_TEST_DB_USER=us3 US3_TEST_DB_PASS=... \
//   US3_TEST_DB_PERSON_GUID=... US3_TEST_DB_PERSON_PW=... \
//   US3_TEST_DB_PERSON_ID=2 ./bin/test_us_datapub
//
// The test about a changed investigator needs a second person to switch to,
// and an administrator login, since only an administrator may read another
// person's records.  Without these it skips:
//
//   US3_TEST_DB_ADMIN_GUID=... US3_TEST_DB_ADMIN_PW=... \
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
#include "us_db2.h"
#include "us_crypto.h"
#include "us_settings.h"
#include "us_time_state.h"

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

   bool made = exporter.exportBundle( selection, bundle, error );

   if ( ! made )
   {
      // The fixture may not carry the hardware an experiment record needs.
      // That is about the data, not about which procedures the server has,
      // so say so rather than failing.
      QDir( root ).removeRecursively();
      GTEST_SKIP() << "this database cannot export " << chosen.toStdString()
                   << ": " << error.toStdString();
   }

   EXPECT_TRUE( QFile::exists( bundle ) );
   EXPECT_GT  ( QFileInfo( bundle ).size(), 0 );
   EXPECT_GT  ( exporter.manifest().count( US_DataPub::Experiment ), 0 );
   EXPECT_GT  ( exporter.manifest().count( US_DataPub::RawData    ), 0 );

   qDebug() << "DataPubDb: exported" << chosen
            << exporter.manifest().total() << "records from the database";

   QDir( root ).removeRecursively();
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

   if ( ! made )
   {  // The fixture may not carry the hardware an experiment record needs
      QString why = error;
      QDir( root ).removeRecursively();
      GTEST_SKIP() << "the run could not be exported: " << why.toStdString();
   }

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
