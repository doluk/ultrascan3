// test/programs/us_data_publication/test_us_datapub_manifest.cpp
#include <gtest/gtest.h>

#include "us_datapub_defs.h"
#include "us_datapub_manifest.h"

// The manifest names a record's ID, GUID and name with keys that say which
// record type they belong to, because "id" on its own is ambiguous and,
// for data read from a disk store, frequently a placeholder.
TEST( DataPubDefs, KeysAreTypeSpecific )
{
   EXPECT_EQ( US_DataPub::idKey  ( US_DataPub::Experiment ), "experimentID" );
   EXPECT_EQ( US_DataPub::guidKey( US_DataPub::Experiment ), "experimentGUID" );
   EXPECT_EQ( US_DataPub::nameKey( US_DataPub::Experiment ), "runID" );

   EXPECT_EQ( US_DataPub::idKey  ( US_DataPub::RawData ), "rawDataID" );
   EXPECT_EQ( US_DataPub::guidKey( US_DataPub::RawData ), "rawDataGUID" );
   EXPECT_EQ( US_DataPub::nameKey( US_DataPub::RawData ), "filename" );

   EXPECT_EQ( US_DataPub::idKey  ( US_DataPub::EditedData ), "editedDataID" );
   EXPECT_EQ( US_DataPub::guidKey( US_DataPub::EditedData ), "editedDataGUID" );

   EXPECT_EQ( US_DataPub::idKey  ( US_DataPub::Model ), "modelID" );
   EXPECT_EQ( US_DataPub::guidKey( US_DataPub::Model ), "modelGUID" );

   EXPECT_EQ( US_DataPub::typeOfGuidKey( "modelGUID" ), US_DataPub::Model );
   EXPECT_EQ( US_DataPub::typeOfGuidKey( "nonsense"  ),
              US_DataPub::UnknownType );
}

TEST( DataPubDefs, ScopesAreOrdered )
{
   QStringList keys = US_DataPub::scopeKeys();

   ASSERT_EQ( keys.size(), 11 );
   EXPECT_EQ( keys.first(), "project" );
   EXPECT_EQ( keys.last(),  "noise" );

   EXPECT_LT( US_DataPub::scopeOfKey( "project" ),
              US_DataPub::scopeOfKey( "experiment" ) );
   EXPECT_LT( US_DataPub::scopeOfKey( "solutions" ),
              US_DataPub::scopeOfKey( "edits" ) );
   EXPECT_LT( US_DataPub::scopeOfKey( "models" ),
              US_DataPub::scopeOfKey( "noise" ) );
   EXPECT_EQ( US_DataPub::scopeOfKey( "nonsense" ), US_DataPub::ScopeNone );
}

static US_DataPubManifest sampleManifest()
{
   US_DataPubManifest mani;
   mani.created    = "2026-01-02T03:04:05Z";
   mani.generator  = "us_data_publication test";
   mani.source     = "disk";
   mani.scope      = "models";
   mani.bundleGUID = "11111111-2222-3333-4444-555555555555";
   mani.comment    = "a comment with \"quotes\" and a\nnewline";

   US_DataPubEntity project( US_DataPub::Project );
   project.id      = "7";
   project.guid    = "aaaaaaaa-0000-0000-0000-000000000001";
   project.name    = "Demo project";
   project.payload = "project/demo.xml";
   project.sha256  = "abc123";
   mani.add( project );

   US_DataPubEntity exper( US_DataPub::Experiment );
   exper.id       = "31";
   exper.guid     = "aaaaaaaa-0000-0000-0000-000000000002";
   exper.name     = "demo_run";
   exper.filename = "demo_run.RA.xml";
   exper.payload  = "experiment/demo_run.RA.xml";
   exper.sha256   = "def456";
   exper.attrs.insert( "runType", "RA" );
   exper.setDepend( US_DataPub::Project, project.guid );
   mani.add( exper );

   US_DataPubEntity model( US_DataPub::Model );
   model.id      = "-1";
   model.guid    = "aaaaaaaa-0000-0000-0000-000000000003";
   model.name    = "demo_run.1A280.2dsa.model";
   model.payload = "models/demo.xml";
   model.sha256  = "0099";
   mani.add( model );

   // Exported, with nothing in them
   mani.declareSection( US_DataPub::RawData );
   mani.declareSection( US_DataPub::Noise );

   return mani;
}

TEST( DataPubManifest, YamlRoundTrip )
{
   US_DataPubManifest original = sampleManifest();
   QString            yaml     = original.toYaml();

   US_DataPubManifest parsed;
   QString            error;

   ASSERT_TRUE( parsed.fromYaml( yaml, error ) ) << error.toStdString();

   EXPECT_EQ( parsed.version,    original.version );
   EXPECT_EQ( parsed.created,    original.created );
   EXPECT_EQ( parsed.source,     original.source );
   EXPECT_EQ( parsed.scope,      original.scope );
   EXPECT_EQ( parsed.bundleGUID, original.bundleGUID );
   EXPECT_EQ( parsed.comment,    original.comment );
   EXPECT_EQ( parsed.total(),    original.total() );

   US_DataPubEntity exper;
   ASSERT_TRUE( parsed.entity( US_DataPub::Experiment,
                "aaaaaaaa-0000-0000-0000-000000000002", exper ) );
   EXPECT_EQ( exper.id,       "31" );
   EXPECT_EQ( exper.name,     "demo_run" );
   EXPECT_EQ( exper.filename, "demo_run.RA.xml" );
   EXPECT_EQ( exper.payload,  "experiment/demo_run.RA.xml" );
   EXPECT_EQ( exper.sha256,   "def456" );
   EXPECT_EQ( exper.attrs.value( "runType" ), "RA" );
   EXPECT_EQ( exper.depend( US_DataPub::Project ),
              "aaaaaaaa-0000-0000-0000-000000000001" );
}

TEST( DataPubManifest, YamlUsesTypeSpecificKeys )
{
   QString yaml = sampleManifest().toYaml();

   EXPECT_TRUE( yaml.contains( "experimentID: \"31\"" ) )
      << yaml.toStdString();
   EXPECT_TRUE( yaml.contains( "experimentGUID:" ) );
   EXPECT_TRUE( yaml.contains( "runID: \"demo_run\"" ) );
   EXPECT_TRUE( yaml.contains( "projectGUID:" ) );
   EXPECT_TRUE( yaml.contains( "modelGUID:" ) );

   // Sections that were exported but hold nothing are listed as empty;
   // those that were not exported at all are left out, so a reader can
   // tell the two apart.
   EXPECT_TRUE ( yaml.contains( "rawData: []" ) );
   EXPECT_TRUE ( yaml.contains( "noise: []" ) );
   EXPECT_FALSE( yaml.contains( "edits:" ) );
   EXPECT_FALSE( yaml.contains( "timeState:" ) );

   US_DataPubManifest parsed;
   QString            error;
   ASSERT_TRUE ( parsed.fromYaml( yaml, error ) ) << error.toStdString();
   EXPECT_TRUE ( parsed.isDeclared( US_DataPub::RawData ) );
   EXPECT_TRUE ( parsed.isDeclared( US_DataPub::Model ) );
   EXPECT_FALSE( parsed.isDeclared( US_DataPub::EditedData ) );
}

