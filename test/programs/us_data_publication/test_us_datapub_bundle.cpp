// test/programs/us_data_publication/test_us_datapub_bundle.cpp
#include <gtest/gtest.h>

#include <QtCore>

#include "datapub_test_env.h"

#include "us_datapub_bundle.h"
#include "us_datapub_manifest.h"
#include "us_datapub_hash.h"

class DataPubBundle : public DataPubTestEnv {};

// The fixture changes "workBaseDir"; that setting has to land in the test
// sandbox, never in the settings of the person running the tests
TEST( DataPubTestSandbox, TheFixtureOnlyRunsInTheSettingsSandbox )
{
   QString why;
   ASSERT_TRUE( datapub_settings_sandboxed( &why ) ) << why.toStdString();

   US_SettingsStore store;
   EXPECT_TRUE( QFileInfo( store.fileName() ).absoluteFilePath().startsWith(
                QDir( qEnvironmentVariable( "US3_TEST_SANDBOX" ) )
                .absolutePath() + "/" ) ) << store.fileName().toStdString();

   // A root the store does not use is no sandbox: the check looks at where
   // the settings really go, not at the variable alone
   QByteArray saved = qgetenv( "US3_SETTINGS_ROOT" );
   qputenv( "US3_SETTINGS_ROOT", QFile::encodeName( QDir::tempPath()
                                 + "/somewhere_else" ) );
   EXPECT_FALSE( datapub_settings_sandboxed( &why ) );
   EXPECT_FALSE( why.isEmpty() );

   qunsetenv( "US3_SETTINGS_ROOT" );
   EXPECT_FALSE( datapub_settings_sandboxed( &why ) );

   qputenv( "US3_SETTINGS_ROOT", saved );
   EXPECT_TRUE( datapub_settings_sandboxed() );
}

TEST_F( DataPubBundle, StageAndPackAndUnpack )
{
   US_DataPubBundle bundle;
   QString          error;

   ASSERT_TRUE( bundle.createStaging( error ) ) << error.toStdString();
   ASSERT_FALSE( bundle.stagingPath().isEmpty() );

   QString nested = bundle.stagedPath( "models/deep/one.xml" );
   ASSERT_FALSE( nested.isEmpty() );
   EXPECT_TRUE ( QDir( QFileInfo( nested ).absolutePath() ).exists() );

   QFile file( nested );
   ASSERT_TRUE( file.open( QIODevice::WriteOnly | QIODevice::Text ) );
   file.write( "<model description=\"packed\"/>" );
   file.close();

   US_DataPubManifest mani;
   mani.scope  = "models";
   mani.source = "disk";
   ASSERT_TRUE( mani.write( bundle.stagedPath(
                US_DataPubBundle::manifestName() ), error ) )
      << error.toStdString();

   // A name with more than one dot used to defeat the archive's format
   // detection, so it is exactly what the test packs to.
   QString archive = root + "/demo.project.v1.tar.gz";
   ASSERT_TRUE( bundle.pack( archive, error ) ) << error.toStdString();
   ASSERT_TRUE( QFile::exists( archive ) );

   US_DataPubBundle reopened;
   ASSERT_TRUE( reopened.unpack( archive, error ) ) << error.toStdString();

   QString manifestPath = reopened.rootPath() + "/"
                          + US_DataPubBundle::manifestName();
   EXPECT_TRUE( QFile::exists( manifestPath ) );
   EXPECT_TRUE( QFile::exists( reopened.rootPath()
                               + "/models/deep/one.xml" ) );

   US_DataPubManifest read;
   ASSERT_TRUE( read.read( manifestPath, error ) ) << error.toStdString();
   EXPECT_EQ( read.scope,  "models" );
   EXPECT_EQ( read.source, "disk" );
}

TEST_F( DataPubBundle, PackingNothingFails )
{
   US_DataPubBundle bundle;
   QString          error;

   ASSERT_TRUE ( bundle.createStaging( error ) );
   EXPECT_FALSE( bundle.pack( root + "/empty.tar.gz", error ) );
   EXPECT_FALSE( error.isEmpty() );
}

TEST_F( DataPubBundle, UnpackingSomethingElseFails )
{
   QString  path = root + "/not_a_bundle.tar.gz";
   QFile    file( path );
   ASSERT_TRUE( file.open( QIODevice::WriteOnly ) );
   file.write( "this is not an archive" );
   file.close();

   US_DataPubBundle bundle;
   QString          error;

   EXPECT_FALSE( bundle.unpack( path, error ) );
   EXPECT_FALSE( error.isEmpty() );
   EXPECT_TRUE ( bundle.stagingPath().isEmpty() );
}

