//! \file us_disk_scan.cpp
#include "us_disk_scan.h"

US_DiskScan::US_DiskScan( QObject* parent ) : QThread( parent )
{
   job       = JobNone;
   job_index = -1;
   stopping  = false;
   err_ok    = true;

   // The catalog is built here rather than in the worker, so that it is
   // there the moment the scanner is.  Built in the worker it would have
   // to announce itself, and that announcement would race a job asked for
   // before the thread had got going -- a caller would be told the job
   // was done when the worker had not started it.
   cat = new US_DataCatalog();

   // Listing a store does not hash its files; that is what a verify job is
   cat->setChecksums( false );

   connect( cat, &US_DataCatalog::message,
            this, &US_DiskScan::message );

   idle.storeRelease( 1 );      // idle, with a catalog ready to be read

   start();
}

US_DiskScan::~US_DiskScan()
{
   {
      QMutexLocker lock( &mutex );
      stopping = true;
      wakeup.wakeOne();
   }

   wait();                      // nothing else touches the catalog now

   delete cat;
   cat = nullptr;
}

void US_DiskScan::request( Job a_job, const QString& guid, int index )
{
   QMutexLocker lock( &mutex );

   idle.storeRelease( 0 );      // busy from here until the worker is done

   job       = a_job;
   job_guid  = guid;
   job_index = index;

   wakeup.wakeOne();
}

void US_DiskScan::list( const QString& projectGUID )
{
   request( JobList, projectGUID, -1 );
}

void US_DiskScan::readChain( void )
{
   request( JobChain, QString(), -1 );
}

void US_DiskScan::verify( int index )
{
   request( JobVerify, QString(), index );
}

bool US_DiskScan::busy( void ) const
{
   return ( idle.loadAcquire() == 0 );
}

US_DataCatalog* US_DiskScan::catalog( void )
{
   // Reading it while the worker is in it would be a race, so it is not
   // offered until the worker has published what it read
   if ( idle.loadAcquire() == 0 )  return nullptr;

   return cat;
}

QString US_DiskScan::error( void ) const
{
   if ( idle.loadAcquire() == 0 )  return QString();

   return err_text;
}

bool US_DiskScan::ok( void ) const
{
   if ( idle.loadAcquire() == 0 )  return false;

   return err_ok;
}

void US_DiskScan::run( void )
{
   forever
   {
      Job     current;
      QString guid;
      int     index;

      {
         QMutexLocker lock( &mutex );

         while ( job == JobNone  &&  ! stopping )
            wakeup.wait( &mutex );

         if ( stopping )  break;

         current = job;
         guid    = job_guid;
         index   = job_index;
         job     = JobNone;
      }

      QString message;
      bool    done = false;

      switch ( current )
      {
         case JobList:
            done = cat->open( US_DataCatalog::Disk, QString(), message )
                   &&  cat->loadRuns( guid, message );
            break;

         case JobChain:
            done = cat->loadAll( message );
            break;

         case JobVerify:
            done = cat->verifyRun( index, message );
            break;

         default:
            done = true;
            break;
      }

      err_ok   = done;
      err_text = message;

      idle.storeRelease( 1 );      // publishes everything written above

      emit jobDone( int( current ), index );
   }
}