// Every record depends only on records of sections listed before its own:
// a solution names its buffer and analytes, a raw-data triple its
// experiment and solution, an experiment its rotor calibration.
TEST( DataPubManifest, SectionsComeAfterWhatTheyDependOn )
{
   QList< US_DataPub::EntityType > order = US_DataPubManifest::orderedTypes();

   EXPECT_LT( order.indexOf( US_DataPub::RotorCalibration ),
              order.indexOf( US_DataPub::Experiment ) );
   EXPECT_LT( order.indexOf( US_DataPub::Experiment ),
              order.indexOf( US_DataPub::Solution ) );
   EXPECT_LT( order.indexOf( US_DataPub::Buffer ),
              order.indexOf( US_DataPub::Solution ) );
   EXPECT_LT( order.indexOf( US_DataPub::Analyte ),
              order.indexOf( US_DataPub::Solution ) );
   EXPECT_LT( order.indexOf( US_DataPub::Solution ),
              order.indexOf( US_DataPub::RawData ) );
   EXPECT_LT( order.indexOf( US_DataPub::Experiment ),
              order.indexOf( US_DataPub::TimeState ) );
   EXPECT_LT( order.indexOf( US_DataPub::RawData ),
              order.indexOf( US_DataPub::EditedData ) );
   EXPECT_EQ( order.size(), 12 );
}

TEST( DataPubManifest, ValidateRejectsADependencyListedLater )
{
   US_DataPubManifest mani;

   // A model cannot depend on noise: noise comes after it
   US_DataPubEntity noise( US_DataPub::Noise );
   noise.guid    = "aaaaaaaa-0000-0000-0000-000000000010";
   noise.payload = "noise/n.xml";
   mani.add( noise );

   US_DataPubEntity model( US_DataPub::Model );
   model.guid    = "aaaaaaaa-0000-0000-0000-000000000011";
   model.payload = "models/m.xml";
   model.setDepend( US_DataPub::Noise, noise.guid );
   mani.add( model );

   QString error;
   EXPECT_FALSE( mani.validate( error ) );
   EXPECT_TRUE ( error.contains( "not listed before" ) ) << error.toStdString();
}

TEST( DataPubManifest, ValidateRejectsTheSameGuidTwice )
{
   US_DataPubManifest mani;

   US_DataPubEntity one( US_DataPub::Model );
   one.guid    = "aaaaaaaa-0000-0000-0000-000000000011";
   one.payload = "models/m.xml";
   mani.add( one );
   one.payload = "models/m2.xml";
   mani.add( one );

   QString error;
   EXPECT_FALSE( mani.validate( error ) );
   EXPECT_TRUE ( error.contains( "more than once" ) ) << error.toStdString();
}

// A solution with several analytes depends on every one of them
TEST( DataPubManifest, ARecordCanDependOnSeveralOfOneType )
{
   US_DataPubManifest mani;
   QStringList        anaGUIDs;

   for ( int ii = 0; ii < 3; ii++ )
   {
      US_DataPubEntity analyte( US_DataPub::Analyte );
      analyte.guid    = QString( "aaaaaaaa-0000-0000-0000-00000000002%1" )
                        .arg( ii );
      analyte.payload = "analytes/" + analyte.guid + ".xml";
      mani.add( analyte );
      anaGUIDs << analyte.guid;
   }

   US_DataPubEntity solution( US_DataPub::Solution );
   solution.guid    = "aaaaaaaa-0000-0000-0000-000000000030";
   solution.payload = "solutions/s.xml";

   for ( int ii = 0; ii < anaGUIDs.size(); ii++ )
      solution.addDepend( US_DataPub::Analyte, anaGUIDs[ ii ] );

   solution.addDepend( US_DataPub::Analyte, anaGUIDs[ 0 ] );   // no repeat
   mani.add( solution );

   EXPECT_EQ( solution.depends.size(), 3 );
   EXPECT_EQ( solution.depend( US_DataPub::Analyte ), anaGUIDs[ 0 ] );

   QString error;
   ASSERT_TRUE( mani.validate( error ) ) << error.toStdString();

   US_DataPubManifest parsed;
   ASSERT_TRUE( parsed.fromYaml( mani.toYaml(), error ) ) << error.toStdString();

   US_DataPubEntity back;
   ASSERT_TRUE( parsed.entity( US_DataPub::Solution, solution.guid, back ) );
   QStringList got = back.dependsOn( US_DataPub::Analyte );
   got.sort();
   anaGUIDs.sort();
   EXPECT_EQ( got, anaGUIDs );

   // ... and every one of them has to be there
   US_DataPubManifest partial;
   partial.add( solution );
   EXPECT_FALSE( partial.validate( error ) );
}

TEST( DataPubManifest, ParseRejectsASectionGivenTwice )
{
   US_DataPubManifest mani;
   QString            error;

   QString yaml = "version: 1\n"
                  "models:\n"
                  "  - modelGUID: \"aaaaaaaa-0000-0000-0000-000000000003\"\n"
                  "    payload: \"models/a.xml\"\n"
                  "models:\n"
                  "  - modelGUID: \"aaaaaaaa-0000-0000-0000-000000000004\"\n"
                  "    payload: \"models/b.xml\"\n";

   EXPECT_FALSE( mani.fromYaml( yaml, error ) );
   EXPECT_TRUE ( error.contains( "more than once" ) ) << error.toStdString();
}

TEST( DataPubManifest, ParseRejectsAHeaderKeyGivenTwice )
{
   US_DataPubManifest mani;
   QString            error;

   EXPECT_FALSE( mani.fromYaml( "version: 1\nscope: \"a\"\nscope: \"b\"\n",
                                error ) );
   EXPECT_TRUE ( error.contains( "scope" ) ) << error.toStdString();
}

TEST( DataPubManifest, ParseRejectsAKeyGivenTwiceInOneRecord )
{
   US_DataPubManifest mani;
   QString            error;

   QString yaml = "version: 1\n"
                  "models:\n"
                  "  - modelGUID: \"aaaaaaaa-0000-0000-0000-000000000003\"\n"
                  "    payload: \"models/a.xml\"\n"
                  "    payload: \"models/b.xml\"\n";

   EXPECT_FALSE( mani.fromYaml( yaml, error ) );
}

