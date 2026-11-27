// test_us_datapub_mock_db.cpp - the database import against a scripted
// database (datapub_fake_db.h): its single transaction, the rules of the
// server it follows, the order of models and noise, and what a bundle's
// records keep of the source's rows.  No server is needed.
#include <gtest/gtest.h>

#include <QtCore>

#include "datapub_test_env.h"
#include "datapub_borrow_scenario.h"
#include "datapub_craft.h"
#include "datapub_noise_rules.h"

#include "us_datapub_catalog.h"
#include "us_datapub_export.h"
#include "us_datapub_import.h"
#include "us_datapub_manifest.h"
#include "us_datapub_hash.h"
#include "us_datapub_bundle.h"
#include "us_db2.h"
#include "us_crypto.h"
#include "us_settings.h"
#include "us_time_state.h"
#include "us_analyte.h"

// ------------------------------------------- importing into a mock database
//
// These run without a server: the importer is handed an IUS_DB2 that the
// fake in datapub_fake_db.h answers.  They pin down what the import does to
// a database -- one transaction, committed once at the end and rolled back
// on any failure; raw data and edits written through US_ConvertIO; nothing
// written to the local disk store; a second import of the same bundle
// writing nothing at all.

#include "datapub_fake_db.h"

class DataPubMockImport : public DataPubTestEnv
{
   protected:
      QString       bundlePath;
      int           savedInv = 0;
      DataPubFakeDb fake;

      void SetUp() override
      {
         DataPubTestEnv::SetUp();

         savedInv = US_Settings::us_inv_ID();
         US_Settings::set_us_inv_ID( 2 );

         addSpeedSteps();

         bundlePath = root + "/to_mock.tar.gz";

         US_DataPubExporter            exporter;
         US_DataPubExporter::Selection selection;
         selection.fromDb = false;
         selection.scope  = US_DataPub::ScopeNoise;
         selection.runIDs << runID;

         QString error;
         ASSERT_TRUE( exporter.exportBundle( selection, bundlePath, error ) )
            << error.toStdString();

         fake.addHardware( "DEMO-1" );
      }

      void TearDown() override
      {
         US_Settings::set_us_inv_ID( savedInv );
         DataPubTestEnv::TearDown();
      }

      // Two speed steps in the run's experiment XML
      void addSpeedSteps()
      {
         QString path = runDir() + "/" + runID + "." + runType + ".xml";
         QFile   file( path );
         ASSERT_TRUE( file.open( QIODevice::ReadOnly ) );
         QString text = QString::fromUtf8( file.readAll() );
         file.close();

         QString steps =
            "<speedstep rotorspeed=\"20000\" duration_hrs=\"1\" scans=\"10\"/>"
            "<speedstep rotorspeed=\"45000\" duration_hrs=\"5\" scans=\"40\"/>"
            "<runTemp";
         text.replace( "<runTemp", steps );

         ASSERT_TRUE( file.open( QIODevice::WriteOnly | QIODevice::Truncate ) );
         file.write( text.toUtf8() );
         file.close();
      }

      US_DataPubImporter::Options dbOptions()
      {
         US_DataPubImporter::Options options;
         options.target       = US_DataPub::TargetDb;
         options.verifyHashes = true;
         options.policy       = US_DataPub::PolicyReuse;

         return options;
      }

      bool importOnce( US_DataPubImporter& importer, QString& error,
                       const US_DataPubImporter::Options& options )
      {
         importer.setDatabase( fake.db() );

         if ( ! importer.inspect( bundlePath, error ) )  return false;

         return importer.runImport( options, error );
      }

      static int filesUnder( const QString& dir )
      {
         int          knt = 0;
         QDirIterator it( dir, QDir::Files, QDirIterator::Subdirectories );

         while ( it.hasNext() )
         {
            it.next();
            knt++;
         }

         return knt;
      }

      /* Every file under a directory, with its digest and modification
         time.  The times are first set back to a fixed moment, so that a
         file written again -- even with the same content, even within the
         same second -- shows up as changed.
      */
      static QMap< QString, QString > snapshot( const QString& dir )
      {
         static const QDateTime past( QDate( 2001, 2, 3 ), QTime( 4, 5, 6 ),
                                      Qt::UTC );
         QMap< QString, QString > files;
         QDirIterator it( dir, QDir::Files, QDirIterator::Subdirectories );

         while ( it.hasNext() )
         {
            QString path = it.next();
            QFile   file( path );

            if ( file.open( QIODevice::ReadWrite ) )
            {
               file.setFileTime( past, QFileDevice::FileModificationTime );
               file.close();
            }

            files.insert( QDir( dir ).relativeFilePath( path ),
                          state( path ) );
         }

         return files;
      }

      // The digest and modification time of every file, as they are now
      static QMap< QString, QString > current( const QString& dir )
      {
         QMap< QString, QString > files;
         QDirIterator it( dir, QDir::Files, QDirIterator::Subdirectories );

         while ( it.hasNext() )
         {
            QString path = it.next();
            files.insert( QDir( dir ).relativeFilePath( path ), state( path ) );
         }

         return files;
      }

      static QString state( const QString& path )
      {
         return US_DataPubHash::fileHash( path ) + " "
                + QString::number( QFileInfo( path ).lastModified()
                                   .toMSecsSinceEpoch() );
      }

      // The files of the local disk store that an import changed or added
      static QStringList changedFiles( const QMap< QString, QString >& before,
                                       const QMap< QString, QString >& after )
      {
         QStringList changed;
         QList< QString > keys = after.keys();

         for ( int ii = 0; ii < keys.size(); ii++ )
            if ( before.value( keys[ ii ] ) != after.value( keys[ ii ] ) )
               changed << keys[ ii ];

         keys = before.keys();

         for ( int ii = 0; ii < keys.size(); ii++ )
            if ( ! after.contains( keys[ ii ] ) )  changed << keys[ ii ];

         return changed;
      }

      US_DataPub::Resolution resolutionOf(
            const QList< US_DataPubImporter::Result >& results,
            US_DataPub::EntityType type )
      {
         for ( int ii = 0; ii < results.size(); ii++ )
            if ( results[ ii ].type == type )  return results[ ii ].resolution;

         return US_DataPub::ResolvedFailed;
      }
};

TEST_F( DataPubMockImport, TheWholeImportIsOneTransactionCommittedAtTheEnd )
{
   // Every file of the local disk store, by content and time
   QMap< QString, QString > dataBefore   = snapshot( US_Settings::dataDir() );
   QMap< QString, QString > resultBefore = snapshot( US_Settings::resultDir() );

   US_DataPubImporter importer;
   QString            error;

   ASSERT_TRUE( importOnce( importer, error, dbOptions() ) )
      << error.toStdString() << "\n"
      << importer.log().join( "\n" ).toStdString();

   EXPECT_EQ( fake.begins,    1 );
   EXPECT_EQ( fake.commits,   1 );
   EXPECT_EQ( fake.rollbacks, 0 );

   // Nothing was written after the commit
   EXPECT_EQ( fake.commitAt, fake.log.size() );

   // Every record went in: the run, its raw data through US_ConvertIO's
   // writer (which clears the run's rows first and writes the cell rows),
   // its edit, time state, model and noise
   EXPECT_EQ( fake.callsOf( "new_experiment" ).size(), 1 );
   EXPECT_EQ( fake.callsOf( "new_rawData"    ).size(), 1 );
   EXPECT_EQ( fake.callsOf( "delete_rawData" ).size(), 1 );
   EXPECT_EQ( fake.callsOf( "new_cell_experiment"     ).size(), 1 );
   EXPECT_EQ( fake.callsOf( "new_experiment_solution" ).size(), 1 );
   EXPECT_EQ( fake.callsOf( "upload_aucData"  ).size(), 1 );
   EXPECT_EQ( fake.callsOf( "new_editedData"  ).size(), 1 );
   EXPECT_EQ( fake.callsOf( "upload_editData" ).size(), 1 );
   EXPECT_EQ( fake.callsOf( "new_timestate"   ).size(), 1 );
   EXPECT_EQ( fake.callsOf( "upload_timestate" ).size(), 1 );
   EXPECT_EQ( fake.callsOf( "new_model" ).size(), 1 );
   EXPECT_EQ( fake.callsOf( "new_noise" ).size(), 1 );

   // The buffer and the analyte went in through their procedures
   EXPECT_EQ( fake.callsOf( "new_buffer"  ).size(), 1 );
   EXPECT_EQ( fake.callsOf( "new_analyte" ).size(), 1 );

   // An edit is stored as us_edit and us_import_run store one: no comment
   ASSERT_EQ( fake.callsOf( "new_editedData" ).size(), 1 );
   EXPECT_EQ( fake.callsOf( "new_editedData" )[ 0 ].value( 5 ), QString() );

   // The speed profile of the run came along
   EXPECT_EQ( fake.callsOf( "new_speedstep" ).size(), 2 );

   // The experiment and the raw data carry their own GUIDs, and the
   // solution its run
   ASSERT_EQ( fake.callsOf( "new_experiment" ).size(), 1 );
   EXPECT_EQ( fake.callsOf( "new_experiment" )[ 0 ].value( 1 ), expGUID );
   EXPECT_EQ( fake.callsOf( "new_rawData" )[ 0 ].value( 1 ), rawGUID );

   QString expID = QString::number(
                   fake.registry[ "experiment" ].value( expGUID ) );
   ASSERT_EQ( fake.callsOf( "new_solution" ).size(), 1 );
   EXPECT_EQ( fake.callsOf( "new_solution" )[ 0 ].value( 6 ), expID );

   // The local disk store was left alone: no file was added, removed or
   // written again, not even with the same content
   EXPECT_EQ( changedFiles( dataBefore, current( US_Settings::dataDir() ) ),
              QStringList() );
   EXPECT_EQ( changedFiles( resultBefore,
                            current( US_Settings::resultDir() ) ),
              QStringList() );
   EXPECT_FALSE( fake.log.contains( "get_lab_names" )
                 &&  QFile::exists( US_Settings::dataDir()
                                    + "/rotors/labs.xml" ) );

   EXPECT_TRUE( importer.outcome().contains( "written" ) )
      << importer.outcome().toStdString();
}

// A failure part way through undoes everything: nothing is committed
TEST_F( DataPubMockImport, AFailureMidImportRollsBack )
{
   fake.failOn = "upload_aucData";

   US_DataPubImporter importer;
   QString            error;

   EXPECT_FALSE( importOnce( importer, error, dbOptions() ) );

   EXPECT_EQ( fake.begins,    1 );
   EXPECT_EQ( fake.commits,   0 );
   EXPECT_EQ( fake.rollbacks, 1 );
   EXPECT_GT( fake.callsOf( "new_experiment" ).size(), 0 )
      << "the failure came after the first writes";
   EXPECT_TRUE( importer.outcome().contains( "rolled back" ) )
      << importer.outcome().toStdString();
}

TEST_F( DataPubMockImport, AFailedNoiseWriteRollsBackTheWholeRun )
{
   fake.failOn = "new_noise";

   US_DataPubImporter importer;
   QString            error;

   EXPECT_FALSE( importOnce( importer, error, dbOptions() ) );
   EXPECT_EQ   ( fake.commits,   0 );
   EXPECT_EQ   ( fake.rollbacks, 1 );
}

// A time state the database refuses is reported with the server's words,
// not only US_TimeState's status -4
TEST_F( DataPubMockImport, ARefusedTimeStateSaysWhy )
{
   fake.failOn = "new_timestate";

   US_DataPubImporter importer;
   QString            error;

   EXPECT_FALSE( importOnce( importer, error, dbOptions() ) );
   EXPECT_TRUE ( error.contains( "time state" )  &&
                 error.contains( "refused" )     &&
                 error.contains( "fake error" ) ) << error.toStdString();
   EXPECT_EQ   ( fake.commits, 0 );
}

// A lookup that fails is not taken for "not there": that would create a
// duplicate of a record the target has
TEST_F( DataPubMockImport, AFailedLookupFailsTheImport )
{
   fake.failOn = "get_bufferID";

   US_DataPubImporter importer;
   QString            error;

   EXPECT_FALSE( importOnce( importer, error, dbOptions() ) );
   EXPECT_TRUE ( error.contains( "could not be asked" ) ) << error.toStdString();
   EXPECT_EQ   ( fake.callsOf( "new_buffer" ).size(), 0 );
   EXPECT_EQ   ( fake.commits, 0 );
}

// The same bundle imported twice: the second import recognises every
// record, the experiment included, and writes nothing
TEST_F( DataPubMockImport, ASecondImportWritesNothing )
{
   QString error;

   {
      US_DataPubImporter first;
      ASSERT_TRUE( importOnce( first, error, dbOptions() ) )
         << error.toStdString();
   }

   int writes = fake.writes();

   US_DataPubImporter second;
   ASSERT_TRUE( importOnce( second, error, dbOptions() ) )
      << error.toStdString();

   EXPECT_EQ( fake.writes(), writes )
      << fake.log.mid( fake.commitAt ).join( " " ).toStdString();

   QList< US_DataPubImporter::Result > results = second.results();
   QList< US_DataPub::EntityType > types;
   types << US_DataPub::Project << US_DataPub::Buffer << US_DataPub::Analyte
         << US_DataPub::Solution << US_DataPub::Experiment
         << US_DataPub::RawData << US_DataPub::EditedData
         << US_DataPub::TimeState << US_DataPub::Model << US_DataPub::Noise;

   for ( int ii = 0; ii < types.size(); ii++ )
      EXPECT_EQ( resolutionOf( results, types[ ii ] ),
                 US_DataPub::ResolvedReused )
         << US_DataPub::typeText( types[ ii ] ).toStdString();
}

// Hardware the target lacks is stood in for, and said so -- or, when the
// import is strict about it, the import fails and nothing is kept
TEST_F( DataPubMockImport, HardwareSubstitutionIsAWarningOrAFailure )
{
   QString error;

   {
      US_DataPubImporter importer;
      ASSERT_TRUE( importOnce( importer, error, dbOptions() ) )
         << error.toStdString();

      // The calibration's rotor, the centerpiece and the operator are not
      // in the target
      EXPECT_FALSE( importer.warnings().isEmpty() );
      EXPECT_EQ( resolutionOf( importer.results(),
                               US_DataPub::RotorCalibration ),
                 US_DataPub::ResolvedOmitted );

      for ( int ii = 0; ii < importer.results().size(); ii++ )
      {
         US_DataPubImporter::Result result = importer.results()[ ii ];

         if ( result.type != US_DataPub::Centerpiece  &&
              result.type != US_DataPub::RotorCalibration )
            continue;

         // Hardware is matched or stood in for, never reported as created
         EXPECT_NE( result.resolution, US_DataPub::ResolvedCreated )
            << result.name.toStdString();
      }
   }
}

TEST_F( DataPubMockImport, AStrictImportRefusesSubstitutes )
{
   US_DataPubImporter          importer;
   US_DataPubImporter::Options options = dbOptions();
   options.strictHardware = true;
   QString                     error;

   EXPECT_FALSE( importOnce( importer, error, options ) );
   EXPECT_TRUE ( error.contains( "substitutions are not allowed" ) )
      << error.toStdString();
   EXPECT_EQ   ( fake.commits, 0 );
}

// The calibration whose rotor the target has is created under that rotor,
// and the run records the target's rotor and lab, not the source's
TEST_F( DataPubMockImport, AKnownRotorTakesTheCalibration )
{
   US_DataPubImporter probe;
   QString            error;
   ASSERT_TRUE( probe.inspect( bundlePath, error ) );

   QList< US_DataPubEntity > cals =
         probe.manifest().section( US_DataPub::RotorCalibration );
   ASSERT_EQ( cals.size(), 1 );
   fake.registry[ "rotor" ].insert( cals[ 0 ].attrs.value( "rotorGUID" ), 3 );

   // add_rotor_calibration takes an administrator
   fake.admin = true;

   US_DataPubImporter importer;
   ASSERT_TRUE( importOnce( importer, error, dbOptions() ) )
      << error.toStdString();

   // ... and -1 for "no calibration experiment": 0 is an experiment ID it
   // looks for, and refuses
   ASSERT_EQ( fake.callsOf( "add_rotor_calibration" ).size(), 1 );
   EXPECT_EQ( fake.callsOf( "add_rotor_calibration" )[ 0 ].value( 1 ), "3" );
   EXPECT_EQ( fake.callsOf( "add_rotor_calibration" )[ 0 ].value( 7 ), "-1" );

   QStringList exp = fake.callsOf( "new_experiment" ).value( 0 );
   EXPECT_EQ( exp.value( 4 ), "1" );    // the target's lab
   EXPECT_EQ( exp.value( 5 ), "5" );    // its instrument, by serial
   EXPECT_EQ( exp.value( 7 ), "3" );    // its rotor
   EXPECT_EQ( exp.value( 8 ),
              QString::number( fake.registry[ "calibration" ]
                               .value( cals[ 0 ].guid ) ) );
}

