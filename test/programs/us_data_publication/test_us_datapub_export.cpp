// test/programs/us_data_publication/test_us_datapub_export.cpp
#include <gtest/gtest.h>

#include "datapub_test_env.h"

#include "us_datapub_catalog.h"
#include "us_datapub_export.h"
#include "us_datapub_manifest.h"
#include "us_datapub_hash.h"

class DataPubExport : public DataPubTestEnv
{
   protected:
      US_DataPubExporter::Selection baseSelection( US_DataPub::Scope scope )
      {
         US_DataPubExporter::Selection selection;
         selection.fromDb = false;
         selection.scope  = scope;
         selection.runIDs << runID;

         return selection;
      }
};

TEST_F( DataPubExport, CatalogSeesTheWholeChain )
{
   US_DataPubCatalog catalog;
   QString           error;

   ASSERT_TRUE( catalog.open( false, QString(), error ) )
      << error.toStdString();

   QList< US_DataPubCatalog::Run > runs = catalog.runs( QString(), error );
   ASSERT_EQ( runs.size(), 1 ) << error.toStdString();
   EXPECT_EQ( runs[ 0 ].runID,       runID );
   EXPECT_EQ( runs[ 0 ].guid,        expGUID );
   EXPECT_EQ( runs[ 0 ].projectGUID, projectGUID );
   EXPECT_EQ( runs[ 0 ].runType,     runType );

   ASSERT_TRUE( catalog.loadRunDetails( runs[ 0 ], error ) )
      << error.toStdString();
   ASSERT_EQ( runs[ 0 ].raws.size(), 1 );
   EXPECT_EQ( runs[ 0 ].raws[ 0 ].guid,     rawGUID );
   EXPECT_EQ( runs[ 0 ].raws[ 0 ].filename, rawFile );
   ASSERT_EQ( runs[ 0 ].raws[ 0 ].edits.size(), 1 );
   EXPECT_EQ( runs[ 0 ].raws[ 0 ].edits[ 0 ].guid,     editGUID );
   EXPECT_EQ( runs[ 0 ].raws[ 0 ].edits[ 0 ].filename, editFile );

   QList< US_DataPubCatalog::Model > models = catalog.models( runs, error );
   ASSERT_EQ( models.size(), 1 ) << error.toStdString();
   EXPECT_EQ( models[ 0 ].guid,     modelGUID );
   EXPECT_EQ( models[ 0 ].editGUID, editGUID );

   QList< US_DataPubCatalog::Noise > noises = catalog.noises( models, error );
   ASSERT_EQ( noises.size(), 1 ) << error.toStdString();
   EXPECT_EQ( noises[ 0 ].guid,      noiseGUID );
   EXPECT_EQ( noises[ 0 ].modelGUID, modelGUID );
}

// "Data only" means the chain down to the raw data.  Raw data cannot be
// re-created without the solutions its channels refer to, so the scope is
// raised to the solutions and the edits, models and noise stay out.
TEST_F( DataPubExport, DataOnlyExport )
{
   US_DataPubExporter exporter;
   QString            bundle = root + "/data_only.tar.gz";
   QString            error;

   ASSERT_TRUE( exporter.exportBundle( baseSelection( US_DataPub::ScopeRawData ),
                bundle, error ) ) << error.toStdString();
   ASSERT_TRUE( QFile::exists( bundle ) );

   const US_DataPubManifest& mani = exporter.manifest();

   EXPECT_EQ( mani.scope, "solutions" );
   EXPECT_EQ( mani.count( US_DataPub::Project    ), 1 );
   EXPECT_EQ( mani.count( US_DataPub::Experiment ), 1 );
   EXPECT_EQ( mani.count( US_DataPub::RawData    ), 1 );
   EXPECT_EQ( mani.count( US_DataPub::Buffer     ), 1 );
   EXPECT_EQ( mani.count( US_DataPub::Analyte    ), 1 );
   EXPECT_EQ( mani.count( US_DataPub::Solution   ), 1 );
   EXPECT_EQ( mani.count( US_DataPub::EditedData ), 0 );
   EXPECT_EQ( mani.count( US_DataPub::Model      ), 0 );
   EXPECT_EQ( mani.count( US_DataPub::Noise      ), 0 );

   US_DataPubEntity raw;
   ASSERT_TRUE( mani.entity( US_DataPub::RawData, rawGUID, raw ) );
   EXPECT_EQ( raw.filename, rawFile );
   EXPECT_EQ( raw.name,     rawFile );
   EXPECT_EQ( raw.depend( US_DataPub::Experiment ), expGUID );
   EXPECT_EQ( raw.depend( US_DataPub::Solution   ), solutionGUID );
   EXPECT_FALSE( raw.sha256.isEmpty() );

   US_DataPubEntity exper;
   ASSERT_TRUE( mani.entity( US_DataPub::Experiment, expGUID, exper ) );
   EXPECT_EQ( exper.name, runID );
   EXPECT_EQ( exper.depend( US_DataPub::Project ), projectGUID );

   QString verror;
   EXPECT_TRUE( mani.validate( verror ) ) << verror.toStdString();
}