TEST( DataPubManifest, ParseRejectsAMissingOrUnreadableVersion )
{
   US_DataPubManifest mani;
   QString            error;

   EXPECT_FALSE( mani.fromYaml( "scope: \"noise\"\nmodels: []\n", error ) );
   EXPECT_TRUE ( error.contains( "version" ) ) << error.toStdString();

   EXPECT_FALSE( mani.fromYaml( "version: abc\n", error ) );
   EXPECT_TRUE ( error.contains( "abc" ) ) << error.toStdString();

   EXPECT_FALSE( mani.fromYaml( "version: 0\n", error ) );
   EXPECT_FALSE( mani.fromYaml( "version: -2\n", error ) );
   EXPECT_FALSE( mani.fromYaml( "version: 1.5\n", error ) );

   EXPECT_TRUE ( mani.fromYaml( "version: 1\n", error ) ) << error.toStdString();
   EXPECT_TRUE ( mani.fromYaml( "version: \"1\"\n", error ) );
   EXPECT_EQ   ( mani.version, 1 );
}

TEST( DataPubManifest, ParseRejectsASectionThatIsNotAList )
{
   US_DataPubManifest mani;
   QString            error;

   EXPECT_FALSE( mani.fromYaml( "version: 1\nmodels: \"x\"\n", error ) );
}

// A manifest names files that an import opens, so a path that could reach
// outside the unpacked bundle is refused before anything acts on it
TEST( DataPubManifest, ParseRejectsPathsOutsideTheBundle )
{
   QStringList bad;
   bad << "../models/a.xml" << "models/../../a.xml" << "/etc/passwd"
       << "C:/a.xml" << "c:a.xml" << "models\\a.xml" << "models//a.xml"
       << "models/./a.xml" << "./a.xml" << "models/.hidden.xml"
       << "models/a.xml/" << "~/a.xml/.." << "models/a:b.xml";

   for ( int ii = 0; ii < bad.size(); ii++ )
   {
      US_DataPubManifest mani;
      QString            error;
      QString yaml = "version: 1\n"
                     "models:\n"
                     "  - modelGUID: \"aaaaaaaa-0000-0000-0000-000000000003\"\n"
                     "    payload: \"" + QString( bad[ ii ] ).replace( "\\", "\\\\" )
                     + "\"\n";

      EXPECT_FALSE( mani.fromYaml( yaml, error ) ) << bad[ ii ].toStdString();
      EXPECT_FALSE( error.isEmpty() );
   }

   // The extra payloads of a record are held to the same rule
   US_DataPubManifest mani;
   QString            error;
   QString yaml = "version: 1\n"
                  "timeState:\n"
                  "  - timeStateGUID: \"aaaaaaaa-0000-0000-0000-000000000003\"\n"
                  "    payload: \"timeState/run/run.time_state.tmst\"\n"
                  "    definitionsPayload: \"../../run.time_state.xml\"\n";

   EXPECT_FALSE( mani.fromYaml( yaml, error ) );
   EXPECT_TRUE ( error.contains( "definitionsPayload" ) ) << error.toStdString();
}

TEST( DataPubManifest, ParseRejectsUnusableNamesAndRunIDs )
{
   US_DataPubManifest mani;
   QString            error;

   QString yaml = "version: 1\n"
                  "rawData:\n"
                  "  - rawDataGUID: \"aaaaaaaa-0000-0000-0000-000000000003\"\n"
                  "    filename: \"../x.auc\"\n"
                  "    payload: \"rawData/run/x.auc\"\n";

   EXPECT_FALSE( mani.fromYaml( yaml, error ) );
   EXPECT_TRUE ( error.contains( "filename" ) ) << error.toStdString();

   yaml = "version: 1\n"
          "experiment:\n"
          "  - experimentGUID: \"aaaaaaaa-0000-0000-0000-000000000003\"\n"
          "    runID: \"run/../../x\"\n"
          "    payload: \"experiment/x.RA.xml\"\n";

   EXPECT_FALSE( mani.fromYaml( yaml, error ) );
   EXPECT_TRUE ( error.contains( "run ID" ) ) << error.toStdString();

   yaml = "version: 1\n"
          "edits:\n"
          "  - editedDataGUID: \"aaaaaaaa-0000-0000-0000-000000000003\"\n"
          "    runID: \"a b\"\n"
          "    payload: \"edits/ab/x.xml\"\n";

   EXPECT_FALSE( mani.fromYaml( yaml, error ) );
}

TEST( DataPubManifest, ValidateAppliesThePathRulesToo )
{
   US_DataPubManifest mani  = sampleManifest();
   US_DataPubEntity   model( US_DataPub::Model );
   model.guid    = "aaaaaaaa-0000-0000-0000-000000000099";
   model.payload = "../outside.xml";
   mani.add( model );

   QString error;
   EXPECT_FALSE( mani.validate( error ) );
   EXPECT_TRUE ( error.contains( "outside.xml" ) ) << error.toStdString();
}

TEST( DataPubManifest, ExtraPayloadsAreListedWithTheirDigestKeys )
{
   US_DataPubEntity entity( US_DataPub::TimeState );
   entity.payload = "timeState/r/r.time_state.tmst";
   entity.attrs.insert( "definitionsPayload", "timeState/r/r.time_state.xml" );
   entity.attrs.insert( "runID", "r" );

   QStringList keys = entity.payloadKeys();
   EXPECT_EQ( keys, QStringList() << "payload" << "definitionsPayload" );

   EXPECT_EQ( US_DataPub::digestKeyOf( "payload" ), "sha256" );
   EXPECT_EQ( US_DataPub::digestKeyOf( "definitionsPayload" ),
              "definitionsSha256" );
   EXPECT_EQ( US_DataPub::digestKeyOf( "riProfilePayload" ),
              "riProfileSha256" );
   EXPECT_TRUE ( US_DataPub::digestKeyOf( "runID" ).isEmpty() );
}

// The manifest is UTF-8 whatever the locale
TEST( DataPubManifest, TheFileIsUtf8 )
{
   US_DataPubManifest mani = sampleManifest();
   mani.comment = QString::fromUtf8( "Lösung µ 20 °C – α" );

   QTemporaryDir dir;
   ASSERT_TRUE( dir.isValid() );
   QString path = dir.path() + "/manifest.yaml";
   QString error;

   ASSERT_TRUE( mani.write( path, error ) ) << error.toStdString();

   QFile file( path );
   ASSERT_TRUE( file.open( QIODevice::ReadOnly ) );
   QByteArray bytes = file.readAll();
   file.close();

   EXPECT_TRUE( bytes.contains( QString::fromUtf8( "Lösung µ 20 °C – α" )
                                .toUtf8() ) );

   US_DataPubManifest back;
   ASSERT_TRUE( back.read( path, error ) ) << error.toStdString();
   EXPECT_EQ( back.comment, mani.comment );
}