// Anyone but an administrator is refused a new calibration by the server:
// it is left out with a warning, and a strict import fails
TEST_F( DataPubMockImport, OnlyAnAdministratorAddsACalibration )
{
   US_DataPubImporter probe;
   QString            error;
   ASSERT_TRUE( probe.inspect( bundlePath, error ) );

   QList< US_DataPubEntity > cals =
         probe.manifest().section( US_DataPub::RotorCalibration );
   ASSERT_EQ( cals.size(), 1 );
   fake.registry[ "rotor" ].insert( cals[ 0 ].attrs.value( "rotorGUID" ), 3 );

   US_DataPubImporter importer;
   ASSERT_TRUE( importOnce( importer, error, dbOptions() ) )
      << error.toStdString();
   EXPECT_EQ( resolutionOf( importer.results(), US_DataPub::RotorCalibration ),
              US_DataPub::ResolvedOmitted );
   EXPECT_TRUE( importer.warnings().join( "\n" ).contains( "administrator" ) )
      << importer.warnings().join( "\n" ).toStdString();
   EXPECT_EQ( fake.callsOf( "add_rotor_calibration" ).size(), 0 );
   EXPECT_EQ( fake.commits, 1 );

   DataPubFakeDb strictDb;
   strictDb.addHardware( "DEMO-1" );
   strictDb.registry[ "rotor" ].insert( cals[ 0 ].attrs.value( "rotorGUID" ),
                                         3 );

   US_DataPubImporter strict;
   strict.setDatabase( strictDb.db() );
   US_DataPubImporter::Options options = dbOptions();
   options.strictHardware = true;
   ASSERT_TRUE ( strict.inspect( bundlePath, error ) );
   EXPECT_FALSE( strict.runImport( options, error ) );
   EXPECT_TRUE ( error.contains( "administrator" ) ) << error.toStdString();
   EXPECT_EQ   ( strictDb.commits, 0 );
}

// A chosen project has to be the current investigator's
TEST_F( DataPubMockImport, AProjectOfAnotherInvestigatorIsRefused )
{
   QString theirs = US_Util::new_guid();
   fake.registry[ "project" ].insert( theirs, 77 );
   fake.canned.insert( "get_project_desc", QList< QVariantList >()
                       << ( QVariantList() << 12 << "Mine" ) );

   US_DataPubImporter          importer;
   US_DataPubImporter::Options options = dbOptions();
   options.projectGUID = theirs;
   QString                     error;

   EXPECT_FALSE( importOnce( importer, error, options ) );
   EXPECT_TRUE ( error.contains( "does not belong" ) ) << error.toStdString();
   EXPECT_EQ   ( fake.begins, 0 );
}


// A run ID the target already uses for another run: the run is imported
// under a usable new ID, and its raw data and edit are filed under it
TEST_F( DataPubMockImport, ARunOfATakenIdIsRenamedWithItsData )
{
   fake.addExperiment( 50, US_Util::new_guid(), runID );

   US_DataPubImporter importer;
   QString            error;

   ASSERT_TRUE( importOnce( importer, error, dbOptions() ) )
      << error.toStdString();

   QString newRun = runID + "-imported";

   ASSERT_EQ( fake.callsOf( "new_experiment" ).size(), 1 );
   EXPECT_EQ( fake.callsOf( "new_experiment" )[ 0 ].value( 3 ), newRun );

   ASSERT_EQ( fake.callsOf( "new_rawData" ).size(), 1 );
   EXPECT_EQ( fake.callsOf( "new_rawData" )[ 0 ].value( 3 ),
              newRun + rawFile.mid( runID.size() ) );

   ASSERT_EQ( fake.callsOf( "new_editedData" ).size(), 1 );
   EXPECT_EQ( fake.callsOf( "new_editedData" )[ 0 ].value( 3 ), newRun );
   EXPECT_EQ( fake.callsOf( "new_editedData" )[ 0 ].value( 4 ),
              newRun + editFile.mid( runID.size() ) );

   ASSERT_EQ( fake.callsOf( "new_timestate" ).size(), 1 );
   EXPECT_TRUE( fake.callsOf( "new_timestate" )[ 0 ].value( 2 )
                .startsWith( newRun + "." ) );

   // and the run that was there is not the one the import used
   EXPECT_EQ( resolutionOf( importer.results(), US_DataPub::Experiment ),
              US_DataPub::ResolvedRenamed );
}

// The same run under the same GUID is recognised although no procedure
// looks an experiment up by its GUID
TEST_F( DataPubMockImport, TheSameRunIsRecognisedByItsGuid )
{
   fake.addExperiment( 50, expGUID, QString( "renamed_before" ) );

   US_DataPubImporter importer;
   QString            error;

   // Its raw data is not in this target, and the import does not add data
   // to a run it did not create
   EXPECT_FALSE( importOnce( importer, error, dbOptions() ) );
   EXPECT_TRUE ( error.contains( "did not create" ) ) << error.toStdString();
   EXPECT_EQ   ( fake.callsOf( "new_experiment" ).size(), 0 );
   EXPECT_EQ   ( fake.rollbacks, 1 );
}

// ------------------------------- buffer components and the RI profile, in DB

class DataPubMockExtras : public DataPubMockImport
{
   protected:
      US_BufferComponent component;

      // The fixture's buffer gets local component 1, and the bundle is
      // made again
      void exportWithComponent()
      {
         QMap< QString, US_BufferComponent > table;
         US_BufferComponent::getAllFromHD( table );
         ASSERT_TRUE( table.contains( "1" ) ) << "no local component table";
         component = table.value( "1" );

         US_Buffer buffer;
         QString   bpath = US_Settings::dataDir() + "/buffers/B0000001.xml";
         ASSERT_TRUE( buffer.readFromDisk( bpath ) );
         buffer.component     << component;
         buffer.componentIDs  << "1";
         buffer.concentration << 150.0;
         ASSERT_TRUE( buffer.writeToDisk( bpath ) );

         reexport();
      }

      void reexport()
      {
         US_DataPubExporter            exporter;
         US_DataPubExporter::Selection selection;
         selection.fromDb = false;
         selection.scope  = US_DataPub::ScopeNoise;
         selection.runIDs << runID;

         QString error;
         ASSERT_TRUE( exporter.exportBundle( selection, bundlePath, error ) )
            << error.toStdString();
      }

      // The component as the database would list it
      QVariantList componentRow( const US_BufferComponent& comp )
      {
         QStringList dens;
         QStringList visc;

         for ( int ii = 0; ii < 6; ii++ )
         {
            dens << QString::number( comp.dens_coeff[ ii ], 'f', 5 );
            visc << QString::number( comp.visc_coeff[ ii ], 'f', 5 );
         }

         return QVariantList() << comp.unit << comp.name << visc.join( " " )
                               << dens.join( " " ) << comp.range << "0";
      }
};

// A component the database has under another ID is linked by that ID
TEST_F( DataPubMockExtras, AKnownComponentIsLinkedByTheTargetsId )
{
   exportWithComponent();

   fake.canned.insert( "get_buffer_component_desc", QList< QVariantList >()
                       << ( QVariantList() << 42 ) );
   fake.canned.insert( "get_buffer_component_info", QList< QVariantList >()
                       << componentRow( component ) );

   US_DataPubImporter importer;
   QString            error;
   ASSERT_TRUE( importOnce( importer, error, dbOptions() ) )
      << error.toStdString();

   EXPECT_EQ( fake.callsOf( "create_buffer_component" ).size(), 0 );
   ASSERT_EQ( fake.callsOf( "add_buffer_component" ).size(), 1 );
   EXPECT_EQ( fake.callsOf( "add_buffer_component" )[ 0 ].value( 2 ), "42" );
}

// One the database lacks is created, and the buffer linked to the new row.
// The procedure inserts with an ID of its own (the row count plus one) and
// answers with LAST_INSERT_ID(), which that does not set: the connection's
// previous insert ID comes back -- here the project's.  The row is found
// again by what it is, never by that ID.
TEST_F( DataPubMockExtras, AnUnknownComponentIsCreated )
{
   exportWithComponent();

   fake.addComponent( 1, "Sodium chloride", "mM", "", "0 0 0 0 0 0",
                      "0 0 0 0 0 0" );
   fake.addComponent( 2, "Cadmium chloride", "mM", "", "0 0 0 0 0 0",
                      "0 0 0 0 0 0" );

   US_DataPubImporter importer;
   QString            error;
   ASSERT_TRUE( importOnce( importer, error, dbOptions() ) )
      << error.toStdString();

   ASSERT_EQ( fake.callsOf( "create_buffer_component" ).size(), 1 );
   EXPECT_EQ( fake.callsOf( "create_buffer_component" )[ 0 ].value( 2 ),
              component.name );
   ASSERT_EQ( fake.state.components.size(), 3 );
   EXPECT_EQ( fake.state.components.value( 3 ).value( 1 ).toString(),
              component.name );

   // Linked to the new row, 3 -- not to the stale insert ID the procedure
   // answered with, nor to another component
   ASSERT_EQ( fake.callsOf( "add_buffer_component" ).size(), 1 );
   EXPECT_EQ( fake.callsOf( "add_buffer_component" )[ 0 ].value( 2 ), "3" );
   EXPECT_EQ( fake.commits, 1 );
}



// An RI run's intensity profile goes into the experiment row
TEST_F( DataPubMockExtras, AnRiProfileIsStoredWithTheRun )
{
   QString xml = runDir() + "/" + runID + "." + runType + ".xml";
   QFile   file( xml );
   ASSERT_TRUE( file.open( QIODevice::ReadOnly ) );
   QString text = QString::fromUtf8( file.readAll() );
   file.close();
   text.replace( "<opticalSystem value=\"RA\"", "<opticalSystem value=\"RI\"" );
   ASSERT_TRUE( file.open( QIODevice::WriteOnly | QIODevice::Truncate ) );
   file.write( text.toUtf8() );
   file.close();

   QFile rip( runDir() + "/" + runID + ".RIProfile.xml" );
   ASSERT_TRUE( rip.open( QIODevice::WriteOnly ) );
   rip.write( "<?xml version=\"1.0\"?>\n<!DOCTYPE US_RIProfile>\n"
              "<US_RIProfile version=\"1.0\"><RI value=\"1.5\"/>"
              "<RI value=\"1.25\"/></US_RIProfile>\n" );
   rip.close();

   reexport();

   US_DataPubImporter importer;
   QString            error;
   ASSERT_TRUE( importOnce( importer, error, dbOptions() ) )
      << error.toStdString();

   ASSERT_EQ( fake.callsOf( "new_experiment" ).size(), 1 );
   QString stored = fake.callsOf( "new_experiment" )[ 0 ].value( 11 );
   EXPECT_TRUE( stored.contains( "<RI value=\"1.5\"" ) ) << stored.toStdString();
   EXPECT_TRUE( stored.contains( "<RI value=\"1.25\"" ) ) << stored.toStdString();
}

// A row of the target's component table that does not parse has nothing to
// do with this bundle's buffers: it is skipped, not taken for a failure
TEST_F( DataPubMockExtras, AnUnparseableComponentRowIsSkipped )
{
   exportWithComponent();

   QVariantList bad = componentRow( component );
   bad[ 2 ] = QString( "1.0 2.0 3.0 4.0 5.0" );    // five viscosity terms

   fake.canned.insert( "get_buffer_component_desc", QList< QVariantList >()
                       << ( QVariantList() << 41 ) << ( QVariantList() << 42 ) );
   fake.cannedFor.insert( "get_buffer_component_info:41",
                          QList< QVariantList >() << bad );
   fake.cannedFor.insert( "get_buffer_component_info:42",
                          QList< QVariantList >() << componentRow( component ) );

   US_DataPubImporter importer;
   QString            error;
   ASSERT_TRUE( importOnce( importer, error, dbOptions() ) )
      << error.toStdString();

   EXPECT_EQ( fake.callsOf( "create_buffer_component" ).size(), 0 );
   ASSERT_EQ( fake.callsOf( "add_buffer_component" ).size(), 1 );
   EXPECT_EQ( fake.callsOf( "add_buffer_component" )[ 0 ].value( 2 ), "42" );
   EXPECT_TRUE( importer.log().join( "\n" ).contains( "component 41" ) )
      << importer.log().join( "\n" ).toStdString();
}

// The extinction spectra of a buffer and an analyte go in with them.  The
// library writers store a spectrum only when told it is new, which one read
// from a file is not, so the import writes the profiles itself.
TEST_F( DataPubMockExtras, ExtinctionSpectraGoInWithTheirRecords )
{
   US_Buffer buffer;
   QString   bpath = US_Settings::dataDir() + "/buffers/B0000001.xml";
   ASSERT_TRUE( buffer.readFromDisk( bpath ) );
   buffer.extinction[ 280.0 ] = 0.25;
   buffer.extinction[ 230.0 ] = 1.5;
   ASSERT_TRUE( buffer.writeToDisk( bpath ) );

   US_Analyte analyte;
   QString    apath = US_Settings::dataDir() + "/analytes/A0000001.xml";
   ASSERT_EQ( analyte.readFromFile( apath ), IUS_DB2::OK );
   analyte.extinction[ 280.0 ] = 43824.0;
   ASSERT_EQ( analyte.write( false, apath ), IUS_DB2::OK );

   reexport();

   US_DataPubImporter importer;
   QString            error;
   ASSERT_TRUE( importOnce( importer, error, dbOptions() ) )
      << error.toStdString();

   QString bufID = QString::number( fake.registry[ "buffer"  ].value( bufferGUID ) );
   QString anaID = QString::number( fake.registry[ "analyte" ].value( analyteGUID ) );
   QList< QStringList > profiles = fake.callsOf( "new_eprofile" );

   ASSERT_EQ( profiles.size(), 2 );

   QMap< QString, QStringList > byType;

   for ( int ii = 0; ii < profiles.size(); ii++ )
      byType.insert( profiles[ ii ].value( 2 ), profiles[ ii ] );

   ASSERT_TRUE( byType.contains( "Buffer"  ) );
   ASSERT_TRUE( byType.contains( "Analyte" ) );
   EXPECT_EQ  ( byType[ "Buffer"  ].value( 1 ), bufID );
   EXPECT_EQ  ( byType[ "Analyte" ].value( 1 ), anaID );
   EXPECT_TRUE( byType[ "Buffer"  ].value( 4 ).contains( "230" ) )
      << byType[ "Buffer" ].value( 4 ).toStdString();
   EXPECT_TRUE( byType[ "Analyte" ].value( 4 ).contains( "280" ) )
      << byType[ "Analyte" ].value( 4 ).toStdString();

   // ... inside the one transaction
   EXPECT_EQ( fake.commits, 1 );
   EXPECT_EQ( fake.callsInTransaction( "new_eprofile" ), 2 );
}

// The runs are looked up before the transaction opens: by run ID first,
// and the investigator's whole experiment list only for a run that leaves
// open -- never one query per experiment inside the transaction
TEST_F( DataPubMockImport, TheRunsAreLookedUpBeforeTheTransaction )
{
   // Some runs of the investigator's own, and one of someone else's
   for ( int ii = 0; ii < 5; ii++ )
      fake.addExperiment( 60 + ii, US_Util::new_guid(),
                          QString( "other_run_%1" ).arg( ii ) );
   fake.addExperiment( 70, US_Util::new_guid(), "foreign_run", 9 );

   QStringList lookups;
   lookups << "get_experiment_desc" << "get_experiment_info"
           << "get_experiment_info_by_runID";

   US_DataPubImporter first;
   QString            error;
   ASSERT_TRUE( importOnce( first, error, dbOptions() ) )
      << error.toStdString();

   // Inside the transaction only the last check that the new run's ID is
   // still free, right before its row is written
   for ( int ii = 0; ii < lookups.size(); ii++ )
      EXPECT_EQ( fake.callsInTransaction( lookups[ ii ] ),
                 lookups[ ii ] == "get_experiment_info_by_runID" ? 1 : 0 )
         << lookups[ ii ].toStdString();

   // A new run is not in the list: it was read once, before the writes
   EXPECT_EQ( fake.callsOf( "get_experiment_desc" ).size(), 1 );
   EXPECT_EQ( fake.callsOf( "get_experiment_info" ).size(), 5 );

   // The second time the run ID finds the run: no listing at all
   int start = fake.log.size();

   US_DataPubImporter second;
   ASSERT_TRUE( importOnce( second, error, dbOptions() ) )
      << error.toStdString();

   QStringList after = fake.log.mid( start );
   EXPECT_EQ( after.count( "get_experiment_info_by_runID" ), 1 );
   EXPECT_EQ( after.count( "get_experiment_desc" ), 0 );
   EXPECT_EQ( after.count( "get_experiment_info" ), 0 );

   for ( int ii = 0; ii < lookups.size(); ii++ )
      EXPECT_EQ( fake.callsInTransaction( lookups[ ii ] ), 0 )
         << lookups[ ii ].toStdString();

   EXPECT_EQ( resolutionOf( second.results(), US_DataPub::Experiment ),
              US_DataPub::ResolvedReused );
}

