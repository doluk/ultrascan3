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

// A model the user picks by hand -- one whose edit is not among those the
// catalog walked -- still brings its noise records along.
TEST_F( DataPubExport, NoiseIsFoundForAModelOutsideTheChain )
{
   US_DataPubCatalog catalog;
   QString           error;

   ASSERT_TRUE( catalog.open( false, QString(), error ) )
      << error.toStdString();

   // as the export pane builds it: a GUID and an edit, but no chain
   US_DataPubCatalog::Model model;
   model.guid     = modelGUID;
   model.editGUID = editGUID;
   ASSERT_TRUE( model.noises.isEmpty() );

   QList< US_DataPubCatalog::Model > models;
   models << model;

   QList< US_DataPubCatalog::Noise > noises = catalog.noises( models, error );

   ASSERT_EQ( noises.size(), 1 ) << error.toStdString();
   EXPECT_EQ( noises[ 0 ].guid,      noiseGUID );
   EXPECT_EQ( noises[ 0 ].modelGUID, modelGUID );
   EXPECT_EQ( noises[ 0 ].editGUID,  editGUID );
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

// The project record is often not the exporter's to publish, and the
// receiving installation may keep its own project list, so it can be left
// out.  The runs still name it, which is what lets an import reattach them.
TEST_F( DataPubExport, ProjectCanBeLeftOut )
{
   US_DataPubExporter            exporter;
   US_DataPubExporter::Selection selection = baseSelection( US_DataPub::ScopeNoise );
   selection.includeProject = false;

   QString bundle = root + "/no_project.tar.gz";
   QString error;

   ASSERT_TRUE( exporter.exportBundle( selection, bundle, error ) )
      << error.toStdString();

   const US_DataPubManifest& mani = exporter.manifest();

   EXPECT_EQ( mani.count( US_DataPub::Project    ), 0 );
   EXPECT_EQ( mani.count( US_DataPub::Experiment ), 1 );
   EXPECT_EQ( mani.count( US_DataPub::RawData    ), 1 );
   EXPECT_EQ( mani.count( US_DataPub::Model      ), 1 );

   US_DataPubEntity exper;
   ASSERT_TRUE( mani.entity( US_DataPub::Experiment, expGUID, exper ) );

   // Not a dependency any more -- the bundle cannot satisfy it -- but still
   // recorded, so an import knows which project the run belongs to.
   EXPECT_TRUE( exper.depend( US_DataPub::Project ).isEmpty() );
   EXPECT_EQ  ( exper.attrs.value( "projectGUID" ), projectGUID );
   EXPECT_EQ  ( exper.attrs.value( "projectDescription" ),
                "Demo publication project" );

   QString verror;
   EXPECT_TRUE( mani.validate( verror ) ) << verror.toStdString();
}

TEST_F( DataPubExport, ProjectOnlyScopeWithoutTheProjectIsRefused )
{
   US_DataPubExporter            exporter;
   US_DataPubExporter::Selection selection;
   selection.fromDb         = false;
   selection.scope          = US_DataPub::ScopeProject;
   selection.projectGUID    = projectGUID;
   selection.includeProject = false;

   QString error;

   EXPECT_FALSE( exporter.exportBundle( selection, root + "/nothing.tar.gz",
                                        error ) );
   EXPECT_TRUE ( error.contains( "nothing in the bundle" ) )
      << error.toStdString();
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

// ------------------------------------------------------------ borrowed noise

// Noise is fitted to an edit.  A model made after it, with no noise of its
// own, is still reproducible with the noise that was there at the time, so
// that is what the bundle carries for it.
TEST_F( DataPubExport, AModelWithNoNoiseTakesTheEditsLatest )
{
   // a second model of the same edit, written after the noise record
   QString  laterGUID = US_Util::new_guid();
   US_Model later;
   later.modelGUID   = laterGUID;
   later.editGUID    = editGUID;
   later.description = "demo_run.1A280.2dsa-mc.model";
   later.write( US_Settings::dataDir() + "/models/M0000002.xml" );

   US_DataPubCatalog catalog;
   QString           error;

   ASSERT_TRUE( catalog.open( false, QString(), error ) )
      << error.toStdString();

   QList< US_DataPubCatalog::Run > runs = catalog.runs( QString(), error );
   ASSERT_EQ  ( runs.size(), 1 );
   ASSERT_TRUE( catalog.loadRunDetails( runs[ 0 ], error ) );

   QList< US_DataPubCatalog::Model > models = catalog.models( runs, error );
   ASSERT_EQ( models.size(), 2 ) << error.toStdString();

   QList< US_DataPubCatalog::Model > chosen;

   for ( int ii = 0; ii < models.size(); ii++ )
      if ( models[ ii ].guid == laterGUID )  chosen << models[ ii ];

   ASSERT_EQ  ( chosen.size(), 1 );
   ASSERT_TRUE( chosen[ 0 ].noises.isEmpty() );

   QList< US_DataPubCatalog::Noise > noises = catalog.noises( chosen, error );

   ASSERT_EQ( noises.size(), 1 ) << error.toStdString();
   EXPECT_EQ( noises[ 0 ].guid,         noiseGUID );
   EXPECT_EQ( noises[ 0 ].modelGUID,    laterGUID );  // carried for this model
   EXPECT_EQ( noises[ 0 ].borrowedFrom, modelGUID );  // fitted to the other
   EXPECT_EQ( noises[ 0 ].editGUID,     editGUID  );
}

// Every selected model keeps its own noise.  Picking several at once used
// to answer for the first one only.
TEST_F( DataPubExport, EverySelectedModelKeepsItsOwnNoise )
{
   QString  secondModel = US_Util::new_guid();
   QString  secondNoise = US_Util::new_guid();

   US_Model model;
   model.modelGUID   = secondModel;
   model.editGUID    = editGUID;
   model.description = "demo_run.1A280.2dsa-mc.model";
   model.write( US_Settings::dataDir() + "/models/M0000002.xml" );

   US_Noise noise;
   noise.noiseGUID   = secondNoise;
   noise.modelGUID   = secondModel;
   noise.description = "demo_run.1A280.2dsa-mc.ri_noise";
   noise.type        = US_Noise::RI;
   noise.values << 0.004 << 0.005;
   noise.count  = 2;
   noise.write( US_Settings::dataDir() + "/noises/N0000002.xml" );

   US_DataPubCatalog catalog;
   QString           error;

   ASSERT_TRUE( catalog.open( false, QString(), error ) )
      << error.toStdString();

   QList< US_DataPubCatalog::Run > runs = catalog.runs( QString(), error );
   ASSERT_EQ  ( runs.size(), 1 );
   ASSERT_TRUE( catalog.loadRunDetails( runs[ 0 ], error ) );

   QList< US_DataPubCatalog::Model > models = catalog.models( runs, error );
   ASSERT_EQ( models.size(), 2 ) << error.toStdString();

   QList< US_DataPubCatalog::Noise > noises = catalog.noises( models, error );

   ASSERT_EQ( noises.size(), 2 ) << error.toStdString();

   QStringList guids;
   QStringList owners;

   for ( int ii = 0; ii < noises.size(); ii++ )
   {
      guids  << noises[ ii ].guid;
      owners << noises[ ii ].modelGUID;

      // neither of them is borrowed: each model has its own
      EXPECT_TRUE( noises[ ii ].borrowedFrom.isEmpty() );
   }

   EXPECT_TRUE( guids .contains( noiseGUID   ) );
   EXPECT_TRUE( guids .contains( secondNoise ) );
   EXPECT_TRUE( owners.contains( modelGUID   ) );
   EXPECT_TRUE( owners.contains( secondModel ) );
}

// A bundle of several models carries the noise of each of them.
TEST_F( DataPubExport, ABundleOfSeveralModelsCarriesAllTheirNoise )
{
   QString  secondModel = US_Util::new_guid();
   QString  secondNoise = US_Util::new_guid();

   US_Model model;
   model.modelGUID   = secondModel;
   model.editGUID    = editGUID;
   model.description = "demo_run.1A280.2dsa-mc.model";
   model.write( US_Settings::dataDir() + "/models/M0000002.xml" );

   US_Noise noise;
   noise.noiseGUID   = secondNoise;
   noise.modelGUID   = secondModel;
   noise.description = "demo_run.1A280.2dsa-mc.ri_noise";
   noise.type        = US_Noise::RI;
   noise.values << 0.004 << 0.005;
   noise.count  = 2;
   noise.write( US_Settings::dataDir() + "/noises/N0000002.xml" );

   US_DataPubExporter::Selection selection = baseSelection(
         US_DataPub::ScopeNoise );

   US_DataPubExporter exporter;
   QString            error;

   ASSERT_TRUE( exporter.previewManifest( selection, error ) )
      << error.toStdString();

   const US_DataPubManifest& mani = exporter.manifest();

   EXPECT_EQ( mani.section( US_DataPub::Model ).size(), 2 );
   ASSERT_EQ( mani.section( US_DataPub::Noise ).size(), 2 );

   US_DataPubEntity first;
   US_DataPubEntity second;

   ASSERT_TRUE( mani.entity( US_DataPub::Noise, noiseGUID,   first  ) );
   ASSERT_TRUE( mani.entity( US_DataPub::Noise, secondNoise, second ) );

   EXPECT_EQ( first .depend( US_DataPub::Model ), modelGUID   );
   EXPECT_EQ( second.depend( US_DataPub::Model ), secondModel );
}

// The rule itself: the newest record of each type that already existed.
TEST_F( DataPubExport, NoiseBeforePicksTheLatestOfEachTypeInTime )
{
   QList< US_DataCatalog::Noise > candidates;

   US_DataCatalog::Noise oldTi;
   oldTi.guid        = "ti-old";
   oldTi.noiseType   = "ti";
   oldTi.lastUpdated = "2024-01-01 10:00:00 UTC";

   US_DataCatalog::Noise newTi;
   newTi.guid        = "ti-new";
   newTi.noiseType   = "ti";
   newTi.lastUpdated = "2024-01-01 11:00:00 UTC";

   US_DataCatalog::Noise laterTi;          // made after the model
   laterTi.guid        = "ti-later";
   laterTi.noiseType   = "ti";
   laterTi.lastUpdated = "2024-01-02 09:00:00 UTC";

   US_DataCatalog::Noise ri;
   ri.guid        = "ri-one";
   ri.noiseType   = "ri";
   ri.lastUpdated = "2024-01-01 09:30:00 UTC";

   candidates << oldTi << newTi << laterTi << ri;

   QList< US_DataCatalog::Noise > picked = US_DataPubCatalog::noiseBefore(
         candidates, "2024-01-01 12:00:00 UTC" );

   ASSERT_EQ( picked.size(), 2 );          // one of each type

   QStringList guids;

   for ( int ii = 0; ii < picked.size(); ii++ )
      guids << picked[ ii ].guid;

   guids.sort();

   EXPECT_EQ( guids.join( "," ).toStdString(), "ri-one,ti-new" );
}

TEST_F( DataPubExport, NoiseBeforeLeavesOutEverythingNewerThanTheModel )
{
   QList< US_DataCatalog::Noise > candidates;

   US_DataCatalog::Noise after;
   after.guid        = "ti-after";
   after.noiseType   = "ti";
   after.lastUpdated = "2024-06-01 00:00:00 UTC";

   candidates << after;

   EXPECT_EQ( US_DataPubCatalog::noiseBefore(
                 candidates, "2024-01-01 00:00:00 UTC" ).size(), 0 );

   // the database hands its time stamps back without the UTC suffix
   EXPECT_EQ( US_DataPubCatalog::noiseBefore(
                 candidates, "2024-12-01 00:00:00" ).size(), 1 );
}

// A record whose time stamp cannot be read is a last resort, not a winner.
TEST_F( DataPubExport, NoiseBeforePrefersARecordThatHasATimeStamp )
{
   QList< US_DataCatalog::Noise > candidates;

   US_DataCatalog::Noise undated;
   undated.guid      = "ti-undated";
   undated.noiseType = "ti";

   US_DataCatalog::Noise dated;
   dated.guid        = "ti-dated";
   dated.noiseType   = "ti";
   dated.lastUpdated = "2024-01-01 10:00:00 UTC";

   candidates << undated << dated;

   QList< US_DataCatalog::Noise > picked = US_DataPubCatalog::noiseBefore(
         candidates, "2024-01-01 12:00:00 UTC" );

   ASSERT_EQ( picked.size(), 1 );
   EXPECT_EQ( picked[ 0 ].guid.toStdString(), std::string( "ti-dated" ) );

   // with nothing else to go on, the undated record is used
   QList< US_DataCatalog::Noise > only;
   only << undated;

   ASSERT_EQ( US_DataPubCatalog::noiseBefore(
                 only, "2024-01-01 12:00:00 UTC" ).size(), 1 );
}

/* A time state is no use without its field definitions, so the bundle has
   to carry both files.  The definitions of a run exported from the database
   are fetched into a working directory of their own, and the path of the
   XML used to be built by rewriting ".tmst" wherever it appeared in the
   path of the binary -- which rewrote the working directory's name too, so
   the definitions were written into a directory that did not exist and
   never reached the archive.
*/
TEST_F( DataPubExport, TheTimeStateCarriesItsDefinitions )
{
   ASSERT_TRUE( QFile::exists( timeStatePath( "tmst" ) ) );
   ASSERT_TRUE( QFile::exists( timeStatePath( "xml"  ) ) );

   US_DataPubExporter exporter;
   QString            bundle = root + "/time_state.tar.gz";
   QString            error;

   ASSERT_TRUE( exporter.exportBundle( baseSelection( US_DataPub::ScopeNoise ),
                bundle, error ) ) << error.toStdString();

   QList< US_DataPubEntity > states =
      exporter.manifest().section( US_DataPub::TimeState );

   ASSERT_EQ( states.size(), 1 );

   QString defs = states[ 0 ].attrs.value( "definitionsPayload" );

   ASSERT_FALSE( defs.isEmpty() )
      << "the manifest names no field definitions";

   US_DataPubBundle unpacked;

   ASSERT_TRUE( unpacked.unpack( bundle, error ) ) << error.toStdString();

   QString tmst = unpacked.rootPath() + "/" + states[ 0 ].payload;
   QString xdef = unpacked.rootPath() + "/" + defs;

   EXPECT_TRUE( QFile::exists( tmst ) ) << states[ 0 ].payload.toStdString();
   ASSERT_TRUE( QFile::exists( xdef ) ) << defs.toStdString();

   // US_TimeState looks for the definitions beside the binary, under the
   // same name, so the two have to land together
   EXPECT_EQ( QFileInfo( xdef ).path().toStdString(),
              QFileInfo( tmst ).path().toStdString() );

   EXPECT_EQ( US_DataPubHash::fileHash( xdef ).toStdString(),
              states[ 0 ].attrs.value( "definitionsSha256" ).toStdString() );
}
