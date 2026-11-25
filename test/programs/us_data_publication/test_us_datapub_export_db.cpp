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

/* An edit with more than one noise record of a type, and models without
   noise of their own, exported from a database: each borrower is named in
   the records it was run with -- the newest of each type that was there
   when it was made, by the rows' time stamps -- and in no other.

     M1 (the store's) 00:00, its noise N1 (ti) 00:10, borrower B1 00:20,
     M2 00:30, its noise N2 (ti) 00:40 and N3 (ri) 00:50, borrower B2 01:00
*/
TEST_F( DataPubDbExport, EachBorrowerIsNamedInTheNoiseItWasRunWith )
{
   auto write_model = [ & ]( const QString& file, const QString& desc )
   {
      US_Model model;
      model.modelGUID   = US_Util::new_guid();
      model.editGUID    = editGUID;
      model.description = desc;
      EXPECT_EQ( model.write( US_Settings::dataDir() + "/models/" + file ),
                 IUS_DB2::OK );
      return model.modelGUID;
   };

   auto write_noise = [ & ]( const QString& file, const QString& model,
                             US_Noise::NoiseType type, const QString& desc )
   {
      US_Noise noise;
      noise.noiseGUID   = US_Util::new_guid();
      noise.modelGUID   = model;
      noise.description = desc;
      noise.type        = type;
      noise.values << 0.001 << 0.002;
      noise.count       = 2;
      EXPECT_EQ( noise.write( US_Settings::dataDir() + "/noises/" + file ),
                 IUS_DB2::OK );
      return noise.noiseGUID;
   };

   QString b1 = write_model( "M0000011.xml", "demo_run.1A280.e1_a1_2DSA-B1.model" );
   QString m2 = write_model( "M0000012.xml", "demo_run.1A280.e1_a1_2DSA-M2.model" );
   QString n2 = write_noise( "N0000012.xml", m2, US_Noise::TI,
                             "demo_run.1A280.e1_a1_2DSA-M2.ti_noise" );
   QString n3 = write_noise( "N0000013.xml", m2, US_Noise::RI,
                             "demo_run.1A280.e1_a1_2DSA-M2.ri_noise" );
   QString b2 = write_model( "M0000013.xml", "demo_run.1A280.e1_a1_2DSA-B2.model" );

   QString dbRunID;
   QString error;
   ASSERT_TRUE( storeInDatabase( dbRunID, error ) ) << error.toStdString();

   const QList< QPair< QString, QString > > models =
   {
      { modelGUID, "00:00:00" }, { b1, "00:00:20" }, { m2, "00:00:30" },
      { b2, "00:01:00" }
   };
   const QList< QPair< QString, QString > > noises =
   {
      { noiseGUID, "00:00:10" }, { n2, "00:00:40" }, { n3, "00:00:50" }
   };

   for ( const auto& mm : models )
   {
      db->rawQuery( QString( "UPDATE model SET lastUpdated = '2020-01-01 %1'"
                             " WHERE modelGUID = '%2'" )
                    .arg( mm.second ).arg( mm.first ) );
      ASSERT_EQ( db->lastErrno(), IUS_DB2::OK ) << db->lastError().toStdString();
   }

   for ( const auto& nn : noises )
   {
      db->rawQuery( QString( "UPDATE noise SET timeEntered = '2020-01-01 %1'"
                             " WHERE noiseGUID = '%2'" )
                    .arg( nn.second ).arg( nn.first ) );
      ASSERT_EQ( db->lastErrno(), IUS_DB2::OK ) << db->lastError().toStdString();
   }

   US_DataPubExporter fromDb;
   ASSERT_TRUE( exportFromDb( dbRunID, root + "/from_db.tar.gz", fromDb,
                              error ) )
      << error.toStdString() << "\n" << fromDb.log().join( "\n" ).toStdString();

   const US_DataPubManifest& mani = fromDb.manifest();
   EXPECT_EQ( mani.count( US_DataPub::Model ), 4 );
   ASSERT_EQ( mani.count( US_DataPub::Noise ), 3 );

   struct Expect { QString noise; QString fitted; QString borrowers; };
   const QList< Expect > expected =
   {
      { noiseGUID, modelGUID, b1 },
      { n2,        m2,        b2 },
      { n3,        m2,        b2 },
   };

   for ( const Expect& ex : expected )
   {
      US_DataPubEntity noise;
      ASSERT_TRUE( mani.entity( US_DataPub::Noise, ex.noise, noise ) );
      EXPECT_EQ( noise.depend( US_DataPub::Model ), ex.fitted );
      EXPECT_EQ( noise.attrs.value( "borrowedByModelGUIDs" ), ex.borrowers )
         << ex.noise.toStdString() << "\n"
         << fromDb.log().join( "\n" ).toStdString();
   }
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

namespace
{
   // The first column of the first row of a query, or "<none>"
   QString sql_value( IUS_DB2* db, const QString& sql )
   {
      db->rawQuery( sql );

      if ( db->lastErrno() != IUS_DB2::OK  ||  ! db->next() )
         return QString( "<none>" );

      return db->value( 0 ).toString();
   }
}

/* What the source's rows hold beyond the payloads travels from one
   database to another: database -> bundle -> database.  The export puts
   the buffer's private flag, the raw data's label and comment, the edit's
   comment and the analyte's spectrum column (also when it is empty) into
   the manifest, and the import stores them.  The export used to leave
   them out, and the target had a private buffer, the run ID for a label,
   no comments and a spectrum made from the profile.

   The target is a second scratch database, US3_TEST_DB_IMPORT2_NAME, built
   like the first (test/README, section 4); the case skips without it.
*/
TEST_F( DataPubDbExport, TheSourcesFieldsTravelToAnotherDatabase )
{
   QString second = qEnvironmentVariable( "US3_TEST_DB_IMPORT2_NAME" );

   if ( second.isEmpty() )
      GTEST_SKIP() << "no second scratch database (US3_TEST_DB_IMPORT2_NAME)";

   QString dbRunID;
   QString error;
   ASSERT_TRUE( storeInDatabase( dbRunID, error ) ) << error.toStdString();

   // An empty spectrum column travels as one
   db->rawQuery( "UPDATE analyte SET spectrum = '' WHERE analyteGUID = '"
                 + analyteGUID + "'" );
   ASSERT_EQ( db->lastErrno(), IUS_DB2::OK ) << db->lastError().toStdString();

   US_DataPubExporter empty;
   ASSERT_TRUE( exportFromDb( dbRunID, root + "/empty.tar.gz", empty, error ) )
      << error.toStdString();

   US_DataPubEntity analyte;
   ASSERT_TRUE( empty.manifest().entity( US_DataPub::Analyte, analyteGUID,
                                         analyte ) );
   EXPECT_TRUE( analyte.attrs.contains( "spectrum" ) );
   EXPECT_EQ  ( analyte.attrs.value( "spectrum" ), QString() );

   // Values of their own in the source's rows
   const QStringList updates =
   {
      "UPDATE bufferPerson bp, buffer b SET bp.private = 0 WHERE"
      " bp.bufferID = b.bufferID AND b.bufferGUID = '" + bufferGUID + "'",
      "UPDATE rawData SET label = 'raw label', comment = 'raw comment, \"quoted\""
      "' WHERE rawDataGUID = '" + rawGUID + "'",
      "UPDATE editedData SET comment = 'edited by hand' WHERE editGUID = '"
      + editGUID + "'",
      "UPDATE analyte SET spectrum = '250/2.5' WHERE analyteGUID = '"
      + analyteGUID + "'"
   };

   for ( const QString& sql : updates )
   {
      db->rawQuery( sql );
      ASSERT_EQ( db->lastErrno(), IUS_DB2::OK ) << db->lastError().toStdString()
                                                 << "\n" << sql.toStdString();
   }

   US_DataPubExporter fromDb;
   QString            bundle = root + "/fields.tar.gz";
   ASSERT_TRUE( exportFromDb( dbRunID, bundle, fromDb, error ) )
      << error.toStdString();

   const US_DataPubManifest& mani = fromDb.manifest();
   US_DataPubEntity entity;

   ASSERT_TRUE( mani.entity( US_DataPub::Buffer, bufferGUID, entity ) );
   EXPECT_EQ( entity.attrs.value( "private" ), QString( "0" ) );
   ASSERT_TRUE( mani.entity( US_DataPub::RawData, rawGUID, entity ) );
   EXPECT_EQ( entity.attrs.value( "label" ),   QString( "raw label" ) );
   EXPECT_EQ( entity.attrs.value( "comment" ),
              QString( "raw comment, \"quoted\"" ) );
   ASSERT_TRUE( mani.entity( US_DataPub::EditedData, editGUID, entity ) );
   EXPECT_EQ( entity.attrs.value( "comment" ), QString( "edited by hand" ) );
   ASSERT_TRUE( mani.entity( US_DataPub::Analyte, analyteGUID, entity ) );
   EXPECT_EQ( entity.attrs.value( "spectrum" ), QString( "250/2.5" ) );

   // ... into the second database
   QStringList source = US_Settings::defaultDB();
   QStringList other  = source;
   other.replace( 2, second );
   US_Settings::set_defaultDB( other );

   US_DataPubImporter importer;
   bool imported = importer.inspect( bundle, error )  &&
                   importer.runImport( target( true, QString() ), error );

   US_DB2  tdb;
   QString cerror;
   bool    connected = tdb.connect( master, cerror );
   US_Settings::set_defaultDB( source );

   ASSERT_TRUE( imported ) << error.toStdString() << "\n"
                           << importer.log().join( "\n" ).toStdString();
   ASSERT_TRUE( connected ) << cerror.toStdString();

   EXPECT_EQ( sql_value( &tdb, "SELECT bp.private FROM buffer b, bufferPerson bp"
                         " WHERE b.bufferID = bp.bufferID AND b.bufferGUID = '"
                         + bufferGUID + "'" ), QString( "0" ) );
   EXPECT_EQ( sql_value( &tdb, "SELECT label FROM rawData WHERE rawDataGUID = '"
                         + rawGUID + "'" ), QString( "raw label" ) );
   EXPECT_EQ( sql_value( &tdb, "SELECT comment FROM rawData WHERE"
                         " rawDataGUID = '" + rawGUID + "'" ),
              QString( "raw comment, \"quoted\"" ) );
   EXPECT_EQ( sql_value( &tdb, "SELECT comment FROM editedData WHERE"
                         " editGUID = '" + editGUID + "'" ),
              QString( "edited by hand" ) );
   EXPECT_EQ( sql_value( &tdb, "SELECT spectrum FROM analyte WHERE"
                         " analyteGUID = '" + analyteGUID + "'" ),
              QString( "250/2.5" ) );
}
