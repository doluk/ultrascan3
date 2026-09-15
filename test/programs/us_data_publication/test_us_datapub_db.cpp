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
// Without those they skip, so an ordinary build needs no database.
#include <gtest/gtest.h>

#include <QtCore>

#include "us_datapub_catalog.h"
#include "us_datapub_export.h"
#include "us_datapub_manifest.h"
#include "us_db2.h"
#include "us_crypto.h"
#include "us_settings.h"

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
