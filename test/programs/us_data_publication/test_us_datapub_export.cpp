// test/programs/us_data_publication/test_us_datapub_export.cpp
#include <gtest/gtest.h>

#include "datapub_test_env.h"

#include "us_datapub_catalog.h"
#include "us_datapub_export.h"
#include "us_datapub_import.h"
#include "us_datapub_manifest.h"
#include "us_datapub_hash.h"
#include "us_datapub_bundle.h"
#include "us_datapub_records.h"
#include "us_buffer.h"
#include "mock_us_db2.h"

#include <gmock/gmock.h>

#include <limits>

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

// ------------------------------------------------------------ what is refused

namespace
{
   QString read_text( const QString& path )
   {
      QFile file( path );

      if ( ! file.open( QIODevice::ReadOnly | QIODevice::Text ) )
         return QString();

      return QString::fromUtf8( file.readAll() );
   }

   bool replace_in_file( const QString& path, const QString& from,
                         const QString& to )
   {
      QString text = read_text( path );

      if ( ! text.contains( from ) )  return false;

      text.replace( from, to );

      QFile file( path );

      if ( ! file.open( QIODevice::WriteOnly | QIODevice::Text ) )
         return false;

      file.write( text.toUtf8() );
      file.close();

      return true;
   }

   // The local file of the record with the given GUID
   QString file_with_guid( const QString& dir, const QString& guid )
   {
      QStringList names = QDir( dir ).entryList( QDir::Files );

      for ( int ii = 0; ii < names.size(); ii++ )
         if ( read_text( dir + "/" + names[ ii ] ).contains( guid ) )
            return dir + "/" + names[ ii ];

      return QString();
   }
}

// A run whose project is unknown, exported at project scope, would make an
// empty bundle; that is an error, not an empty file and a success
TEST_F( DataPubExport, AnEmptyBundleIsRefused )
{
   QString expXml = runDir() + "/" + runID + "." + runType + ".xml";
   ASSERT_TRUE( replace_in_file( expXml, projectGUID, QString() ) );

   US_DataPubExporter            exporter;
   US_DataPubExporter::Selection selection =
      baseSelection( US_DataPub::ScopeProject );
   QString                       bundle = root + "/empty.tar.gz";
   QString                       error;

   EXPECT_FALSE( exporter.exportBundle( selection, bundle, error ) );
   EXPECT_TRUE ( error.contains( "nothing to export" ) ) << error.toStdString();
   EXPECT_FALSE( QFile::exists( bundle ) );

   EXPECT_FALSE( exporter.previewManifest( selection, error ) );
}

TEST_F( DataPubExport, ASelectionWithoutRawDataIsRefused )
{
   US_DataPubExporter            exporter;
   US_DataPubExporter::Selection selection =
      baseSelection( US_DataPub::ScopeRawData );
   selection.setRaws( QStringList() );

   QString error;

   EXPECT_FALSE( exporter.exportBundle( selection, root + "/none.tar.gz",
                                        error ) );
   EXPECT_TRUE ( error.contains( "raw data" ) ) << error.toStdString();
}

// A failed load of a solution leaves the solution empty, so a solution
// whose analyte or buffer is missing cannot be exported as if it were fine
TEST_F( DataPubExport, ASolutionWithoutItsAnalyteFailsTheExport )
{
   QString analyte = file_with_guid( dataDir( "analytes" ), analyteGUID );
   ASSERT_FALSE( analyte.isEmpty() );
   ASSERT_TRUE ( QFile::remove( analyte ) );

   US_DataPubExporter exporter;
   QString            error;

   EXPECT_FALSE( exporter.exportBundle( baseSelection( US_DataPub::ScopeRawData ),
                 root + "/no_analyte.tar.gz", error ) );
   EXPECT_TRUE ( error.contains( solutionGUID ) ) << error.toStdString();
   EXPECT_TRUE ( error.contains( "analytes could not be read" ) )
      << error.toStdString();
}

TEST_F( DataPubExport, ASolutionWithoutItsBufferFailsTheExport )
{
   QString buffer = file_with_guid( dataDir( "buffers" ), bufferGUID );
   ASSERT_FALSE( buffer.isEmpty() );
   ASSERT_TRUE ( QFile::remove( buffer ) );

   US_DataPubExporter exporter;
   QString            error;

   EXPECT_FALSE( exporter.exportBundle( baseSelection( US_DataPub::ScopeRawData ),
                 root + "/no_buffer.tar.gz", error ) );
   EXPECT_TRUE ( error.contains( "buffer could not be read" ) )
      << error.toStdString();
}

TEST_F( DataPubExport, ARawTripleWithoutASolutionFailsTheExport )
{
   QString expXml = runDir() + "/" + runID + "." + runType + ".xml";
   ASSERT_TRUE( replace_in_file( expXml, "guid=\"" + solutionGUID + "\"",
                                 "guid=\"\"" ) );

   US_DataPubExporter exporter;
   QString            error;

   EXPECT_FALSE( exporter.exportBundle( baseSelection( US_DataPub::ScopeRawData ),
                 root + "/no_solution.tar.gz", error ) );
   EXPECT_TRUE ( error.contains( "names no solution" ) ) << error.toStdString();
}

TEST_F( DataPubExport, AModelGuidThatMatchesNothingIsAnError )
{
   US_DataPubExporter            exporter;
   US_DataPubExporter::Selection selection =
      baseSelection( US_DataPub::ScopeNoise );
   QString                       ghost = US_Util::new_guid();
   selection.setModels( QStringList() << modelGUID << ghost );

   QString error;

   EXPECT_FALSE( exporter.exportBundle( selection, root + "/ghost.tar.gz",
                                        error ) );
   EXPECT_TRUE ( error.contains( ghost ) ) << error.toStdString();
}

TEST_F( DataPubExport, ANoiseGuidThatMatchesNothingIsAnError )
{
   US_DataPubExporter            exporter;
   US_DataPubExporter::Selection selection =
      baseSelection( US_DataPub::ScopeNoise );
   QString                       ghost = US_Util::new_guid();
   selection.setNoises( QStringList() << ghost );

   QString error;

   EXPECT_FALSE( exporter.previewManifest( selection, error ) );
   EXPECT_TRUE ( error.contains( ghost ) ) << error.toStdString();
}

TEST_F( DataPubExport, ARawGuidThatMatchesNothingIsAnError )
{
   US_DataPubExporter            exporter;
   US_DataPubExporter::Selection selection =
      baseSelection( US_DataPub::ScopeRawData );
   QString                       ghost = US_Util::new_guid();
   selection.setRaws( QStringList() << rawGUID << ghost );

   QString error;

   EXPECT_FALSE( exporter.previewManifest( selection, error ) );
   EXPECT_TRUE ( error.contains( ghost ) ) << error.toStdString();
}

TEST_F( DataPubExport, ARunOfAnotherProjectIsRefused )
{
   US_DataPubExporter            exporter;
   US_DataPubExporter::Selection selection =
      baseSelection( US_DataPub::ScopeRawData );
   selection.projectGUID = US_Util::new_guid();

   QString error;

   EXPECT_FALSE( exporter.previewManifest( selection, error ) );
   EXPECT_TRUE ( error.contains( selection.projectGUID ) )
      << error.toStdString();
}

TEST_F( DataPubExport, ARunNamedTwiceIsExportedOnce )
{
   US_DataPubExporter            exporter;
   US_DataPubExporter::Selection selection =
      baseSelection( US_DataPub::ScopeRawData );
   selection.runIDs << runID;

   QString error;

   ASSERT_TRUE( exporter.previewManifest( selection, error ) )
      << error.toStdString();
   EXPECT_EQ( exporter.manifest().count( US_DataPub::Experiment ), 1 );
   EXPECT_EQ( exporter.manifest().count( US_DataPub::RawData ),    1 );
}

// ------------------------------------------------------- what the bundle holds

TEST_F( DataPubExport, EveryPayloadHasADigestAndTheManifestHasOne )
{
   US_DataPubExporter exporter;
   QString            bundle = root + "/all_digests.tar.gz";
   QString            error;

   ASSERT_TRUE( exporter.exportBundle( baseSelection( US_DataPub::ScopeNoise ),
                bundle, error ) ) << error.toStdString();

   US_DataPubBundle unpacked;
   ASSERT_TRUE( unpacked.unpack( bundle, error ) ) << error.toStdString();

   QList< US_DataPubEntity > entities = exporter.manifest().all();
   int                       extras   = 0;

   for ( int ii = 0; ii < entities.size(); ii++ )
   {
      QStringList keys = entities[ ii ].payloadKeys();

      for ( int jj = 0; jj < keys.size(); jj++ )
      {
         QString path   = unpacked.rootPath() + "/"
                          + entities[ ii ].field( keys[ jj ] );
         QString digest = entities[ ii ].field(
                          US_DataPub::digestKeyOf( keys[ jj ] ) );

         ASSERT_FALSE( digest.isEmpty() ) << keys[ jj ].toStdString();
         EXPECT_EQ   ( US_DataPubHash::fileHash( path ), digest )
            << path.toStdString();

         if ( keys[ jj ] != "payload" )  extras++;
      }
   }

   EXPECT_GE( extras, 1 );         // the time state's definitions at least

   // manifest.sha256 holds the digest of manifest.yaml, as sha256sum has it
   QString line = read_text( unpacked.rootPath() + "/"
                             + US_DataPubExporter::manifestDigestName() );
   QString mdig = US_DataPubHash::fileHash( unpacked.rootPath() + "/"
                                            + US_DataPubBundle::manifestName() );

   EXPECT_EQ( line, mdig + "  " + US_DataPubBundle::manifestName() + "\n" );
}

// Sections that were not exported are not in the manifest at all
TEST_F( DataPubExport, OnlyExportedSectionsAreListed )
{
   US_DataPubExporter            exporter;
   US_DataPubExporter::Selection selection =
      baseSelection( US_DataPub::ScopeRawData );
   selection.includeTimeState = false;

   QString error;

   ASSERT_TRUE( exporter.previewManifest( selection, error ) )
      << error.toStdString();

   QString yaml = exporter.manifest().toYaml();

   EXPECT_TRUE ( yaml.contains( "\nsolutions:" ) ) << yaml.toStdString();
   EXPECT_FALSE( yaml.contains( "\nedits:" ) );
   EXPECT_FALSE( yaml.contains( "\nmodels:" ) );
   EXPECT_FALSE( yaml.contains( "\ntimeState:" ) );
}

// The time state of a run is a record of its own
TEST_F( DataPubExport, TheTimeStateHasAnIdentityOfItsOwn )
{
   US_DataPubExporter exporter;
   QString            error;

   ASSERT_TRUE( exporter.previewManifest(
                baseSelection( US_DataPub::ScopeRawData ), error ) )
      << error.toStdString();

   QList< US_DataPubEntity > states =
      exporter.manifest().section( US_DataPub::TimeState );

   ASSERT_EQ( states.size(), 1 );
   EXPECT_NE( states[ 0 ].guid, expGUID );
   EXPECT_EQ( states[ 0 ].guid,
              US_DataPub::derivedGuid( "timeState:" + expGUID ) );
   EXPECT_EQ( states[ 0 ].depend( US_DataPub::Experiment ), expGUID );
}

