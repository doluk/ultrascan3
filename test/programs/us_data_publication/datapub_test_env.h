// test/programs/us_data_publication/datapub_test_env.h
#pragma once

#include <gtest/gtest.h>

#include <QtCore>

#include "us_settings.h"
#include "us_util.h"
#include "us_project.h"
#include "us_buffer.h"
#include "us_analyte.h"
#include "us_solution.h"
#include "us_model.h"
#include "us_noise.h"
#include "us_dataIO.h"
#include "us_time_state.h"
#include "us_crypto.h"
#include "us_db2.h"

#include "us_datapub_records.h"

#include "us3_gtest_sandbox.h"

/**
 * Whether US_Settings writes into the test sandbox.
 *
 * The fixture below changes the "workBaseDir" setting.  That is only safe
 * when the process runs in the sandbox us3EnterSandbox() sets up (as
 * test_us_datapub_main.cpp does): US3_SETTINGS_ROOT is set, and the
 * settings store's file really is under it -- the store resolves the root
 * once, so a variable set too late would not count.
 * \param why Filled in with the reason when it is not
 */
inline bool datapub_settings_sandboxed( QString* why = nullptr )
{
   QString reason;
   QString sandbox = qEnvironmentVariable( "US3_SETTINGS_ROOT" );

   if ( sandbox.isEmpty() )
      reason = "US3_SETTINGS_ROOT is not set; call us3EnterSandbox() in main()"
               " before anything reads US_Settings";

   else
   {
      US_SettingsStore store;
      QString file = QFileInfo( store.fileName() ).absoluteFilePath();
      QString dir  = QDir( sandbox ).absolutePath() + "/";

      if ( ! file.startsWith( dir ) )
         reason = QString( "the settings are stored in %1, outside the sandbox"
                           " %2" ).arg( file ).arg( dir );
   }

   if ( why != nullptr )  *why = reason;

   return reason.isEmpty();
}

/**
 * A self-contained UltraScan3 disk store, built in a temporary directory.
 *
 * The fixture points US_Settings at a scratch directory for the duration of
 * the test and fills it with one project, one buffer, one analyte, one
 * solution, one run (experiment XML, one .auc triple, one edit), one model
 * and one noise record.  That is the whole dependency chain the exporter
 * walks, so a test can export it, import it somewhere else and compare.
 *
 * It refuses to run outside the settings sandbox (datapub_settings_sandboxed()),
 * so that a test never changes the settings of the person running it.
 */
class DataPubTestEnv : public ::testing::Test
{
   protected:
      QString  root;          // The scratch directory of this test
      QString  savedWorkDir;  // The setting to restore afterwards
      bool     sandboxed = false;  // Whether the settings may be changed

      QString  runID;
      QString  runType;
      QString  projectGUID;
      QString  bufferGUID;
      QString  analyteGUID;
      QString  solutionGUID;
      QString  expGUID;
      QString  rawGUID;
      QString  editGUID;
      QString  modelGUID;
      QString  noiseGUID;
      QString  calGUID;
      QString  rawFile;
      QString  editFile;
      QString  editID;

      // A second triple, in cell 2 with a solution of its own; only there
      // after addSecondTriple()
      QString  rawGUID2;
      QString  rawFile2;
      QString  solutionGUID2;
      bool     secondTriple = false;

      void SetUp() override
      {
         QString why;
         sandboxed = datapub_settings_sandboxed( &why );
         ASSERT_TRUE( sandboxed ) << why.toStdString();

         savedWorkDir = US_Settings::workBaseDir();

         root = qEnvironmentVariable( "US3_TEST_SANDBOX", QDir::tempPath() )
                + "/us_datapub_test_"
                + QString::number( QCoreApplication::applicationPid() ) + "_"
                + QString::number( QDateTime::currentMSecsSinceEpoch() );
         QDir().mkpath( root );

         US_Settings::set_workBaseDir( root + "/work" );
         QDir().mkpath( US_Settings::dataDir()   );
         QDir().mkpath( US_Settings::resultDir() );
         QDir().mkpath( US_Settings::tmpDir()    );

         runID   = "demo_run";
         runType = "RA";
         editID  = "2401011200";

         projectGUID  = US_Util::new_guid();
         bufferGUID   = US_Util::new_guid();
         analyteGUID  = US_Util::new_guid();
         solutionGUID = US_Util::new_guid();
         expGUID      = US_Util::new_guid();
         rawGUID      = US_Util::new_guid();
         editGUID     = US_Util::new_guid();
         modelGUID    = US_Util::new_guid();
         noiseGUID    = US_Util::new_guid();
         calGUID      = US_Util::new_guid();

         buildStore();
      }

