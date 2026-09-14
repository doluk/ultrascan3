#include <gtest/gtest.h>
#include <QApplication>

#include "us_test_settings.h"

int main( int argc, char** argv )
{
   qputenv( "QT_QPA_PLATFORM", "offscreen" );

   // The tests point UltraScan at a scratch store of their own, which means
   // writing the setting that says where that store is.  This process gets
   // its own settings file so that parallel test processes -- and the
   // person running them -- are left alone.
   isolateTestSettings();

   QApplication app( argc, argv );

   ::testing::InitGoogleTest( &argc, argv );

   return RUN_ALL_TESTS();
}