TEST_F( DataPubExport, ProjectOnlyExportNeedsNoRun )
{
   US_DataPubExporter            exporter;
   US_DataPubExporter::Selection selection;
   selection.fromDb      = false;
   selection.scope       = US_DataPub::ScopeProject;
   selection.projectGUID = projectGUID;

   QString bundle = root + "/project_only.tar.gz";
   QString error;

   ASSERT_TRUE( exporter.exportBundle( selection, bundle, error ) )
      << error.toStdString();

   EXPECT_EQ( exporter.manifest().total(), 1 );
   EXPECT_EQ( exporter.manifest().count( US_DataPub::Project ), 1 );
}

TEST_F( DataPubExport, ModelExportCarriesItsDependencies )
{
   US_DataPubExporter            exporter;
   US_DataPubExporter::Selection selection = baseSelection( US_DataPub::ScopeNoise );
   QStringList                   wanted;
   wanted << modelGUID;
   selection.setModels( wanted );

   QString bundle = root + "/with_models.tar.gz";
   QString error;

   ASSERT_TRUE( exporter.exportBundle( selection, bundle, error ) )
      << error.toStdString();

   const US_DataPubManifest& mani = exporter.manifest();

   EXPECT_EQ( mani.count( US_DataPub::EditedData ), 1 );
   EXPECT_EQ( mani.count( US_DataPub::Model      ), 1 );
   EXPECT_EQ( mani.count( US_DataPub::Noise      ), 1 );
   EXPECT_EQ( mani.count( US_DataPub::Solution   ), 1 );

   US_DataPubEntity model;
   ASSERT_TRUE( mani.entity( US_DataPub::Model, modelGUID, model ) );
   EXPECT_EQ( model.depend( US_DataPub::EditedData ), editGUID );

   US_DataPubEntity noise;
   ASSERT_TRUE( mani.entity( US_DataPub::Noise, noiseGUID, noise ) );
   EXPECT_EQ( noise.depend( US_DataPub::Model ), modelGUID );

   US_DataPubEntity edit;
   ASSERT_TRUE( mani.entity( US_DataPub::EditedData, editGUID, edit ) );
   EXPECT_EQ( edit.depend( US_DataPub::RawData ), rawGUID );

   QString verror;
   EXPECT_TRUE( mani.validate( verror ) ) << verror.toStdString();
}

TEST_F( DataPubExport, DeselectingTheEditDropsTheModel )
{
   US_DataPubExporter            exporter;
   US_DataPubExporter::Selection selection = baseSelection( US_DataPub::ScopeNoise );
   selection.setEdits ( QStringList() );        // no edits at all
   selection.setModels( QStringList() );        // and therefore no models

   QString bundle = root + "/no_edits.tar.gz";
   QString error;

   ASSERT_TRUE( exporter.exportBundle( selection, bundle, error ) )
      << error.toStdString();

   EXPECT_EQ( exporter.manifest().count( US_DataPub::EditedData ), 0 );
   EXPECT_EQ( exporter.manifest().count( US_DataPub::Model      ), 0 );
   EXPECT_EQ( exporter.manifest().count( US_DataPub::Noise      ), 0 );
   EXPECT_EQ( exporter.manifest().count( US_DataPub::RawData    ), 1 );
}

TEST_F( DataPubExport, PayloadDigestsMatchTheStagedFiles )
{
   US_DataPubExporter exporter;
   QString            bundle = root + "/digest_check.tar.gz";
   QString            error;

   ASSERT_TRUE( exporter.exportBundle( baseSelection( US_DataPub::ScopeNoise ),
                bundle, error ) ) << error.toStdString();

   US_DataPubBundle unpacked;
   ASSERT_TRUE( unpacked.unpack( bundle, error ) ) << error.toStdString();

   QList< US_DataPubEntity > entities = exporter.manifest().all();
   ASSERT_GT( entities.size(), 0 );

   for ( int ii = 0; ii < entities.size(); ii++ )
   {
      QString path = unpacked.rootPath() + "/" + entities[ ii ].payload;

      ASSERT_TRUE( QFile::exists( path ) )
         << entities[ ii ].payload.toStdString();
      EXPECT_EQ( US_DataPubHash::fileHash( path ), entities[ ii ].sha256 )
         << entities[ ii ].payload.toStdString();
   }
}

TEST_F( DataPubExport, PreviewStagesNothing )
{
   US_DataPubExporter exporter;
   QString            error;

   ASSERT_TRUE( exporter.previewManifest(
                baseSelection( US_DataPub::ScopeNoise ), error ) )
      << error.toStdString();

   const US_DataPubManifest& mani = exporter.manifest();

   EXPECT_GT( mani.total(), 0 );
   EXPECT_EQ( mani.count( US_DataPub::Model ), 1 );

   // A preview answers "what would be exported", so the entries name their
   // payloads but carry no digest: nothing was copied to digest.
   QList< US_DataPubEntity > entities = mani.all();

   for ( int ii = 0; ii < entities.size(); ii++ )
   {
      EXPECT_FALSE( entities[ ii ].payload.isEmpty() );
      EXPECT_TRUE ( entities[ ii ].sha256 .isEmpty() );
   }
}

TEST_F( DataPubExport, AnUnknownRunIsAnError )
{
   US_DataPubExporter            exporter;
   US_DataPubExporter::Selection selection;
   selection.fromDb = false;
   selection.scope  = US_DataPub::ScopeEdits;
   selection.runIDs << "no_such_run";

   QString error;

   EXPECT_FALSE( exporter.exportBundle( selection, root + "/bad.tar.gz",
                                        error ) );
   EXPECT_TRUE ( error.contains( "no_such_run" ) ) << error.toStdString();
}
