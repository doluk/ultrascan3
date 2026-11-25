// test/programs/us_data_publication/test_us_datapub_export_db.cpp
//
// Exports from a real database, through the scratch import database (see
// test/README, section 4): the synthetic disk store is imported into it,
// and what an export of it then gives is checked -- also after a trip
// through a disk store.  They skip without US3_TEST_DB_IMPORT_NAME.

#include <gtest/gtest.h>

#include <QtCore>

#include <functional>

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

/* A NULL centrifugeProtocol and RIProfile of the source's experiment row
   stay NULL in another database: database -> bundle -> database.  The
   export tells NULL from empty (US_DB2::value() gives a null string for a
   NULL column) and names the NULL columns in the run's "nullFields"; empty
   ones are not named.  A target whose procedures are of
   us3_catalog_version() 2 sets them to NULL again (clear_experiment_fields);
   an older one keeps the '' new_experiment stores, and the import says so
   in one warning.  The bundle is of manifest format 2, which the import
   reads.  The export used to write no marker, and every NULL became ''.

   The target is a second scratch database, US3_TEST_DB_IMPORT2_NAME, built
   like the first (test/README, section 4); the case skips without it.
*/
TEST_F( DataPubDbExport, NullExperimentFieldsStayNullInAnotherDatabase )
{
   QString second = qEnvironmentVariable( "US3_TEST_DB_IMPORT2_NAME" );

   if ( second.isEmpty() )
      GTEST_SKIP() << "no second scratch database (US3_TEST_DB_IMPORT2_NAME)";

   QString dbRunID;
   QString error;
   ASSERT_TRUE( storeInDatabase( dbRunID, error ) ) << error.toStdString();

   QString where = " WHERE experimentID IN ( SELECT experimentID FROM"
                   " experimentPerson WHERE personID = "
                   + QString::number( US_Settings::us_inv_ID() )
                   + " ) AND runID = '" + dbRunID + "'";

   // Empty columns are not NULL ones
   db->rawQuery( "UPDATE experiment SET centrifugeProtocol = '',"
                 " RIProfile = ''" + where );
   ASSERT_EQ( db->lastErrno(), IUS_DB2::OK ) << db->lastError().toStdString();

   US_DataPubExporter empty;
   ASSERT_TRUE( exportFromDb( dbRunID, root + "/empty.tar.gz", empty, error ) )
      << error.toStdString();
   QList< US_DataPubEntity > runs = empty.manifest().section(
                                       US_DataPub::Experiment );
   ASSERT_EQ( runs.size(), 1 );
   EXPECT_FALSE( runs[ 0 ].attrs.contains( "nullFields" ) )
      << runs[ 0 ].attrs.value( "nullFields" ).toStdString();

   db->rawQuery( "UPDATE experiment SET centrifugeProtocol = NULL,"
                 " RIProfile = NULL" + where );
   ASSERT_EQ( db->lastErrno(), IUS_DB2::OK ) << db->lastError().toStdString();

   US_DataPubExporter fromDb;
   QString            bundle = root + "/nulls.tar.gz";
   ASSERT_TRUE( exportFromDb( dbRunID, bundle, fromDb, error ) )
      << error.toStdString();
   EXPECT_EQ( fromDb.manifest().version, 2 );
   runs = fromDb.manifest().section( US_DataPub::Experiment );
   ASSERT_EQ( runs.size(), 1 );
   EXPECT_EQ( runs[ 0 ].attrs.value( "nullFields" ),
              QString( "centrifugeProtocol RIProfile" ) );

   // ... into the second database
   QStringList source = US_Settings::defaultDB();
   QStringList other  = source;
   other.replace( 2, second );
   US_Settings::set_defaultDB( other );

   US_DataPubImporter importer;
   bool imported = importer.inspect( bundle, error );
   int  version  = imported ? importer.manifest().version : -1;
   imported      = imported  &&
                   importer.runImport( target( true, QString() ), error );

   US_DB2  tdb;
   QString cerror;
   bool    connected = tdb.connect( master, cerror );
   US_Settings::set_defaultDB( source );

   ASSERT_TRUE( imported ) << error.toStdString() << "\n"
                           << importer.log().join( "\n" ).toStdString();
   ASSERT_TRUE( connected ) << cerror.toStdString();
   EXPECT_EQ( version, 2 );

   QString expID;

   for ( const US_DataPubImporter::Result& result : importer.results() )
      if ( result.type == US_DataPub::Experiment )
         expID = result.targetID;

   ASSERT_GT( expID.toInt(), 0 );

   QString procs  = sql_value( &tdb, "SELECT us3_catalog_version()" );
   bool    kept   = ( procs != "<none>"  &&  procs.toInt() >= 2 );

   EXPECT_EQ( sql_value( &tdb, "SELECT centrifugeProtocol IS NULL FROM"
                         " experiment WHERE experimentID = " + expID ),
              QString( kept ? "1" : "0" ) ) << procs.toStdString();
   EXPECT_EQ( sql_value( &tdb, "SELECT RIProfile IS NULL FROM experiment"
                         " WHERE experimentID = " + expID ),
              QString( kept ? "1" : "0" ) ) << procs.toStdString();

   QStringList about;

   for ( const QString& text : importer.warnings() )
      if ( text.contains( "us3_catalog_version()" ) )  about << text;

   if ( kept )
   {
      EXPECT_TRUE( about.isEmpty() ) << about.join( "\n" ).toStdString();
   }

   else
   {
      ASSERT_EQ( about.size(), 1 ) << importer.warnings().join( "\n" )
                                       .toStdString();
      EXPECT_TRUE( about[ 0 ].contains( "centrifugeProtocol and RIProfile" ) )
         << about[ 0 ].toStdString();
   }
}

