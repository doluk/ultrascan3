// test/programs/us_data_publication/test_us_datapub_import.cpp
#include <gtest/gtest.h>

#include "datapub_test_env.h"

#include "us_datapub_export.h"
#include "us_datapub_import.h"
#include "us_datapub_records.h"
#include "us_datapub_hash.h"

class DataPubImport : public DataPubTestEnv
{
   protected:
      QString bundlePath;
      QString targetDir;

      void SetUp() override
      {
         DataPubTestEnv::SetUp();

         targetDir  = root + "/target";
         QDir().mkpath( targetDir );

         bundlePath = root + "/full.tar.gz";

         US_DataPubExporter            exporter;
         US_DataPubExporter::Selection selection;
         selection.fromDb = false;
         selection.scope  = US_DataPub::ScopeNoise;
         selection.runIDs << runID;

         QString error;
         ASSERT_TRUE( exporter.exportBundle( selection, bundlePath, error ) )
            << error.toStdString();
      }

      US_DataPubImporter::Options diskOptions()
      {
         US_DataPubImporter::Options options;
         options.target       = US_DataPub::TargetDisk;
         options.outputDir    = targetDir;
         options.verifyHashes = true;
         options.dryRun       = false;
         options.policy       = US_DataPub::PolicyReuse;

         return options;
      }

      int countOf( const QList< US_DataPubImporter::Result >& results,
                   US_DataPub::Resolution resolution )
      {
         int knt = 0;

         for ( int ii = 0; ii < results.size(); ii++ )
            if ( results[ ii ].resolution == resolution )  knt++;

         return knt;
      }
};

TEST_F( DataPubImport, InspectReadsTheManifest )
{
   US_DataPubImporter importer;
   QString            error;

   ASSERT_TRUE( importer.inspect( bundlePath, error ) )
      << error.toStdString();

   EXPECT_EQ( importer.manifest().source, "disk" );
   EXPECT_EQ( importer.manifest().scope,  "noise" );
   EXPECT_GT( importer.manifest().total(), 0 );
   EXPECT_TRUE( importer.verifyPayloads().isEmpty() );
}

TEST_F( DataPubImport, AlteredPayloadIsCaught )
{
   US_DataPubImporter importer;
   QString            error;

   ASSERT_TRUE( importer.inspect( bundlePath, error ) );

   US_DataPubEntity model;
   ASSERT_TRUE( importer.manifest().entity( US_DataPub::Model, modelGUID,
                                            model ) );

   QString path = importer.bundleRoot() + "/" + model.payload;
   QFile   file( path );
   ASSERT_TRUE( file.open( QIODevice::Append ) );
   file.write( "<!-- tampered -->" );
   file.close();

   QStringList problems = importer.verifyPayloads();
   ASSERT_EQ( problems.size(), 1 );
   EXPECT_TRUE( problems[ 0 ].contains( "digest" ) )
      << problems[ 0 ].toStdString();

   EXPECT_FALSE( importer.runImport( diskOptions(), error ) );
   EXPECT_TRUE ( error.contains( "not intact" ) ) << error.toStdString();
}

TEST_F( DataPubImport, DryRunWritesNothing )
{
   US_DataPubImporter          importer;
   US_DataPubImporter::Options options = diskOptions();
   options.dryRun = true;

   QString error;
   ASSERT_TRUE( importer.inspect( bundlePath, error ) );
   ASSERT_TRUE( importer.runImport( options, error ) )
      << error.toStdString();

   EXPECT_GT( importer.results().size(), 0 );
   EXPECT_FALSE( QDir( targetDir + "/data/projects" ).exists() );
   EXPECT_FALSE( QDir( targetDir + "/results/" + runID ).exists() );
}