// A .tmst without the definitions that say how to read it is no time state
TEST_F( DataPubExport, ATimeStateWithoutDefinitionsFailsTheExport )
{
   ASSERT_TRUE( QFile::remove( timeStatePath( "xml" ) ) );

   US_DataPubExporter            exporter;
   US_DataPubExporter::Selection selection =
      baseSelection( US_DataPub::ScopeRawData );
   QString                       error;

   EXPECT_FALSE( exporter.exportBundle( selection, root + "/no_defs.tar.gz",
                                        error ) );
   EXPECT_TRUE ( error.contains( "definitions" ) ) << error.toStdString();

   // ... unless the time state is left out on purpose
   selection.includeTimeState = false;
   EXPECT_TRUE ( exporter.exportBundle( selection, root + "/no_ts.tar.gz",
                                        error ) ) << error.toStdString();
   EXPECT_EQ   ( exporter.manifest().count( US_DataPub::TimeState ), 0 );
}

// The RI intensity profile of a run travels with its experiment
TEST_F( DataPubExport, TheRIProfileTravelsWithTheExperiment )
{
   QString ripPath = runDir() + "/" + runID + ".RIProfile.xml";
   QFile   rip( ripPath );
   ASSERT_TRUE( rip.open( QIODevice::WriteOnly | QIODevice::Text ) );
   rip.write( "<?xml version=\"1.0\"?>\n<!DOCTYPE US_RIProfile>\n"
              "<US_RIProfile version=\"1.0\"><RI value=\"0.5\"/>"
              "</US_RIProfile>\n" );
   rip.close();

   US_DataPubExporter exporter;
   QString            bundle = root + "/ri.tar.gz";
   QString            error;

   ASSERT_TRUE( exporter.exportBundle( baseSelection( US_DataPub::ScopeRawData ),
                bundle, error ) ) << error.toStdString();

   US_DataPubEntity exper;
   ASSERT_TRUE( exporter.manifest().entity( US_DataPub::Experiment, expGUID,
                                            exper ) );

   QString payload = exper.attrs.value( "riProfilePayload" );
   EXPECT_EQ( payload, "experiment/" + runID + ".RIProfile.xml" );

   US_DataPubBundle unpacked;
   ASSERT_TRUE( unpacked.unpack( bundle, error ) ) << error.toStdString();
   EXPECT_EQ( US_DataPubHash::fileHash( unpacked.rootPath() + "/" + payload ),
              exper.attrs.value( "riProfileSha256" ) );
   EXPECT_EQ( US_DataPubHash::fileHash( unpacked.rootPath() + "/" + payload ),
              US_DataPubHash::fileHash( ripPath ) );
}

// A buffer names its components by ID only; the bundle carries what those
// IDs stand for, so that another installation can make sense of them
TEST_F( DataPubExport, BufferComponentsTravelWithTheBuffer )
{
   QMap< QString, US_BufferComponent > table;
   US_BufferComponent::getAllFromHD( table );
   ASSERT_TRUE( table.contains( "1" ) ) << "no local buffer component table";

   US_Buffer buffer;
   buffer.GUID        = bufferGUID;
   buffer.bufferID    = "3";
   buffer.description = "Demo buffer";
   buffer.density     = 1.0;
   buffer.viscosity   = 1.002;
   buffer.pH          = 7.0;
   buffer.component     << table.value( "1" );
   buffer.componentIDs  << "1";
   buffer.concentration << 150.0;

   QString bfile = file_with_guid( dataDir( "buffers" ), bufferGUID );
   ASSERT_FALSE( bfile.isEmpty() );
   ASSERT_TRUE ( buffer.writeToDisk( bfile ) );

   US_DataPubExporter exporter;
   QString            bundle = root + "/components.tar.gz";
   QString            error;

   ASSERT_TRUE( exporter.exportBundle( baseSelection( US_DataPub::ScopeRawData ),
                bundle, error ) ) << error.toStdString();

   US_DataPubEntity entity;
   ASSERT_TRUE( exporter.manifest().entity( US_DataPub::Buffer, bufferGUID,
                                            entity ) );
   EXPECT_EQ( entity.attrs.value( "components" ), "1" );

   US_DataPubBundle unpacked;
   ASSERT_TRUE( unpacked.unpack( bundle, error ) ) << error.toStdString();

   // The buffer file still names its component ...
   US_Buffer staged;
   ASSERT_TRUE( staged.readFromDisk( unpacked.rootPath() + "/"
                                     + entity.payload ) );
   ASSERT_EQ( staged.componentIDs, QStringList() << "1" );
   EXPECT_DOUBLE_EQ( staged.concentration.value( 0 ), 150.0 );

   // ... and the bundle says what the component is
   QMap< QString, US_BufferComponent > carried;
   QString comps = unpacked.rootPath() + "/"
                   + entity.attrs.value( "componentsPayload" );
   ASSERT_TRUE( US_DataPubRecords::readBufferComponents( comps, carried ) );
   ASSERT_TRUE( carried.contains( "1" ) );
   EXPECT_EQ( carried[ "1" ].name, table[ "1" ].name );
   EXPECT_EQ( carried[ "1" ].unit, table[ "1" ].unit );

   for ( int ii = 0; ii < 6; ii++ )
   {
      EXPECT_DOUBLE_EQ( carried[ "1" ].dens_coeff[ ii ],
                        table[ "1" ].dens_coeff[ ii ] );
      EXPECT_DOUBLE_EQ( carried[ "1" ].visc_coeff[ ii ],
                        table[ "1" ].visc_coeff[ ii ] );
   }

   EXPECT_EQ( US_DataPubHash::fileHash( comps ),
              entity.attrs.value( "componentsSha256" ) );
}

// Every analyte of a solution is a dependency of it, not a remark
TEST_F( DataPubExport, AnalytesAreDependenciesOfTheirSolution )
{
   US_DataPubExporter exporter;
   QString            error;

   ASSERT_TRUE( exporter.previewManifest(
                baseSelection( US_DataPub::ScopeRawData ), error ) )
      << error.toStdString();

   US_DataPubEntity solution;
   ASSERT_TRUE( exporter.manifest().entity( US_DataPub::Solution,
                                            solutionGUID, solution ) );
   EXPECT_EQ( solution.dependsOn( US_DataPub::Analyte ),
              QStringList() << analyteGUID );
   EXPECT_EQ( solution.depend( US_DataPub::Buffer ), bufferGUID );
}

// Only what the selected triples need goes into the bundle: the solution
// and centerpiece of a deselected triple stay out, and so does its dataset
// in the run's experiment XML
TEST_F( DataPubExport, ADeselectedTripleLeavesNoTrace )
{
   addSecondTriple();

   US_DataPubExporter            exporter;
   US_DataPubExporter::Selection selection =
      baseSelection( US_DataPub::ScopeRawData );
   QString                       bundle = root + "/one_of_two.tar.gz";
   QString                       error;

   // Both triples, to begin with
   ASSERT_TRUE( exporter.previewManifest( selection, error ) )
      << error.toStdString();
   ASSERT_EQ( exporter.manifest().count( US_DataPub::RawData ),     2 );
   ASSERT_EQ( exporter.manifest().count( US_DataPub::Solution ),    2 );
   ASSERT_EQ( exporter.manifest().count( US_DataPub::Centerpiece ), 2 );

   selection.setRaws( QStringList() << rawGUID );

   ASSERT_TRUE( exporter.exportBundle( selection, bundle, error ) )
      << error.toStdString();

   const US_DataPubManifest& mani = exporter.manifest();
   EXPECT_EQ   ( mani.count( US_DataPub::RawData ),     1 );
   EXPECT_EQ   ( mani.count( US_DataPub::Solution ),    1 );
   EXPECT_EQ   ( mani.count( US_DataPub::Centerpiece ), 1 );
   EXPECT_FALSE( mani.contains( US_DataPub::Solution, solutionGUID2 ) );

   US_DataPubEntity exper;
   ASSERT_TRUE( mani.entity( US_DataPub::Experiment, expGUID, exper ) );

   US_DataPubBundle unpacked;
   ASSERT_TRUE( unpacked.unpack( bundle, error ) ) << error.toStdString();

   QString xml = read_text( unpacked.rootPath() + "/" + exper.payload );
   EXPECT_TRUE ( xml.contains( rawGUID ) );
   EXPECT_FALSE( xml.contains( rawGUID2 ) );
   EXPECT_FALSE( xml.contains( solutionGUID2 ) );
   EXPECT_TRUE ( xml.contains( "Demo experiment label" ) );

   // The trimmed XML is still an experiment record the reader understands
   US_DataPubCatalog::ExpInfo info;
   ASSERT_TRUE( info.readFromFile( unpacked.rootPath() + "/" + exper.payload ) );
   EXPECT_EQ( info.expGUID, expGUID );
   EXPECT_EQ( info.tripleSolutionGUIDs.value( "1/A" ), solutionGUID );
}

TEST_F( DataPubExport, CenterpiecesAndCalibrationsHaveValidGuids )
{
   US_DataPubExporter exporter;
   QString            error;

   ASSERT_TRUE( exporter.previewManifest(
                baseSelection( US_DataPub::ScopeRawData ), error ) )
      << error.toStdString();

   QList< US_DataPubEntity > found =
      exporter.manifest().section( US_DataPub::Centerpiece )
      + exporter.manifest().section( US_DataPub::RotorCalibration );

   ASSERT_GE( found.size(), 2 );

   for ( int ii = 0; ii < found.size(); ii++ )
      EXPECT_FALSE( QUuid( found[ ii ].guid ).isNull() )
         << found[ ii ].guid.toStdString();
}

// A rotor calibration the run names but nobody can read is not silently
// dropped: the run would arrive without what its data was corrected with
TEST_F( DataPubExport, AnUnreadableCalibrationFailsTheExport )
{
   QString cal = file_with_guid( dataDir( "rotors" ), calGUID );
   ASSERT_FALSE( cal.isEmpty() );
   ASSERT_TRUE ( QFile::remove( cal ) );

   US_DataPubExporter exporter;
   QString            error;

   EXPECT_FALSE( exporter.previewManifest(
                 baseSelection( US_DataPub::ScopeRawData ), error ) );
   EXPECT_TRUE ( error.contains( "rotor calibration" ) ) << error.toStdString();
}

TEST_F( DataPubExport, AnUnknownCenterpieceFailsTheExport )
{
   QString expXml = runDir() + "/" + runID + "." + runType + ".xml";
   ASSERT_TRUE( replace_in_file( expXml, "<centerpiece id=\"2\"/>",
                                 "<centerpiece id=\"9999\"/>" ) );

   US_DataPubExporter exporter;
   QString            error;

   EXPECT_FALSE( exporter.previewManifest(
                 baseSelection( US_DataPub::ScopeRawData ), error ) );
   EXPECT_TRUE ( error.contains( "9999" ) ) << error.toStdString();
}

TEST_F( DataPubExport, StepsAreAnnouncedBeforeTheFirstRecord )
{
   US_DataPubExporter exporter;
   QList< int >       events;       // > 0: steps announced, < 0: step done

   QObject::connect( &exporter, &US_DataPubExporter::steps,
                     [ &events ]( int steps ) { events << steps; } );
   QObject::connect( &exporter, &US_DataPubExporter::stepDone,
                     [ &events ]( int step )  { events << -step; } );

   QString error;
   ASSERT_TRUE( exporter.previewManifest(
                baseSelection( US_DataPub::ScopeNoise ), error ) )
      << error.toStdString();

   ASSERT_FALSE( events.isEmpty() );
   EXPECT_GT( events.first(), 0 );
   EXPECT_EQ( events.last(), exporter.manifest().total() );
}

// ------------------------------------------------- database queries, mocked

