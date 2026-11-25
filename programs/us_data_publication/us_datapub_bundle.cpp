//! \file us_datapub_bundle.cpp
#include "us_datapub_bundle.h"
#include "us_datapub_defs.h"
#include "us_datapub_hash.h"

#include "us_archive.h"
#include "us_settings.h"

US_DataPubBundle::US_DataPubBundle()
{
   keep = false;
   lock = nullptr;
}

US_DataPubBundle::~US_DataPubBundle()
{
   if ( ! keep )  cleanup();

   delete lock;                    // A kept directory loses its lock
   lock = nullptr;
}

void US_DataPubBundle::setStagingLock( const QString& name )
{
   lock_name = name;
}

bool US_DataPubBundle::stagingLocked( void ) const
{
   return lock != nullptr  &&  lock->isLocked();
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
   if ( lock == nullptr )
      removeTree( base );

   else
   {  // Everything but the lock, then the lock, then the directory
      QString     lockFile = lock_name;
      QStringList entries  = QDir( base ).entryList( QDir::AllEntries
                                                     | QDir::Hidden
                                                     | QDir::System
                                                     | QDir::NoDotAndDotDot );

      for ( int ii = 0; ii < entries.size(); ii++ )
      {
         if ( entries[ ii ] == lockFile )  continue;

         QString   path = base + "/" + entries[ ii ];
         QFileInfo info( path );

         if ( info.isDir()  &&  ! info.isSymLink() )
            removeTree( path );
         else
            QFile::remove( path );
      }

      delete lock;                 // Unlocks, and removes the lock file
      lock = nullptr;

      if ( ! base.isEmpty() )
         QDir().rmdir( base );
   }

   base   .clear();
   staging.clear();
   work   .clear();
   root   .clear();
   staged_rels .clear();
   staged_first.clear();
}

void US_DataPubBundle::setKeepStaging( bool a_keep )
{
   keep = a_keep;
}

QString US_DataPubBundle::stagingPath( void ) const
{
   return staging;
}

QString US_DataPubBundle::workPath( void ) const
{
   return work;
}

QString US_DataPubBundle::rootPath( void ) const
{
   return root;
}

/* The private directory is made by QTemporaryDir, so two exports started in
   the same millisecond -- or by two processes -- never share one.  It holds
   the bundle tree and, beside it, the scratch space; nothing that is not
   meant for the bundle is ever put into the tree.
*/
bool US_DataPubBundle::createStaging( QString& error )
{
   cleanup();

   QString parent = US_Settings::tmpDir() + "/us_datapub";

   if ( ! QDir().mkpath( parent ) )
   {
      error = QObject::tr( "Cannot create the work directory %1" ).arg( parent );
      return false;
   }

   QTemporaryDir tdir( parent + "/bundle_XXXXXX" );

   if ( ! tdir.isValid() )
   {
      error = QObject::tr( "Cannot create a staging directory in %1: %2" )
              .arg( parent ).arg( tdir.errorString() );
      return false;
   }

   tdir.setAutoRemove( false );    // cleanup() removes it, unless kept

   base    = tdir.path();

   // The lock comes first: a directory of a running process is never
   // without it
   if ( ! lock_name.isEmpty() )
   {
      lock = new QLockFile( base + "/" + lock_name );
      lock->setStaleLockTime( 0 );

      if ( ! lock->tryLock( 0 ) )
      {
         error = QObject::tr( "Cannot lock the staging directory %1" )
                 .arg( base );
         delete lock;
         lock = nullptr;
         removeTree( base );
         base.clear();
         return false;
      }
   }
   staging = base + "/bundle";
   work    = base + "/work";

   if ( ! QDir().mkpath( staging )  ||  ! QDir().mkpath( work ) )
   {
      error = QObject::tr( "Cannot create the staging directory %1" )
              .arg( staging );
      cleanup();
      return false;
   }

   root = staging;

   error.clear();

   return true;
}

QString US_DataPubBundle::stagedPath( const QString& relativePath )
{
   if ( staging.isEmpty() )  return QString();

   // Nothing is staged anywhere but inside the tree
   if ( ! US_DataPub::isSafeRelativePath( relativePath ) )  return QString();

   QString  path = staging + "/" + relativePath;
   QFileInfo info( path );

   QDir().mkpath( info.absolutePath() );

   if ( ! staged_first.contains( relativePath ) )
      staged_first.insert( relativePath, staged_rels.size() );

   staged_rels << relativePath;

   return path;
}

