// test/common/us_test_settings.cpp
#include "us_test_settings.h"

#include <QCoreApplication>
#include <QDir>
#include <QSettings>

void isolateTestSettings()
{
   const QString dir = QDir::tempPath() + "/us3_test_settings_"
                       + QString::number( QCoreApplication::applicationPid() );

   QDir().mkpath( dir );

   // US_Settings builds QSettings( "US3", "UltraScan" ), which is the
   // native user scope, so that is what is redirected here
   QSettings::setPath( QSettings::NativeFormat, QSettings::UserScope, dir );
   QSettings::setPath( QSettings::IniFormat,    QSettings::UserScope, dir );
}