TEST( DataPubManifest, WritingToAnUnwritablePlaceFails )
{
   US_DataPubManifest mani = sampleManifest();
   QString            error;

   EXPECT_FALSE( mani.write( "/nonexistent-dir-for-datapub/x/manifest.yaml",
                             error ) );
   EXPECT_FALSE( error.isEmpty() );
}

TEST( DataPubDefs, SafePathsAndNames )
{
   EXPECT_TRUE ( US_DataPub::isSafeRelativePath( "models/a.xml" ) );
   EXPECT_TRUE ( US_DataPub::isSafeRelativePath( "rawData/run_1/run_1.RA.1.A.280.auc" ) );
   EXPECT_FALSE( US_DataPub::isSafeRelativePath( "" ) );
   EXPECT_FALSE( US_DataPub::isSafeRelativePath( ".." ) );
   EXPECT_FALSE( US_DataPub::isSafeRelativePath( "a/\x01.xml" ) );

   EXPECT_TRUE ( US_DataPub::isSafeFileName( "a.xml" ) );
   EXPECT_FALSE( US_DataPub::isSafeFileName( "a/b.xml" ) );
   EXPECT_FALSE( US_DataPub::isSafeFileName( ".." ) );

   EXPECT_TRUE ( US_DataPub::runIdIsUsable( "Demo_2024-Run3" ) );
   EXPECT_FALSE( US_DataPub::runIdIsUsable( "Demo 2024" ) );
   EXPECT_FALSE( US_DataPub::runIdIsUsable( "a.b" ) );
   EXPECT_FALSE( US_DataPub::runIdIsUsable( QString( 81, 'a' ) ) );
}

// A name has to be usable as it is on every platform a bundle is opened on
TEST( DataPubDefs, NamesWindowsCannotHoldAreRefused )
{
   QStringList refused;
   refused << "CON" << "con.xml" << "Nul.tmst" << "aux" << "PRN.txt"
           << "COM1" << "com9.auc" << "LPT1.xml" << "lpt9"
           << "name." << "name " << "a<b" << "a>b" << "a\"b" << "a|b"
           << "a?b" << "a*b"
           << QString( "a" ) + QChar( 0x85 ) + "b"        // C1 control
           << QString( "a" ) + QChar( 0x9f ) + "b"
           << QString( "a" ) + QChar( 0x202e ) + "lmx.exe" // bidi override
           << QString( "a" ) + QChar( 0x200b ) + "b"      // zero width space
           << QString( "a" ) + QChar( 0xfeff ) + "b"      // zero width no-break
           << QString( 128, QChar( 0x00e9 ) );            // 256 bytes in UTF-8

   for ( int ii = 0; ii < refused.size(); ii++ )
   {
      QString why;
      EXPECT_FALSE( US_DataPub::isSafeFileName( refused[ ii ], &why ) )
         << refused[ ii ].toStdString();
      EXPECT_FALSE( why.isEmpty() );
      EXPECT_FALSE( US_DataPub::isSafeRelativePath( "models/" + refused[ ii ] ) )
         << refused[ ii ].toStdString();
   }

   // Only the part before the first dot is a device name
   EXPECT_TRUE( US_DataPub::isSafeFileName( "CONSOLE.xml" ) );
   EXPECT_TRUE( US_DataPub::isSafeFileName( "run.CON.xml" ) );
   EXPECT_TRUE( US_DataPub::isSafeFileName( "COM10.xml" ) );
   EXPECT_TRUE( US_DataPub::isSafeFileName( QString( 127, QChar( 0x00e9 ) ) ) );
   EXPECT_TRUE( US_DataPub::isSafeFileName( QString( "Gr" ) + QChar( 0x00fc )
                                            + "n.xml" ) );
}

// A run ID with a trailing newline or a device name cannot name a directory
TEST( DataPubDefs, RunIdsAreCheckedWhole )
{
   EXPECT_FALSE( US_DataPub::runIdIsUsable( "run1\n" ) );
   EXPECT_FALSE( US_DataPub::runIdIsUsable( "\nrun1" ) );
   EXPECT_FALSE( US_DataPub::runIdIsUsable( "CON" ) );
   EXPECT_FALSE( US_DataPub::runIdIsUsable( "lpt3" ) );
   EXPECT_TRUE ( US_DataPub::runIdIsUsable( "run1" ) );
   EXPECT_TRUE ( US_DataPub::runIdIsUsable( "CONTROL" ) );
}

// A record without a GUID of its own is given one derived from what
// identifies it: always the same, and a valid UUID
TEST( DataPubDefs, DerivedGuidsAreStableUuids )
{
   QString one = US_DataPub::derivedGuid( "centerpiece:2:Epon" );

   EXPECT_EQ( one, US_DataPub::derivedGuid( "centerpiece:2:Epon" ) );
   EXPECT_NE( one, US_DataPub::derivedGuid( "centerpiece:3:Epon" ) );
   EXPECT_FALSE( QUuid( one ).isNull() );
   EXPECT_EQ( QUuid( one ).version(), QUuid::Sha1 );
   EXPECT_EQ( one.size(), 36 );
}

// Two payloads whose paths differ only in case are one file on Windows
// and macOS
TEST( DataPubManifest, PathsThatDifferOnlyInCaseAreRefused )
{
   US_DataPubManifest mani;

   US_DataPubEntity one( US_DataPub::Model );
   one.guid    = "aaaaaaaa-0000-0000-0000-000000000011";
   one.payload = "models/Model.xml";
   mani.add( one );

   US_DataPubEntity two( US_DataPub::Model );
   two.guid    = "aaaaaaaa-0000-0000-0000-000000000012";
   two.payload = "models/model.XML";
   mani.add( two );

   QString error;
   EXPECT_FALSE( mani.validate( error ) );
   EXPECT_TRUE ( error.contains( "ignores case" ) ) << error.toStdString();

   QString text = mani.toYaml();
   US_DataPubManifest back;
   EXPECT_FALSE( back.fromYaml( text, error ) );
}

// Only "depends" may open a nested map; any other key with no value used
// to swallow the lines under it without a word
TEST( DataPubManifest, ParseRejectsANestedMapOtherThanDepends )
{
   QString text =
      "version: 1\n"
      "models:\n"
      "  - modelGUID: \"aaaaaaaa-0000-0000-0000-000000000011\"\n"
      "    payload: \"models/m.xml\"\n"
      "    extras:\n"
      "      payload: \"../../etc/passwd\"\n";

   US_DataPubManifest mani;
   QString            error;

   EXPECT_FALSE( mani.fromYaml( text, error ) );
   EXPECT_TRUE ( error.contains( "extras" ) ) << error.toStdString();

   // A dependency needs a value too
   text =
      "version: 1\n"
      "models:\n"
      "  - modelGUID: \"aaaaaaaa-0000-0000-0000-000000000011\"\n"
      "    depends:\n"
      "      editedDataGUID:\n";

   EXPECT_FALSE( mani.fromYaml( text, error ) );
}