// A run that another investigator holds is not imported again beside it,
// and nothing is attached to their records
TEST_F( DataPubMockImport, ARunOfAnotherInvestigatorIsRefused )
{
   fake.addExperiment( 50, expGUID, runID, 9 );
   fake.addRecord( "rawData", rawGUID, 51, 9 );

   US_DataPubImporter importer;
   QString            error;

   EXPECT_FALSE( importOnce( importer, error, dbOptions() ) );
   EXPECT_TRUE ( error.contains( "another investigator" ) )
      << error.toStdString();
   EXPECT_EQ   ( fake.begins, 0 );
   EXPECT_EQ   ( fake.callsOf( "new_experiment" ).size(), 0 );
   EXPECT_EQ   ( fake.writes(), 0 );
}

// One the import cannot see at all still never gets a second experiment of
// its GUID: the database refuses it and the whole import is rolled back
TEST_F( DataPubMockImport, AnotherInvestigatorsRunIsNeverDuplicated )
{
   fake.addExperiment( 50, expGUID, "their_name", 9 );

   US_DataPubImporter importer;
   QString            error;

   EXPECT_FALSE( importOnce( importer, error, dbOptions() ) );
   EXPECT_EQ   ( fake.rollbacks, 1 );
   EXPECT_EQ   ( fake.state.experiments.size(), 1 );
   EXPECT_EQ   ( fake.state.experiments.value( 50 ).owner, 9 );
   EXPECT_FALSE( fake.registry[ "buffer" ].contains( bufferGUID ) )
      << "the rollback left a buffer behind";
}

// A run the target already has is not added to: not even a solution
TEST_F( DataPubMockImport, ASolutionIsNotAddedToARunTheTargetHad )
{
   fake.addExperiment( 50, expGUID, runID );

   US_DataPubImporter importer;
   QString            error;

   EXPECT_FALSE( importOnce( importer, error, dbOptions() ) );
   EXPECT_TRUE ( error.contains( "does not add a solution" ) )
      << error.toStdString();
   EXPECT_EQ   ( fake.callsOf( "new_solution" ).size(), 0 );
   EXPECT_EQ   ( fake.commits, 0 );
}

// A listing that fails is a failed lookup, never an empty one
TEST_F( DataPubMockImport, AFailedHardwareListingFailsTheImport )
{
   QStringList procs;
   procs << "get_rotor_calibration_profiles" << "get_rotor_names"
         << "get_abstractCenterpiece_names" << "get_abstractCenterpiece_info"
         << "get_instrument_names";

   for ( int ii = 0; ii < procs.size(); ii++ )
   {
      DataPubFakeDb broken;
      broken.addHardware( "DEMO-1" );
      broken.failOn = procs[ ii ];

      US_DataPubImporter importer;
      importer.setDatabase( broken.db() );
      QString            error;

      ASSERT_TRUE ( importer.inspect( bundlePath, error ) );
      EXPECT_FALSE( importer.runImport( dbOptions(), error ) )
         << procs[ ii ].toStdString();
      EXPECT_TRUE ( error.contains( "could not be asked" ) )
         << procs[ ii ].toStdString() << ": " << error.toStdString();
      EXPECT_EQ   ( broken.commits, 0 ) << procs[ ii ].toStdString();
   }
}

// A failed import leaves nothing in the database, and what was uploaded in
// a successful one is the bytes of the bundle's files
TEST_F( DataPubMockImport, TheDatabaseHoldsWhatWasCommittedAndNothingElse )
{
   {
      DataPubFakeDb broken;
      broken.addHardware( "DEMO-1" );
      broken.failOn = "new_noise";

      US_DataPubImporter importer;
      importer.setDatabase( broken.db() );
      QString            error;

      ASSERT_TRUE ( importer.inspect( bundlePath, error ) );
      EXPECT_FALSE( importer.runImport( dbOptions(), error ) );
      EXPECT_GT   ( broken.callsOf( "new_buffer" ).size(), 0 );
      EXPECT_TRUE ( broken.registry.value( "buffer" ).isEmpty() );
      EXPECT_TRUE ( broken.registry.value( "rawData" ).isEmpty() );
      EXPECT_TRUE ( broken.state.experiments.isEmpty() );
      EXPECT_TRUE ( broken.state.blobs.isEmpty() );
   }

   US_DataPubImporter importer;
   QString            error;
   ASSERT_TRUE( importOnce( importer, error, dbOptions() ) )
      << error.toStdString();

   QFile raw( runDir() + "/" + rawFile );
   ASSERT_TRUE( raw.open( QIODevice::ReadOnly ) );
   QByteArray bytes = raw.readAll();
   raw.close();

   QString key = "upload_aucData:" + QString::number(
                 fake.registry[ "rawData" ].value( rawGUID ) );
   ASSERT_TRUE( fake.state.blobs.contains( key ) ) << key.toStdString();
   EXPECT_EQ  ( fake.state.blobs.value( key ), bytes );
}

// ------------------------------- whose records an import may use, in DB
//
// The GUID lookups of edits, models and noise (and, for an admin, of
// projects, solutions and raw data) find a record whoever owns it, so the
// import checks against the investigator's own listings before it reuses a
// record or attaches one to it.

#include "us_datapub_bundle.h"

class DataPubMockOwners : public DataPubMockImport
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
         }

         return true;
      }

      // The fixture's bundle with its manifest changed, packed again with a
      // matching manifest digest
      void craft( Change change )
      {
         US_DataPubImporter source;
         QString            error;
         ASSERT_TRUE( source.inspect( bundlePath, error ) ) << error.toStdString();

         US_DataPubBundle out;
         ASSERT_TRUE( out.createStaging( error ) ) << error.toStdString();

         QString stage = out.stagingPath();
         ASSERT_TRUE( copyTree( source.bundleRoot(), stage ) );

         US_DataPubManifest mani;
         QString mpath = stage + "/" + US_DataPubBundle::manifestName();
         ASSERT_TRUE( mani.read( mpath, error ) ) << error.toStdString();

         change( mani, stage );

         ASSERT_TRUE( mani.write( mpath, error ) ) << error.toStdString();

         QFile digest( stage + "/" + US_DataPubExporter::manifestDigestName() );
         ASSERT_TRUE( digest.open( QIODevice::WriteOnly | QIODevice::Truncate ) );
         digest.write( ( US_DataPubHash::fileHash( mpath ) + "  "
                         + US_DataPubBundle::manifestName() + "\n" ).toUtf8() );
         digest.close();

         bundlePath = root + "/crafted.tar.gz";
         QFile::remove( bundlePath );
         ASSERT_TRUE( out.pack( bundlePath, error ) ) << error.toStdString();
      }

      // Leave a record type out of a manifest, and every dependency on it
      static void leaveOut( US_DataPubManifest& mani,
                            US_DataPub::EntityType gone )
      {
         mani.setSection( gone, QList< US_DataPubEntity >() );

         QList< US_DataPubEntity > all = mani.all();
         QSet< int >               done;

         for ( int ii = 0; ii < all.size(); ii++ )
         {
            if ( done.contains( int( all[ ii ].type ) ) )  continue;

            done << int( all[ ii ].type );
            QList< US_DataPubEntity > list = mani.section( all[ ii ].type );

            for ( int jj = 0; jj < list.size(); jj++ )
            {
               QList< QString > keys = list[ jj ].depends.keys();

               for ( int kk = 0; kk < keys.size(); kk++ )
                  if ( US_DataPubEntity::dependType( keys[ kk ] ) == gone )
                     list[ jj ].depends.remove( keys[ kk ] );
            }

            mani.setSection( all[ ii ].type, list );
         }
      }

      // The import is refused before anything is written
      void expectRefused( DataPubFakeDb& db, const QString& what )
      {
         US_DataPubImporter importer;
         importer.setDatabase( db.db() );
         QString            error;

         ASSERT_TRUE ( importer.inspect( bundlePath, error ) )
            << error.toStdString();
         EXPECT_FALSE( importer.runImport( dbOptions(), error ) );
         EXPECT_TRUE ( error.contains( "another investigator" ) )
            << error.toStdString();
         EXPECT_TRUE ( error.contains( what ) ) << error.toStdString();
         EXPECT_EQ   ( db.begins,   0 );
         EXPECT_EQ   ( db.writes(), 0 );
      }

      // A fresh database with the fixture's hardware, logged in as an admin
      // or not, holding one record of investigator 9
      void expectRefusedFor( const QString& family, const QString& guid,
                             bool admin )
      {
         DataPubFakeDb db;
         db.addHardware( "DEMO-1" );
         db.admin = admin;
         db.addRecord( family, guid, 300, 9 );

         expectRefused( db, guid );
      }
};

// A record of the bundle that the target holds for someone else is not
// reused -- and nothing of the bundle is hung from it.  The lookups of
// edits, models and noise find anyone's record; a project's lookup refuses
// someone else's for a user and finds it for an admin.
TEST_F( DataPubMockOwners, NoRecordOfAnotherInvestigatorIsReused )
{
   for ( int admin = 0; admin < 2; admin++ )
   {
      expectRefusedFor( "model",   modelGUID,   admin );
      expectRefusedFor( "edit",    editGUID,    admin );
      expectRefusedFor( "noise",   noiseGUID,   admin );
      expectRefusedFor( "project", projectGUID, admin );
   }
}

// A noise record whose model the bundle does not carry is attached only to
// the investigator's own model of that GUID
TEST_F( DataPubMockOwners, ANoiseIsNotAttachedToAnotherInvestigatorsModel )
{
   craft( []( US_DataPubManifest& m, const QString& )
          { leaveOut( m, US_DataPub::Model ); } );

   expectRefusedFor( "model", modelGUID, false );
}

TEST_F( DataPubMockOwners, ANoiseGoesOnTheInvestigatorsOwnModel )
{
   craft( []( US_DataPubManifest& m, const QString& )
          { leaveOut( m, US_DataPub::Model ); } );

   // The investigator's own model, fitted to an edit of theirs
   QString theirEdit = US_Util::new_guid();
   fake.addRecord( "edit",  theirEdit, 250 );
   fake.addRecord( "model", modelGUID, 300, 2, "their model" );
   fake.state.modelEdits.insert( modelGUID, theirEdit );

   US_DataPubImporter importer;
   QString            error;

   ASSERT_TRUE( importOnce( importer, error, dbOptions() ) )
      << error.toStdString();

   // Written with the model's ID and the edit it was fitted to, without the
   // lookups US_Noise::write() would make inside the transaction
   ASSERT_EQ( fake.callsOf( "new_noise" ).size(), 1 );
   EXPECT_EQ( fake.callsOf( "new_noise" )[ 0 ].value( 1 ), noiseGUID );
   EXPECT_EQ( fake.callsOf( "new_noise" )[ 0 ].value( 2 ), "250" );
   EXPECT_EQ( fake.callsOf( "new_noise" )[ 0 ].value( 3 ), "300" );
   EXPECT_EQ( fake.callsInTransaction( "get_model_desc" ), 0 );
   EXPECT_EQ( fake.callsInTransaction( "get_modelID" ), 0 );
   EXPECT_EQ( fake.commits, 1 );
}

// A model whose edit the bundle does not carry is attached only to the
// investigator's own edit
TEST_F( DataPubMockOwners, AModelIsNotAttachedToAnotherInvestigatorsEdit )
{
   craft( []( US_DataPubManifest& m, const QString& )
          { leaveOut( m, US_DataPub::EditedData ); } );

   expectRefusedFor( "edit", editGUID, false );
}

// A triple's solution that the bundle does not carry has to be the
// investigator's: someone else's is refused by the lookup (for a user) or
// by the listing (for an admin)
TEST_F( DataPubMockOwners, ATripleIsNotGivenAnotherInvestigatorsSolution )
{
   craft( []( US_DataPubManifest& m, const QString& )
          { leaveOut( m, US_DataPub::Solution ); } );

   expectRefusedFor( "solution", solutionGUID, false );
   expectRefusedFor( "solution", solutionGUID, true  );
}

// An admin importing for an investigator gets the same answer about
// someone else's run as the investigator would
TEST_F( DataPubMockOwners, AnAdminIsRefusedAnotherInvestigatorsRunToo )
{
   DataPubFakeDb db;
   db.addHardware( "DEMO-1" );
   db.admin = true;
   db.addExperiment( 50, expGUID, runID, 9 );
   db.addRecord( "rawData", rawGUID, 51, 9 );

   expectRefused( db, runID );
}

// ----------------------------- what runs inside the transaction, in DB

class DataPubMockTxn : public DataPubMockExtras
{
};

// Every lookup that can be made before the writes is made before them: the
// transaction runs the writes, the library writers' own queries, and the
// last check of a new run's ID -- no listing, hardware read or GUID lookup
// the preflight could have made
TEST_F( DataPubMockTxn, TheTransactionRunsNoLookupThatCouldComeFirst )
{
   exportWithComponent();

   // A name conflict, so the fingerprint of the target's record is read
   QString other = US_Util::new_guid();
   fake.addRecord( "analyte", other, 40, 2, "Demo analyte" );

   US_DataPubImporter importer;
   QString            error;
   ASSERT_TRUE( importOnce( importer, error, dbOptions() ) )
      << error.toStdString();

   QStringList early;
   early << "get_lab_names" << "get_lab_info" << "get_instrument_names"
         << "get_instrument_info_new" << "get_rotor_names" << "get_rotor_info"
         << "get_rotor_calibration_profiles" << "get_rotor_calibration_info"
         << "get_abstractCenterpiece_names" << "get_abstractCenterpiece_info"
         << "get_project_desc" << "get_buffer_desc" << "get_analyte_desc"
         << "all_solutionIDs" << "all_rawDataIDs" << "all_editedDataIDs"
         << "get_model_desc" << "get_noise_desc" << "get_experiment_desc"
         << "get_experiment_info" << "get_rotorID_from_GUID"
         << "get_personID_from_GUID" << "get_projectID_from_GUID"
         << "get_bufferID" << "get_analyteID" << "get_analyte_info"
         << "get_modelID" << "get_noiseID" << "get_editID";

   QStringList inside = fake.transactionLog();

   for ( int ii = 0; ii < early.size(); ii++ )
      EXPECT_EQ( inside.count( early[ ii ] ), 0 )
         << early[ ii ].toStdString() << " in: "
         << inside.join( " " ).toStdString();

   // The component table is read in the transaction only around a
   // component being added (US_BufferComponent::saveToDB()), to find the
   // new row
   int added = inside.indexOf( "create_buffer_component" );
   ASSERT_GE( added, 1 );
   EXPECT_EQ( inside.count( "get_buffer_component_desc" ), 2 );
   EXPECT_EQ( inside.indexOf( "get_buffer_component_desc" ), added - 1 );
   EXPECT_EQ( inside.lastIndexOf( "get_buffer_component_desc" ), added + 1 );

   // ... and the analyte was renamed, after comparing it with the other one
   EXPECT_EQ( fake.callsOf( "new_analyte" ).value( 0 ).value( 5 ),
              QString( "Demo analyte (imported)" ) );
   EXPECT_EQ( fake.writesBeforeBegin(), 0 );
}

// The run ID is checked once more right before the run is written, inside
// the transaction: a run of that ID that appeared since the preflight
// fails the import instead of being duplicated
TEST_F( DataPubMockTxn, ARunIdTakenDuringTheImportFailsIt )
{
   fake.onBegin = [ this ]()
   {
      fake.addExperiment( 50, US_Util::new_guid(), runID );
   };

   US_DataPubImporter importer;
   QString            error;

   EXPECT_FALSE( importOnce( importer, error, dbOptions() ) );
   EXPECT_TRUE ( error.contains( "was taken" ) ) << error.toStdString();
   EXPECT_EQ   ( fake.callsOf( "new_experiment" ).size(), 0 );
   EXPECT_EQ   ( fake.rollbacks, 1 );
   EXPECT_EQ   ( fake.commits,   0 );
}

// (The two implicit-commit cases exercise the fake's implicit-commit path
// and pin the importer's report down; that report was right before.)
// A procedure that commits by itself ends the transaction: the import can
// no longer undo what was written before it, and says so instead of
// reporting a clean rollback
TEST_F( DataPubMockTxn, AnImplicitCommitIsNotReportedAsARollback )
{
   fake.implicitCommitOn = "new_buffer";
   fake.failOn           = "new_noise";

   US_DataPubImporter importer;
   QString            error;

   EXPECT_FALSE( importOnce( importer, error, dbOptions() ) );
   EXPECT_EQ   ( fake.commits, 0 );
   EXPECT_TRUE ( importer.outcome().contains( "may have been kept" ) )
      << importer.outcome().toStdString();
   EXPECT_FALSE( importer.outcome().contains( "Nothing was imported" ) )
      << importer.outcome().toStdString();

   // What the procedure committed is there
   EXPECT_TRUE ( fake.registry.value( "buffer" ).contains( bufferGUID ) );
}