using ::testing::_;
using ::testing::An;
using ::testing::NiceMock;
using ::testing::Return;

namespace
{
   US_DataPubCatalog::Run db_run( void )
   {
      US_DataPubCatalog::Run run;
      run.id    = "31";
      run.runID = "demo_run";
      run.guid  = "aaaaaaaa-0000-0000-0000-000000000031";
      return run;
   }
}

class DataPubCatalogDb : public ::testing::Test
{
   protected:
      NiceMock< US_DB2_Mock > db;
      US_DataPubCatalog       catalog;
      QTemporaryDir           work;

      void SetUp() override
      {
         ASSERT_TRUE( work.isValid() );
         catalog.attachDb( &db );
      }

      // One row of get_experiment_timestate: ID, file, definitions, ...
      void timeStateRow( const QString& fname, const QString& xdefs )
      {
         ON_CALL( db, lastErrno() ).WillByDefault( Return( IUS_DB2::OK ) );
         EXPECT_CALL( db, next() ).WillOnce( Return( true ) );
         ON_CALL( db, value( 0u ) ).WillByDefault( Return( QVariant( "9" ) ) );
         ON_CALL( db, value( 1u ) ).WillByDefault( Return( QVariant( fname ) ) );
         ON_CALL( db, value( 2u ) ).WillByDefault( Return( QVariant( xdefs ) ) );
      }
};

// No time state is the normal case; it is told apart from a lookup failure
TEST_F( DataPubCatalogDb, NoTimeStateIsNotAFailure )
{
   ON_CALL( db, lastErrno() ).WillByDefault( Return( IUS_DB2::NOROWS ) );

   QString tmst, xdef, id, error;

   EXPECT_EQ( catalog.timeState( db_run(), work.path(), true, tmst, xdef, id,
                                 error ),
              US_DataPubCatalog::TimeStateNone );
   EXPECT_TRUE( error.isEmpty() );
}

TEST_F( DataPubCatalogDb, AFailedTimeStateLookupIsAFailure )
{
   ON_CALL( db, lastErrno() ).WillByDefault( Return( IUS_DB2::DBERROR ) );
   ON_CALL( db, lastError() ).WillByDefault( Return( QString( "gone" ) ) );

   QString tmst, xdef, id, error;

   EXPECT_EQ( catalog.timeState( db_run(), work.path(), true, tmst, xdef, id,
                                 error ),
              US_DataPubCatalog::TimeStateFailed );
   EXPECT_TRUE( error.contains( "gone" ) ) << error.toStdString();
}

TEST_F( DataPubCatalogDb, ATimeStateWithoutDefinitionsIsAFailure )
{
   timeStateRow( "demo_run.time_state.tmst", QString() );

   QString tmst, xdef, id, error;

   EXPECT_EQ( catalog.timeState( db_run(), work.path(), true, tmst, xdef, id,
                                 error ),
              US_DataPubCatalog::TimeStateFailed );
   EXPECT_TRUE( error.contains( "definitions" ) ) << error.toStdString();
}

TEST_F( DataPubCatalogDb, ATimeStateIsFetchedWithItsDefinitions )
{
   // The stored file name is not trusted to name a place, nor the file
   timeStateRow( "../../elsewhere.tmst", "<TimeState/>" );

   EXPECT_CALL( db, readBlobFromDB( _, QString( "download_timestate" ), 9 ) )
      .WillOnce( [] ( const QString& path, const QString&, int )
                 {
                    QFile file( path );
                    file.open( QIODevice::WriteOnly );
                    file.write( "USTS" );
                    file.close();
                    return int( IUS_DB2::OK );
                 } );

   QString tmst, xdef, id, error;

   ASSERT_EQ( catalog.timeState( db_run(), work.path(), true, tmst, xdef, id,
                                 error ),
              US_DataPubCatalog::TimeStateFound ) << error.toStdString();

   EXPECT_EQ( id, "9" );
   EXPECT_EQ( QFileInfo( tmst ).absolutePath(), QDir( work.path() ).absolutePath() );
   EXPECT_EQ( QFileInfo( tmst ).fileName(), "demo_run.time_state.tmst" );
   EXPECT_EQ( QFileInfo( xdef ).fileName(), "demo_run.time_state.xml" );
   EXPECT_TRUE( QFile::exists( tmst ) );

   QFile defs( xdef );
   ASSERT_TRUE( defs.open( QIODevice::ReadOnly ) );
   EXPECT_EQ( defs.readAll(), QByteArray( "<TimeState/>" ) );
}

// An import only takes a run's file whose name starts with "<runID>.", so
// a time state the database keeps under another name -- another run's, a
// plain one -- is exported under the run's own
TEST_F( DataPubCatalogDb, ATimeStateIsNamedAfterItsRun )
{
   QStringList stored;
   stored << "other_run.time_state.tmst" << "timestate.tmst"
          << "demo_run.time_state.tmst";

   for ( int ii = 0; ii < stored.size(); ii++ )
   {
      ::testing::Mock::VerifyAndClearExpectations( &db );
      timeStateRow( stored[ ii ], "<TimeState/>" );

      QString tmst, xdef, id, error;

      ASSERT_EQ( catalog.timeState( db_run(), work.path(), false, tmst, xdef,
                                    id, error ),
                 US_DataPubCatalog::TimeStateFound ) << error.toStdString();
      EXPECT_EQ( QFileInfo( tmst ).fileName(), "demo_run.time_state.tmst" )
         << stored[ ii ].toStdString();
   }
}

TEST_F( DataPubCatalogDb, AFailedTimeStateDownloadIsAFailure )
{
   timeStateRow( "demo_run.time_state.tmst", "<TimeState/>" );

   EXPECT_CALL( db, readBlobFromDB( _, _, 9 ) )
      .WillOnce( Return( int( IUS_DB2::DBERROR ) ) );

   QString tmst, xdef, id, error;

   EXPECT_EQ( catalog.timeState( db_run(), work.path(), true, tmst, xdef, id,
                                 error ),
              US_DataPubCatalog::TimeStateFailed );
   EXPECT_TRUE( error.contains( "downloaded" ) ) << error.toStdString();
}

// A project list that could not be read is an error, not a shorter list
TEST_F( DataPubCatalogDb, AProjectThatCannotBeReadIsAnError )
{
   EXPECT_CALL( db, lastErrno() )
      .WillOnce( Return( IUS_DB2::OK ) )           // get_project_desc
      .WillRepeatedly( Return( IUS_DB2::DBERROR ) );
   EXPECT_CALL( db, next() )
      .WillOnce( Return( true ) )
      .WillRepeatedly( Return( false ) );
   ON_CALL( db, value( 0u ) ).WillByDefault( Return( QVariant( "5" ) ) );

   QString error;
   QList< US_DataPubCatalog::Project > list = catalog.projects( error );

   EXPECT_TRUE ( list.isEmpty() );
   EXPECT_FALSE( error.isEmpty() );
}

TEST_F( DataPubCatalogDb, AFailedExperimentQueryIsAnError )
{
   ON_CALL( db, lastErrno() ).WillByDefault( Return( IUS_DB2::DBERROR ) );
   ON_CALL( db, lastError() ).WillByDefault( Return( QString( "lost" ) ) );

   US_DataPubCatalog::ExpInfo info;
   QString                    error;

   EXPECT_FALSE( catalog.expInfo( db_run(), info, error ) );
   EXPECT_TRUE ( error.contains( "lost" ) ) << error.toStdString();
}

// The cell table gives channels as an index into the channel letters
TEST_F( DataPubCatalogDb, CellAssignmentsUseTheChannelLetters )
{
   ON_CALL( db, lastErrno() ).WillByDefault( Return( IUS_DB2::OK ) );

   QString current;

   ON_CALL( db, query( An< const QStringList& >() ) )
      .WillByDefault( [ &current ]( const QStringList& args )
                      { current = args.value( 0 ); } );

   int rows = 0;

   ON_CALL( db, next() ).WillByDefault( [ &current, &rows ]()
   {
      if ( current == "all_cell_experiments" )  return ( rows++ < 1 );
      return ( current == "get_experiment_info_by_runID" );
   } );

   ON_CALL( db, value( _ ) ).WillByDefault( [ &current ]( unsigned col )
   {
      if ( current == "all_cell_experiments" )
      {
         if ( col == 2 )  return QVariant( "3" );    // cell
         if ( col == 3 )  return QVariant( 2 );      // channel index: B
         if ( col == 4 )  return QVariant( "4" );    // centerpiece
      }

      if ( current == "get_experiment_info_by_runID"  &&  col == 7 )
         return QVariant( "12" );                    // a calibration ID

      return QVariant( "1" );
   } );

   US_DataPubCatalog::ExpInfo info;
   QString                    error;

   ASSERT_TRUE( catalog.expInfo( db_run(), info, error ) )
      << error.toStdString();
   EXPECT_EQ( info.tripleCenterpieceIDs.value( "3/B" ), "4" );
   EXPECT_EQ( info.centerpieceIDs, QStringList() << "4" );
   EXPECT_EQ( info.calibrationID, "12" );
}

// A run whose record names no calibration is not given the rotor's first
TEST_F( DataPubCatalogDb, ACalibrationIsNotAssumed )
{
   ON_CALL( db, lastErrno() ).WillByDefault( Return( IUS_DB2::OK ) );

   QString     current;
   QStringList asked;

   ON_CALL( db, query( An< const QStringList& >() ) )
      .WillByDefault( [ &current, &asked ]( const QStringList& args )
                      { current = args.value( 0 );  asked << current; } );

   ON_CALL( db, next() ).WillByDefault( [ &current ]()
   {
      return ( current == "get_experiment_info_by_runID"  ||
               current == "get_rotor_calibration_profiles" );
   } );

   ON_CALL( db, value( _ ) ).WillByDefault( [ &current ]( unsigned col )
   {
      if ( current == "get_experiment_info_by_runID"  &&  col == 7 )
         return QVariant( "0" );                     // no calibration
      if ( current == "get_rotor_calibration_profiles" )
         return QVariant( "44" );                    // the rotor's first
      return QVariant( "1" );
   } );

   US_DataPubCatalog::ExpInfo info;
   QString                    error;

   ASSERT_TRUE( catalog.expInfo( db_run(), info, error ) )
      << error.toStdString();
   EXPECT_TRUE ( info.calibrationID.isEmpty() )
      << info.calibrationID.toStdString();
   EXPECT_FALSE( asked.contains( "get_rotor_calibration_profiles" ) );
}

// A NULL rotor or project column reads as "", which a server in strict mode
// refuses as an integer argument; a record that names none is not looked up
TEST_F( DataPubCatalogDb, ANullRotorOrProjectIsNotLookedUp )
{
   ON_CALL( db, lastErrno() ).WillByDefault( Return( IUS_DB2::OK ) );

   QString                    current;
   QList< QStringList >       asked;

   ON_CALL( db, query( An< const QStringList& >() ) )
      .WillByDefault( [ &current, &asked ]( const QStringList& args )
                      { current = args.value( 0 );  asked << args; } );

   ON_CALL( db, next() ).WillByDefault( [ &current ]()
   {
      return current == "get_experiment_info_by_runID";
   } );

   ON_CALL( db, value( _ ) ).WillByDefault( [ &current ]( unsigned col )
   {
      if ( col == 0  ||  col == 6 )  return QVariant( QString() );  // NULL
      return QVariant( "1" );
   } );

   US_DataPubCatalog::ExpInfo info;
   QString                    error;

   ASSERT_TRUE( catalog.expInfo( db_run(), info, error ) )
      << error.toStdString();

   for ( int ii = 0; ii < asked.size(); ii++ )
   {
      EXPECT_NE( asked[ ii ].value( 0 ), QString( "get_rotor_info" ) );
      EXPECT_NE( asked[ ii ].value( 0 ), QString( "get_project_info" ) );

      // No procedure argument may be an empty string where a number goes
      for ( int jj = 1; jj < asked[ ii ].size(); jj++ )
         EXPECT_FALSE( asked[ ii ][ jj ].isEmpty() )
            << asked[ ii ].join( " " ).toStdString();
   }

   EXPECT_TRUE( info.rotorID.isEmpty() );
   EXPECT_TRUE( info.projectGUID.isEmpty() );
}