TEST( DataPubManifest, AManifestTooLargeIsNotRead )
{
   QTemporaryDir dir;
   ASSERT_TRUE( dir.isValid() );

   QString path = dir.path() + "/manifest.yaml";
   QFile   file( path );
   ASSERT_TRUE( file.open( QIODevice::WriteOnly ) );

   QByteArray line( 1023, '#' );
   line += '\n';
   QByteArray head( "version: 1\n" );
   file.write( head );

   for ( qint64 left = US_DataPubManifest::maxManifestBytes; left > 0;
         left -= line.size() )
      file.write( line );

   file.close();

   US_DataPubManifest mani;
   QString            error;

   EXPECT_FALSE( mani.read( path, error ) );
   EXPECT_TRUE ( error.contains( "larger" ) ) << error.toStdString();
}

// GUID lookups stay right while the manifest is built and rebuilt
TEST( DataPubManifest, LookupsFollowTheSections )
{
   US_DataPubManifest mani;

   for ( int ii = 0; ii < 2000; ii++ )
   {
      US_DataPubEntity entity( US_DataPub::Model );
      entity.guid    = US_DataPub::derivedGuid( QString::number( ii ) );
      entity.payload = "models/" + entity.guid + ".xml";
      entity.name    = QString::number( ii );
      mani.add( entity );
   }

   US_DataPubEntity found;
   ASSERT_TRUE( mani.entity( US_DataPub::Model,
                             US_DataPub::derivedGuid( "1234" ), found ) );
   EXPECT_EQ  ( found.name, "1234" );
   EXPECT_FALSE( mani.contains( US_DataPub::Noise,
                                US_DataPub::derivedGuid( "1234" ) ) );

   QString error;
   EXPECT_TRUE( mani.validate( error ) ) << error.toStdString();

   QList< US_DataPubEntity > one = mani.section( US_DataPub::Model ).mid( 5, 1 );
   mani.setSection( US_DataPub::Model, one );
   EXPECT_TRUE ( mani.contains( US_DataPub::Model,
                                US_DataPub::derivedGuid( "5" ) ) );
   EXPECT_FALSE( mani.contains( US_DataPub::Model,
                                US_DataPub::derivedGuid( "1234" ) ) );

   mani.clear();
   EXPECT_FALSE( mani.contains( US_DataPub::Model,
                                US_DataPub::derivedGuid( "5" ) ) );
}

TEST( DataPubManifest, ValidateCatchesMissingDependency )
{
   US_DataPubManifest mani;

   US_DataPubEntity model( US_DataPub::Model );
   model.guid    = "aaaaaaaa-0000-0000-0000-000000000003";
   model.name    = "orphan model";
   model.payload = "models/orphan.xml";
   model.setDepend( US_DataPub::EditedData,
                    "aaaaaaaa-0000-0000-0000-0000000000ff" );
   mani.add( model );

   QString error;
   EXPECT_FALSE( mani.validate( error ) );
   EXPECT_TRUE ( error.contains( "Edited Data" ) ) << error.toStdString();
}

TEST( DataPubManifest, ValidateAcceptsCompleteChain )
{
   US_DataPubManifest mani  = sampleManifest();
   QString            error;

   EXPECT_TRUE( mani.validate( error ) ) << error.toStdString();
}

TEST( DataPubManifest, ParseRejectsUnknownSection )
{
   US_DataPubManifest mani;
   QString            error;

   EXPECT_FALSE( mani.fromYaml( "version: 1\nnonsense:\n  - id: \"1\"\n",
                                error ) );
   EXPECT_TRUE ( error.contains( "nonsense" ) ) << error.toStdString();
}

// ------------------------------------------------------ acceptance rules

TEST( DataPubDefs, GuidsAreCheckedWhole )
{
   EXPECT_TRUE ( US_DataPub::guidIsUsable( "aaaaaaaa-0000-0000-0000-00000000000A" ) );
   EXPECT_FALSE( US_DataPub::guidIsUsable( "aaaaaaaa-0000-0000-0000-000000000001\n" ) );
   EXPECT_FALSE( US_DataPub::guidIsUsable( "{aaaaaaaa-0000-0000-0000-000000000001}" ) );
   EXPECT_FALSE( US_DataPub::guidIsUsable( "aaaaaaaa00000000000000000000000001" ) );
   EXPECT_FALSE( US_DataPub::guidIsUsable( "gaaaaaaa-0000-0000-0000-000000000001" ) );
   EXPECT_FALSE( US_DataPub::guidIsUsable( QString() ) );
}

TEST( DataPubDefs, RunTypesAndRunFileNames )
{
   EXPECT_TRUE ( US_DataPub::runTypeIsUsable( "RA" ) );
   EXPECT_TRUE ( US_DataPub::runTypeIsUsable( "WA12345" ) );
   EXPECT_FALSE( US_DataPub::runTypeIsUsable( "RA\n" ) );
   EXPECT_FALSE( US_DataPub::runTypeIsUsable( "R.A" ) );
   EXPECT_FALSE( US_DataPub::runTypeIsUsable( "ABCDEFGHI" ) );
   EXPECT_FALSE( US_DataPub::runTypeIsUsable( QString() ) );

   EXPECT_TRUE ( US_DataPub::runFileNameIsUsable( "run1.RA.1.A.280.auc", "run1" ) );
   EXPECT_FALSE( US_DataPub::runFileNameIsUsable( "run10.RA.1.A.280.auc", "run1" ) );
   EXPECT_FALSE( US_DataPub::runFileNameIsUsable( "other.RA.1.A.280.auc", "run1" ) );
   EXPECT_FALSE( US_DataPub::runFileNameIsUsable( "run1.RA/1.auc", "run1" ) );
   EXPECT_FALSE( US_DataPub::runFileNameIsUsable( "run1.x", QString() ) );

   EXPECT_EQ( US_DataPub::timeStateFileName( "run1" ),
              QString( "run1.time_state.tmst" ) );
}