TEST_F( DataPubImport, ImportToAFreshDiskStore )
{
   US_DataPubImporter importer;
   QString            error;

   ASSERT_TRUE( importer.inspect( bundlePath, error ) );
   ASSERT_TRUE( importer.runImport( diskOptions(), error ) )
      << error.toStdString();

   QList< US_DataPubImporter::Result > results = importer.results();

   EXPECT_EQ( countOf( results, US_DataPub::ResolvedRenamed ), 0 );
   EXPECT_GT( countOf( results, US_DataPub::ResolvedCreated ), 0 );

   // Everything the bundle carries is new to this store, except the
   // centerpiece: that is reference data an installation already has, and
   // the import points the run at the target's own entry for it.
   for ( int ii = 0; ii < results.size(); ii++ )
   {
      if ( results[ ii ].type == US_DataPub::Centerpiece )  continue;

      EXPECT_EQ( results[ ii ].resolution, US_DataPub::ResolvedCreated )
         << results[ ii ].name.toStdString();
   }

   // The records landed where an UltraScan3 disk store keeps them
   EXPECT_EQ( QDir( targetDir + "/data/projects"  )
              .entryList( QStringList( "P*.xml" ), QDir::Files ).size(), 1 );
   EXPECT_EQ( QDir( targetDir + "/data/buffers"   )
              .entryList( QStringList( "B*.xml" ), QDir::Files ).size(), 1 );
   EXPECT_EQ( QDir( targetDir + "/data/analytes"  )
              .entryList( QStringList( "A*.xml" ), QDir::Files ).size(), 1 );
   EXPECT_EQ( QDir( targetDir + "/data/solutions" )
              .entryList( QStringList( "S*.xml" ), QDir::Files ).size(), 1 );
   EXPECT_EQ( QDir( targetDir + "/data/models"    )
              .entryList( QStringList( "M*.xml" ), QDir::Files ).size(), 1 );
   EXPECT_EQ( QDir( targetDir + "/data/noises"    )
              .entryList( QStringList( "N*.xml" ), QDir::Files ).size(), 1 );

   QString rundir = targetDir + "/results/" + runID;
   EXPECT_TRUE( QFile::exists( rundir + "/" + rawFile  ) );
   EXPECT_TRUE( QFile::exists( rundir + "/" + editFile ) );
   EXPECT_TRUE( QFile::exists( rundir + "/" + runID + "." + runType
                               + ".xml" ) );

   // GUIDs are what tie a record to its parents, so they are carried over
   QStringList models = QDir( targetDir + "/data/models" )
                        .entryList( QStringList( "M*.xml" ), QDir::Files );
   QString     mpath  = targetDir + "/data/models/" + models[ 0 ];
   QFile       mfile( mpath );
   ASSERT_TRUE( mfile.open( QIODevice::ReadOnly ) );
   QString mcontent = QString::fromUtf8( mfile.readAll() );
   mfile.close();

   EXPECT_TRUE( mcontent.contains( modelGUID ) );
   EXPECT_TRUE( mcontent.contains( editGUID  ) );
}

TEST_F( DataPubImport, SecondImportReusesWhatIsAlreadyThere )
{
   QString error;

   {
      US_DataPubImporter first;
      ASSERT_TRUE( first.inspect  ( bundlePath, error ) );
      ASSERT_TRUE( first.runImport( diskOptions(), error ) )
         << error.toStdString();
   }

   US_DataPubImporter second;
   ASSERT_TRUE( second.inspect  ( bundlePath, error ) );
   ASSERT_TRUE( second.runImport( diskOptions(), error ) )
      << error.toStdString();

   QList< US_DataPubImporter::Result > results = second.results();

   EXPECT_GT( countOf( results, US_DataPub::ResolvedReused  ), 0 );
   EXPECT_EQ( countOf( results, US_DataPub::ResolvedCreated ), 0 );
   EXPECT_EQ( countOf( results, US_DataPub::ResolvedRenamed ), 0 );

   for ( int ii = 0; ii < results.size(); ii++ )
      EXPECT_EQ( results[ ii ].resolution, US_DataPub::ResolvedReused )
         << results[ ii ].name.toStdString();

   // Nothing was written a second time
   EXPECT_EQ( QDir( targetDir + "/data/projects" )
              .entryList( QStringList( "P*.xml" ), QDir::Files ).size(), 1 );
   EXPECT_EQ( QDir( targetDir + "/data/models" )
              .entryList( QStringList( "M*.xml" ), QDir::Files ).size(), 1 );
}

TEST_F( DataPubImport, ADifferentRecordOfTheSameNameIsRenamed )
{
   QString error;

   // Put a project of the same name but different content in the target
   QString projDir = targetDir + "/data/projects";
   QDir().mkpath( projDir );

   US_Project other;
   other.projectID   = 99;
   other.projectGUID = US_Util::new_guid();
   other.projectDesc = "Demo publication project";      // same name
   other.goals       = "something else entirely";       // different content
   ASSERT_TRUE( other.saveToFile( projDir + "/P0000001.xml" ) );

   US_DataPubImporter importer;
   ASSERT_TRUE( importer.inspect  ( bundlePath, error ) );
   ASSERT_TRUE( importer.runImport( diskOptions(), error ) )
      << error.toStdString();

   QList< US_DataPubImporter::Result > results = importer.results();
   bool renamed = false;

   for ( int ii = 0; ii < results.size(); ii++ )
   {
      if ( results[ ii ].type != US_DataPub::Project )  continue;

      EXPECT_EQ( results[ ii ].resolution, US_DataPub::ResolvedRenamed );
      EXPECT_NE( results[ ii ].targetName, "Demo publication project" );
      EXPECT_TRUE( results[ ii ].targetName.contains( "imported" ) )
         << results[ ii ].targetName.toStdString();
      renamed = true;
   }

   EXPECT_TRUE( renamed );

   // The record that was already there is untouched
   EXPECT_EQ( US_DataPubRecords::recordName( projDir + "/P0000001.xml",
                                             US_DataPub::Project ),
              "Demo publication project" );

   // and the imported one lives beside it under its new name
   QStringList files = QDir( projDir ).entryList(
                       QStringList( "P*.xml" ), QDir::Files, QDir::Name );
   EXPECT_EQ( files.size(), 2 );
}

