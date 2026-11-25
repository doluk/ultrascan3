// test/programs/us_data_publication/test_us_datapub_import.cpp
#include <gtest/gtest.h>

#include "datapub_test_env.h"
#include "datapub_borrow_scenario.h"
#include "datapub_craft.h"
#include "datapub_noise_rules.h"

#include "us_datapub_export.h"
#include "us_datapub_import.h"
#include "us_datapub_records.h"
#include "us_datapub_hash.h"
#include "us_datapub_bundle.h"
#include "us_datapub_cli.h"
#include "us_datapub_catalog.h"
#include "us_analyte.h"
#include "us_simparms.h"
#include "us_dataIO.h"
#include "us_crypto.h"
#include <openssl/evp.h>

#include <functional>

#if defined( Q_OS_UNIX )
#include <cstdio>
#include <csignal>
#include <cstring>
#include <unistd.h>
#endif

namespace
{
   // The value of an attribute of the first element of that name
   QString first_attribute( const QString& path, const QString& element,
                            const QString& attr )
   {
      QFile file( path );

      if ( ! file.open( QIODevice::ReadOnly ) )  return QString();

      QXmlStreamReader reader( &file );

      while ( ! reader.atEnd() )
      {
         reader.readNext();

         if ( reader.isStartElement()  &&  reader.name().toString() == element )
            return reader.attributes().value( attr ).toString();
      }

      return QString();
   }
}

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

