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

   // A job asked to stop stops at the next file it would read
   cat->setCancelFlag( &abort_flag );

   connect( cat, &US_DataCatalog::message,
            this, &US_DiskScan::message );

   // An experiment a job changed is noted on the worker, as it happens,
   // and announced on the owning thread once the job is over, when the
   // catalog can be read; see finished().  One the caller's own read of the
   // idle catalog changed is announced at once.
   connect( cat, &US_DataCatalog::runChanged, cat,
            [ this ]( int index )
            {
               if ( QThread::currentThread() == thread() )
                  emit runChanged( index );
               else
                  changed_runs << index;
            },
            Qt::DirectConnection );

   idle.storeRelease( 1 );      // idle, with a catalog ready to be read

   start();
}

US_DiskScan::~US_DiskScan()
{
   {
      QMutexLocker lock( &mutex );
      stopping = true;
      abort_flag.storeRelease( 1 );   // stop what it is doing, not just after
      wakeup.wakeOne();
   }

   wait();                      // nothing else touches the catalog now

   delete cat;
   cat = nullptr;
}

bool US_DiskScan::request( Job a_job, const QString& guid, int index )
{
   QMutexLocker lock( &mutex );

   // One job at a time: the catalog is the worker's until the owning
   // thread has taken the last result
   if ( idle.loadAcquire() == 0 )  return false;

   idle.storeRelease( 0 );      // busy from here until the result is taken
   abort_flag.storeRelease( 0 );

   job       = a_job;
   job_guid  = guid;
   job_index = index;

   wakeup.wakeOne();

   return true;
}

bool US_DiskScan::list( const QString& projectGUID )
{
   return request( JobList, projectGUID, -1 );
}

bool US_DiskScan::readChain( void )
{
   return request( JobChain, QString(), -1 );
}

bool US_DiskScan::verify( int index )
{
   return request( JobVerify, QString(), index );
}

void US_DiskScan::cancel( void )
{
   // Only a running job is stopped: with none running there is nothing to
   // stop, and a flag left set would stop the caller's own next read
   if ( idle.loadAcquire() == 0 )
      abort_flag.storeRelease( 1 );
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

// The owning thread takes the result: idle first, so whoever jobDone
// reaches finds the catalog readable
//
// The cancel request belongs to the job that just finished.  Left set, it
// would stop whatever reads the catalog next -- the next job is cleared by
// request(), but a caller that reads the idle catalog directly, as
// catalog()->loadRunDetail() does, would be told it was cancelled.  It is
// cleared here, before the scanner goes idle, unless the scanner is being
// torn down.
void US_DiskScan::finished( int a_job, int index )
{
   {
      QMutexLocker lock( &mutex );

      if ( ! stopping )
         abort_flag.storeRelease( 0 );
   }

   const QList< int > changed = changed_runs;
   changed_runs.clear();

   idle.storeRelease( 1 );

   for ( int run : changed )
      emit runChanged( run );

   emit jobDone( a_job, index );
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
         changed_runs.clear();
         guid    = job_guid;
         index   = job_index;
         job     = JobNone;
      }

      QString message;
      bool    done = false;

      switch ( current )
      {
         case JobList:
            // Not open(): that starts the catalog afresh, and a listing that
            // then fails or is cancelled would leave it empty.  The catalog
            // reads the local store from the start, and loadRuns() keeps
            // the last listing when it cannot make a new one.
            done = cat->loadRuns( guid, message );
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

      // Posting the result is what hands the catalog back: the event
      // queue's lock orders everything written above before the owning
      // thread reads it.  If the scanner is destroyed first, the posted
      // call is dropped with it.
      const int finished_job = int( current );
      QMetaObject::invokeMethod( this, [ this, finished_job, index ]()
                                 { finished( finished_job, index ); },
                                 Qt::QueuedConnection );
   }
}
