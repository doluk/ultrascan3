//! \file us3_gtest_sandbox.h
#pragma once

#include <QDir>
#include <QFile>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <memory>

#include "us_settings.h"

/**
 * Process-wide sandbox for gtest program suites, the gtest counterpart of
 * US3_GUI_TEST_MAIN.
 *
 * Call us3EnterSandbox() from main, before the Q(Core)Application is created
 * and before anything reads US_Settings: US_SettingsStore caches the
 * US3_SETTINGS_ROOT override on first use.  Call us3ApplySandboxPaths() once
 * the application object exists.  The sandbox directory is removed when the
 * process exits normally.
 */
inline std::unique_ptr<QTemporaryDir>& us3SandboxDir()
{
   static std::unique_ptr<QTemporaryDir> dir;
   return dir;
}

inline bool us3EnterSandbox( const QString& tag )
{
   QStandardPaths::setTestModeEnabled( true );

   auto& dir = us3SandboxDir();
   dir.reset( new QTemporaryDir( QDir::tempPath() + "/ultrascan3-" + tag + "-XXXXXX" ) );
   if ( ! dir->isValid() )
      return false;

   const QString root          = dir->path();
   const QString settingsRoot  = root + "/settings";
   const QString workRoot      = root + "/work";

   if ( ! QDir().mkpath( settingsRoot )
        ||  ! QDir().mkpath( workRoot + "/imports" )
        ||  ! QDir().mkpath( workRoot + "/tmp" ) )
      return false;

   qputenv( "US3_TEST_SANDBOX",       QFile::encodeName( root ) );
   qputenv( "US3_TEST_SETTINGS_ROOT", QFile::encodeName( settingsRoot ) );
   qputenv( "US3_TEST_WORK_ROOT",     QFile::encodeName( workRoot ) );
   qputenv( "US3_SETTINGS_ROOT",      QFile::encodeName( settingsRoot ) );

   QSettings::setPath( QSettings::IniFormat, QSettings::UserScope,   settingsRoot );
   QSettings::setPath( QSettings::IniFormat, QSettings::SystemScope, settingsRoot );
   return true;
}

inline void us3ApplySandboxPaths()
{
   const QString workRoot = qEnvironmentVariable( "US3_TEST_WORK_ROOT" );
   US_Settings::set_workBaseDir( workRoot );
   US_Settings::set_importDir  ( workRoot + "/imports" );
   US_Settings::set_tmpDir     ( workRoot + "/tmp" );
}