// ------------------------------------------- export steps, one at a time

/**
 * Drives the steps of an export directly, so that a database export can be
 * run against the mock without scripting the whole record chain.
 */
class US_DataPubExporterTest
{
   public:
      static void attach( US_DataPubExporter& ex, IUS_DB2* db )
      {
         ex.catalog.attachDb( db );
      }

      static bool stage( US_DataPubExporter& ex, QString& error )
      {
         ex.stage_payloads = true;
         return ex.bundle.createStaging( error );
      }

      static void setScope( US_DataPubExporter& ex, US_DataPub::Scope scope )
      {
         ex.exp_scope = scope;
      }

      static void askForNoises( US_DataPubExporter& ex, const QStringList& g )
      {
         ex.explicit_noises = g;
      }

      static void setSolution( US_DataPubExporter& ex, const QString& raw,
                               const QString& solution )
      {
         ex.raw_solutions.insert( raw, solution );
      }

      static void add( US_DataPubExporter& ex, const US_DataPubEntity& e )
      {
         ex.mani.add( e );
      }

      static bool addExperiment( US_DataPubExporter& ex,
                                 const US_DataPubCatalog::Run& run,
                                 QString& error )
      {
         return ex.addExperiment( run, US_DataPubCatalog::ExpInfo(), error );
      }

      static bool addRawData( US_DataPubExporter& ex,
                              const US_DataPubCatalog::Run& run,
                              QString& error )
      {
         return ex.addRawData( run, US_DataPubCatalog::ExpInfo(), error );
      }

      static bool addEdits( US_DataPubExporter& ex,
                            const US_DataPubCatalog::Run& run, QString& error )
      {
         return ex.addEdits( run, error );
      }

      static bool addNoises( US_DataPubExporter& ex,
                             const QList< US_DataPubCatalog::Noise >& noises,
                             QString& error )
      {
         return ex.addNoises( noises, error );
      }

      static QString staged( US_DataPubExporter& ex, const QString& rel )
      {
         return ex.bundle.stagingPath() + "/" + rel;
      }

      static bool addSolution( US_DataPubExporter& ex, US_Solution& solution,
                               const QString& expGUID, QString& error )
      {
         return ex.addSolution( solution, expGUID, error );
      }

      static bool checkStagedTree( US_DataPubExporter& ex, QString& error )
      {
         return ex.checkStagedTree( error );
      }

      static const US_DataPubManifest& manifest( US_DataPubExporter& ex )
      {
         return ex.mani;
      }

      static bool openDisk( US_DataPubExporter& ex )
      {
         QString error;
         return ex.catalog.open( false, QString(), error );
      }

      static bool addFittedModels( US_DataPubExporter& ex,
                                   QList< US_DataPubCatalog::Model >& models,
                                   QList< US_DataPubCatalog::Noise >& noises,
                                   QString& error )
      {
         return ex.addFittedModels( models, noises, error );
      }

      static bool checkExperimentXml( const QString& path,
                                      const QString& runID,
                                      const QString& guid, QString& error )
      {
         return US_DataPubExporter::checkExperimentXml( path, runID, guid,
                                                        error );
      }

      static bool runFault( US_DataPubExporter& ex )
      {
         return ex.run_fault;
      }
};

/**
 * A database that answers each procedure with the rows set up for it,
 * and downloads blobs from the files set up for each record ID.
 */
class ScriptedDb
{
   public:
      NiceMock< US_DB2_Mock >               mock;
      QMap< QString, QList< QVariantList > > rows;
      QMap< int, QString >                  blobs;
      QStringList                           asked;

      ScriptedDb()
      {
         ON_CALL( mock, isConnected() ).WillByDefault( Return( true ) );

         ON_CALL( mock, query( An< const QStringList& >() ) )
            .WillByDefault( [ this ]( const QStringList& q )
            {
               current = q.value( 0 );
               asked << current;
               cursor  = 0;
            } );

         ON_CALL( mock, lastErrno() ).WillByDefault( [ this ]()
         {
            return rows.value( current ).isEmpty() ? int( IUS_DB2::NOROWS )
                                                   : int( IUS_DB2::OK );
         } );

         ON_CALL( mock, next() ).WillByDefault( [ this ]()
         {
            return ++cursor <= rows.value( current ).size();
         } );

         ON_CALL( mock, value( _ ) ).WillByDefault( [ this ]( unsigned col )
         {
            QList< QVariantList > list = rows.value( current );

            if ( cursor < 1  ||  cursor > list.size() )  return QVariant();

            return list[ cursor - 1 ].value( int( col ) );
         } );

         ON_CALL( mock, readBlobFromDB( _, _, _ ) )
            .WillByDefault( [ this ]( const QString& path, const QString&,
                                      int id )
            {
               QFile::remove( path );

               return ( blobs.contains( id )  &&
                        QFile::copy( blobs.value( id ), path ) )
                      ? int( IUS_DB2::OK ) : int( IUS_DB2::DBERROR );
            } );
      }

      IUS_DB2* db() { return &mock; }

   private:
      QString current;
      int     cursor = 0;
};

class DataPubExportSteps : public DataPubTestEnv
{
   protected:
      ScriptedDb         sdb;
      US_DataPubExporter exporter;

      void SetUp() override
      {
         DataPubTestEnv::SetUp();

         US_DataPubExporterTest::attach( exporter, sdb.db() );

         QString error;
         ASSERT_TRUE( US_DataPubExporterTest::stage( exporter, error ) )
            << error.toStdString();
      }

      // One raw triple of the scratch run, as the database catalog lists it
      US_DataPubCatalog::Run dbRun( void )
      {
         US_DataPubCatalog::Run run;
         run.id      = "31";
         run.runID   = runID;
         run.guid    = expGUID;
         run.runType = runType;

         US_DataPubCatalog::Raw raw;
         raw.id         = "5";
         raw.guid       = rawGUID;
         raw.filename   = rawFile;
         raw.triple     = "1.A.280";
         raw.dataType   = runType;
         raw.solutionID = "11";

         US_DataPubCatalog::Edit edit;
         edit.id       = "6";
         edit.guid     = editGUID;
         edit.filename = editFile;
         edit.editID   = editID;
         edit.triple   = "1.A.280";
         raw.edits << edit;

         run.raws << raw;

         return run;
      }

      // A row of get_experiment_info_by_runID
      QVariantList experimentRow( const QString& optics, const QString& rip )
      {
         QVariantList row;
         row << 7 << 31 << expGUID << 1 << 1 << 1 << 2 << 21 << "velocity"
             << "20.0" << "label" << "" << "" << "2024-01-01" << 1
             << optics << rip;
         return row;
      }
};

// A database export reads the RI intensity profile from the record and
// stages it; it does not leave a copy in the user's results directory, or
// touch the one that is there
TEST_F( DataPubExportSteps, TheRIProfileIsNotCachedInTheResultsDirectory )
{
   QByteArray rip( "<?xml version=\"1.0\"?>\n<US_RIProfile version=\"1.0\">"
                   "<RI value=\"0.5\"/><RI value=\"0.625\"/>"
                   "</US_RIProfile>\n" );

   sdb.rows.insert( "get_experiment_info_by_runID", QList< QVariantList >()
                    << experimentRow( "RI", QString::fromLatin1( rip ) ) );

   US_DataPubCatalog::Run run = dbRun();
   run.runType = "RI";
   run.raws.clear();

   // An empty local copy, from long ago: an interactive read would fill it
   QString   cached = runDir() + "/" + runID + ".RIProfile.xml";
   QFile     cfile( cached );
   ASSERT_TRUE( cfile.open( QIODevice::WriteOnly ) );
   cfile.close();
   QDateTime past = QDateTime::currentDateTimeUtc().addDays( -30 );
   ASSERT_TRUE( cfile.open( QIODevice::ReadWrite ) );
   ASSERT_TRUE( cfile.setFileTime( past, QFileDevice::FileModificationTime ) );
   cfile.close();

   QString error;
   ASSERT_TRUE( US_DataPubExporterTest::addExperiment( exporter, run, error ) )
      << error.toStdString();

   QFileInfo after( cached );
   EXPECT_EQ( after.size(), 0 );
   EXPECT_LT( qAbs( after.lastModified().toUTC().secsTo( past ) ), 2 );

   // ... and with none there, none is made
   ASSERT_TRUE( QFile::remove( cached ) );
   US_DataPubExporter again;
   US_DataPubExporterTest::attach( again, sdb.db() );
   ASSERT_TRUE( US_DataPubExporterTest::stage( again, error ) );
   ASSERT_TRUE( US_DataPubExporterTest::addExperiment( again, run, error ) )
      << error.toStdString();
   EXPECT_FALSE( QFile::exists( cached ) );

   // The profile travels in the bundle
   US_DataPubEntity exper;
   ASSERT_TRUE( again.manifest().entity( US_DataPub::Experiment, expGUID,
                                         exper ) );
   QString rel = exper.attrs.value( "riProfilePayload" );
   ASSERT_EQ( rel, "experiment/" + runID + ".RIProfile.xml" );

   QFile staged( US_DataPubExporterTest::staged( again, rel ) );
   ASSERT_TRUE( staged.open( QIODevice::ReadOnly ) );
   QByteArray content = staged.readAll();
   EXPECT_TRUE( US_DataPubRecords::riProfileIsUsable( content ) );
   EXPECT_TRUE( content.contains( "0.625" ) );
}

// A database run whose record names no calibration is staged without one:
// the export does not let the experiment reader take the rotor's first
TEST_F( DataPubExportSteps, AnUncalibratedRunIsNotGivenTheRotorsFirstCalibration )
{
   QVariantList row = experimentRow( "RA", QString() );
   row[ 7 ] = 0;                                   // rotorCalibrationID
   sdb.rows.insert( "get_experiment_info_by_runID",
                    QList< QVariantList >() << row );
   sdb.rows.insert( "get_rotor_calibration_profiles", QList< QVariantList >()
                    << ( QVariantList() << 99 << "first calibration" ) );

   US_DataPubCatalog::Run run = dbRun();
   run.raws.clear();

   QString error;
   ASSERT_TRUE( US_DataPubExporterTest::addExperiment( exporter, run, error ) )
      << error.toStdString();

   EXPECT_FALSE( sdb.asked.contains( "get_rotor_calibration_profiles" ) )
      << sdb.asked.join( ", " ).toStdString();

   US_DataPubEntity exper;
   ASSERT_TRUE( exporter.manifest().entity( US_DataPub::Experiment, expGUID,
                                            exper ) );
   QFile staged( US_DataPubExporterTest::staged( exporter, exper.payload ) );
   ASSERT_TRUE( staged.open( QIODevice::ReadOnly ) );
   QByteArray xml = staged.readAll();
   EXPECT_FALSE( xml.contains( "\"99\"" ) ) << xml.constData();
}