TEST_F( DataPubBundle, StagingIsRemovedOnCleanup )
{
   QString staging;
   QString error;

   {
      US_DataPubBundle bundle;
      ASSERT_TRUE( bundle.createStaging( error ) );
      staging = bundle.stagingPath();
      EXPECT_TRUE( QDir( staging ).exists() );
   }

   EXPECT_FALSE( QDir( staging ).exists() );
}

namespace
{
   // A minimal ustar archive, written by hand so that it can hold entries
   // no well-behaved packer would write
   QByteArray tar_entry( const QString& name, const QByteArray& data )
   {
      QByteArray header( 512, '\0' );
      QByteArray bname = name.toUtf8();

      auto put = [ &header ]( int at, const QByteArray& text )
      {
         for ( int ii = 0; ii < text.size(); ii++ )
            header[ at + ii ] = text[ ii ];
      };

      put(   0, bname.left( 100 ) );
      put( 100, "0000644" );
      put( 108, "0000000" );
      put( 116, "0000000" );
      put( 124, QString( "%1" ).arg( data.size(), 11, 8, QChar( '0' ) )
                .toLatin1() );
      put( 136, "00000000000" );
      put( 148, "        " );
      header[ 156 ] = '0';
      put( 257, "ustar" );
      put( 263, "00" );

      unsigned sum = 0;

      for ( int ii = 0; ii < 512; ii++ )
         sum += static_cast< unsigned char >( header[ ii ] );

      put( 148, QString( "%1" ).arg( sum, 6, 8, QChar( '0' ) ).toLatin1() );
      header[ 154 ] = '\0';
      header[ 155 ] = ' ';

      QByteArray body = data;
      body.append( QByteArray( ( 512 - data.size() % 512 ) % 512, '\0' ) );

      return header + body;
   }
}

// Packing goes to a work file first and replaces the target in one step:
// the old file is either still there, whole, or replaced by the new one
TEST_F( DataPubBundle, PackReplacesAnExistingFile )
{
   QString archive = root + "/replace.tar.gz";
   QFile   old( archive );
   ASSERT_TRUE( old.open( QIODevice::WriteOnly ) );
   old.write( "an older bundle" );
   old.close();

   US_DataPubBundle bundle;
   QString          error;
   ASSERT_TRUE( bundle.createStaging( error ) ) << error.toStdString();

   US_DataPubManifest mani;
   ASSERT_TRUE( mani.write( bundle.stagedPath(
                US_DataPubBundle::manifestName() ), error ) );
   ASSERT_TRUE( bundle.pack( archive, error ) ) << error.toStdString();

   US_DataPubBundle reopened;
   EXPECT_TRUE( reopened.unpack( archive, error ) ) << error.toStdString();

   // Nothing is left behind next to the target
   QStringList beside = QDir( root ).entryList( QDir::Files | QDir::Hidden );
   EXPECT_EQ( beside, QStringList() << "replace.tar.gz" );
}

// A hidden file would be left out of the archive without a word, so a pack
// that would lose one fails -- and leaves the existing target alone
TEST_F( DataPubBundle, AFailedPackLeavesTheTargetAlone )
{
   QString archive = root + "/kept.tar.gz";
   QFile   old( archive );
   ASSERT_TRUE( old.open( QIODevice::WriteOnly ) );
   old.write( "an older bundle" );
   old.close();

   US_DataPubBundle bundle;
   QString          error;
   ASSERT_TRUE( bundle.createStaging( error ) );

   US_DataPubManifest mani;
   ASSERT_TRUE( mani.write( bundle.stagedPath(
                US_DataPubBundle::manifestName() ), error ) );

   QFile hidden( bundle.stagingPath() + "/models/.secret.xml" );
   QDir().mkpath( bundle.stagingPath() + "/models" );
   ASSERT_TRUE( hidden.open( QIODevice::WriteOnly ) );
   hidden.write( "<model/>" );
   hidden.close();

   EXPECT_FALSE( bundle.pack( archive, error ) );
   EXPECT_TRUE ( error.contains( ".secret.xml" ) ) << error.toStdString();

   ASSERT_TRUE( old.open( QIODevice::ReadOnly ) );
   EXPECT_EQ( old.readAll(), QByteArray( "an older bundle" ) );
   old.close();
}