// Every attribute that names a record by GUID, and only those
TEST( DataPubDefs, GuidsInsidePayloadsAreFound )
{
   EXPECT_TRUE( US_DataPub::unusableGuidInXml(
      "<solution guid=\"aaaaaaaa-0000-0000-0000-000000000001\">"
      "<buffer guid=\"\"/></solution>" ).isEmpty() );

   EXPECT_EQ( US_DataPub::unusableGuidInXml(
      "<experiment><operator guid=\"7\"/></experiment>" ), QString( "7" ) );
   EXPECT_EQ( US_DataPub::unusableGuidInXml(
      "<model editGUID=\"edit-1\"/>" ), QString( "edit-1" ) );
   EXPECT_EQ( US_DataPub::unusableGuidInXml(
      "<x><rawDataGUID value=\"raw 1\"/></x>" ), QString( "raw 1" ) );

   // A "value" is only a GUID on a ...GUID element
   EXPECT_TRUE( US_DataPub::unusableGuidInXml(
      "<x><meniscus value=\"5.9\"/></x>" ).isEmpty() );
   EXPECT_TRUE( US_DataPub::isGuidAttribute( "solution", "guid" ) );
   EXPECT_TRUE( US_DataPub::isGuidAttribute( "x", "bufferGUID" ) );
   EXPECT_TRUE( US_DataPub::isGuidAttribute( "editGUID", "value" ) );
   EXPECT_FALSE( US_DataPub::isGuidAttribute( "x", "value" ) );
   EXPECT_FALSE( US_DataPub::isGuidAttribute( "x", "guidance" ) );
}

// Every value of a payload that would be read as a NaN or an infinity, and
// none that is text
TEST( DataPubDefs, NonFiniteNumbersInsidePayloadsAreFound )
{
   QStringList bad = QStringList() << "nan" << "NaN" << "-nan" << "inf"
                                   << "+Inf" << "-inf" << "infinity"
                                   << " INFINITY " << "1e999" << "-2.5e400";

   for ( int ii = 0; ii < bad.size(); ii++ )
      EXPECT_TRUE( US_DataPub::isNonFiniteNumber( bad[ ii ] ) )
         << bad[ ii ].toStdString();

   QStringList good = QStringList() << "0" << "1.0057" << "-4.6e-10"
                                    << "1e300" << "" << "nano" << "info"
                                    << "Inf buffer" << "abc";

   for ( int ii = 0; ii < good.size(); ii++ )
      EXPECT_FALSE( US_DataPub::isNonFiniteNumber( good[ ii ] ) )
         << good[ ii ].toStdString();

   EXPECT_TRUE( US_DataPub::nonFiniteNumberInXml(
      "<buffer description=\"Inf\" ph=\"7\" density=\"1.0\">"
      "<component id=\"1\" name=\"NaN\" concentration=\"150\"/>"
      "</buffer>" ).isEmpty() );
   EXPECT_EQ( US_DataPub::nonFiniteNumberInXml(
      "<buffer ph=\"7\" density=\"nan\"/>" ),
      QString( "buffer/@density=\"nan\"" ) );
   EXPECT_EQ( US_DataPub::nonFiniteNumberInXml(
      "<analyte vbar20=\"inf\" mw=\"1\"/>" ),
      QString( "analyte/@vbar20=\"inf\"" ) );
   EXPECT_EQ( US_DataPub::nonFiniteNumberInXml(
      "<noise><d v=\"0\"/><d v=\"-inf\"/></noise>" ),
      QString( "d/@v=\"-inf\"" ) );

   // An experiment's name is text; a meniscus is not
   EXPECT_TRUE( US_DataPub::nonFiniteNumberInXml(
      "<experiment><name value=\"inf\"/></experiment>" ).isEmpty() );
   EXPECT_FALSE( US_DataPub::nonFiniteNumberInXml(
      "<x><meniscus value=\"nan\"/></x>" ).isEmpty() );
   EXPECT_TRUE( US_DataPub::isTextAttribute( "analyte", "description" ) );
   EXPECT_TRUE( US_DataPub::isTextAttribute( "x", "runID" ) );
   EXPECT_FALSE( US_DataPub::isTextAttribute( "buffer", "density" ) );
}

TEST( DataPubDefs, AFileDigestIsAllOrNothing )
{
   QTemporaryDir dir;
   ASSERT_TRUE( dir.isValid() );

   QFile file( dir.path() + "/a.bin" );
   ASSERT_TRUE( file.open( QIODevice::WriteOnly ) );
   file.write( "abc" );
   file.close();

   QString digest;
   QString why;
   ASSERT_TRUE( US_DataPub::fileSha256( file.fileName(), digest, &why ) );
   EXPECT_EQ( digest, QString( "ba7816bf8f01cfea414140de5dae2223"
                               "b00361a396177a9cb410ff61f20015ad" ) );

   EXPECT_FALSE( US_DataPub::fileSha256( dir.path() + "/none", digest, &why ) );
   EXPECT_TRUE ( digest.isEmpty() );
   EXPECT_FALSE( why.isEmpty() );

#if defined( Q_OS_LINUX )
   // Opens, but every read fails (EIO): no digest of the part that was read
   EXPECT_FALSE( US_DataPub::fileSha256( "/proc/self/mem", digest, &why ) );
   EXPECT_TRUE ( digest.isEmpty() );
#endif
}

static US_DataPubManifest acceptableManifest()
{
   US_DataPubManifest mani = sampleManifest();

   US_DataPubEntity raw( US_DataPub::RawData );
   raw.guid     = "aaaaaaaa-0000-0000-0000-000000000004";
   raw.filename = "demo_run.RA.1.A.280.auc";
   raw.name     = raw.filename;
   raw.payload  = "rawData/demo_run/demo_run.RA.1.A.280.auc";
   raw.attrs.insert( "runID", "demo_run" );
   raw.setDepend( US_DataPub::Experiment, "aaaaaaaa-0000-0000-0000-000000000002" );

   QList< US_DataPubEntity > raws;
   raws << raw;
   mani.setSection( US_DataPub::RawData, raws );

   return mani;
}

