// test/programs/us_data_publication/test_us_datapub_export_db.cpp
//
// Exports from a real database, through the scratch import database (see
// test/README, section 4): the synthetic disk store is imported into it,
// and what an export of it then gives is checked -- also after a trip
// through a disk store.  They skip without US3_TEST_DB_IMPORT_NAME.

#include <gtest/gtest.h>

#include <QtCore>

#include "datapub_test_env.h"

#include "us_datapub_export.h"
#include "us_datapub_import.h"
#include "us_datapub_manifest.h"
#include "us_datapub_catalog.h"

class DataPubDbExport : public DataPubDbTargetEnv
{
   protected:
      US_DataPubImporter::Options target( bool toDb, const QString& dir )
      {
         US_DataPubImporter::Options options;
         options.target       = toDb ? US_DataPub::TargetDb
                                     : US_DataPub::TargetDisk;
         options.outputDir    = dir;
         options.dbPassword   = master;
         options.verifyHashes = true;
         options.dryRun       = false;
         options.policy       = toDb ? US_DataPub::PolicyRename
                                     : US_DataPub::PolicyReuse;
         return options;
      }

      /* Export the disk store's run and import it into the scratch
         database; the run ID it has there (a run of that ID may be there
         from an earlier test, and the import then renames this one)
      */
      bool storeInDatabase( QString& dbRunID, QString& error )
      {
         US_DataPubExporter            exporter;
         US_DataPubExporter::Selection selection;
         selection.fromDb = false;
         selection.scope  = US_DataPub::ScopeNoise;
         selection.runIDs << runID;

         QString bundle = root + "/to_db.tar.gz";

         if ( ! exporter.exportBundle( selection, bundle, error ) )
            return false;

         US_DataPubImporter importer;

         if ( ! importer.inspect( bundle, error )  ||
              ! importer.runImport( target( true, QString() ), error ) )
         {
            error += "\n" + importer.log().join( "\n" );
            return false;
         }

         for ( const US_DataPubImporter::Result& result : importer.results() )
            if ( result.type == US_DataPub::Experiment )
               dbRunID = result.targetName;

         if ( dbRunID.isEmpty() )
         {
            error = "the run was not imported";
            return false;
         }

         return true;
      }

      // Export a run of the scratch database
      bool exportFromDb( const QString& dbRunID, const QString& bundle,
                         US_DataPubExporter& exporter, QString& error )
      {
         US_DataPubExporter::Selection selection;
         selection.fromDb     = true;
         selection.dbPassword = master;
         selection.scope      = US_DataPub::ScopeNoise;
         selection.runIDs << dbRunID;

         return exporter.exportBundle( selection, bundle, error );
      }

      // Import a bundle into a disk store and export the run from there
      bool throughDisk( const QString& bundle, const QString& dbRunID,
                        US_DataPubExporter& exporter, QString& error )
      {
         QString            store = root + "/disk_target";
         US_DataPubImporter importer;

         if ( ! importer.inspect( bundle, error )  ||
              ! importer.runImport( target( false, store ), error ) )
            return false;

         US_Settings::set_workBaseDir( store );

         US_DataPubExporter::Selection selection;
         selection.fromDb = false;
         selection.scope  = US_DataPub::ScopeNoise;
         selection.runIDs << dbRunID;

         return exporter.exportBundle( selection, root + "/from_disk.tar.gz",
                                       error );
      }
};

