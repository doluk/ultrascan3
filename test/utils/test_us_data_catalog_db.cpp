// test_us_data_catalog_db.cpp - US_DataCatalog against a real database
//
// These tests need a MySQL/MariaDB server with the US3 schema and stored
// procedures loaded.  Point them at one with:
//
//   US3_TEST_DB_HOST=localhost US3_TEST_DB_NAME=us3test \
//   US3_TEST_DB_USER=us3test US3_TEST_DB_PASS=... \
//   US3_TEST_DB_PERSON_GUID=... US3_TEST_DB_PERSON_PW=... \
//   US3_TEST_DB_PERSON_ID=2 ./test_us_utils
//
// Without those, every test here skips, so an ordinary build needs no
// database.
#include "qt_test_base.h"

#include <QtCore>

#include "us_data_catalog.h"
#include "us_db2.h"
#include "us_crypto.h"
#include "us_settings.h"

class TestUSDataCatalogDb : public QtTestBase
{
   protected:
      US_DB2*     db       = nullptr;
      int         personID = 0;
      int         savedInv = 0;
      QStringList savedDB;
      bool        restoreDB = false;

      void SetUp() override
      {
         QtTestBase::SetUp();

         QString host  = qEnvironmentVariable( "US3_TEST_DB_HOST" );
         QString name  = qEnvironmentVariable( "US3_TEST_DB_NAME" );
         QString user  = qEnvironmentVariable( "US3_TEST_DB_USER" );
         QString pass  = qEnvironmentVariable( "US3_TEST_DB_PASS" );
         QString guid  = qEnvironmentVariable( "US3_TEST_DB_PERSON_GUID" );
         QString pw    = qEnvironmentVariable( "US3_TEST_DB_PERSON_PW" );
         QString email = qEnvironmentVariable( "US3_TEST_DB_EMAIL" );

         if ( host.isEmpty()  ||  name.isEmpty()  ||
              user.isEmpty()  ||  guid.isEmpty() )
            GTEST_SKIP() << "no test database configured";

         // US_DB2 takes the investigator GUID and password from the stored
         // database definition, so that is how a test supplies them too.
         const QString master = "us3-data-catalog-test";

         QStringList dbCipher   = US_Crypto::encrypt( pass, master );
         QStringList userCipher = US_Crypto::encrypt( pw,   master );

         QStringList entry;
         entry << "us3 test database"   // 0 description
               << user                  // 1 database user
               << name                  // 2 database name
               << host                  // 3 host[:port]
               << dbCipher  .at( 0 )    // 4 database password, encrypted
               << dbCipher  .at( 1 )    // 5 initialization vector
               << email                 // 6 investigator email
               << userCipher.at( 0 )    // 7 investigator password, encrypted
               << userCipher.at( 1 )    // 8 initialization vector
               << guid;                 // 9 investigator GUID

         savedDB   = US_Settings::defaultDB();
         restoreDB = true;
         US_Settings::set_defaultDB( entry );

         db = new US_DB2();
         QString error;

         if ( ! db->connect( master, error ) )
         {
            delete db;
            db = nullptr;
            US_Settings::set_defaultDB( savedDB );
            restoreDB = false;
            GTEST_SKIP() << "cannot reach the test database: "
                         << error.toStdString();
         }

         personID = qEnvironmentVariable( "US3_TEST_DB_PERSON_ID" ).toInt();
         savedInv = US_Settings::us_inv_ID();
         US_Settings::set_us_inv_ID( personID );
      }

      void TearDown() override
      {
         if ( db != nullptr )
         {
            US_Settings::set_us_inv_ID( savedInv );
            delete db;
            db = nullptr;
         }

         if ( restoreDB )
         {
            US_Settings::set_defaultDB( savedDB );
            restoreDB = false;
         }

         QtTestBase::TearDown();
      }
};

