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

TEST( DataPubDefs, EveryResolutionHasItsOwnText )
{
   EXPECT_EQ( US_DataPub::resolutionText( US_DataPub::ResolvedSubstituted ),
              QString( "substituted" ) );
   EXPECT_EQ( US_DataPub::resolutionText( US_DataPub::ResolvedOmitted ),
              QString( "not imported" ) );
   EXPECT_EQ( US_DataPub::resolutionText( US_DataPub::ResolvedFailed ),
              QString( "failed" ) );
}
