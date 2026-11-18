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
