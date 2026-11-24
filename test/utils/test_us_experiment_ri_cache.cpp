//! \file test_us_experiment_ri_cache.cpp
// US_Experiment::readFromDB and the local copy of an RI intensity profile:
// written by default, as it always was, and not when the caller says not to.
#include "mock_us_db2.h"
#include "qt_test_base.h"
#include "us_experiment.h"
#include "us_settings.h"

#include <QTemporaryDir>

using ::testing::_;
using ::testing::An;
using ::testing::Invoke;
using ::testing::NiceMock;
using ::testing::Return;

namespace
{
const QString kRun = "RI_run";
const QString kXml = "<?xml version=\"1.0\"?><!DOCTYPE US_RIProfile><US_RIProfile>"
                     "<RI value=\"0.5\"/><RI value=\"0.75\"/></US_RIProfile>";

// A server with one RI experiment and nothing else
class RiServer
{
   public:
      bool expRow = false;
      int  row    = -1;

      void wire( NiceMock<US_DB2_Mock>& db )
      {
         ON_CALL( db, isConnected() ).WillByDefault( Return( true ) );
         ON_CALL( db, lastErrno() ).WillByDefault( Return( IUS_DB2::OK ) );
         ON_CALL( db, query( An<const QStringList&>() ) )
            .WillByDefault( Invoke( [ this ]( const QStringList& q )
            {
               expRow = ( q.value( 0 ) == "get_experiment_info_by_runID" );
               row    = -1;
            } ) );
         ON_CALL( db, next() )
            .WillByDefault( Invoke( [ this ]() { return expRow && ++row == 0; } ) );
         ON_CALL( db, value( _ ) )
            .WillByDefault( Invoke( [ this ]( unsigned ii ) -> QVariant
            {
               if ( ! expRow || row != 0 )  return QVariant();
               if ( ii == 15 )  return QVariant( QString( "RI" ) );
               if ( ii == 16 )  return QVariant( kXml );
               if ( ii == 1 )   return QVariant( QString( "31" ) );
               return QVariant( QString( "0" ) );
            } ) );
      }
};
}

class ExperimentRiCache : public QtTestBase
{
   protected:
      QTemporaryDir scratch;
      QString       savedWorkDir;
      QString       profile;

      void SetUp() override
      {
         QtTestBase::SetUp();
         ASSERT_TRUE( scratch.isValid() );
         savedWorkDir = US_Settings::workBaseDir();
         US_Settings::set_workBaseDir( scratch.path() );

         QDir().mkpath( US_Settings::resultDir() + "/" + kRun );
         profile = US_Settings::resultDir() + "/" + kRun + "/" + kRun + ".RIProfile.xml";
      }

      void TearDown() override
      {
         US_Settings::set_workBaseDir( savedWorkDir );
         QtTestBase::TearDown();
      }
};

TEST_F( ExperimentRiCache, TheProfileIsCopiedLocallyByDefault )
{
   NiceMock<US_DB2_Mock>    db;
   RiServer                 server;
   US_Experiment            experiment;
   QVector<SP_SPEEDPROFILE> speedsteps;
   server.wire( db );

   ASSERT_EQ( experiment.readFromDB( kRun, &db, speedsteps, "17" ), IUS_DB2::OK );
   EXPECT_EQ( experiment.RIProfile.size(), 2 );
   EXPECT_TRUE( QFile::exists( profile ) );
}

TEST_F( ExperimentRiCache, AReadOnlyCallerLeavesTheResultsDirectoryAlone )
{
   NiceMock<US_DB2_Mock>    db;
   RiServer                 server;
   US_Experiment            experiment;
   QVector<SP_SPEEDPROFILE> speedsteps;
   server.wire( db );

   ASSERT_EQ( experiment.readFromDB( kRun, &db, speedsteps, "17", false ), IUS_DB2::OK );
   EXPECT_EQ   ( experiment.RIProfile.size(), 2 ) << "the profile is still read";
   EXPECT_FALSE( QFile::exists( profile ) );

   // and its XML is to be had without a file
   QByteArray xml = experiment.riProfileXml();
   EXPECT_TRUE ( xml.contains( "US_RIProfile" ) );
   EXPECT_FALSE( QFile::exists( profile ) );
   EXPECT_EQ   ( xml.count( "<RI " ), 2 ) << xml.toStdString();
}

// ------------------------------------------------- the rotor calibration

namespace
{
// A server with one experiment of rotor 5 that names no calibration, and a
// rotor whose first calibration is 44
class CalibrationServer
{
   public:
      QString     proc;
      int         row = -1;
      QStringList calls;
      bool        noCalibrationRow = false;   //!< get_rotor_calibration_info finds nothing

      QList< QVariantList > rows( void ) const
      {
         if ( proc == "get_experiment_info_by_runID" )
         {
            QVariantList r;
            while ( r.size() < 17 )  r << QVariant( QString( "0" ) );
            r[ 1 ]  = 31;           // experimentID
            r[ 6 ]  = 5;            // rotorID
            r[ 7 ]  = 0;            // rotorCalibrationID: none recorded
            r[ 15 ] = "RA";
            r[ 16 ] = "";
            return QList< QVariantList >() << r;
         }

         if ( proc == "get_rotor_calibration_profiles" )
            return QList< QVariantList >() << ( QVariantList() << 44 );

         if ( proc == "get_rotor_calibration_info"  &&  ! noCalibrationRow )
            return QList< QVariantList >() << ( QVariantList() << 44 << "" << "" << ""
                                                << "1.5e-4" << "2.5e-8" << "" << "2020-01-02 03:04:05" );

         return QList< QVariantList >();
      }