      void TearDown() override
      {
         if ( ! sandboxed )  return;      // SetUp() changed nothing

         US_Settings::set_workBaseDir( savedWorkDir );

         QDir( root ).removeRecursively();
      }

      // ---- the store ----------------------------------------------------

      /*! Copy the run directory to one of \a newRunID, every file renamed
          to start with it, but with the same GUIDs in it: what a user who
          copies a run directory gets.  With \a renameInXml the experiment
          XML names the copy's run ID, so the copy holds together on its
          own; without, it still names the original run.
      */
      bool copyRunDir( const QString& newRunID, bool renameInXml )
      {
         QString     target = US_Settings::resultDir() + "/" + newRunID;
         QStringList names  = QDir( runDir() ).entryList( QDir::Files );

         if ( ! QDir().mkpath( target ) )  return false;

         for ( int ii = 0; ii < names.size(); ii++ )
         {
            QString name = names[ ii ];
            QString copy = target + "/" + newRunID + name.mid( runID.length() );

            if ( ! name.startsWith( runID + "." )  ||
                 ! QFile::copy( runDir() + "/" + name, copy ) )
               return false;
         }

         if ( ! renameInXml )  return true;

         return set_xml_attribute( target + "/" + newRunID + "." + runType
                                   + ".xml", "experiment", "runID",
                                   newRunID );
      }

   public:
      /*! Set the attribute \a attr of the first element \a element of an
          XML file to \a value, as it is written: no parsing, no escaping
      */
      static bool set_xml_attribute( const QString& path,
                                     const QString& element,
                                     const QString& attr,
                                     const QString& value )
      {
         QFile file( path );

         if ( ! file.open( QIODevice::ReadOnly ) )  return false;

         QString text = QString::fromUtf8( file.readAll() );
         file.close();

         QRegularExpression rx( "(<" + element + "\\b[^>]*\\s" + attr
                                + "=\")[^\"]*(\")" );
         QRegularExpressionMatch match = rx.match( text );

         if ( ! match.hasMatch() )  return false;

         text.replace( match.capturedStart( 0 ), match.capturedLength( 0 ),
                       match.captured( 1 ) + value + match.captured( 2 ) );

         if ( ! file.open( QIODevice::WriteOnly | QIODevice::Truncate ) )
            return false;

         QByteArray bytes = text.toUtf8();
         bool       ok    = ( file.write( bytes ) == bytes.size() );
         file.close();

         return ok;
      }

   protected:
      QString dataDir( const QString& sub )
      {
         QString path = US_Settings::dataDir() + "/" + sub;
         QDir().mkpath( path );
         return path;
      }

      QString runDir()
      {
         QString path = US_Settings::resultDir() + "/" + runID;
         QDir().mkpath( path );
         return path;
      }

      void buildStore()
      {
         writeProject();
         writeBufferAndAnalyte();
         writeSolution();
         writeCalibration();
         writeRawData();
         writeExperimentXml();
         writeEditXml();
         writeModel();
         writeNoise();
         writeTimeState();
      }

      void writeProject()
      {
         US_Project project;
         project.projectID   = 7;
         project.projectGUID = projectGUID;
         project.projectDesc = "Demo publication project";
         project.goals       = "Show that a bundle round-trips";
         project.saveToFile( dataDir( "projects" ) + "/P0000001.xml" );
      }