/* The rotor calibration of a database run survives a disk store: database
   -> bundle -> disk -> bundle.  The disk store gives the calibration no ID
   of its own (-1), and the run came out of it without its calibration.
*/
TEST_F( DataPubDbExport, ACalibrationSurvivesADiskStore )
{
   QString dbRunID;
   QString error;
   ASSERT_TRUE( storeInDatabase( dbRunID, error ) ) << error.toStdString();

   US_DataPubExporter fromDb;
   QString            bundle = root + "/from_db.tar.gz";
   ASSERT_TRUE( exportFromDb( dbRunID, bundle, fromDb, error ) )
      << error.toStdString() << "\n" << fromDb.log().join( "\n" ).toStdString();

   QList< US_DataPubEntity > cals =
      fromDb.manifest().section( US_DataPub::RotorCalibration );
   ASSERT_EQ( cals.size(), 1 ) << fromDb.log().join( "\n" ).toStdString();

   US_DataPubExporter fromDisk;
   ASSERT_TRUE( throughDisk( bundle, dbRunID, fromDisk, error ) )
      << error.toStdString() << "\n"
      << fromDisk.log().join( "\n" ).toStdString();

   EXPECT_TRUE( fromDisk.manifest().contains( US_DataPub::RotorCalibration,
                                              cals[ 0 ].guid ) )
      << fromDisk.warnings().join( "\n" ).toStdString();
   EXPECT_FALSE( fromDisk.warnings().join( "\n" ).contains( "calibration" ) )
      << fromDisk.warnings().join( "\n" ).toStdString();

   US_DataPubEntity calibration;
   ASSERT_TRUE( fromDisk.manifest().entity( US_DataPub::RotorCalibration,
                                            cals[ 0 ].guid, calibration ) );
   EXPECT_EQ( calibration.attrs.value( "coeff1" ),
              cals[ 0 ].attrs.value( "coeff1" ) );
   EXPECT_EQ( calibration.attrs.value( "coeff2" ),
              cals[ 0 ].attrs.value( "coeff2" ) );
}

/* A model's variance and meniscus, as its database row holds them, go
   into the bundle with every digit -- in the manifest and in the payload --
   and so into a disk store the bundle is imported into, which has only the
   payload.  The payload used to hold six digits, or nothing when the stored
   XML lacked them.
*/
TEST_F( DataPubDbExport, AModelRowTravelsWithEveryDigit )
{
   QString dbRunID;
   QString error;
   ASSERT_TRUE( storeInDatabase( dbRunID, error ) ) << error.toStdString();

   const double variance = 1.2345678901234e-05;
   const double meniscus = 5.8512345678901;

   QStringList query;
   query << "get_modelID" << modelGUID;
   db->query( query );
   ASSERT_TRUE( db->next() ) << db->lastError().toStdString();
   QString modelID = db->value( 0 ).toString();

   // The row holds what the stored XML does not
   db->rawQuery( QString( "UPDATE model SET variance = %1, meniscus = %2"
                          " WHERE modelID = %3" )
                 .arg( variance, 0, 'g', 17 ).arg( meniscus, 0, 'g', 17 )
                 .arg( modelID ) );
   ASSERT_EQ( db->lastErrno(), IUS_DB2::OK ) << db->lastError().toStdString();

   US_DataPubExporter fromDb;
   QString            bundle = root + "/from_db.tar.gz";
   ASSERT_TRUE( exportFromDb( dbRunID, bundle, fromDb, error ) )
      << error.toStdString();

   US_DataPubEntity entity;
   ASSERT_TRUE( fromDb.manifest().entity( US_DataPub::Model, modelGUID,
                                          entity ) );
   EXPECT_EQ( entity.attrs.value( "variance" ).toDouble(), variance );
   EXPECT_EQ( entity.attrs.value( "meniscus" ).toDouble(), meniscus );

   US_DataPubImporter inspector;
   ASSERT_TRUE( inspector.inspect( bundle, error ) ) << error.toStdString();

   US_Model payload;
   ASSERT_EQ( payload.load( inspector.bundleRoot() + "/" + entity.payload ),
              IUS_DB2::OK );
   EXPECT_EQ( payload.variance, variance );
   EXPECT_EQ( payload.meniscus, meniscus );

   US_DataPubExporter fromDisk;
   ASSERT_TRUE( throughDisk( bundle, dbRunID, fromDisk, error ) )
      << error.toStdString();

   ASSERT_TRUE( fromDisk.manifest().entity( US_DataPub::Model, modelGUID,
                                            entity ) );
   EXPECT_EQ( entity.attrs.value( "variance" ).toDouble(), variance );
   EXPECT_EQ( entity.attrs.value( "meniscus" ).toDouble(), meniscus );
}

