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
    asks for a job, waits for \ref busy to go false, and only then reads
    \ref catalog; while a job is running \ref catalog answers null.  The
    worker publishes what it read by going idle and the caller sees it by
    reading that, so what the worker wrote is visible to whoever finds it
    idle.  Nothing else is shared, so there is nothing else to lock.

    \section diskscan_jobs The jobs

    \ref list reads the experiments, \ref readChain reads the records
    below them, and \ref verify reads and hashes the files of one
    experiment.  They are the same three phases a scan has, so a caller
    can run each of them against the database at the same time.
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

      //! \brief Finish the current job, stop the thread and release the
      //!        catalog
      ~US_DiskScan() override;

      //! \brief Read the experiments of the local store
      //! \param projectGUID When not empty, only experiments of that project
      void list     ( const QString& projectGUID = QString() );

      //! \brief Read the records below the experiments
      void readChain( void );

      //! \brief Read and hash the files of one experiment
      //! \param index The position of the experiment
      void verify   ( int index );

      //! \brief True while a job is running
      //!
      //! A caller waits for this to go false before reading anything the
      //! worker produced.
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
      //! \brief A job has finished
      //! \param job   The job that finished
      //! \param index The experiment it was for, or -1
      void jobDone ( int job, int index );

      //! \brief A human readable note from the catalog
      //! \param message The note
      void message ( const QString& message );

   protected:
      //! \brief The worker loop: wait for a job, do it, say it is done
      void run( void ) override;

   private:
      US_DataCatalog*  cat;

      mutable QMutex   mutex;
      QWaitCondition   wakeup;

      Job              job;        //!< The job asked for, guarded by mutex
      QString          job_guid;   //!< Its project filter, guarded by mutex
      int              job_index;  //!< Its experiment, guarded by mutex
      bool             stopping;   //!< Guarded by mutex

      //! Zero while a job is running.  The worker publishes what it read by
      //! storing 1 here, and a caller reads it with an acquire, so what the
      //! worker wrote is visible to whoever sees the 1.
      QAtomicInt       idle;

      QString          err_text;   //!< Written by the worker before idle
      bool             err_ok;     //!< Written by the worker before idle

      void request( Job job, const QString& guid, int index );
};
#endif