// An experiment exported without its solutions does not need them
TEST_F( DataPubExportSteps, AnExperimentAloneNeedsNoSolutions )
{
   sdb.rows.insert( "get_experiment_info_by_runID", QList< QVariantList >()
                    << experimentRow( "RA", QString() ) );

   US_DataPubCatalog::Run run = dbRun();
   run.raws[ 0 ].solutionID = "0";

   US_DataPubExporterTest::setScope( exporter, US_DataPub::ScopeExperiment );

   QString error;
   EXPECT_TRUE( US_DataPubExporterTest::addExperiment( exporter, run, error ) )
      << error.toStdString();

   // ... but one that carries them does
   US_DataPubExporter full;
   US_DataPubExporterTest::attach( full, sdb.db() );
   ASSERT_TRUE( US_DataPubExporterTest::stage( full, error ) );
   US_DataPubExporterTest::setScope( full, US_DataPub::ScopeSolutions );

   EXPECT_FALSE( US_DataPubExporterTest::addExperiment( full, run, error ) );
   EXPECT_TRUE ( error.contains( "names no solution" ) ) << error.toStdString();
}

// A downloaded raw data record has to be the one the catalog listed: an
// import refuses a payload whose GUID is not its manifest entry's
TEST_F( DataPubExportSteps, ARawRecordWithAnotherGuidFailsTheExport )
{
   US_DataPubCatalog::Run run = dbRun();
   US_DataPubExporterTest::setSolution( exporter, rawGUID, solutionGUID );

   sdb.blobs.insert( 5, runDir() + "/" + writeRawFile( 3, US_Util::new_guid() ) );

   QString error;
   EXPECT_FALSE( US_DataPubExporterTest::addRawData( exporter, run, error ) );
   EXPECT_TRUE ( error.contains( rawGUID ) ) << error.toStdString();
   EXPECT_EQ   ( exporter.manifest().count( US_DataPub::RawData ), 0 );

   // The right record goes through
   sdb.blobs.insert( 5, runDir() + "/" + rawFile );

   EXPECT_TRUE( US_DataPubExporterTest::addRawData( exporter, run, error ) )
      << error.toStdString();
   EXPECT_EQ  ( exporter.manifest().count( US_DataPub::RawData ), 1 );
}

TEST_F( DataPubExportSteps, AnEditRecordWithOtherGuidsFailsTheExport )
{
   US_DataPubCatalog::Run run = dbRun();
   QString source = runDir() + "/" + editFile;
   QString other  = root + "/other_edit.xml";

   // Another edit of the same triple
   ASSERT_TRUE( QFile::copy( source, other ) );
   ASSERT_TRUE( replace_in_file( other, editGUID, US_Util::new_guid() ) );
   sdb.blobs.insert( 6, other );

   QString error;
   EXPECT_FALSE( US_DataPubExporterTest::addEdits( exporter, run, error ) );
   EXPECT_TRUE ( error.contains( editGUID ) ) << error.toStdString();

   // The edit, but of another triple
   ASSERT_TRUE( QFile::remove( other ) );
   ASSERT_TRUE( QFile::copy( source, other ) );
   ASSERT_TRUE( replace_in_file( other, rawGUID, US_Util::new_guid() ) );

   EXPECT_FALSE( US_DataPubExporterTest::addEdits( exporter, run, error ) );
   EXPECT_TRUE ( error.contains( rawGUID ) ) << error.toStdString();

   sdb.blobs.insert( 6, source );

   EXPECT_TRUE( US_DataPubExporterTest::addEdits( exporter, run, error ) )
      << error.toStdString();
   EXPECT_EQ  ( exporter.manifest().count( US_DataPub::EditedData ), 1 );
}

// Noise asked for by GUID is not dropped because its model is not there
TEST_F( DataPubExportSteps, NoiseAskedForWithoutItsModelFailsTheExport )
{
   US_DataPubCatalog::Noise noise;
   noise.guid      = noiseGUID;
   noise.modelGUID = US_Util::new_guid();

   QList< US_DataPubCatalog::Noise > noises;
   noises << noise;

   QString error;
   EXPECT_TRUE( US_DataPubExporterTest::addNoises( exporter, noises, error ) )
      << error.toStdString();
   EXPECT_EQ  ( exporter.manifest().count( US_DataPub::Noise ), 0 );

   US_DataPubExporterTest::askForNoises( exporter, QStringList() << noiseGUID );

   EXPECT_FALSE( US_DataPubExporterTest::addNoises( exporter, noises, error ) );
   EXPECT_TRUE ( error.contains( noiseGUID ) ) << error.toStdString();
}

// A record without a GUID is reported, not passed over in silence
TEST_F( DataPubExportSteps, ANoiseRecordWithoutAGuidIsReported )
{
   US_DataPubCatalog::Noise noise;
   noise.description = "nameless noise";

   QString error;
   EXPECT_TRUE( US_DataPubExporterTest::addNoises( exporter,
                QList< US_DataPubCatalog::Noise >() << noise, error ) );
   EXPECT_TRUE( exporter.log().join( "\n" ).contains( "nameless noise" ) );
}

// ------------------------------------------------- disk store edge cases

// An import refuses a bundle whose RI profile does not parse, so an empty
// or broken one is left out of it, with a warning
TEST_F( DataPubExport, AnUnusableRIProfileIsLeftOut )
{
   QStringList contents;
   contents << QString() << "<US_RIProfile><RI value=\"0.5\"/>"
            << "<US_RIProfile version=\"1.0\"/>";

   for ( int ii = 0; ii < contents.size(); ii++ )
   {
      QString ripPath = runDir() + "/" + runID + ".RIProfile.xml";
      QFile   rip( ripPath );
      ASSERT_TRUE( rip.open( QIODevice::WriteOnly | QIODevice::Truncate ) );
      rip.write( contents[ ii ].toUtf8() );
      rip.close();

      US_DataPubExporter exporter;
      QString            error;

      ASSERT_TRUE( exporter.exportBundle(
                   baseSelection( US_DataPub::ScopeRawData ),
                   root + "/ri_bad.tar.gz", error ) ) << error.toStdString();

      US_DataPubEntity exper;
      ASSERT_TRUE( exporter.manifest().entity( US_DataPub::Experiment, expGUID,
                                               exper ) );
      EXPECT_FALSE( exper.attrs.contains( "riProfilePayload" ) ) << ii;
      EXPECT_TRUE ( exporter.log().join( "\n" ).contains( "RI intensity" ) );
   }
}

// A dataset of the experiment XML whose raw data file is gone names a
// triple -- and a solution -- the bundle does not carry; it is left out
TEST_F( DataPubExport, ADatasetWithoutItsRawDataIsLeftOut )
{
   addSecondTriple();
   ASSERT_TRUE( QFile::remove( runDir() + "/" + rawFile2 ) );

   US_DataPubExporter exporter;
   QString            bundle = root + "/missing_auc.tar.gz";
   QString            error;

   ASSERT_TRUE( exporter.exportBundle( baseSelection( US_DataPub::ScopeRawData ),
                bundle, error ) ) << error.toStdString();

   const US_DataPubManifest& mani = exporter.manifest();
   EXPECT_EQ   ( mani.count( US_DataPub::RawData ), 1 );
   EXPECT_FALSE( mani.contains( US_DataPub::Solution, solutionGUID2 ) );

   US_DataPubEntity exper;
   ASSERT_TRUE( mani.entity( US_DataPub::Experiment, expGUID, exper ) );

   US_DataPubBundle unpacked;
   ASSERT_TRUE( unpacked.unpack( bundle, error ) ) << error.toStdString();

   QString xml = read_text( unpacked.rootPath() + "/" + exper.payload );
   EXPECT_TRUE ( xml.contains( rawGUID ) );
   EXPECT_FALSE( xml.contains( rawGUID2 ) );
   EXPECT_FALSE( xml.contains( solutionGUID2 ) );
   EXPECT_TRUE ( exporter.log().join( "\n" ).contains( "no raw data file" ) );
}

// When the project file is gone, the message says how to go on without it
TEST_F( DataPubExport, AMissingProjectFileSuggestsLeavingItOut )
{
   ASSERT_TRUE( QFile::remove( dataDir( "projects" ) + "/P0000001.xml" ) );

   US_DataPubExporter exporter;
   QString            error;

   EXPECT_FALSE( exporter.previewManifest(
                 baseSelection( US_DataPub::ScopeRawData ), error ) );
   EXPECT_TRUE ( error.contains( "--no-project" ) ) << error.toStdString();
}

// --------------------------------------------------- round three: export

/**
 * A second run of the same project in the scratch store: its own
 * experiment and triple, in a channel filled with the same solution as
 * the first run's.
 */
class DataPubExportTwoRuns : public DataPubExport
{
   protected:
      QString runID1;
      QString expGUID1;
      QString rawGUID1;
      QString runID2;
      QString expGUID2;
      QString rawGUID2b;

      void addRun( const QString& newRunID )
      {
         runID1   = runID;
         expGUID1 = expGUID;
         rawGUID1 = rawGUID;

         runID     = newRunID;
         expGUID   = US_Util::new_guid();
         rawGUID   = US_Util::new_guid();
         runID2    = runID;
         expGUID2  = expGUID;
         rawGUID2b = rawGUID;

         QString file = writeRawFile( 1, rawGUID );
         writeExperimentXml();

         // Back to the first run, which the other helpers work on
         runID   = runID1;
         expGUID = expGUID1;
         rawGUID = rawGUID1;
         Q_UNUSED( file );
      }
};

// A solution that fills channels of two runs belongs to both of them
TEST_F( DataPubExportTwoRuns, ASharedSolutionDependsOnEveryRunsExperiment )
{
   addRun( "demo_run2" );

   US_DataPubExporter            exporter;
   US_DataPubExporter::Selection selection =
      baseSelection( US_DataPub::ScopeSolutions );
   selection.runIDs << runID2;
   QString error;

   ASSERT_TRUE( exporter.previewManifest( selection, error ) )
      << error.toStdString();

   const US_DataPubManifest& mani = exporter.manifest();
   EXPECT_EQ( mani.count( US_DataPub::Experiment ), 2 );
   ASSERT_EQ( mani.count( US_DataPub::Solution   ), 1 );

   QStringList exps = mani.section( US_DataPub::Solution )[ 0 ]
                      .dependsOn( US_DataPub::Experiment );
   EXPECT_EQ( exps.size(), 2 );
   EXPECT_TRUE( exps.contains( expGUID1 ) );
   EXPECT_TRUE( exps.contains( expGUID2 ) );

   QString verror;
   EXPECT_TRUE( mani.validate( verror ) ) << verror.toStdString();
}