TEST( DataPubManifest, AcceptanceFollowsTheImportRules )
{
   QString error;
   EXPECT_TRUE( acceptableManifest().checkAcceptance( error ) )
      << error.toStdString();

   // A record GUID that is not one
   {
      US_DataPubManifest mani = acceptableManifest();
      QList< US_DataPubEntity > raws = mani.section( US_DataPub::RawData );
      raws[ 0 ].guid = "legacy-raw-1";
      mani.setSection( US_DataPub::RawData, raws );
      EXPECT_FALSE( mani.checkAcceptance( error ) );
      EXPECT_TRUE ( error.contains( "legacy-raw-1" ) ) << error.toStdString();
   }

   // A dependency that is not a GUID
   {
      US_DataPubManifest mani = acceptableManifest();
      QList< US_DataPubEntity > raws = mani.section( US_DataPub::RawData );
      raws[ 0 ].setDepend( US_DataPub::Solution, "sol 1" );
      mani.setSection( US_DataPub::RawData, raws );
      EXPECT_FALSE( mani.checkAcceptance( error ) );
      EXPECT_TRUE ( error.contains( "sol 1" ) ) << error.toStdString();
   }

   // A raw data file that is not named after its run
   {
      US_DataPubManifest mani = acceptableManifest();
      QList< US_DataPubEntity > raws = mani.section( US_DataPub::RawData );
      raws[ 0 ].filename = "renamed.RA.1.A.280.auc";
      raws[ 0 ].payload  = "rawData/demo_run/renamed.RA.1.A.280.auc";
      mani.setSection( US_DataPub::RawData, raws );
      EXPECT_FALSE( mani.checkAcceptance( error ) );
   }

   // ... or names another run than its experiment's
   {
      US_DataPubManifest mani = acceptableManifest();
      QList< US_DataPubEntity > raws = mani.section( US_DataPub::RawData );
      raws[ 0 ].attrs.insert( "runID", "other_run" );
      raws[ 0 ].filename = "other_run.RA.1.A.280.auc";
      raws[ 0 ].payload  = "rawData/other_run/other_run.RA.1.A.280.auc";
      mani.setSection( US_DataPub::RawData, raws );
      EXPECT_FALSE( mani.checkAcceptance( error ) );
   }

   // A run type that cannot be part of a file name
   {
      US_DataPubManifest mani = acceptableManifest();
      QList< US_DataPubEntity > exps = mani.section( US_DataPub::Experiment );
      exps[ 0 ].attrs.insert( "runType", "R A" );
      mani.setSection( US_DataPub::Experiment, exps );
      EXPECT_FALSE( mani.checkAcceptance( error ) );
      EXPECT_TRUE ( error.contains( "R A" ) ) << error.toStdString();
   }

   // A time state of another name, or without its definitions
   {
      US_DataPubManifest mani = acceptableManifest();
      US_DataPubEntity   tmst( US_DataPub::TimeState );
      tmst.guid     = "aaaaaaaa-0000-0000-0000-000000000005";
      tmst.filename = "demo_run.other.tmst";
      tmst.payload  = "timeState/demo_run/demo_run.other.tmst";
      tmst.attrs.insert( "runID", "demo_run" );
      tmst.attrs.insert( "definitionsPayload",
                         "timeState/demo_run/demo_run.other.xml" );
      mani.add( tmst );
      EXPECT_FALSE( mani.checkAcceptance( error ) );

      US_DataPubManifest bare = acceptableManifest();
      tmst.filename = "demo_run.time_state.tmst";
      tmst.payload  = "timeState/demo_run/demo_run.time_state.tmst";
      tmst.attrs.remove( "definitionsPayload" );
      bare.add( tmst );
      EXPECT_FALSE( bare.checkAcceptance( error ) );
      EXPECT_TRUE ( error.contains( "definitions" ) ) << error.toStdString();
   }
}

// Descriptions come from users: any text has to come back as it went in
TEST( DataPubManifest, AnyTextRoundTrips )
{
   QString text = QString::fromUtf8( "Z\xC3\xBCrich \"quoted\" \\ back\\slash\n"
                                     "second line\r\ttabbed" );
   text += QChar( 0x01 );
   text += QChar( 0x00 );
   text += QChar( 0x1f );
   text += QChar( 0x7f );
   text += QChar( 0x85 );
   text += QChar( 0x2028 );
   text += QString::fromUtf8( " end \xE2\x80\x93 \xCE\xB1 \\u0041" );

   US_DataPubManifest mani = sampleManifest();
   mani.comment = text;

   QList< US_DataPubEntity > projects = mani.section( US_DataPub::Project );
   projects[ 0 ].name = text;
   mani.setSection( US_DataPub::Project, projects );

   QString yaml = mani.toYaml();

   // Nothing raw that a YAML reader refuses, and nothing that ends the text
   for ( int ii = 0; ii < yaml.size(); ii++ )
   {
      ushort code = yaml.at( ii ).unicode();
      EXPECT_FALSE( ( code < 0x20  &&  code != '\n' )  ||
                    ( code >= 0x7f  &&  code <= 0x9f )  ||  code == 0x2028 )
         << "raw character " << code << " at " << ii;
   }

   QTemporaryDir dir;
   ASSERT_TRUE( dir.isValid() );
   QString path = dir.path() + "/manifest.yaml";
   QString error;
   ASSERT_TRUE( mani.write( path, error ) ) << error.toStdString();

   US_DataPubManifest back;
   ASSERT_TRUE( back.read( path, error ) ) << error.toStdString();
   EXPECT_EQ( back.comment, text );
   EXPECT_EQ( back.section( US_DataPub::Project ).value( 0 ).name, text );
   EXPECT_EQ( back.toYaml(), yaml );
}

