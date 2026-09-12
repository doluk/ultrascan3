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

   // Sections with nothing in them are still listed, so that a reader can
   // tell "exported and empty" from "not exported at all" by the scope.
   EXPECT_TRUE( yaml.contains( "rawData: []" ) );
   EXPECT_TRUE( yaml.contains( "noise: []" ) );
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
