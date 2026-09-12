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