      void writeBufferAndAnalyte()
      {
         US_Buffer buffer;
         buffer.GUID        = bufferGUID;
         buffer.bufferID    = "3";
         buffer.description = "Demo buffer";
         buffer.density     = 1.0;
         buffer.viscosity   = 1.002;
         buffer.pH          = 7.0;
         buffer.writeToDisk( dataDir( "buffers" ) + "/B0000001.xml" );

         US_Analyte analyte;
         analyte.analyteGUID = analyteGUID;
         analyte.analyteID   = "5";
         analyte.description = "Demo analyte";
         analyte.type        = US_Analyte::PROTEIN;
         analyte.vbar20      = 0.72;
         analyte.mw          = 66000.0;
         analyte.write( false, dataDir( "analytes" ) + "/A0000001.xml" );
      }

      void writeSolution()
      {
         writeSolutionFile( solutionGUID, "Demo solution", "S0000001.xml" );
      }

      void writeSolutionFile( const QString& guid, const QString& desc,
                              const QString& fname )
      {
         US_Solution solution;
         solution.solutionID   = 11;
         solution.solutionGUID = guid;
         solution.solutionDesc = desc;
         solution.commonVbar20 = 0.72;
         solution.storageTemp  = 20.0;
         solution.buffer.GUID        = bufferGUID;
         solution.buffer.bufferID    = "3";
         solution.buffer.description = "Demo buffer";

         US_Solution::AnalyteInfo info;
         info.analyte.analyteGUID = analyteGUID;
         info.analyte.analyteID   = "5";
         info.analyte.description = "Demo analyte";
         info.analyte.vbar20      = 0.72;
         info.analyte.mw          = 66000.0;
         info.amount              = 1.0;
         solution.analyteInfo << info;

         solution.saveToFile( dataDir( "solutions" ) + "/" + fname );
      }

      // A second triple, in cell 2, filled with a solution of its own
      void addSecondTriple()
      {
         rawGUID2      = US_Util::new_guid();
         solutionGUID2 = US_Util::new_guid();
         secondTriple  = true;

         writeSolutionFile( solutionGUID2, "Second solution", "S0000002.xml" );
         rawFile2 = writeRawFile( 2, rawGUID2 );
         writeExperimentXml();
      }

      void writeCalibration()
      {
         US_Rotor::RotorCalibration calibration;
         calibration.ID          = 21;
         calibration.GUID        = calGUID;
         calibration.rotorID     = 2;
         calibration.rotorGUID   = US_Util::new_guid();
         calibration.coeff1      = 1.2e-5;
         calibration.coeff2      = 3.4e-10;
         calibration.label       = "Demo calibration";
         calibration.report      = "Stretch measured on a demo rotor";
         calibration.lastUpdated = QDate( 2024, 1, 1 );

         US_DataPubRecords::writeCalibration( calibration,
               dataDir( "rotors" ) + "/C0000001.xml" );
      }

      void writeRawData()
      {
         rawFile = writeRawFile( 1, rawGUID );
      }

      QString writeRawFile( int cell, const QString& guid )
      {
         US_DataIO::RawData data;
         data.type[ 0 ] = 'R';
         data.type[ 1 ] = 'A';
         US_Util::uuid_parse( guid, (unsigned char*)data.rawGUID );
         data.cell        = cell;
         data.channel     = 'A';
         data.description = "Demo triple";

         for ( int ii = 0; ii < 16; ii++ )
            data.xvalues << 5.8 + 0.01 * ii;

         US_DataIO::Scan scan;
         scan.temperature = 20.0;
         scan.rpm         = 45000.0;
         scan.seconds     = 1000.0;
         scan.omega2t     = 1.0e10;
         scan.wavelength  = 280.0;
         scan.plateau     = 0.5;
         scan.delta_r     = 0.01;
         scan.nz_stddev   = false;

         for ( int ii = 0; ii < data.xvalues.size(); ii++ )
         {
            scan.rvalues << 0.1 * double( ii + 1 );
            scan.stddevs << 0.0;
         }

         scan.interpolated = QByteArray( ( data.xvalues.size() + 7 ) / 8,
                                         char( 0 ) );
         data.scanData << scan;

         QString fname = runID + "." + runType + "." + QString::number( cell )
                         + ".A.280.auc";

         US_DataIO::writeRawData( runDir() + "/" + fname, data );

         return fname;
      }

