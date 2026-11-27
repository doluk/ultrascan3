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

// A buffer's component element holds only an ID and a concentration, so the
// ID is what tells 150 mM NaCl from 150 mM KCl.  The importer puts both
// buffers in terms of the target's component table before it compares.
TEST( DataPubHash, BuffersOfDifferentComponentsDiffer )
{
   QByteArray nacl =
      "<BufferData><buffer id=\"3\" guid=\"11111111-1111-1111-1111-111111111111\""
      " description=\"Salt buffer\" density=\"1.0\">"
      "<component id=\"12\" concentration=\"150.00000\"/>"
      "</buffer></BufferData>";
   QByteArray kcl =
      "<BufferData><buffer id=\"4\" guid=\"22222222-2222-2222-2222-222222222222\""
      " description=\"Salt buffer\" density=\"1.0\">"
      "<component id=\"17\" concentration=\"150.00000\"/>"
      "</buffer></BufferData>";
   QByteArray same =
      "<BufferData><buffer id=\"9\" guid=\"33333333-3333-3333-3333-333333333333\""
      " description=\"Salt buffer\" density=\"1.0\">"
      "<component id=\"12\" concentration=\"150.00000\"/>"
      "</buffer></BufferData>";

   EXPECT_NE( US_DataPubHash::xmlFingerprint( nacl, "buffer", "guid" ),
              US_DataPubHash::xmlFingerprint( kcl,  "buffer", "guid" ) );

   // The buffer's own ID and GUID still do not count
   EXPECT_EQ( US_DataPubHash::xmlFingerprint( nacl, "buffer", "guid" ),
              US_DataPubHash::xmlFingerprint( same, "buffer", "guid" ) );

   EXPECT_FALSE( US_DataPubHash::isVolatileOn( "component", "id" ) );
   EXPECT_TRUE ( US_DataPubHash::isVolatileOn( "buffer",    "id" ) );
}

// A value that holds a quote, or text that holds markup, cannot pass for
// other attributes or elements in the canonical form
TEST( DataPubHash, CraftedValuesCannotFoldTwoRecordsTogether )
{
   QByteArray quoted = "<r><e a=\"1&quot; b=&quot;2\"/></r>";
   QByteArray split  = "<r><e a=\"1\" b=\"2\"/></r>";

   EXPECT_NE( US_DataPubHash::canonicalXml( quoted ),
              US_DataPubHash::canonicalXml( split ) );
   EXPECT_NE( US_DataPubHash::xmlFingerprint( quoted ),
              US_DataPubHash::xmlFingerprint( split ) );

   QByteArray marked = "<r><a>x&lt;/a&gt;&lt;a&gt;y</a></r>";
   QByteArray two    = "<r><a>x</a><a>y</a></r>";

   EXPECT_NE( US_DataPubHash::canonicalXml( marked ),
              US_DataPubHash::canonicalXml( two ) );
   EXPECT_NE( US_DataPubHash::xmlFingerprint( marked ),
              US_DataPubHash::xmlFingerprint( two ) );
}