int US_DataPubBundle::stagedMark( void ) const
{
   return staged_rels.size();
}

bool US_DataPubBundle::unstageSince( int mark, QString& error )
{
   error.clear();

   if ( mark < 0 )  mark = 0;

   QString top = QDir( staging ).absolutePath();

   while ( staged_rels.size() > mark )
   {
      QString rel = staged_rels.takeLast();

      // Handed out before the mark too: not this stage's to take back
      if ( staged_first.value( rel, -1 ) < mark )  continue;

      staged_first.remove( rel );

      QString path = staging + "/" + rel;

      if ( QFileInfo::exists( path )  &&  ! QFile::remove( path ) )
      {
         error = QObject::tr( "Cannot take back the staged file %1" )
                 .arg( rel );
         return false;
      }

      // ... and the directories that held nothing else
      QDir dir = QFileInfo( path ).dir();

      while ( dir.absolutePath().startsWith( top + "/" )  &&
              dir.exists()  &&  dir.isEmpty()  &&
              dir.rmdir( dir.absolutePath() ) )
         dir.cdUp();
   }

   return true;
}

namespace
{
   // A staging directory without a lock that has not changed for this long
   // belongs to no running importer or exporter
   const qint64 stale_unlocked_secs = 2 * 24 * 3600;
}

int US_DataPubBundle::sweepStaleStaging( void )
{
   QString     parent  = US_Settings::tmpDir() + "/us_datapub";
   QStringList dirs    = QDir( parent ).entryList( QStringList( "bundle_*" ),
                                                   QDir::Dirs
                                                   | QDir::NoDotAndDotDot );
   int         removed = 0;
   QDateTime   now     = QDateTime::currentDateTimeUtc();

   for ( int ii = 0; ii < dirs.size(); ii++ )
   {
      QString     path  = parent + "/" + dirs[ ii ];
      QStringList locks = QDir( path ).entryList( QStringList( "*.lock" ),
                                                  QDir::Files | QDir::Hidden );
      bool        gone  = false;

      if ( ! locks.isEmpty() )
      {
         // Stale only when no process that took a lock is running: the
         // lock of a live exporter or importer, however old, is never
         // taken over
         bool held = false;

         for ( int jj = 0; jj < locks.size()  &&  ! held; jj++ )
         {
            QLockFile probe( path + "/" + locks[ jj ] );
            probe.setStaleLockTime( 0 );

            if ( probe.tryLock( 0 ) )
               probe.unlock();
            else
               held = true;
         }

         gone = ! held;
      }

      else
      {
         QDateTime changed = QFileInfo( path ).lastModified().toUTC();

         gone = changed.isValid()  &&
                changed.secsTo( now ) > stale_unlocked_secs;
      }

      if ( gone  &&  QDir( path ).removeRecursively() )  removed++;
   }

   return removed;
}

bool US_DataPubBundle::listTree( const QString& dir,
                                 QMap< QString, qint64 >& files,
                                 QString& error )
{
   files.clear();

   QDir         top( dir );
   QDirIterator it( dir, QDir::AllEntries | QDir::Hidden | QDir::System
                         | QDir::NoDotAndDotDot,
                    QDirIterator::Subdirectories );

   while ( it.hasNext() )
   {
      it.next();

      QFileInfo info = it.fileInfo();
      QString   rel  = top.relativeFilePath( info.filePath() );

      if ( info.isSymLink() )
      {
         error = QObject::tr( "%1 is a symbolic link" ).arg( rel );
         return false;
      }

      if ( info.isDir() )  continue;

      if ( ! info.isFile() )
      {
         error = QObject::tr( "%1 is not a regular file" ).arg( rel );
         return false;
      }

      files.insert( rel, info.size() );
   }

   error.clear();

   return true;
}

QString US_DataPubBundle::packingStopped( void )
{
   return QObject::tr( "The packing of the bundle was stopped" );
}

