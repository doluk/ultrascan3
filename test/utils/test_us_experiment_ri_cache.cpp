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
}
