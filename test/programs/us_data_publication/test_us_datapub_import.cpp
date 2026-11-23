// test/programs/us_data_publication/test_us_datapub_import.cpp
#include <gtest/gtest.h>

#include "datapub_test_env.h"

#include "us_datapub_export.h"
#include "us_datapub_import.h"
#include "us_datapub_records.h"
#include "us_datapub_hash.h"
#include "us_datapub_bundle.h"
#include "us_datapub_cli.h"

#include <functional>

#if defined( Q_OS_UNIX )
#include <cstdio>
#include <unistd.h>
#endif

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

      int filesIn( const QString& dir )
      {
         int knt = 0;
         QDirIterator it( dir, QDir::Files, QDirIterator::Subdirectories );

         while ( it.hasNext() )
         {
            it.next();
            knt++;
         }

         return knt;
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
   options.dryRun    = true;
   options.outputDir = root + "/not_there_yet";   // not even this is made

   QString error;
   ASSERT_TRUE( importer.inspect( bundlePath, error ) );
   ASSERT_TRUE( importer.runImport( options, error ) )
      << error.toStdString();

   EXPECT_GT( importer.results().size(), 0 );
   EXPECT_FALSE( QFileInfo::exists( options.outputDir ) );
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

   // Everything the bundle carries is new to this store, except a
   // centerpiece: that is reference hardware, which an import matches in
   // the installation's own centerpiece table and never creates.  When the
   // table has no such centerpiece it says so: the run names a stand-in
   // (substituted) or none (not imported), with a warning either way.
   int centerpieces = 0;

   for ( int ii = 0; ii < results.size(); ii++ )
   {
      if ( results[ ii ].type == US_DataPub::Centerpiece )
      {
         centerpieces++;
         EXPECT_NE( results[ ii ].resolution, US_DataPub::ResolvedCreated );

         if ( results[ ii ].resolution != US_DataPub::ResolvedReused )
         {
            EXPECT_FALSE( importer.warnings().isEmpty() );
         }

         continue;
      }

      EXPECT_EQ( results[ ii ].resolution, US_DataPub::ResolvedCreated )
         << results[ ii ].name.toStdString();
   }

   EXPECT_EQ( centerpieces,
              importer.manifest().count( US_DataPub::Centerpiece ) );

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

// A bundle without a project must not blank out the project the run names:
// the import has nothing to remap it to, so the reference is left alone.
TEST_F( DataPubImport, BundleWithoutAProjectKeepsTheRunsReference )
{
   QString noProject = root + "/no_project.tar.gz";
   QString error;

   {
      US_DataPubExporter            exporter;
      US_DataPubExporter::Selection selection;
      selection.fromDb         = false;
      selection.scope          = US_DataPub::ScopeNoise;
      selection.includeProject = false;
      selection.runIDs << runID;

      ASSERT_TRUE( exporter.exportBundle( selection, noProject, error ) )
         << error.toStdString();
   }

   US_DataPubImporter importer;
   ASSERT_TRUE( importer.inspect  ( noProject, error ) );
   ASSERT_TRUE( importer.runImport( diskOptions(), error ) )
      << error.toStdString();

   QList< US_DataPubImporter::Result > results = importer.results();

   for ( int ii = 0; ii < results.size(); ii++ )
      EXPECT_NE( results[ ii ].type, US_DataPub::Project );

   EXPECT_FALSE( QDir( targetDir + "/data/projects" )
                 .entryList( QStringList( "P*.xml" ), QDir::Files ).size() );

   QString expPath = targetDir + "/results/" + runID + "/" + runID + "."
                     + runType + ".xml";
   ASSERT_TRUE( QFile::exists( expPath ) );

   QFile file( expPath );
   ASSERT_TRUE( file.open( QIODevice::ReadOnly ) );
   QString content = QString::fromUtf8( file.readAll() );
   file.close();

   // The reference stays, but the source's numeric ID does not: the target
   // has no project 7, so the run names no project ID
   EXPECT_TRUE ( content.contains( projectGUID ) ) << content.toStdString();
   EXPECT_FALSE( content.contains( "id=\"7\"" ) ) << content.toStdString();
   EXPECT_FALSE( content.contains( "guid=\"\"" ) );

   // The raw data and edits still arrived
   EXPECT_TRUE( QFile::exists( targetDir + "/results/" + runID + "/"
                               + rawFile ) );
   EXPECT_TRUE( QFile::exists( targetDir + "/results/" + runID + "/"
                               + editFile ) );
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

// ------------------------------------------------ importing into a project

// The receiving installation keeps its own projects, so an import can be
// pointed at one of them.  The bundle's own project record is then reused
// rather than created, and every run is filed under the chosen one.
TEST_F( DataPubImport, TheRunsAreAttachedToTheChosenProject )
{
   QString error;

   QString projDir = targetDir + "/data/projects";
   QDir().mkpath( projDir );

   QString    mineGUID = US_Util::new_guid();
   US_Project mine;
   mine.projectGUID = mineGUID;
   mine.projectID   = 77;
   mine.projectDesc = "A project of my own";
   ASSERT_TRUE( mine.saveToFile( projDir + "/P0000009.xml" ) );

   US_DataPubImporter          importer;
   US_DataPubImporter::Options options = diskOptions();
   options.projectGUID = mineGUID;

   ASSERT_TRUE( importer.inspect  ( bundlePath, error ) );
   ASSERT_TRUE( importer.runImport( options, error ) ) << error.toStdString();

   // the bundle's project was not created; the chosen one stands in for it
   QList< US_DataPubImporter::Result > results = importer.results();
   bool sawProject = false;

   for ( int ii = 0; ii < results.size(); ii++ )
   {
      if ( results[ ii ].type != US_DataPub::Project )  continue;

      sawProject = true;
      EXPECT_EQ( results[ ii ].resolution,
                 US_DataPub::ResolvedReused );
      EXPECT_EQ( results[ ii ].targetGUID, mineGUID );
   }

   EXPECT_TRUE( sawProject );

   QStringList written = QDir( projDir ).entryList( QStringList( "P*.xml" ),
                                                    QDir::Files );
   EXPECT_EQ( written.size(), 1 ) << written.join( "," ).toStdString();

   // and the run's own XML names it
   QString expPath = targetDir + "/results/" + runID + "/" + runID + "."
                     + runType + ".xml";
   ASSERT_TRUE( QFile::exists( expPath ) );

   QFile file( expPath );
   ASSERT_TRUE( file.open( QIODevice::ReadOnly ) );
   QString content = QString::fromUtf8( file.readAll() );
   file.close();

   EXPECT_TRUE ( content.contains( mineGUID    ) ) << content.toStdString();
   EXPECT_TRUE ( content.contains( "A project of my own" ) );
   EXPECT_FALSE( content.contains( projectGUID ) );
}

// A project the target does not have is refused rather than created: being
// told which project to use only means anything if it is really there.
TEST_F( DataPubImport, AnUnknownTargetProjectIsRefused )
{
   QString error;

   US_DataPubImporter          importer;
   US_DataPubImporter::Options options = diskOptions();
   options.projectGUID = US_Util::new_guid();

   ASSERT_TRUE ( importer.inspect  ( bundlePath, error ) );
   EXPECT_FALSE( importer.runImport( options, error ) );
   EXPECT_TRUE ( error.contains( options.projectGUID ) )
      << error.toStdString();
}

// Without a chosen project nothing changes: the bundle still decides.
TEST_F( DataPubImport, WithoutAChosenProjectTheBundleStillDecides )
{
   QString error;

   US_DataPubImporter importer;
   ASSERT_TRUE( importer.inspect  ( bundlePath, error ) );
   ASSERT_TRUE( importer.runImport( diskOptions(), error ) )
      << error.toStdString();

   QString expPath = targetDir + "/results/" + runID + "/" + runID + "."
                     + runType + ".xml";
   QFile file( expPath );
   ASSERT_TRUE( file.open( QIODevice::ReadOnly ) );
   QString content = QString::fromUtf8( file.readAll() );
   file.close();

   EXPECT_TRUE( content.contains( projectGUID ) ) << content.toStdString();
}

// ------------------------------------------------ bundles from elsewhere
//
// A bundle is data from another installation, or from anyone.  These tests
// take the fixture's bundle apart, change what a hostile or broken exporter
// could have written, pack it again, and check that the import refuses it
// before it writes anything.

class DataPubCrafted : public DataPubImport
{
   protected:
      typedef std::function< void( US_DataPubManifest&, const QString& ) >
              Change;

      static bool copyTree( const QString& from, const QString& to )
      {
         QDir src( from );
         QDir().mkpath( to );

         QFileInfoList list = src.entryInfoList(
               QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot );

         for ( int ii = 0; ii < list.size(); ii++ )
         {
            QString target = to + "/" + list[ ii ].fileName();

            if ( list[ ii ].isDir() )
            {
               if ( ! copyTree( list[ ii ].absoluteFilePath(), target ) )
                  return false;
            }

            else if ( ! QFile::copy( list[ ii ].absoluteFilePath(), target ) )
               return false;

            QFile::setPermissions( target, QFileDevice::ReadOwner
                                           | QFileDevice::WriteOwner
                                           | QFileDevice::ExeOwner );
         }

         return true;
      }

      // The fixture's bundle, changed and packed again under a new name
      QString craft( const QString& name, Change change )
      {
         US_DataPubImporter source;
         QString            error;

         if ( ! source.inspect( bundlePath, error ) )  return QString();

         US_DataPubBundle out;

         if ( ! out.createStaging( error ) )  return QString();

         QString stage = out.stagingPath();

         if ( ! copyTree( source.bundleRoot(), stage ) )  return QString();

         US_DataPubManifest mani;
         QString            mpath = stage + "/"
                                    + US_DataPubBundle::manifestName();

         if ( ! mani.read( mpath, error ) )  return QString();

         change( mani, stage );

         if ( ! mani.write( mpath, error ) )  return QString();

         // A crafted manifest gets a matching manifest digest, so only the
         // check under test can catch it
         QFile digest( stage + "/" + US_DataPubExporter::manifestDigestName() );

         if ( ! digest.open( QIODevice::WriteOnly | QIODevice::Truncate ) )
            return QString();

         digest.write( ( US_DataPubHash::fileHash( mpath ) + "  "
                         + US_DataPubBundle::manifestName() + "\n" ).toUtf8() );
         digest.close();

         QString path = root + "/" + name + ".tar.gz";

         if ( ! out.pack( path, error ) )  return QString();

         return path;
      }

      // Change the first entity of a type in a manifest
      static void editEntity( US_DataPubManifest& mani,
                              US_DataPub::EntityType type,
                              std::function< void( US_DataPubEntity& ) > edit )
      {
         QList< US_DataPubEntity > list = mani.section( type );

         if ( list.isEmpty() )  return;

         edit( list[ 0 ] );
         mani.setSection( type, list );
      }

      // Replace text in the payload of the first entity of a type, and
      // digest it again, so only the content check can catch it
      static void editPayload( US_DataPubManifest& mani, const QString& stage,
                               US_DataPub::EntityType type,
                               const QString& from, const QString& to )
      {
         editEntity( mani, type, [ & ]( US_DataPubEntity& entity )
         {
            QString path = stage + "/" + entity.payload;
            QFile   file( path );

            if ( ! file.open( QIODevice::ReadOnly ) )  return;

            QString text = QString::fromUtf8( file.readAll() );
            file.close();
            text.replace( from, to );

            if ( ! file.open( QIODevice::WriteOnly | QIODevice::Truncate ) )
               return;

            file.write( text.toUtf8() );
            file.close();
            entity.sha256 = US_DataPubHash::fileHash( path );
         } );
      }

      // Inspect must refuse, with a message holding the given text
      void expectRefused( const QString& path, const QString& text )
      {
         ASSERT_FALSE( path.isEmpty() );

         US_DataPubImporter importer;
         QString            error;

         // The manifest reader refuses most of these itself; the importer
         // checks again.  Either message will do, as long as it is refused.
         EXPECT_FALSE( importer.inspect( path, error ) );
         EXPECT_TRUE ( error.contains( text )  ||
                       error.contains( "cannot be used" ) )
            << error.toStdString();
      }

      // Inspect may pass, but the import must fail and write nothing
      void expectImportFails( const QString& path, const QString& text,
                              bool verify = true )
      {
         ASSERT_FALSE( path.isEmpty() );

         US_DataPubImporter          importer;
         US_DataPubImporter::Options options = diskOptions();
         options.verifyHashes = verify;
         QString                     error;

         ASSERT_TRUE ( importer.inspect( path, error ) ) << error.toStdString();
         EXPECT_FALSE( importer.runImport( options, error ) );
         EXPECT_TRUE ( error.contains( text ) ) << error.toStdString();
         EXPECT_EQ   ( filesIn( targetDir ), 0 );
      }
};

TEST_F( DataPubCrafted, APayloadOutsideTheBundleIsRefused )
{
   expectRefused( craft( "up", [ & ]( US_DataPubManifest& m, const QString& )
   {
      editEntity( m, US_DataPub::Model, []( US_DataPubEntity& e )
      { e.payload = "models/../../../../etc/passwd"; } );
   } ), "leaves the bundle" );
}

TEST_F( DataPubCrafted, AnAbsolutePayloadIsRefused )
{
   expectRefused( craft( "abs", [ & ]( US_DataPubManifest& m, const QString& )
   {
      editEntity( m, US_DataPub::Buffer, []( US_DataPubEntity& e )
      { e.payload = "/etc/passwd"; } );
   } ), "not a relative path" );
}

// An archive could carry a symbolic link; whether or not the archive code
// lets one through, the import must not follow it out of the bundle
TEST_F( DataPubCrafted, APayloadLinkedOutOfTheBundleIsRefused )
{
   QString secret = root + "/secret.xml";
   QFile   file( secret );
   ASSERT_TRUE( file.open( QIODevice::WriteOnly ) );
   file.write( "<secret/>" );
   file.close();

   US_DataPubImporter importer;
   QString            error;
   ASSERT_TRUE( importer.inspect( bundlePath, error ) ) << error.toStdString();

   US_DataPubEntity buffer;
   ASSERT_TRUE( importer.manifest().entity( US_DataPub::Buffer, bufferGUID,
                                            buffer ) );
   QString payload = importer.bundleRoot() + "/" + buffer.payload;
   ASSERT_TRUE( QFile::remove( payload ) );
   ASSERT_TRUE( QFile::link( secret, payload ) );

   US_DataPubImporter::Options options = diskOptions();
   options.verifyHashes = false;

   EXPECT_FALSE( importer.runImport( options, error ) );
   EXPECT_TRUE ( error.contains( "outside the bundle" )  ||
                 error.contains( "missing" ) ) << error.toStdString();
   EXPECT_EQ   ( filesIn( targetDir ), 0 );
}

TEST_F( DataPubCrafted, TimeStateDefinitionsOutsideTheBundleAreRefused )
{
   expectRefused( craft( "defs", [ & ]( US_DataPubManifest& m, const QString& )
   {
      editEntity( m, US_DataPub::TimeState, []( US_DataPubEntity& e )
      { e.attrs.insert( "definitionsPayload", "../defs.xml" ); } );
   } ), "leaves the bundle" );
}

TEST_F( DataPubCrafted, AnUnusableRunIdIsRefused )
{
   expectRefused( craft( "dotted", [ & ]( US_DataPubManifest& m,
                                          const QString& )
   {
      editEntity( m, US_DataPub::Experiment, []( US_DataPubEntity& e )
      { e.name = "demo.run"; } );
   } ), "run ID" );

   expectRefused( craft( "slash", [ & ]( US_DataPubManifest& m,
                                         const QString& )
   {
      editEntity( m, US_DataPub::RawData, []( US_DataPubEntity& e )
      { e.attrs.insert( "runID", "../../escape" ); } );
   } ), "run ID" );
}

TEST_F( DataPubCrafted, ARunFileNameMustBeAPlainNameUnderItsRun )
{
   expectRefused( craft( "sep", [ & ]( US_DataPubManifest& m, const QString& )
   {
      editEntity( m, US_DataPub::EditedData, []( US_DataPubEntity& e )
      { e.filename = e.name = "demo_run/../../../evil.xml"; } );
   } ), "plain file name" );

   expectRefused( craft( "prefix", [ & ]( US_DataPubManifest& m,
                                          const QString& )
   {
      editEntity( m, US_DataPub::RawData, []( US_DataPubEntity& e )
      { e.filename = e.name = "other_run.RA.1.A.280.auc"; } );
   } ), "does not start with its run ID" );
}

TEST_F( DataPubCrafted, APayloadWithoutADigestIsNotIntact )
{
   QString path = craft( "nosha", [ & ]( US_DataPubManifest& m,
                                         const QString& )
   {
      editEntity( m, US_DataPub::Buffer, []( US_DataPubEntity& e )
      { e.sha256.clear(); } );
   } );

   expectImportFails( path, "carries no digest" );

   // Switching the check off is the way to import such a bundle anyway
   US_DataPubImporter          importer;
   US_DataPubImporter::Options options = diskOptions();
   options.verifyHashes = false;
   QString                     error;

   ASSERT_TRUE( importer.inspect  ( path, error ) );
   EXPECT_TRUE( importer.runImport( options, error ) ) << error.toStdString();
}

TEST_F( DataPubCrafted, TamperedTimeStateDefinitionsAreCaught )
{
   expectImportFails( craft( "tdefs", [ & ]( US_DataPubManifest& m,
                                             const QString& stage )
   {
      editEntity( m, US_DataPub::TimeState, [ & ]( US_DataPubEntity& e )
      {
         QFile file( stage + "/" + e.attrs.value( "definitionsPayload" ) );
         ASSERT_TRUE( file.open( QIODevice::Append ) );
         file.write( "<!-- tampered -->" );
         file.close();
      } );
   } ), "time_state.xml does not match its digest" );
}

TEST_F( DataPubCrafted, MissingTimeStateDefinitionsFailTheImport )
{
   // Even with the digest check off, a time state without its definitions
   // is no use and is not written
   expectImportFails( craft( "nodefs", [ & ]( US_DataPubManifest& m,
                                              const QString& stage )
   {
      editEntity( m, US_DataPub::TimeState, [ & ]( US_DataPubEntity& e )
      { QFile::remove( stage + "/" + e.attrs.value( "definitionsPayload" ) ); } );
   } ), "definitions", false );
}

// The library writers upsert by the GUID inside a payload: one that names a
// record other than its manifest entry could overwrite that record
TEST_F( DataPubCrafted, APayloadOfAnotherGuidIsRefused )
{
   QString other = US_Util::new_guid();

   expectImportFails( craft( "bufguid", [ & ]( US_DataPubManifest& m,
                                               const QString& stage )
   {
      editPayload( m, stage, US_DataPub::Buffer, bufferGUID, other );
   } ), "carries the GUID" );

   expectImportFails( craft( "editguid", [ & ]( US_DataPubManifest& m,
                                                const QString& stage )
   {
      editPayload( m, stage, US_DataPub::EditedData, editGUID, other );
   } ), "carries the GUID" );

   expectImportFails( craft( "noiseguid", [ & ]( US_DataPubManifest& m,
                                                 const QString& stage )
   {
      editPayload( m, stage, US_DataPub::Noise, noiseGUID, other );
   } ), "carries the GUID" );
}

TEST_F( DataPubCrafted, AMalformedPayloadIsRefused )
{
   expectImportFails( craft( "garbage", [ & ]( US_DataPubManifest& m,
                                               const QString& stage )
   {
      editPayload( m, stage, US_DataPub::Project, "<project", "<nothing" );
   } ), "does not parse" );
}

// A solution naming an analyte that neither the bundle nor the target has
// would fail half way through; it is caught before anything is written
TEST_F( DataPubCrafted, ASolutionsMissingAnalyteIsFoundFirst )
{
   QString stranger = US_Util::new_guid();

   expectImportFails( craft( "noana", [ & ]( US_DataPubManifest& m,
                                             const QString& stage )
   {
      editPayload( m, stage, US_DataPub::Solution, analyteGUID, stranger );
   } ), stranger );
}

// ------------------------------------------------ renames and remapping

// A run whose ID the target already uses for another run is imported under
// a new run ID, and everything that carries the run ID follows it
TEST_F( DataPubImport, ARenamedRunTakesItsFilesAlong )
{
   QString error;

   QString other = targetDir + "/results/" + runID;
   QString oxml  = other + "/" + runID + "." + runType + ".xml";
   QDir().mkpath( other );
   ASSERT_TRUE( QFile::copy( US_Settings::resultDir() + "/" + runID + "/"
                             + runID + "." + runType + ".xml", oxml ) );
   QFile::setPermissions( oxml, QFileDevice::ReadOwner
                                | QFileDevice::WriteOwner );

   QFile xml( oxml );
   ASSERT_TRUE( xml.open( QIODevice::ReadOnly ) );
   QString text = QString::fromUtf8( xml.readAll() );
   xml.close();
   text.replace( expGUID, US_Util::new_guid() );      // another run
   ASSERT_TRUE( xml.open( QIODevice::WriteOnly | QIODevice::Truncate ) );
   xml.write( text.toUtf8() );
   xml.close();

   // An edit made by us_edit names its run inside; the fixture's does not,
   // so the source edit gets the element and the bundle is made again
   QString sedit = US_Settings::resultDir() + "/" + runID + "/" + editFile;
   QFile   sfile( sedit );
   ASSERT_TRUE( sfile.open( QIODevice::ReadOnly ) );
   QString stext = QString::fromUtf8( sfile.readAll() );
   sfile.close();
   stext.replace( "<editGUID", "<runid value=\"" + runID + "\"/><editGUID" );
   ASSERT_TRUE( sfile.open( QIODevice::WriteOnly | QIODevice::Truncate ) );
   sfile.write( stext.toUtf8() );
   sfile.close();

   QString withRunid = root + "/runid.tar.gz";
   {
      US_DataPubExporter            exporter;
      US_DataPubExporter::Selection selection;
      selection.fromDb = false;
      selection.scope  = US_DataPub::ScopeNoise;
      selection.runIDs << runID;
      ASSERT_TRUE( exporter.exportBundle( selection, withRunid, error ) )
         << error.toStdString();
   }

   US_DataPubImporter          importer;
   US_DataPubImporter::Options options = diskOptions();
   options.renameSuffix = "my copy";                  // not usable as is

   ASSERT_TRUE( importer.inspect  ( withRunid, error ) );
   ASSERT_TRUE( importer.runImport( options, error ) ) << error.toStdString();

   QString newRun;
   QList< US_DataPubImporter::Result > results = importer.results();

   for ( int ii = 0; ii < results.size(); ii++ )
   {
      if ( results[ ii ].type != US_DataPub::Experiment )  continue;

      EXPECT_EQ( results[ ii ].resolution, US_DataPub::ResolvedRenamed );
      newRun = results[ ii ].targetName;
   }

   ASSERT_FALSE( newRun.isEmpty() );
   EXPECT_TRUE ( US_DataPubImporter::runIdIsUsable( newRun ) )
      << newRun.toStdString();
   EXPECT_EQ   ( newRun, runID + "-my_copy" );

   QString dir = targetDir + "/results/" + newRun;
   EXPECT_TRUE( QFile::exists( dir + "/" + newRun + rawFile.mid( runID.size() ) ) );
   EXPECT_TRUE( QFile::exists( dir + "/" + newRun + "." + runType + ".xml" ) );
   EXPECT_TRUE( QFile::exists( dir + "/" + newRun + ".time_state.tmst" ) );
   EXPECT_TRUE( QFile::exists( dir + "/" + newRun + ".time_state.xml" ) );

   // The edit names the new run inside, too
   EXPECT_TRUE( QFile::exists( dir + "/" + newRun + editFile.mid( runID.size() ) ) )
      << QDir( dir ).entryList().join( " " ).toStdString();
   QFile edit( dir + "/" + newRun + editFile.mid( runID.size() ) );
   ASSERT_TRUE( edit.open( QIODevice::ReadOnly ) );
   QString etext = QString::fromUtf8( edit.readAll() );
   edit.close();
   EXPECT_TRUE ( etext.contains( "value=\"" + newRun + "\"" ) )
      << etext.toStdString();
   EXPECT_FALSE( etext.contains( "value=\"" + runID + "\"" ) );

   // and so does the model named after it
   QStringList models = QDir( targetDir + "/data/models" ).entryList(
                        QStringList( "M*.xml" ), QDir::Files );
   ASSERT_EQ( models.size(), 1 );
   EXPECT_TRUE( US_DataPubRecords::recordName( targetDir + "/data/models/"
                + models[ 0 ], US_DataPub::Model ).startsWith( newRun + "." ) );
}

// A buffer and an analyte the target already holds under other GUIDs are
// reused, and the solution imported with them names the target's records
TEST_F( DataPubImport, AReusedParentIsNamedByItsTargetGuid )
{
   QString error;

   QDir().mkpath( targetDir + "/data/buffers" );
   QDir().mkpath( targetDir + "/data/analytes" );

   QString myBuffer  = US_Util::new_guid();
   QString myAnalyte = US_Util::new_guid();

   US_Buffer buffer;
   ASSERT_TRUE( buffer.readFromDisk( US_Settings::dataDir()
                                     + "/buffers/B0000001.xml" ) );
   buffer.GUID     = myBuffer;
   buffer.bufferID = "44";
   buffer.writeToDisk( targetDir + "/data/buffers/B0000001.xml" );

   US_Analyte analyte;
   ASSERT_EQ( analyte.readFromFile( US_Settings::dataDir()
                                    + "/analytes/A0000001.xml" ), US_DB2::OK );
   analyte.analyteGUID = myAnalyte;
   analyte.write( false, targetDir + "/data/analytes/A0000001.xml" );

   US_DataPubImporter importer;
   ASSERT_TRUE( importer.inspect  ( bundlePath, error ) );
   ASSERT_TRUE( importer.runImport( diskOptions(), error ) )
      << error.toStdString();

   QStringList sols = QDir( targetDir + "/data/solutions" ).entryList(
                      QStringList( "S*.xml" ), QDir::Files );
   ASSERT_EQ( sols.size(), 1 );

   US_Solution solution;
   ASSERT_EQ( solution.readFromFile( targetDir + "/data/solutions/"
                                     + sols[ 0 ], false ), US_DB2::OK );
   EXPECT_EQ( solution.buffer.GUID, myBuffer );
   EXPECT_NE( solution.buffer.bufferID, "3" );   // the source's ID
   ASSERT_EQ( solution.analyteInfo.size(), 1 );
   EXPECT_EQ( solution.analyteInfo[ 0 ].analyte.analyteGUID, myAnalyte );
   EXPECT_EQ( solution.solutionGUID, solutionGUID );

   // A disk store gets no numeric ID from the source
   EXPECT_EQ( solution.solutionID, -1 );
}

// A solution of a different buffer is a different solution: the fingerprint
// compares the parents too, once both name the target's records
TEST_F( DataPubImport, ASolutionOfAnotherBufferIsNotReused )
{
   QString error;

   QDir().mkpath( targetDir + "/data/solutions" );

   US_Solution same;
   ASSERT_EQ( same.readFromFile( US_Settings::dataDir()
                                 + "/solutions/S0000001.xml", false ),
              US_DB2::OK );
   same.solutionGUID = US_Util::new_guid();
   same.buffer.GUID  = US_Util::new_guid();        // another buffer
   ASSERT_TRUE( same.saveToFile( targetDir + "/data/solutions/S0000001.xml" ) );

   US_DataPubImporter importer;
   ASSERT_TRUE( importer.inspect  ( bundlePath, error ) );
   ASSERT_TRUE( importer.runImport( diskOptions(), error ) )
      << error.toStdString();

   bool seen = false;
   QList< US_DataPubImporter::Result > results = importer.results();

   for ( int ii = 0; ii < results.size(); ii++ )
   {
      if ( results[ ii ].type != US_DataPub::Solution )  continue;

      seen = true;
      EXPECT_EQ( results[ ii ].resolution, US_DataPub::ResolvedRenamed );
   }

   EXPECT_TRUE( seen );
}

// A target record without a GUID can never be what a bundle record is
TEST_F( DataPubImport, ARecordWithoutAGuidIsNeverReused )
{
   QString error;

   QString projDir = targetDir + "/data/projects";
   QString ppath   = projDir + "/P0000001.xml";
   QDir().mkpath( projDir );
   ASSERT_TRUE( QFile::copy( US_Settings::dataDir() + "/projects/P0000001.xml",
                             ppath ) );
   QFile::setPermissions( ppath, QFileDevice::ReadOwner
                                 | QFileDevice::WriteOwner );

   QFile file( ppath );
   ASSERT_TRUE( file.open( QIODevice::ReadOnly ) );
   QString text = QString::fromUtf8( file.readAll() );
   file.close();
   ASSERT_TRUE( text.contains( projectGUID ) );
   text.replace( projectGUID, QString() );
   ASSERT_TRUE( file.open( QIODevice::WriteOnly | QIODevice::Truncate ) );
   file.write( text.toUtf8() );
   file.close();

   US_DataPubImporter importer;
   ASSERT_TRUE( importer.inspect  ( bundlePath, error ) );
   ASSERT_TRUE( importer.runImport( diskOptions(), error ) )
      << error.toStdString();

   bool seen = false;
   QList< US_DataPubImporter::Result > results = importer.results();

   for ( int ii = 0; ii < results.size(); ii++ )
   {
      if ( results[ ii ].type != US_DataPub::Project )  continue;

      seen = true;
      EXPECT_EQ   ( results[ ii ].resolution, US_DataPub::ResolvedRenamed );
      EXPECT_FALSE( results[ ii ].targetGUID.isEmpty() );
   }

   EXPECT_TRUE( seen );
}

TEST_F( DataPubImport, RunIdsAreCheckedByTheConvertRule )
{
   EXPECT_TRUE ( US_DataPubImporter::runIdIsUsable( "demo_run-2" ) );
   EXPECT_FALSE( US_DataPubImporter::runIdIsUsable( "demo.run" ) );
   EXPECT_FALSE( US_DataPubImporter::runIdIsUsable( "demo run" ) );
   EXPECT_FALSE( US_DataPubImporter::runIdIsUsable( "../demo" ) );
   EXPECT_FALSE( US_DataPubImporter::runIdIsUsable( "" ) );
   EXPECT_FALSE( US_DataPubImporter::runIdIsUsable( QString( 81, 'a' ) ) );
}

// A disk store gets no numeric IDs from the source: the records the import
// writes carry -1, and the run names them by that
TEST_F( DataPubImport, ADiskImportCarriesNoSourceIds )
{
   QString error;

   US_DataPubImporter importer;
   ASSERT_TRUE( importer.inspect  ( bundlePath, error ) );
   ASSERT_TRUE( importer.runImport( diskOptions(), error ) )
      << error.toStdString();

   US_Project project;
   ASSERT_EQ( project.readFromFile( targetDir + "/data/projects/P0000001.xml" ),
              US_DB2::OK );
   EXPECT_EQ( project.projectID, -1 );

   QFile file( targetDir + "/results/" + runID + "/" + runID + "." + runType
               + ".xml" );
   ASSERT_TRUE( file.open( QIODevice::ReadOnly ) );
   QString content = QString::fromUtf8( file.readAll() );
   file.close();

   EXPECT_FALSE( content.contains( "id=\"7\"" ) )  << content.toStdString();
   EXPECT_FALSE( content.contains( "id=\"11\"" ) ) << content.toStdString();
   EXPECT_FALSE( content.contains( "id=\"21\"" ) ) << content.toStdString();
}

// ------------------------------------------------------ the command line

class DataPubCliImport : public DataPubImport
{
   protected:
      int run( const QStringList& args )
      {
         US_DataPubCLI cli;
         return cli.run( QStringList( "us_data_publication" ) << args );
      }
};

TEST_F( DataPubCliImport, ImportNeedsABundle )
{
   EXPECT_EQ( run( QStringList() << "--mode" << "import" ), 2 );
}

TEST_F( DataPubCliImport, ImportRefusesAnUnknownTargetOrPolicy )
{
   EXPECT_EQ( run( QStringList() << "--mode" << "import" << "--bundle"
                   << bundlePath << "--target" << "cloud" ), 2 );
   EXPECT_EQ( run( QStringList() << "--mode" << "import" << "--bundle"
                   << bundlePath << "--target" << "disk"
                   << "--on-conflict" << "overwrite" ), 2 );
   EXPECT_EQ( run( QStringList() << "--mode" << "import" << "--bundle"
                   << bundlePath << "--target" << "disk"
                   << "--on-conflict-buffers" << "overwrite" ), 2 );

   EXPECT_EQ( filesIn( targetDir ), 0 );
}

TEST_F( DataPubCliImport, ADatabaseImportWithoutAPasswordStopsEarly )
{
   QByteArray saved = qgetenv( "US3_DB_PASSWORD" );
   qunsetenv( "US3_DB_PASSWORD" );

   EXPECT_EQ( run( QStringList() << "--mode" << "import" << "--bundle"
                   << bundlePath << "--target" << "db"
                   << "--non-interactive" ), 2 );

   if ( ! saved.isEmpty() )  qputenv( "US3_DB_PASSWORD", saved );
}

#if defined( Q_OS_UNIX )
// The password is settled before the bundle is even unpacked, with one
// error that recommends the safe ways to give it
TEST_F( DataPubCliImport, TheDatabasePasswordIsCheckedFirstWithOneError )
{
   QByteArray saved = qgetenv( "US3_DB_PASSWORD" );
   qunsetenv( "US3_DB_PASSWORD" );

   QString capture = root + "/stderr.txt";
   fflush( stderr );
   int     keep = dup( fileno( stderr ) );
   FILE*   file = fopen( capture.toLocal8Bit().constData(), "w" );
   ASSERT_NE( file, nullptr );
   dup2( fileno( file ), fileno( stderr ) );

   // A bundle that does not exist would be the error if inspect came first
   int code = run( QStringList() << "--mode" << "import" << "--bundle"
                   << root + "/no_such.tar.gz" << "--target" << "db"
                   << "--non-interactive" );

   fflush( stderr );
   dup2( keep, fileno( stderr ) );
   ::close( keep );
   fclose( file );

   if ( ! saved.isEmpty() )  qputenv( "US3_DB_PASSWORD", saved );

   QFile   out( capture );
   ASSERT_TRUE( out.open( QIODevice::ReadOnly ) );
   QString text = QString::fromUtf8( out.readAll() );

   EXPECT_EQ   ( code, 2 ) << text.toStdString();
   EXPECT_EQ   ( text.count( "Error:" ), 1 ) << text.toStdString();
   EXPECT_TRUE ( text.contains( "--db-password-file" ) ) << text.toStdString();
   EXPECT_FALSE( text.contains( "no_such" ) ) << text.toStdString();
}
#endif

// A password file that cannot be read is a failure (1), not a usage error
// (2), as any other unreadable input is
TEST_F( DataPubCliImport, AnUnreadablePasswordFileFailsTheImport )
{
   EXPECT_EQ( run( QStringList() << "--mode" << "import" << "--bundle"
                   << bundlePath << "--target" << "db" << "--non-interactive"
                   << "--db-password-file" << root + "/no_such_password" ),
              1 );
}

TEST_F( DataPubCliImport, ADiskImportSucceedsAndAFailedOneSaysSo )
{
   EXPECT_EQ( run( QStringList() << "--mode" << "import" << "--bundle"
                   << bundlePath << "--target" << "disk" << "--output-dir"
                   << targetDir << "--strict-hardware" << "--dry-run" ), 0 );
   EXPECT_EQ( filesIn( targetDir ), 0 );

   EXPECT_EQ( run( QStringList() << "--mode" << "import" << "--bundle"
                   << bundlePath << "--target" << "disk" << "--output-dir"
                   << targetDir ), 0 );
   EXPECT_GT( filesIn( targetDir ), 0 );

   EXPECT_EQ( run( QStringList() << "--mode" << "import" << "--bundle"
                   << root + "/no_such.tar.gz" << "--target" << "disk"
                   << "--output-dir" << targetDir ), 1 );
}

// ------------------------------------------- the manifest and extra payloads

// The manifest is checked against manifest.sha256 like any payload
TEST_F( DataPubImport, AnAlteredManifestIsCaught )
{
   US_DataPubImporter importer;
   QString            error;
   ASSERT_TRUE( importer.inspect( bundlePath, error ) ) << error.toStdString();

   QFile file( importer.bundleRoot() + "/" + US_DataPubBundle::manifestName() );
   ASSERT_TRUE( file.open( QIODevice::Append ) );
   file.write( "# edited after export\n" );
   file.close();

   EXPECT_FALSE( importer.runImport( diskOptions(), error ) );
   EXPECT_TRUE ( error.contains( "manifest does not match" ) )
      << error.toStdString();
   EXPECT_EQ   ( filesIn( targetDir ), 0 );
}

TEST_F( DataPubImport, AManifestWithoutItsDigestIsCaught )
{
   US_DataPubImporter importer;
   QString            error;
   ASSERT_TRUE( importer.inspect( bundlePath, error ) ) << error.toStdString();
   ASSERT_TRUE( QFile::remove( importer.bundleRoot() + "/"
                + US_DataPubExporter::manifestDigestName() ) );

   EXPECT_FALSE( importer.runImport( diskOptions(), error ) );
   EXPECT_TRUE ( error.contains( US_DataPubExporter::manifestDigestName() ) )
      << error.toStdString();
}

// An RI run's intensity profile goes into the run directory beside the
// experiment XML, under the run's (possibly new) ID
TEST_F( DataPubImport, AnRiProfileIsRestoredBesideTheRun )
{
   QString rip = runDir() + "/" + runID + ".RIProfile.xml";
   QFile   file( rip );
   ASSERT_TRUE( file.open( QIODevice::WriteOnly ) );
   file.write( "<?xml version=\"1.0\"?>\n<!DOCTYPE US_RIProfile>\n"
               "<US_RIProfile version=\"1.0\"><RI value=\"1.5\"/>"
               "<RI value=\"1.25\"/></US_RIProfile>\n" );
   file.close();

   QString bundle = root + "/ri.tar.gz";
   QString error;
   {
      US_DataPubExporter            exporter;
      US_DataPubExporter::Selection selection;
      selection.fromDb = false;
      selection.scope  = US_DataPub::ScopeNoise;
      selection.runIDs << runID;
      ASSERT_TRUE( exporter.exportBundle( selection, bundle, error ) )
         << error.toStdString();
   }

   US_DataPubImporter importer;
   ASSERT_TRUE( importer.inspect  ( bundle, error ) ) << error.toStdString();
   ASSERT_TRUE( importer.runImport( diskOptions(), error ) )
      << error.toStdString();

   QString restored = targetDir + "/results/" + runID + "/" + runID
                      + ".RIProfile.xml";
   ASSERT_TRUE( QFile::exists( restored ) );
   EXPECT_EQ  ( US_DataPubHash::fileHash( restored ),
                US_DataPubHash::fileHash( rip ) );
}

// A buffer's components are found in the target by what they are, not by
// the number the source happened to give them
TEST_F( DataPubCrafted, BufferComponentsAreMatchedByDefinition )
{
   QMap< QString, US_BufferComponent > table;
   US_BufferComponent::getAllFromHD( table );
   ASSERT_TRUE( table.contains( "1" ) ) << "no local buffer component table";

   US_Buffer buffer;
   ASSERT_TRUE( buffer.readFromDisk( US_Settings::dataDir()
                                     + "/buffers/B0000001.xml" ) );
   buffer.component     << table.value( "1" );
   buffer.componentIDs  << "1";
   buffer.concentration << 150.0;
   ASSERT_TRUE( buffer.writeToDisk( US_Settings::dataDir()
                                    + "/buffers/B0000001.xml" ) );

   QString error;
   {
      US_DataPubExporter            exporter;
      US_DataPubExporter::Selection selection;
      selection.fromDb = false;
      selection.scope  = US_DataPub::ScopeNoise;
      selection.runIDs << runID;
      ASSERT_TRUE( exporter.exportBundle( selection, bundlePath, error ) )
         << error.toStdString();
   }

   // The source called component 1 "999"
   QString path = craft( "renumbered", [ & ]( US_DataPubManifest& m,
                                              const QString& stage )
   {
      editPayload( m, stage, US_DataPub::Buffer, "id=\"1\"", "id=\"999\"" );
      editEntity( m, US_DataPub::Buffer, [ & ]( US_DataPubEntity& e )
      {
         QString cpath = stage + "/" + e.attrs.value( "componentsPayload" );
         QFile   cfile( cpath );
         ASSERT_TRUE( cfile.open( QIODevice::ReadOnly ) );
         QString text = QString::fromUtf8( cfile.readAll() );
         cfile.close();
         text.replace( "id=\"1\"", "id=\"999\"" );
         ASSERT_TRUE( cfile.open( QIODevice::WriteOnly | QIODevice::Truncate ) );
         cfile.write( text.toUtf8() );
         cfile.close();
         e.attrs.insert( "componentsSha256", US_DataPubHash::fileHash( cpath ) );
      } );
   } );
   ASSERT_FALSE( path.isEmpty() );

   US_DataPubImporter importer;
   ASSERT_TRUE( importer.inspect  ( path, error ) ) << error.toStdString();
   ASSERT_TRUE( importer.runImport( diskOptions(), error ) )
      << error.toStdString();

   QStringList bufs = QDir( targetDir + "/data/buffers" ).entryList(
                      QStringList( "B*.xml" ), QDir::Files );
   ASSERT_EQ( bufs.size(), 1 );

   US_Buffer imported;
   ASSERT_TRUE( imported.readFromDisk( targetDir + "/data/buffers/"
                                       + bufs[ 0 ] ) );
   EXPECT_EQ( imported.componentIDs, QStringList() << "1" );
}

// A component digest is checked like the buffer's own
TEST_F( DataPubCrafted, AnExtraPayloadWithoutADigestIsNotIntact )
{
   expectImportFails( craft( "notsdigest", [ & ]( US_DataPubManifest& m,
                                                  const QString& )
   {
      editEntity( m, US_DataPub::TimeState, []( US_DataPubEntity& e )
      { e.attrs.remove( "definitionsSha256" ); } );
   } ), "carries no digest" );
}

// ------------------------------------------- buffer components on disk

class DataPubComponents : public DataPubCrafted
{
   protected:
      US_BufferComponent salt;     // The component the fixture's buffer has
      QString            table;    // A component table of this test's own

      void SetUp() override
      {
         DataPubCrafted::SetUp();

         // A component the exporter can describe; it reads the
         // installation's table, and nothing here writes to that
         QMap< QString, US_BufferComponent > installed;
         US_BufferComponent::getAllFromHD( installed );
         ASSERT_TRUE( installed.contains( "1" ) )
            << "no local buffer component table";
         salt = installed.value( "1" );

         US_Buffer buffer;
         QString   bpath = US_Settings::dataDir() + "/buffers/B0000001.xml";
         ASSERT_TRUE( buffer.readFromDisk( bpath ) );
         buffer.component     << salt;
         buffer.componentIDs  << "1";
         buffer.concentration << 150.0;
         ASSERT_TRUE( buffer.writeToDisk( bpath ) );

         US_DataPubExporter            exporter;
         US_DataPubExporter::Selection selection;
         selection.fromDb = false;
         selection.scope  = US_DataPub::ScopeNoise;
         selection.runIDs << runID;

         QString error;
         ASSERT_TRUE( exporter.exportBundle( selection, bundlePath, error ) )
            << error.toStdString();

         QDir().mkpath( root + "/etc" );
         table = root + "/etc/bufferComponents.xml";
      }

      void writeTable( const QList< US_BufferComponent >& comps )
      {
         ASSERT_TRUE( US_DataPubRecords::writeBufferComponents( comps, table ) );
      }

      US_DataPubImporter::Options tableOptions()
      {
         US_DataPubImporter::Options options = diskOptions();
         options.componentTable = table;
         return options;
      }

      static QString installedTable()
      {
         return US_Settings::appBaseDir() + "/etc/bufferComponents.xml";
      }
};

// A component the table has under another ID is named by that ID, and the
// table is only read
TEST_F( DataPubComponents, AComponentIsMappedToTheTablesOwnId )
{
   US_BufferComponent other = salt;
   other.componentID = "3";
   other.name        = salt.name + " (other)";
   US_BufferComponent same = salt;
   same.componentID  = "7";
   writeTable( QList< US_BufferComponent >() << other << same );

   QString tableHash     = US_DataPubHash::fileHash( table );
   QString installedHash = US_DataPubHash::fileHash( installedTable() );

   US_DataPubImporter importer;
   QString            error;
   ASSERT_TRUE( importer.inspect  ( bundlePath, error ) ) << error.toStdString();
   ASSERT_TRUE( importer.runImport( tableOptions(), error ) )
      << error.toStdString();

   QStringList bufs = QDir( targetDir + "/data/buffers" ).entryList(
                      QStringList( "B*.xml" ), QDir::Files );
   ASSERT_EQ( bufs.size(), 1 );

   US_Buffer imported;
   ASSERT_TRUE( imported.readFromDisk( targetDir + "/data/buffers/"
                                       + bufs[ 0 ] ) );
   EXPECT_EQ( imported.componentIDs, QStringList() << "7" );

   EXPECT_EQ( US_DataPubHash::fileHash( table ), tableHash );
   EXPECT_EQ( US_DataPubHash::fileHash( installedTable() ), installedHash );
}

// A component the table lacks -- or has with another range or gradient
// forming -- fails the import before anything is written, and the table is
// not changed to make room for it
TEST_F( DataPubComponents, AMissingComponentFailsTheImportUpFront )
{
   US_BufferComponent ranged = salt;
   ranged.range = salt.range + " (other range)";
   US_BufferComponent forming = salt;
   forming.grad_form = ! salt.grad_form;
   forming.componentID = "2";

   QList< QList< US_BufferComponent > > tables;
   tables << QList< US_BufferComponent >()
          << ( QList< US_BufferComponent >() << ranged )
          << ( QList< US_BufferComponent >() << forming );

   QString installedHash = US_DataPubHash::fileHash( installedTable() );

   for ( int ii = 0; ii < tables.size(); ii++ )
   {
      writeTable( tables[ ii ] );
      QString tableHash = US_DataPubHash::fileHash( table );

      US_DataPubImporter importer;
      QString            error;
      ASSERT_TRUE ( importer.inspect  ( bundlePath, error ) );
      EXPECT_FALSE( importer.runImport( tableOptions(), error ) ) << ii;
      EXPECT_TRUE ( error.contains( salt.name ) ) << error.toStdString();
      EXPECT_TRUE ( error.contains( "does not change" ) ) << error.toStdString();
      EXPECT_EQ   ( filesIn( targetDir ), 0 ) << ii;
      EXPECT_EQ   ( US_DataPubHash::fileHash( table ), tableHash ) << ii;
   }

   EXPECT_EQ( US_DataPubHash::fileHash( installedTable() ), installedHash );
}

// A bundle of the older format names components by the exporter's IDs
// alone; a buffer with components is refused before anything is written
TEST_F( DataPubComponents, AnOldBundleWithComponentsIsRefusedUpFront )
{
   writeTable( QList< US_BufferComponent >() << salt );

   expectImportFails( craft( "oldformat", [ & ]( US_DataPubManifest& m,
                                                 const QString& )
   {
      editEntity( m, US_DataPub::Buffer, []( US_DataPubEntity& e )
      {
         e.attrs.remove( "componentsPayload" );
         e.attrs.remove( "componentsSha256" );
         e.attrs.remove( "components" );
      } );
   } ), "older bundle format" );
}

// ------------------------------------------- what a manifest may name

// A GUID is a GUID: not any text, and not one with a newline after it
TEST_F( DataPubCrafted, AManifestGuidMustBeAGuid )
{
   // Nothing depends on a noise record, so only the GUID check can see it
   expectRefused( craft( "badguid", [ & ]( US_DataPubManifest& m,
                                           const QString& )
   {
      editEntity( m, US_DataPub::Noise, []( US_DataPubEntity& e )
      { e.guid = "not-a-guid"; } );
   } ), "is not a GUID" );

   expectRefused( craft( "nlguid", [ & ]( US_DataPubManifest& m,
                                          const QString& )
   {
      editEntity( m, US_DataPub::Noise, [ & ]( US_DataPubEntity& e )
      { e.guid = noiseGUID + "\n"; } );
   } ), "is not a GUID" );
}

// ... and so is every GUID a payload names
TEST_F( DataPubCrafted, APayloadGuidMustBeAGuid )
{
   expectImportFails( craft( "payloadguid", [ & ]( US_DataPubManifest& m,
                                                   const QString& stage )
   {
      editPayload( m, stage, US_DataPub::Noise, modelGUID,
                   QString( "model; drop table" ) );
   } ), "not a GUID" );
}

// A run type is a few letters and digits, and nothing after them
TEST_F( DataPubCrafted, ARunTypeWithATrailingNewlineIsRefused )
{
   expectRefused( craft( "runtype", [ & ]( US_DataPubManifest& m,
                                           const QString& )
   {
      editEntity( m, US_DataPub::Experiment, []( US_DataPubEntity& e )
      { e.attrs.insert( "runType", "RA\n" ); } );
   } ), "unusable run type" );
}

// A digest is hexadecimal; its case means nothing
TEST_F( DataPubCrafted, ADigestInCapitalsStillMatches )
{
   QString path = craft( "upper", [ & ]( US_DataPubManifest& m,
                                         const QString& )
   {
      editEntity( m, US_DataPub::Buffer, []( US_DataPubEntity& e )
      { e.sha256 = e.sha256.toUpper(); } );
   } );
   ASSERT_FALSE( path.isEmpty() );

   US_DataPubImporter importer;
   QString            error;
   ASSERT_TRUE( importer.inspect( path, error ) ) << error.toStdString();
   EXPECT_EQ  ( importer.verifyPayloads(), QStringList() );
   EXPECT_TRUE( importer.runImport( diskOptions(), error ) )
      << error.toStdString();
}

// A model names its edit; when the manifest does not tie it to one, the
// edit still has to be in the bundle or the target
TEST_F( DataPubCrafted, AModelsEditMustBeThereEvenWithoutADependency )
{
   QString nowhere = US_Util::new_guid();

   expectImportFails( craft( "noedit", [ & ]( US_DataPubManifest& m,
                                              const QString& stage )
   {
      editPayload( m, stage, US_DataPub::Model, editGUID, nowhere );
      editEntity( m, US_DataPub::Model, []( US_DataPubEntity& e )
      {
         QList< QString > keys = e.depends.keys();

         for ( int ii = 0; ii < keys.size(); ii++ )
            if ( US_DataPubEntity::dependType( keys[ ii ] )
                 == US_DataPub::EditedData )
               e.depends.remove( keys[ ii ] );
      } );
   } ), nowhere );
}

// ------------------------------------------- the order of the records

static int indexIn( const QList< US_DataPub::EntityType >& order,
                    US_DataPub::EntityType type )
{
   return order.indexOf( type );
}

// The order follows the dependencies of the target and of the records
TEST( DataPubImportOrder, TheOrderFollowsTheDependencies )
{
   US_DataPubManifest mani;
   QString            error;

   QList< US_DataPub::EntityType > disk =
         US_DataPubImporter::importOrder( mani, US_DataPub::TargetDisk, error );
   QList< US_DataPub::EntityType > db =
         US_DataPubImporter::importOrder( mani, US_DataPub::TargetDb, error );

   // A disk store's run XML names its solutions; a database's solution is
   // created with its run's ID
   EXPECT_LT( indexIn( disk, US_DataPub::Solution ),
              indexIn( disk, US_DataPub::Experiment ) );
   EXPECT_LT( indexIn( db,   US_DataPub::Experiment ),
              indexIn( db,   US_DataPub::Solution ) );

   QList< US_DataPub::EntityType > dbOrder;
   dbOrder << US_DataPub::Project    << US_DataPub::RotorCalibration
           << US_DataPub::Centerpiece << US_DataPub::Buffer
           << US_DataPub::Analyte    << US_DataPub::Experiment
           << US_DataPub::Solution   << US_DataPub::RawData
           << US_DataPub::TimeState  << US_DataPub::EditedData
           << US_DataPub::Model      << US_DataPub::Noise;
   EXPECT_EQ( db, dbOrder );

   for ( int pass = 0; pass < 2; pass++ )
   {
      const QList< US_DataPub::EntityType >& order = pass ? db : disk;

      EXPECT_LT( indexIn( order, US_DataPub::Buffer ),
                 indexIn( order, US_DataPub::Solution ) );
      EXPECT_LT( indexIn( order, US_DataPub::RawData ),
                 indexIn( order, US_DataPub::EditedData ) );
      EXPECT_LT( indexIn( order, US_DataPub::EditedData ),
                 indexIn( order, US_DataPub::Model ) );
      EXPECT_LT( indexIn( order, US_DataPub::Model ),
                 indexIn( order, US_DataPub::Noise ) );
   }

   // A dependency only the records name is followed too: here a project
   // that names a buffer, which no fixed list would put first
   US_DataPubEntity buffer( US_DataPub::Buffer );
   buffer.guid = US_Util::new_guid();
   US_DataPubEntity project( US_DataPub::Project );
   project.guid = US_Util::new_guid();
   project.setDepend( US_DataPub::Buffer, buffer.guid );
   mani.add( buffer );
   mani.add( project );

   disk = US_DataPubImporter::importOrder( mani, US_DataPub::TargetDisk,
                                           error );
   EXPECT_LT( indexIn( disk, US_DataPub::Buffer ),
              indexIn( disk, US_DataPub::Project ) ) << error.toStdString();

   // ... and one that goes round in a circle has no order at all
   US_DataPubEntity circle( US_DataPub::Buffer );
   circle.guid = US_Util::new_guid();
   circle.setDepend( US_DataPub::Project, project.guid );
   mani.add( circle );

   EXPECT_TRUE( US_DataPubImporter::importOrder( mani, US_DataPub::TargetDb,
                                                 error ).isEmpty() );
   EXPECT_TRUE( error.contains( "circle" ) ) << error.toStdString();
}

// ------------------------------------------- the import tab's rules

// The end of an import or an inspection leaves the chosen project alone;
// only a real change of target clears it
TEST( DataPubImportState, OnlyAChangeOfTargetClearsTheProject )
{
   US_DataPubImportState state( true );
   QString               guid = US_Util::new_guid();

   state.setProject( guid, "Mine" );
   state.setInspected( true );

   state.setBusy( true );
   EXPECT_FALSE( state.controlsEnabled() );
   EXPECT_FALSE( state.importEnabled() );
   EXPECT_FALSE( state.outputDirEnabled() );

   state.setBusy( false );
   EXPECT_EQ  ( state.projectGUID(), guid );
   EXPECT_TRUE( state.controlsEnabled() );
   EXPECT_TRUE( state.importEnabled() );

   EXPECT_FALSE( state.setTarget( true ) );      // the same target
   EXPECT_EQ   ( state.projectGUID(), guid );
   EXPECT_FALSE( state.outputDirEnabled() );

   EXPECT_TRUE ( state.setTarget( false ) );     // the other target
   EXPECT_TRUE ( state.projectGUID().isEmpty() );
   EXPECT_TRUE ( state.outputDirEnabled() );

   EXPECT_FALSE( state.setTarget( true ) );      // nothing left to clear
   state.setInspected( false );
   EXPECT_FALSE( state.importEnabled() );
}

// ------------------------------- what is already in a disk store's run

class DataPubDiskAgain : public DataPubImport
{
   protected:
      QString targetRun() { return targetDir + "/results/" + runID; }

      void importFirst()
      {
         US_DataPubImporter first;
         QString            error;
         ASSERT_TRUE( first.inspect  ( bundlePath, error ) );
         ASSERT_TRUE( first.runImport( diskOptions(), error ) )
            << error.toStdString();
      }

      // The second import of the same bundle fails, with this in its error
      void expectSecondFails( const QString& text )
      {
         US_DataPubImporter second;
         QString            error;
         ASSERT_TRUE ( second.inspect( bundlePath, error ) );
         EXPECT_FALSE( second.runImport( diskOptions(), error ) );
         EXPECT_TRUE ( error.contains( text ) ) << error.toStdString();
      }
};

// A time state is the pair of files: one whose definitions are missing or
// different is not the bundle's, whatever its binary holds
TEST_F( DataPubDiskAgain, ATimeStateIsTheSameOnlyWithItsDefinitions )
{
   importFirst();

   QString defs = targetRun() + "/" + runID + ".time_state.xml";
   ASSERT_TRUE( QFile::exists( defs ) );

   QFile file( defs );
   ASSERT_TRUE( file.open( QIODevice::ReadOnly ) );
   QByteArray bytes = file.readAll();
   file.close();

   // Different definitions
   ASSERT_TRUE( file.open( QIODevice::WriteOnly | QIODevice::Truncate ) );
   file.write( bytes + "<!-- changed -->\n" );
   file.close();

   expectSecondFails( "field definitions" );

   // No definitions at all
   ASSERT_TRUE( QFile::remove( defs ) );

   expectSecondFails( "field definitions" );

   // ... and with them back, the run is the same again
   ASSERT_TRUE( file.open( QIODevice::WriteOnly | QIODevice::Truncate ) );
   file.write( bytes );
   file.close();

   US_DataPubImporter third;
   QString            error;
   ASSERT_TRUE( third.inspect  ( bundlePath, error ) );
   EXPECT_TRUE( third.runImport( diskOptions(), error ) ) << error.toStdString();
}

// A payload that cannot even be copied to be compared is reported as that,
// not as a file of different content
TEST_F( DataPubDiskAgain, AFailedComparisonSaysWhy )
{
   importFirst();

   US_DataPubImporter second;
   QString            error;
   ASSERT_TRUE( second.inspect( bundlePath, error ) );

   // The work directory cannot be made: a file stands where it would go
   QString work = QFileInfo( second.bundleRoot() ).path() + "/work";
   QDir( work ).removeRecursively();
   QFile block( work );
   ASSERT_TRUE( block.open( QIODevice::WriteOnly ) );
   block.close();

   EXPECT_FALSE( second.runImport( diskOptions(), error ) );
   EXPECT_TRUE ( error.contains( "work directory" ) ) << error.toStdString();
   EXPECT_FALSE( error.contains( "different content" ) ) << error.toStdString();
}

// The experiment XML of a run directory is the one named for a run type,
// never an RI profile that happens to sort before it
TEST_F( DataPubDiskAgain, AnRiProfileIsNotTakenForTheExperiment )
{
   importFirst();

   // The run is filed under a run type that sorts after "RIProfile", beside
   // an RI profile
   QString xml = targetRun() + "/" + runID + "." + runType + ".xml";
   ASSERT_TRUE( QFile::exists( xml ) );
   ASSERT_TRUE( QFile::rename( xml, targetRun() + "/" + runID + ".WA.xml" ) );

   QFile rip( targetRun() + "/" + runID + ".RIProfile.xml" );
   ASSERT_TRUE( rip.open( QIODevice::WriteOnly ) );
   rip.write( "<?xml version=\"1.0\"?>\n<US_RIProfile version=\"1.0\">"
              "<RI value=\"1.5\"/></US_RIProfile>\n" );
   rip.close();

   US_DataPubImporter second;
   QString            error;
   ASSERT_TRUE( second.inspect  ( bundlePath, error ) );
   ASSERT_TRUE( second.runImport( diskOptions(), error ) )
      << error.toStdString();

   EXPECT_EQ( countOf( second.results(), US_DataPub::ResolvedRenamed ), 0 );

   for ( int ii = 0; ii < second.results().size(); ii++ )
      if ( second.results()[ ii ].type == US_DataPub::Experiment )
         EXPECT_EQ( second.results()[ ii ].resolution,
                    US_DataPub::ResolvedReused );
}
