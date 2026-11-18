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
   } ), "definitions" );
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

class DataPubCli : public DataPubImport
{
   protected:
      int run( const QStringList& args )
      {
         US_DataPubCLI cli;
         return cli.run( QStringList( "us_data_publication" ) << args );
      }
};

TEST_F( DataPubCli, ImportNeedsABundle )
{
   EXPECT_EQ( run( QStringList() << "--mode" << "import" ), 2 );
}

TEST_F( DataPubCli, ImportRefusesAnUnknownTargetOrPolicy )
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

TEST_F( DataPubCli, ADatabaseImportWithoutAPasswordStopsEarly )
{
   QByteArray saved = qgetenv( "US3_DB_PASSWORD" );
   qunsetenv( "US3_DB_PASSWORD" );

   EXPECT_EQ( run( QStringList() << "--mode" << "import" << "--bundle"
                   << bundlePath << "--target" << "db"
                   << "--non-interactive" ), 2 );

   if ( ! saved.isEmpty() )  qputenv( "US3_DB_PASSWORD", saved );
}

TEST_F( DataPubCli, ADiskImportSucceedsAndAFailedOneSaysSo )
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