      void writeDataset( QXmlStreamWriter& xml, const QString& id,
                         const QString& guid, const QString& cell,
                         const QString& centerpiece, const QString& solGUID,
                         const QString& solDesc )
      {
         xml.writeStartElement( "dataset" );
         xml.writeAttribute( "id",         id );
         xml.writeAttribute( "guid",       guid );
         xml.writeAttribute( "cell",       cell );
         xml.writeAttribute( "channel",    "A" );
         xml.writeAttribute( "wavelength", "280" );

         xml.writeStartElement( "centerpiece" );
         xml.writeAttribute( "id", centerpiece );
         xml.writeEndElement();

         xml.writeStartElement( "solution" );
         xml.writeAttribute( "id",   "11" );
         xml.writeAttribute( "guid", solGUID );
         xml.writeAttribute( "desc", solDesc );
         xml.writeEndElement();

         xml.writeEndElement();      // dataset
      }

      void writeExperimentXml()
      {
         QFile file( runDir() + "/" + runID + "." + runType + ".xml" );
         ASSERT_TRUE( file.open( QIODevice::WriteOnly | QIODevice::Text ) );

         QXmlStreamWriter xml( &file );
         xml.setAutoFormatting( true );
         xml.writeStartDocument();
         xml.writeDTD( "<!DOCTYPE US_Scandata>" );
         xml.writeStartElement( "US_Scandata" );
         xml.writeAttribute( "version", "1.0" );

         xml.writeStartElement( "experiment" );
         xml.writeAttribute( "id",    "31" );
         xml.writeAttribute( "guid",  expGUID );
         xml.writeAttribute( "type",  "velocity" );
         xml.writeAttribute( "runID", runID );

         xml.writeStartElement( "project" );
         xml.writeAttribute( "id",   "7" );
         xml.writeAttribute( "guid", projectGUID );
         xml.writeAttribute( "desc", "Demo publication project" );
         xml.writeEndElement();

         xml.writeStartElement( "lab" );
         xml.writeAttribute( "id", "1" );
         xml.writeEndElement();

         xml.writeStartElement( "instrument" );
         xml.writeAttribute( "id",     "1" );
         xml.writeAttribute( "serial", "DEMO-1" );
         xml.writeEndElement();

         xml.writeStartElement( "operator" );
         xml.writeAttribute( "id",   "1" );
         xml.writeAttribute( "guid", US_Util::new_guid() );
         xml.writeEndElement();

         xml.writeStartElement( "rotor" );
         xml.writeAttribute( "id",     "2" );
         xml.writeAttribute( "guid",   US_Util::new_guid() );
         xml.writeAttribute( "serial", "AN50-1" );
         xml.writeAttribute( "name",   "Demo rotor" );
         xml.writeEndElement();

         xml.writeStartElement( "calibration" );
         xml.writeAttribute( "id",     "21" );
         xml.writeAttribute( "coeff1", "1.2e-05" );
         xml.writeAttribute( "coeff2", "3.4e-10" );
         xml.writeAttribute( "date",   "2024-01-01" );
         xml.writeEndElement();

         writeDataset( xml, "41", rawGUID, "1", "2", solutionGUID,
                       "Demo solution" );

         if ( secondTriple )
            writeDataset( xml, "42", rawGUID2, "2", "3", solutionGUID2,
                          "Second solution" );

         xml.writeStartElement( "runTemp" );
         xml.writeAttribute( "value", "20.0" );
         xml.writeEndElement();

         xml.writeStartElement( "opticalSystem" );
         xml.writeAttribute( "value", runType );
         xml.writeEndElement();

         xml.writeTextElement( "label", "Demo experiment label" );

         xml.writeEndElement();      // experiment
         xml.writeEndElement();      // US_Scandata
         xml.writeEndDocument();
         file.close();
      }