/* A bundle of manifest format 2 -- what every export writes since round 7
   -- goes through a disk store and back: the import reads format 2, and an
   export of what it wrote gives a format 2 bundle with the same records
   and the same payloads, which imports again.
*/
TEST_F( DataPubImport, AFormatTwoBundleRoundTripsThroughADiskStore )
{
   QString error;

   US_DataPubImporter importer;
   ASSERT_TRUE( importer.inspect( bundlePath, error ) ) << error.toStdString();
   EXPECT_EQ( importer.manifest().version, 2 );
   ASSERT_TRUE( importer.runImport( diskOptions(), error ) )
      << error.toStdString() << "\n"
      << importer.log().join( "\n" ).toStdString();

   const US_DataPubManifest first = importer.manifest();

   // export the run again, from the store it was imported into
   QString saved = US_Settings::workBaseDir();
   US_Settings::set_workBaseDir( targetDir );

   US_DataPubExporter            exporter;
   US_DataPubExporter::Selection selection;
   selection.fromDb = false;
   selection.scope  = US_DataPub::ScopeNoise;
   selection.runIDs << runID;
   QString again = root + "/again.tar.gz";
   bool    ok    = exporter.exportBundle( selection, again, error );
   US_Settings::set_workBaseDir( saved );
   ASSERT_TRUE( ok ) << error.toStdString() << "\n"
                     << exporter.log().join( "\n" ).toStdString();

   const US_DataPubManifest& second = exporter.manifest();
   EXPECT_EQ( second.version, 2 );

   for ( int tt = 0; tt < int( US_DataPub::UnknownType ); tt++ )
   {
      US_DataPub::EntityType    type = US_DataPub::EntityType( tt );
      QList< US_DataPubEntity > ones = first.section( type );
      SCOPED_TRACE( tt );
      EXPECT_EQ( ones.size(), second.section( type ).size() );

      for ( const US_DataPubEntity& one : ones )
      {
         US_DataPubEntity two;
         ASSERT_TRUE( second.entity( type, one.guid, two ) )
            << one.guid.toStdString();
         EXPECT_EQ( one.name, two.name );

         // the payloads the source wrote are the ones that come back, but
         // for the IDs the disk store gives them: none (-1) for a project,
         // run, rotor calibration, solution and its buffer
         if ( type != US_DataPub::Experiment        &&
              type != US_DataPub::Project           &&
              type != US_DataPub::RotorCalibration  &&
              type != US_DataPub::Solution )
         {
            EXPECT_EQ( one.sha256.toStdString(), two.sha256.toStdString() )
               << one.guid.toStdString() << " " << one.payload.toStdString();
         }
      }
   }

   // and the second bundle imports as well
   US_DataPubImporter next;
   ASSERT_TRUE( next.inspect( again, error ) ) << error.toStdString();
   EXPECT_EQ( next.manifest().version, 2 );
   US_DataPubImporter::Options options = diskOptions();
   options.outputDir = root + "/target2";
   ASSERT_TRUE( next.runImport( options, error ) )
      << error.toStdString() << "\n" << next.log().join( "\n" ).toStdString();
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

/* A rotor calibration written to a disk store carries the ID -1, and so
   does the run's XML: an analysis maps the ID a run names to the
   coefficients of the calibration of that ID among the installation's
   (US_Hardware::readRotorMap(), etc/rotors), and any real ID could be
   another calibration there.  The GUID the run's XML names finds the
   calibration when the run is exported from the store again. */
TEST_F( DataPubImport, ADiskImportNamesTheCalibrationItWrote )
{
   QString error;

   // The store already holds a calibration of the ID the bundle's has
   QString rotors = targetDir + "/data/rotors";
   QDir().mkpath( rotors );
   US_Rotor::RotorCalibration other;
   other.ID          = 21;
   other.GUID        = US_Util::new_guid();
   other.rotorGUID   = US_Util::new_guid();
   other.coeff1      = 2.0e-5;
   other.label       = "Another calibration";
   other.lastUpdated = QDate( 2023, 5, 1 );
   ASSERT_TRUE( US_DataPubRecords::writeCalibration( other,
                                                      rotors + "/C0000001.xml" ) );

   US_DataPubImporter importer;
   ASSERT_TRUE( importer.inspect  ( bundlePath, error ) );
   ASSERT_TRUE( importer.runImport( diskOptions(), error ) )
      << error.toStdString();

   QStringList files = QDir( rotors ).entryList( QStringList( "C*.xml" ),
                                                 QDir::Files, QDir::Name );
   ASSERT_EQ( files.size(), 2 );

   US_Rotor::RotorCalibration written;
   ASSERT_TRUE( US_DataPubRecords::readCalibration( rotors + "/" + files[ 1 ],
                                                    written ) );
   EXPECT_EQ( written.GUID, calGUID );
   EXPECT_EQ( written.ID,   -1 );
   EXPECT_EQ( written.label, QString( "Demo calibration" ) );
   EXPECT_EQ( written.lastUpdated, QDate( 2024, 1, 1 ) );   // the source's

   // The run names it by GUID, and by no ID of a calibration
   QString xml = targetDir + "/results/" + runID + "/" + runID + "."
                 + runType + ".xml";
   EXPECT_EQ( first_attribute( xml, "calibration", "id" ),   QString( "-1" ) );
   EXPECT_EQ( first_attribute( xml, "calibration", "guid" ), calGUID );

   for ( const US_DataPubImporter::Result& result : importer.results() )
   {
      if ( result.type == US_DataPub::RotorCalibration )
      {
         EXPECT_EQ( result.targetID, QString( "-1" ) );
      }
   }

   // ... and the store's run comes out with it
   US_Settings::set_workBaseDir( targetDir );

   US_DataPubExporter            exporter;
   US_DataPubExporter::Selection selection;
   selection.fromDb = false;
   selection.scope  = US_DataPub::ScopeNoise;
   selection.runIDs << runID;
   QString again = root + "/again.tar.gz";
   ASSERT_TRUE( exporter.exportBundle( selection, again, error ) )
      << error.toStdString();

   QList< US_DataPubEntity > cals =
         exporter.manifest().section( US_DataPub::RotorCalibration );
   ASSERT_EQ( cals.size(), 1 );
   EXPECT_EQ( cals[ 0 ].guid, calGUID );
}

/* The analysis reads a run's rotor stretch through
   US_SimulationParameters::initFromData(): the coefficients of the run's
   XML, replaced by those of the installation's calibration (etc/rotors) of
   the ID the XML names.  Imported into an empty disk store, the run has to
   keep its own coefficients, whatever calibrations the installation has --
   an ID the store's count gave (1 here) is one the installation's first
   calibration has. */
TEST_F( DataPubImport, TheAnalysisUsesTheImportedRunsOwnCalibration )
{
   // An installed calibration of ID 1 with other coefficients (the file
   // read last wins an ID)
   QString etc     = US_Settings::appBaseDir() + "/etc/rotors";
   bool    madeEtc = ! QDir( etc ).exists();
   QString planted = etc + "/C9999999.xml";
   QDir().mkpath( etc );
   ASSERT_FALSE( QFile::exists( planted ) );

   struct Cleanup
   {
      QString file;
      QString dir;
      bool    rmdir;
      ~Cleanup()
      {
         QFile::remove( file );
         if ( rmdir )  QDir().rmdir( dir );
      }
   } cleanup { planted, etc, madeEtc };

   US_Rotor::RotorCalibration installed;
   installed.ID          = 1;
   installed.GUID        = US_Util::new_guid();
   installed.rotorGUID   = US_Util::new_guid();
   installed.coeff1      = 3.0e-3;
   installed.coeff2      = 4.0e-6;
   installed.label       = "Installed calibration";
   installed.lastUpdated = QDate( 2020, 1, 1 );
   ASSERT_TRUE( US_DataPubRecords::writeCalibration( installed, planted ) );

   QString error;
   US_DataPubImporter importer;
   ASSERT_TRUE( importer.inspect  ( bundlePath, error ) );
   ASSERT_TRUE( importer.runImport( diskOptions(), error ) )
      << error.toStdString();

   US_Settings::set_workBaseDir( targetDir );

   QString     dir  = targetDir + "/results/" + runID;
   QStringList aucs = QDir( dir ).entryList( QStringList( "*.auc" ),
                                             QDir::Files, QDir::Name );
   ASSERT_FALSE( aucs.isEmpty() );

   US_DataIO::RawData raw;
   ASSERT_EQ( US_DataIO::readRawData( dir + "/" + aucs[ 0 ], raw ),
              US_DataIO::OK );

   US_SimulationParameters simparams;
   simparams.initFromData( nullptr, raw, false, runID, runType );

   EXPECT_DOUBLE_EQ( simparams.rotorcoeffs[ 0 ], 1.2e-05 );
   EXPECT_DOUBLE_EQ( simparams.rotorcoeffs[ 1 ], 3.4e-10 );
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

#if defined( Q_OS_UNIX )
// An import started with a hang-up ignored (nohup) keeps it ignored, and
// leaves the other stop signals as it found them: no handler of the
// importer's takes their place
TEST_F( DataPubCliImport, AnIgnoredHangUpStaysIgnored )
{
   const int sigs[] = { SIGHUP, SIGINT, SIGTERM };
   struct sigaction saved[ 3 ];

   struct sigaction ignore;
   memset( &ignore, 0, sizeof( ignore ) );
   ignore.sa_handler = SIG_IGN;
   sigemptyset( &ignore.sa_mask );
   ASSERT_EQ( sigaction( SIGHUP, &ignore, &saved[ 0 ] ), 0 );

   for ( int ii = 1; ii < 3; ii++ )
      ASSERT_EQ( sigaction( sigs[ ii ], nullptr, &saved[ ii ] ), 0 );

   int rc = run( QStringList() << "--mode" << "import" << "--bundle"
                 << bundlePath << "--target" << "disk" << "--output-dir"
                 << targetDir << "--dry-run" );

   struct sigaction now[ 3 ];

   for ( int ii = 0; ii < 3; ii++ )
      sigaction( sigs[ ii ], nullptr, &now[ ii ] );

   sigaction( SIGHUP, &saved[ 0 ], nullptr );
   for ( int ii = 1; ii < 3; ii++ )
      sigaction( sigs[ ii ], &saved[ ii ], nullptr );

   EXPECT_EQ( rc, 0 );
   EXPECT_TRUE( now[ 0 ].sa_handler == SIG_IGN ) << "SIGHUP";
   EXPECT_TRUE( now[ 1 ].sa_handler == saved[ 1 ].sa_handler ) << "SIGINT";
   EXPECT_TRUE( now[ 2 ].sa_handler == saved[ 2 ].sa_handler ) << "SIGTERM";
}
#endif

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
   {
      if ( second.results()[ ii ].type == US_DataPub::Experiment )
      {
         EXPECT_EQ( second.results()[ ii ].resolution,
                    US_DataPub::ResolvedReused );
      }
   }
}

namespace
{
   // What US_Analyte's old writer made of a sequence in a file: a newline,
   // pieces of 80 characters each with a newline, the rest and a newline
   QString old_framing( const QString& seq )
   {
      QString text = "\n";
      int     full = seq.size() / 80;

      for ( int ii = 0; ii < full; ii++ )
         text += seq.mid( ii * 80, 80 ) + "\n";

      if ( seq.size() % 80 > 0 )
         text += seq.mid( full * 80 );

      return text + "\n";
   }

   // A sequence as a person pastes it: a FASTA header and numbered lines
   const char* const fasta =
      ">sp|P02768|ALBU_HUMAN Serum albumin OS=Homo sapiens\n"
      "1 MKWVTFISLL FLFSSAYSRG VFRRDAHKSE VAHRFKDLGE ENFKALVLIA FAQYLQQCPF\n"
      "61 EDHVKLVNEV TEFAKTCVAD ESAENCDKSL HTLFGDKLCT VATLRETYGE MADCCAKQEP\n";
}

// The old writer's framing -- however many round trips piled it up -- is
// what the fingerprints take off a sequence, and nothing else: a FASTA
// header, residue numbers and the sequence's own line breaks stay
TEST( DataPubImportUnit, OnlyTheOldWritersFramingIsTakenOffASequence )
{
   QString seq = QString( "MKWVTFISLL" ).repeated( 17 );        // 170 letters

   for ( const QString& one : { seq, QString( fasta ), QString(),
                                QString( "MKWVTFISLL" ).repeated( 8 ) } )
   {
      QString text = one;

      for ( int ii = 0; ii < 3; ii++ )
      {
         text = old_framing( text );
         EXPECT_EQ( US_DataPubImporter::unframedSequence( text ), one )
            << "round trip " << ii << ": " << one.toStdString();
      }
   }

   // Text no writer framed comes back as it is
   EXPECT_EQ( US_DataPubImporter::unframedSequence( fasta ), QString( fasta ) );
   EXPECT_EQ( US_DataPubImporter::unframedSequence( "ACGT" ), QString( "ACGT" ) );
   EXPECT_EQ( US_DataPubImporter::unframedSequence( "\nACGT" ),
              QString( "\nACGT" ) );

   // Lines of its own, of any length, inside a newline at either end: not
   // the old writer's pieces of 80
   QString own = "\n" + QString( "A" ).repeated( 90 ) + "\nCC\n";
   EXPECT_EQ( US_DataPubImporter::unframedSequence( own ), own );
   QString shortLines = "\nMKWV\nTFIS\n";
   EXPECT_EQ( US_DataPubImporter::unframedSequence( shortLines ),
              QString( "MKWV\nTFIS" ) )
      << "a short sequence with a line break of its own, framed once";
}

// What a MySQL character set can store, as a strict server decides it
TEST( DataPubImportUnit, TheCharactersACharsetCannotStoreAreFound )
{
   QString alpha = QString::fromUtf8( "Prot\xC3\xA9ine \xCE\xB1" );  // é, α
   QString euro  = QString::fromUtf8( "Prot\xC3\xA9ine \xE2\x82\xAC" ); // é, €
   QString smile = QString::fromUtf8( "ok \xF0\x9F\x98\x80" );       // U+1F600

   EXPECT_EQ( US_DataPubImporter::unstorableCharacter( "latin1", alpha ),
              QString::fromUtf8( "\xCE\xB1" ) );
   EXPECT_EQ( US_DataPubImporter::unstorableCharacter( "latin1", euro ),
              QString() );
   EXPECT_EQ( US_DataPubImporter::unstorableCharacter( "latin1", "plain" ),
              QString() );
   EXPECT_EQ( US_DataPubImporter::unstorableCharacter( "utf8mb3", alpha ),
              QString() );
   EXPECT_EQ( US_DataPubImporter::unstorableCharacter( "utf8", smile ),
              QString::fromUtf8( "\xF0\x9F\x98\x80" ) );
   EXPECT_EQ( US_DataPubImporter::unstorableCharacter( "utf8mb4", smile ),
              QString() );
   EXPECT_EQ( US_DataPubImporter::unstorableCharacter( "ascii", euro ),
              QString::fromUtf8( "\xC3\xA9" ) );
   EXPECT_EQ( US_DataPubImporter::unstorableCharacter( "binary", smile ),
              QString() );
   // A set this does not know: only ASCII is safe
   EXPECT_EQ( US_DataPubImporter::unstorableCharacter( "koi8r", alpha ),
              QString::fromUtf8( "\xC3\xA9" ) );
}

// A sequence goes into a disk store as the bundle holds it, and the same
// bundle imported again finds it the same analyte
TEST_F( DataPubCrafted, ASequenceIsKeptAsItIsOnDisk )
{
   QString path = craft( "fasta", [ & ]( US_DataPubManifest& m,
                                         const QString& stage )
   {
      editEntity( m, US_DataPub::Analyte, [ & ]( US_DataPubEntity& entity )
      {
         QString    file = stage + "/" + entity.payload;
         US_Analyte analyte;
         ASSERT_EQ( analyte.readFromFile( file ), IUS_DB2::OK );
         analyte.sequence = fasta;
         QFile::remove( file );
         ASSERT_EQ( analyte.write( false, file ), IUS_DB2::OK );
         entity.sha256 = US_DataPubHash::fileHash( file );
      } );
   } );
   ASSERT_FALSE( path.isEmpty() );

   US_DataPubImporter importer;
   QString            error;
   ASSERT_TRUE( importer.inspect  ( path, error ) ) << error.toStdString();
   ASSERT_TRUE( importer.runImport( diskOptions(), error ) )
      << error.toStdString();

   QStringList files = QDir( targetDir + "/data/analytes" )
                       .entryList( QStringList( "*.xml" ), QDir::Files );
   ASSERT_EQ( files.size(), 1 );
   US_Analyte stored;
   ASSERT_EQ( stored.readFromFile( targetDir + "/data/analytes/" + files[ 0 ] ),
              IUS_DB2::OK );
   EXPECT_EQ( stored.sequence.toUtf8(), QByteArray( fasta ) );

   US_DataPubImporter again;
   ASSERT_TRUE( again.inspect  ( path, error ) ) << error.toStdString();
   ASSERT_TRUE( again.runImport( diskOptions(), error ) )
      << error.toStdString();

   for ( const US_DataPubImporter::Result& result : again.results() )
   {
      if ( result.type == US_DataPub::Analyte )
      {
         EXPECT_EQ( result.resolution, US_DataPub::ResolvedReused );
      }
   }
}

// A number that is not finite -- an old or foreign bundle's -- is refused
// before anything is written, by the dry run as by the import: a disk
// store would hold "nan", a database what its column makes of it
TEST_F( DataPubCrafted, ANumberThatIsNotFiniteIsRefusedByTheDryRunToo )
{
   struct Case
   {
      US_DataPub::EntityType type;
      const char*            pattern;
      const char*            value;
      const char*            field;
   };

   const Case cases[] =
   {
      { US_DataPub::Buffer,  "density=\"[^\"]*\"", "density=\"nan\"",
        "density" },
      { US_DataPub::Analyte, "vbar20=\"[^\"]*\"",  "vbar20=\"inf\"",
        "vbar20" },
      { US_DataPub::Model,   "wavelength=\"[^\"]*\"", "wavelength=\"-inf\"",
        "wavelength" }
   };

   for ( const Case& one : cases )
   {
      SCOPED_TRACE( one.field );
      QDir( targetDir ).removeRecursively();
      QDir().mkpath( targetDir );

      QString path = craft( QString( "nonfinite_" ) + US_DataPub::typeKey(
                               one.type ),
                            [ & ]( US_DataPubManifest& m, const QString& stage )
      {
         editEntity( m, one.type, [ & ]( US_DataPubEntity& entity )
         {
            QString file = stage + "/" + entity.payload;
            QFile   xml( file );
            ASSERT_TRUE( xml.open( QIODevice::ReadOnly ) );
            QString text = QString::fromUtf8( xml.readAll() );
            xml.close();
            ASSERT_TRUE( text.contains( QRegularExpression( one.pattern ) ) );
            text.replace( QRegularExpression( one.pattern ), one.value );
            ASSERT_TRUE( xml.open( QIODevice::WriteOnly | QIODevice::Truncate ) );
            xml.write( text.toUtf8() );
            xml.close();
            entity.sha256 = US_DataPubHash::fileHash( file );
         } );
      } );
      ASSERT_FALSE( path.isEmpty() );

      for ( int dry = 1; dry >= 0; dry-- )
      {
         US_DataPubImporter          importer;
         US_DataPubImporter::Options options = diskOptions();
         options.dryRun = ( dry == 1 );
         QString                     error;

         ASSERT_TRUE ( importer.inspect( path, error ) ) << error.toStdString();
         EXPECT_FALSE( importer.runImport( options, error ) )
            << ( dry ? "dry run" : "import" );
         EXPECT_TRUE ( error.contains( "finite" )  &&
                       error.contains( one.field ) ) << error.toStdString();
         EXPECT_EQ   ( filesIn( targetDir ), 0 );
      }
   }
}

// ... and so is one among the manifest's own fields of a record
TEST_F( DataPubCrafted, AManifestNumberThatIsNotFiniteIsRefused )
{
   QString path = craft( "nonfinite_manifest", [ & ]( US_DataPubManifest& m,
                                                      const QString& )
   {
      editEntity( m, US_DataPub::Model, [ & ]( US_DataPubEntity& entity )
      {
         entity.attrs.insert( "variance", "nan" );
      } );
   } );

   expectImportFails( path, "finite" );
}

// ------------------------------------------------- round six, follow-up


// A wrong master password is said to be wrong before the database is
// opened, not reported as the server's refusal of a login without one
TEST_F( DataPubImport, AWrongMasterPasswordIsSaidToBeWrong )
{
   QStringList savedDB   = US_Settings::defaultDB();
   QByteArray  savedHash = US_Settings::UltraScanPW();

   QStringList cipher = US_Crypto::encrypt( "dbpw", "right master" );
   QStringList entry;
   entry << "a database" << "us3" << "us3_somewhere" << "127.0.0.1:9"
         << cipher.at( 0 ) << cipher.at( 1 ) << "me@example.org"
         << cipher.at( 0 ) << cipher.at( 1 ) << US_Util::new_guid();
   US_Settings::set_defaultDB( entry );
   US_Settings::set_UltraScanPW( QByteArray() );

   US_DataPubImporter::Options options;
   options.target     = US_DataPub::TargetDb;
   options.dbPassword = "wrong";
   options.dryRun     = true;

   US_DataPubImporter importer;
   QString            error;
   ASSERT_TRUE ( importer.inspect( bundlePath, error ) ) << error.toStdString();
   EXPECT_FALSE( importer.runImport( options, error ) );
   EXPECT_TRUE ( error.contains( "master password is wrong" ) )
      << error.toStdString();
   EXPECT_FALSE( error.contains( "using password" ) ) << error.toStdString();

   US_Settings::set_defaultDB   ( savedDB   );
   US_Settings::set_UltraScanPW( savedHash );
}

/* A database password stored empty is unlocked by the right master
   password too.  In the current format its ciphertext is empty; in the old
   one (AES-128-CBC) it is one block of padding, which decrypts to nothing
   as a wrong master password does -- and the right one was refused. */
TEST_F( DataPubImport, AnEmptyStoredPasswordTakesTheRightMasterPassword )
{
   QStringList savedDB   = US_Settings::defaultDB();
   QByteArray  savedHash = US_Settings::UltraScanPW();
   US_Settings::set_UltraScanPW( QByteArray() );

   // The old format, as US3 once wrote it: AES-128-CBC with the master
   // password's first 16 characters as the key, PKCS#7 padding
   auto legacy = []( const QString& plain, const QString& master,
                     QString& ivHex ) -> QString
   {
      unsigned char key[ 16 ];
      US_Crypto::deriveLegacyKey_16( master, key );
      // A fixed vector, so that what a wrong key makes of the block -- a
      // padding error here -- is the same in every run
      QByteArray iv( "0123456789abcdef" );
      QByteArray in  = plain.toLatin1();
      QByteArray out( in.size() + 32, '\0' );
      int        len = 0;
      int        fin = 0;

      EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
      EVP_EncryptInit_ex( ctx, EVP_aes_128_cbc(), nullptr, key,
                          reinterpret_cast< const unsigned char* >( iv.constData() ) );
      EVP_EncryptUpdate( ctx, reinterpret_cast< unsigned char* >( out.data() ),
                         &len,
                         reinterpret_cast< const unsigned char* >( in.constData() ),
                         in.size() );
      EVP_EncryptFinal_ex( ctx, reinterpret_cast< unsigned char* >( out.data() )
                                + len, &fin );
      EVP_CIPHER_CTX_free( ctx );
      out.resize( len + fin );
      ivHex = QString::fromLatin1( iv.toHex() );
      return QString::fromLatin1( out.toHex() );
   };

   auto store = []( const QString& cipher, const QString& iv )
   {
      QStringList entry;
      entry << "a database" << "us3" << "us3_somewhere" << "127.0.0.1:9"
            << cipher << iv << "me@example.org" << cipher << iv
            << US_Util::new_guid();
      US_Settings::set_defaultDB( entry );
   };

   // Old format, empty
   QString iv;
   QString cipher = legacy( QString(), "right master", iv );
   ASSERT_EQ( cipher.size(), 32 );
   ASSERT_TRUE( US_Crypto::decrypt( cipher, "right master", iv ).isEmpty() );
   store( cipher, iv );
   EXPECT_TRUE ( US_DataPub::masterPasswordProblem( "right master" ).isEmpty() );
   EXPECT_FALSE( US_DataPub::masterPasswordProblem( "wrong master" ).isEmpty() );

   // Old format, not empty: the rule is unchanged
   cipher = legacy( "dbpw", "right master", iv );
   ASSERT_EQ( US_Crypto::decrypt( cipher, "right master", iv ), QString( "dbpw" ) );
   store( cipher, iv );
   EXPECT_TRUE ( US_DataPub::masterPasswordProblem( "right master" ).isEmpty() );
   EXPECT_FALSE( US_DataPub::masterPasswordProblem( "wrong master" ).isEmpty() );

   // The current format, empty
   QStringList current = US_Crypto::encrypt( QString(), "right master" );
   ASSERT_EQ( current.size(), 2 );
   store( current.at( 0 ), current.at( 1 ) );
   EXPECT_TRUE( US_DataPub::masterPasswordProblem( "right master" ).isEmpty() );

   US_Settings::set_defaultDB   ( savedDB   );
   US_Settings::set_UltraScanPW( savedHash );
}

// A disk store keeps a model's variance and meniscus in its XML only: the
// manifest's, at full precision, go into it when the XML says something
// else -- a model the database stored need not hold them at all
TEST_F( DataPubCrafted, ADiskModelTakesTheManifestsVarianceAndMeniscus )
{
   const double variance = 1.0 / 3.0e5;
   const double meniscus = 5.8712345678901;

   QString crafted = craft( "variance", [ & ]( US_DataPubManifest& m,
                                               const QString& )
   {
      QList< US_DataPubEntity > list = m.section( US_DataPub::Model );
      ASSERT_EQ( list.size(), 1 );
      list[ 0 ].attrs.insert( "variance", QString::number( variance, 'g', 17 ) );
      list[ 0 ].attrs.insert( "meniscus", QString::number( meniscus, 'g', 17 ) );
      m.setSection( US_DataPub::Model, list );
   } );

   US_DataPubImporter importer;
   QString            error;
   ASSERT_TRUE( importer.inspect  ( crafted, error ) ) << error.toStdString();
   ASSERT_TRUE( importer.runImport( diskOptions(), error ) )
      << error.toStdString();

   QStringList models = QDir( targetDir + "/data/models" )
                        .entryList( QStringList( "M*.xml" ), QDir::Files );
   ASSERT_EQ( models.size(), 1 );
   QString path = targetDir + "/data/models/" + models[ 0 ];

   EXPECT_EQ( first_attribute( path, "model", "variance" ).toDouble(), variance )
      << first_attribute( path, "model", "variance" ).toStdString();
   EXPECT_EQ( first_attribute( path, "model", "meniscus" ).toDouble(), meniscus )
      << first_attribute( path, "model", "meniscus" ).toStdString();

   // ... and a second import finds the model it wrote the same
   US_DataPubImporter again;
   ASSERT_TRUE( again.inspect  ( crafted, error ) ) << error.toStdString();
   ASSERT_TRUE( again.runImport( diskOptions(), error ) ) << error.toStdString();

   for ( const US_DataPubImporter::Result& result : again.results() )
   {
      if ( result.type == US_DataPub::Model )
      {
         EXPECT_EQ( result.resolution, US_DataPub::ResolvedReused );
      }
   }
}

// A model with no noise of its own borrows the noise of its edit that is
// not newer than it.  The import writes it after that noise, so the store
// it went into still says so: exported again, the noise travels with it.
TEST_F( DataPubImport, ABorrowingModelKeepsItsNoiseThroughAnImport )
{
   QString  laterGUID = US_Util::new_guid();
   US_Model later;
   later.modelGUID   = laterGUID;
   later.editGUID    = editGUID;
   later.description = runID + ".1A280.2dsa-mc.model";
   later.write( US_Settings::dataDir() + "/models/M0000002.xml" );

   US_DataPubExporter            exporter;
   US_DataPubExporter::Selection selection;
   selection.fromDb = false;
   selection.scope  = US_DataPub::ScopeNoise;
   selection.runIDs << runID;

   QString error;
   QString bundle = root + "/borrowing.tar.gz";
   ASSERT_TRUE( exporter.exportBundle( selection, bundle, error ) )
      << error.toStdString();

   US_DataPubEntity noise;
   ASSERT_TRUE( exporter.manifest().entity( US_DataPub::Noise, noiseGUID,
                                            noise ) );
   ASSERT_EQ( noise.attrs.value( "borrowedByModelGUIDs" ), laterGUID );

   US_DataPubImporter importer;
   ASSERT_TRUE( importer.inspect  ( bundle, error ) ) << error.toStdString();
   ASSERT_TRUE( importer.runImport( diskOptions(), error ) )
      << error.toStdString();

   // The borrowing model's file is not older than the noise's
   QString laterFile;
   QString noiseFile;
   QDir    mdir( targetDir + "/data/models" );
   QDir    ndir( targetDir + "/data/noises" );

   for ( const QString& name : mdir.entryList( QStringList( "M*.xml" ) ) )
      if ( first_attribute( mdir.filePath( name ), "model", "modelGUID" )
           == laterGUID )
         laterFile = mdir.filePath( name );

   for ( const QString& name : ndir.entryList( QStringList( "N*.xml" ) ) )
      if ( first_attribute( ndir.filePath( name ), "noise", "noiseGUID" )
           == noiseGUID )
         noiseFile = ndir.filePath( name );

   ASSERT_FALSE( laterFile.isEmpty() );
   ASSERT_FALSE( noiseFile.isEmpty() );
   EXPECT_GE( QFileInfo( laterFile ).lastModified(),
              QFileInfo( noiseFile ).lastModified() );

   // ... so exported from the store again, the noise is still borrowed
   US_Settings::set_workBaseDir( targetDir );

   US_DataPubExporter again;
   ASSERT_TRUE( again.exportBundle( selection, root + "/again.tar.gz", error ) )
      << error.toStdString();

   US_DataPubEntity back;
   ASSERT_TRUE( again.manifest().entity( US_DataPub::Noise, noiseGUID, back ) );
   EXPECT_EQ( back.attrs.value( "borrowedByModelGUIDs" ), laterGUID );
}

/* Two records of the bundle that share a name -- two models and two noise
   records here, as two fits of an edit can have -- go into an empty store
   under that name, as the source holds them: the name is taken by a record
   the import wrote itself, which is no conflict.  A dry run says the
   same. */
TEST_F( DataPubImport, SameNamedRecordsKeepTheirNamesInAnEmptyStore )
{
   QString  twinGUID = US_Util::new_guid();
   US_Model twin;
   twin.modelGUID   = twinGUID;
   twin.editGUID    = editGUID;
   twin.description = "demo_run.1A280.2dsa.model";      // the fixture's
   twin.write( US_Settings::dataDir() + "/models/M0000002.xml" );

   US_Noise noise;
   noise.noiseGUID   = US_Util::new_guid();
   noise.modelGUID   = twinGUID;
   noise.description = "demo_run.1A280.2dsa.ti_noise";  // the fixture's
   noise.type        = US_Noise::TI;
   noise.minradius   = 5.8;
   noise.maxradius   = 5.95;
   for ( int ii = 0; ii < 16; ii++ )  noise.values << 0.003 * double( ii );
   noise.count = noise.values.size();
   noise.write( US_Settings::dataDir() + "/noises/N0000002.xml" );

   US_DataPubExporter            exporter;
   US_DataPubExporter::Selection selection;
   selection.fromDb = false;
   selection.scope  = US_DataPub::ScopeNoise;
   selection.runIDs << runID;

   QString error;
   QString bundle = root + "/twins.tar.gz";
   ASSERT_TRUE( exporter.exportBundle( selection, bundle, error ) )
      << error.toStdString();
   ASSERT_EQ( exporter.manifest().section( US_DataPub::Model ).size(), 2 );
   ASSERT_EQ( exporter.manifest().section( US_DataPub::Noise ).size(), 2 );

   for ( int dry = 1; dry >= 0; dry-- )
   {
      US_DataPubImporter          importer;
      US_DataPubImporter::Options options = diskOptions();
      options.dryRun = ( dry == 1 );
      ASSERT_TRUE( importer.inspect  ( bundle, error ) ) << error.toStdString();
      ASSERT_TRUE( importer.runImport( options, error ) ) << error.toStdString();

      for ( const US_DataPubImporter::Result& result : importer.results() )
      {
         if ( result.type != US_DataPub::Model  &&
              result.type != US_DataPub::Noise )
            continue;

         EXPECT_EQ( result.resolution, US_DataPub::ResolvedCreated )
            << ( dry ? "dry run " : "import " ) << result.targetName.toStdString();
         EXPECT_FALSE( result.targetName.contains( "imported" ) )
            << result.targetName.toStdString();
      }
   }

   QStringList models = QDir( targetDir + "/data/models" )
                        .entryList( QStringList( "M*.xml" ), QDir::Files );
   ASSERT_EQ( models.size(), 2 );

   for ( const QString& name : models )
      EXPECT_EQ( first_attribute( targetDir + "/data/models/" + name, "model",
                                  "description" ),
                 QString( "demo_run.1A280.2dsa.model" ) );

   // A store that already holds the name is a conflict, though: renamed
   QString other = root + "/target2";
   QDir().mkpath( other + "/data/models" );
   US_Model held;
   held.modelGUID   = US_Util::new_guid();
   held.editGUID    = US_Util::new_guid();
   held.description = "demo_run.1A280.2dsa.model";
   held.write( other + "/data/models/M0000001.xml" );

   US_DataPubImporter          importer;
   US_DataPubImporter::Options options = diskOptions();
   options.outputDir = other;
   options.policy    = US_DataPub::PolicyRename;
   ASSERT_TRUE( importer.inspect  ( bundle, error ) ) << error.toStdString();
   ASSERT_TRUE( importer.runImport( options, error ) ) << error.toStdString();

   int renamed = 0;

   for ( const US_DataPubImporter::Result& result : importer.results() )
      if ( result.type == US_DataPub::Model  &&
           result.resolution == US_DataPub::ResolvedRenamed )
         renamed++;

   EXPECT_EQ( renamed, 2 );
}

/* Two noise records of a type dated the same second as each other (and as
   the model) are told apart by a fixed rule, not by the order the catalog
   lists them in: the higher database ID, else the higher GUID. */
TEST_F( DataPubImport, NoiseBeforeBreaksATieTheSameWayInEveryOrder )
{
   US_DataCatalog::Noise low;
   low.guid        = "aaaaaaaa-0000-4000-8000-000000000001";
   low.id          = "7";
   low.noiseType   = "ti";
   low.lastUpdated = "2024-01-01 10:00:00 UTC";

   US_DataCatalog::Noise high = low;
   high.guid = "bbbbbbbb-0000-4000-8000-000000000001";
   high.id   = "5";

   QString model = "2024-01-01 10:00:00";        // the same second

   for ( int order = 0; order < 2; order++ )
   {
      QList< US_DataCatalog::Noise > both;
      both << ( order == 0 ? low : high ) << ( order == 0 ? high : low );

      // In a database, the higher ID: written last
      QList< US_DataCatalog::Noise > picked =
            US_DataPubCatalog::noiseBefore( both, model );
      ASSERT_EQ( picked.size(), 1 );
      EXPECT_EQ( picked[ 0 ].guid, low.guid ) << order;

      // On disk (no IDs), the higher GUID
      both[ 0 ].id = "-1";
      both[ 1 ].id = "-1";
      picked = US_DataPubCatalog::noiseBefore( both, model );
      ASSERT_EQ( picked.size(), 1 );
      EXPECT_EQ( picked[ 0 ].guid, high.guid ) << order;

      // ... and so with neither dated
      both[ 0 ].lastUpdated.clear();
      both[ 1 ].lastUpdated.clear();
      picked = US_DataPubCatalog::noiseBefore( both, model );
      ASSERT_EQ( picked.size(), 1 );
      EXPECT_EQ( picked[ 0 ].guid, high.guid ) << order;
   }
}

/* Several noise records of a type in one edit, and models that borrow
   them or none: what each model borrows is told by time stamps alone, so
   the import writes the models and noise in an order -- with the clock moved
   on in between where needed -- that keeps it.  Exported from the store
   again, every noise record names the same borrowers as in the source, and
   the model made before any noise borrows none. */
TEST_F( DataPubImport, EveryBorrowingSurvivesSeveralNoiseRecordsOfAType )
{
   DataPubBorrowScenario scenario;
   scenario.write( US_Settings::dataDir() + "/models",
                   US_Settings::dataDir() + "/noises", runID, editGUID,
                   noiseGUID );

   US_DataPubExporter            exporter;
   US_DataPubExporter::Selection selection;
   selection.fromDb = false;
   selection.scope  = US_DataPub::ScopeNoise;
   selection.runIDs << runID;

   QString error;
   QString bundle = root + "/several.tar.gz";
   ASSERT_TRUE( exporter.exportBundle( selection, bundle, error ) )
      << error.toStdString();
   ASSERT_EQ( DataPubBorrowScenario::text(
                 DataPubBorrowScenario::borrowers( exporter.manifest() ) ),
              DataPubBorrowScenario::text( scenario.expected() ) );

   US_DataPubImporter importer;
   ASSERT_TRUE( importer.inspect  ( bundle, error ) ) << error.toStdString();
   ASSERT_TRUE( importer.runImport( diskOptions(), error ) )
      << error.toStdString();

   US_Settings::set_workBaseDir( targetDir );

   US_DataPubExporter again;
   ASSERT_TRUE( again.exportBundle( selection, root + "/again.tar.gz", error ) )
      << error.toStdString();
   EXPECT_EQ( DataPubBorrowScenario::text(
                 DataPubBorrowScenario::borrowers( again.manifest() ) ),
              DataPubBorrowScenario::text( scenario.expected() ) );
   EXPECT_TRUE( again.manifest().contains( US_DataPub::Model, scenario.m0 ) );
}

// A calibration the target already has under another GUID is named by that
// GUID in the run's XML, as by its ID
TEST_F( DataPubImport, AReusedCalibrationIsNamedByItsTargetGuid )
{
   // The bundle's calibration, under another GUID and ID
   US_DataPubImporter probe;
   QString            perror;
   ASSERT_TRUE( probe.inspect( bundlePath, perror ) ) << perror.toStdString();
   QList< US_DataPubEntity > cals =
         probe.manifest().section( US_DataPub::RotorCalibration );
   ASSERT_EQ( cals.size(), 1 );

   US_Rotor::RotorCalibration same;
   ASSERT_TRUE( US_DataPubRecords::readCalibration(
                probe.bundleRoot() + "/" + cals[ 0 ].payload, same ) );

   QString theirs = US_Util::new_guid();
   same.GUID    = theirs;
   same.ID      = 5;
   same.rotorID = -1;

   QString rotors = targetDir + "/data/rotors";
   QDir().mkpath( rotors );
   ASSERT_TRUE( US_DataPubRecords::writeCalibration( same,
                                                      rotors + "/C0000001.xml" ) );

   US_DataPubImporter importer;
   QString            error;
   ASSERT_TRUE( importer.inspect  ( bundlePath, error ) );
   ASSERT_TRUE( importer.runImport( diskOptions(), error ) )
      << error.toStdString();

   bool reused = false;

   for ( const US_DataPubImporter::Result& result : importer.results() )
      if ( result.type == US_DataPub::RotorCalibration )
         reused = ( result.resolution == US_DataPub::ResolvedReused  &&
                    result.targetGUID == theirs );

   ASSERT_TRUE( reused ) << importer.log().join( "\n" ).toStdString();

   QString xml = targetDir + "/results/" + runID + "/" + runID + "."
                 + runType + ".xml";
   EXPECT_EQ( first_attribute( xml, "calibration", "id" ),   QString( "5" ) );
   EXPECT_EQ( first_attribute( xml, "calibration", "guid" ), theirs );
}

// ------------------------------------------- the order of models and noise

/* The models and noise records of an edit go into a target in the source's
   order.  The analysis programs load the noise of the latest model with
   noise of an edit (US_LoadableNoise::count_noise(): the last M*.xml file
   on disk, the highest model ID in a database; see datapub_noise_rules.h),
   and a catalog tells which noise a model without its own was run with by
   time stamps (US_DataPubCatalog::noiseBefore()).  A database export lists
   the models newest first, and an import that wrote them in the bundle's
   order made the oldest model with noise the latest. */
class DataPubOrder : public DataPubImport
{
   protected:
      US_DataPubExporter::Selection selection()
      {
         US_DataPubExporter::Selection chosen;
         chosen.fromDb = false;
         chosen.scope  = US_DataPub::ScopeNoise;
         chosen.runIDs << runID;
         return chosen;
      }

      // The fixture's store, with a scenario's models and noise, exported
      QString exportStore( const QString& name )
      {
         QString            bundle = root + "/" + name + ".tar.gz";
         QString            error;
         US_DataPubExporter exporter;
         EXPECT_TRUE( exporter.exportBundle( selection(), bundle, error ) )
            << error.toStdString();
         return bundle;
      }

      // Imported into a fresh disk store under root
      bool importInto( const QString& bundle, const QString& store,
                       US_DataPubImporter& importer, QString& error )
      {
         US_DataPubImporter::Options options = diskOptions();
         options.outputDir = store;

         return importer.inspect( bundle, error )  &&
                importer.runImport( options, error );
      }

      // noise GUID -> borrowers, as an export of a disk store says
      QMap< QString, QString > borrowersIn( const QString& store )
      {
         QString saved = US_Settings::workBaseDir();
         US_Settings::set_workBaseDir( store );

         US_DataPubExporter again;
         QString            error;
         bool               ok = again.exportBundle( selection(),
                                                     store + "/again.tar.gz",
                                                     error );
         US_Settings::set_workBaseDir( saved );
         EXPECT_TRUE( ok ) << error.toStdString();

         return DataPubBorrowScenario::borrowers( again.manifest() );
      }

      static QStringList orderProblems( const US_DataPubImporter& importer )
      {
         QStringList found;

         for ( const QString& text : importer.warnings() )
            if ( text.contains( "may not survive" ) )  found << text;

         return found;
      }
};

/* A bundle laid out as a database export -- models and noise newest first
   -- carrying the source's time stamps and file numbers: the target's last
   model file with noise is the source's, and every model borrows what it
   did. */
TEST_F( DataPubOrder, TheLatestModelWithNoiseSurvivesADatabaseOrderedBundle )
{
   DataPubBorrowScenario scenario;
   scenario.write( US_Settings::dataDir() + "/models",
                   US_Settings::dataDir() + "/noises", runID, editGUID,
                   noiseGUID );
   ASSERT_EQ( DataPubNoiseRules::latestModelOnDisk( US_Settings::dataDir(),
                                                    editGUID ),
              scenario.latest() );

   QString error;
   QString plain  = exportStore( "plain" );
   QString bundle = datapubRecraft( plain, root + "/dbordered.tar.gz",
                    []( US_DataPubManifest& m, const QString& )
                    { datapubAsDatabaseExport( m, true, false ); }, error );
   ASSERT_FALSE( bundle.isEmpty() ) << error.toStdString();

   // The export says when and in which order the source made each record
   US_DataPubImporter probe;
   ASSERT_TRUE( probe.inspect( plain, error ) ) << error.toStdString();

   for ( US_DataPub::EntityType type : { US_DataPub::Model,
                                         US_DataPub::Noise } )
      for ( const US_DataPubEntity& entity : probe.manifest().section( type ) )
      {
         EXPECT_TRUE( entity.attrs.value( "sourceLastUpdated" ).endsWith( "Z" ) )
            << entity.guid.toStdString();
         EXPECT_EQ( entity.attrs.value( "sourceIndex" ),
                    QString::number( entity.filename.mid( 1, 7 ).toInt() ) );
      }

   QString            store = root + "/ordered";
   US_DataPubImporter importer;
   ASSERT_TRUE( importInto( bundle, store, importer, error ) )
      << error.toStdString();
   EXPECT_TRUE( orderProblems( importer ).isEmpty() )
      << orderProblems( importer ).join( "\n" ).toStdString();

   EXPECT_EQ( DataPubNoiseRules::latestModelOnDisk( store + "/data", editGUID ),
              scenario.latest() );
   EXPECT_EQ( DataPubBorrowScenario::text( borrowersIn( store ) ),
              DataPubBorrowScenario::text( scenario.expected() ) );
}

/* A bundle of an export before the time stamps and file numbers were
   written: a database export's records by their database IDs (the
   manifest's modelID and noiseID), and failing those in the manifest's
   order, which a database export lists newest first. */
TEST_F( DataPubOrder, AnOlderBundleIsWrittenInItsSourcesOrder )
{
   DataPubBorrowScenario scenario;
   scenario.write( US_Settings::dataDir() + "/models",
                   US_Settings::dataDir() + "/noises", runID, editGUID,
                   noiseGUID );

   QString plain = exportStore( "plain" );

   for ( bool ids : { true, false } )
   {
      SCOPED_TRACE( ids ? "by the database IDs" : "by the manifest's order" );
      QString error;
      QString bundle = datapubRecraft( plain, root + "/old.tar.gz",
                       [ ids ]( US_DataPubManifest& m, const QString& )
                       { datapubAsDatabaseExport( m, false, ids ); }, error );
      ASSERT_FALSE( bundle.isEmpty() ) << error.toStdString();

      QString            store = root + ( ids ? "/byid" : "/bylist" );
      US_DataPubImporter importer;
      ASSERT_TRUE( importInto( bundle, store, importer, error ) )
         << error.toStdString();

      EXPECT_EQ( DataPubNoiseRules::latestModelOnDisk( store + "/data",
                                                       editGUID ),
                 scenario.latest() );
      EXPECT_EQ( DataPubBorrowScenario::text( borrowersIn( store ) ),
                 DataPubBorrowScenario::text( scenario.expected() ) );
   }
}

/* The borrowing of the review's case: noise records listed newest first
   (as a database export lists them), which the greedy sequence the import
   used to write them in ran into a dead end with, after which two models
   borrowed the wrong noise and the import only noted it.  Written in the
   source's order, every model borrows what it did. */
TEST_F( DataPubOrder, TheGreedySequencesDeadEndIsWrittenInTheSourcesOrder )
{
   DataPubDeadEndScenario scenario;
   scenario.write( US_Settings::dataDir() + "/models",
                   US_Settings::dataDir() + "/noises", runID, editGUID,
                   noiseGUID );

   QString            error;
   QString            plain = exportStore( "deadend" );
   US_DataPubImporter probe;
   ASSERT_TRUE( probe.inspect( plain, error ) ) << error.toStdString();
   ASSERT_EQ( DataPubBorrowScenario::text(
                 DataPubBorrowScenario::borrowers( probe.manifest() ) ),
              DataPubBorrowScenario::text( scenario.expected() ) );

   QString bundle = datapubRecraft( plain, root + "/reversed.tar.gz",
                    []( US_DataPubManifest& m, const QString& )
                    { datapubAsDatabaseExport( m, true, false ); }, error );
   ASSERT_FALSE( bundle.isEmpty() ) << error.toStdString();

   QString            store = root + "/deadend";
   US_DataPubImporter importer;
   ASSERT_TRUE( importInto( bundle, store, importer, error ) )
      << error.toStdString();
   EXPECT_TRUE( orderProblems( importer ).isEmpty() )
      << orderProblems( importer ).join( "\n" ).toStdString();

   EXPECT_EQ( DataPubBorrowScenario::text( borrowersIn( store ) ),
              DataPubBorrowScenario::text( scenario.expected() ) );
   EXPECT_EQ( DataPubNoiseRules::latestModelOnDisk( store + "/data", editGUID ),
              scenario.latest() );
}

/* A store that already holds a model of the edit (here the fixture's model,
   which the import reuses) in a file after a gap in the numbers: the
   models the import writes go after it, not into the gap, so the latest
   of them is still the latest model with noise. */
TEST_F( DataPubOrder, ImportedModelsFollowTheFilesAlreadyThere )
{
   DataPubBorrowScenario scenario;
   scenario.write( US_Settings::dataDir() + "/models",
                   US_Settings::dataDir() + "/noises", runID, editGUID,
                   noiseGUID );

   QString store = root + "/gaps";
   QDir().mkpath( store + "/data/models" );
   QDir().mkpath( store + "/data/noises" );

   // Another edit's model in M0000001, the fixture's model and noise of
   // this edit at 20
   US_Model other;
   other.modelGUID   = US_Util::new_guid();
   other.editGUID    = US_Util::new_guid();
   other.description = "other.1A280.e9_a9_2DSA.model";
   ASSERT_EQ( other.write( store + "/data/models/M0000001.xml" ), IUS_DB2::OK );
   ASSERT_TRUE( QFile::copy( US_Settings::dataDir() + "/models/M0000001.xml",
                             store + "/data/models/M0000020.xml" ) );
   ASSERT_TRUE( QFile::copy( US_Settings::dataDir() + "/noises/N0000001.xml",
                             store + "/data/noises/N0000020.xml" ) );

   QString            error;
   QString            bundle = exportStore( "gaps" );
   US_DataPubImporter importer;
   ASSERT_TRUE( importInto( bundle, store, importer, error ) )
      << error.toStdString();

   EXPECT_EQ( DataPubNoiseRules::latestModelOnDisk( store + "/data", editGUID ),
              scenario.latest() );
}

/* A disk store whose file numbers have gaps filled after removals
   (DataPubGapScenario), through a bundle into another store: the target's
   last model file with noise is the source's, and every model borrows what
   it did, with no warning.  The import used to chain the models by their
   file numbers, read the filled gaps as a contradiction and drop all the
   borrowing of the edit. */
TEST_F( DataPubOrder, AStoreWithFilledGapsKeepsItsLatestModelAndBorrowing )
{
   DataPubGapScenario scenario;
   scenario.write( US_Settings::dataDir() + "/models",
                   US_Settings::dataDir() + "/noises", runID, editGUID,
                   noiseGUID );

   QString            error;
   QString            bundle = exportStore( "gaps" );
   US_DataPubImporter probe;
   ASSERT_TRUE( probe.inspect( bundle, error ) ) << error.toStdString();
   ASSERT_EQ( DataPubBorrowScenario::text(
                 DataPubBorrowScenario::borrowers( probe.manifest() ) ),
              DataPubBorrowScenario::text( scenario.expected() ) );

   for ( bool reversed : { false, true } )
   {
      SCOPED_TRACE( reversed ? "listed as a database lists them" : "as is" );
      QString from = bundle;

      if ( reversed )
      {
         from = datapubRecraft( bundle, root + "/gaps_reversed.tar.gz",
                []( US_DataPubManifest& m, const QString& )
                { datapubAsDatabaseExport( m, true, false ); }, error );
         ASSERT_FALSE( from.isEmpty() ) << error.toStdString();
      }

      QString            store = root + ( reversed ? "/gaps_r" : "/gaps_t" );
      US_DataPubImporter importer;
      ASSERT_TRUE( importInto( from, store, importer, error ) )
         << error.toStdString();
      EXPECT_TRUE( orderProblems( importer ).isEmpty() )
         << orderProblems( importer ).join( "\n" ).toStdString();

      EXPECT_EQ( DataPubNoiseRules::latestModelOnDisk( store + "/data",
                                                       editGUID ),
                 scenario.latest() );
      EXPECT_EQ( DataPubBorrowScenario::text( borrowersIn( store ) ),
                 DataPubBorrowScenario::text( scenario.expected() ) );
   }
}

namespace
{
   US_DataPubEntity order_model( const QString& guid, const QString& edit,
                                 int index, const QString& stamp )
   {
      US_DataPubEntity model( US_DataPub::Model );
      model.guid = guid;
      model.setDepend( US_DataPub::EditedData, edit );

      if ( index > 0 )
         model.attrs.insert( "sourceIndex", QString::number( index ) );
      if ( ! stamp.isEmpty() )
         model.attrs.insert( "sourceLastUpdated", stamp );

      return model;
   }

   US_DataPubEntity order_noise( const QString& guid, const QString& model,
                                 const QString& type, int index,
                                 const QString& stamp,
                                 const QString& borrowers = QString() )
   {
      US_DataPubEntity noise( US_DataPub::Noise );
      noise.guid = guid;
      noise.setDepend( US_DataPub::Model, model );
      noise.attrs.insert( "noiseType", type );

      if ( index > 0 )
         noise.attrs.insert( "sourceIndex", QString::number( index ) );
      if ( ! stamp.isEmpty() )
         noise.attrs.insert( "sourceLastUpdated", stamp );
      if ( ! borrowers.isEmpty() )
         noise.attrs.insert( "borrowedByModelGUIDs", borrowers );

      return noise;
   }

   QStringList order_guids( const QList< US_DataPubImporter::OrderStep >& steps )
   {
      QStringList guids;

      for ( const US_DataPubImporter::OrderStep& step : steps )
         guids << step.entity.guid;

      return guids;
   }
}

/* The order itself, on manifests made by hand: the models by their source
   IDs (the highest last), each noise record after its model, in the
   source's time where the stamps say; a database export's manifest order
   reversed where nothing else says. */
TEST( DataPubWriteOrder, ModelsKeepTheSourcesIdOrder )
{
   // 2DSA (300, noise 400) and 2DSA-IT (301, noise 402), listed newest
   // first as a database export lists them
   US_DataPubManifest mani;
   mani.source = "db";
   mani.add( order_model( "m301", "e1", 301, "2024-05-01T10:05:00Z" ) );
   mani.add( order_model( "m300", "e1", 300, "2024-05-01T10:00:00Z" ) );
   mani.add( order_noise( "n402", "m301", "ti", 402, "2024-05-01T10:05:00Z" ) );
   mani.add( order_noise( "n400", "m300", "ti", 400, "2024-05-01T10:00:00Z" ) );

   QStringList problems;
   QList< QList< US_DataPubImporter::OrderStep > > order =
         US_DataPubImporter::writeOrder( mani, problems );

   ASSERT_EQ( order.size(), 1 );
   EXPECT_TRUE( problems.isEmpty() ) << problems.join( "\n" ).toStdString();
   EXPECT_EQ( order_guids( order[ 0 ] ).join( " " ),
              QString( "m300 n400 m301 n402" ) );

   // Without stamps and indices, the manifest's order reversed, each noise
   // record as early as it can go
   US_DataPubManifest old;
   old.source = "db";
   old.add( order_model( "m301", "e1", 0, QString() ) );
   old.add( order_model( "m300", "e1", 0, QString() ) );
   old.add( order_noise( "n402", "m301", "ti", 0, QString() ) );
   old.add( order_noise( "n400", "m300", "ti", 0, QString() ) );

   order = US_DataPubImporter::writeOrder( old, problems );
   ASSERT_EQ( order.size(), 1 );
   EXPECT_EQ( order_guids( order[ 0 ] ).join( " " ),
              QString( "m300 n400 m301 n402" ) );

   // A disk export lists them as the files go
   old.source = "disk";
   order = US_DataPubImporter::writeOrder( old, problems );
   EXPECT_EQ( order_guids( order[ 0 ] ).join( " " ),
              QString( "m301 n402 m300 n400" ) );
}

/* What a bundle's borrowing cannot keep is said, never dropped: a borrower
   of another edit (a catalog lends a model only the noise of its own
   edit), one that is not in the bundle, one named for two records of a
   type, and borrowing that contradicts the rest -- of which only the
   borrowing of the model that closes the circle is left out.  A model that
   borrows is placed by what it borrows, not by its ID or file number: one
   made before the model of the record it borrows is no contradiction. */
TEST( DataPubWriteOrder, BorrowingThatCannotBeKeptIsSaid )
{
   US_DataPubManifest mani;
   mani.source = "disk";
   mani.add( order_model( "a",  "e1", 1, QString() ) );
   mani.add( order_model( "x",  "e1", 2, QString() ) );
   mani.add( order_model( "o",  "e2", 3, QString() ) );
   mani.add( order_noise( "na", "a", "ti", 1, QString(), "o ghost" ) );

   QStringList problems;
   QList< QList< US_DataPubImporter::OrderStep > > order =
         US_DataPubImporter::writeOrder( mani, problems );
   QString said = problems.join( "\n" );

   EXPECT_TRUE( said.contains( "another edit (e2)" ) ) << said.toStdString();
   EXPECT_TRUE( said.contains( "ghost" ) ) << said.toStdString();
   ASSERT_EQ( order.size(), 2 );

   // x, made after a and its noise, is named as borrowing none -- but na
   // comes after a, which comes before x
   US_DataPubManifest twice;
   twice.source = "disk";
   twice.add( order_model( "a",  "e1", 1, QString() ) );
   twice.add( order_model( "b",  "e1", 2, QString() ) );
   twice.add( order_model( "x",  "e1", 3, QString() ) );
   twice.add( order_noise( "na", "a", "ti", 1, QString(), "x" ) );
   twice.add( order_noise( "nb", "b", "ti", 2, QString(), "x" ) );

   order = US_DataPubImporter::writeOrder( twice, problems );
   said  = problems.join( "\n" );
   EXPECT_TRUE( said.contains( "two ti noise records" ) ) << said.toStdString();

   // y borrows n2 but was made (index 2) before n2's model (index 3): a
   // model that borrows is placed by what it borrows, not by its index --
   // there is no contradiction, and no warning
   US_DataPubManifest late;
   late.source = "disk";
   late.add( order_model( "a",  "e1", 1, QString() ) );
   late.add( order_model( "y",  "e1", 2, QString() ) );
   late.add( order_model( "c",  "e1", 3, QString() ) );
   late.add( order_noise( "n1", "a", "ti", 1, QString() ) );
   late.add( order_noise( "n2", "c", "ti", 2, QString(), "y" ) );

   order = US_DataPubImporter::writeOrder( late, problems );
   EXPECT_TRUE( problems.isEmpty() ) << problems.join( "\n" ).toStdString();
   ASSERT_EQ( order.size(), 1 );
   EXPECT_EQ( order_guids( order[ 0 ] ).join( " " ),
              QString( "a n1 c n2 y" ) );

   // A real contradiction: x borrows t1 and r2, y borrows t2 and r1 -- x
   // has to come before t2 and after r2, y after t2 and before r2.  Only
   // y, the later of the two, is left out; z, which borrows t1 and r1,
   // keeps its place, and so does x.
   US_DataPubManifest clash;
   clash.source = "disk";
   clash.add( order_model( "ma", "e1", 1, QString() ) );
   clash.add( order_model( "mb", "e1", 2, QString() ) );
   clash.add( order_model( "mc", "e1", 3, QString() ) );
   clash.add( order_model( "md", "e1", 4, QString() ) );
   clash.add( order_model( "x",  "e1", 5, QString() ) );
   clash.add( order_model( "y",  "e1", 6, QString() ) );
   clash.add( order_model( "z",  "e1", 7, QString() ) );
   clash.add( order_noise( "t1", "ma", "ti", 1, QString(), "x z" ) );
   clash.add( order_noise( "t2", "mb", "ti", 2, QString(), "y" ) );
   clash.add( order_noise( "r1", "mc", "ri", 3, QString(), "y z" ) );
   clash.add( order_noise( "r2", "md", "ri", 4, QString(), "x" ) );

   order = US_DataPubImporter::writeOrder( clash, problems );
   said  = problems.join( "\n" );
   ASSERT_EQ( problems.size(), 1 ) << said.toStdString();
   EXPECT_TRUE( said.contains( "contradicts" )  &&
                said.contains( "model y " ) ) << said.toStdString();
   ASSERT_EQ( order.size(), 1 );

   QStringList at = order_guids( order[ 0 ] );
   ASSERT_EQ( at.size(), 11 );
   EXPECT_LT( at.indexOf( "t1" ), at.indexOf( "x" ) );
   EXPECT_LT( at.indexOf( "r2" ), at.indexOf( "x" ) );
   EXPECT_LT( at.indexOf( "x"  ), at.indexOf( "t2" ) );
   EXPECT_LT( at.indexOf( "t1" ), at.indexOf( "z" ) );
   EXPECT_LT( at.indexOf( "r1" ), at.indexOf( "z" ) );
   EXPECT_LT( at.indexOf( "z"  ), at.indexOf( "t2" ) );
   EXPECT_LT( at.indexOf( "z"  ), at.indexOf( "r2" ) );
}

namespace
{
   /* A source's history as rows: what an export of it carries, and what a
      target written in writeOrder()'s order makes of it, through the
      catalog's rule (noiseBefore()) and count_noise()'s. */
   class OrderHistory
   {
      public:
         class Row
         {
            public:
               QString guid;
               QString edit;
               QString model;           // a noise record's
               QString type;            // a noise record's
               QString desc;            // a model's
               QString request;         // a Monte Carlo model's
               qint64  index = 0;       // ID or file number
               qint64  when  = 0;       // seconds
         };

         QString      source = "db";
         QList< Row > models;
         QList< Row > noises;

         static QString guid( int id )
         {
            return QString( "aaaaaaaa-0000-4000-8000-%1" )
                   .arg( id, 12, 10, QChar( '0' ) );
         }

         static QString stamp( qint64 when )
         {
            return QDateTime::fromSecsSinceEpoch( when, Qt::UTC )
                   .toString( "yyyy-MM-dd hh:mm:ss" ) + " UTC";
         }

         bool fitted( const QString& model ) const
         {
            for ( const Row& noise : noises )
               if ( noise.model == model )  return true;

            return false;
         }

         // noise GUID -> its borrowers, sorted, by the catalog's rule over
         // noise rows of the given IDs (-1: none, as on disk) and times
         QMap< QString, QString > borrowing(
               const QHash< QString, qint64 >& ids,
               const QHash< QString, qint64 >& times ) const
         {
            QMap< QString, QStringList > by;

            for ( const Row& model : models )
            {
               if ( fitted( model.guid ) )  continue;

               QList< US_DataCatalog::Noise > rows;

               for ( const Row& noise : noises )
               {
                  if ( noise.edit != model.edit )  continue;

                  US_DataCatalog::Noise row;
                  row.guid        = noise.guid;
                  row.id          = QString::number( ids.value( noise.guid,
                                                                -1 ) );
                  row.noiseType   = noise.type;
                  row.lastUpdated = stamp( times.value( noise.guid ) );
                  rows << row;
               }

               for ( const US_DataCatalog::Noise& noise :
                     US_DataPubCatalog::noiseBefore(
                        rows, stamp( times.value( model.guid ) ) ) )
                  by[ noise.guid ] << model.guid;
            }

            QMap< QString, QString > map;

            for ( const Row& noise : noises )
            {
               QStringList names = by.value( noise.guid );
               std::sort( names.begin(), names.end() );
               map.insert( noise.guid, names.join( " " ) );
            }

            return map;
         }

         // The source's own borrowing
         QMap< QString, QString > sourceBorrowing( void ) const
         {
            QHash< QString, qint64 > ids;
            QHash< QString, qint64 > times;

            for ( const Row& row : models + noises )
            {
               ids  .insert( row.guid, source == "db" ? row.index : -1 );
               times.insert( row.guid, row.when );
            }

            return borrowing( ids, times );
         }

         // count_noise()'s model of an edit, the models with noise given in
         // the order of their IDs or files: a Monte Carlo request counts
         // once (the lowest ID in a database, the first file on disk); the
         // last one there is
         QString latest( const QString& edit, const QStringList& inOrder ) const
         {
            QStringList requests;
            QString     last;

            for ( const QString& guid : inOrder )
            {
               const Row* model = nullptr;

               for ( const Row& row : models )
                  if ( row.guid == guid )  model = &row;

               if ( model == nullptr  ||  model->edit != edit  ||
                    ! fitted( guid ) )
                  continue;

               if ( ! model->request.isEmpty() )
               {
                  if ( requests.contains( model->request ) )  continue;
                  requests << model->request;
               }

               last = guid;
            }

            return last;
         }

         QStringList edits( void ) const
         {
            QStringList list;

            for ( const Row& row : models )
               if ( ! list.contains( row.edit ) )  list << row.edit;

            return list;
         }

         QString sourceLatest( const QString& edit ) const
         {
            QList< Row > sorted = models;
            std::sort( sorted.begin(), sorted.end(),
                       []( const Row& aa, const Row& bb )
                       { return aa.index < bb.index; } );
            QStringList guids;

            for ( const Row& row : sorted )  guids << row.guid;

            return latest( edit, guids );
         }

         // The manifest an export of the source writes: newest first from a
         // database, by file from a disk; with the borrowing it names
         US_DataPubManifest manifest( void ) const
         {
            US_DataPubManifest       mani;
            QMap< QString, QString > lent = sourceBorrowing();
            mani.source = source;

            QList< Row > ms = models;
            QList< Row > ns = noises;
            auto byIndex = [ & ]( const Row& aa, const Row& bb )
            {
               return source == "db" ? aa.index > bb.index
                                     : aa.index < bb.index;
            };
            std::sort( ms.begin(), ms.end(), byIndex );
            std::sort( ns.begin(), ns.end(), byIndex );

            for ( const Row& row : ms )
            {
               US_DataPubEntity model( US_DataPub::Model );
               model.guid = row.guid;
               model.name = row.desc;
               model.setDepend( US_DataPub::EditedData, row.edit );
               model.attrs.insert( "sourceIndex",
                                   QString::number( row.index ) );
               model.attrs.insert( "sourceLastUpdated", QDateTime::
                                   fromSecsSinceEpoch( row.when, Qt::UTC )
                                   .toString( Qt::ISODate ) );
               mani.add( model );
            }

            for ( const Row& row : ns )
            {
               US_DataPubEntity noise( US_DataPub::Noise );
               noise.guid = row.guid;
               noise.setDepend( US_DataPub::Model, row.model );
               noise.attrs.insert( "noiseType", row.type + "_noise" );
               noise.attrs.insert( "sourceIndex",
                                   QString::number( row.index ) );
               noise.attrs.insert( "sourceLastUpdated", QDateTime::
                                   fromSecsSinceEpoch( row.when, Qt::UTC )
                                   .toString( Qt::ISODate ) );

               if ( ! lent.value( row.guid ).isEmpty() )
                  noise.attrs.insert( "borrowedByModelGUIDs",
                                      lent.value( row.guid ) );

               mani.add( noise );
            }

            return mani;
         }

         /* A target written in the order: each record a second after the
            one before, a database's IDs (and a disk's file numbers) in
            the order written.  Compared with the source per edit, as text
            a failed comparison shows. */
         std::string compare( const QList< QList<
                                 US_DataPubImporter::OrderStep > >& order ) const
         {
            QHash< QString, qint64 > ids;
            QHash< QString, qint64 > times;
            QHash< QString, QStringList > written;    // edit -> models
            qint64                   tick = 1000000;

            for ( const QList< US_DataPubImporter::OrderStep >& steps : order )
               for ( const US_DataPubImporter::OrderStep& step : steps )
               {
                  ids  .insert( step.entity.guid,
                                source == "db" ? tick : -1 );
                  times.insert( step.entity.guid, tick++ );

                  if ( ! step.isNoise )
                     written[ step.editGUID ] << step.entity.guid;
               }

            QStringList lines;

            for ( const QString& edit : edits() )
            {
               QString want = sourceLatest( edit );
               QString got  = latest( edit, written.value( edit ) );

               if ( want != got )
                  lines << "edit " + edit + ": latest " + got + ", source "
                           + want;
            }

            QMap< QString, QString > want = sourceBorrowing();
            QMap< QString, QString > got  = borrowing( ids, times );

            for ( auto it = want.constBegin(); it != want.constEnd(); ++it )
               if ( got.value( it.key() ) != it.value() )
                  lines << "noise " + it.key() + ": [" + got.value( it.key() )
                           + "], source [" + it.value() + "]";

            return lines.join( "\n" ).toStdString();
         }
   };

   /* The review's crafted history: two edits, models with ti and ri noise,
      a Monte Carlo request, three models that borrow, ties to the second;
      "updated": model 301 renamed later, so dated after 306. */
   OrderHistory crafted_history( bool updated )
   {
      OrderHistory history;
      QDateTime    day( QDate( 2026, 1, 1 ), QTime( 0, 0 ), Qt::UTC );
      auto at = [ & ]( const QString& hms )
      {
         return day.addSecs( QTime( 0, 0 ).secsTo(
                   QTime::fromString( hms, "hh:mm:ss" ) ) ).toSecsSinceEpoch();
      };

      struct M { int id; int edit; const char* time; const char* desc;
                 const char* request; };
      struct N { int id; int model; int edit; const char* type;
                 const char* time; };

      const M models[] = {
         { 300, 200, "10:00:00", "MWL_demo_run.1A280.2dsa.model", "" },
         { 301, 200, updated ? "10:50:00" : "10:05:00",
                          "MWL_demo_run.1A280.e1-2DSA.model", "" },
         { 302, 200, "10:10:00", "MWL_demo_run.1A280.e2-2DSA-IT.model", "" },
         { 303, 200, "10:20:00", "MWL_demo_run.1A280.e3-GA.model", "" },
         { 304, 200, "10:30:00", "MWL_demo_run.1A280.e4-2DSA-MC_r1_mc0001.model", "r1" },
         { 305, 200, "10:30:00", "MWL_demo_run.1A280.e4-2DSA-MC_r1_mc0002.model", "r1" },
         { 306, 200, "10:40:00", "MWL_demo_run.1A280.e5-PCSA.model", "" },
         { 310, 201, "09:00:00", "MWL_demo_run.1A280.f1-2DSA.model", "" },
         { 311, 201, "09:30:00", "MWL_demo_run.1A280.f2-GA.model", "" },
         { 312, 201, "10:15:00", "MWL_demo_run.1A280.f3-2DSA.model", "" } };
      const N noises[] = {
         { 400, 300, 200, "ti", "10:00:00" },
         { 401, 302, 200, "ti", "10:11:00" }, { 402, 302, 200, "ri", "10:11:00" },
         { 403, 304, 200, "ti", "10:31:00" }, { 405, 305, 200, "ti", "10:31:00" },
         { 410, 310, 201, "ri", "09:01:00" }, { 411, 312, 201, "ti", "10:15:30" } };

      for ( const M& m : models )
      {
         OrderHistory::Row row;
         row.guid    = OrderHistory::guid( m.id );
         row.edit    = OrderHistory::guid( m.edit );
         row.desc    = m.desc;
         row.request = m.request;
         row.index   = m.id;
         row.when    = at( m.time );
         history.models << row;
      }

      for ( const N& n : noises )
      {
         OrderHistory::Row row;
         row.guid  = OrderHistory::guid( n.id );
         row.edit  = OrderHistory::guid( n.edit );
         row.model = OrderHistory::guid( n.model );
         row.type  = n.type;
         row.index = n.id;
         row.when  = at( n.time );
         history.noises << row;
      }

      return history;
   }
}

/* The review's crafted history -- also with a model whose time stamp moved
   on a later update, so that its ID no longer follows its time -- and a
   disk store whose file numbers have gaps filled after removals: written
   in writeOrder()'s order, a target keeps count_noise()'s model of every
   edit and every model's borrowing, with no warning.  The import used to
   chain every model by its ID or file number, took the moved time stamp or
   the filled gap for a contradiction, and dropped all the borrowing of the
   edit. */
TEST( DataPubWriteOrder, ABorrowerIsPlacedByWhatItBorrowsAlone )
{
   for ( bool updated : { false, true } )
   {
      SCOPED_TRACE( updated ? "a model updated later" : "the crafted history" );
      OrderHistory history = crafted_history( updated );

      if ( updated )
      {  // The update makes 301 borrow other records than before
         EXPECT_NE( history.sourceBorrowing(),
                    crafted_history( false ).sourceBorrowing() );
      }

      QStringList problems;
      QList< QList< US_DataPubImporter::OrderStep > > order =
            US_DataPubImporter::writeOrder( history.manifest(), problems );

      EXPECT_TRUE( problems.isEmpty() ) << problems.join( "\n" ).toStdString();
      EXPECT_EQ( order.size(), 2 );
      EXPECT_EQ( history.compare( order ), std::string() );
   }

   // On disk: files numbered as get_filename() numbers them, the first gap
   // first.  Models 1, 11, 12, 13 have noise; 5 and 2 filled gaps later and
   // borrow; noise file 3 filled a gap too.
   OrderHistory gaps;
   gaps.source = "disk";
   QDateTime t0( QDate( 2026, 1, 1 ), QTime( 10, 0 ), Qt::UTC );
   struct G { int file; int seconds; };
   const G models[] = { { 1, 0 }, { 11, 10 }, { 12, 20 }, { 5, 30 },
                        { 13, 40 }, { 2, 50 } };
   const int  fittedTo[] = { 1, 11, 12, 13 };
   const char* types[]   = { "ti", "ri", "ti", "ti" };
   const int  noiseFile[] = { 1, 11, 12, 3 };

   for ( const G& g : models )
   {
      OrderHistory::Row row;
      row.guid  = OrderHistory::guid( 1000 + g.file );
      row.edit  = OrderHistory::guid( 200 );
      row.desc  = QString( "gap.1A280.m%1.model" ).arg( g.file );
      row.index = g.file;
      row.when  = t0.addSecs( g.seconds ).toSecsSinceEpoch();
      gaps.models << row;
   }

   for ( int ii = 0; ii < 4; ii++ )
   {
      OrderHistory::Row row;
      row.guid  = OrderHistory::guid( 2000 + noiseFile[ ii ] );
      row.edit  = OrderHistory::guid( 200 );
      row.model = OrderHistory::guid( 1000 + fittedTo[ ii ] );
      row.type  = types[ ii ];
      row.index = noiseFile[ ii ];

      for ( const OrderHistory::Row& model : gaps.models )
         if ( model.guid == row.model )  row.when = model.when;

      gaps.noises << row;
   }

   QStringList problems;
   QList< QList< US_DataPubImporter::OrderStep > > order =
         US_DataPubImporter::writeOrder( gaps.manifest(), problems );

   EXPECT_TRUE( problems.isEmpty() ) << problems.join( "\n" ).toStdString();
   EXPECT_EQ( gaps.compare( order ), std::string() );
}

/* The catalog's time stamps are UTC, and are read as UTC: in a time zone
   with a spring-forward gap (Europe/Berlin, 2024-03-31 02:00-03:00 local)
   a stamp of that hour, read as a local time, came out an hour late or
   invalid, and a model borrowed a noise record made after it. */
TEST( DataPubNoiseBefore, TheSpringForwardGapIsNoGap )
{
   QByteArray saved = qgetenv( "TZ" );
   qputenv( "TZ", "Europe/Berlin" );
   tzset();

   US_DataCatalog::Noise later;
   later.guid        = "aaaaaaaa-0000-4000-8000-000000000001";
   later.id          = "-1";
   later.noiseType   = "ti";
   later.lastUpdated = "2024-03-31 03:10:00 UTC";

   US_DataCatalog::Noise earlier = later;
   earlier.guid        = "bbbbbbbb-0000-4000-8000-000000000001";
   earlier.lastUpdated = "2024-03-31 01:00:00 UTC";

   QList< US_DataCatalog::Noise > both;
   both << later << earlier;

   // Made at 02:30 UTC: the noise of 03:10 UTC is newer
   QList< US_DataCatalog::Noise > picked =
         US_DataPubCatalog::noiseBefore( both, "2024-03-31 02:30:00 UTC" );
   QList< US_DataCatalog::Noise > db =
         US_DataPubCatalog::noiseBefore( both, "2024-03-31 02:30:00" );

   if ( saved.isNull() )  qunsetenv( "TZ" );
   else                   qputenv( "TZ", saved );
   tzset();

   ASSERT_EQ( picked.size(), 1 );
   EXPECT_EQ( picked[ 0 ].guid, earlier.guid );
   ASSERT_EQ( db.size(), 1 );
   EXPECT_EQ( db[ 0 ].guid, earlier.guid );

   qint64 seconds = 0;
   ASSERT_TRUE( US_DataPub::utcStampSeconds( "2024-03-31T02:30:00Z", seconds ) );
   EXPECT_EQ( seconds, QDateTime( QDate( 2024, 3, 31 ), QTime( 2, 30 ),
                                  Qt::UTC ).toSecsSinceEpoch() );
   ASSERT_TRUE( US_DataPub::utcStampSeconds( "2024-03-31 04:30:00+02:00",
                                             seconds ) );
   EXPECT_EQ( US_DataPub::isoUtcStamp( "2024-03-31 04:30:00+02:00" ),
              QString( "2024-03-31T02:30:00Z" ) );
}

/* A record the store already holds under its GUID is reused, and its name
   counts as the bundle's: another record of the bundle of the same name --
   the source holds both under it -- is no conflict and keeps the name, as
   it does when the import writes both. */
TEST_F( DataPubImport, AReusedRecordsNameIsNoConflictForItsTwin )
{
   QString  twinGUID = US_Util::new_guid();
   US_Model twin;
   twin.modelGUID   = twinGUID;
   twin.editGUID    = editGUID;
   twin.description = "demo_run.1A280.2dsa.model";      // the fixture's
   twin.write( US_Settings::dataDir() + "/models/M0000002.xml" );

   US_DataPubExporter            exporter;
   US_DataPubExporter::Selection selection;
   selection.fromDb = false;
   selection.scope  = US_DataPub::ScopeNoise;
   selection.runIDs << runID;

   QString error;
   QString bundle = root + "/twins.tar.gz";
   ASSERT_TRUE( exporter.exportBundle( selection, bundle, error ) )
      << error.toStdString();

   // The store holds the fixture's model already, under its GUID
   QDir().mkpath( targetDir + "/data/models" );
   ASSERT_TRUE( QFile::copy( US_Settings::dataDir() + "/models/M0000001.xml",
                             targetDir + "/data/models/M0000001.xml" ) );

   for ( int dry = 1; dry >= 0; dry-- )
   {
      US_DataPubImporter          importer;
      US_DataPubImporter::Options options = diskOptions();
      options.dryRun = ( dry == 1 );
      options.policy = US_DataPub::PolicyRename;
      ASSERT_TRUE( importer.inspect  ( bundle, error ) ) << error.toStdString();
      ASSERT_TRUE( importer.runImport( options, error ) ) << error.toStdString();

      for ( const US_DataPubImporter::Result& result : importer.results() )
      {
         if ( result.type != US_DataPub::Model )  continue;

         EXPECT_EQ( result.targetName, QString( "demo_run.1A280.2dsa.model" ) )
            << ( dry ? "dry run" : "import" );
         EXPECT_EQ( result.resolution, result.guid == twinGUID
                                       ? US_DataPub::ResolvedCreated
                                       : US_DataPub::ResolvedReused );
      }
   }
}

/* A rename suffix given in brackets is not bracketed again. */
TEST_F( DataPubImport, ARenameSuffixInBracketsIsBracketedOnce )
{
   // The store holds another model of the fixture model's name
   QDir().mkpath( targetDir + "/data/models" );
   US_Model held;
   held.modelGUID   = US_Util::new_guid();
   held.editGUID    = US_Util::new_guid();
   held.description = "demo_run.1A280.2dsa.model";
   ASSERT_EQ( held.write( targetDir + "/data/models/M0000001.xml" ),
              IUS_DB2::OK );

   US_DataPubImporter          importer;
   US_DataPubImporter::Options options = diskOptions();
   options.policy       = US_DataPub::PolicyRename;
   options.renameSuffix = "(copy)";
   QString error;
   ASSERT_TRUE( importer.inspect  ( bundlePath, error ) ) << error.toStdString();
   ASSERT_TRUE( importer.runImport( options, error ) ) << error.toStdString();

   bool renamed = false;

   for ( const US_DataPubImporter::Result& result : importer.results() )
      if ( result.type == US_DataPub::Model )
      {
         renamed = true;
         EXPECT_EQ( result.targetName,
                    QString( "demo_run.1A280.2dsa.model (copy)" ) );
      }

   EXPECT_TRUE( renamed );
}