// ... and an import that otherwise succeeds fails at the commit
TEST_F( DataPubMockTxn, AnImplicitCommitFailsTheCommit )
{
   fake.implicitCommitOn = "new_analyte";

   US_DataPubImporter importer;
   QString            error;

   EXPECT_FALSE( importOnce( importer, error, dbOptions() ) );
   EXPECT_TRUE ( error.contains( "commit" ) ) << error.toStdString();
   EXPECT_TRUE ( importer.outcome().contains( "may have been kept" ) )
      << importer.outcome().toStdString();
}

// A manually entered buffer is stored without its "  [M]" marker, so it is
// compared and renamed under the name it will have
TEST_F( DataPubMockTxn, AManualBufferIsRenamedUnderItsStoredName )
{
   US_Buffer buffer;
   QString   bpath = US_Settings::dataDir() + "/buffers/B0000001.xml";
   ASSERT_TRUE( buffer.readFromDisk( bpath ) );
   buffer.description = "Demo buffer  [M]";
   buffer.manual      = true;
   ASSERT_TRUE( buffer.writeToDisk( bpath ) );
   reexport();

   // The target has another buffer of that stored name
   fake.addRecord( "buffer", US_Util::new_guid(), 40, 2, "Demo buffer" );

   US_DataPubImporter importer;
   QString            error;
   ASSERT_TRUE( importOnce( importer, error, dbOptions() ) )
      << error.toStdString();

   ASSERT_EQ( fake.callsOf( "new_buffer" ).size(), 1 );
   EXPECT_EQ( fake.callsOf( "new_buffer" )[ 0 ].value( 2 ),
              QString( "Demo buffer (imported)" ) );
   EXPECT_EQ( fake.callsOf( "new_buffer" )[ 0 ].value( 7 ), QString( "1" ) );
}

// An analyte's profile holds the extinction values its bundle carries, and
// nothing the reader computes: US_Analyte's reader adds the 280 nm value of
// a protein's sequence, which must not become a stored profile point.  The
// payloads are crafted so that they hold exactly what the test says, in
// this bundle format (a spectra payload, absent without spectra) and in the
// older one (the values in the analyte XML).
class DataPubMockSpectra : public DataPubMockOwners
{
   protected:
      // The analyte with a protein sequence put in and its extinction
      // values replaced
      void craftAnalyte( const QMap< double, double >& values,
                         bool spectraPayload )
      {
         QString extinction;

         if ( ! spectraPayload )
            for ( auto it = values.constBegin(); it != values.constEnd(); ++it )
               extinction += QString( "<extinction frequency=\"%1\""
                                      " value=\"%2\"/>" )
                             .arg( it.key() ).arg( it.value() );

         craft( [ & ]( US_DataPubManifest& m, const QString& stage )
         {
            QList< US_DataPubEntity > list = m.section( US_DataPub::Analyte );
            ASSERT_EQ( list.size(), 1 );

            QString path = stage + "/" + list[ 0 ].payload;
            QFile   file( path );
            ASSERT_TRUE( file.open( QIODevice::ReadOnly ) );
            QString text = QString::fromUtf8( file.readAll() );
            file.close();

            text.remove( QRegularExpression( "<extinction [^>]*/>" ) );
            text.replace( QRegularExpression( "<sequence>\\s*</sequence>" ),
                          "<sequence>\nMKWVTFISLLLLFSSAYSRGVFRRDTHKSEIAHRFKD\n"
                          "</sequence>" + extinction );
            ASSERT_TRUE( text.contains( "MKWV" ) ) << text.toStdString();

            ASSERT_TRUE( file.open( QIODevice::WriteOnly | QIODevice::Truncate ) );
            file.write( text.toUtf8() );
            file.close();

            list[ 0 ].sha256 = US_DataPubHash::fileHash( path );
            list[ 0 ].attrs.remove( "spectraPayload" );
            list[ 0 ].attrs.remove( US_DataPub::digestKeyOf( "spectraPayload" ) );

            if ( spectraPayload  &&  ! values.isEmpty() )
            {
               QString rel = US_DataPub::payloadDir( US_DataPub::Analyte ) + "/"
                             + list[ 0 ].guid + ".crafted.spectra.xml";
               US_DataPubRecords::Spectra spectra;
               spectra.insert( "extinction", values );
               ASSERT_TRUE( US_DataPubRecords::writeSpectra(
                               spectra, stage + "/" + rel ) );
               list[ 0 ].attrs.insert( "spectraPayload", rel );
               list[ 0 ].attrs.insert(
                     US_DataPub::digestKeyOf( "spectraPayload" ),
                     US_DataPubHash::fileHash( stage + "/" + rel ) );
            }

            m.setSection( US_DataPub::Analyte, list );
         } );
      }

      QStringList analyteProfiles()
      {
         QList< QStringList > profiles = fake.callsOf( "new_eprofile" );
         QStringList          stored;

         for ( int ii = 0; ii < profiles.size(); ii++ )
            if ( profiles[ ii ].value( 2 ) == "Analyte" )
               stored << profiles[ ii ].value( 4 );

         return stored;
      }

      void expectNoProfile( bool spectraPayload )
      {
         craftAnalyte( QMap< double, double >(), spectraPayload );

         US_DataPubImporter importer;
         QString            error;
         ASSERT_TRUE( importOnce( importer, error, dbOptions() ) )
            << error.toStdString();

         EXPECT_EQ( analyteProfiles(), QStringList() );
         EXPECT_EQ( fake.callsOf( "new_analyte" ).value( 0 ).value( 6 ),
                    QString() );
      }

      void expectOnlyItsPoint( bool spectraPayload )
      {
         QMap< double, double > values;
         values.insert( 230.0, 150000.0 );
         craftAnalyte( values, spectraPayload );

         US_DataPubImporter importer;
         QString            error;
         ASSERT_TRUE( importOnce( importer, error, dbOptions() ) )
            << error.toStdString();

         QStringList stored = analyteProfiles();
         ASSERT_EQ   ( stored.size(), 1 );
         EXPECT_TRUE ( stored[ 0 ].contains( "230" ) ) << stored[ 0 ].toStdString();
         EXPECT_FALSE( stored[ 0 ].contains( "280" ) ) << stored[ 0 ].toStdString();
         EXPECT_TRUE ( fake.callsOf( "new_analyte" ).value( 0 ).value( 6 )
                       .startsWith( "230/" ) )
            << fake.callsOf( "new_analyte" ).value( 0 ).value( 6 ).toStdString();
      }
};

TEST_F( DataPubMockSpectra, AProteinWithoutASpectrumGetsNoProfile )
{
   expectNoProfile( true );
}

TEST_F( DataPubMockSpectra, AProteinWithoutASpectrumGetsNoProfileFromAnOldBundle )
{
   expectNoProfile( false );
}

TEST_F( DataPubMockSpectra, AMeasuredSpectrumGainsNoComputedPoint )
{
   expectOnlyItsPoint( true );
}

TEST_F( DataPubMockSpectra, AMeasuredSpectrumGainsNoComputedPointFromAnOldBundle )
{
   expectOnlyItsPoint( false );
}

// ------------------------------------------- spectra, export to import

// The stored profile of a component type, read back from new_eprofile
static QMap< double, double > stored_profile( const DataPubFakeDb& fake,
                                              const QString& compType )
{
   QList< QStringList >   profiles = fake.callsOf( "new_eprofile" );
   QMap< double, double > values;

   for ( int ii = 0; ii < profiles.size(); ii++ )
   {
      if ( profiles[ ii ].value( 2 ) != compType )  continue;

      QXmlStreamReader xml( profiles[ ii ].value( 4 ) );

      while ( ! xml.atEnd() )
      {
         xml.readNext();

         if ( ! xml.isStartElement()  ||
              xml.name().toString() != "extinction" )  continue;

         QXmlStreamAttributes a = xml.attributes();
         values.insert( a.value( "wavelength" ).toString().toDouble(),
                        a.value( "value"      ).toString().toDouble() );
      }
   }

   return values;
}

// The spectra of a buffer and an analyte, and a buffer's component
// concentrations, go from the source's files through a bundle into the
// database at full precision: the library's own files round them
class DataPubMockRoundTrip : public DataPubMockExtras
{
   protected:
      QMap< double, double > storedProfile( const QString& compType )
      {
         return stored_profile( fake, compType );
      }
};

TEST_F( DataPubMockRoundTrip, SpectraAndConcentrationsKeepTheirDigits )
{
   // Two buffer points 0.01 nm apart, and values with every digit in use
   QMap< double, double > bufSpectrum;
   bufSpectrum.insert( 230.01, 1.2345678901234567 );
   bufSpectrum.insert( 230.02, 0.000123456789012345 );

   QMap< double, double > anaSpectrum;
   anaSpectrum.insert( 278.123456789, 43824.123456789 );
   anaSpectrum.insert( 280.0,         45678.987654321 );

   exportWithComponent();

   US_Buffer buffer;
   QString   bpath = US_Settings::dataDir() + "/buffers/B0000001.xml";
   ASSERT_TRUE( buffer.readFromDisk( bpath ) );
   buffer.extinction    = bufSpectrum;
   buffer.concentration = QList< double >() << 0.000005;     // 5 uM in mM
   ASSERT_TRUE( buffer.writeToDisk( bpath ) );

   US_Analyte analyte;
   QString    apath = US_Settings::dataDir() + "/analytes/A0000001.xml";
   ASSERT_EQ( analyte.readFromFile( apath ), IUS_DB2::OK );
   analyte.extinction = anaSpectrum;
   ASSERT_EQ( analyte.write( false, apath ), IUS_DB2::OK );

   reexport();

   US_DataPubImporter importer;
   QString            error;
   ASSERT_TRUE( importOnce( importer, error, dbOptions() ) )
      << error.toStdString();

   QMap< double, double > storedBuf = storedProfile( "Buffer" );
   QMap< double, double > storedAna = storedProfile( "Analyte" );

   ASSERT_EQ( storedBuf.size(), 2 ) << "two points 0.01 nm apart stay two";
   EXPECT_EQ( storedBuf, bufSpectrum );
   EXPECT_EQ( storedAna, anaSpectrum );

   ASSERT_EQ( fake.callsOf( "add_buffer_component" ).size(), 1 );
   EXPECT_EQ( fake.callsOf( "add_buffer_component" )[ 0 ].value( 3 )
              .toDouble(), 0.000005 )
      << fake.callsOf( "add_buffer_component" )[ 0 ].value( 3 ).toStdString();
}

// A solution's own profile goes in with it, as US_Solution::saveToDB()
// stores one: component type "Sample"
TEST_F( DataPubMockSpectra, ASolutionsOwnProfileGoesInWithIt )
{
   QString error;

   // The disk store keeps no solution profile, so it is given to the bundle
   // the way a database export gives it: as the solution's spectra payload
   craft( [ & ]( US_DataPubManifest& m, const QString& stage )
   {
      QList< US_DataPubEntity > list = m.section( US_DataPub::Solution );
      ASSERT_FALSE( list.isEmpty() );

      QString rel = US_DataPub::payloadDir( US_DataPub::Solution ) + "/"
                    + list[ 0 ].guid + ".spectra.xml";
      US_DataPubRecords::Spectra spectra;
      spectra[ "extinction" ].insert( 260.5, 0.123456789012345678 );
      spectra[ "extinction" ].insert( 280.25, 9.87654321098765 );
      ASSERT_TRUE( US_DataPubRecords::writeSpectra( spectra,
                                                    stage + "/" + rel ) );
      list[ 0 ].attrs.insert( "spectraPayload", rel );
      list[ 0 ].attrs.insert( US_DataPub::digestKeyOf( "spectraPayload" ),
                              US_DataPubHash::fileHash( stage + "/" + rel ) );
      m.setSection( US_DataPub::Solution, list );
   } );

   US_DataPubImporter importer;
   ASSERT_TRUE( importOnce( importer, error, dbOptions() ) )
      << error.toStdString();

   QMap< double, double > want;
   want.insert( 260.5, 0.123456789012345678 );
   want.insert( 280.25, 9.87654321098765 );

   QList< QStringList > profiles = fake.callsOf( "new_eprofile" );
   QString              solID    = QString::number(
                                   fake.registry[ "solution" ].value( solutionGUID ) );
   bool                 found    = false;

   for ( int ii = 0; ii < profiles.size(); ii++ )
      if ( profiles[ ii ].value( 2 ) == "Sample" )
      {
         found = true;
         EXPECT_EQ( profiles[ ii ].value( 1 ), solID );
      }

   EXPECT_TRUE( found );
   EXPECT_EQ  ( stored_profile( fake, "Sample" ), want );
   EXPECT_EQ  ( fake.callsInTransaction( "new_eprofile" ),
                fake.callsOf( "new_eprofile" ).size() );
}

// End to end: a protein the source holds without a spectrum gets no
// profile in the target: the export carries only the spectra the analyte
// file holds, not the 280 nm value US_Analyte's disk reader computes from
// the sequence; the import side is covered by DataPubMockSpectra above.
TEST_F( DataPubMockRoundTrip, AProteinWithoutASpectrumStaysWithout )
{
   US_DataPubImporter importer;
   QString            error;
   ASSERT_TRUE( importOnce( importer, error, dbOptions() ) )
      << error.toStdString();

   EXPECT_TRUE( storedProfile( "Analyte" ).isEmpty() );
}

// A target record that was read but cannot be written out to be compared
// fails the import before anything is written; it is not taken for a
// record that differs (and the bundle's renamed past it)
TEST_F( DataPubMockTxn, ATargetRecordThatCannotBeComparedFailsTheImport )
{
   QString other = US_Util::new_guid();
   fake.addRecord( "buffer", other, 40, 2, "Demo buffer" );
   fake.cannedFor.insert( "get_buffer_info:40", QList< QVariantList >()
      << ( QVariantList() << other << "Demo buffer" << "0.0" << "7.0"
                          << "1.002" << "1.0" << "0" << 2 ) );

   US_DataPubImporter importer;
   importer.setDatabase( fake.db() );
   QString            error;
   ASSERT_TRUE( importer.inspect( bundlePath, error ) ) << error.toStdString();

   // Where the target's buffer would be written out, a directory
   QString where = QFileInfo( importer.bundleRoot() ).path() + "/work/target/"
                   + US_DataPub::typeKey( US_DataPub::Buffer ) + ".xml";
   ASSERT_TRUE( QDir().mkpath( where ) );

   EXPECT_FALSE( importer.runImport( dbOptions(), error ) );
   EXPECT_TRUE ( error.contains( "written out to be compared" ) )
      << error.toStdString();
   EXPECT_EQ   ( fake.writes(), 0 );
}

// ------------------------- what the server would refuse, refused up front
//
// The fake refuses what the real procedures refuse (datapub_fake_db.h), so
// an import that would fail half way on a server fails half way here too.
// These pin down that the import finds it before the transaction opens, or
// makes the record so the server takes it.

class DataPubMockRules : public DataPubMockSpectra
{
   protected:
      // The import is refused before anything is written
      void expectRefusedEarly( DataPubFakeDb& db, const QString& what )
      {
         {
            US_DataPubImporter importer;
            QString            error;

            importer.setDatabase( db.db() );
            ASSERT_TRUE ( importer.inspect( bundlePath, error ) )
               << error.toStdString();
            EXPECT_FALSE( importer.runImport( dbOptions(), error ) );
            EXPECT_TRUE ( error.contains( what ) ) << error.toStdString();
         }

         EXPECT_EQ( db.begins,   0 );
         EXPECT_EQ( db.writes(), 0 );
      }

      QString resultName( const QList< US_DataPubImporter::Result >& results,
                          US_DataPub::EntityType type )
      {
         for ( int ii = 0; ii < results.size(); ii++ )
            if ( results[ ii ].type == type )  return results[ ii ].targetName;

         return QString();
      }