      void writeEditXml()
      {
         editFile = runID + "." + editID + "." + runType + ".1.A.280.xml";

         QFile file( runDir() + "/" + editFile );
         ASSERT_TRUE( file.open( QIODevice::WriteOnly | QIODevice::Text ) );

         QXmlStreamWriter xml( &file );
         xml.setAutoFormatting( true );
         xml.writeStartDocument();
         xml.writeDTD( "<!DOCTYPE UltraScanEdits>" );
         xml.writeStartElement( "experiment" );
         xml.writeAttribute( "type", "velocity" );

         xml.writeStartElement( "editGUID" );
         xml.writeAttribute( "value", editGUID );
         xml.writeEndElement();

         xml.writeStartElement( "rawDataGUID" );
         xml.writeAttribute( "value", rawGUID );
         xml.writeEndElement();

         xml.writeStartElement( "parameters" );
         xml.writeAttribute( "meniscus",  "5.85" );
         xml.writeAttribute( "dataLeft",  "5.90" );
         xml.writeAttribute( "dataRight", "5.95" );
         xml.writeEndElement();

         xml.writeEndElement();      // experiment
         xml.writeEndDocument();
         file.close();
      }

      void writeModel()
      {
         US_Model model;
         model.modelGUID   = modelGUID;
         model.editGUID    = editGUID;
         model.description = "demo_run.1A280.2dsa.model";
         model.write( dataDir( "models" ) + "/M0000001.xml" );
      }

      void writeNoise()
      {
         US_Noise noise;
         noise.noiseGUID   = noiseGUID;
         noise.modelGUID   = modelGUID;
         noise.description = "demo_run.1A280.2dsa.ti_noise";
         noise.type        = US_Noise::TI;
         noise.minradius   = 5.8;
         noise.maxradius   = 5.95;

         for ( int ii = 0; ii < 16; ii++ )
            noise.values << 0.001 * double( ii );

         noise.count = noise.values.size();
         noise.write( dataDir( "noises" ) + "/N0000001.xml" );
      }

      // A time state is a pair of files -- the binary readings and the XML
      // that says what the fields in them are.  One is no use without the
      // other, so a run always has both.  Both are what US_TimeState writes
      // for a run: the fields an import writes, one record a second.
      void writeTimeState()
      {
         writeTimeStateFiles( runDir() + "/" + runID + ".time_state.tmst" );
      }

   public:
      //! The number of records writeTimeStateFiles() writes
      static const int timeStateRecords = 5;

      /*! Write a time state as US_TimeState does: \a tmstPath and the
          definitions beside it (".xml" for ".tmst")
      */
      static bool writeTimeStateFiles( const QString& tmstPath )
      {
         US_TimeState tsobj;

         if ( tsobj.open_write_data( tmstPath, 1.0, 0.0 ) != 0 )  return false;

         QStringList keys;
         QStringList formats;
         keys    << "Time" << "RawSpeed" << "SetSpeed" << "Omega2T"
                 << "Temperature" << "Step" << "Scan";
         formats << "I4"   << "F4"       << "I4"       << "F4"
                 << "F4"          << "I2"   << "I2";
         tsobj.set_keys( keys, formats );

         double omega2t = 0.0;

         for ( int ii = 0; ii < timeStateRecords; ii++ )
         {
            double rpm   = 45000.0 - 3.0 * ii;
            double omega = rpm * M_PI / 30.0;
            omega2t     += omega * omega;

            tsobj.set_value( "Time",        ii + 1 );
            tsobj.set_value( "RawSpeed",    rpm );
            tsobj.set_value( "SetSpeed",    45000 );
            tsobj.set_value( "Omega2T",     omega2t );
            tsobj.set_value( "Temperature", 20.0 + 0.01 * ii );
            tsobj.set_value( "Step",        1 );
            tsobj.set_value( "Scan",        ii == 2 ? 1 : 0 );

            if ( tsobj.flush_record() != 0 )  return false;
         }

         if ( tsobj.close_write_data() != 0 )  return false;

         tsobj.setImportType( US_TimeState::IMPORT_TYPE::XLA );

         return tsobj.write_defs( 1.0 ) == 0;
      }

