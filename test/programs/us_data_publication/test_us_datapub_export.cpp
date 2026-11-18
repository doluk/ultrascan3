// test/programs/us_data_publication/test_us_datapub_export.cpp
#include <gtest/gtest.h>

#include "datapub_test_env.h"

#include "us_datapub_catalog.h"
#include "us_datapub_export.h"
#include "us_datapub_manifest.h"
#include "us_datapub_hash.h"
#include "us_datapub_bundle.h"
#include "us_datapub_records.h"
#include "us_buffer.h"
#include "mock_us_db2.h"

#include <gmock/gmock.h>

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
   // The stored file name is not trusted to name a place
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
   EXPECT_EQ( QFileInfo( tmst ).fileName(), "elsewhere.tmst" );
   EXPECT_TRUE( QFile::exists( tmst ) );

   QFile defs( xdef );
   ASSERT_TRUE( defs.open( QIODevice::ReadOnly ) );
   EXPECT_EQ( defs.readAll(), QByteArray( "<TimeState/>" ) );
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
}