      // The analyte's payload, with its XML text changed
      void editAnalyteXml( std::function< void( QString& ) > change,
                           bool dropSpectra )
      {
         craft( [ & ]( US_DataPubManifest& m, const QString& stage )
         {
            QList< US_DataPubEntity > list = m.section( US_DataPub::Analyte );
            ASSERT_EQ( list.size(), 1 );

            QString path = stage + "/" + list[ 0 ].payload;
            QFile   file( path );
            ASSERT_TRUE( file.open( QIODevice::ReadOnly ) );
            QString text = QString::fromUtf8( file.readAll() );
            file.close();

            change( text );

            ASSERT_TRUE( file.open( QIODevice::WriteOnly | QIODevice::Truncate ) );
            file.write( text.toUtf8() );
            file.close();

            list[ 0 ].sha256 = US_DataPubHash::fileHash( path );

            if ( dropSpectra )
            {
               list[ 0 ].attrs.remove( "spectraPayload" );
               list[ 0 ].attrs.remove(
                     US_DataPub::digestKeyOf( "spectraPayload" ) );
            }

            m.setSection( US_DataPub::Analyte, list );
         } );
      }
};

// Someone else's analyte of the bundle's GUID is not hung from the
// investigator's solution -- new_solutionAnalyte would refuse it half way
// through the import.  The import makes a copy of the bundle's analyte
// under a GUID of its own.
TEST_F( DataPubMockRules, AnotherInvestigatorsAnalyteIsCopied )
{
   fake.addRecord( "analyte", analyteGUID, 300, 9, "Demo analyte" );

   US_DataPubImporter importer;
   QString            error;
   ASSERT_TRUE( importOnce( importer, error, dbOptions() ) )
      << error.toStdString() << "\n"
      << importer.log().join( "\n" ).toStdString();

   ASSERT_EQ( fake.callsOf( "new_analyte" ).size(), 1 );
   QString copy = fake.callsOf( "new_analyte" )[ 0 ].value( 1 );
   EXPECT_NE( copy, analyteGUID );
   EXPECT_TRUE( US_DataPubImporter::guidIsUsable( copy ) );

   ASSERT_EQ( fake.callsOf( "new_solutionAnalyte" ).size(), 1 );
   EXPECT_EQ( fake.callsOf( "new_solutionAnalyte" )[ 0 ].value( 3 ), copy );
   EXPECT_EQ( resolutionOf( importer.results(), US_DataPub::Analyte ),
              US_DataPub::ResolvedCreated );
   EXPECT_EQ( fake.commits, 1 );
}

// An administrator may use any analyte (verify_analyte_permission): the
// target's is reused as it is
TEST_F( DataPubMockRules, AnAdministratorReusesAnyAnalyte )
{
   fake.admin = true;
   fake.addRecord( "analyte", analyteGUID, 300, 9, "Demo analyte" );

   US_DataPubImporter importer;
   QString            error;
   ASSERT_TRUE( importOnce( importer, error, dbOptions() ) )
      << error.toStdString();

   EXPECT_EQ( fake.callsOf( "new_analyte" ).size(), 0 );
   ASSERT_EQ( fake.callsOf( "new_solutionAnalyte" ).size(), 1 );
   EXPECT_EQ( fake.callsOf( "new_solutionAnalyte" )[ 0 ].value( 3 ),
              analyteGUID );
   EXPECT_EQ( resolutionOf( importer.results(), US_DataPub::Analyte ),
              US_DataPub::ResolvedReused );
}

// Another investigator's analyte that the bundle does not carry cannot be
// copied: the import is refused before it starts
TEST_F( DataPubMockRules, AnotherInvestigatorsAnalyteTheBundleLacksIsRefused )
{
   craft( []( US_DataPubManifest& m, const QString& )
          { leaveOut( m, US_DataPub::Analyte ); } );

   fake.addRecord( "analyte", analyteGUID, 300, 9, "Demo analyte" );

   expectRefusedEarly( fake, "another investigator's analyte" );
}

// Another investigator's private buffer is not linked to the solution
// (no procedure would stop that): the import makes a copy of its own.  A
// public one is shared, and reused.
TEST_F( DataPubMockRules, AnotherInvestigatorsPrivateBufferIsCopied )
{
   fake.addRecord( "buffer", bufferGUID, 300, 9, "Demo buffer" );

   US_DataPubImporter importer;
   QString            error;
   ASSERT_TRUE( importOnce( importer, error, dbOptions() ) )
      << error.toStdString();

   ASSERT_EQ( fake.callsOf( "new_buffer" ).size(), 1 );
   QString copy = fake.callsOf( "new_buffer" )[ 0 ].value( 1 );
   EXPECT_NE( copy, bufferGUID );
   ASSERT_EQ( fake.callsOf( "new_solutionBuffer" ).size(), 1 );
   EXPECT_EQ( fake.callsOf( "new_solutionBuffer" )[ 0 ].value( 3 ), copy );

   DataPubFakeDb shared;
   shared.addHardware( "DEMO-1" );
   shared.addRecord( "buffer", bufferGUID, 300, 9, "Demo buffer" );
   shared.state.publicBuffers << bufferGUID;

   US_DataPubImporter second;
   second.setDatabase( shared.db() );
   ASSERT_TRUE( second.inspect( bundlePath, error ) );
   ASSERT_TRUE( second.runImport( dbOptions(), error ) ) << error.toStdString();
   EXPECT_EQ  ( shared.callsOf( "new_buffer" ).size(), 0 );
   ASSERT_EQ  ( shared.callsOf( "new_solutionBuffer" ).size(), 1 );
   EXPECT_EQ  ( shared.callsOf( "new_solutionBuffer" )[ 0 ].value( 3 ),
                bufferGUID );
}


// A noise record whose edit the target cannot give is refused, and the
// import rolled back, rather than written with edit 0
TEST_F( DataPubMockRules, ANoiseWithoutAnEditIsRefused )
{
   craft( []( US_DataPubManifest& m, const QString& )
          { leaveOut( m, US_DataPub::Model ); } );

   // The investigator's own model, fitted to no edit the target has
   fake.addRecord( "model", modelGUID, 300, 2, "their model" );

   US_DataPubImporter importer;
   QString            error;
   EXPECT_FALSE( importOnce( importer, error, dbOptions() ) );
   EXPECT_TRUE ( error.contains( "edit" ) ) << error.toStdString();
   EXPECT_EQ   ( fake.callsOf( "new_noise" ).size(), 0 );
   EXPECT_EQ   ( fake.commits, 0 );
}

// A project the import makes from what a run names is reported with the
// records
TEST_F( DataPubMockRules, AProjectMadeFromARunIsReported )
{
   craft( []( US_DataPubManifest& m, const QString& )
          { leaveOut( m, US_DataPub::Project ); } );

   {
      US_DataPubImporter importer;
      QString            error;
      ASSERT_TRUE( importOnce( importer, error, dbOptions() ) )
         << error.toStdString();

      QList< US_DataPubImporter::Result > results = importer.results();
      int                                 found   = 0;

      for ( int ii = 0; ii < results.size(); ii++ )
      {
         if ( results[ ii ].type != US_DataPub::Project )  continue;

         found++;
         EXPECT_EQ( results[ ii ].guid, projectGUID );
         EXPECT_EQ( results[ ii ].resolution, US_DataPub::ResolvedCreated );
         EXPECT_TRUE( results[ ii ].detail.contains( runID ) )
            << results[ ii ].detail.toStdString();
      }

      EXPECT_EQ( found, 1 );
   }

   EXPECT_EQ( fake.callsOf( "new_project2" ).size(), 1 );
}

// The raw data, edit and time state of a renamed run are reported under
// the names they have in the target
TEST_F( DataPubMockRules, ARenamedRunsFilesAreReportedUnderTheirNewNames )
{
   fake.addExperiment( 50, US_Util::new_guid(), runID );

   US_DataPubImporter importer;
   QString            error;
   ASSERT_TRUE( importOnce( importer, error, dbOptions() ) )
      << error.toStdString();

   QString newRun = runID + "-imported";
   QList< US_DataPubImporter::Result > results = importer.results();

   EXPECT_EQ( resultName( results, US_DataPub::RawData ),
              newRun + rawFile.mid( runID.size() ) );
   EXPECT_EQ( resultName( results, US_DataPub::EditedData ),
              newRun + editFile.mid( runID.size() ) );
   EXPECT_TRUE( resultName( results, US_DataPub::TimeState )
                .startsWith( newRun + "." ) )
      << resultName( results, US_DataPub::TimeState ).toStdString();

   QString log = importer.log().join( "\n" );
   EXPECT_FALSE( log.contains( "\"" + rawFile + "\"" ) ) << log.toStdString();
}

// The investigator's own raw data under another of their runs is not said
// to be someone else's
TEST_F( DataPubMockRules, OwnRawDataUnderAnotherRunIsNotCalledForeign )
{
   fake.addRecord( "rawData", rawGUID, 51, 2 );

   US_DataPubImporter importer;
   QString            error;
   EXPECT_FALSE( importOnce( importer, error, dbOptions() ) );
   EXPECT_TRUE ( error.contains( "same investigator" ) ) << error.toStdString();
   EXPECT_FALSE( error.contains( "another investigator" ) )
      << error.toStdString();
   EXPECT_EQ   ( fake.begins, 0 );
}

// An old bundle's extinction value that is not a finite number refuses
// the bundle in the preflight
TEST_F( DataPubMockRules, AnOldBundlesSpectrumHasToBeFinite )
{
   editAnalyteXml( []( QString& text )
   {
      text.remove( QRegularExpression( "<extinction [^>]*/>" ) );
      text.replace( "</sequence>", "</sequence><extinction frequency=\"230\""
                                   " value=\"nan\"/>" );
   }, true );

   expectRefusedEarly( fake, "finite" );
}

// An old bundle's protein carries the 280 nm value US_Analyte computed
// from its sequence; a lone point at 280 nm is that, and not stored as if
// it were measured
TEST_F( DataPubMockRules, AnOldBundlesComputedPointIsNotStored )
{
   editAnalyteXml( []( QString& text )
   {
      text.remove( QRegularExpression( "<extinction [^>]*/>" ) );
      text.replace( QRegularExpression( "<sequence>\\s*</sequence>" ),
                    "<sequence>\nMKWVTFISLLLLFSSAYS\n</sequence>"
                    "<extinction frequency=\"280\" value=\"5960\"/>" );
   }, true );

   US_DataPubImporter importer;
   QString            error;
   ASSERT_TRUE( importOnce( importer, error, dbOptions() ) )
      << error.toStdString();

   EXPECT_EQ( analyteProfiles(), QStringList() );
   EXPECT_EQ( fake.callsOf( "new_analyte" ).value( 0 ).value( 6 ), QString() );
}

// A sequence goes in exactly as the bundle holds it -- a FASTA header,
// residue numbers and line breaks are the analyte's own -- as a disk
// import keeps it; and the target's copy is the same analyte to a second
// import of the bundle
TEST_F( DataPubMockRules, ASequenceGoesInAsTheBundleHoldsIt )
{
   QString fasta =
      ">sp|P02768|ALBU_HUMAN Serum albumin OS=Homo sapiens\n"
      "1 MKWVTFISLL FLFSSAYSRG VFRRDAHKSE VAHRFKDLGE ENFKALVLIA FAQYLQQCPF\n"
      "61 EDHVKLVNEV TEFAKTCVAD ESAENCDKSL HTLFGDKLCT VATLRETYGE MADCCAKQEP\n";
   QString escaped = fasta;
   escaped.replace( ">", "&gt;" );

   editAnalyteXml( [ & ]( QString& text )
   {
      text.replace( QRegularExpression( "<sequence>\\s*</sequence>" ),
                    "<sequence>" + escaped + "</sequence>" );
   }, false );

   US_DataPubImporter importer;
   QString            error;
   ASSERT_TRUE( importOnce( importer, error, dbOptions() ) )
      << error.toStdString();
   ASSERT_EQ( fake.callsOf( "new_analyte" ).size(), 1 );
   EXPECT_EQ( fake.callsOf( "new_analyte" )[ 0 ].value( 3 ), fasta );

   // A second import finds the stored row, sequence and all, the same
   US_DataPubImporter probe;
   ASSERT_TRUE( probe.inspect( bundlePath, error ) );
   US_Analyte source;
   ASSERT_EQ( source.readFromFile( probe.bundleRoot() + "/"
                 + probe.manifest().section( US_DataPub::Analyte )
                   .value( 0 ).payload ), IUS_DB2::OK );

   DataPubFakeDb other;
   other.addHardware( "DEMO-1" );
   QString copy = US_Util::new_guid();
   other.addRecord( "analyte", copy, 40, 2, "Demo analyte" );
   other.cannedFor.insert( "get_analyte_info:40", QList< QVariantList >()
      << ( QVariantList() << copy << "Protein" << fasta
                          << QString::number( source.vbar20, 'g', 17 )
                          << "Demo analyte" << ""
                          << QString::number( source.mw, 'g', 17 ) << "0" ) );

   US_DataPubImporter second;
   second.setDatabase( other.db() );
   ASSERT_TRUE( second.inspect( bundlePath, error ) );
   ASSERT_TRUE( second.runImport( dbOptions(), error ) ) << error.toStdString();
   EXPECT_EQ( other.callsOf( "new_analyte" ).size(), 0 );
   EXPECT_EQ( resolutionOf( second.results(), US_DataPub::Analyte ),
              US_DataPub::ResolvedReused );
}


// A number that is not finite is refused by the preflight of a database
// import too
TEST_F( DataPubMockRules, ANumberThatIsNotFiniteIsRefusedUpFront )
{
   editAnalyteXml( []( QString& text )
   {
      text.replace( QRegularExpression( "vbar20=\"[^\"]*\"" ),
                    "vbar20=\"inf\"" );
   }, false );

   expectRefusedEarly( fake, "vbar20" );
}

// A protein the bundle carries from a disk store holds the 280 nm value
// US_Analyte computed; the same analyte in the database holds no such
// point.  They are the same analyte, and the target's is reused.
TEST_F( DataPubMockRules, ADiskProteinMatchesItsDatabaseCopy )
{
   QString seq = "MKWVTFISLLLLFSSAYSRGVFRRDTHKSEIAHRFKD";

   editAnalyteXml( [ & ]( QString& text )
   {
      text.remove( QRegularExpression( "<extinction [^>]*/>" ) );
      text.replace( QRegularExpression( "<sequence>\\s*</sequence>" ),
                    "<sequence>\n" + seq + "\n</sequence>"
                    "<extinction frequency=\"280\" value=\"5960\"/>" );
   }, false );

   US_DataPubImporter probe;
   QString            error;
   ASSERT_TRUE( probe.inspect( bundlePath, error ) );
   QString     analytePath = probe.bundleRoot() + "/"
                             + probe.manifest().section( US_DataPub::Analyte )
                               .value( 0 ).payload;
   US_Analyte  source;
   ASSERT_EQ( source.readFromFile( analytePath ), IUS_DB2::OK );

   // The same analyte in the database, under another GUID
   QString other = US_Util::new_guid();
   fake.addRecord( "analyte", other, 40, 2, "Demo analyte" );
   fake.cannedFor.insert( "get_analyte_info:40", QList< QVariantList >()
      << ( QVariantList() << other << "Protein" << seq
                          << QString::number( source.vbar20, 'g', 17 )
                          << "Demo analyte" << ""
                          << QString::number( source.mw, 'g', 17 ) << "0" ) );

   US_DataPubImporter importer;
   ASSERT_TRUE( importOnce( importer, error, dbOptions() ) )
      << error.toStdString();

   EXPECT_EQ( fake.callsOf( "new_analyte" ).size(), 0 );
   EXPECT_EQ( resolutionOf( importer.results(), US_DataPub::Analyte ),
              US_DataPub::ResolvedReused );
}

// A target record of the name that cannot be read is a failure, not a
// different record: a renamed duplicate would be made of it
TEST_F( DataPubMockRules, ATargetRecordThatCannotBeReadFailsTheImport )
{
   QString other = US_Util::new_guid();
   fake.addRecord( "buffer", other, 40, 2, "Demo buffer" );
   fake.failOn = "get_buffer_components";

   US_DataPubImporter importer;
   QString            error;
   EXPECT_FALSE( importOnce( importer, error, dbOptions() ) );
   EXPECT_TRUE ( error.contains( "could not be read" ) ) << error.toStdString();
   EXPECT_EQ   ( fake.callsOf( "new_buffer" ).size(), 0 );
   EXPECT_EQ   ( fake.writes(), 0 );
}

