// test_us_solution_file.cpp - US_Solution::readFromFile is all or nothing
#include "qt_test_base.h"

#include <QtCore>

#include "us_solution.h"
#include "us_settings.h"
#include "us_util.h"
#include "ius_db2.h"

class TestUSSolutionFile : public QtTestBase
{
   protected:
      QTemporaryDir* scratch = nullptr;
      QString        savedWorkDir;

      void SetUp() override
      {
         QtTestBase::SetUp();
         scratch = new QTemporaryDir();
         ASSERT_TRUE( scratch->isValid() );

         // An empty local store: no buffer or analyte file can be found
         savedWorkDir = US_Settings::workBaseDir();
         US_Settings::set_workBaseDir( scratch->path() );
      }

      void TearDown() override
      {
         US_Settings::set_workBaseDir( savedWorkDir );
         delete scratch;
         scratch = nullptr;
         QtTestBase::TearDown();
      }

      // A solution file whose buffer is nowhere in the local store
      QString writeSolution( const QString& name, const QString& desc )
      {
         US_Solution solution;
         solution.solutionGUID = US_Util::new_guid();
         solution.solutionDesc = desc;
         solution.commonVbar20 = 0.72;
         solution.buffer.GUID  = US_Util::new_guid();
         solution.buffer.description = "nowhere";

         QString path = scratch->path() + "/" + name;
         EXPECT_TRUE( solution.saveToFile( path ) );
         return path;
      }

      // A solution already loaded, the way a caller would hold one
      US_Solution loadedSolution( void )
      {
         US_Solution held;
         EXPECT_EQ( held.readFromFile( writeSolution( "held.xml", "held" ), false ),
                    IUS_DB2::OK );
         EXPECT_EQ( held.solutionDesc, QString( "held" ) );
         return held;
      }
};

TEST_F( TestUSSolutionFile, AFileIsReadWithoutItsReferences )
{
   US_Solution solution;
   QString     path = writeSolution( "plain.xml", "plain solution" );

   ASSERT_EQ( solution.readFromFile( path, false ), IUS_DB2::OK );
   EXPECT_EQ( solution.solutionDesc, QString( "plain solution" ) );
   EXPECT_DOUBLE_EQ( solution.commonVbar20, 0.72 );
}

// The file parses, but its buffer cannot be found.  The solution the
// caller held must survive that, rather than turn into the half-read one.
TEST_F( TestUSSolutionFile, AMissingBufferLeavesTheSolutionAsItWas )
{
   US_Solution held    = loadedSolution();
   QString     heldGUID = held.solutionGUID;

   EXPECT_NE( held.readFromFile( writeSolution( "orphan.xml", "half read" ), true ),
              IUS_DB2::OK );

   EXPECT_EQ( held.solutionDesc, QString( "held" ) );
   EXPECT_EQ( held.solutionGUID, heldGUID );
}

TEST_F( TestUSSolutionFile, ABrokenFileLeavesTheSolutionAsItWas )
{
   US_Solution held     = loadedSolution();
   QString     heldGUID = held.solutionGUID;

   QString path = scratch->path() + "/broken.xml";
   QFile   ff( path );
   ASSERT_TRUE( ff.open( QIODevice::WriteOnly | QIODevice::Text ) );
   ff.write( "<?xml version=\"1.0\"?>\n<SolutionData>\n"
             "<solution id=\"5\" guid=\"aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee\">\n"
             "<description>broken</description>\n<buffer id=\"1\" " );
   ff.close();

   EXPECT_NE( held.readFromFile( path, false ), IUS_DB2::OK );
   EXPECT_EQ( held.solutionDesc, QString( "held" ) );
   EXPECT_EQ( held.solutionGUID, heldGUID );

   EXPECT_NE( held.readFromFile( scratch->path() + "/absent.xml", false ),
              IUS_DB2::OK );
   EXPECT_EQ( held.solutionGUID, heldGUID );
}

// Copies carry every member, by construction and by assignment
TEST_F( TestUSSolutionFile, CopiesCarryEveryMember )
{
   US_Solution original;
   original.solutionID   = 9;
   original.solutionGUID = "guid-9";
   original.solutionDesc = "nine";
   original.notes        = "notes";
   original.storageTemp  = 4.0;
   original.saveStatus   = US_Solution::BOTH;
   original.extinction[ 280.0 ] = 1.5;
   US_Solution::AnalyteInfo info;
   info.amount = 2.0;
   info.analyte.analyteGUID = "an-1";
   original.analyteInfo << info;

   US_Solution built( original );
   US_Solution assigned;
   assigned.analyteInfo << info << info;      // replaced, not appended to
   assigned = original;

   for ( const US_Solution* copy : { &built, &assigned } )
   {
      EXPECT_EQ( copy->solutionID,   9 );
      EXPECT_EQ( copy->solutionGUID, QString( "guid-9" ) );
      EXPECT_EQ( copy->notes,        QString( "notes" ) );
      EXPECT_EQ( copy->saveStatus,   US_Solution::BOTH );
      EXPECT_EQ( copy->extinction.value( 280.0 ), 1.5 );
      ASSERT_EQ( copy->analyteInfo.size(), 1 );
      EXPECT_EQ( copy->analyteInfo[ 0 ].analyte.analyteGUID, QString( "an-1" ) );
   }
}
