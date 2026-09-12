//! \file us_datapub_bundle.cpp
#include "us_datapub_bundle.h"

#include "us_archive.h"
#include "us_settings.h"

US_DataPubBundle::US_DataPubBundle()
{
   keep = false;
}

US_DataPubBundle::~US_DataPubBundle()
{
   if ( ! keep )  cleanup();
}

QString US_DataPubBundle::manifestName( void )
{
   return QString( "manifest.yaml" );
}

bool US_DataPubBundle::removeTree( const QString& path )
{
   if ( path.isEmpty() )  return true;

   QDir dir( path );

   if ( ! dir.exists() )  return true;

   return dir.removeRecursively();
}

void US_DataPubBundle::cleanup( void )
{
   removeTree( staging );
   staging.clear();
   root   .clear();
}

void US_DataPubBundle::setKeepStaging( bool a_keep )
{
   keep = a_keep;
}

QString US_DataPubBundle::stagingPath( void ) const
{
   return staging;
}

QString US_DataPubBundle::rootPath( void ) const
{
   return root;
}

bool US_DataPubBundle::createStaging( QString& error )
{
   cleanup();

   QString base = US_Settings::tmpDir() + "/us_datapub";

   if ( ! QDir().mkpath( base ) )
   {
      error = QObject::tr( "Cannot create the work directory %1" ).arg( base );
      return false;
   }

   QString name = QString( "bundle_%1_%2" )
                  .arg( QCoreApplication::applicationPid() )
                  .arg( QDateTime::currentDateTime()
                        .toString( "yyyyMMddhhmmsszzz" ) );

   staging = base + "/" + name;

   if ( ! QDir().mkpath( staging ) )
   {
      error   = QObject::tr( "Cannot create the staging directory %1" )
                .arg( staging );
      staging.clear();
      return false;
   }

   root = staging;

   error.clear();

   return true;
}

QString US_DataPubBundle::stagedPath( const QString& relativePath )
{
   if ( staging.isEmpty() )  return QString();

   QString  path = staging + "/" + relativePath;
   QFileInfo info( path );

   QDir().mkpath( info.absolutePath() );

   return path;
}

bool US_DataPubBundle::pack( const QString& archivePath, QString& error )
{
   if ( staging.isEmpty() )
   {
      error = QObject::tr( "There is nothing staged to pack" );
      return false;
   }

   QStringList entries = QDir( staging ).entryList(
                         QDir::AllEntries | QDir::NoDotAndDotDot, QDir::Name );

   if ( entries.isEmpty() )
   {
      error = QObject::tr( "The staged bundle is empty" );
      return false;
   }

   QStringList sources;

   for ( int ii = 0; ii < entries.size(); ii++ )
      sources << staging + "/" + entries[ ii ];

   // US_Archive picks the compression from everything after the first dot
   // of the file name, so pack under a plain name and move afterwards.
   QString workDir = staging + "_pack";

   if ( ! QDir().mkpath( workDir ) )
   {
      error = QObject::tr( "Cannot create the packing directory %1" )
              .arg( workDir );
      return false;
   }

   QString    workFile = workDir + "/bundle.tar.gz";
   US_Archive archive;

   if ( ! archive.compress( sources, workFile ) )
   {
      error = archive.getError();
      removeTree( workDir );
      return false;
   }

   QFileInfo target( archivePath );

   if ( ! QDir().mkpath( target.absolutePath() ) )
   {
      error = QObject::tr( "Cannot create the output directory %1" )
              .arg( target.absolutePath() );
      removeTree( workDir );
      return false;
   }

   if ( QFile::exists( archivePath )  &&  ! QFile::remove( archivePath ) )
   {
      error = QObject::tr( "Cannot replace the existing file %1" )
              .arg( archivePath );
      removeTree( workDir );
      return false;
   }

   bool moved = QFile::rename( workFile, archivePath );

   if ( ! moved )
   {  // rename fails across file systems; fall back to a copy
      moved = QFile::copy( workFile, archivePath );
   }

   removeTree( workDir );

   if ( ! moved )
   {
      error = QObject::tr( "Cannot write the bundle to %1" ).arg( archivePath );
      return false;
   }

   error.clear();

   return true;
}

bool US_DataPubBundle::unpack( const QString& archivePath, QString& error )
{
   if ( ! QFile::exists( archivePath ) )
   {
      error = QObject::tr( "The bundle %1 does not exist" ).arg( archivePath );
      return false;
   }

   if ( ! createStaging( error ) )  return false;

   US_Archive archive;

   if ( ! archive.extract( archivePath, staging ) )
   {
      error = archive.getError();
      return false;
   }

   root = staging;

   if ( QFile::exists( staging + "/" + manifestName() ) )
   {
      error.clear();
      return true;
   }

   // Allow a bundle that carries one wrapping directory
   QStringList subdirs = QDir( staging ).entryList(
                         QDir::AllDirs | QDir::NoDotAndDotDot, QDir::Name );

   for ( int ii = 0; ii < subdirs.size(); ii++ )
   {
      QString path = staging + "/" + subdirs[ ii ];

      if ( ! QFile::exists( path + "/" + manifestName() ) )  continue;

      root = path;
      error.clear();
      return true;
   }

   error = QObject::tr( "The bundle %1 holds no %2" )
           .arg( archivePath ).arg( manifestName() );

   return false;
}