TEST_F( TestUSDataCatalogDb, FirstLayerListsExperimentsWithCounts )
{
   US_DataCatalog catalog;
   QString        error;

   ASSERT_TRUE( catalog.attach( db, error ) ) << error.toStdString();
   ASSERT_TRUE( catalog.loadRuns( error ) )   << error.toStdString();

   ASSERT_GT( catalog.runCount(), 0 );

   int found = -1;

   for ( int ii = 0; ii < catalog.runCount(); ii++ )
      if ( catalog.run( ii ).rawCount > 0 )  found = ii;

   ASSERT_GE( found, 0 ) << "expected at least one experiment with raw data";

   const US_DataCatalog::Run& run = catalog.run( found );

   EXPECT_FALSE( run.id   .isEmpty() );
   EXPECT_FALSE( run.guid .isEmpty() );
   EXPECT_FALSE( run.runID.isEmpty() );
   EXPECT_GE   ( run.editCount,  0 );
   EXPECT_GE   ( run.modelCount, 0 );
   EXPECT_GE   ( run.noiseCount, 0 );
   EXPECT_FALSE( run.isLoaded() );
}

TEST_F( TestUSDataCatalogDb, SecondLayerMatchesTheCounts )
{
   US_DataCatalog catalog;
   QString        error;

   ASSERT_TRUE( catalog.attach( db, error ) );
   ASSERT_TRUE( catalog.loadRuns( error ) );

   int found = -1;

   for ( int ii = 0; ii < catalog.runCount(); ii++ )
      if ( catalog.run( ii ).rawCount > 0 )  found = ii;

   ASSERT_GE( found, 0 );

   int rawCount   = catalog.run( found ).rawCount;
   int editCount  = catalog.run( found ).editCount;
   int modelCount = catalog.run( found ).modelCount;

   ASSERT_TRUE( catalog.loadRunDetail( found, error ) ) << error.toStdString();

   const US_DataCatalog::Run& run = catalog.run( found );
   ASSERT_TRUE( run.isLoaded() );

   // what the summary promised is what the detail delivers
   EXPECT_EQ( run.raws.size(), rawCount );

   int edits  = 0;
   int models = 0;

   for ( int ii = 0; ii < run.raws.size(); ii++ )
   {
      EXPECT_FALSE( run.raws[ ii ].guid    .isEmpty() );
      EXPECT_FALSE( run.raws[ ii ].filename.isEmpty() );
      EXPECT_FALSE( run.raws[ ii ].checksum.isEmpty() );

      edits += run.raws[ ii ].edits.size();

      for ( int jj = 0; jj < run.raws[ ii ].edits.size(); jj++ )
      {
         const US_DataCatalog::Edit& edit = run.raws[ ii ].edits[ jj ];

         EXPECT_FALSE( edit.guid.isEmpty() );
         EXPECT_EQ   ( edit.rawGUID, run.raws[ ii ].guid );

         models += edit.models.size();

         for ( int kk = 0; kk < edit.models.size(); kk++ )
         {
            EXPECT_EQ( edit.models[ kk ].editGUID, edit.guid );

            for ( int mm = 0; mm < edit.models[ kk ].noises.size(); mm++ )
               EXPECT_EQ( edit.models[ kk ].noises[ mm ].modelGUID,
                          edit.models[ kk ].guid );
         }
      }
   }

   EXPECT_EQ( edits,  editCount );
   EXPECT_EQ( models, modelCount );
}

TEST_F( TestUSDataCatalogDb, PendingQueueWorksThroughEveryExperiment )
{
   US_DataCatalog catalog;
   QString        error;

   ASSERT_TRUE( catalog.attach( db, error ) );
   ASSERT_TRUE( catalog.loadRuns( error ) );

   int total = catalog.runCount();
   int index = -1;
   int knt   = 0;

   while ( catalog.loadNextPending( index, error ) )  knt++;

   EXPECT_TRUE( error.isEmpty() ) << error.toStdString();
   EXPECT_EQ  ( knt, total );
   EXPECT_EQ  ( catalog.pendingCount(), 0 );
}