TEST_F( DataPubBundle, PackingIntoAnUnwritablePlaceFails )
{
   // A directory cannot be made under a plain file
   QString blocker = root + "/blocker";
   QFile   file( blocker );
   ASSERT_TRUE( file.open( QIODevice::WriteOnly ) );
   file.close();

   US_DataPubBundle bundle;
   QString          error;
   ASSERT_TRUE( bundle.createStaging( error ) );

   US_DataPubManifest mani;
   ASSERT_TRUE( mani.write( bundle.stagedPath(
                US_DataPubBundle::manifestName() ), error ) );

   EXPECT_FALSE( bundle.pack( blocker + "/sub/out.tar.gz", error ) );
   EXPECT_FALSE( error.isEmpty() );
}

TEST_F( DataPubBundle, StagingDirectoriesAreNeverShared )
{
   US_DataPubBundle one;
   US_DataPubBundle two;
   QString          error;

   ASSERT_TRUE( one.createStaging( error ) );
   ASSERT_TRUE( two.createStaging( error ) );

   EXPECT_NE( one.stagingPath(), two.stagingPath() );
   EXPECT_NE( one.workPath(),    two.workPath() );

   // The scratch space is beside the bundle tree, never inside it
   EXPECT_FALSE( one.workPath().startsWith( one.stagingPath() + "/" ) );
}

TEST_F( DataPubBundle, NothingIsStagedOutsideTheTree )
{
   US_DataPubBundle bundle;
   QString          error;
   ASSERT_TRUE( bundle.createStaging( error ) );

   EXPECT_TRUE ( bundle.stagedPath( "../escape.xml" ).isEmpty() );
   EXPECT_TRUE ( bundle.stagedPath( "/tmp/escape.xml" ).isEmpty() );
   EXPECT_FALSE( bundle.stagedPath( "models/fine.xml" ).isEmpty() );
}

// An archive whose entry climbs out of the extraction directory is refused,
// whatever the archive reader did with the entry
TEST_F( DataPubBundle, ABundleThatWritesOutsideItselfIsRefused )
{
   QByteArray tar = tar_entry( "manifest.yaml", "version: 1\n" )
                    + tar_entry( "../escaped.txt", "out of bounds" )
                    + QByteArray( 1024, '\0' );

   QString path = root + "/slip.tar";
   QFile   file( path );
   ASSERT_TRUE( file.open( QIODevice::WriteOnly ) );
   file.write( tar );
   file.close();

   US_DataPubBundle bundle;
   QString          error;

   EXPECT_FALSE( bundle.unpack( path, error ) );
   EXPECT_FALSE( error.isEmpty() );
   EXPECT_TRUE ( bundle.stagingPath().isEmpty() );
}

TEST_F( DataPubBundle, AnArchiveWithoutAManifestLeavesNothingBehind )
{
   QByteArray tar = tar_entry( "readme.txt", "no manifest here" )
                    + QByteArray( 1024, '\0' );

   QString path = root + "/nomanifest.tar";
   QFile   file( path );
   ASSERT_TRUE( file.open( QIODevice::WriteOnly ) );
   file.write( tar );
   file.close();

   US_DataPubBundle bundle;
   QString          error;

   EXPECT_FALSE( bundle.unpack( path, error ) );
   EXPECT_TRUE ( error.contains( "holds no" ) ) << error.toStdString();
   EXPECT_TRUE ( bundle.stagingPath().isEmpty() );
}