// A model's variance and meniscus are columns of its row: a bundle that
// carries them beside an XML without them has them stored -- with every
// digit the manifest gives, not the six of US_Model's XML
TEST_F( DataPubMockRules, AModelsVarianceAndMeniscusComeFromTheManifest )
{
   const double variance = 1.0 / 3.0e5;
   const double meniscus = 5.8712345678901;

   craft( [ & ]( US_DataPubManifest& m, const QString& )
   {
      QList< US_DataPubEntity > list = m.section( US_DataPub::Model );
      ASSERT_EQ( list.size(), 1 );
      list[ 0 ].attrs.insert( "variance", QString::number( variance, 'g', 17 ) );
      list[ 0 ].attrs.insert( "meniscus", QString::number( meniscus, 'g', 17 ) );
      m.setSection( US_DataPub::Model, list );
   } );

   US_DataPubImporter importer;
   QString            error;
   ASSERT_TRUE( importOnce( importer, error, dbOptions() ) )
      << error.toStdString();

   ASSERT_EQ( fake.callsOf( "new_model" ).size(), 1 );
   EXPECT_EQ( fake.callsOf( "new_model" )[ 0 ].value( 4 ).toDouble(), variance )
      << fake.callsOf( "new_model" )[ 0 ].value( 4 ).toStdString();
   EXPECT_EQ( fake.callsOf( "new_model" )[ 0 ].value( 5 ).toDouble(), meniscus )
      << fake.callsOf( "new_model" )[ 0 ].value( 5 ).toStdString();
}

// The manifest's values are the source row's, at full precision; the XML's
// are US_Model's six digits of them, and do not win
TEST_F( DataPubMockRules, TheManifestsVarianceWinsOverTheXmls )
{
   const double variance = 2.0 / 3.0e4;

   craft( [ & ]( US_DataPubManifest& m, const QString& stage )
   {
      QList< US_DataPubEntity > list = m.section( US_DataPub::Model );
      ASSERT_EQ( list.size(), 1 );

      // The XML says 6.66667e-05
      QString path = stage + "/" + list[ 0 ].payload;
      QFile   file( path );
      ASSERT_TRUE( file.open( QIODevice::ReadOnly ) );
      QString text = QString::fromUtf8( file.readAll() );
      file.close();
      ASSERT_TRUE( text.contains( "<model " ) );
      text.remove( QRegularExpression( " variance=\"[^\"]*\"" ) );
      text.replace( "<model ", "<model variance=\"" + QString::number( variance )
                               + "\" " );
      ASSERT_TRUE( file.open( QIODevice::WriteOnly | QIODevice::Truncate ) );
      file.write( text.toUtf8() );
      file.close();
      list[ 0 ].sha256 = US_DataPubHash::fileHash( path );

      list[ 0 ].attrs.insert( "variance", QString::number( variance, 'g', 17 ) );
      m.setSection( US_DataPub::Model, list );
   } );

   US_DataPubImporter importer;
   QString            error;
   ASSERT_TRUE( importOnce( importer, error, dbOptions() ) )
      << error.toStdString();

   ASSERT_EQ( fake.callsOf( "new_model" ).size(), 1 );
   EXPECT_EQ( fake.callsOf( "new_model" )[ 0 ].value( 4 ).toDouble(), variance )
      << fake.callsOf( "new_model" )[ 0 ].value( 4 ).toStdString();
}

// The concentrations a bundle carries for a buffer's components are
// matched by component ID, not by their order in the definitions file
class DataPubMockComponents : public DataPubMockExtras
{
};

TEST_F( DataPubMockComponents, ConcentrationsAreMatchedByComponentId )
{
   QMap< QString, US_BufferComponent > table;
   US_BufferComponent::getAllFromHD( table );
   ASSERT_TRUE( table.contains( "1" )  &&  table.contains( "2" ) )
      << "no local component table";

   US_Buffer buffer;
   QString   bpath = US_Settings::dataDir() + "/buffers/B0000001.xml";
   ASSERT_TRUE( buffer.readFromDisk( bpath ) );
   buffer.component     << table.value( "1" ) << table.value( "2" );
   buffer.componentIDs  << "1" << "2";
   buffer.concentration << 150.0 << 25.0;
   ASSERT_TRUE( buffer.writeToDisk( bpath ) );
   reexport();

   // The definitions file with its two components the other way round
   US_DataPubImporter source;
   QString            error;
   ASSERT_TRUE( source.inspect( bundlePath, error ) ) << error.toStdString();

   US_DataPubBundle out;
   ASSERT_TRUE( out.createStaging( error ) );
   QString stage = out.stagingPath();

   QDir        src( source.bundleRoot() );
   QStringList files;
   QDirIterator it( source.bundleRoot(), QDir::Files,
                    QDirIterator::Subdirectories );

   while ( it.hasNext() )  files << src.relativeFilePath( it.next() );

   for ( int ii = 0; ii < files.size(); ii++ )
   {
      QDir().mkpath( QFileInfo( stage + "/" + files[ ii ] ).path() );
      ASSERT_TRUE( QFile::copy( source.bundleRoot() + "/" + files[ ii ],
                                stage + "/" + files[ ii ] ) );
   }

   US_DataPubManifest mani;
   QString            mpath = stage + "/" + US_DataPubBundle::manifestName();
   ASSERT_TRUE( mani.read( mpath, error ) ) << error.toStdString();

   QList< US_DataPubEntity > bufs = mani.section( US_DataPub::Buffer );
   ASSERT_EQ( bufs.size(), 1 );
   QString cpath = stage + "/" + bufs[ 0 ].attrs.value( "componentsPayload" );

   QMap< QString, US_BufferComponent > defs;
   ASSERT_TRUE( US_DataPubRecords::readBufferComponents( cpath, defs ) );
   ASSERT_EQ  ( defs.size(), 2 );

   QList< US_BufferComponent > reversed;
   reversed << defs.value( "2" ) << defs.value( "1" );
   QFile::remove( cpath );
   ASSERT_TRUE( US_DataPubRecords::writeBufferComponents(
                   reversed, QList< double >() << 25.0 << 150.0, cpath ) );

   bufs[ 0 ].attrs.insert( US_DataPub::digestKeyOf( "componentsPayload" ),
                           US_DataPubHash::fileHash( cpath ) );
   mani.setSection( US_DataPub::Buffer, bufs );
   ASSERT_TRUE( mani.write( mpath, error ) ) << error.toStdString();

   QFile digest( stage + "/" + US_DataPubExporter::manifestDigestName() );
   ASSERT_TRUE( digest.open( QIODevice::WriteOnly | QIODevice::Truncate ) );
   digest.write( ( US_DataPubHash::fileHash( mpath ) + "  "
                   + US_DataPubBundle::manifestName() + "\n" ).toUtf8() );
   digest.close();

   bundlePath = root + "/reversed.tar.gz";
   QFile::remove( bundlePath );
   ASSERT_TRUE( out.pack( bundlePath, error ) ) << error.toStdString();

   US_DataPubImporter importer;
   ASSERT_TRUE( importOnce( importer, error, dbOptions() ) )
      << error.toStdString();

   // Each component with its own concentration, whatever IDs the target
   // gave them
   QList< QStringList > adds = fake.callsOf( "add_buffer_component" );
   ASSERT_EQ( adds.size(), 2 );

   QMap< QString, QString > concOf;   // component name -> concentration

   for ( int ii = 0; ii < adds.size(); ii++ )
   {
      int id = adds[ ii ].value( 2 ).toInt();
      ASSERT_TRUE( fake.state.components.contains( id ) );
      concOf.insert( fake.state.components.value( id ).value( 1 ).toString(),
                     adds[ ii ].value( 3 ) );
   }

   EXPECT_EQ( concOf.value( table.value( "1" ).name ).toDouble(), 150.0 );
   EXPECT_EQ( concOf.value( table.value( "2" ).name ).toDouble(),  25.0 );
}

// ------------------------------------------------ staging directories

#if defined( Q_OS_UNIX )
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

// A staging directory whose importer is gone is swept; a live importer's
// is not, whatever its age
TEST_F( DataPubMockImport, StaleStagingDirectoriesAreSwept )
{
   US_DataPubImporter live;
   QString            error;
   ASSERT_TRUE( live.inspect( bundlePath, error ) ) << error.toStdString();
   QString liveDir = QFileInfo( live.bundleRoot() ).path();
   ASSERT_TRUE( QFile::exists( liveDir + "/import.lock" ) );

   QString parent = US_Settings::tmpDir() + "/us_datapub";
   ASSERT_TRUE( QDir().mkpath( parent + "/bundle_dead" ) );

   // The lock of a process that took it and was killed
   fflush( nullptr );
   pid_t pid = fork();
   ASSERT_GE( pid, 0 );

   if ( pid == 0 )
   {
      QLockFile held( parent + "/bundle_dead/import.lock" );
      held.tryLock( 0 );
      _exit( held.isLocked() ? 0 : 1 );    // Without unlocking it
   }

   int status = 0;
   ASSERT_EQ( waitpid( pid, &status, 0 ), pid );
   ASSERT_TRUE( WIFEXITED( status )  &&  WEXITSTATUS( status ) == 0 );
   ASSERT_TRUE( QFile::exists( parent + "/bundle_dead/import.lock" ) );

   // A directory without a lock that has not changed for days
   ASSERT_TRUE( QDir().mkpath( parent + "/bundle_old" ) );
   QDateTime past = QDateTime::currentDateTimeUtc().addDays( -3 );
   struct timeval times[ 2 ];
   times[ 0 ].tv_sec  = time_t( past.toSecsSinceEpoch() );
   times[ 0 ].tv_usec = 0;
   times[ 1 ]         = times[ 0 ];
   ASSERT_EQ( utimes( QFile::encodeName( parent + "/bundle_old" ).constData(),
                      times ), 0 );

   // ... and one that changed a moment ago
   ASSERT_TRUE( QDir().mkpath( parent + "/bundle_new" ) );

   EXPECT_EQ  ( US_DataPubBundle::sweepStaleStaging(), 2 );
   EXPECT_FALSE( QFile::exists( parent + "/bundle_old" ) );
   EXPECT_TRUE ( QFile::exists( parent + "/bundle_new" ) );
   EXPECT_FALSE( QFile::exists( parent + "/bundle_dead" ) );
   EXPECT_TRUE ( QFile::exists( liveDir ) );

   // ... and once the importer is gone, so is its directory
   live.inspect( bundlePath, error );
   EXPECT_FALSE( QFile::exists( liveDir ) );
}

// An importer stopped by a signal leaves its staging directory to the
// sweep: no handler removes it (a signal handler cannot do that safely),
// the signal ends the process as it would anyway, and the directory keeps
// the lock of a process that is gone, so the next import removes it
TEST_F( DataPubMockImport, AStoppedImportersDirectoryIsSweptNextTime )
{
   QString     parent = US_Settings::tmpDir() + "/us_datapub";
   QStringList before = QDir( parent ).entryList( QStringList( "bundle_*" ),
                                                  QDir::Dirs
                                                  | QDir::NoDotAndDotDot );
   fflush( nullptr );
   pid_t pid = fork();
   ASSERT_GE( pid, 0 );

   if ( pid == 0 )
   {
      US_DataPubImporter importer;
      QString            error;

      if ( ! importer.inspect( bundlePath, error ) )  _exit( 2 );

      raise( SIGTERM );
      _exit( 3 );                     // Not reached
   }

   int status = 0;
   ASSERT_EQ( waitpid( pid, &status, 0 ), pid );
   ASSERT_TRUE( WIFSIGNALED( status ) ) << "exit " << WEXITSTATUS( status );
   EXPECT_EQ( WTERMSIG( status ), SIGTERM );

   QStringList after = QDir( parent ).entryList( QStringList( "bundle_*" ),
                                                 QDir::Dirs
                                                 | QDir::NoDotAndDotDot );
   QStringList left;

   for ( const QString& dir : after )
      if ( ! before.contains( dir ) )  left << dir;

   ASSERT_EQ( left.size(), 1 );
   QString stage = parent + "/" + left[ 0 ];
   EXPECT_TRUE( QFile::exists( stage + "/import.lock" ) );

   EXPECT_GE  ( US_DataPubBundle::sweepStaleStaging(), 1 );
   EXPECT_FALSE( QFile::exists( stage ) );
}

// The sweep an import starts with is the bundle's, which honours any
// process's lock: the staging directory of an export that is still
// running -- holding export.lock, not import.lock -- is never removed,
// however long it has not changed
TEST_F( DataPubMockImport, AnImportLeavesARunningExportsStagingAlone )
{
   US_DataPubBundle exporting;
   QString          error;
   exporting.setStagingLock( "export.lock" );
   ASSERT_TRUE( exporting.createStaging( error ) ) << error.toStdString();

   QString base = QFileInfo( exporting.stagingPath() ).path();
   ASSERT_TRUE( QFile::exists( base + "/export.lock" ) );

   QDateTime past = QDateTime::currentDateTimeUtc().addDays( -3 );
   struct timeval times[ 2 ];
   times[ 0 ].tv_sec  = time_t( past.toSecsSinceEpoch() );
   times[ 0 ].tv_usec = 0;
   times[ 1 ]         = times[ 0 ];
   ASSERT_EQ( utimes( QFile::encodeName( base ).constData(), times ), 0 );

   US_DataPubImporter importer;
   ASSERT_TRUE( importer.inspect( bundlePath, error ) ) << error.toStdString();
   EXPECT_TRUE( QFile::exists( exporting.stagingPath() ) );
   EXPECT_TRUE( exporting.stagingLocked() );

   exporting.cleanup();
}

// The lock is in the staging directory from the moment it is made -- a
// sweep never finds a live directory without it -- and it is the last
// thing removed
TEST_F( DataPubMockImport, TheStagingDirectoryIsLockedFromTheStart )
{
   US_DataPubBundle bundle;
   QString          error;
   bundle.setStagingLock( "import.lock" );
   ASSERT_TRUE( bundle.createStaging( error ) ) << error.toStdString();

   QString base = QFileInfo( bundle.stagingPath() ).path();
   EXPECT_TRUE( bundle.stagingLocked() );
   ASSERT_TRUE( QFile::exists( base + "/import.lock" ) );

   // Held: a probe cannot take it, and the sweep leaves the directory
   QLockFile probe( base + "/import.lock" );
   probe.setStaleLockTime( 0 );
   EXPECT_FALSE( probe.tryLock( 0 ) );

   // ... even one that has not changed for days
   QDateTime past = QDateTime::currentDateTimeUtc().addDays( -3 );
   struct timeval times[ 2 ];
   times[ 0 ].tv_sec  = time_t( past.toSecsSinceEpoch() );
   times[ 0 ].tv_usec = 0;
   times[ 1 ]         = times[ 0 ];
   ASSERT_EQ( utimes( QFile::encodeName( base ).constData(), times ), 0 );
   US_DataPubBundle::sweepStaleStaging();
   EXPECT_TRUE( QFile::exists( bundle.stagingPath() ) );

   bundle.cleanup();
   EXPECT_FALSE( bundle.stagingLocked() );
   EXPECT_FALSE( QFile::exists( base ) );

   // A bundle that fails to unpack leaves nothing either
   QString junk = root + "/junk.tar.gz";
   QFile   file( junk );
   ASSERT_TRUE( file.open( QIODevice::WriteOnly ) );
   file.write( "not an archive" );
   file.close();

   QStringList before = QDir( US_Settings::tmpDir() + "/us_datapub" )
                        .entryList( QDir::Dirs | QDir::NoDotAndDotDot );
   US_DataPubImporter importer;
   EXPECT_FALSE( importer.inspect( junk, error ) );
   EXPECT_EQ( QDir( US_Settings::tmpDir() + "/us_datapub" )
              .entryList( QDir::Dirs | QDir::NoDotAndDotDot ), before );
}
#endif

// new_experiment refuses an operator without a permit for the instrument:
// the import records one who holds a permit instead, with a warning -- or,
// when no one does, refuses the run
TEST_F( DataPubMockRules, TheOperatorHoldsAPermitForTheInstrument )
{
   fake.operators = QList< int >() << 5;

   US_DataPubImporter importer;
   QString            error;
   ASSERT_TRUE( importOnce( importer, error, dbOptions() ) )
      << error.toStdString();

   ASSERT_EQ( fake.callsOf( "new_experiment" ).size(), 1 );
   EXPECT_EQ( fake.callsOf( "new_experiment" )[ 0 ].value( 6 ), "5" );
   EXPECT_TRUE( importer.warnings().join( "\n" ).contains( "operator" ) )
      << importer.warnings().join( "\n" ).toStdString();

   DataPubFakeDb none;
   none.addHardware( "DEMO-1" );
   none.operators.clear();

   US_DataPubImporter refused;
   refused.setDatabase( none.db() );
   ASSERT_TRUE ( refused.inspect( bundlePath, error ) );
   EXPECT_FALSE( refused.runImport( dbOptions(), error ) );
   EXPECT_TRUE ( error.contains( "permit" ) ) << error.toStdString();
}

// ------------------------------------------------- the minors of round six



