// test/programs/us_data_publication/test_us_datapub_main.cpp
#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QDir>
#include <QSettings>
#include <QStandardPaths>

/**
 * The tests write to an UltraScan3 disk store of their own, which means
 * they also write the "workBaseDir" setting that says where that store is.
 * QSettings is redirected into a scratch directory first so that a test run
 * never touches the settings of the person running it.
 */
class DataPubEnvironment : public ::testing::Environment
{
   public:
      void SetUp() override
      {
         qputenv( "QT_QPA_PLATFORM", "offscreen" );
         qputenv( "QT_LOGGING_RULES", "*.debug=false" );

         settingsDir = QDir::tempPath() + "/us_datapub_settings_"
                       + QString::number( QCoreApplication::applicationPid() );
         QDir().mkpath( settingsDir );

         QSettings::setPath( QSettings::NativeFormat, QSettings::UserScope,
                             settingsDir );
         QSettings::setPath( QSettings::IniFormat, QSettings::UserScope,
                             settingsDir );
      }

      void TearDown() override
      {
         if ( ! settingsDir.isEmpty() )
            QDir( settingsDir ).removeRecursively();
      }

   private:
      QString settingsDir;
};

int main( int argc, char** argv )
{
   qputenv( "QT_QPA_PLATFORM", "offscreen" );

   QCoreApplication app( argc, argv );
   QCoreApplication::setOrganizationName( "UltraScan3" );
   QCoreApplication::setApplicationName ( "us_datapub_tests" );

   ::testing::InitGoogleTest( &argc, argv );
   ::testing::AddGlobalTestEnvironment( new DataPubEnvironment );

   return RUN_ALL_TESTS();
}