/* Taking back what was staged since a mark removes what was first staged
   after it, and the directories that are left empty -- nothing staged
   before the mark, even when it was handed out again afterwards, and
   nothing it did not hand out: it looks at what it staged, not at the
   tree.
*/
TEST_F( DataPubBundle, UnstagingTakesBackOnlyWhatWasStagedSinceTheMark )
{
   US_DataPubBundle bundle;
   QString          error;
   ASSERT_TRUE( bundle.createStaging( error ) ) << error.toStdString();

   auto put = [ & ]( const QString& rel )
   {
      QFile file( bundle.stagedPath( rel ) );
      ASSERT_TRUE( file.open( QIODevice::WriteOnly ) );
      file.write( rel.toUtf8() );
      file.close();
   };

   put( "buffers/shared.xml" );

   int mark = bundle.stagedMark();
   put( "buffers/shared.xml" );           // Handed out again: still kept
   put( "rawData/bad_run/bad.auc" );
   put( "experiments/bad_run.RA.xml" );

   // A file nobody staged is not looked at
   QFile stray( bundle.stagingPath() + "/experiments/stray.xml" );
   ASSERT_TRUE( stray.open( QIODevice::WriteOnly ) );
   stray.close();

   ASSERT_TRUE( bundle.unstageSince( mark, error ) ) << error.toStdString();

   QString top = bundle.stagingPath();
   EXPECT_TRUE ( QFile::exists( top + "/buffers/shared.xml" ) );
   EXPECT_FALSE( QFile::exists( top + "/rawData/bad_run/bad.auc" ) );
   EXPECT_FALSE( QFile::exists( top + "/rawData" ) );
   EXPECT_FALSE( QFile::exists( top + "/experiments/bad_run.RA.xml" ) );
   EXPECT_TRUE ( QFile::exists( top + "/experiments/stray.xml" ) );
   EXPECT_EQ   ( bundle.stagedMark(), mark );

   // What is staged again after that is a stage of its own
   put( "rawData/good_run/good.auc" );
   ASSERT_TRUE( bundle.unstageSince( mark, error ) );
   EXPECT_FALSE( QFile::exists( top + "/rawData/good_run/good.auc" ) );
   EXPECT_TRUE ( QFile::exists( top + "/buffers/shared.xml" ) );
}

#if defined( Q_OS_UNIX )
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

namespace
{
   void make_old( const QString& path )
   {
      QDateTime past = QDateTime::currentDateTimeUtc().addDays( -3 );
      struct timeval times[ 2 ];
      times[ 0 ].tv_sec  = time_t( past.toSecsSinceEpoch() );
      times[ 0 ].tv_usec = 0;
      times[ 1 ]         = times[ 0 ];
      ASSERT_EQ( utimes( QFile::encodeName( path ).constData(), times ), 0 );
   }
}

/* The sweep removes the staging directories of exporters and importers
   that were killed -- a lock of a process that is gone, of either name --
   and directories without a lock that have not changed for days, but never
   the directory of a process that is still running, however old.
*/
TEST_F( DataPubBundle, TheSweepRemovesOnlyWhatNoRunningProcessHolds )
{
   QString parent = US_Settings::tmpDir() + "/us_datapub";

   US_DataPubBundle live;
   QString          error;
   live.setStagingLock( "export.lock" );
   ASSERT_TRUE( live.createStaging( error ) ) << error.toStdString();
   QString liveDir = QFileInfo( live.stagingPath() ).path();
   make_old( liveDir );

   const QStringList names = { "export.lock", "import.lock" };

   for ( const QString& name : names )
   {
      QString dir = parent + "/bundle_dead_" + name.section( '.', 0, 0 );
      ASSERT_TRUE( QDir().mkpath( dir + "/bundle" ) );

      fflush( nullptr );
      pid_t pid = fork();
      ASSERT_GE( pid, 0 );

      if ( pid == 0 )
      {
         QLockFile held( dir + "/" + name );
         held.tryLock( 0 );
         _exit( held.isLocked() ? 0 : 1 );    // Without unlocking it
      }

      int status = 0;
      ASSERT_EQ( waitpid( pid, &status, 0 ), pid );
      ASSERT_TRUE( WIFEXITED( status )  &&  WEXITSTATUS( status ) == 0 );
      ASSERT_TRUE( QFile::exists( dir + "/" + name ) );
   }

   ASSERT_TRUE( QDir().mkpath( parent + "/bundle_old" ) );
   make_old( parent + "/bundle_old" );
   ASSERT_TRUE( QDir().mkpath( parent + "/bundle_new" ) );

   EXPECT_EQ   ( US_DataPubBundle::sweepStaleStaging(), 3 );
   EXPECT_FALSE( QFile::exists( parent + "/bundle_dead_export" ) );
   EXPECT_FALSE( QFile::exists( parent + "/bundle_dead_import" ) );
   EXPECT_FALSE( QFile::exists( parent + "/bundle_old" ) );
   EXPECT_TRUE ( QFile::exists( parent + "/bundle_new" ) );
   EXPECT_TRUE ( QFile::exists( live.stagingPath() ) );

   live.cleanup();
   EXPECT_FALSE( QFile::exists( liveDir ) );
   QDir( parent + "/bundle_new" ).removeRecursively();
}
#endif
