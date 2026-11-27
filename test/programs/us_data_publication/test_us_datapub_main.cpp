// test/programs/us_data_publication/test_us_datapub_main.cpp
#include <gtest/gtest.h>

#include <QCoreApplication>

#include "us3_gtest_sandbox.h"

/**
 * The tests write to an UltraScan3 disk store of their own, which means
 * they also write the "workBaseDir" setting that says where that store is.
 * The process runs in a sandbox (US3_SETTINGS_ROOT) so that a test run
 * never touches the settings of the person running it.
 */
int main( int argc, char** argv )
{
   qputenv( "QT_QPA_PLATFORM", "offscreen" );
   qputenv( "QT_LOGGING_RULES", "*.debug=false" );

   if ( ! us3EnterSandbox( "datapub-tests" ) )
      return 2;

   QCoreApplication app( argc, argv );
   QCoreApplication::setOrganizationName( "UltraScan3" );
   QCoreApplication::setApplicationName ( "us_datapub_tests" );
   us3ApplySandboxPaths();

   ::testing::InitGoogleTest( &argc, argv );

   return RUN_ALL_TESTS();
}
