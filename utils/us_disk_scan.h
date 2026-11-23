//! \file us_disk_scan.h
#ifndef US_DISK_SCAN_H
#define US_DISK_SCAN_H

#include <QtCore>

#include "us_extern.h"
#include "us_data_catalog.h"

/*! \class US_DiskScan
    \brief Reads a local UltraScan3 data store on a thread of its own

    Scanning a local store is file system work: a listing per run
    directory, the header of every .auc file, the GUIDs out of every edit
    file, and -- when a record has to be compared against a database copy
    -- the whole of every file, hashed.  On a store of multi-wavelength
    runs that is thousands of files, and doing it on the thread that draws
    the window is what makes the window stop responding.

    This runs it on a worker thread instead, so that it overlaps the
    database queries rather than following them.

    \section diskscan_contract Who may touch the catalog

    Exactly one thread is in the \ref US_DataCatalog at a time.  A caller
    asks for a job and waits for \ref busy to go false, and only then reads
    \ref catalog; while a job is running \ref catalog answers null and a
    request for another job is refused.

    The scanner goes idle on the thread that owns it -- the one that
    created it -- not on the worker: the worker finishes the job and posts
    the result, and when that thread's event loop delivers it, \ref busy
    goes false and then \ref jobDone is emitted.  So a caller waiting for a
    job keeps its event loop turning, as a window does anyway, and a slot
    connected to \ref jobDone finds the scanner idle and its catalog
    readable, whatever kind of connection it used.  What the worker wrote
    is visible to the owning thread once it sees the scanner idle.

    \section diskscan_jobs The jobs

    \ref list reads the experiments, \ref readChain reads the records
    below them, and \ref verify reads and hashes the files of one
    experiment.  They are the same three phases a scan has, so a caller
    can run each of them against the database at the same time.

    \section diskscan_cancel Stopping

    \ref cancel stops the job that is running at the next file it would
    read, and the job then finishes as failed.  The request ends with that
    job: once the scanner is idle again, reading its catalog directly
    (\ref catalog, then US_DataCatalog::loadRunDetail, say) is not
    cancelled, and \ref cancel while no job runs does nothing.  Destroying
    the scanner cancels whatever it is doing first, so closing a window does
    not wait for a whole store to be indexed.
*/
class US_UTIL_EXTERN US_DiskScan : public QThread
{
   Q_OBJECT

   public:
      //! \brief What the worker was last asked to do
      enum Job
      {
         JobNone = 0,  //!< Nothing; the worker is waiting
         JobList,      //!< Read the experiments
         JobChain,     //!< Read the records below them
         JobVerify     //!< Read and hash the files of one experiment
      };

      //! \brief Start the worker thread; it waits for its first job
      //! \param parent The parent object, for Qt ownership
      explicit US_DiskScan( QObject* parent = nullptr );

      //! \brief Cancel the current job, stop the thread and release the
      //!        catalog
      ~US_DiskScan() override;

      //! \brief Read the experiments of the local store
      //! \param projectGUID When not empty, only experiments of that project
      //! \returns False, and nothing is done, while a job is running
      bool list     ( const QString& projectGUID = QString() );

      //! \brief Read the records below the experiments
      //! \returns False, and nothing is done, while a job is running
      bool readChain( void );

      //! \brief Read and hash the files of one experiment
      //! \param index The position of the experiment
      //! \returns False, and nothing is done, while a job is running
      bool verify   ( int index );

      //! \brief Stop the job that is running, if any; it finishes as
      //!        failed.  With no job running this does nothing.
      void cancel   ( void );

      //! \brief True from a request until the owning thread has seen the
      //!        job finish
      //!
      //! A caller waits for this to go false, with its event loop turning,
      //! before reading anything the worker produced.
      bool busy( void ) const;

      /*! \brief The catalog the worker reads into

          Null while a job is running: reading it then would race the
          worker.  Between jobs it is the caller's to read.
      */
      US_DataCatalog* catalog( void );

      //! \brief Why the last job failed, or empty when it did not
      QString error( void ) const;

      //! \brief True when the last job succeeded
      bool ok( void ) const;

   signals:
      //! \brief A job has finished, and the scanner is idle again
      //! \param job   The job that finished
      //! \param index The experiment it was for, or -1
      void jobDone ( int job, int index );

      //! \brief The job changed an experiment that was loaded already, as
      //!        US_DataCatalog::runChanged says.  Emitted on the owning
      //!        thread once the job has finished and the scanner is idle,
      //!        just before \ref jobDone, so a slot can read \ref catalog;
      //!        for a read the caller makes itself on the idle catalog, as
      //!        the catalog emits it.
      //! \param index The position of the experiment
      void runChanged ( int index );

      //! \brief A human readable note from the catalog
      //! \param message The note
      void message ( const QString& message );

   protected:
      //! \brief The worker loop: wait for a job, do it, post the result
      void run( void ) override;

   private:
      US_DataCatalog*  cat;

      mutable QMutex   mutex;
      QWaitCondition   wakeup;

      Job              job;        //!< The job asked for, guarded by mutex
      QString          job_guid;   //!< Its project filter, guarded by mutex
      int              job_index;  //!< Its experiment, guarded by mutex
      bool             stopping;   //!< Guarded by mutex

      //! Zero from a request until the owning thread has taken the result.
      //! Only the owning thread changes it, so a caller that sees 1 sees
      //! everything the worker wrote before it posted the result.
      QAtomicInt       idle;

      //! Non-zero once the running job should stop; the catalog checks it
      QAtomicInt       abort_flag;

      QString          err_text;   //!< Written by the worker before posting
      //! The experiments the job changed; written by the worker before
      //! posting, read and cleared by the owning thread in finished()
      QList< int >     changed_runs;
      bool             err_ok;     //!< Written by the worker before posting

      bool request ( Job job, const QString& guid, int index );

      //! \brief Take a finished job's result, on the owning thread
      void finished( int job, int index );
};
#endif