// The per-experiment catalog procedures are an optimization, and a server
// that does not have them still has to give the same answer, so the two
// paths are compared against each other.
TEST_F( TestUSDataCatalogDb, BulkAndLegacyPathsAgree )
{
   US_DataCatalog bulk;
   US_DataCatalog legacy;
   QString        error;

   ASSERT_TRUE( bulk  .attach( db, error ) ) << error.toStdString();
   ASSERT_TRUE( legacy.attach( db, error ) ) << error.toStdString();

   legacy.setBulkQueries( false );
   ASSERT_TRUE( bulk  .bulkQueries() );
   ASSERT_FALSE( legacy.bulkQueries() );

   ASSERT_TRUE( bulk  .loadRuns( error ) ) << error.toStdString();
   ASSERT_TRUE( legacy.loadRuns( error ) ) << error.toStdString();

   ASSERT_EQ( bulk.runCount(), legacy.runCount() );
   ASSERT_GT( bulk.runCount(), 0 );

   for ( int ii = 0; ii < bulk.runCount(); ii++ )
   {
      const US_DataCatalog::Run& bb = bulk  .run( ii );
      const US_DataCatalog::Run& ll = legacy.run( ii );

      ASSERT_EQ( bb.runID.toStdString(), ll.runID.toStdString() );
      EXPECT_EQ( bb.guid       .toStdString(), ll.guid       .toStdString() );
      EXPECT_EQ( bb.id         .toStdString(), ll.id         .toStdString() );
      EXPECT_EQ( bb.projectGUID.toStdString(), ll.projectGUID.toStdString() );

      // The first layer of the legacy path cannot count the chain without
      // walking it, and says so with -1; where it does answer, it agrees.
      if ( ll.rawCount   >= 0 )  EXPECT_EQ( bb.rawCount,   ll.rawCount   );
      if ( ll.editCount  >= 0 )  EXPECT_EQ( bb.editCount,  ll.editCount  );
      if ( ll.modelCount >= 0 )  EXPECT_EQ( bb.modelCount, ll.modelCount );
      if ( ll.noiseCount >= 0 )  EXPECT_EQ( bb.noiseCount, ll.noiseCount );

      ASSERT_TRUE( bulk  .loadRunDetail( ii, error ) ) << error.toStdString();
      ASSERT_TRUE( legacy.loadRunDetail( ii, error ) ) << error.toStdString();

      const US_DataCatalog::Run& bd = bulk  .run( ii );
      const US_DataCatalog::Run& ld = legacy.run( ii );

      // Loaded, both paths know exactly what is there
      EXPECT_EQ( bd.rawCount,   ld.rawCount   );
      EXPECT_EQ( bd.editCount,  ld.editCount  );
      EXPECT_EQ( bd.modelCount, ld.modelCount );
      EXPECT_EQ( bd.noiseCount, ld.noiseCount );

      ASSERT_EQ( bd.raws.size(), ld.raws.size() ) << bb.runID.toStdString();

      for ( int jj = 0; jj < bd.raws.size(); jj++ )
      {
         const US_DataCatalog::Raw& br = bd.raws[ jj ];
         const US_DataCatalog::Raw& lr = ld.raws[ jj ];

         EXPECT_EQ( br.guid    .toStdString(), lr.guid    .toStdString() );
         EXPECT_EQ( br.filename.toStdString(), lr.filename.toStdString() );
         EXPECT_EQ( br.checksum.toStdString(), lr.checksum.toStdString() );
         ASSERT_EQ( br.edits.size(), lr.edits.size() );

         for ( int kk = 0; kk < br.edits.size(); kk++ )
         {
            const US_DataCatalog::Edit& be = br.edits[ kk ];
            const US_DataCatalog::Edit& le = lr.edits[ kk ];

            EXPECT_EQ( be.guid    .toStdString(), le.guid    .toStdString() );
            EXPECT_EQ( be.filename.toStdString(), le.filename.toStdString() );
            ASSERT_EQ( be.models.size(), le.models.size() );

            for ( int mm = 0; mm < be.models.size(); mm++ )
            {
               const US_DataCatalog::Model& bm = be.models[ mm ];
               const US_DataCatalog::Model& lm = le.models[ mm ];

               EXPECT_EQ( bm.guid.toStdString(), lm.guid.toStdString() );
               EXPECT_EQ( bm.description.toStdString(),
                          lm.description.toStdString() );
               ASSERT_EQ( bm.noises.size(), lm.noises.size() );

               for ( int nn = 0; nn < bm.noises.size(); nn++ )
                  EXPECT_EQ( bm.noises[ nn ].guid.toStdString(),
                             lm.noises[ nn ].guid.toStdString() );
            }
         }
      }
   }
}