// A project export leaves out a run it cannot export, with a warning; the
// same run named on its own fails the export
TEST_F( DataPubExportTwoRuns, AProjectExportSkipsARunWithAnUnusableRunId )
{
   addRun( "bad run" );

   US_DataPubCatalog catalog;
   QString           error;
   ASSERT_TRUE( catalog.open( false, QString(), error ) );
   QList< US_DataPubCatalog::Run > listed = catalog.runs( projectGUID, error );
   bool seen = false;

   for ( int ii = 0; ii < listed.size(); ii++ )
      if ( listed[ ii ].runID == runID2 )  seen = true;

   ASSERT_TRUE( seen ) << "the catalog does not list the run";

   US_DataPubExporter            exporter;
   US_DataPubExporter::Selection selection;
   selection.fromDb      = false;
   selection.scope       = US_DataPub::ScopeSolutions;
   selection.projectGUID = projectGUID;

   ASSERT_TRUE( exporter.previewManifest( selection, error ) )
      << error.toStdString();
   EXPECT_EQ( exporter.manifest().count( US_DataPub::Experiment ), 1 );
   EXPECT_TRUE( exporter.log().join( "\n" ).contains( "bad run" ) );

   US_DataPubExporter            named;
   US_DataPubExporter::Selection one = baseSelection( US_DataPub::ScopeSolutions );
   one.runIDs = QStringList() << runID2;

   EXPECT_FALSE( named.previewManifest( one, error ) );
   EXPECT_TRUE ( error.contains( "bad run" ) ) << error.toStdString();
}

// With only a project picked there is no tree: everything of its runs goes
TEST_F( DataPubExport, AProjectAloneExportsAllOfItsRuns )
{
   US_DataPubExporter::Selection selection;
   selection.fromDb      = false;
   selection.scope       = US_DataPub::ScopeNoise;
   selection.projectGUID = projectGUID;

   // What the export tab used to make of it: nothing checked, so nothing
   US_DataPubExporter::Selection empty = selection;
   empty.setChecked( true, QStringList(), QStringList(), QStringList(),
                     QStringList() );
   US_DataPubExporter before;
   QString            error;
   EXPECT_FALSE( before.previewManifest( empty, error ) );

   selection.setChecked( false, QStringList(), QStringList(), QStringList(),
                         QStringList() );
   EXPECT_FALSE( selection.rawsExplicit );

   US_DataPubExporter exporter;
   ASSERT_TRUE( exporter.exportBundle( selection, root + "/project.tar.gz",
                error ) ) << error.toStdString();
   EXPECT_EQ( exporter.manifest().count( US_DataPub::RawData    ), 1 );
   EXPECT_EQ( exporter.manifest().count( US_DataPub::EditedData ), 1 );
   EXPECT_EQ( exporter.manifest().count( US_DataPub::Model      ), 1 );
   EXPECT_EQ( exporter.manifest().count( US_DataPub::Noise      ), 1 );
}

// A legacy record that names something other than a GUID where a GUID
// belongs would be refused on import; the export refuses it first, and
// writes no bundle
TEST_F( DataPubExport, AnUnusableGuidInsideAPayloadFailsTheExport )
{
   QString expXml = runDir() + "/" + runID + "." + runType + ".xml";
   QString text   = read_text( expXml );
   QRegularExpression rx( "(<operator[^>]*guid=\")[^\"]*\"" );
   ASSERT_TRUE( rx.match( text ).hasMatch() );
   text.replace( rx, "\\1operator-7\"" );

   QFile file( expXml );
   ASSERT_TRUE( file.open( QIODevice::WriteOnly | QIODevice::Truncate ) );
   file.write( text.toUtf8() );
   file.close();

   US_DataPubExporter exporter;
   QString            bundle = root + "/legacy.tar.gz";
   QString            error;

   EXPECT_FALSE( exporter.exportBundle( baseSelection( US_DataPub::ScopeRawData ),
                 bundle, error ) );
   EXPECT_TRUE ( error.contains( "operator-7" ) ) << error.toStdString();
   EXPECT_FALSE( QFile::exists( bundle ) );
}

// A description of any kind -- non-ASCII, quoted, over several lines --
// reaches the bundle's manifest as it is
TEST_F( DataPubExport, DescriptionsOfAnyKindSurviveTheManifest )
{
   QString desc = QString::fromUtf8( "Pr\xC3\xBC" "fung \"A\" \\ 1\n"
                                     "zweite Zeile\t\xCE\xB1" );

   US_Project project;
   project.projectID   = 7;
   project.projectGUID = projectGUID;
   project.projectDesc = desc;
   ASSERT_TRUE( project.saveToFile( dataDir( "projects" ) + "/P0000001.xml" ) );

   US_DataPubExporter::Selection selection =
      baseSelection( US_DataPub::ScopeSolutions );
   selection.comment = desc + QChar( 0x01 ) + QChar( 0x00 ) + "after a NUL";

   US_DataPubExporter exporter;
   QString            bundle = root + "/text.tar.gz";
   QString            error;

   ASSERT_TRUE( exporter.exportBundle( selection, bundle, error ) )
      << error.toStdString();

   US_DataPubBundle unpacked;
   ASSERT_TRUE( unpacked.unpack( bundle, error ) ) << error.toStdString();

   US_DataPubManifest mani;
   ASSERT_TRUE( mani.read( unpacked.rootPath() + "/"
                           + US_DataPubBundle::manifestName(), error ) )
      << error.toStdString();

   EXPECT_EQ( mani.comment, desc + QChar( 0x01 ) + QChar( 0x00 ) + "after a NUL" );
   ASSERT_EQ( mani.count( US_DataPub::Project ), 1 );
   EXPECT_EQ( mani.section( US_DataPub::Project )[ 0 ].name, desc );
}

// Buffer, analyte and solution spectra, and the buffer's concentrations,
// leave at the precision they have: a database record is read in full
TEST_F( DataPubExportSteps, SpectraAndConcentrationsTravelAtFullPrecision )
{
   US_BufferComponent comp;
   comp.componentID = "99";
   comp.name        = "sodium chloride";
   comp.unit        = "mM";

   US_Solution solution;
   solution.solutionID   = 11;
   solution.solutionGUID = solutionGUID;
   solution.solutionDesc = "Demo solution";
   solution.buffer.GUID        = bufferGUID;
   solution.buffer.bufferID    = "3";
   solution.buffer.description = "Demo buffer";
   solution.buffer.component     << comp;
   solution.buffer.componentIDs  << "99";
   solution.buffer.concentration << 0.005;           // 5 uM
   solution.buffer.extinction  .insert( 280.0,  1.0 / 3.0 );
   solution.buffer.extinction  .insert( 280.02, 2.0 / 3.0 );
   solution.buffer.refraction  .insert( 589.3,  1.333000123456789 );
   solution.buffer.fluorescence.insert( 350.0,  1.0e-9 / 7.0 );

   US_Solution::AnalyteInfo info;
   info.analyte.analyteGUID = analyteGUID;
   info.analyte.analyteID   = "5";
   info.analyte.description = "Demo analyte";
   info.analyte.type        = US_Analyte::PROTEIN;
   info.analyte.extinction.insert( 280.0, 43824.123456789 );
   info.analyte.extinction.insert( 280.04, 43800.5 );
   info.amount = 1.0;
   solution.analyteInfo << info;

   solution.extinction.insert( 280.0, 12345.678901234567 );
   solution.extinction.insert( 230.5, 0.1 / 3.0 );

   QString error;
   ASSERT_TRUE( US_DataPubExporterTest::addSolution( exporter, solution,
                expGUID, error ) ) << error.toStdString();

   const US_DataPubManifest& mani = US_DataPubExporterTest::manifest( exporter );

   auto spectraOf = [ this, &mani ]( US_DataPub::EntityType type,
                                     const QString& guid,
                                     US_DataPubRecords::Spectra& spectra )
   {
      US_DataPubEntity entity;
      if ( ! mani.entity( type, guid, entity ) )  return false;

      QString rel    = entity.attrs.value( "spectraPayload" );
      QString staged = US_DataPubExporterTest::staged( exporter, rel );
      QString digest;

      return ! rel.isEmpty()  &&
             US_DataPub::fileSha256( staged, digest )  &&
             digest == entity.attrs.value( "spectraSha256" )  &&
             US_DataPubRecords::readSpectra( staged, spectra );
   };

   US_DataPubRecords::Spectra spectra;

   ASSERT_TRUE( spectraOf( US_DataPub::Buffer, bufferGUID, spectra ) );
   EXPECT_EQ( spectra.value( "extinction"   ), solution.buffer.extinction );
   EXPECT_EQ( spectra.value( "refraction"   ), solution.buffer.refraction );
   EXPECT_EQ( spectra.value( "fluorescence" ), solution.buffer.fluorescence );

   ASSERT_TRUE( spectraOf( US_DataPub::Analyte, analyteGUID, spectra ) );
   EXPECT_EQ( spectra.value( "extinction" ), info.analyte.extinction );
   EXPECT_FALSE( spectra.contains( "refraction" ) );

   ASSERT_TRUE( spectraOf( US_DataPub::Solution, solutionGUID, spectra ) );
   EXPECT_EQ( spectra.value( "extinction" ), solution.extinction );

   // The concentrations, with the component definitions
   US_DataPubEntity buffer;
   ASSERT_TRUE( mani.entity( US_DataPub::Buffer, bufferGUID, buffer ) );

   QStringList     ids;
   QList< double > concs;
   ASSERT_TRUE( US_DataPubRecords::readBufferConcentrations(
                US_DataPubExporterTest::staged( exporter,
                   buffer.attrs.value( "componentsPayload" ) ), ids, concs ) );
   EXPECT_EQ( ids, QStringList() << "99" );
   ASSERT_EQ( concs.size(), 1 );
   EXPECT_EQ( concs[ 0 ], 0.005 );

   // The library writer's own file is there too, and now keeps the
   // spectra whole as well
   US_Buffer written;
   ASSERT_TRUE( written.readFromDisk(
                US_DataPubExporterTest::staged( exporter, buffer.payload ) ) );
   EXPECT_EQ( written.extinction, solution.buffer.extinction );
}

// A record without spectra carries no spectra payload
TEST_F( DataPubExportSteps, NoSpectraNoSpectraPayload )
{
   US_Solution solution;
   solution.solutionGUID       = solutionGUID;
   solution.solutionDesc       = "Plain";
   solution.buffer.GUID        = bufferGUID;
   solution.buffer.description = "Plain buffer";

   QString error;
   ASSERT_TRUE( US_DataPubExporterTest::addSolution( exporter, solution,
                expGUID, error ) ) << error.toStdString();

   US_DataPubEntity entity;
   const US_DataPubManifest& mani = US_DataPubExporterTest::manifest( exporter );
   ASSERT_TRUE( mani.entity( US_DataPub::Solution, solutionGUID, entity ) );
   EXPECT_FALSE( entity.attrs.contains( "spectraPayload" ) );
   ASSERT_TRUE( mani.entity( US_DataPub::Buffer, bufferGUID, entity ) );
   EXPECT_FALSE( entity.attrs.contains( "spectraPayload" ) );
}

