//! \file test_us_convertio_live.cpp
// US_ConvertIO::writeRawDataToDB() against a database with the US3 schema.
//
// Outside a transaction a failed upload is undone by delete_experiment.  That
// procedure passes the run's report ID to delete_report -- NULL for a run
// that has no report yet -- and returns that call's "no such report" status
// although it went on and removed the experiment.  The writer must not then
// report the cleanup as failed.
//
// They need a server with the US3 schema and procedures and skip without
// one.  Set US3_TEST_DB_HOST, US3_TEST_DB_NAME, US3_TEST_DB_USER,
// US3_TEST_DB_PASS, US3_TEST_DB_PERSON_GUID, US3_TEST_DB_PERSON_PW and
// US3_TEST_DB_PERSON_ID, an investigator who owns an experiment there (its
// project, lab, instrument, rotor and operator are reused) and may read a solution.  A missing
// permit for the experiment's operator on its instrument is added for the
// test and removed after it.
// The tests write a new experiment and remove it again.

#include <gtest/gtest.h>

#include "us_convert.h"
#include "us_convertio.h"
#include "us_db2.h"
#include "us_experiment.h"
#include "us_util.h"

#include <QString>
#include <QStringList>
#include <QTemporaryDir>

namespace
{
#ifndef NO_DB
class ConvertIOLive : public ::testing::Test
{
   protected:
      void SetUp() override
      {
         const QString host = qEnvironmentVariable( "US3_TEST_DB_HOST" );
         const QString name = qEnvironmentVariable( "US3_TEST_DB_NAME" );
         const QString user = qEnvironmentVariable( "US3_TEST_DB_USER" );
         const QString pass = qEnvironmentVariable( "US3_TEST_DB_PASS" );
         const QString guid = qEnvironmentVariable( "US3_TEST_DB_PERSON_GUID" );
         const QString pw   = qEnvironmentVariable( "US3_TEST_DB_PERSON_PW" );
         personID           = qEnvironmentVariableIntValue( "US3_TEST_DB_PERSON_ID" );

         if ( host.isEmpty() || name.isEmpty() || user.isEmpty() || guid.isEmpty() || personID <= 0 )
         {
            GTEST_SKIP() << "no test database configured";
         }

         QString error;
         ASSERT_TRUE( db.connectAuthenticated( host, name, user, pass, guid, pw, error ) ) << error.toStdString();

         db.rawQuery( QString( "SELECT e.projectID, e.labID, e.instrumentID, e.rotorID, e.rotorCalibrationID, e.operatorID "
                               "FROM experiment e JOIN experimentPerson ep ON ep.experimentID = e.experimentID "
                               "WHERE ep.personID = %1 AND e.rotorID IS NOT NULL ORDER BY e.experimentID LIMIT 1" )
                         .arg( personID ) );
         if ( !db.next() )
         {
            GTEST_SKIP() << "the investigator owns no experiment to take the hardware from";
         }
         for ( int ii = 0; ii < 6; ii++ )
            hardware << db.value( ii ).toString();

         // new_experiment wants a permit for the operator on the instrument,
         // which a database filled by hand may lack; one is added for the
         // test and taken away again
         db.rawQuery( QString( "SELECT COUNT(*) FROM permits WHERE personID = %1 AND instrumentID = %2" )
                         .arg( hardware[ 5 ], hardware[ 2 ] ) );
         if ( db.next() && db.value( 0 ).toInt() == 0 )
         {
            db.rawQuery( QString( "INSERT INTO permits ( personID, collaboratorID, instrumentID ) "
                                  "VALUES ( %1, %1, %2 )" )
                            .arg( hardware[ 5 ], hardware[ 2 ] ) );
            ASSERT_FALSE( db.lastError().startsWith( "MySQL error" ) ) << db.lastError().toStdString();
            addedPermit = true;
         }

         db.rawQuery( "SELECT solutionGUID FROM solution ORDER BY solutionID LIMIT 1" );
         if ( !db.next() )
         {
            GTEST_SKIP() << "no solution in the database";
         }
         solutionGUID = db.value( 0 ).toString();
      }