bool US_DataPubBundle::pack( const QString& archivePath, QString& error,
                             const std::function< bool( int, int ) >& progress )
{
   if ( staging.isEmpty() )
   {
      error = QObject::tr( "There is nothing staged to pack" );
      return false;
   }

   // ---- what is staged, exactly -------------------------------------------
   QMap< QString, qint64 > staged;

   if ( ! listTree( staging, staged, error ) )
   {
      error = QObject::tr( "The staged bundle cannot be packed: %1" )
              .arg( error );
      return false;
   }

   if ( staged.isEmpty() )
   {
      error = QObject::tr( "The staged bundle is empty" );
      return false;
   }

   // The archive leaves hidden files out, so one in the tree would vanish
   // from the bundle without a word.  Nothing hidden is ever staged on
   // purpose; refuse rather than drop it.
   QList< QString > relPaths = staged.keys();

   for ( int ii = 0; ii < relPaths.size(); ii++ )
   {
      if ( US_DataPub::isSafeRelativePath( relPaths[ ii ] ) )  continue;

      error = QObject::tr( "The staged file %1 cannot be packed into a"
                           " bundle" ).arg( relPaths[ ii ] );
      return false;
   }

   QStringList entries = QDir( staging ).entryList(
                         QDir::AllEntries | QDir::NoDotAndDotDot, QDir::Name );
   QStringList sources;

   for ( int ii = 0; ii < entries.size(); ii++ )
      sources << staging + "/" + entries[ ii ];

   // ---- build the archive in the scratch space ---------------------------
   // US_Archive picks the compression from everything after the first dot
   // of the file name, so pack under a plain name.
   QString packDir   = work + "/pack";
   QString verifyDir = work + "/verify";
   removeTree( packDir );
   removeTree( verifyDir );

   if ( ! QDir().mkpath( packDir ) )
   {
      error = QObject::tr( "Cannot create the packing directory %1" )
              .arg( packDir );
      return false;
   }

   QString    workFile = packDir + "/bundle.tar.gz";
   US_Archive archive;

   // Two steps per file: packed, then compared.  The archive cannot be
   // stopped while it is written, so a stop asked for meanwhile is
   // carried out once it is.
   const int total = 2 * staged.size();
   int       done  = 0;
   bool      going = true;

   auto step = [ & ]( void )
   {
      done = qMin( done + 1, total );

      if ( progress  &&  ! progress( done, total ) )
         going = false;

      return going;
   };

   QObject::connect( &archive, &US_Archive::itemAdded, &archive,
                     [ & ]( const QString&, const QString& )
                     {
                        if ( done < staged.size() )  step();
                     } );

   if ( ! archive.compress( sources, workFile )  ||
        ! QFile::exists( workFile ) )
   {
      error = QObject::tr( "The bundle could not be packed: %1" )
              .arg( archive.getError() );
      removeTree( packDir );
      return false;
   }

   done = staged.size();

   if ( ! going  ||  ( progress  &&  ! progress( done, total ) ) )
   {
      error = packingStopped();
      removeTree( packDir );
      return false;
   }

   // ---- read it back, and compare with what was staged ------------------
   US_Archive              reader;
   QMap< QString, qint64 > packed;

   bool readable = reader.extract( workFile, verifyDir )  &&
                   listTree( verifyDir, packed, error );

   // Names and sizes first, then the content of every file: a file that
   // came back the right size but not the same is caught as well
   QStringList missing;

   if ( readable )
   {
      for ( int ii = 0; ii < relPaths.size()  &&  going; ii++ )
      {
         QString rel = relPaths[ ii ];

         if ( ! step() )  break;

         // Two reads that both failed give two empty digests, which are
         // equal; a digest that could not be taken is a failure
         QString packedDigest;
         QString stagedDigest;

         if ( packed.value( rel, -1 ) != staged.value( rel )  ||
              ! US_DataPub::fileSha256( verifyDir + "/" + rel, packedDigest )  ||
              ! US_DataPub::fileSha256( staging + "/" + rel, stagedDigest )  ||
              packedDigest.isEmpty()  ||  packedDigest != stagedDigest )
            missing << rel;
      }
   }

   removeTree( verifyDir );

   if ( ! going )
   {
      error = packingStopped();
      removeTree( packDir );
      return false;
   }

   if ( ! readable  ||  packed != staged  ||  ! missing.isEmpty() )
   {

      error = readable
              ? QObject::tr( "The packed bundle does not hold what was staged"
                             " (%1)" ).arg( missing.isEmpty()
                                            ? QObject::tr( "extra entries" )
                                            : missing.join( ", " ) )
              : QObject::tr( "The packed bundle cannot be read back: %1" )
                .arg( reader.getError().isEmpty() ? error
                                                  : reader.getError() );
      removeTree( packDir );
      return false;
   }

   // ---- put it in place in one step ---------------------------------------
   QFileInfo target( archivePath );

   if ( ! QDir().mkpath( target.absolutePath() ) )
   {
      error = QObject::tr( "Cannot create the output directory %1" )
              .arg( target.absolutePath() );
      removeTree( packDir );
      return false;
   }

   // The last chance to stop: nothing has been written yet
   if ( progress  &&  ! progress( total, total ) )
   {
      error = packingStopped();
      removeTree( packDir );
      return false;
   }

   QFile     input( workFile );
   QSaveFile output( archivePath );
   bool      written = input.open( QIODevice::ReadOnly )  &&
                       output.open( QIODevice::WriteOnly );

   while ( written  &&  ! input.atEnd() )
   {
      QByteArray chunk = input.read( 1 << 20 );

      if ( chunk.isEmpty()  &&  input.error() != QFileDevice::NoError )
         written = false;

      else if ( output.write( chunk ) != chunk.size() )
         written = false;
   }

   input.close();

   if ( written )
      written = output.commit();       // The atomic replace
   else
      output.cancelWriting();

   removeTree( packDir );

   if ( ! written )
   {
      error = QObject::tr( "Cannot write the bundle to %1: %2" )
              .arg( archivePath ).arg( output.errorString() );
      return false;
   }

   error.clear();

   return true;
}