// Of several rows of the same definition, the buffer is linked to the one
// of the highest ID: the rule of US_BufferComponent::matchingID(), which
// the library's own lookups follow too
TEST_F( DataPubMockExtras, OfIdenticalComponentsTheHighestIdIsTaken )
{
   exportWithComponent();

   fake.canned.insert( "get_buffer_component_desc", QList< QVariantList >()
                       << ( QVariantList() << 41 ) << ( QVariantList() << 42 ) );
   fake.canned.insert( "get_buffer_component_info", QList< QVariantList >()
                       << componentRow( component ) );

   US_DataPubImporter importer;
   QString            error;
   ASSERT_TRUE( importOnce( importer, error, dbOptions() ) )
      << error.toStdString();

   EXPECT_EQ( fake.callsOf( "create_buffer_component" ).size(), 0 );
   ASSERT_EQ( fake.callsOf( "add_buffer_component" ).size(), 1 );
   EXPECT_EQ( fake.callsOf( "add_buffer_component" )[ 0 ].value( 2 ), "42" );
}

// A rotor calibration without a label is named by its GUID in what the
// import says, never as ""
TEST_F( DataPubMockExtras, ACalibrationWithoutALabelIsNamedByItsGuid )
{
   QString path = US_Settings::dataDir() + "/rotors/C0000001.xml";
   US_Rotor::RotorCalibration calibration;
   ASSERT_TRUE( US_DataPubRecords::readCalibration( path, calibration ) );
   calibration.label.clear();
   ASSERT_TRUE( US_DataPubRecords::writeCalibration( calibration, path ) );
   reexport();

   US_DataPubImporter probe;
   QString            error;
   ASSERT_TRUE( probe.inspect( bundlePath, error ) );
   QList< US_DataPubEntity > cals =
         probe.manifest().section( US_DataPub::RotorCalibration );
   ASSERT_EQ( cals.size(), 1 );
   fake.registry[ "rotor" ].insert( cals[ 0 ].attrs.value( "rotorGUID" ), 3 );

   US_DataPubImporter importer;
   ASSERT_TRUE( importOnce( importer, error, dbOptions() ) )
      << error.toStdString();

   QString said = importer.warnings().join( "\n" );
   EXPECT_TRUE ( said.contains( "Rotor calibration " + cals[ 0 ].guid ) )
      << said.toStdString();
   EXPECT_FALSE( said.contains( "\"\"" ) ) << said.toStdString();
}

// Experiment 1 is the ID verify_experiment_permission lets anyone use, so
// the raw data of whoever's first run it became is found for everyone.
// Found is not one's own: the run is another investigator's
TEST_F( DataPubMockImport, TheRunOfExperimentOneIsAnotherInvestigators )
{
   fake.addExperiment( 1, expGUID, runID, 9 );
   fake.addRecord( "rawData", rawGUID, 51, 9 );
   fake.state.rawExperiments.insert( rawGUID, 1 );

   US_DataPubImporter importer;
   QString            error;

   EXPECT_FALSE( importOnce( importer, error, dbOptions() ) );
   EXPECT_TRUE ( error.contains( "another investigator's run" ) )
      << error.toStdString();
   EXPECT_FALSE( error.contains( "same investigator" ) ) << error.toStdString();
   EXPECT_EQ   ( fake.writes(), 0 );
}

// What the manifest carries of the source's rows beyond their payloads is
// written as it was: a buffer's private flag, an edit's comment, the raw
// data's label and comment, an analyte's spectrum column.  Without it,
// what the importer always wrote.
class DataPubMockFields : public DataPubMockRules
{
   protected:
      void setAttr( US_DataPubManifest& m, US_DataPub::EntityType type,
                    const QString& key, const QString& value )
      {
         QList< US_DataPubEntity > list = m.section( type );
         ASSERT_FALSE( list.isEmpty() );

         for ( int ii = 0; ii < list.size(); ii++ )
            list[ ii ].attrs.insert( key, value );

         m.setSection( type, list );
      }

      void exportAgain( const QString& name )
      {
         US_DataPubExporter            exporter;
         US_DataPubExporter::Selection selection;
         selection.fromDb = false;
         selection.scope  = US_DataPub::ScopeNoise;
         selection.runIDs << runID;
         bundlePath = root + "/" + name + ".tar.gz";
         QString error;
         ASSERT_TRUE( exporter.exportBundle( selection, bundlePath, error ) )
            << error.toStdString();
      }
};

TEST_F( DataPubMockFields, WithoutTheFieldsTheDefaultsAreWritten )
{
   US_DataPubImporter importer;
   QString            error;
   ASSERT_TRUE( importOnce( importer, error, dbOptions() ) )
      << error.toStdString();

   ASSERT_EQ( fake.callsOf( "new_buffer" ).size(), 1 );
   EXPECT_EQ( fake.callsOf( "new_buffer" )[ 0 ].value( 8 ), "1" );
   ASSERT_EQ( fake.callsOf( "new_editedData" ).size(), 1 );
   EXPECT_EQ( fake.callsOf( "new_editedData" )[ 0 ].value( 5 ), QString() );
   ASSERT_EQ( fake.callsOf( "new_rawData" ).size(), 1 );
   EXPECT_EQ( fake.callsOf( "new_rawData" )[ 0 ].value( 2 ),
              fake.callsOf( "new_experiment" ).value( 0 ).value( 13 ) );
}

TEST_F( DataPubMockFields, TheSourcesFieldsAreWritten )
{
   craft( [ & ]( US_DataPubManifest& m, const QString& )
   {
      setAttr( m, US_DataPub::Buffer,     "private",  "0" );
      setAttr( m, US_DataPub::EditedData, "comment",  "edited by hand" );
      setAttr( m, US_DataPub::RawData,    "label",    "raw label" );
      setAttr( m, US_DataPub::RawData,    "comment",  "raw comment" );
      setAttr( m, US_DataPub::Analyte,    "spectrum", "" );
   } );

   US_DataPubImporter importer;
   QString            error;
   ASSERT_TRUE( importOnce( importer, error, dbOptions() ) )
      << error.toStdString();

   ASSERT_EQ( fake.callsOf( "new_buffer" ).size(), 1 );
   EXPECT_EQ( fake.callsOf( "new_buffer" )[ 0 ].value( 8 ), "0" );
   EXPECT_TRUE( fake.state.publicBuffers.contains( bufferGUID ) );

   ASSERT_EQ( fake.callsOf( "new_editedData" ).size(), 1 );
   EXPECT_EQ( fake.callsOf( "new_editedData" )[ 0 ].value( 5 ),
              "edited by hand" );

   ASSERT_EQ( fake.callsOf( "new_rawData" ).size(), 1 );
   EXPECT_EQ( fake.callsOf( "new_rawData" )[ 0 ].value( 2 ), "raw label" );
   EXPECT_EQ( fake.callsOf( "new_rawData" )[ 0 ].value( 4 ), "raw comment" );

   // ... while the run keeps its own label
   EXPECT_NE( fake.callsOf( "new_experiment" ).value( 0 ).value( 13 ),
              "raw label" );

   ASSERT_EQ( fake.callsOf( "new_analyte" ).size(), 1 );
   EXPECT_EQ( fake.callsOf( "new_analyte" )[ 0 ].value( 6 ), QString() );
}

// A spectrum column the source filled is kept as it was, not made again
// from the extinction profile
TEST_F( DataPubMockFields, AnAnalytesSpectrumColumnIsKept )
{
   craft( [ & ]( US_DataPubManifest& m, const QString& )
   { setAttr( m, US_DataPub::Analyte, "spectrum", "260/1.5" ); } );

   US_DataPubImporter importer;
   QString            error;
   ASSERT_TRUE( importOnce( importer, error, dbOptions() ) )
      << error.toStdString();

   ASSERT_EQ( fake.callsOf( "new_analyte" ).size(), 1 );
   EXPECT_EQ( fake.callsOf( "new_analyte" )[ 0 ].value( 6 ), "260/1.5" );
}

// Raw data records of one run that disagree on their label are written
// with the run's, and the import says so
TEST_F( DataPubMockFields, RawDataThatDisagreeKeepTheRunsLabel )
{
   addSecondTriple();
   ASSERT_TRUE( secondTriple );
   exportAgain( "two_triples" );

   craft( [ & ]( US_DataPubManifest& m, const QString& )
   {
      QList< US_DataPubEntity > list = m.section( US_DataPub::RawData );
      ASSERT_EQ( list.size(), 2 );
      list[ 0 ].attrs.insert( "label", "one" );
      list[ 1 ].attrs.insert( "label", "two" );
      m.setSection( US_DataPub::RawData, list );
   } );

   US_DataPubImporter importer;
   QString            error;
   ASSERT_TRUE( importOnce( importer, error, dbOptions() ) )
      << error.toStdString();

   ASSERT_EQ( fake.callsOf( "new_rawData" ).size(), 2 );
   QString runLabel = fake.callsOf( "new_experiment" ).value( 0 ).value( 13 );
   EXPECT_EQ( fake.callsOf( "new_rawData" )[ 0 ].value( 2 ), runLabel );
   EXPECT_EQ( fake.callsOf( "new_rawData" )[ 1 ].value( 2 ), runLabel );
   EXPECT_TRUE( importer.log().join( "\n" ).contains( "do not share one label" ) )
      << importer.log().join( "\n" ).toStdString();
}

// A public buffer stays public, so another investigator's import of the
// same chemistry uses it instead of copying it
TEST_F( DataPubMockFields, APublicBufferIsUsedByAnotherInvestigator )
{
   craft( [ & ]( US_DataPubManifest& m, const QString& )
   { setAttr( m, US_DataPub::Buffer, "private", "0" ); } );

   US_DataPubImporter first;
   QString            error;
   ASSERT_TRUE( importOnce( first, error, dbOptions() ) )
      << error.toStdString();
   ASSERT_EQ( fake.callsOf( "new_buffer" ).size(), 1 );

   // Another run of the same buffer, by investigator 3
   QDir( US_Settings::dataDir()   ).removeRecursively();
   QDir( US_Settings::resultDir() ).removeRecursively();
   QDir().mkpath( US_Settings::dataDir()   );
   QDir().mkpath( US_Settings::resultDir() );
   projectGUID  = US_Util::new_guid();
   analyteGUID  = US_Util::new_guid();
   solutionGUID = US_Util::new_guid();
   expGUID      = US_Util::new_guid();
   rawGUID      = US_Util::new_guid();
   editGUID     = US_Util::new_guid();
   modelGUID    = US_Util::new_guid();
   noiseGUID    = US_Util::new_guid();
   calGUID      = US_Util::new_guid();
   buildStore();
   exportAgain( "second" );

   craft( [ & ]( US_DataPubManifest& m, const QString& )
   { setAttr( m, US_DataPub::Buffer, "private", "0" ); } );

   fake.person = 3;
   fake.operators << 3;
   US_Settings::set_us_inv_ID( 3 );

   US_DataPubImporter second;
   ASSERT_TRUE( importOnce( second, error, dbOptions() ) )
      << error.toStdString() << "\n" << second.log().join( "\n" ).toStdString();

   EXPECT_EQ( fake.callsOf( "new_buffer" ).size(), 1 );

   for ( const US_DataPubImporter::Result& result : second.results() )
      if ( result.type == US_DataPub::Buffer )
      {
         EXPECT_EQ( result.resolution, US_DataPub::ResolvedReused );
         EXPECT_EQ( result.targetGUID, bufferGUID );
      }
}







// A model that borrows a noise record is written after it: a database
// tells a borrowing model by a lastUpdated not earlier than the noise's
// timeEntered
TEST_F( DataPubMockImport, ABorrowingModelIsWrittenAfterItsNoise )
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
   ASSERT_TRUE( exporter.exportBundle( selection, bundlePath, error ) )
      << error.toStdString();

   US_DataPubEntity noise;
   ASSERT_TRUE( exporter.manifest().entity( US_DataPub::Noise, noiseGUID,
                                            noise ) );
   ASSERT_EQ( noise.attrs.value( "borrowedByModelGUIDs" ), laterGUID );

   US_DataPubImporter importer;
   ASSERT_TRUE( importOnce( importer, error, dbOptions() ) )
      << error.toStdString();

   int fitted   = -1;
   int borrower = -1;
   int noiseAt  = -1;

   for ( int ii = 0; ii < fake.calls.size(); ii++ )
   {
      const QStringList& call = fake.calls[ ii ];

      if ( call.value( 0 ) == "new_model"  &&  call.value( 1 ) == modelGUID )
         fitted = ii;
      if ( call.value( 0 ) == "new_model"  &&  call.value( 1 ) == laterGUID )
         borrower = ii;
      if ( call.value( 0 ) == "new_noise"  &&  call.value( 1 ) == noiseGUID )
         noiseAt = ii;
   }

   ASSERT_GE( fitted,   0 );
   ASSERT_GE( borrower, 0 );
   ASSERT_GE( noiseAt,  0 );
   EXPECT_LT( fitted,  noiseAt );
   EXPECT_GT( borrower, noiseAt );
   EXPECT_EQ( fake.commits, 1 );
}






// ------------------------------------- fix round 8: order, clock, dates

/* The import into a database, on the fake: the models and noise records of
   an edit in the source's order, the server's clock (UNIX_TIMESTAMP())
   moved on where two records have to be dated apart, the calibration's
   instant, the procedures a version marker stands for, and what a failed
   rollback says. */
class DataPubMockOrder : public DataPubMockRules
{
   protected:
      QString exportScenario( const QString& name )
      {
         US_DataPubExporter            exporter;
         US_DataPubExporter::Selection selection;
         selection.fromDb = false;
         selection.scope  = US_DataPub::ScopeNoise;
         selection.runIDs << runID;

         QString bundle = root + "/" + name + ".tar.gz";
         QString error;
         EXPECT_TRUE( exporter.exportBundle( selection, bundle, error ) )
            << error.toStdString();
         return bundle;
      }

      // The latest model with noise in the fake, as count_noise() picks it
      // in a database: the highest ID of the edit's models with noise
      static QString latestIn( DataPubFakeDb& db,
                               const US_DataPubManifest& mani )
      {
         QSet< QString > fitted;

         for ( const US_DataPubEntity& noise :
               mani.section( US_DataPub::Noise ) )
            fitted << noise.depend( US_DataPub::Model );

         QString latest;
         int     highest = 0;

         for ( const QString& guid : fitted )
         {
            int id = db.state.registry.value( "model" ).value( guid );

            if ( id > highest )
            {
               highest = id;
               latest  = guid;
            }
         }

         return latest;
      }

      // noise GUID -> borrowers, as a catalog of the fake would tell them
      // from the rows' time stamps and IDs (US_DataPubCatalog::noiseBefore)
      static QMap< QString, QString > borrowersIn(
            DataPubFakeDb& db, const US_DataPubManifest& mani )
      {
         QSet< QString >                fitted;
         QList< US_DataCatalog::Noise > rows;

         auto stampOf = [ & ]( const QString& guid )
         {
            return QDateTime::fromSecsSinceEpoch(
                      db.state.stamps.value( guid ), Qt::UTC )
                   .toString( "yyyy-MM-dd hh:mm:ss" ) + " UTC";
         };

         for ( const US_DataPubEntity& noise :
               mani.section( US_DataPub::Noise ) )
         {
            fitted << noise.depend( US_DataPub::Model );

            US_DataCatalog::Noise row;
            row.guid        = noise.guid;
            row.id          = QString::number(
                                 db.state.registry.value( "noise" )
                                 .value( noise.guid ) );
            row.noiseType   = noise.attrs.value( "noiseType" );
            row.lastUpdated = stampOf( noise.guid );
            rows << row;
         }

         QMap< QString, QStringList > by;

         for ( const US_DataPubEntity& model :
               mani.section( US_DataPub::Model ) )
         {
            if ( fitted.contains( model.guid ) )  continue;

            for ( const US_DataCatalog::Noise& noise :
                  US_DataPubCatalog::noiseBefore( rows, stampOf( model.guid ) ) )
               by[ noise.guid ] << model.guid;
         }

         QMap< QString, QString > map;

         for ( const US_DataPubEntity& noise :
               mani.section( US_DataPub::Noise ) )
         {
            QStringList names = by.value( noise.guid );
            std::sort( names.begin(), names.end() );
            map.insert( noise.guid, names.join( " " ) );
         }

         return map;
      }
};

/* A bundle laid out as a database export (newest first) into a database:
   the latest model with noise has the highest ID, as in the source, and
   every model borrows what it did -- the fake's rows dated by its clock,
   which the import waits on with UNIX_TIMESTAMP() (NOW(), whose wall time
   steps back an hour at the fall-back, only as timestamp2UTC() reads it,
   to see that step). */