// Just before packing, the staged tree has to be exactly what the manifest
// names, and every payload still what its digest says
TEST_F( DataPubExportSteps, TheStagedTreeIsTheManifest )
{
   US_Solution solution;
   solution.solutionGUID       = solutionGUID;
   solution.solutionDesc       = "Plain";
   solution.buffer.GUID        = bufferGUID;
   solution.buffer.description = "Plain buffer";
   solution.extinction.insert( 280.0, 1.5 );

   QString error;
   ASSERT_TRUE( US_DataPubExporterTest::addSolution( exporter, solution,
                expGUID, error ) ) << error.toStdString();

   auto touch = [ this ]( const QString& rel, const QByteArray& data )
   {
      QFile file( US_DataPubExporterTest::staged( exporter, rel ) );
      file.open( QIODevice::WriteOnly | QIODevice::Truncate );
      file.write( data );
      file.close();
   };

   touch( US_DataPubBundle::manifestName(), "manifest" );
   EXPECT_FALSE( US_DataPubExporterTest::checkStagedTree( exporter, error ) );
   EXPECT_TRUE ( error.contains( US_DataPubExporter::manifestDigestName() ) )
      << error.toStdString();

   touch( US_DataPubExporter::manifestDigestName(), "digest" );
   ASSERT_TRUE( US_DataPubExporterTest::checkStagedTree( exporter, error ) )
      << error.toStdString();

   // Something the manifest does not name
   touch( "solutions/stray.xml", "<x/>" );
   EXPECT_FALSE( US_DataPubExporterTest::checkStagedTree( exporter, error ) );
   EXPECT_TRUE ( error.contains( "stray.xml" ) ) << error.toStdString();
   ASSERT_TRUE( QFile::remove( US_DataPubExporterTest::staged( exporter,
                               "solutions/stray.xml" ) ) );

   // A payload changed after it was digested
   US_DataPubEntity entity;
   ASSERT_TRUE( US_DataPubExporterTest::manifest( exporter ).entity(
                US_DataPub::Solution, solutionGUID, entity ) );
   touch( entity.attrs.value( "spectraPayload" ), "<US_DataPubSpectra/>" );
   EXPECT_FALSE( US_DataPubExporterTest::checkStagedTree( exporter, error ) );
   EXPECT_TRUE ( error.contains( "digest" ) ) << error.toStdString();
}

// The RI intensity profile of a database run goes into the bundle as the
// record stores it, not as six-digit values written again
TEST_F( DataPubExportSteps, TheStoredRIProfileIsCopiedByteForByte )
{
   QByteArray rip( "<?xml version=\"1.0\"?>\n<US_RIProfile version=\"1.0\">"
                   "<RI value=\"0.12345678901234567\"/>"
                   "<RI value=\"0.98765432109876543\"/>"
                   "</US_RIProfile>\n" );

   sdb.rows.insert( "get_experiment_info_by_runID", QList< QVariantList >()
                    << experimentRow( "RI", QString::fromLatin1( rip ) ) );

   US_DataPubCatalog::Run run = dbRun();
   run.runType = "RI";
   run.raws.clear();

   QString error;
   ASSERT_TRUE( US_DataPubExporterTest::addExperiment( exporter, run, error ) )
      << error.toStdString();

   US_DataPubEntity exper;
   ASSERT_TRUE( exporter.manifest().entity( US_DataPub::Experiment, expGUID,
                                            exper ) );

   QFile staged( US_DataPubExporterTest::staged( exporter,
                 exper.attrs.value( "riProfilePayload" ) ) );
   ASSERT_TRUE( staged.open( QIODevice::ReadOnly ) );
   EXPECT_EQ( staged.readAll(), rip );
}

// Only what an analyte file holds is a measured spectrum: US_Analyte's
// reader adds the extinction at 280 nm it computes from a protein's
// sequence, which must not travel as if it had been measured
TEST_F( DataPubExport, AnAnalyteCarriesOnlyItsMeasuredSpectrum )
{
   auto exportedSpectrum = [ this ]( QMap< double, double >& spectrum )
   {
      US_DataPubExporter exporter;
      QString            bundle = root + "/analyte.tar.gz";
      QString            error;

      if ( ! exporter.exportBundle( baseSelection( US_DataPub::ScopeSolutions ),
                                    bundle, error ) )
         return false;

      US_DataPubEntity entity;
      US_DataPubBundle unpacked;

      if ( ! exporter.manifest().entity( US_DataPub::Analyte, analyteGUID,
                                         entity )  ||
           entity.attrs.value( "spectraPayload" ).isEmpty()  ||
           ! unpacked.unpack( bundle, error ) )
         return false;

      US_DataPubRecords::Spectra spectra;
      bool ok = US_DataPubRecords::readSpectra( unpacked.rootPath() + "/"
                   + entity.attrs.value( "spectraPayload" ), spectra );
      spectrum = spectra.value( "extinction" );
      return ok;
   };

   // A protein with no spectrum: what the reader makes of it has a point
   US_Analyte read;
   QString    apath = file_with_guid( dataDir( "analytes" ), analyteGUID );
   ASSERT_EQ( read.readFromFile( apath ), IUS_DB2::OK );
   ASSERT_TRUE( read.extinction.contains( 280.0 ) );

   QMap< double, double > spectrum;
   ASSERT_TRUE( exportedSpectrum( spectrum ) );
   EXPECT_TRUE( spectrum.isEmpty() ) << spectrum.size();

   // A measured one comes as it is, with no computed point added
   QFile file( apath );
   ASSERT_TRUE( file.open( QIODevice::ReadOnly ) );
   QString xml = QString::fromUtf8( file.readAll() );
   file.close();
   ASSERT_TRUE( xml.contains( "</analyte>" ) );
   xml.replace( "</analyte>", "<extinction frequency=\"230.5\" value=\"1234.5\"/>"
                              "</analyte>" );
   ASSERT_TRUE( file.open( QIODevice::WriteOnly | QIODevice::Truncate ) );
   file.write( xml.toUtf8() );
   file.close();

   ASSERT_TRUE( exportedSpectrum( spectrum ) );
   QMap< double, double > want;
   want.insert( 230.5, 1234.5 );
   EXPECT_EQ( spectrum, want );
}

// ------------------------------------------------------- round trips, fixes

namespace
{
   US_DataPubImporter::Options disk_target( const QString& dir )
   {
      US_DataPubImporter::Options options;
      options.target       = US_DataPub::TargetDisk;
      options.outputDir    = dir;
      options.verifyHashes = true;
      options.dryRun       = false;
      options.policy       = US_DataPub::PolicyReuse;

      return options;
   }

   // The noise records of a disk store, by GUID: the model each one names
   QMap< QString, QString > noise_models( const QString& dir )
   {
      QMap< QString, QString > found;
      QStringList names = QDir( dir ).entryList( QStringList( "N*.xml" ),
                                                 QDir::Files );

      for ( int ii = 0; ii < names.size(); ii++ )
      {
         US_Noise noise;

         if ( noise.load( dir + "/" + names[ ii ] ) == IUS_DB2::OK )
            found.insert( noise.noiseGUID, noise.modelGUID );
      }

      return found;
   }
}

/* A model with no noise of its own travels with the noise of its edit,
   which still belongs to the model it was fitted to: the payload names
   that model, and so does the manifest, which is what an import checks
   the payload against.  The model it was fitted to travels too, and the
   bundle says which model the noise is carried for.  Exported as the
   export tab does it -- the later model and its (borrowed) noise ticked,
   nothing else -- the bundle imports.
*/
TEST_F( DataPubExport, BorrowedNoiseTravelsWithItsOwnModelAndImports )
{
   QString  laterGUID = US_Util::new_guid();
   US_Model later;
   later.modelGUID   = laterGUID;
   later.editGUID    = editGUID;
   later.description = "demo_run.1A280.2dsa-mc.model";
   later.write( US_Settings::dataDir() + "/models/M0000002.xml" );

   US_DataPubExporter::Selection selection =
      baseSelection( US_DataPub::ScopeNoise );
   selection.setModels( QStringList() << laterGUID );
   selection.setNoises( QStringList() << noiseGUID );

   US_DataPubExporter exporter;
   QString            bundle = root + "/borrowed.tar.gz";
   QString            error;

   ASSERT_TRUE( exporter.exportBundle( selection, bundle, error ) )
      << error.toStdString() << "\n"
      << exporter.log().join( "\n" ).toStdString();

   const US_DataPubManifest& mani = exporter.manifest();

   EXPECT_TRUE( mani.contains( US_DataPub::Model, laterGUID ) );
   EXPECT_TRUE( mani.contains( US_DataPub::Model, modelGUID ) )
      << "the model the noise was fitted to does not travel";

   US_DataPubEntity noise;
   ASSERT_TRUE( mani.entity( US_DataPub::Noise, noiseGUID, noise ) );
   EXPECT_EQ( noise.depend( US_DataPub::Model ), modelGUID );
   EXPECT_EQ( noise.attrs.value( "borrowedByModelGUID" ), laterGUID );
   EXPECT_FALSE( noise.attrs.contains( "fittedToModelGUID" ) );

   // The import takes it, and the noise lands on the model it was fitted to
   QString            target = root + "/target";
   US_DataPubImporter importer;

   ASSERT_TRUE( importer.inspect( bundle, error ) ) << error.toStdString();
   ASSERT_TRUE( importer.runImport( disk_target( target ), error ) )
      << error.toStdString();

   QMap< QString, QString > noises = noise_models( target + "/data/noises" );
   ASSERT_TRUE( noises.contains( noiseGUID ) );
   EXPECT_EQ( noises.value( noiseGUID ), modelGUID );
   EXPECT_EQ( QDir( target + "/data/models" ).entryList(
              QStringList( "M*.xml" ), QDir::Files ).size(), 2 );
}

// The same when the catalog lists the later model first: the noise is
// carried for it, and still depends on the model it was fitted to
TEST_F( DataPubExport, BorrowedNoiseOfAWholeRunImports )
{
   QString  laterGUID = US_Util::new_guid();
   US_Model later;
   later.modelGUID   = laterGUID;
   later.editGUID    = editGUID;
   later.description = "demo_run.1A280.2dsa-mc.model";
   later.write( US_Settings::dataDir() + "/models/M0000000.xml" );

   US_DataPubExporter exporter;
   QString            bundle = root + "/whole.tar.gz";
   QString            error;

   ASSERT_TRUE( exporter.exportBundle( baseSelection( US_DataPub::ScopeNoise ),
                                       bundle, error ) )
      << error.toStdString();

   const US_DataPubManifest& mani = exporter.manifest();
   EXPECT_EQ( mani.count( US_DataPub::Model ), 2 );
   ASSERT_EQ( mani.count( US_DataPub::Noise ), 1 );
   EXPECT_EQ( mani.section( US_DataPub::Noise )[ 0 ]
              .depend( US_DataPub::Model ), modelGUID );

   US_DataPubImporter importer;
   ASSERT_TRUE( importer.inspect( bundle, error ) ) << error.toStdString();
   ASSERT_TRUE( importer.runImport( disk_target( root + "/target" ), error ) )
      << error.toStdString();
   EXPECT_EQ( noise_models( root + "/target/data/noises" ).value( noiseGUID ),
              modelGUID );
}

// Noise carried for a model whose own model cannot travel -- it is gone --
// stays out with a warning, or fails the export when it was asked for
TEST_F( DataPubExport, BorrowedNoiseWithoutItsModelStaysOut )
{
   QString  laterGUID = US_Util::new_guid();
   US_Model later;
   later.modelGUID   = laterGUID;
   later.editGUID    = editGUID;
   later.description = "demo_run.1A280.2dsa-mc.model";
   later.write( US_Settings::dataDir() + "/models/M0000002.xml" );

   QList< US_DataPubCatalog::Model > models;
   US_DataPubCatalog::Model model;
   model.guid     = laterGUID;
   model.editGUID = editGUID;
   models << model;

   US_DataPubCatalog::Noise noise;
   noise.guid         = noiseGUID;
   noise.modelGUID    = laterGUID;
   noise.borrowedFrom = US_Util::new_guid();      // no such model
   noise.noiseType    = "ti";
   QList< US_DataPubCatalog::Noise > noises;
   noises << noise;

   US_DataPubExporter exporter;
   QString            error;
   US_DataPubExporterTest::openDisk( exporter );

   QList< US_DataPubCatalog::Noise > kept = noises;
   ASSERT_TRUE( US_DataPubExporterTest::addFittedModels( exporter, models,
                kept, error ) ) << error.toStdString();
   EXPECT_TRUE( kept.isEmpty() );
   EXPECT_EQ( models.size(), 1 );
   EXPECT_TRUE( exporter.log().join( "\n" ).contains( "left out" ) );

   US_DataPubExporterTest::askForNoises( exporter, QStringList() << noiseGUID );
   kept = noises;
   EXPECT_FALSE( US_DataPubExporterTest::addFittedModels( exporter, models,
                 kept, error ) );
   EXPECT_TRUE( error.contains( noiseGUID ) ) << error.toStdString();
}

