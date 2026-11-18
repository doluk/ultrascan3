#include <gtest/gtest.h>
#include <QApplication>

#include "us3_gtest_sandbox.h"

int main( int argc, char** argv )
{
   qputenv( "QT_QPA_PLATFORM", "offscreen" );

   // The tests point UltraScan at a scratch store of their own, which means
   // writing the setting that says where that store is.  The process runs in
   // a sandbox so that parallel test processes -- and the person running
   // them -- are left alone.
   if ( ! us3EnterSandbox( "manage-data-tests" ) )
      return 2;

   QApplication app( argc, argv );
   us3ApplySandboxPaths();

   ::testing::InitGoogleTest( &argc, argv );

   return RUN_ALL_TESTS();
}