TEST_F( DataPubMockOrder, ADatabaseOrderedBundleKeepsTheLatestModelAndTheBorrowing )
{
   DataPubBorrowScenario scenario;
   scenario.write( US_Settings::dataDir() + "/models",
                   US_Settings::dataDir() + "/noises", runID, editGUID,
                   noiseGUID );

   QString error;
   QString bundle = datapubRecraft( exportScenario( "plain" ),
                    root + "/dbordered.tar.gz",
                    []( US_DataPubManifest& m, const QString& )
                    { datapubAsDatabaseExport( m, true, false ); }, error );
   ASSERT_FALSE( bundle.isEmpty() ) << error.toStdString();

   US_DataPubImporter importer;
   importer.setDatabase( fake.db() );
   ASSERT_TRUE( importer.inspect( bundle, error ) ) << error.toStdString();
   ASSERT_TRUE( importer.runImport( dbOptions(), error ) )
      << error.toStdString() << "\n" << importer.log().join( "\n" ).toStdString();

   EXPECT_EQ( latestIn( fake, importer.manifest() ), scenario.latest() );
   EXPECT_EQ( DataPubBorrowScenario::text(
                 borrowersIn( fake, importer.manifest() ) ),
              DataPubBorrowScenario::text( scenario.expected() ) );

   int clock = 0;

   for ( const QStringList& call : fake.callsOf( "sql" ) )
   {
      if ( call.value( 1 ).contains( "UNIX_TIMESTAMP()" ) )  clock++;
      EXPECT_FALSE( QString( call.value( 1 ) )
                    .remove( "timestamp2UTC( NOW() )" ).contains( "NOW()" ) )
         << call.value( 1 ).toStdString();
   }

   EXPECT_GT( clock, 0 );
   EXPECT_EQ( fake.commits, 1 );
}

/* The review's dead end of the greedy sequence, into a database. */
TEST_F( DataPubMockOrder, TheGreedySequencesDeadEndIsWrittenInTheSourcesOrder )
{
   DataPubDeadEndScenario scenario;
   scenario.write( US_Settings::dataDir() + "/models",
                   US_Settings::dataDir() + "/noises", runID, editGUID,
                   noiseGUID );

   QString error;
   QString bundle = datapubRecraft( exportScenario( "deadend" ),
                    root + "/reversed.tar.gz",
                    []( US_DataPubManifest& m, const QString& )
                    { datapubAsDatabaseExport( m, true, false ); }, error );
   ASSERT_FALSE( bundle.isEmpty() ) << error.toStdString();

   US_DataPubImporter importer;
   importer.setDatabase( fake.db() );
   ASSERT_TRUE( importer.inspect( bundle, error ) ) << error.toStdString();
   ASSERT_TRUE( importer.runImport( dbOptions(), error ) )
      << error.toStdString();

   for ( const QString& text : importer.warnings() )
      EXPECT_FALSE( text.contains( "may not survive" ) ) << text.toStdString();

   EXPECT_EQ( DataPubBorrowScenario::text(
                 borrowersIn( fake, importer.manifest() ) ),
              DataPubBorrowScenario::text( scenario.expected() ) );
   EXPECT_EQ( latestIn( fake, importer.manifest() ), scenario.latest() );
}

/* A calibration's date is the instant the source's row had (the bundle's
   "dateUpdated", UTC), stored with the session on UTC for the call -- in a
   session east of UTC as anywhere -- and the session's zone put back.  A
   bundle with the day only (a disk store, an older export) stores 12:00
   UTC of it, which is that day in every zone within twelve hours of UTC:
   1970-01-01 too, which midnight east of UTC put before the epoch.  A date
   the TIMESTAMP column cannot hold is warned of, and the calibration keeps
   the date of the import. */
TEST_F( DataPubMockOrder, ACalibrationKeepsTheSourcesInstant )
{
   US_DataPubImporter probe;
   QString            error;
   ASSERT_TRUE( probe.inspect( bundlePath, error ) );
   QList< US_DataPubEntity > cals =
         probe.manifest().section( US_DataPub::RotorCalibration );
   ASSERT_EQ( cals.size(), 1 );
   QString rotorGUID = cals[ 0 ].attrs.value( "rotorGUID" );
   QString plain     = bundlePath;

   struct Case
   {
      QString dateUpdated;     // the bundle's; empty for the day only
      QString payloadDay;      // the payload's day, when changed
      QString stored;          // what set_rotor_calibration_date gets
   };

   const QList< Case > cases =
   {
      { "2024-01-01T21:30:05Z", QString(),    "2024-01-01 21:30:05" },
      { QString(),              QString(),    "2024-01-01 12:00:00" },
      { QString(),              "1970-01-01", "1970-01-01 12:00:00" },
      { "1969-12-31T23:00:00Z", QString(),    QString() },
      { "2038-01-19T03:14:08Z", QString(),    QString() }
   };

   for ( const Case& one : cases )
   {
      SCOPED_TRACE( ( one.dateUpdated + " " + one.payloadDay ).toStdString() );
      bundlePath = plain;
      craft( [ & ]( US_DataPubManifest& m, const QString& stage )
      {
         QList< US_DataPubEntity > list =
               m.section( US_DataPub::RotorCalibration );

         if ( ! one.dateUpdated.isEmpty() )
            list[ 0 ].attrs.insert( "dateUpdated", one.dateUpdated );

         if ( ! one.payloadDay.isEmpty() )
         {
            QString path = stage + "/" + list[ 0 ].payload;
            QFile   file( path );
            ASSERT_TRUE( file.open( QIODevice::ReadOnly ) );
            QString text = QString::fromUtf8( file.readAll() );
            file.close();
            text.replace( "2024-01-01", one.payloadDay );
            ASSERT_TRUE( file.open( QIODevice::WriteOnly | QIODevice::Truncate ) );
            file.write( text.toUtf8() );
            file.close();
            list[ 0 ].sha256 = US_DataPubHash::fileHash( path );
         }

         m.setSection( US_DataPub::RotorCalibration, list );
      } );

      DataPubFakeDb target;
      target.addHardware( "DEMO-1" );
      target.registry[ "rotor" ].insert( rotorGUID, 3 );
      target.admin          = true;
      target.timeZone       = "+05:00";

      QStringList said[ 1 ];

      {
         US_DataPubImporter importer;
         importer.setDatabase( target.db() );
         ASSERT_TRUE( importer.inspect( bundlePath, error ) );
         ASSERT_TRUE( importer.runImport( dbOptions(), error ) )
            << error.toStdString();

         for ( const QString& text : importer.warnings() )
            if ( text.contains( "calibration" ) )
               said[ 0 ] << text;
      }

      EXPECT_EQ( target.timeZone, QString( "+05:00" ) );

      int calID = target.registry[ "calibration" ].value( cals[ 0 ].guid );
      QList< QStringList > dates = target.callsOf( "set_rotor_calibration_date" );

      if ( one.stored.isEmpty() )
      {
         EXPECT_TRUE( dates.isEmpty() );
         ASSERT_EQ( said[ 0 ].size(), 1 );
         EXPECT_TRUE( said[ 0 ][ 0 ].contains( "outside" ) )
            << said[ 0 ][ 0 ].toStdString();
         continue;
      }

      EXPECT_TRUE( said[ 0 ].isEmpty() ) << said[ 0 ].join( "\n" ).toStdString();
      ASSERT_EQ( dates.size(), 1 );
      EXPECT_EQ( target.state.calibrationDates.value( calID ), one.stored );
      EXPECT_EQ( target.state.calibrationZones.value( calID ),
                 QString( "+00:00" ) );
   }
}

/* A connection lost while set_rotor_calibration_date runs is said as the
   loss it is.  The import then set the session's zone back over the dead
   connection and said only that the zone could not be set back. */
TEST_F( DataPubMockOrder, ALostConnectionDuringTheCalibrationDateIsSaid )
{
   US_DataPubImporter probe;
   QString            error;
   ASSERT_TRUE( probe.inspect( bundlePath, error ) );
   QString rotorGUID = probe.manifest().section( US_DataPub::RotorCalibration )
                       [ 0 ].attrs.value( "rotorGUID" );

   DataPubFakeDb lost;
   lost.addHardware( "DEMO-1" );
   lost.registry[ "rotor" ].insert( rotorGUID, 3 );
   lost.admin            = true;
   lost.timeZone         = "+05:00";
   lost.loseConnectionOn = "set_rotor_calibration_date";

   US_DataPubImporter importer;
   importer.setDatabase( lost.db() );
   ASSERT_TRUE ( importer.inspect( bundlePath, error ) );
   EXPECT_FALSE( importer.runImport( dbOptions(), error ) );

   EXPECT_TRUE( error.contains( "connection to the target database was lost"
                                " during set_rotor_calibration_date" ) )
      << error.toStdString();
   EXPECT_FALSE( error.contains( "set back" ) ) << error.toStdString();
   EXPECT_TRUE( importer.outcome().contains( "Nothing was imported" ) )
      << importer.outcome().toStdString();
   EXPECT_EQ( lost.commits, 0 );
   EXPECT_TRUE( lost.state.registry.value( "calibration" ).isEmpty() );
}

/* Where the session's zone cannot be read (nor so be set to UTC and back),
   the instant goes to set_rotor_calibration_date as the server's wall time
   for it (FROM_UNIXTIME()), which the fake now gives in the session's zone,
   as a server does: the row holds the source's instant, east of UTC as
   west.  The fake gave the UTC wall time whatever the zone, so this path
   was never tested. */
TEST_F( DataPubMockOrder, ACalibrationDateInAZoneThatCannotBeReadIsTheSources )
{
   const QString instant = "2024-01-01T21:30:05Z";
   craft( [ & ]( US_DataPubManifest& m, const QString& )
   {
      QList< US_DataPubEntity > cals = m.section( US_DataPub::RotorCalibration );
      cals[ 0 ].attrs.insert( "dateUpdated", instant );
      m.setSection( US_DataPub::RotorCalibration, cals );
   } );

   US_DataPubImporter probe;
   QString            error;
   ASSERT_TRUE( probe.inspect( bundlePath, error ) );
   QList< US_DataPubEntity > cals =
         probe.manifest().section( US_DataPub::RotorCalibration );
   qint64 wanted = 0;
   ASSERT_TRUE( US_DataPub::utcStampSeconds( instant, wanted ) );

   for ( const QString& system : { QString( "Europe/Berlin" ),
                                   QString( "+05:30" ),
                                   QString( "America/Chicago" ) } )
   {
      SCOPED_TRACE( system.toStdString() );
      DataPubFakeDb target;
      target.addHardware( "DEMO-1" );
      target.registry[ "rotor" ].insert( cals[ 0 ].attrs.value( "rotorGUID" ),
                                         3 );
      target.admin          = true;
      target.zoneUnreadable = true;
      target.systemZone     = system;

      US_DataPubImporter importer;
      importer.setDatabase( target.db() );
      ASSERT_TRUE( importer.inspect( bundlePath, error ) );
      ASSERT_TRUE( importer.runImport( dbOptions(), error ) )
         << error.toStdString();

      int calID = target.registry[ "calibration" ].value( cals[ 0 ].guid );
      ASSERT_EQ( target.callsOf( "set_rotor_calibration_date" ).size(), 1 );
      EXPECT_EQ( target.state.calibrationZones.value( calID ),
                 QString( "SYSTEM" ) );
      EXPECT_EQ( target.instantOf( target.state.calibrationDates.value( calID ),
                                   target.state.calibrationZones.value( calID ) ),
                 wanted )
         << target.state.calibrationDates.value( calID ).toStdString();
   }
}

/* The catalog reads a database's time stamps through timestamp2UTC(),
   which steps back an hour where the server's zone falls back to standard
   time; records written across that step are read in the wrong order, and
   a model may borrow another noise record than in the source.  The import
   sees the step while it writes and says so, naming the edit; without the
   step it says nothing. */
TEST_F( DataPubMockOrder, AClockThatFallsBackWhileBorrowersAreWrittenIsSaid )
{
   DataPubBorrowScenario scenario;
   scenario.write( US_Settings::dataDir() + "/models",
                   US_Settings::dataDir() + "/noises", runID, editGUID,
                   noiseGUID );
   QString bundle = exportScenario( "fallback" );

   for ( bool falls : { false, true } )
   {
      SCOPED_TRACE( falls ? "falls back" : "steady" );
      DataPubFakeDb target;
      target.addHardware( "DEMO-1" );

      if ( falls )
      {  // An hour back a few seconds into the writes
         qint64 step = target.clockMs / 1000 + 2;
         target.catalogClock = [ step ]( qint64 second )
         { return second < step ? second : second - 3600; };
      }

      US_DataPubImporter importer;
      QString            error;
      importer.setDatabase( target.db() );
      ASSERT_TRUE( importer.inspect( bundle, error ) );
      ASSERT_TRUE( importer.runImport( dbOptions(), error ) )
         << error.toStdString();

      QStringList said;

      for ( const QString& text : importer.warnings() )
         if ( text.contains( "went back" ) )  said << text;

      if ( ! falls )
      {
         EXPECT_TRUE( said.isEmpty() ) << said.join( "\n" ).toStdString();
         EXPECT_EQ( DataPubBorrowScenario::text(
                       borrowersIn( target, importer.manifest() ) ),
                    DataPubBorrowScenario::text( scenario.expected() ) );
         continue;
      }

      ASSERT_EQ( said.size(), 1 ) << importer.warnings().join( "\n" )
                                     .toStdString();
      EXPECT_TRUE( said[ 0 ].contains( editGUID ) ) << said[ 0 ].toStdString();

      // What the warning is about: the borrowing the catalog reads is not
      // the source's
      EXPECT_NE( DataPubBorrowScenario::text(
                    borrowersIn( target, importer.manifest() ) ),
                 DataPubBorrowScenario::text( scenario.expected() ) );
   }
}

/* A disk store whose file numbers have gaps filled after removals, into a
   database: the latest model with noise has the highest ID, and every
   model borrows what it did. */
TEST_F( DataPubMockOrder, AStoreWithFilledGapsKeepsItsLatestModelAndBorrowing )
{
   DataPubGapScenario scenario;
   scenario.write( US_Settings::dataDir() + "/models",
                   US_Settings::dataDir() + "/noises", runID, editGUID,
                   noiseGUID );

   US_DataPubImporter importer;
   QString            error;
   importer.setDatabase( fake.db() );
   ASSERT_TRUE( importer.inspect( exportScenario( "gaps" ), error ) );
   ASSERT_TRUE( importer.runImport( dbOptions(), error ) )
      << error.toStdString();

   for ( const QString& text : importer.warnings() )
      EXPECT_FALSE( text.contains( "may not survive" ) ) << text.toStdString();

   EXPECT_EQ( latestIn( fake, importer.manifest() ), scenario.latest() );
   EXPECT_EQ( DataPubBorrowScenario::text(
                 borrowersIn( fake, importer.manifest() ) ),
              DataPubBorrowScenario::text( scenario.expected() ) );
}

/* A rollback refused because the connection was lost says what is known:
   the server rolled back an import whose connection was lost before the
   commit, and nothing was kept -- not both that and "some records may have
   been kept"; a connection lost during the commit leaves it open. */
TEST_F( DataPubMockOrder, ALostConnectionSaysWhatWasKept )
{
   struct Case
   {
      QString     rollback;      // what the rollback says
      bool        commitFails;   // the commit is what fails
      QStringList says;
      QStringList never;
   };

   QString lostBefore = QObject::tr( US_DB2::connectionLostRollbackMessage );
   QString lostDuring = QObject::tr( US_DB2::commitUnknownMessage )
                        .arg( "Lost connection to server during query" );

   const QList< Case > cases =
   {
      { lostBefore, false, { "Nothing was imported", "rolled the import back" },
                           { "may have been kept" } },
      { lostDuring, true,  { "not known" },
                           { "Nothing was imported", "may have been kept" } },
      { QObject::tr( US_DB2::transactionEndedMessage ), false,
                           { "may have been kept", "implicit commit" },
                           { "Nothing was imported" } }
   };

   for ( const Case& one : cases )
   {
      SCOPED_TRACE( one.rollback.toStdString() );
      DataPubFakeDb lost;
      lost.addHardware( "DEMO-1" );

      if ( ! one.commitFails )  lost.failOn = "new_noise";

      ON_CALL( lost.mock, rollbackTransaction( ::testing::_ ) )
         .WillByDefault( ::testing::Invoke( [ & ]( QString& error )
         {
            lost.inTxn = false;
            error      = one.rollback;
            return false;
         } ) );

      if ( one.commitFails )
         ON_CALL( lost.mock, commitTransaction( ::testing::_ ) )
            .WillByDefault( ::testing::Invoke( [ & ]( QString& error )
            {
               error = one.rollback;
               return false;
            } ) );

      US_DataPubImporter importer;
      QString            error;
      importer.setDatabase( lost.db() );
      ASSERT_TRUE ( importer.inspect( bundlePath, error ) );
      EXPECT_FALSE( importer.runImport( dbOptions(), error ) );

      for ( const QString& text : one.says )
         EXPECT_TRUE( importer.outcome().contains( text ) )
            << importer.outcome().toStdString();

      for ( const QString& text : one.never )
         EXPECT_FALSE( importer.outcome().contains( text ) )
            << importer.outcome().toStdString();
   }
}

