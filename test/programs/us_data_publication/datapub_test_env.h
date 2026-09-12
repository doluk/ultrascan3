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

#include "us_datapub_records.h"

/**
 * A self-contained UltraScan3 disk store, built in a temporary directory.
 *
 * The fixture points US_Settings at a scratch directory for the duration of
 * the test and fills it with one project, one buffer, one analyte, one
 * solution, one run (experiment XML, one .auc triple, one edit), one model
 * and one noise record.  That is the whole dependency chain the exporter
 * walks, so a test can export it, import it somewhere else and compare.
 */
class DataPubTestEnv : public ::testing::Test
{
   protected:
      QString  root;          // The scratch directory of this test
      QString  savedWorkDir;  // The setting to restore afterwards

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

      void SetUp() override
      {
         savedWorkDir = US_Settings::workBaseDir();

         root = QDir::tempPath() + "/us_datapub_test_"
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
         US_Settings::set_workBaseDir( savedWorkDir );

         QDir( root ).removeRecursively();
      }

      // ---- the store ----------------------------------------------------

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
         US_Solution solution;
         solution.solutionID   = 11;
         solution.solutionGUID = solutionGUID;
         solution.solutionDesc = "Demo solution";
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

         solution.saveToFile( dataDir( "solutions" ) + "/S0000001.xml" );
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
         US_DataIO::RawData data;
         data.type[ 0 ] = 'R';
         data.type[ 1 ] = 'A';
         US_Util::uuid_parse( rawGUID, (unsigned char*)data.rawGUID );
         data.cell        = 1;
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

         rawFile = runID + "." + runType + ".1.A.280.auc";

         US_DataIO::writeRawData( runDir() + "/" + rawFile, data );
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

         xml.writeStartElement( "dataset" );
         xml.writeAttribute( "id",         "41" );
         xml.writeAttribute( "guid",       rawGUID );
         xml.writeAttribute( "cell",       "1" );
         xml.writeAttribute( "channel",    "A" );
         xml.writeAttribute( "wavelength", "280" );

         xml.writeStartElement( "centerpiece" );
         xml.writeAttribute( "id", "2" );
         xml.writeEndElement();

         xml.writeStartElement( "solution" );
         xml.writeAttribute( "id",   "11" );
         xml.writeAttribute( "guid", solutionGUID );
         xml.writeAttribute( "desc", "Demo solution" );
         xml.writeEndElement();

         xml.writeEndElement();      // dataset

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
};