/* The experiment XML of a run names its run ID and GUID, and an import
   refuses one that names another run than its manifest entry: a run
   directory renamed by hand, say.  The export refuses it first, when the
   run was named; a project export leaves the run out with a warning.
*/
TEST_F( DataPubExport, AnExperimentXmlOfAnotherRunIdIsRefused )
{
   QString expXml = runDir() + "/" + runID + "." + runType + ".xml";
   ASSERT_TRUE( replace_in_file( expXml, "runID=\"" + runID + "\"",
                                 "runID=\"old_name\"" ) );

   US_DataPubExporter exporter;
   QString            error;

   EXPECT_FALSE( exporter.exportBundle( baseSelection( US_DataPub::ScopeNoise ),
                                        root + "/renamed.tar.gz", error ) );
   EXPECT_TRUE( error.contains( "old_name" ) ) << error.toStdString();
   EXPECT_FALSE( QFile::exists( root + "/renamed.tar.gz" ) );
}

TEST_F( DataPubExport, AnExperimentXmlOfAnotherGuidIsRefused )
{
   QString staged = root + "/staged.xml";
   QString expXml = runDir() + "/" + runID + "." + runType + ".xml";
   ASSERT_TRUE( QFile::copy( expXml, staged ) );

   QString error;
   EXPECT_TRUE ( US_DataPubExporterTest::checkExperimentXml( staged, runID,
                 expGUID, error ) ) << error.toStdString();

   QString other = US_Util::new_guid();
   EXPECT_FALSE( US_DataPubExporterTest::checkExperimentXml( staged, runID,
                 other, error ) );
   EXPECT_TRUE ( error.contains( expGUID ) ) << error.toStdString();

   EXPECT_FALSE( US_DataPubExporterTest::checkExperimentXml( staged,
                 "another_run", expGUID, error ) );
   EXPECT_TRUE ( error.contains( runID ) ) << error.toStdString();

   // The comparison is the import's: exact
   EXPECT_FALSE( US_DataPubExporterTest::checkExperimentXml( staged, runID,
                 expGUID.toUpper(), error ) );
}

// A project export leaves out a run whose records do not hold together --
// here the experiment XML of a renamed run directory, and a channel whose
// solution is gone -- with a warning, takes back what was staged for it,
// and writes a bundle that imports; the same run named fails the export
TEST_F( DataPubExportTwoRuns, AProjectExportSkipsARunThatFailsToStage )
{
   addRun( "demo_run2" );
   QString xml2 = US_Settings::resultDir() + "/" + runID2 + "/" + runID2
                  + "." + runType + ".xml";

   for ( int pass = 0; pass < 2; pass++ )
   {
      SCOPED_TRACE( pass == 0 ? "renamed run" : "no solution" );

      QString original = read_text( xml2 );

      if ( pass == 0 )
         ASSERT_TRUE( replace_in_file( xml2, "runID=\"" + runID2 + "\"",
                                       "runID=\"old_name\"" ) );
      else
         ASSERT_TRUE( replace_in_file( xml2, solutionGUID,
                                       US_Util::new_guid() ) );

         US_DataPubExporter            exporter;
      US_DataPubExporter::Selection selection;
      selection.fromDb      = false;
      selection.scope       = US_DataPub::ScopeNoise;
      selection.projectGUID = projectGUID;

      QString bundle = root + QString( "/project%1.tar.gz" ).arg( pass );
      QString error;

      ASSERT_TRUE( exporter.exportBundle( selection, bundle, error ) )
         << error.toStdString() << "\n"
         << exporter.log().join( "\n" ).toStdString();

      const US_DataPubManifest& mani = exporter.manifest();
      EXPECT_EQ( mani.count( US_DataPub::Experiment ), 1 );
      EXPECT_TRUE( mani.contains( US_DataPub::Experiment, expGUID1 ) );
      EXPECT_FALSE( mani.contains( US_DataPub::RawData, rawGUID2b ) );
      EXPECT_EQ( mani.count( US_DataPub::Model ), 1 );
      EXPECT_TRUE( exporter.log().join( "\n" ).contains(
                   "run " + runID2 + " of the project is left out" ) )
         << exporter.log().join( "\n" ).toStdString();

      // Nothing of the run is left in the bundle, and the bundle imports
      US_DataPubImporter importer;
      ASSERT_TRUE( importer.inspect( bundle, error ) ) << error.toStdString();
      EXPECT_TRUE( importer.verifyPayloads().isEmpty() );

      QMap< QString, qint64 > files;
      QString                 why;
      ASSERT_TRUE( US_DataPubBundle::listTree( importer.bundleRoot(), files,
                                               why ) );

      for ( auto it = files.constBegin(); it != files.constEnd(); ++it )
         EXPECT_FALSE( it.key().contains( runID2 ) ) << it.key().toStdString();

      QDir dirs( importer.bundleRoot() + "/rawData" );
      EXPECT_FALSE( dirs.exists( runID2 ) );

      ASSERT_TRUE( importer.runImport( disk_target( root + "/target"
                   + QString::number( pass ) ), error ) )
         << error.toStdString();

      // The same run, named, fails the export
      US_DataPubExporter            named;
      US_DataPubExporter::Selection one =
         baseSelection( US_DataPub::ScopeNoise );
      one.runIDs = QStringList() << runID2;

      EXPECT_FALSE( named.exportBundle( one, root + "/one.tar.gz", error ) );
      EXPECT_FALSE( QFile::exists( root + "/one.tar.gz" ) );

      // Back to a run that holds together for the next pass
      QFile file( xml2 );
      ASSERT_TRUE( file.open( QIODevice::WriteOnly | QIODevice::Truncate ) );
      file.write( original.toUtf8() );
      file.close();
   }
}

// When no run of the project can be exported, there is nothing to export
TEST_F( DataPubExport, AProjectOfOnlyBrokenRunsFailsTheExport )
{
   QString expXml = runDir() + "/" + runID + "." + runType + ".xml";
   ASSERT_TRUE( replace_in_file( expXml, solutionGUID, US_Util::new_guid() ) );

   US_DataPubExporter            exporter;
   US_DataPubExporter::Selection selection;
   selection.fromDb      = false;
   selection.scope       = US_DataPub::ScopeNoise;
   selection.projectGUID = projectGUID;
   QString error;

   EXPECT_FALSE( exporter.exportBundle( selection, root + "/none.tar.gz",
                                        error ) );
   EXPECT_TRUE( error.contains( "None of the 1 runs" ) ) << error.toStdString();
}

/* The analyte's main payload holds what was measured and nothing else: no
   extinction at 280 nm computed from the sequence, which the library's
   reader adds.  So it is the same whether the analyte is exported from
   disk or from the database (which stores only the measured profile).
*/
TEST_F( DataPubExport, TheAnalytePayloadHoldsOnlyMeasuredSpectra )
{
   auto payloadOf = [ this ]( QString& xml )
   {
      US_DataPubExporter exporter;
      QString            bundle = root + "/analyte_main.tar.gz";
      QString            error;

      if ( ! exporter.exportBundle( baseSelection( US_DataPub::ScopeSolutions ),
                                    bundle, error ) )
         return false;

      US_DataPubEntity entity;
      US_DataPubBundle unpacked;

      if ( ! exporter.manifest().entity( US_DataPub::Analyte, analyteGUID,
                                         entity )  ||
           ! unpacked.unpack( bundle, error ) )
         return false;

      xml = read_text( unpacked.rootPath() + "/" + entity.payload );
      return ! xml.isEmpty();
   };

   QString xml;
   ASSERT_TRUE( payloadOf( xml ) );
   EXPECT_TRUE( xml.contains( analyteGUID ) );
   EXPECT_FALSE( xml.contains( "<extinction" ) ) << xml.toStdString();

   // A measured spectrum is all there is
   QString apath = file_with_guid( dataDir( "analytes" ), analyteGUID );
   ASSERT_TRUE( replace_in_file( apath, "</analyte>",
                "<extinction frequency=\"230.5\" value=\"1234.5\"/>"
                "</analyte>" ) );

   ASSERT_TRUE( payloadOf( xml ) );
   EXPECT_EQ( xml.count( "<extinction" ), 1 ) << xml.toStdString();
   EXPECT_TRUE( xml.contains( "frequency=\"230.5\"" ) ) << xml.toStdString();
}

// A spectrum or a concentration that is not a number would be refused on
// import; the export refuses it first
TEST_F( DataPubExportSteps, NonFiniteSpectraAndConcentrationsAreRefused )
{
   auto plain = [ this ]()
   {
      US_Solution solution;
      solution.solutionGUID       = solutionGUID;
      solution.solutionDesc       = "Plain";
      solution.buffer.GUID        = bufferGUID;
      solution.buffer.description = "Plain buffer";
      return solution;
   };

   const double nan = std::numeric_limits< double >::quiet_NaN();
   const double inf = std::numeric_limits< double >::infinity();

   QList< US_Solution > bad;

   US_Solution solution = plain();
   solution.extinction.insert( 280.0, nan );
   bad << solution;

   solution = plain();
   solution.buffer.extinction.insert( 280.0, inf );
   bad << solution;

   solution = plain();
   solution.buffer.refraction.insert( nan, 1.33 );
   bad << solution;

   solution = plain();
   US_BufferComponent comp;
   comp.componentID = "99";
   comp.name        = "sodium chloride";
   solution.buffer.component     << comp;
   solution.buffer.componentIDs  << "99";
   solution.buffer.concentration << inf;
   bad << solution;

   solution = plain();
   US_Solution::AnalyteInfo info;
   info.analyte.analyteGUID = analyteGUID;
   info.analyte.description = "Demo analyte";
   info.analyte.extinction.insert( 280.0, -inf );
   info.amount = 1.0;
   solution.analyteInfo << info;
   bad << solution;

   solution = plain();
   info.analyte.extinction.clear();
   info.amount = nan;
   solution.analyteInfo.clear();
   solution.analyteInfo << info;
   bad << solution;

   for ( int ii = 0; ii < bad.size(); ii++ )
   {
      US_DataPubExporter fresh;
      US_DataPubExporterTest::attach( fresh, sdb.db() );
      QString error;
      ASSERT_TRUE( US_DataPubExporterTest::stage( fresh, error ) );

      EXPECT_FALSE( US_DataPubExporterTest::addSolution( fresh, bad[ ii ],
                    expGUID, error ) ) << "case " << ii;
      EXPECT_TRUE( error.contains( "not a number" ) )
         << "case " << ii << ": " << error.toStdString();
      EXPECT_TRUE( US_DataPubExporterTest::runFault( fresh ) ) << "case " << ii;
   }

   // A finite one goes
   solution = plain();
   solution.extinction.insert( 280.0, 1.5 );
   QString error;
   EXPECT_TRUE( US_DataPubExporterTest::addSolution( exporter, solution,
                expGUID, error ) ) << error.toStdString();
}