/* A model with no noise of its own is named in the noise record it
   borrows, when it is exported from a database too -- also when the model
   the record was fitted to is exported with it and lists the record first.
*/
TEST_F( DataPubDbExport, ABorrowingModelIsNamedFromTheDatabase )
{
   QString  laterGUID = US_Util::new_guid();
   US_Model later;
   later.modelGUID   = laterGUID;
   later.editGUID    = editGUID;
   later.description = "demo_run.1A280.2dsa-mc.model";
   ASSERT_EQ( later.write( US_Settings::dataDir() + "/models/M0000002.xml" ),
              IUS_DB2::OK );

   QString dbRunID;
   QString error;
   ASSERT_TRUE( storeInDatabase( dbRunID, error ) ) << error.toStdString();

   // The record was there before the later model was made
   db->rawQuery( QString( "UPDATE noise SET timeEntered = '2020-01-01 00:00:00'"
                          " WHERE noiseGUID = '%1'" ).arg( noiseGUID ) );
   ASSERT_EQ( db->lastErrno(), IUS_DB2::OK ) << db->lastError().toStdString();
   db->rawQuery( QString( "UPDATE model SET lastUpdated = NOW()"
                          " WHERE modelGUID = '%1'" ).arg( laterGUID ) );
   ASSERT_EQ( db->lastErrno(), IUS_DB2::OK ) << db->lastError().toStdString();

   US_DataPubExporter fromDb;
   ASSERT_TRUE( exportFromDb( dbRunID, root + "/from_db.tar.gz", fromDb,
                              error ) ) << error.toStdString();

   US_DataPubEntity noise;
   ASSERT_TRUE( fromDb.manifest().entity( US_DataPub::Noise, noiseGUID,
                                          noise ) );
   EXPECT_EQ( noise.depend( US_DataPub::Model ), modelGUID );
   EXPECT_EQ( noise.attrs.value( "borrowedByModelGUIDs" ), laterGUID )
      << fromDb.log().join( "\n" ).toStdString();
}

/**
 * The fixture database itself (US3_TEST_DB_NAME), read only: no import
 * into a database, so these run against MySQL 8 as well.
 */
class DataPubDbFixtureExport : public DataPubDbTargetEnv
{
   protected:
      const char* databaseVariable( void ) const override
      {
         return "US3_TEST_DB_NAME";
      }
};

/* The fixture's run, with its rotor calibration, out of the database, into
   a disk store and out of that again: the calibration is still there.  It
   used to be lost on the way, the disk store giving it the ID -1.  A
   preview of the export from disk says the same as the export.
*/
TEST_F( DataPubDbFixtureExport, TheFixturesCalibrationSurvivesADiskStore )
{
   US_DataPubExporter::Selection selection;
   selection.fromDb     = true;
   selection.dbPassword = master;
   selection.scope      = US_DataPub::ScopeNoise;
   selection.runIDs << "MWL_demo_run";

   US_DataPubExporter fromDb;
   QString            bundle = root + "/fixture.tar.gz";
   QString            error;
   bool               made   = fromDb.exportBundle( selection, bundle, error );

   if ( ! made  &&  error.contains( "MWL_demo_run" ) )
      GTEST_SKIP() << "not the fixture database: " << error.toStdString();

   ASSERT_TRUE( made ) << error.toStdString();

   QList< US_DataPubEntity > cals =
      fromDb.manifest().section( US_DataPub::RotorCalibration );
   ASSERT_EQ( cals.size(), 1 );

   QString            store = root + "/disk_target";
   US_DataPubImporter importer;
   ASSERT_TRUE( importer.inspect( bundle, error ) ) << error.toStdString();

   US_DataPubImporter::Options options;
   options.target       = US_DataPub::TargetDisk;
   options.outputDir    = store;
   options.verifyHashes = true;
   options.dryRun       = false;
   options.policy       = US_DataPub::PolicyReuse;
   ASSERT_TRUE( importer.runImport( options, error ) ) << error.toStdString();

   US_Settings::set_workBaseDir( store );

   US_DataPubExporter::Selection fromStore;
   fromStore.fromDb = false;
   fromStore.scope  = US_DataPub::ScopeNoise;
   fromStore.runIDs << "MWL_demo_run";

   US_DataPubExporter preview;
   ASSERT_TRUE( preview.previewManifest( fromStore, error ) )
      << error.toStdString();

   US_DataPubExporter again;
   ASSERT_TRUE( again.exportBundle( fromStore, root + "/again.tar.gz", error ) )
      << error.toStdString();

   EXPECT_TRUE( again.manifest().contains( US_DataPub::RotorCalibration,
                                           cals[ 0 ].guid ) )
      << again.warnings().join( "\n" ).toStdString();
   EXPECT_EQ( again.manifest().total(), fromDb.manifest().total() );
   EXPECT_EQ( preview.manifest().total(), again.manifest().total() );
   EXPECT_EQ( preview.warnings(), again.warnings() );
}