TEST_F( DataPubImport, AMatchingRecordOfTheSameNameIsReused )
{
   QString error;

   // Put the very same project in the target under a different GUID
   QString projDir = targetDir + "/data/projects";
   QDir().mkpath( projDir );

   US_Project same;
   ASSERT_EQ( same.readFromFile( US_Settings::dataDir()
                                 + "/projects/P0000001.xml" ), US_DB2::OK );
   same.projectGUID = US_Util::new_guid();      // a different identity
   same.projectID   = 55;
   ASSERT_TRUE( same.saveToFile( projDir + "/P0000001.xml" ) );

   US_DataPubImporter importer;
   ASSERT_TRUE( importer.inspect  ( bundlePath, error ) );
   ASSERT_TRUE( importer.runImport( diskOptions(), error ) )
      << error.toStdString();

   QList< US_DataPubImporter::Result > results = importer.results();

   for ( int ii = 0; ii < results.size(); ii++ )
   {
      if ( results[ ii ].type != US_DataPub::Project )  continue;

      EXPECT_EQ( results[ ii ].resolution, US_DataPub::ResolvedReused );
      EXPECT_EQ( results[ ii ].targetGUID, same.projectGUID );
      EXPECT_NE( results[ ii ].targetGUID, projectGUID );
   }

   // Only the record that was already there remains
   EXPECT_EQ( QDir( projDir ).entryList( QStringList( "P*.xml" ),
                                         QDir::Files ).size(), 1 );
}

TEST_F( DataPubImport, ReusedProjectIsRemappedIntoTheExperimentXml )
{
   QString error;

   QString projDir = targetDir + "/data/projects";
   QDir().mkpath( projDir );

   US_Project same;
   ASSERT_EQ( same.readFromFile( US_Settings::dataDir()
                                 + "/projects/P0000001.xml" ), US_DB2::OK );
   QString reusedGUID = US_Util::new_guid();
   same.projectGUID   = reusedGUID;
   same.projectID     = 55;
   ASSERT_TRUE( same.saveToFile( projDir + "/P0000001.xml" ) );

   US_DataPubImporter importer;
   ASSERT_TRUE( importer.inspect  ( bundlePath, error ) );
   ASSERT_TRUE( importer.runImport( diskOptions(), error ) )
      << error.toStdString();

   QString expPath = targetDir + "/results/" + runID + "/" + runID + "."
                     + runType + ".xml";
   ASSERT_TRUE( QFile::exists( expPath ) );

   QFile file( expPath );
   ASSERT_TRUE( file.open( QIODevice::ReadOnly ) );
   QString content = QString::fromUtf8( file.readAll() );
   file.close();

   // The run now points at the project that the target already had
   EXPECT_TRUE ( content.contains( reusedGUID  ) ) << content.toStdString();
   EXPECT_FALSE( content.contains( projectGUID ) );
   EXPECT_TRUE ( content.contains( rawGUID     ) );
}

TEST_F( DataPubImport, FailPolicyStopsOnAConflict )
{
   QString error;

   QString projDir = targetDir + "/data/projects";
   QDir().mkpath( projDir );

   US_Project other;
   other.projectGUID = US_Util::new_guid();
   other.projectDesc = "Demo publication project";
   other.goals       = "something else entirely";
   ASSERT_TRUE( other.saveToFile( projDir + "/P0000001.xml" ) );

   US_DataPubImporter          importer;
   US_DataPubImporter::Options options = diskOptions();
   options.policy = US_DataPub::PolicyFail;

   ASSERT_TRUE ( importer.inspect  ( bundlePath, error ) );
   EXPECT_FALSE( importer.runImport( options, error ) );
   EXPECT_TRUE ( error.contains( "fail" ) ) << error.toStdString();
}

TEST_F( DataPubImport, RoundTripKeepsThePayloadsIdentical )
{
   QString error;

   US_DataPubImporter importer;
   ASSERT_TRUE( importer.inspect  ( bundlePath, error ) );
   ASSERT_TRUE( importer.runImport( diskOptions(), error ) )
      << error.toStdString();

   // Export again, from the store that was just imported into, and compare
   // the raw data payload byte for byte with the original.
   QString sourceRaw = US_Settings::resultDir() + "/" + runID + "/" + rawFile;
   QString targetRaw = targetDir + "/results/" + runID + "/" + rawFile;

   ASSERT_TRUE( QFile::exists( targetRaw ) );
   EXPECT_EQ( US_DataPubHash::fileHash( sourceRaw ),
              US_DataPubHash::fileHash( targetRaw ) );

   QString sourceEdit = US_Settings::resultDir() + "/" + runID + "/"
                        + editFile;
   QString targetEdit = targetDir + "/results/" + runID + "/" + editFile;

   EXPECT_EQ( US_DataPubHash::fileHash( sourceEdit ),
              US_DataPubHash::fileHash( targetEdit ) );
}