/* Check what an extraction left behind.

   The archive reader puts every entry where its path says, and a path of
   "../x" lands beside the bundle tree instead of in it.  The private
   directory holds nothing but the tree and the scratch space, so anything
   else in it -- or anything in the tree that is not a plain file or
   directory -- means the bundle tried to write where it should not.
*/
bool US_DataPubBundle::verifyEscapes( QString& error ) const
{
   QStringList beside = QDir( base ).entryList(
                        QDir::AllEntries | QDir::Hidden | QDir::System
                        | QDir::NoDotAndDotDot );
   beside.removeAll( "bundle" );
   beside.removeAll( "work" );

   // The lock, when one is held -- still the regular file this process
   // wrote, not something an entry of the archive put in its place
   if ( lock != nullptr  &&  beside.contains( lock_name ) )
   {
      QString   path = base + "/" + lock_name;
      QFileInfo info( path );
      QFile     file( path );
      QString   pid;

      if ( ! info.isSymLink()  &&  info.isFile()  &&
           file.open( QIODevice::ReadOnly ) )
         pid = QString::fromLatin1( file.readLine( 64 ) ).trimmed();

      if ( pid == QString::number( QCoreApplication::applicationPid() ) )
         beside.removeAll( lock_name );
   }

   if ( ! beside.isEmpty() )
   {
      error = QObject::tr( "the bundle holds entries outside of its own"
                           " directory (%1)" ).arg( beside.join( ", " ) );
      return false;
   }

   QMap< QString, qint64 > files;

   if ( ! listTree( staging, files, error ) )  return false;

   QString          canonRoot = QFileInfo( staging ).canonicalFilePath() + "/";
   QList< QString > rels      = files.keys();

   for ( int ii = 0; ii < rels.size(); ii++ )
   {
      QString canon = QFileInfo( staging + "/" + rels[ ii ] )
                      .canonicalFilePath();

      if ( canon.startsWith( canonRoot ) )  continue;

      error = QObject::tr( "the entry %1 lies outside of the bundle" )
              .arg( rels[ ii ] );
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
      cleanup();     // Leave nothing of a refused or broken bundle behind
      return false;
   }

   QString why;

   if ( ! verifyEscapes( why ) )
   {
      error = QObject::tr( "The bundle %1 is refused: %2" )
              .arg( archivePath ).arg( why );
      cleanup();
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
   cleanup();

   return false;
}