// The same buffer written with the library's old number format (fixed
// decimals, 'e' notation) and its new one (shortest round trip) is the same
// buffer; a buffer of another value still is not
TEST( DataPubHash, NumbersCompareByValueNotByDigits )
{
   QByteArray before( "<buffer guid=\"g\" description=\"PBS\" ph=\"7.00000\""
                      " density=\"1.002000\" compressibility=\"4.6000e-04\">"
                      "<component id=\"3\" concentration=\"150.00000\"/>"
                      "<extinction wavelength=\"280.0\" value=\"1.0000e-01\"/>"
                      "</buffer>" );
   QByteArray now   ( "<buffer guid=\"g\" description=\"PBS\" ph=\"7\""
                      " density=\"1.002\" compressibility=\"0.00046\">"
                      "<component id=\"3\" concentration=\"150\"/>"
                      "<extinction wavelength=\"280\" value=\"0.1\"/>"
                      "</buffer>" );
   QByteArray other ( "<buffer guid=\"g\" description=\"PBS\" ph=\"7.5\""
                      " density=\"1.002\" compressibility=\"0.00046\">"
                      "<component id=\"3\" concentration=\"150\"/>"
                      "<extinction wavelength=\"280\" value=\"0.1\"/>"
                      "</buffer>" );

   EXPECT_EQ( US_DataPubHash::xmlFingerprint( before, "buffer", "guid" ),
              US_DataPubHash::xmlFingerprint( now,    "buffer", "guid" ) );
   EXPECT_NE( US_DataPubHash::xmlFingerprint( now,    "buffer", "guid" ),
              US_DataPubHash::xmlFingerprint( other,  "buffer", "guid" ) );

   // Only a value that is a number as a whole: text with a number in it
   // keeps its digits
   QByteArray named ( "<buffer guid=\"g\" description=\"007\"/>" );
   QByteArray seven ( "<buffer guid=\"g\" description=\"7\"/>" );
   QByteArray mixed ( "<buffer guid=\"g\" description=\"7.0 mM PBS\"/>" );
   QByteArray mixed2( "<buffer guid=\"g\" description=\"7 mM PBS\"/>" );

   EXPECT_NE( US_DataPubHash::canonicalXml( mixed ),
              US_DataPubHash::canonicalXml( mixed2 ) );
   EXPECT_NE( US_DataPubHash::canonicalXml( named ),
              US_DataPubHash::canonicalXml( seven ) )
      << "a description is text, even one that reads as a number";
}

// Free text is never read as a number, whatever it looks like: a
// description, label, name, serial or comment of "007" is not one of "7"
TEST( DataPubHash, FreeTextKeepsItsDigits )
{
   QStringList textKeys;
   textKeys << "description" << "desc" << "label" << "name" << "serial"
            << "comment" << "serialNumber";

   for ( int ii = 0; ii < textKeys.size(); ii++ )
   {
      QByteArray named = QString( "<r %1=\"007\"/>" ).arg( textKeys[ ii ] )
                         .toUtf8();
      QByteArray seven = QString( "<r %1=\"7\"/>" ).arg( textKeys[ ii ] )
                         .toUtf8();
      QByteArray zero  = QString( "<r %1=\"1.50\"/>" ).arg( textKeys[ ii ] )
                         .toUtf8();
      QByteArray other = QString( "<r %1=\"1.5\"/>" ).arg( textKeys[ ii ] )
                         .toUtf8();

      EXPECT_NE( US_DataPubHash::canonicalXml( named ),
                 US_DataPubHash::canonicalXml( seven ) )
         << textKeys[ ii ].toStdString();
      EXPECT_NE( US_DataPubHash::canonicalXml( zero ),
                 US_DataPubHash::canonicalXml( other ) )
         << textKeys[ ii ].toStdString();
      EXPECT_FALSE( US_DataPubHash::isNumericKey( textKeys[ ii ] ) );
   }

   // ... while a numeric attribute still compares by value
   EXPECT_EQ( US_DataPubHash::canonicalXml( "<r ph=\"007\"/>" ),
              US_DataPubHash::canonicalXml( "<r ph=\"7\"/>" ) );
}

// Values a double cannot tell apart keep their text: huge integers that
// differ beyond a double's precision, and a value that underflows to zero
TEST( DataPubHash, NumbersADoubleCannotHoldKeepTheirText )
{
   EXPECT_NE( US_DataPubHash::canonicalXml(
                 "<r mw=\"12345678901234567890\"/>" ),
              US_DataPubHash::canonicalXml(
                 "<r mw=\"12345678901234567891\"/>" ) );
   EXPECT_NE( US_DataPubHash::canonicalXml( "<r value=\"1e-400\"/>" ),
              US_DataPubHash::canonicalXml( "<r value=\"0\"/>" ) );
   EXPECT_NE( US_DataPubHash::canonicalXml( "<r value=\"1e-400\"/>" ),
              US_DataPubHash::canonicalXml( "<r value=\"2e-400\"/>" ) );

   // Zero itself, in any form, is still zero
   EXPECT_EQ( US_DataPubHash::canonicalXml( "<r value=\"0.000\"/>" ),
              US_DataPubHash::canonicalXml( "<r value=\"-0\"/>" ) );
   EXPECT_EQ( US_DataPubHash::canonicalXml( "<r value=\"0e-400\"/>" ),
              US_DataPubHash::canonicalXml( "<r value=\"0\"/>" ) );
}
