// test/programs/us_data_publication/test_us_datapub_bundle.cpp
#include <gtest/gtest.h>

#include <QtCore>

#include "datapub_test_env.h"

#include "us_datapub_bundle.h"
#include "us_datapub_manifest.h"
#include "us_datapub_hash.h"

class DataPubBundle : public DataPubTestEnv {};

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