      void TearDown() override
      {
         for ( int id : created )
         {
            if ( experimentExists( id ) )
               db.statusQuery( QStringList() << "delete_experiment" << QString::number( id ) );
         }
         if ( addedPermit )
         {
            db.rawQuery( QString( "DELETE FROM permits WHERE personID = %1 AND instrumentID = %2" )
                            .arg( hardware[ 5 ], hardware[ 2 ] ) );
         }
      }

      // A new experiment of the investigator's, named runID
      int newExperiment( const QString& runID )
      {
         QStringList q( "new_experiment" );
         q << US_Util::new_guid() << hardware[ 0 ] << runID << hardware[ 1 ] << hardware[ 2 ]
           << hardware[ 5 ] << hardware[ 3 ] << hardware[ 4 ] << "velocity" << "RA" << "" << "20"
           << runID << "" << "" << QString::number( personID );
         const int status = db.statusQuery( q );
         EXPECT_EQ( status, IUS_DB2::OK ) << db.lastError().toStdString();
         const int id = status == IUS_DB2::OK ? db.lastInsertID() : 0;
         if ( id > 0 )
            created << id;
         return id;
      }

      bool experimentExists( int id )
      {
         db.query( QStringList() << "get_experiment_info" << QString::number( id ) );
         return db.lastErrno() == IUS_DB2::OK;
      }

      US_DB2      db;
      int         personID = 0;
      QStringList hardware;   // project, lab, instrument, rotor, calibration, operator
      QString     solutionGUID;
      QList<int>  created;
      bool        addedPermit = false;
};

// What the writer's check relies on: the server's own answer for a run
// without a report, on unpatched and patched procedures alike
TEST_F( ConvertIOLive, DeleteExperimentReportsAMissingReportButRemovesTheRun )
{
   const int id = newExperiment( "cvio_live_" + US_Util::new_guid().left( 8 ) );
   ASSERT_GT( id, 0 );
   ASSERT_TRUE( experimentExists( id ) );

   const int status = db.statusQuery( QStringList() << "delete_experiment" << QString::number( id ) );
   EXPECT_EQ( status, IUS_DB2::NO_REPORT ) << db.lastError().toStdString();
   EXPECT_FALSE( experimentExists( id ) );
   EXPECT_EQ( db.lastErrno(), IUS_DB2::NO_EXPERIMENT );
}

// A failed upload outside a transaction: the cleanup removed the run, so
// only the upload's failure is reported
TEST_F( ConvertIOLive, ACleanupThatRemovedTheRunIsNotReportedAsFailed )
{
   const QString runID = "cvio_live_" + US_Util::new_guid().left( 8 );
   const int     id    = newExperiment( runID );
   ASSERT_GT( id, 0 );

   US_Experiment experiment;
   experiment.clear();
   experiment.expID    = id;
   experiment.invID    = personID;
   experiment.runID    = runID;
   experiment.label    = runID;
   experiment.comments = "";

   US_Convert::TripleInfo triple;
   triple.clear();
   triple.tripleDesc            = "1 / A / 280";
   triple.tripleFilename        = runID + ".RA.1.A.280.auc";   // not there: the upload fails
   triple.excluded              = false;
   triple.centerpiece           = 1;
   triple.solution.solutionGUID = solutionGUID;
   US_Util::uuid_parse( US_Util::new_guid(), (unsigned char*) triple.tripleGUID );
   QList<US_Convert::TripleInfo> triples{ triple };

   QTemporaryDir dir;
   ASSERT_TRUE( dir.isValid() );
   ASSERT_FALSE( db.transactionActive() );

   const QString error = US_ConvertIO::writeRawDataToDB( experiment, triples, dir.path() + "/", &db );

   EXPECT_TRUE( error.contains( triple.tripleFilename ) ) << error.toStdString();
   EXPECT_FALSE( error.contains( "deleting experiment" ) ) << error.toStdString();
   EXPECT_FALSE( experimentExists( id ) ) << "the cleanup left the run";
}
#endif
} // namespace