      /*! Whether \a tmstPath, with its definitions beside it, is the time
          state writeTimeStateFiles() writes, as US_TimeState reads it
      */
      static bool isTheTimeState( const QString& tmstPath, QString& why )
      {
         US_TimeState tsobj;

         if ( tsobj.open_read_data( tmstPath, true ) != 0 )
         {
            why = "not readable: " + tsobj.last_error_message();
            return false;
         }

         int count = tsobj.time_count();

         if ( count != timeStateRecords )
         {
            why = QString( "%1 records" ).arg( count );
            return false;
         }

         if ( tsobj.read_record( 2 ) != 0  ||
              tsobj.time_ivalue( "Time" ) != 3  ||
              tsobj.time_ivalue( "Scan" ) != 1  ||
              qAbs( tsobj.time_dvalue( "RawSpeed" ) - 44994.0 ) > 0.01 )
         {
            why = "record 3 does not hold what was written";
            return false;
         }

         tsobj.close_read_data();
         why.clear();

         return true;
      }

   protected:
      QString timeStatePath( const QString& ext )
      {
         return runDir() + "/" + runID + ".time_state." + ext;
      }
};

/**
 * The scratch disk store above, with the default database pointed at the
 * scratch import database (US3_TEST_DB_IMPORT_NAME, see test/README,
 * section 4) -- or, overriding databaseVariable(), another one -- and a
 * connection to it in \c db.  Skips without one.  The database settings
 * and the investigator are put back afterwards.
 */
class DataPubDbTargetEnv : public DataPubTestEnv
{
   protected:
      QString     master;
      QStringList savedDB;
      int         savedInv  = 0;
      bool        restore   = false;
      US_DB2*     db        = nullptr;

      //! The variable naming the database: the scratch import database
      virtual const char* databaseVariable( void ) const
      {
         return "US3_TEST_DB_IMPORT_NAME";
      }

      void SetUp() override
      {
         QString host = qEnvironmentVariable( "US3_TEST_DB_HOST" );
         QString name = qEnvironmentVariable( databaseVariable() );
         QString user = qEnvironmentVariable( "US3_TEST_DB_USER" );
         QString guid = qEnvironmentVariable( "US3_TEST_DB_PERSON_GUID" );

         if ( host.isEmpty()  ||  name.isEmpty()  ||
              user.isEmpty()  ||  guid.isEmpty() )
            GTEST_SKIP() << "no database configured in "
                         << databaseVariable();

         master = "us3-datapub-target";

         QStringList dbCipher   = US_Crypto::encrypt(
               qEnvironmentVariable( "US3_TEST_DB_PASS" ), master );
         QStringList userCipher = US_Crypto::encrypt(
               qEnvironmentVariable( "US3_TEST_DB_PERSON_PW" ), master );

         QStringList entry;
         entry << "us3 import database" << user << name << host
               << dbCipher.at( 0 ) << dbCipher.at( 1 )
               << qEnvironmentVariable( "US3_TEST_DB_EMAIL" )
               << userCipher.at( 0 ) << userCipher.at( 1 ) << guid;

         savedDB  = US_Settings::defaultDB();
         savedInv = US_Settings::us_inv_ID();
         restore  = true;

         US_Settings::set_defaultDB( entry );
         US_Settings::set_us_inv_ID(
               qEnvironmentVariable( "US3_TEST_DB_PERSON_ID" ).toInt() );

         db = new US_DB2();
         QString error;

         if ( ! db->connect( master, error ) )
         {
            delete db;
            db = nullptr;
            US_Settings::set_defaultDB( savedDB );
            US_Settings::set_us_inv_ID( savedInv );
            restore = false;
            GTEST_SKIP() << "cannot reach the database " << name.toStdString()
                         << ": " << error.toStdString();
         }

         // The store on disk comes last: it moves workBaseDir, and the
         // settings above have to be in place whether or not it is built
         DataPubTestEnv::SetUp();
      }

      void TearDown() override
      {
         delete db;
         db = nullptr;

         if ( restore )
         {
            US_Settings::set_defaultDB ( savedDB  );
            US_Settings::set_us_inv_ID ( savedInv );
            restore = false;

            DataPubTestEnv::TearDown();
         }
      }
};