/* Two experiments of the investigator under one run ID.  The run ID alone
   names neither: the export is refused with a message that names both
   experiment IDs (not "directories").  The experiment ID picks one, and
   what is exported is that experiment -- its GUID, its raw data -- also
   when it is the one listed second.

   The second experiment is the store's run made again under new GUIDs and
   imported too; when the import gave it another run ID it is then given
   the first one's, with its file names to match.  Everything is put back
   afterwards.
*/
TEST_F( DataPubDbExport, TwoExperimentsOfOneRunIdArePickedByTheirIds )
{
   QString runA;
   QString runB;
   QString error;
   QString guidA = expGUID;
   ASSERT_TRUE( storeInDatabase( runA, error ) ) << error.toStdString();

   // The same run again, as another experiment: its own records under new
   // GUIDs, the chemistry, project and calibration as they were
   QDir( US_Settings::dataDir()   ).removeRecursively();
   QDir( US_Settings::resultDir() ).removeRecursively();
   QDir().mkpath( US_Settings::dataDir()   );
   QDir().mkpath( US_Settings::resultDir() );
   expGUID   = US_Util::new_guid();
   rawGUID   = US_Util::new_guid();
   editGUID  = US_Util::new_guid();
   modelGUID = US_Util::new_guid();
   noiseGUID = US_Util::new_guid();
   buildStore();

   QString guidB = expGUID;
   ASSERT_TRUE( storeInDatabase( runB, error ) ) << error.toStdString();

   auto idOf = [ this ]( const QString& guid )
   {
      db->rawQuery( "SELECT experimentID FROM experiment"
                    " WHERE experimentGUID = '" + guid + "'" );
      return db->next() ? db->value( 0 ).toInt() : -1;
   };

   int idA = idOf( guidA );
   int idB = idOf( guidB );
   ASSERT_GT( idA, 0 );
   ASSERT_GT( idB, idA );
   // Experiment B under run ID A, its files named as A's are
   auto rename = [ this, idB ]( const QString& from, const QString& to )
   {
      QString len = QString::number( from.length() + 1 );
      db->rawQuery( "UPDATE experiment SET runID = '" + to + "'"
                    " WHERE experimentID = " + QString::number( idB ) );
      db->rawQuery( "UPDATE rawData SET filename = CONCAT( '" + to + "',"
                    " SUBSTRING( filename, " + len + " ) )"
                    " WHERE experimentID = " + QString::number( idB )
                    + " AND filename LIKE '" + from + ".%'" );
      db->rawQuery( "UPDATE editedData e JOIN rawData r"
                    " ON r.rawDataID = e.rawDataID"
                    " SET e.filename = CONCAT( '" + to + "',"
                    " SUBSTRING( e.filename, " + len + " ) )"
                    " WHERE r.experimentID = " + QString::number( idB )
                    + " AND e.filename LIKE '" + from + ".%'" );
   };

   struct PutBack
   {
      std::function< void() > undo;
      ~PutBack() { undo(); }
   } putBack { [ & ]() { if ( runA != runB )  rename( runA, runB ); } };

   if ( runA != runB )  rename( runB, runA );

   db->rawQuery( "SELECT COUNT(*) FROM experiment WHERE runID = '" + runA
                 + "'" );
   ASSERT_TRUE( db->next() );
   ASSERT_EQ( db->value( 0 ).toInt(), 2 );

   // By the run ID: refused, naming both experiments
   US_DataPubExporter            refused;
   US_DataPubExporter::Selection selection;
   selection.fromDb     = true;
   selection.dbPassword = master;
   selection.scope      = US_DataPub::ScopeNoise;
   selection.runIDs << runA;

   EXPECT_FALSE( refused.previewManifest( selection, error ) );
   QString both = QString( "%1, %2" ).arg( idA ).arg( idB );
   EXPECT_TRUE( error.contains( both ) ) << error.toStdString();
   EXPECT_FALSE( error.contains( "directories" ) ) << error.toStdString();

   // By the experiment ID: that experiment, the first or the second
   for ( int pass = 0; pass < 2; pass++ )
   {
      int     id   = ( pass == 0 ) ? idA : idB;
      QString guid = ( pass == 0 ) ? guidA : guidB;
      SCOPED_TRACE( QString::number( id ).toStdString() );

      US_DataPubExporter            exporter;
      US_DataPubExporter::Selection picked = selection;
      picked.experimentIDs.insert( runA, QString::number( id ) );

      ASSERT_TRUE( exporter.previewManifest( picked, error ) )
         << error.toStdString() << "\n"
         << exporter.log().join( "\n" ).toStdString();

      QList< US_DataPubEntity > exps =
         exporter.manifest().section( US_DataPub::Experiment );
      ASSERT_EQ( exps.size(), 1 );
      EXPECT_EQ( exps[ 0 ].guid, guid );
      EXPECT_EQ( exps[ 0 ].id, QString::number( id ) );

      // Its own raw data, by the experiment's rows
      db->rawQuery( "SELECT rawDataGUID FROM rawData WHERE experimentID = "
                    + QString::number( id ) );
      QStringList raws;

      while ( db->next() )  raws << db->value( 0 ).toString();

      QList< US_DataPubEntity > staged =
         exporter.manifest().section( US_DataPub::RawData );
      ASSERT_EQ( staged.size(), raws.size() );

      for ( int ii = 0; ii < staged.size(); ii++ )
         EXPECT_TRUE( raws.contains( staged[ ii ].guid ) )
            << staged[ ii ].guid.toStdString();
   }
}