      void wire( NiceMock<US_DB2_Mock>& db )
      {
         ON_CALL( db, isConnected() ).WillByDefault( Return( true ) );
         ON_CALL( db, lastErrno() ).WillByDefault( Return( IUS_DB2::OK ) );
         ON_CALL( db, query( An<const QStringList&>() ) )
            .WillByDefault( Invoke( [ this ]( const QStringList& q )
            {
               proc = q.value( 0 );
               row  = -1;
               calls << proc;
            } ) );
         ON_CALL( db, next() )
            .WillByDefault( Invoke( [ this ]() { return ++row < rows().size(); } ) );
         ON_CALL( db, value( _ ) )
            .WillByDefault( Invoke( [ this ]( unsigned ii ) -> QVariant
            {
               QList< QVariantList > all = rows();
               if ( row < 0  ||  row >= all.size() )  return QVariant();
               return all[ row ].value( int( ii ) );
            } ) );
      }
};
}

// Programs that analyse or save a run have always been given the rotor's
// first calibration when the experiment names none
TEST( ExperimentCalibration, TheRotorsFirstCalibrationIsTakenByDefault )
{
   NiceMock<US_DB2_Mock>    db;
   CalibrationServer        server;
   US_Experiment            experiment;
   QVector<SP_SPEEDPROFILE> speedsteps;
   server.wire( db );

   ASSERT_EQ( experiment.readFromDB( "Cal_run", &db, speedsteps, "17", false ), IUS_DB2::OK );
   EXPECT_EQ( experiment.calibrationID, 44 );
   EXPECT_FLOAT_EQ( experiment.rotorCoeff1, 1.5e-4f );
   EXPECT_TRUE( server.calls.contains( "get_rotor_calibration_profiles" ) );
}

// A caller that records what the run used gets no calibration in its place
TEST( ExperimentCalibration, NoCalibrationIsMadeUpWhenAskedNotTo )
{
   NiceMock<US_DB2_Mock>    db;
   CalibrationServer        server;
   US_Experiment            experiment;
   QVector<SP_SPEEDPROFILE> speedsteps;
   server.wire( db );

   experiment.rotorCoeff1 = 9.0;      // left over from an earlier read

   ASSERT_EQ( experiment.readFromDB( "Cal_run", &db, speedsteps, "17", false, false ),
              IUS_DB2::OK );
   EXPECT_EQ   ( experiment.calibrationID, 0 );
   EXPECT_FLOAT_EQ( experiment.rotorCoeff1, 0.0f );
   EXPECT_FLOAT_EQ( experiment.rotorCoeff2, 0.0f );
   EXPECT_FALSE( server.calls.contains( "get_rotor_calibration_profiles" ) );
   EXPECT_FALSE( server.calls.contains( "get_rotor_calibration_info" ) );
}

// A calibration the experiment names but the server no longer has leaves no
// coefficients from an earlier read, and a missing rotor no name
TEST( ExperimentCalibration, AMissingCalibrationRowLeavesNoStaleCoefficients )
{
   NiceMock<US_DB2_Mock>    db;
   CalibrationServer        server;
   US_Experiment            experiment;
   QVector<SP_SPEEDPROFILE> speedsteps;
   server.wire( db );
   server.noCalibrationRow = true;

   experiment.rotorCoeff1  = 9.0;      // left over from an earlier read
   experiment.rotorCoeff2  = 8.0;
   experiment.rotorName    = "old rotor";
   experiment.rotorUpdated = QDate( 2001, 2, 3 );

   ASSERT_EQ( experiment.readFromDB( "Cal_run", &db, speedsteps, "17", false ), IUS_DB2::OK );
   EXPECT_EQ( experiment.calibrationID, 44 );
   EXPECT_FLOAT_EQ( experiment.rotorCoeff1, 0.0f );
   EXPECT_FLOAT_EQ( experiment.rotorCoeff2, 0.0f );
   EXPECT_FALSE( experiment.rotorUpdated.isValid() );
   EXPECT_TRUE( experiment.rotorName.isEmpty() );
}

// An investigator with no person row is no investigator: the GUID of the
// one an earlier read found is not kept, as the operator's is not
TEST( ExperimentCalibration, AnInvestigatorWithNoPersonRowLeavesNoGuid )
{
   NiceMock<US_DB2_Mock>    db;
   CalibrationServer        server;
   US_Experiment            experiment;
   QVector<SP_SPEEDPROFILE> speedsteps;
   server.wire( db );

   experiment.invGUID      = "aaaaaaaa-0000-4000-8000-00000000dead";   // an earlier read's
   experiment.operatorGUID = "aaaaaaaa-0000-4000-8000-00000000beef";

   ASSERT_EQ( experiment.readFromDB( "Cal_run", &db, speedsteps, "17", false ), IUS_DB2::OK );
   EXPECT_TRUE( server.calls.contains( "get_person_info" ) );
   EXPECT_TRUE( experiment.invGUID.isEmpty() ) << experiment.invGUID.toStdString();
   EXPECT_TRUE( experiment.operatorGUID.isEmpty() ) << experiment.operatorGUID.toStdString();
}