// Outside YAML's printable set there are more than control characters:
// U+FFFE and U+FFFF, and a surrogate without its pair, which UTF-8 cannot
// hold at all (it would be written as '?').  Those, and the noncharacters,
// go escaped -- "\UXXXXXXXX" beyond the first plane -- while ordinary text
// beyond it, an emoji, stays as it is.
TEST( DataPubManifest, TextOutsideYamlsPrintableSetIsEscaped )
{
   QString text = "a";
   text += QChar( 0xfffe );
   text += QChar( 0xffff );
   text += QChar( 0xfdd0 );
   text += QChar( 0xd800 );                     // a lone high surrogate
   text += "b";
   text += QChar( 0xdc00 );                     // a lone low surrogate
   text += QChar( QChar::highSurrogate( 0x1fffe ) );  // a noncharacter of
   text += QChar( QChar::lowSurrogate ( 0x1fffe ) );  // plane 1
   text += QString::fromUtf8( "\xF0\x9F\x98\x80" );    // U+1F600, kept raw
   text += QChar( 0xd83d );                     // a high surrogate at the end

   US_DataPubManifest mani = sampleManifest();
   mani.comment = text;

   QList< US_DataPubEntity > projects = mani.section( US_DataPub::Project );
   projects[ 0 ].name = text;
   mani.setSection( US_DataPub::Project, projects );

   QString yaml = mani.toYaml();

   EXPECT_TRUE( yaml.contains( "\\ufffe\\uffff\\ufdd0\\ud800b\\udc00"
                               "\\U0001fffe" ) ) << yaml.toStdString();
   EXPECT_TRUE( yaml.contains( QString::fromUtf8( "\xF0\x9F\x98\x80" )
                               + "\\ud83d\"" ) ) << yaml.toStdString();

   QTemporaryDir dir;
   ASSERT_TRUE( dir.isValid() );
   QString path = dir.path() + "/manifest.yaml";
   QString error;
   ASSERT_TRUE( mani.write( path, error ) ) << error.toStdString();

   // What is in the file is all printable for YAML: c-printable is TAB, LF,
   // CR, 0x20-0x7E, 0x85, 0xA0-0xD7FF, 0xE000-0xFFFD and 0x10000-0x10FFFF
   QFile file( path );
   ASSERT_TRUE( file.open( QIODevice::ReadOnly ) );
   QByteArray raw = file.readAll();
   file.close();

   QString decoded = QString::fromUtf8( raw );
   ASSERT_EQ( decoded.toUtf8(), raw ) << "the file is not valid UTF-8";

   QVector< uint > codes = decoded.toUcs4();

   for ( int ii = 0; ii < codes.size(); ii++ )
   {
      uint code = codes[ ii ];
      bool printable = code == 0x09  ||  code == 0x0a  ||  code == 0x0d  ||
                       ( code >= 0x20  &&  code <= 0x7e )  ||  code == 0x85  ||
                       ( code >= 0xa0  &&  code <= 0xd7ff )  ||
                       ( code >= 0xe000  &&  code <= 0xfffd )  ||
                       ( code >= 0x10000  &&  code <= 0x10ffff );
      EXPECT_TRUE( printable ) << "code point " << code << " at " << ii;
   }

   US_DataPubManifest back;
   ASSERT_TRUE( back.read( path, error ) ) << error.toStdString();
   EXPECT_EQ( back.comment, text );
   EXPECT_EQ( back.section( US_DataPub::Project ).value( 0 ).name, text );
   EXPECT_EQ( back.toYaml(), yaml );

   // A real YAML reader, when there is one, reads the same text back
   QProcess python;
   QString  script =
      "import sys, yaml\n"
      "d = yaml.safe_load(open(sys.argv[1], encoding='utf-8'))\n"
      "sys.stdout.write(d['comment'].encode('utf-16-le',"
      " 'surrogatepass').hex())\n";
   python.start( "python3", QStringList() << "-I" << "-c" << script << path );

   if ( ! python.waitForFinished( 30000 )  ||
        python.exitStatus() != QProcess::NormalExit )
      GTEST_SKIP() << "no python3 to read the manifest with";

   if ( python.exitCode() != 0  &&
        python.readAllStandardError().contains( "No module named" ) )
      GTEST_SKIP() << "python3 has no yaml module";

   ASSERT_EQ( python.exitCode(), 0 )
      << python.readAllStandardError().toStdString();

   QByteArray utf16;                            // little-endian

   for ( int ii = 0; ii < text.size(); ii++ )
   {
      ushort unit = text.at( ii ).unicode();
      utf16.append( char( unit & 0xff ) );
      utf16.append( char( unit >> 8 ) );
   }

   EXPECT_EQ( python.readAllStandardOutput(), utf16.toHex() );
}

// A NUL in the file does not end what is read: the text after it is still
// parsed (and here refused), not silently dropped
TEST( DataPubManifest, ANulInTheFileDoesNotEndIt )
{
   QTemporaryDir dir;
   ASSERT_TRUE( dir.isValid() );
   QString path = dir.path() + "/manifest.yaml";
   QString error;

   US_DataPubManifest mani = sampleManifest();
   ASSERT_TRUE( mani.write( path, error ) ) << error.toStdString();

   QFile file( path );
   ASSERT_TRUE( file.open( QIODevice::Append ) );
   file.write( QByteArray( "\0nonsense: [\n", 13 ) );
   file.close();

   US_DataPubManifest back;
   EXPECT_FALSE( back.read( path, error ) );
}

TEST( DataPubManifest, ADependencyCanBeAddedLater )
{
   US_DataPubManifest mani = sampleManifest();
   QString            model = "aaaaaaaa-0000-0000-0000-000000000003";

   EXPECT_TRUE ( mani.addDependency( US_DataPub::Model, model,
                 US_DataPub::Project, "aaaaaaaa-0000-0000-0000-000000000001" ) );
   EXPECT_FALSE( mani.addDependency( US_DataPub::Model,
                 "aaaaaaaa-0000-0000-0000-00000000ffff",
                 US_DataPub::Project, "aaaaaaaa-0000-0000-0000-000000000001" ) );

   US_DataPubEntity found;
   ASSERT_TRUE( mani.entity( US_DataPub::Model, model, found ) );
   EXPECT_EQ( found.dependsOn( US_DataPub::Project ).size(), 1 );
}

TEST( DataPubDefs, EveryResolutionHasItsOwnText )
{
   EXPECT_EQ( US_DataPub::resolutionText( US_DataPub::ResolvedSubstituted ),
              QString( "substituted" ) );
   EXPECT_EQ( US_DataPub::resolutionText( US_DataPub::ResolvedOmitted ),
              QString( "not imported" ) );
   EXPECT_EQ( US_DataPub::resolutionText( US_DataPub::ResolvedFailed ),
              QString( "failed" ) );
}

/* Some development builds of format 1 listed noise carried for a model
   without its own under that model and named the model it was fitted to
   in fittedToModelGUID.  The payload names the other model, so the bundle
   cannot be imported; it is refused with a message that says why, not
   with one about a payload that does not match.  Format 2 never writes
   the attribute.
*/
TEST( DataPubManifest, AFormatOneBundleOfBorrowedNoiseIsRefusedPrecisely )
{
   QString borrower = "aaaaaaaa-0000-4000-8000-000000000011";
   QString fitted   = "aaaaaaaa-0000-4000-8000-000000000012";

   US_DataPubManifest mani;
   US_DataPubEntity   model( US_DataPub::Model );
   model.guid    = borrower;
   model.name    = "borrower";
   model.payload = "models/" + borrower + ".xml";
   mani.add( model );

   US_DataPubEntity noise( US_DataPub::Noise );
   noise.guid    = "aaaaaaaa-0000-4000-8000-000000000013";
   noise.name    = "borrowed";
   noise.payload = "noises/" + noise.guid + ".xml";
   noise.setDepend( US_DataPub::Model, borrower );
   noise.attrs.insert( "fittedToModelGUID", fitted );
   mani.add( noise );

   QString error;
   mani.version = 1;
   EXPECT_FALSE( mani.validate( error ) );
   EXPECT_TRUE( error.contains( "development build" ) ) << error.toStdString();
   EXPECT_TRUE( error.contains( fitted ) ) << error.toStdString();
   EXPECT_TRUE( error.contains( borrower ) ) << error.toStdString();

   // The same manifest read back from its text
   US_DataPubManifest parsed;
   ASSERT_TRUE( parsed.fromYaml( mani.toYaml(), error ) ) << error.toStdString();
   EXPECT_EQ( parsed.version, 1 );
   EXPECT_FALSE( parsed.validate( error ) );
   EXPECT_TRUE( error.contains( "development build" ) ) << error.toStdString();

   // Without the attribute a format-1 manifest is valid
   noise.attrs.remove( "fittedToModelGUID" );
   US_DataPubManifest plain;
   plain.version = 1;
   plain.add( model );
   plain.add( noise );
   EXPECT_TRUE( plain.validate( error ) ) << error.toStdString();
}
