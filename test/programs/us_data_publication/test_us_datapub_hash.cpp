// test/programs/us_data_publication/test_us_datapub_hash.cpp
#include <gtest/gtest.h>

#include <QtCore>

#include "us_datapub_hash.h"

static QString writeTemp( const QString& name, const QByteArray& content )
{
   QString path = QDir::tempPath() + "/us_datapub_hash_" + name;
   QFile   file( path );

   if ( ! file.open( QIODevice::WriteOnly ) )  return QString();

   file.write( content );
   file.close();

   return path;
}

TEST( DataPubHash, FileHashIsTheDigestOfTheBytes )
{
   QString path = writeTemp( "plain.bin", QByteArray( "hello bundle" ) );
   ASSERT_FALSE( path.isEmpty() );

   EXPECT_EQ( US_DataPubHash::fileHash( path ),
              US_DataPubHash::dataHash( QByteArray( "hello bundle" ) ) );

   QFile::remove( path );
}

TEST( DataPubHash, MissingFileHasNoDigest )
{
   EXPECT_TRUE( US_DataPubHash::fileHash( "/no/such/file" ).isEmpty() );
}

TEST( DataPubHash, VolatileKeysAreRecognized )
{
   EXPECT_TRUE ( US_DataPubHash::isVolatileKey( "id" ) );
   EXPECT_TRUE ( US_DataPubHash::isVolatileKey( "editedDataID" ) );
   EXPECT_TRUE ( US_DataPubHash::isVolatileKey( "rotorID" ) );
   EXPECT_TRUE ( US_DataPubHash::isVolatileKey( "lastUpdated" ) );
   EXPECT_TRUE ( US_DataPubHash::isVolatileKey( "timeCreated" ) );
   EXPECT_TRUE ( US_DataPubHash::isVolatileKey( "date" ) );

   EXPECT_FALSE( US_DataPubHash::isVolatileKey( "description" ) );
   EXPECT_FALSE( US_DataPubHash::isVolatileKey( "density" ) );
   EXPECT_FALSE( US_DataPubHash::isVolatileKey( "vbar20" ) );
}

// The list is explicit: a property that merely ends in "id" or a reference
// that merely contains "guid" is part of what the record means
TEST( DataPubHash, RealPropertiesAndReferencesAreNotVolatile )
{
   EXPECT_FALSE( US_DataPubHash::isVolatileKey( "solid" ) );
   EXPECT_FALSE( US_DataPubHash::isVolatileKey( "liquid" ) );
   EXPECT_FALSE( US_DataPubHash::isVolatileKey( "editGUID" ) );
   EXPECT_FALSE( US_DataPubHash::isVolatileKey( "modelGUID" ) );
   EXPECT_FALSE( US_DataPubHash::isVolatileKey( "guid" ) );
}

TEST( DataPubHash, FingerprintSeesAPropertyEndingInId )
{
   QByteArray one = "<r><component name=\"salt\" solid=\"1\"/></r>";
   QByteArray two = "<r><component name=\"salt\" solid=\"0\"/></r>";

   EXPECT_NE( US_DataPubHash::xmlFingerprint( one ),
              US_DataPubHash::xmlFingerprint( two ) );
}

// A solution of another buffer is another solution, even when everything
// else about it is the same
TEST( DataPubHash, FingerprintSeesADifferentParent )
{
   QByteArray one =
      "<SolutionData><solution id=\"1\" guid=\"11111111-1111-1111-1111-111111111111\">"
      "<buffer id=\"3\" guid=\"aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa\" desc=\"b\"/>"
      "</solution></SolutionData>";
   QByteArray two =
      "<SolutionData><solution id=\"2\" guid=\"22222222-2222-2222-2222-222222222222\">"
      "<buffer id=\"3\" guid=\"bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb\" desc=\"b\"/>"
      "</solution></SolutionData>";
   QByteArray three =
      "<SolutionData><solution id=\"9\" guid=\"33333333-3333-3333-3333-333333333333\">"
      "<buffer id=\"8\" guid=\"aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa\" desc=\"b\"/>"
      "</solution></SolutionData>";

   EXPECT_NE( US_DataPubHash::xmlFingerprint( one, "solution", "guid" ),
              US_DataPubHash::xmlFingerprint( two, "solution", "guid" ) );

   // Its own identity and the numeric IDs are what may differ
   EXPECT_EQ( US_DataPubHash::xmlFingerprint( one,   "solution", "guid" ),
              US_DataPubHash::xmlFingerprint( three, "solution", "guid" ) );
}

TEST( DataPubHash, AnOversizedXmlPayloadHasNoFingerprint )
{
   QString path = QDir::tempPath() + "/us_datapub_hash_big.xml";
   QFile   file( path );
   ASSERT_TRUE( file.open( QIODevice::WriteOnly ) );
   ASSERT_TRUE( file.resize( US_DataPubHash::maxXmlSize + 16 ) );
   file.close();

   EXPECT_TRUE( US_DataPubHash::fingerprint( path ).isEmpty() );

   QFile::remove( path );
}

// Two copies of the same record that were made in different installations
// differ in their IDs and time stamps but describe the same thing; the
// fingerprint has to see through that, or an import could never reuse a
// record it already has.
TEST( DataPubHash, FingerprintIgnoresIdentityAndTimestamps )
{
   QByteArray one =
      "<BufferData version=\"1.0\">"
      "<buffer id=\"3\" guid=\"11111111-1111-1111-1111-111111111111\""
      " description=\"Demo buffer\" density=\"1.0\" lastUpdated=\"2024-01-01\">"
      "</buffer></BufferData>";

   QByteArray two =
      "<BufferData version=\"1.0\">"
      "<buffer id=\"98\" guid=\"22222222-2222-2222-2222-222222222222\""
      " description=\"Demo buffer\" density=\"1.0\" lastUpdated=\"2026-09-12\">"
      "</buffer></BufferData>";

   EXPECT_EQ( US_DataPubHash::xmlFingerprint( one, "buffer", "guid" ),
              US_DataPubHash::xmlFingerprint( two, "buffer", "guid" ) );
   EXPECT_NE( US_DataPubHash::dataHash( one ),
              US_DataPubHash::dataHash( two ) );
}

TEST( DataPubHash, FingerprintSeesRealDifferences )
{
   QByteArray one =
      "<BufferData><buffer id=\"3\" description=\"Demo buffer\""
      " density=\"1.0\"></buffer></BufferData>";
   QByteArray two =
      "<BufferData><buffer id=\"3\" description=\"Demo buffer\""
      " density=\"1.5\"></buffer></BufferData>";

   EXPECT_NE( US_DataPubHash::xmlFingerprint( one ),
              US_DataPubHash::xmlFingerprint( two ) );
}

TEST( DataPubHash, FingerprintIgnoresAttributeOrderAndWhitespace )
{
   QByteArray one = "<r><e a=\"1\" b=\"2\">  text  </e></r>";
   QByteArray two = "<r>\n  <e b=\"2\" a=\"1\">text</e>\n</r>";

   EXPECT_EQ( US_DataPubHash::xmlFingerprint( one ),
              US_DataPubHash::xmlFingerprint( two ) );
}

TEST( DataPubHash, BinaryPayloadsFallBackToTheFileDigest )
{
   QByteArray content( "\x01\x02\x03 not xml at all", 24 );
   QString    path = writeTemp( "binary.auc", content );
   ASSERT_FALSE( path.isEmpty() );

   EXPECT_EQ( US_DataPubHash::fingerprint( path ),
              US_DataPubHash::fileHash( path ) );

   QFile::remove( path );
}
