//! \file us_data_model.cpp

#include "us_data_model.h"
#include "us_util.h"
#include "us_settings.h"
#include "us_model.h"
#include "us_noise.h"

#include <QCryptographicHash>

#include <algorithm>
#include <functional>

#define timeFmt QString("hh:mm:ss")
#define nowTime() "T="+QDateTime::currentDateTime().toString(timeFmt)

namespace
{
   const QString dmyGUID = "00000000-0000-0000-0000-000000000000";

   // A label short enough for the tree, from a model or noise description
   QString short_label( const QString& descript )
   {
      QString label = descript.section( ".", 0, -2 );

      if ( label.length() > 40 )
         label = label.left( 13 ) + "..." + label.right( 24 );

      return label;
   }
}

// Scan the database and local disk for R/E/M/N data sets
//
// The window owns the model, so the disk worker -- and its thread -- go when
// the window does
US_DataModel::US_DataModel( QWidget* parwidg /*=0*/ ) : QObject( parwidg )
{
   parentw    = parwidg;   // parent (main manage_data) widget

   db         = NULL;
   cat_db     = NULL;
   cat_lo     = NULL;
   scanner    = NULL;
   use_db     = false;
   use_lo     = false;
   db_failed  = false;
   scanning   = false;
   stop_req   = false;
   stopped    = false;
   demo       = false;
   orph_indexed = false;
   busy       = 0;
   progress   = NULL;
   lb_status  = NULL;
   ob_process = NULL;
   ob_tree    = NULL;
   kdb_recs   = 0;
   klo_recs   = 0;
   batch_depth = 0;
   remerging  = false;
   merging_run = -1;
   case_set   = false;
   file_case  = Qt::CaseSensitive;
   orph_size[ 0 ] = 0;
   orph_size[ 1 ] = 0;

   dbg_level  = US_Settings::us_debug();
}

US_DataModel::~US_DataModel()
{
   close_catalogs();
}

// Set database related pointers
void US_DataModel::setDatabase( IUS_DB2* a_db )
{
   db         = a_db;      // pointer to opened db connection
}

// How the local store compares file names, from the next scan
void US_DataModel::setFileNameCase( Qt::CaseSensitivity sensitivity )
{
   case_set   = true;
   file_case  = sensitivity;
}

// Set progress bar related pointers
void US_DataModel::setProgress( QProgressBar* a_progr, QLabel* a_lbstat )
{
   progress   = a_progr;   // pointer to progress bar
   lb_status  = a_lbstat;  // pointer to status label
}

// set sibling classes pointers
void US_DataModel::setSiblings( QObject* a_proc, QObject* a_tree )
{
   ob_process = a_proc;    // pointer to sister DataProcess object
   ob_tree    = a_tree;    // pointer to sister DataTree object
}

// Get database pointer
IUS_DB2* US_DataModel::dbase()
{
   return db;
}

// Get progress bar pointer
QProgressBar* US_DataModel::progrBar()
{
   return progress;
}

// Get status label pointer
QLabel* US_DataModel::statlab()
{
   return lb_status;
}

// get us_data_process object pointer
QObject* US_DataModel::procobj()
{
   return ob_process;
}

// get us_data_tree object pointer
QObject* US_DataModel::treeobj()
{
   return ob_tree;
}

// ------------------------------------------------------------ the selectors
//
// The run and triple selectors only need names, so they are filled the way
// they always were: one query and one directory listing.  Reading the
// record chain, or indexing the local store, to fill a combo box is what
// made opening the window and picking a run slow.

// The run IDs of one person's database experiments
QStringList US_DataModel::db_run_ids( IUS_DB2* dbP, int invID,
                                      QString& error )
{
   QStringList runIDs;
   error.clear();

   if ( dbP == NULL )   return runIDs;

   QStringList query;
   query << "get_experiment_desc" << QString::number( invID );
   dbP->query( query );

   int status = dbP->lastErrno();

   if ( status != IUS_DB2::OK  &&  status != IUS_DB2::NOROWS )
   {
      error = QObject::tr( "The experiments could not be listed: %1" )
              .arg( dbP->lastError() );
      return runIDs;
   }

   while ( dbP->next() )
   {
      QString runID = dbP->value( 1 ).toString();

      if ( ! runID.isEmpty()  &&  ! runIDs.contains( runID ) )
         runIDs << runID;
   }

   return runIDs;
}

// The run IDs of the local results directory
QStringList US_DataModel::local_run_ids( void )
{
   QStringList runIDs;
   QString     rdir    = US_Settings::resultDir() + "/";
   QStringList aucdirs = QDir( rdir )
      .entryList( QDir::AllDirs | QDir::NoDotAndDotDot, QDir::Name );
   QStringList aucfilt( "*.auc" );

   for ( int ii = 0; ii < aucdirs.size(); ii++ )
   {  // A run directory is one that holds AUC data; its name is the run ID
      if ( QDir( rdir + aucdirs[ ii ] )
           .entryList( aucfilt, QDir::Files ).isEmpty() )
         continue;

      runIDs << aucdirs[ ii ];
   }

   return runIDs;
}

// The triples of one database run
QStringList US_DataModel::db_triples( IUS_DB2* dbP, const QString& runID,
                                      int invID, QString& error )
{
   QStringList triples;
   error.clear();

   if ( dbP == NULL )   return triples;

   QStringList query;
   query << "get_experiment_info_by_runID" << runID
         << QString::number( invID );
   dbP->query( query );

   if ( dbP->lastErrno() != IUS_DB2::OK  ||  ! dbP->next() )
      return triples;                  // not in the database: nothing to add

   QString expID = dbP->value( 1 ).toString();

   query.clear();
   query << "get_rawDataIDs" << expID;
   dbP->query( query );

   int status = dbP->lastErrno();

   if ( status != IUS_DB2::OK  &&  status != IUS_DB2::NOROWS )
   {
      error = QObject::tr( "The raw data of %1 could not be listed: %2" )
              .arg( runID ).arg( dbP->lastError() );
      return triples;
   }

   while ( dbP->next() )
   {
      QString fname = dbP->value( 2 ).toString().section( "/", -1, -1 );

      if ( US_DataCatalog::runIdOfFile( fname, false ) != runID )
         continue;

      QString triple = fname.section( ".", -4, -2 );

      if ( ! triples.contains( triple ) )
         triples << triple;
   }

   return triples;
}

// The triples of one local run
QStringList US_DataModel::local_triples( const QString& runID )
{
   QStringList triples;
   QStringList aucfiles = QDir( US_Settings::resultDir() + "/" + runID )
      .entryList( QStringList( "*.auc" ), QDir::Files, QDir::Name );

   for ( int ii = 0; ii < aucfiles.size(); ii++ )
   {
      QString fname = aucfiles.at( ii );

      if ( US_DataCatalog::runIdOfFile( fname, false ) != runID )
         continue;

      QString triple = fname.section( ".", -4, -2 );

      if ( ! triples.contains( triple ) )
         triples << triple;
   }

   return triples;
}

// Scan the database and local disk for run identifiers
void US_DataModel::getRunIDs( QStringList& runIDs, int& source )
{
   runIDs.clear();

   // source: 0=ALL, 1=DB Only, 2=Local Only,
   //         3=Exclude Local-Only Trees, 4=Exclude DB-Only Trees
   QStringList dbIDs;
   QStringList loIDs;
   QString     error;

   if ( source != 2 )
   {
      dbIDs = db_run_ids( db, US_Settings::us_inv_ID(), error );

      if ( ! error.isEmpty() )
         note( error );
   }

   if ( source != 1 )
      loIDs = local_run_ids();

   if ( source == 3 )
   {  // only runs that are in the database
      runIDs = dbIDs;
   }

   else if ( source == 4 )
   {  // only runs that are on local disk
      runIDs = loIDs;
   }

   else
   {
      runIDs = dbIDs;

      for ( int ii = 0; ii < loIDs.size(); ii++ )
         if ( ! runIDs.contains( loIDs[ ii ] ) )
            runIDs << loIDs[ ii ];
   }
DbgLv(1) << "gRI: db+local runs" << runIDs.size();

   runIDs.sort();
}

// Scan the database and local for triples in a runID then return to caller
void US_DataModel::getTriples( QStringList& triples, QString runID )
{
   QString error;

   triples = db_triples( db, runID, US_Settings::us_inv_ID(), error );

   if ( ! error.isEmpty() )
      note( error );

   QStringList lotrips = local_triples( runID );

   for ( int ii = 0; ii < lotrips.size(); ii++ )
      if ( ! triples.contains( lotrips[ ii ] ) )
         triples << lotrips[ ii ];
DbgLv(1) << "gTr: db+local triples" << triples.size();

   triples.sort();
}

// Set run and triple filters
void US_DataModel::setFilters( QString a_runf, QString a_tripf, QString a_srcf )
{
   filt_run    = a_runf;   // Filter string for runID
   filt_triple = a_tripf;  // Filter string for triple
   filt_source = a_srcf;   // Filter string for source (DB/local)
}

// ------------------------------------------------------------ status

// Put a note on the status line, cut to what the line can show
void US_DataModel::note( const QString& text )
{
   if ( lb_status == NULL )   return;

   int width = qMax( lb_status->width() - 4, 80 );

   lb_status->setText( lb_status->fontMetrics().elidedText(
                       text, Qt::ElideMiddle, width ) );
}

// What a catalog has to say goes on the status line
void US_DataModel::scanner_note( const QString& text )
{
   note( text );
}

// Record an error of the scan, so it is not only in the debug log
void US_DataModel::scan_error( const QString& text )
{
   DbgLv(1) << "ScanErr:" << text;

   if ( ! errors.contains( text ) )
      errors << text;
}

QStringList US_DataModel::scan_errors( void ) const
{
   return errors;
}

bool US_DataModel::db_listing_failed( void ) const
{
   return db_failed;
}

bool US_DataModel::scan_active( void ) const
{
   return scanning;
}

bool US_DataModel::is_busy( void ) const
{
   return ( busy > 0 );
}

// Hold off reads and merges while an action asks and runs
void US_DataModel::begin_hold( void )
{
   busy++;
}

void US_DataModel::end_hold( void )
{
   if ( busy > 0 )
      busy--;
}

// A read asked for while another is in progress, or while the scan loop
// runs, is queued: the scan loop, or whoever runs the queue, gets to it
bool US_DataModel::must_queue( void ) const
{
   return ( busy > 0  ||  scanning );
}

/* Scan the database and local disk for R/E/M/N data sets.

   This is the loop the window runs.  It processes events between
   experiments so the window can take input there, and anything the window
   asks of the model meanwhile is only queued -- see must_queue().  The
   queries themselves block, and while the local store is read the window
   repaints but takes no input -- see await_disk().
*/
void US_DataModel::scan_data()
{
   if ( must_queue() )   return;

   scanning = true;
   stop_req = false;
   stopped  = false;

DbgLv(1) << "ScnD: start scan   " << nowTime();
   emit scanPhase( tr( "Listing experiments..." ), 1 );

   scan_runs();            // First layer:  list the experiments

   emit runsListed();
   emit scanStep( 1 );
   qApp->processEvents();
DbgLv(1) << "ScnD: runs listed  " << nowTime();

   int  nruns = runents.size();
   bool rfilt = ( ! filt_run.isEmpty()  &&  filt_run != "ALL" );
   bool bulk  = false;

   if ( nruns > 0 )
      emit scanPhase( tr( "Reading %1 experiment(s)..." ).arg( nruns ), nruns );

   // A run filter reads only that run, one experiment at a time: reading
   // the whole store to show one run is what the filter is there to avoid.
   // Without one the whole store is read even when no experiment is
   // listed, since it may still hold records of no experiment.
   if ( ! rfilt  &&  ! stop_req )
      bulk = scan_all();

   else if ( cat_db != NULL )
      cat_db->setChecksums( false );   // compared later, if in both places
DbgLv(1) << "ScnD: bulk read" << bulk << nowTime();

   int done  = 0;
   int index = -1;

   while ( ! stop_req  &&  ( index = next_pending_run() ) >= 0 )
   {  // Second layer:  merge one experiment at a time
      note( tr( "Reading %1 ..." ).arg( runents.at( index ).runID ) );

      if ( bulk )  do_merge_run( index );
      else         do_scan_run ( index );

      run_queue.removeAll( index );     // never the same one twice

      emit runMerged( index );
      emit scanStep ( ++done );
      qApp->processEvents();
   }
DbgLv(1) << "ScnD: chain read   " << nowTime();

   // What attaches to no experiment, under stand-in experiments of its own
   int orphx = stop_req ? -1 : collect_orphans();

   for ( int ii = orphx; ii >= 0  &&  ii < runents.size(); ii++ )
      emit orphansAdded( ii );

   // Third layer:  compare the experiments that are in both places
   int nver = stop_req ? 0 : queue_verifies();
   done     = 0;

   if ( nver > 0 )
      emit scanPhase( tr( "Comparing %1 experiment(s) held in both places..." )
                      .arg( nver ), nver );

   while ( ! stop_req  &&  ( index = next_pending_verify() ) >= 0 )
   {
      note( tr( "Comparing %1 ..." ).arg( runents.at( index ).runID ) );

      if ( do_verify_run( index ) )
         emit runCompared( index );

      ver_queue.removeAll( index );

      emit scanStep( ++done );
      qApp->processEvents();
   }

   stopped  = stop_req;
   stop_req = false;
   scanning = false;

   if ( stopped )
   {  // Whatever was not reached is left for the next scan
      run_queue.clear();
      ver_queue.clear();
      note( tr( "Data scan stopped" ) );
DbgLv(1) << "ScnD: scan stopped " << nowTime();
      return;
   }

   note( errors.isEmpty() ? tr( "Data scan complete" )
                          : tr( "Data scan complete, with %1 error(s)" )
                            .arg( errors.size() ) );
DbgLv(1) << "ScnD: scan done    " << nowTime();
}

// Ask the running scan, or the read or comparison in progress outside one,
// to stop after the step it is in
void US_DataModel::stop_scan( void )
{
   if ( ! scanning  &&  busy == 0 )   return;

   if ( scanning )
      stop_req = true;

   if ( scanner != NULL )
      scanner->cancel();               // a read of the local store stops too
}

bool US_DataModel::scan_stopped( void ) const
{
   return stopped;
}

bool US_DataModel::is_demo( void ) const
{
   return demo;
}

// Get data description object at specified row
US_DataModel::DataDesc US_DataModel::row_datadesc( int irow )
{
   return adescs.at( irow );
}

// Get current data description object
US_DataModel::DataDesc US_DataModel::current_datadesc( )
{
   return cdesc;
}

// Change data description object at a specified row
void US_DataModel::change_datadesc( DataDesc ddesc, int row )
{
   adescs[ row ] = ddesc;
   cdesc         = ddesc;
}

// Set current data description object
void US_DataModel::setCurrent( int irow )
{
   cdesc   = adescs.at( irow );
}

// get count of total data records
int US_DataModel::recCount()
{
   return adescs.size();
}

// get count of DB data records
int US_DataModel::recCountDB()
{
   return kdb_recs;
}

// get count of local data records
int US_DataModel::recCountLoc()
{
   return klo_recs;
}

// ------------------------------------------------------------ the scan
//
// A scan is read in two layers, because a store of multi-wavelength runs
// holds hundreds of triples per experiment and thousands of records below
// them.  scan_runs() lists the experiments; scan_run() reads the chain of
// one of them.  Both sources -- the database and the local disk -- are read
// through US_DataCatalog, so the walk of the record chain is the same one
// every other program uses, and the local store is read once per scan.
//
// The experiment rows are rows 0 to N-1, and each experiment's records are
// appended when it is read.  So the records of an experiment are its
// firstRow to lastRow, and the rows that follow an experiment row are other
// experiments: anything that walks the rows below a row has to stay inside
// the row range of its experiment -- see run_of_row() and descendant_rows().

US_DataModel::RunEntry::RunEntry()
{
   row      = -1;
   firstRow = -1;
   lastRow  = -1;
   dbIndex  = -1;
   loIndex  = -1;
   dbCount  = 0;
   loCount  = 0;
   loaded   = false;
   verified = false;
   catVerified = false;
   orphans  = false;
   globals  = false;
   unsafe   = false;
}


bool US_DataModel::RunEntry::inBoth( void ) const
{
   return ( dbIndex >= 0  &&  loIndex >= 0 );
}

// Release the catalogs and the disk worker
void US_DataModel::close_catalogs( )
{
   // The worker has to be stopped before its catalog goes away, and the
   // catalog is the worker's, so deleting the scanner does both
   delete cat_db;
   delete scanner;
   cat_db     = NULL;
   cat_lo     = NULL;
   scanner    = NULL;
}

/* Open the sources the current filter calls for.

   The local store is read on a worker thread, so that walking thousands of
   files overlaps the database queries instead of following them.  The
   catalog it reads into belongs to that worker: this class only ever looks
   at it through await_disk(), which waits for the worker to be between
   jobs first.
*/
void US_DataModel::open_catalogs( )
{
   close_catalogs();

   use_db     = ( db != NULL  &&  filt_source != "Local Only" );
   use_lo     = ( filt_source != "DB Only" );

   QString error;

   if ( use_db )
   {
      cat_db     = new US_DataCatalog( this );

      connect( cat_db, &US_DataCatalog::message,
               this,   &US_DataModel::scanner_note );
      connect( cat_db, &US_DataCatalog::runChanged,
               this,   &US_DataModel::catalog_run_changed );

      if ( ! cat_db->attach( db, error ) )
      {
         scan_error( tr( "The database could not be read: %1" ).arg( error ) );
         db_failed  = true;
         delete cat_db;
         cat_db     = NULL;
         use_db     = false;
      }
   }

   if ( use_lo )
   {
      scanner    = new US_DiskScan( this );

      // The worker has no job yet, so its catalog may be set up from here
      if ( case_set )
         scanner->catalog()->setFileNameCase( file_case );

      connect( scanner, &US_DiskScan::message,
               this,    &US_DataModel::scanner_note );
   }
}

/* Wait for the disk worker to be between jobs, keeping the window alive.

   Everything this class reads out of the local catalog goes through here
   first, which is what makes reading it safe: the worker publishes what it
   read before it goes idle.
*/
void US_DataModel::await_disk( void )
{
   if ( scanner == NULL )   return;

   bool waited = scanner->busy();

   if ( waited  &&  progress != NULL )
      progress->setMaximum( 0 );   // a bar with no range: something is going
                                   // on, and how far along is not knowable

   while ( scanner->busy() )
   {
      // User input is left out on purpose: it is what would call back into
      // this object, and this object is between two threads just now
      qApp->processEvents( QEventLoop::ExcludeUserInputEvents, 10 );
      QThread::msleep( 1 );
   }

   if ( waited  &&  progress != NULL )
   {
      progress->setMaximum( 1 );
      progress->setValue  ( 1 );
   }

   cat_lo = scanner->catalog();
}

// Whether the source filter excludes a tree in the given state
bool US_DataModel::excluded_tree( int state ) const
{
   if ( ! filt_source.startsWith( "Exclude" ) )
      return false;

   bool isDba = ( ( state & REC_DB ) != 0 );
   bool isLoc = ( ( state & REC_LO ) != 0 );

   if ( isDba  &&  isLoc )
      return false;                            // in both: never excluded

   if ( ! isDba  &&  ! isLoc )
      return false;                            // a filler: never excluded

   return filt_source.contains( "DB" ) ? isDba : isLoc;
}

// How many records the first layer says hang off an experiment
//
// A source that cannot count them without walking the chain says so with
// -1, and that travels all the way to the tree, which shows a question mark
// rather than a wrong number.
int US_DataModel::run_record_count( const US_DataCatalog::Run& run )
{
   if ( run.rawCount   < 0  ||  run.editCount  < 0  ||
        run.modelCount < 0  ||  run.noiseCount < 0 )
      return -1;

   return run.rawCount + run.editCount + run.modelCount + run.noiseCount;
}

// Read the first layer:  one record per experiment
void US_DataModel::scan_runs( )
{
   if ( busy > 0 )   return;

   BusyGuard guard( busy );

   ddescs   .clear();
   ldescs   .clear();
   clear_rows();
   runents  .clear();
   run_queue.clear();
   ver_queue.clear();
   batch_runs.clear();
   stale_runs.clear();
   run_taken.clear();
   acted    .clear();
   db_gone  .clear();
   errors   .clear();
   db_failed  = false;
   demo       = false;
   kdb_recs   = 0;
   klo_recs   = 0;

   for ( int side = 0; side < 2; side++ )
   {
      orph_by_parent[ side ].clear();
      orph_branches [ side ].clear();
      orph_adopted  [ side ].clear();
      orph_taken    [ side ].clear();
      orph_size     [ side ] = 0;
   }

   orph_indexed = false;

   open_catalogs();

   note( tr( "Listing Experiments..." ) );

   if ( progress != NULL )
   {
      progress->setMaximum( 1 );
      progress->setValue  ( 0 );
   }

   bool    rfilt = ( ! filt_run.isEmpty()  &&  filt_run != "ALL" );
   QString error;

   // The local store is walked on the worker thread while the database is
   // queried here, so the two passes overlap
   if ( scanner != NULL )
      start_disk( [ this ]() { return scanner->list(); } );

   // An experiment is the same experiment in both places when it has the
   // same run identifier.  Two database experiments may share a run
   // identifier (they are told apart by their experiment IDs), so each
   // database experiment is an entry of its own, and a local run joins the
   // one of its name whose GUID -- or else whose experiment ID -- its
   // experiment file records, or failing both the first one of its name
   // that has no local run yet.
   QList< RunEntry > entries;
   QMultiMap< QString, int > byRun;

   if ( use_db )
   {
      if ( cat_db->loadRuns( error ) )
      {
         for ( int ii = 0; ii < cat_db->runCount(); ii++ )
         {
            const US_DataCatalog::Run& run = cat_db->run( ii );

            // The database never tells run IDs apart by case, and pairs
            // them with local runs so (see runKey below): a filter that
            // did would show a database run as missing from local disk,
            // and a download would write into the directory it is in
            if ( rfilt  &&  run.runID.compare( filt_run,
                                               Qt::CaseInsensitive ) != 0 )
               continue;

            RunEntry entry;
            entry.runID     = run.runID;
            entry.dbIndex   = ii;
            entry.dbCount   = run_record_count( run );
            entry.problem   = run.usable ? QString()
                            : tr( "database: %1" ).arg( run.problem );
            entries << entry;
         }
      }

      else
      {
         db_failed  = true;
         scan_error( tr( "The database experiments could not be listed: %1" )
                     .arg( error ) );
      }
   }

   await_disk();                  // the worker has finished its listing

   // A results directory that is not there is an empty store, as on a new
   // installation: nothing is created, and it is not an error
   if ( use_lo  &&  scanner != NULL  &&  ! scanner->ok()  &&
        QDir( US_Settings::resultDir() ).exists() )
      scan_error( tr( "The local results could not be listed: %1" )
                  .arg( scanner->error() ) );

   // A run ID names the same run in both places as the local store compares
   // names: where they ignore case, "runa" on disk is the database's "RunA"
   // (the database never tells the two apart)
   Qt::CaseSensitivity lcase = ( cat_lo != NULL ) ? cat_lo->fileNameCase()
                                                  : Qt::CaseSensitive;
   auto runKey = [ lcase ]( const QString& runID )
   { return ( lcase == Qt::CaseInsensitive ) ? runID.toLower() : runID; };

   for ( int ii = 0; ii < entries.size(); ii++ )
      byRun.insert( runKey( entries[ ii ].runID ), ii );

   // How many local directories hold each run
   QHash< QString, int > lodirs;

   for ( int ii = 0; cat_lo != NULL  &&  ii < cat_lo->runCount(); ii++ )
      lodirs[ cat_lo->run( ii ).runID ]++;

   if ( use_lo  &&  cat_lo != NULL )
   {
      // Each local run takes the database experiment of its name its
      // experiment file records by GUID, or else by ID, or else the first
      // one of its name nobody took.  The best matches are made first, over
      // all the local runs, so a run that matches by order alone does not
      // take the experiment another run records.  An ID says something only
      // in the database it came from: a run whose file records a GUID that
      // matches none of these experiments came from another one, and its ID
      // is not looked at.
      QList< int > lruns;

      for ( int ii = 0; ii < cat_lo->runCount(); ii++ )
      {
         // The run filter compares as the local store compares names, and
         // a local run that pairs with a database run it kept is kept too:
         // its records would otherwise read as in the database only, and a
         // download would write copies of them into its directory
         QString key = runKey( cat_lo->run( ii ).runID );

         if ( rfilt  &&  key != runKey( filt_run )  &&  ! byRun.contains( key ) )
            continue;

         lruns << ii;
      }

      auto rank = [ & ]( int ii, int cand )
      {
         const US_DataCatalog::Run& run  = cat_lo->run( ii );
         const RunEntry&            cent = entries[ cand ];

         if ( cent.dbIndex < 0  ||  cat_db == NULL )   return 1;

         const US_DataCatalog::Run& dbr = cat_db->run( cent.dbIndex );
         QString loGUID = run.guid.simplified();
         QString loID   = run.id.simplified();
         bool    hasGUID = ( loGUID.length() == 36 );

         if ( hasGUID  &&
              loGUID.compare( dbr.guid.simplified(), Qt::CaseInsensitive ) == 0 )
            return 3;

         if ( ! hasGUID  &&  ! loID.isEmpty()  &&  loID != "-1"  &&
              loID == dbr.id.simplified() )
            return 2;

         return 1;
      };

      QHash< int, int > lmatch;               // local index -> entry

      for ( int tier = 3; tier >= 1; tier-- )
      {
         for ( int kk = 0; kk < lruns.size(); kk++ )
         {
            int ii = lruns[ kk ];

            if ( lmatch.contains( ii ) )   continue;

            QList< int > same = byRun.values( runKey( cat_lo->run( ii ).runID ) );
            std::sort( same.begin(), same.end() );

            for ( int jj = 0; jj < same.size(); jj++ )
            {
               if ( entries[ same[ jj ] ].loIndex >= 0 )   continue;   // taken

               if ( rank( ii, same[ jj ] ) >= tier )
               {
                  lmatch.insert( ii, same[ jj ] );
                  entries[ same[ jj ] ].loIndex = ii;
                  break;
               }
            }
         }
      }

      for ( int kk = 0; kk < lruns.size(); kk++ )
      {
         int ii = lruns[ kk ];
         const US_DataCatalog::Run& run = cat_lo->run( ii );
         int  match = lmatch.value( ii, -1 );

         if ( match < 0 )
         {  // A run of no database experiment, or of one taken already
            RunEntry entry;
            entry.runID = run.runID;
            match       = entries.size();
            entries << entry;
         }

         entries[ match ].loIndex = ii;
         entries[ match ].loCount = run_record_count( run );

         // A directory not named after its run, or one of two holding the
         // same run: what an upload or a download writes would not go
         // where the run is.  Names compare as the catalog compares them,
         // so this agrees with what it says is wrong with the run.
         QString dname  = QDir( run.dirPath ).dirName();
         bool    misdir = ( dname.compare( run.runID,
                                           cat_lo->fileNameCase() ) != 0 );
         bool    twodir = ( lodirs.value( run.runID ) > 1 );

         if ( misdir  ||  twodir )
            entries[ match ].unsafe = true;

         QString lprob = run.usable ? QString() : run.problem;

         // A run that is refused always says why, whatever the catalog said
         if ( lprob.isEmpty()  &&  misdir )
            lprob = tr( "The directory %1 holds the files of run %2" )
                    .arg( dname, run.runID );

         if ( lprob.isEmpty()  &&  twodir )
            lprob = tr( "Run %1 is in more than one directory" )
                    .arg( run.runID );

         if ( ! lprob.isEmpty() )
         {
            lprob = tr( "local %1: %2" ).arg( dname ).arg( lprob );
            QString& prob = entries[ match ].problem;
            prob = prob.isEmpty() ? lprob : prob + "; " + lprob;
         }
      }
   }

   // A stable alphabetic order of the experiments
   QList< int > order;

   for ( int ii = 0; ii < entries.size(); ii++ )
      order << ii;

   std::stable_sort( order.begin(), order.end(),
      [ &entries ]( int a, int b )
      { return entries[ a ].runID < entries[ b ].runID; } );

   for ( int ii = 0; ii < order.size(); ii++ )
   {
      RunEntry entry = entries[ order[ ii ] ];
      int      state = ( entry.dbIndex >= 0 ? REC_DB : 0 )
                     | ( entry.loIndex >= 0 ? REC_LO : 0 );

      if ( excluded_tree( state ) )   continue;

      entry.row      = adescs.size();

      append_row( run_datadesc( entry ), runents.size() );
      runents   << entry;
      run_queue << ( runents.size() - 1 );
   }

   if ( progress != NULL )
      progress->setValue( 1 );

   note( tr( "%1 Experiments Listed" ).arg( runents.size() ) );
DbgLv(1) << "ScnR: experiments" << runents.size() << nowTime();
}

// Number of experiments the first layer found, and the stand-ins
int US_DataModel::runCount( ) const
{
   return runents.size();
}

// Number of real experiments
int US_DataModel::experimentCount( ) const
{
   int count = 0;

   for ( int ii = 0; ii < runents.size(); ii++ )
      count += runents.at( ii ).orphans ? 0 : 1;

   return count;
}

US_DataModel::Counts::Counts()
{
   for ( int ii = 0; ii < 5; ii++ )
   {
      db[ ii ] = 0;
      lo[ ii ] = 0;
   }
}

void US_DataModel::Counts::add( const Counts& other )
{
   for ( int ii = 0; ii < 5; ii++ )
   {
      db[ ii ] += other.db[ ii ];
      lo[ ii ] += other.lo[ ii ];
   }
}

// Count the records of one experiment's row range, or of every row
US_DataModel::Counts US_DataModel::count_records( int index ) const
{
   Counts counts;
   int    first = 0;
   int    last  = adescs.size() - 1;

   if ( index >= 0 )
   {
      if ( index >= runents.size() )   return counts;

      first = runents.at( index ).firstRow;
      last  = runents.at( index ).lastRow;
   }

   for ( int row = first; row >= 0  &&  row <= last; row++ )
   {
      if ( row_run.at( row ) < 0 )   continue;   // left behind by a re-merge

      const DataDesc& desc = adescs.at( row );
      int ityp = desc.recType;

      if ( ityp < RAW  ||  ityp > NOISE )   continue;

      counts.db[ ityp ] += ( ( desc.recState & REC_DB ) != 0 ) ? 1 : 0;
      counts.lo[ ityp ] += ( ( desc.recState & REC_LO ) != 0 ) ? 1 : 0;
   }

   return counts;
}

// One experiment entry, by position
US_DataModel::RunEntry US_DataModel::run_entry( int index ) const
{
   if ( index < 0  ||  index >= runents.size() )
      return RunEntry();

   return runents.at( index );
}

// Position of an experiment, by run identifier
//
// As the catalogs find one: the exact run ID first, then one that differs
// only in case -- the database never tells those apart, nor does a local
// store whose file names ignore case
int US_DataModel::index_of_run( const QString& runID ) const
{
   for ( int ii = 0; ii < runents.size(); ii++ )
      if ( runents.at( ii ).runID == runID )
         return ii;

   for ( int ii = 0; ii < runents.size(); ii++ )
      if ( ! runents.at( ii ).orphans  &&
           runents.at( ii ).runID.compare( runID, Qt::CaseInsensitive ) == 0 )
         return ii;

   return -1;
}

// The experiment a row belongs to: looked up, not searched for, since an
// action asks this of every row it touches
int US_DataModel::run_of_row( int row ) const
{
   if ( row < 0  ||  row >= row_run.size() )   return -1;

   return row_run.at( row );
}

// Append a row of an experiment, and say whose it is
//
// A record's parent is the nearest row above it, of its own experiment, of
// a lower level: the row above, or one of that row's ancestors.  So it is
// found in a step or four, and kept, rather than searched for each time.
void US_DataModel::append_row( const DataDesc& desc, int runIndex )
{
   int prnt = -1;
   int ityp = desc.recType;

   if ( ityp > RAW )
   {
      int prev = adescs.size() - 1;

      while ( prev >= 0  &&  row_run.at( prev ) == runIndex  &&
              adescs.at( prev ).recType >= ityp )
         prev = row_parent.at( prev );

      if ( prev >= 0  &&  row_run.at( prev ) == runIndex  &&
           adescs.at( prev ).recType >= RAW )
         prnt = prev;
   }

   adescs     << desc;
   row_run    << runIndex;
   row_parent << prnt;
   row_file   << desc.filename;
   row_dbid   << desc.recordID;
}

// Clear the rows, and the indexes kept of them
void US_DataModel::clear_rows( void )
{
   adescs    .clear();
   row_run   .clear();
   row_parent.clear();
   row_file  .clear();
   row_dbid  .clear();
   touched   .clear();
}

// The row of a record's parent
int US_DataModel::parent_row( int row ) const
{
   int index = run_of_row( row );

   if ( index < 0 )   return -1;

   const RunEntry& entry = runents.at( index );
   int  ityp = adescs.at( row ).recType;

   if ( row == entry.row  ||  ityp <= EXPERIMENT )
      return -1;

   if ( ityp == RAW )
      return entry.row;

   // The records are in tree order, so the parent is the nearest record
   // above of a lower level, found when the row was appended
   return row_parent.value( row, -1 );
}

// The rows of every descendant of a row
QVector< int > US_DataModel::descendant_rows( int row ) const
{
   QVector< int > rows;
   int index = run_of_row( row );

   if ( index < 0 )   return rows;

   const RunEntry& entry = runents.at( index );

   if ( ! entry.loaded  ||  entry.firstRow < 0 )
      return rows;

   if ( row == entry.row )
   {  // An experiment: everything in its range
      for ( int jj = entry.firstRow; jj <= entry.lastRow; jj++ )
         rows << jj;

      return rows;
   }

   int ityp = adescs.at( row ).recType;

   for ( int jj = row + 1; jj <= entry.lastRow; jj++ )
   {
      if ( adescs.at( jj ).recType <= ityp )   break;

      rows << jj;
   }

   return rows;
}

// The run ID a row belongs to
QString US_DataModel::run_id_of_row( int row ) const
{
   if ( row < 0  ||  row >= adescs.size() )   return QString();

   int index = run_of_row( row );

   if ( index >= 0  &&  ! runents.at( index ).orphans )
      return runents.at( index ).runID;

   // An orphan: the run its file or its description names
   const DataDesc& desc = adescs.at( row );
   QString fname = desc.filename.section( "/", -1, -1 );

   if ( desc.recType == RAW  &&  fname.endsWith( ".auc" ) )
      return US_DataCatalog::runIdOfFile( fname, false );

   if ( desc.recType == EDIT  &&  fname.endsWith( ".xml" ) )
      return US_DataCatalog::runIdOfFile( fname, true );

   // runID.triple.analysis.model, or .ti_noise / .ri_noise
   if ( desc.description.count( "." ) >= 3 )
      return desc.description.section( ".", 0, -4 );

   return desc.description.section( ".", 0, 0 );
}

// Ask for an experiment to be read before the others
void US_DataModel::request_run( int index )
{
   if ( index < 0  ||  index >= runents.size() )   return;
   if ( runents.at( index ).loaded )               return;

   run_queue.removeAll( index );
   run_queue.prepend  ( index );

   if ( ! scanning )
      emit workRequested();            // nobody else will get to it
}


// The next experiment waiting to be read
int US_DataModel::next_pending_run( ) const
{
   return run_queue.isEmpty() ? -1 : run_queue.first();
}

// How many experiments are still waiting to be read
int US_DataModel::pending_runs( ) const
{
   return run_queue.size();
}

// Whether a client error number says the connection to the server is gone:
// CR_SERVER_GONE_ERROR (2006), CR_SERVER_LOST (2013) or
// CR_SERVER_LOST_EXTENDED (2055), which is what a TLS connection that breaks
// reports
bool US_DataModel::connection_lost( int serverErrno )
{
   return ( serverErrno == 2006  ||  serverErrno == 2013  ||
            serverErrno == 2055 );
}

// Read the second layer of one experiment and merge it into the whole
bool US_DataModel::scan_run( int index )
{
   if ( must_queue() )
   {
      request_run( index );
      return false;
   }

   return do_scan_run( index );
}

bool US_DataModel::do_scan_run( int index )
{
   if ( index < 0  ||  index >= runents.size() )   return false;

   BusyGuard guard( busy );
   RunEntry  entry = runents.at( index );

   if ( entry.loaded )
   {
      run_queue.removeAll( index );
      return true;
   }

   // do_merge_run() reads what is not read yet, on either side
   return do_merge_run( index );
}

/* Make sure both sources have read the chain of one experiment.

   A side that cannot be read is not the same thing as a side that holds
   nothing: merging the other side alone would show every record as in that
   place only, and a download would then overwrite the local copies, or
   write model and noise files that duplicate the GUIDs of the ones there.
   So the read is tried once more, which helps when what made it fail
   passes -- a lock the server waited on too long, a file being written --
   and when it still fails the experiment is reported and left unread:
   opening it later tries again.  A lost database connection is not tried
   again: the connection is not made anew (US_DB2 does not reconnect), so
   every read after it fails too, and the message says to open the window
   again.
*/
bool US_DataModel::load_sides( int index )
{
   const RunEntry entry = runents.at( index );
   bool ok = true;

   for ( int side = 0; side < 2; side++ )
   {
      int cx = ( side == 0 ) ? entry.dbIndex : entry.loIndex;

      if ( cx < 0 )   continue;

      if ( side == 1 )
         await_disk();                 // the worker is done with its catalog

      US_DataCatalog* catalog = ( side == 0 ) ? cat_db : cat_lo;

      if ( catalog == NULL  ||  cx >= catalog->runCount() )   continue;

      if ( catalog->isRunLoaded( cx ) )   continue;

      QString error;

      if ( catalog->loadRunDetail( cx, error ) )   continue;

      bool lost = ( side == 0  &&  db != NULL  &&
                    connection_lost( db->lastServerErrno() ) );

      DbgLv(1) << "LoadSides: retry" << entry.runID << side << lost << error;

      if ( ! lost  &&  catalog->loadRunDetail( cx, error ) )   continue;

      ok = false;

      if ( lost )
         scan_error( tr( "%1: the connection to the database was lost, so"
                         " the experiment is not shown.  This window does"
                         " not connect again: close it and open it again."
                         "  (%2)" ).arg( entry.runID ).arg( error ) );

      else
         scan_error( ( side == 0 )
            ? tr( "%1: the database records could not be read, so the"
                  " experiment is not shown; open it to try again: %2" )
              .arg( entry.runID ).arg( error )
            : tr( "%1: the local records could not be read, so the"
                  " experiment is not shown; open it to try again: %2" )
              .arg( entry.runID ).arg( error ) );
   }

   return ok;
}

/* Read the chain of every experiment at once.

   Asking the database one experiment at a time is four round trips per
   experiment, each of which asks the server to hash that experiment's data
   blobs.  The bulk form lists the whole store in four queries and hashes
   nothing; the local store is the single pass it always was.
*/
bool US_DataModel::scan_all( void )
{
   if ( busy > 0 )   return false;

   BusyGuard guard( busy );
   QString   error;

   // The orphans are what this read finds, so they are indexed afresh
   orph_indexed = false;

   // Again the local store is walked on the worker thread while the
   // database is queried here
   if ( scanner != NULL )
      start_disk( [ this ]() { return scanner->readChain(); } );

   bool ok = true;

   // An older server is read with the whole-store procedures it has, so a
   // failure here is a real one; the scan then reads one experiment at a
   // time, which may still get some of them
   // Not when the listing failed: every record would read as an orphan
   if ( cat_db != NULL  &&  ! db_failed  &&  ! cat_db->loadAll( error ) )
   {
      scan_error( tr( "The database records could not be read in one pass: "
                      "%1" ).arg( error ) );
      ok = false;
   }

   await_disk();

   if ( scanner != NULL  &&  ! scanner->ok()  &&
        QDir( US_Settings::resultDir() ).exists() )
      scan_error( tr( "The local records could not be read: %1" )
                  .arg( scanner->error() ) );

   return ok;

}

/* Merge one experiment's records into the tree.

   The records are appended, so rows already in the tree keep their
   position and the tree can be filled in as the experiments arrive.
*/
bool US_DataModel::merge_run( int index )
{
   if ( must_queue() )
   {
      request_run( index );
      return false;
   }

   return do_merge_run( index );
}

bool US_DataModel::do_merge_run( int index )
{
   if ( index < 0  ||  index >= runents.size() )   return false;

   BusyGuard guard( busy );
   RunEntry  entry = runents.at( index );

   if ( entry.loaded )
   {
      run_queue.removeAll( index );
      return true;
   }

   // Each side has to have read the experiment: one that could not is not
   // merged as if it held nothing
   if ( ! load_sides( index ) )
   {
      run_queue.removeAll( index );
      return false;
   }

   if ( progress != NULL )
   {  // one range per experiment
      progress->setMaximum( 1 );
      progress->setValue  ( 0 );
   }

   ddescs.clear();
   ldescs.clear();

   // The catalog has repaired the descriptions an older server cut short
   if ( entry.dbIndex >= 0  &&  cat_db != NULL )
      catalog_descs( cat_db, entry.dbIndex, REC_DB, ddescs );

   if ( entry.loIndex >= 0  &&  cat_lo != NULL )
      catalog_descs( cat_lo, entry.loIndex, REC_LO, ldescs );

   // A record whose parent only the other source has in this experiment is
   // an orphan of its own source -- a model downloaded while its edit was
   // not, say.  It belongs here, where its other copy and its parent are,
   // not under the stand-in.
   index_orphans();

   merging_run = index;

   for ( int pass = 0; pass < 3; pass++ )
   {
      int before = ddescs.size() + ldescs.size();

      adopt_orphans( 0, ldescs, ddescs );
      adopt_orphans( 1, ddescs, ldescs );

      if ( ddescs.size() + ldescs.size() == before )   break;
   }

   merging_run = -1;

   // What the database lost since the catalog read it is not there
   drop_db_gone( ddescs );

   kdb_recs      += ddescs.size();
   klo_recs      += ldescs.size();

   QVector< DataDesc > mdescs = kept_records( merge_descs( ddescs, ldescs ) );

   int  base  = adescs.size();

   for ( int ii = 0; ii < mdescs.size(); ii++ )
   {
      // The catalogs are what the scan read: a record an action changed
      // since is where the action left it, not where they say it is
      DataDesc desc   = mdescs.at( ii );
      bool     redone = apply_action( desc );

      append_row( desc, index );
      row_file.last() = mdescs.at( ii ).filename;   // what the catalogs say
      row_dbid.last() = mdescs.at( ii ).recordID;

      if ( redone  &&  ( desc.recState & REC_DB ) != 0  &&
           ( desc.recState & REC_LO ) != 0 )
         touched.insert( adescs.size() - 1 );   // compared on its own
   }

   bool any       = ( adescs.size() > base );
   entry.firstRow = any ? base : -1;
   entry.lastRow  = any ? adescs.size() - 1 : -1;
   entry.loaded   = true;

   runents[ index ] = entry;

   // The counts on the experiment row are now what is really under it
   recount_run( index );
   entry = runents.at( index );

   run_queue.removeAll( index );

   // A read that came with checksums on both sides (a detail read, or an
   // older server's whole-store read) has nothing left to compare later
   if ( entry.inBoth()  &&  cat_db != NULL  &&  cat_lo != NULL  &&
        cat_db->run( entry.dbIndex ).verified  &&
        cat_lo->run( entry.loIndex ).verified )
      apply_digests( index );

   update_run_row( index );

   if ( progress != NULL )
      progress->setValue( 1 );

   // What this read added to experiments merged before it
   merge_stale_runs();

   return true;
}

/* The database catalog hung records under an experiment merged already.

   Noise is recorded against an edit, and belongs to its model: read with
   one experiment, it may belong to a model of another one, and the catalog
   hangs it there (US_DataCatalog::runChanged).  That experiment's rows are
   then merged again.  This is said while a read is going on, so it is only
   noted here and done once that read is over (merge_stale_runs).
*/
void US_DataModel::catalog_run_changed( int dbIndex )
{
   for ( int ii = 0; ii < runents.size(); ii++ )
   {
      const RunEntry& entry = runents.at( ii );

      if ( entry.dbIndex == dbIndex  &&  entry.loaded  &&  ! entry.orphans  &&
           ! stale_runs.contains( ii ) )
         stale_runs << ii;
   }
}

// Merge again the experiments the catalog changed after they were merged
void US_DataModel::merge_stale_runs( void )
{
   if ( remerging )   return;

   remerging = true;

   while ( ! stale_runs.isEmpty() )
   {
      int index = stale_runs.takeFirst();

      if ( remerge_run( index ) )
         emit runReloaded( index );
   }

   remerging = false;
}

/* Merge one experiment's records again.

   Its rows are appended anew and the old ones are left behind, belonging
   to no experiment, so every row number held elsewhere stays what it was;
   the tree drops the old rows' items (see runReloaded).  The orphans it
   had taken are given back first, so it takes them again.  Its records
   are compared afresh.
*/
bool US_DataModel::remerge_run( int index )
{
   if ( index < 0  ||  index >= runents.size() )   return false;

   RunEntry& entry = runents[ index ];

   if ( ! entry.loaded  ||  entry.orphans )        return false;

   for ( int row = entry.firstRow; row >= 0  &&  row <= entry.lastRow; row++ )
   {
      row_run[ row ] = -1;
      touched.remove( row );
   }

   QList< QPair< int, QString > > taken = run_taken.take( index );

   for ( int ii = 0; ii < taken.size(); ii++ )
   {
      int side = taken[ ii ].first;
      orph_taken[ side ].remove( taken[ ii ].second );

      for ( int bx = 0; bx < orph_branches[ side ].size(); bx++ )
         if ( branch_key( orph_branches[ side ][ bx ].first() )
              == taken[ ii ].second )
            orph_adopted[ side ][ bx ] = false;
   }

   kdb_recs      -= qMax( entry.dbCount, 0 );
   klo_recs      -= qMax( entry.loCount, 0 );
   entry.loaded      = false;
   entry.verified    = false;
   entry.catVerified = false;
   entry.firstRow    = -1;
   entry.lastRow     = -1;

   bool ok = do_merge_run( index );

   request_verify( index );

   return ok;
}

// ------------------------------------------------------------ verification

bool US_DataModel::run_verified( int index ) const
{
   if ( index < 0  ||  index >= runents.size() )   return false;

   return runents.at( index ).verified;
}

// Queue every experiment worth comparing.  What was asked for already --
// an experiment the user opened while the chain was read -- stays in front.
int US_DataModel::queue_verifies( void )
{
   QList< int > queue;

   for ( int ii = 0; ii < ver_queue.size(); ii++ )
   {
      int index = ver_queue.at( ii );

      if ( needs_verify( index )  &&  ! queue.contains( index ) )
         queue << index;
   }

   for ( int ii = 0; ii < runents.size(); ii++ )
   {
      if ( needs_verify( ii )  &&  ! queue.contains( ii ) )
         queue << ii;
   }

   ver_queue = queue;

   return ver_queue.size();
}

// Whether an experiment holds records in both places not compared yet
bool US_DataModel::needs_verify( int index ) const
{
   if ( index < 0  ||  index >= runents.size() )   return false;

   const RunEntry& entry = runents.at( index );

   if ( entry.verified )                            return false;

   // One not read yet can be asked for -- it is read first -- when it is
   // in both places
   if ( ! entry.loaded )                            return entry.inBoth();

   // An experiment in both places is compared as a whole
   if ( entry.inBoth()  &&  ! entry.catVerified )   return true;

   // Otherwise the records that are in both and not compared: a stand-in's,
   // a record adopted from the other source, or one an action changed
   for ( int row = entry.firstRow; row >= 0  &&  row <= entry.lastRow; row++ )
   {
      int state = adescs.at( row ).recState;

      if ( ( state & REC_DB ) != 0  &&  ( state & REC_LO ) != 0  &&
           ( state & HV_DET ) == 0 )
         return true;
   }

   return false;
}

void US_DataModel::request_verify( int index )
{
   if ( ! needs_verify( index ) )                   return;

   ver_queue.removeAll( index );
   ver_queue.prepend  ( index );

   if ( ! scanning )
      emit workRequested();
}

int US_DataModel::next_pending_verify( void ) const
{
   return ver_queue.isEmpty() ? -1 : ver_queue.first();
}

int US_DataModel::pending_verifies( void ) const
{
   return ver_queue.size();
}

// Do the first piece of the work queued outside a scan
bool US_DataModel::pump_once( void )
{
   if ( must_queue() )   return false;

   int index = next_pending_run();

   if ( index >= 0 )
   {
      bool was = runents.at( index ).loaded;
      bool ok  = do_scan_run( index );

      run_queue.removeAll( index );

      if ( ok  &&  ! was )
         emit runMerged( index );

      return true;
   }

   index = next_pending_verify();

   if ( index >= 0 )
   {
      if ( do_verify_run( index ) )
         emit runCompared( index );

      ver_queue.removeAll( index );
      return true;
   }

   return false;
}

// A record was uploaded, downloaded or removed
void US_DataModel::record_changed( int row )
{
   int index = run_of_row( row );

   if ( index < 0 )   return;

   const DataDesc& desc = adescs.at( row );

   // Where the action left it, for a merge from the catalogs to come: by
   // the local file and the database record the row was merged with
   QString key = action_key( desc );

   if ( ! key.isEmpty() )
   {
      Acted done;
      done.desc       = desc;
      done.mergedFile = row_file.value( row );
      done.mergedID   = row_dbid.value( row, -1 );

      QList< Acted >& list = acted[ key ];

      for ( int ii = list.size() - 1; ii >= 0; ii-- )
         if ( list.at( ii ).mergedFile == done.mergedFile  &&
              list.at( ii ).mergedID   == done.mergedID )
            list.removeAt( ii );

      list << done;
   }

   if ( ( desc.recState & REC_DB ) != 0  &&
        ( desc.recState & REC_LO ) != 0 )
   {  // Two copies that were not compared with each other
      adescs[ row ].recState &= ~HV_DET;
      adescs[ row ].contents  = QString();
      touched.insert( row );
      runents[ index ].verified = false;
   }

   else
      touched.remove( row );

   // Each of these walks the experiment's rows, so in a batch they are done
   // once per experiment at its end rather than once per record
   if ( batch_depth > 0 )
   {
      if ( ! batch_runs.contains( index ) )
         batch_runs << index;

      return;
   }

   run_changed( index );
}

// What a record an action changed is known by: its type and GUID
QString US_DataModel::action_key( const DataDesc& desc )
{
   QString guid = desc.dataGUID.simplified().toLower();

   if ( guid.isEmpty()  ||  guid == dmyGUID )   return QString();

   return QString::number( desc.recType ) + ":" + guid;
}

/* Put back what an action did to a record the catalogs read before it.

   An action changes the row, not the catalogs, so a merge from them --
   an experiment merged again when a later read hung noise under it, a
   branch of orphans adopted -- would show a removed record as present, a
   downloaded one as in the database only, an uploaded one as local only.
   Where the record is, its IDs and its file are taken from the last
   action on it; it is compared again.
*/
bool US_DataModel::apply_action( DataDesc& desc ) const
{
   QString key = action_key( desc );

   if ( key.isEmpty()  ||  ! acted.contains( key ) )   return false;

   // The same record: the same local file, or none, and the same database
   // record, or none, as the acted row was merged with.  A copy of it with
   // the same GUID -- in a run directory copied under another name, or a
   // second local file of it -- is another record.  Model and noise files
   // are not a run's: one file shown in two experiments is one record.
   const QList< Acted >& list  = acted[ key ];
   int                   found = -1;

   for ( int ii = list.size() - 1; ii >= 0  &&  found < 0; ii-- )
   {
      const Acted& aa = list.at( ii );

      if ( aa.mergedFile == desc.filename  &&  aa.mergedID == desc.recordID )
         found = ii;
   }

   if ( found < 0 )   return false;

   const DataDesc& done = list.at( found ).desc;

   desc.recordID    = done.recordID;
   desc.recState    = done.recState & ~HV_DET;
   desc.filename    = done.filename;
   desc.filemodDate = done.filemodDate;
   desc.lastmodDate = done.lastmodDate;
   desc.contents    = QString();

   return true;
}

// A record is gone from the database: no later merge shows it there
void US_DataModel::removed_from_db( int recType, int recordID )
{
   if ( recordID < 0 )   return;

   db_gone.insert( QString::number( recType ) + ":"
                   + QString::number( recordID ) );
}

/* Leave out the catalog's database records that are gone since it read them.

   The catalogs are not read again after an action, so an experiment read
   after it -- one the scan had not reached, or one merged again -- would
   show what an action removed from the database as still there, and so
   would what the database removed with it: an edit's models, and a model's
   noise, wherever the catalog has them.  Those are the records whose parent
   (by database ID) is gone; the parents come first in the list, and a
   record left out takes its own descendants with it.
*/
void US_DataModel::drop_db_gone( QVector< DataDesc >& descs )
{
   if ( db_gone.isEmpty() )   return;

   auto key = []( int recType, int recordID )
   {
      return QString::number( recType ) + ":" + QString::number( recordID );
   };

   QVector< DataDesc > kept;

   for ( int ii = 0; ii < descs.size(); ii++ )
   {
      const DataDesc& desc = descs.at( ii );

      if ( desc.recordID >= 0  &&  desc.recType >= RAW  &&
           ( db_gone.contains( key( desc.recType, desc.recordID ) )  ||
             db_gone.contains( key( desc.recType - 1, desc.parentID ) ) ) )
      {
         db_gone.insert( key( desc.recType, desc.recordID ) );
         continue;
      }

      kept << desc;
   }

   descs = kept;
}

// Bring an experiment up to date after records of it changed
void US_DataModel::run_changed( int index )
{
   update_run_row( index );
   recount_run   ( index );
   request_verify( index );
}

void US_DataModel::begin_changes( void )
{
   batch_depth++;
}

QList< int > US_DataModel::end_changes( void )
{
   if ( batch_depth <= 0 )     return QList< int >();
   if ( --batch_depth > 0 )    return QList< int >();

   QList< int > runs = batch_runs;
   batch_runs.clear();

   for ( int ii = 0; ii < runs.size(); ii++ )
      run_changed( runs.at( ii ) );

   return runs;
}

// Count again what is under an experiment, in each place
void US_DataModel::recount_run( int index )
{
   if ( index < 0  ||  index >= runents.size() )   return;

   RunEntry& entry = runents[ index ];

   if ( ! entry.loaded )   return;            // the listing's counts stand

   entry.dbCount = 0;
   entry.loCount = 0;

   for ( int row = entry.firstRow; row >= 0  &&  row <= entry.lastRow; row++ )
   {
      int state = adescs.at( row ).recState;
      entry.dbCount += ( ( state & REC_DB ) != 0 ) ? 1 : 0;
      entry.loCount += ( ( state & REC_LO ) != 0 ) ? 1 : 0;
   }
}

// The checksum of one local file
QString US_DataModel::file_digest( const QString& path )
{
   if ( path.isEmpty()  ||  ! QFileInfo( path ).isFile() )
      return QString();

   // "0 0" is what md5sum_file() gives for a file it could not read
   QString digest = US_Util::md5sum_file( path );

   return ( digest == "0 0" ) ? QString() : digest;
}

/* The checksum of a record's local file, as it compares with the database.

   A model or noise file is XML, which the database holds with LF line
   ends: written on Windows (US_Model::write rewrites the file a model is
   uploaded from) it has CRLF ones, and would never compare equal.  Its
   line ends are made LF before it is hashed.  Raw and edit files are
   compared byte for byte, as the server hashes their blobs.
*/
QString US_DataModel::local_digest( const DataDesc& desc )
{
   if ( desc.recType != MODEL  &&  desc.recType != NOISE )
      return file_digest( desc.filename );

   QFile file( desc.filename );

   if ( desc.filename.isEmpty()  ||  ! file.open( QIODevice::ReadOnly ) )
      return QString();

   QByteArray bytes = file.readAll();

   if ( file.error() != QFileDevice::NoError )
      return QString();

   bytes.replace( "\r\n", "\n" );

   return QString( QCryptographicHash::hash( bytes, QCryptographicHash::Md5 )
                   .toHex() ) + " " + QString::number( bytes.size() );
}

// The checksum of one database record, read on its own
bool US_DataModel::db_digest( IUS_DB2* dbP, const DataDesc& desc,
                              QString& digest, QString& error )
{
   digest.clear();
   error .clear();

   if ( dbP == NULL  ||  desc.recordID < 0 )
   {
      error = QObject::tr( "no database record to read" );
      return false;
   }

   QStringList query;
   QString     recID = QString::number( desc.recordID );
   int         xcol  = -1;       // the column of a record's XML, if hashed here
   int         ccol  = -1;       // the column of the server's checksum

   switch ( desc.recType )
   {
      case RAW:    query << "get_rawData"     << recID;  ccol = 8;  break;
      case EDIT:   query << "get_editedData"  << recID;  ccol = 6;  break;
      case MODEL:  query << "get_model_info"  << recID;  xcol = 2;  break;
      case NOISE:  query << "get_noise_info"  << recID;  xcol = 5;  break;
      default:
         error = QObject::tr( "no checksum for a record of type %1" )
                 .arg( desc.recType );
         return false;
   }

   dbP->query( query );

   if ( dbP->lastErrno() != IUS_DB2::OK  ||  ! dbP->next() )
   {
      error = QObject::tr( "%1 %2: %3" ).arg( query[ 0 ] ).arg( recID )
              .arg( dbP->lastError() );
      return false;
   }

   if ( ccol >= 0 )
   {
      QString cksum = dbP->value( ccol     ).toString();
      QString size  = dbP->value( ccol + 1 ).toString();

      if ( cksum.isEmpty() )
      {
         error = QObject::tr( "%1 %2 gave no checksum" ).arg( query[ 0 ] )
                 .arg( recID );
         return false;
      }

      digest = cksum + " " + size;
      return true;
   }

   // The server hashes the same XML when it lists these, as MD5 over its
   // bytes, so the hash of what it returns is the one it would give.  Its
   // line ends are made LF, as the local file's are (local_digest): a
   // record uploaded from a file with CRLF ones holds those, and is the
   // same record all the same
   QByteArray xml = dbP->value( xcol ).toString().toUtf8();
   xml.replace( "\r\n", "\n" );
   digest = QString( QCryptographicHash::hash( xml, QCryptographicHash::Md5 )
                     .toHex() ) + " " + QString::number( xml.size() );

   return true;
}

// The checksum of every record of one catalog run, by GUID
QMap< QString, QString > US_DataModel::run_digests( US_DataCatalog* catalog,
                                                    int index,
                                                    QSet< QString >* present )
{
   QMap< QString, QString > digests;

   if ( catalog == NULL  ||  index < 0 )   return digests;

   const US_DataCatalog::Run& run = catalog->run( index );

   // The rows carry their GUIDs simplified, so the keys are too
   auto add = [ & ]( const QString& guid, const QString& checksum,
                     const QString& size )
   {
      QString key = guid.simplified();

      if ( present != NULL )
         present->insert( key );

      if ( ! checksum.isEmpty() )
         digests.insert( key, checksum + " " + size );
   };

   for ( int ii = 0; ii < run.raws.size(); ii++ )
   {
      const US_DataCatalog::Raw& raw = run.raws.at( ii );

      add( raw.guid, raw.checksum, raw.size );

      for ( int jj = 0; jj < raw.edits.size(); jj++ )
      {
         const US_DataCatalog::Edit& edit = raw.edits.at( jj );

         add( edit.guid, edit.checksum, edit.size );

         for ( int kk = 0; kk < edit.models.size(); kk++ )
         {
            const US_DataCatalog::Model& model = edit.models.at( kk );

            add( model.guid, model.checksum, model.size );

            for ( int mm = 0; mm < model.noises.size(); mm++ )
            {
               const US_DataCatalog::Noise& noise = model.noises.at( mm );

               add( noise.guid, noise.checksum, noise.size );
            }
         }
      }
   }

   return digests;
}

/* Read the checksums of one experiment and compare them again.

   Listing a store leaves the checksums out, so up to here a record that is
   in both places reads as unverified.  This is what decides whether it is
   in sync or in conflict.
*/
bool US_DataModel::verify_run( int index )
{
   if ( must_queue() )
   {
      request_verify( index );
      return false;
   }

   return do_verify_run( index );
}

bool US_DataModel::do_verify_run( int index )
{
   if ( index < 0  ||  index >= runents.size() )   return false;

   BusyGuard guard( busy );
   RunEntry  entry = runents.at( index );

   ver_queue.removeAll( index );

   if ( entry.verified  ||  ! entry.loaded )       return false;

   // An experiment in both places is hashed as a whole, once: the local
   // files are hashed on the worker thread while the database hashes its
   // blobs, so the two halves of the comparison overlap
   if ( entry.inBoth()  &&  ! entry.catVerified  &&  ! entry.orphans )
   {
      QString error;
      bool    dbOk = true;
      bool    loOk = true;

      bool loJob = ( entry.loIndex >= 0  &&  scanner != NULL  &&
                     start_disk( [ & ]() { return scanner->verify( entry.loIndex ); } ) );

      if ( entry.dbIndex >= 0  &&  cat_db != NULL  &&
           ! cat_db->verifyRun( entry.dbIndex, error ) )
      {
         dbOk = false;
         scan_error( tr( "%1: the database copies could not be compared: %2" )
                     .arg( entry.runID ).arg( error ) );
      }

      await_disk();

      if ( entry.loIndex >= 0  &&  ( ! loJob  ||  ! scanner->ok() ) )
      {
         loOk = false;
         scan_error( tr( "%1: the local copies could not be compared: %2" )
                     .arg( entry.runID )
                     .arg( loJob ? scanner->error() : tr( "no local reader" ) ) );
      }

      if ( ! dbOk  ||  ! loOk )
         return false;               // the rows stay unverified

      apply_digests( index );
   }

   // What is left -- a stand-in's records, records adopted from the other
   // source, records an action changed -- is compared record by record
   bool ok = verify_rows( index );

   runents[ index ].verified = ok;
   update_run_row( index );

   return ok;
}

/* Compare the checksums both catalogs hold for one experiment.

   A side without a checksum is a side that could not be read, and that is
   not the same thing as the two being equal: the record is a conflict.  A
   record one catalog does not hold at all -- adopted from the other
   source's orphans, or written by an action since the scan -- is left for
   verify_rows(), which reads it on its own.
*/
void US_DataModel::apply_digests( int index )
{
   RunEntry entry = runents.at( index );

   QSet< QString > dbHeld;
   QSet< QString > loHeld;
   QMap< QString, QString > dbDigests = run_digests( cat_db, entry.dbIndex,
                                                     &dbHeld );
   QMap< QString, QString > loDigests = run_digests( cat_lo, entry.loIndex,
                                                     &loHeld );

   for ( int row = entry.firstRow; row >= 0  &&  row <= entry.lastRow; row++ )
   {
      DataDesc desc = adescs.at( row );

      bool isDba = ( ( desc.recState & REC_DB ) != 0 );
      bool isLoc = ( ( desc.recState & REC_LO ) != 0 );

      if ( ! isDba  ||  ! isLoc )   continue;   // nothing to compare

      if ( touched.contains( row )  ||
           ! dbHeld.contains( desc.dataGUID )  ||
           ! loHeld.contains( desc.dataGUID ) )
         continue;                              // read on its own

      QString dbc = dbDigests.value( desc.dataGUID );
      QString loc = loDigests.value( desc.dataGUID );

      // A model or noise file whose bytes differ may differ only in its
      // line ends: it is compared on its own (local_digest)
      if ( ( desc.recType == MODEL  ||  desc.recType == NOISE )  &&
           ! dbc.isEmpty()  &&  ! loc.isEmpty()  &&  dbc != loc )
         continue;

      desc.contents  = ( dbc.isEmpty() ? QString( "- -" ) : dbc ) + " "
                     + ( loc.isEmpty() ? QString( "- -" ) : loc );
      desc.recState |= HV_DET;

      adescs[ row ] = desc;
   }

   // Verified when nothing is left to read record by record
   runents[ index ].catVerified = true;
   runents[ index ].verified    = false;
   runents[ index ].verified    = ! needs_verify( index );

   if ( runents[ index ].verified )
      ver_queue.removeAll( index );

   update_run_row( index );
}

// Compare record by record the rows in both places not compared yet
bool US_DataModel::verify_rows( int index )
{
   const RunEntry entry = runents.at( index );
   bool  ok             = true;

   for ( int row = entry.firstRow; row >= 0  &&  row <= entry.lastRow; row++ )
   {
      DataDesc desc = adescs.at( row );

      if ( ( desc.recState & REC_DB ) == 0  ||
           ( desc.recState & REC_LO ) == 0  ||
           ( desc.recState & HV_DET ) != 0 )
         continue;

      // Written under an ID that could not be read back (see
      // US_DataProcess::record_upload): a new scan reads it
      if ( desc.recordID < 0 )
         continue;

      QString dbc;
      QString error;

      if ( ! db_digest( db, desc, dbc, error ) )
      {
         ok = false;
         scan_error( tr( "%1: the database copy of %2 could not be read: %3" )
                     .arg( entry.runID ).arg( desc.label ).arg( error ) );
         continue;                   // the row stays unverified
      }

      QString loc = local_digest( desc );

      if ( loc.isEmpty() )
      {
         ok = false;
         scan_error( tr( "%1: the local copy of %2 could not be read: %3" )
                     .arg( entry.runID ).arg( desc.label )
                     .arg( desc.filename ) );
         continue;
      }

      desc.contents  = dbc + " " + loc;
      desc.recState |= HV_DET;
      adescs[ row ]  = desc;
      touched.remove( row );
   }

   return ok;
}

// Set the experiment row's state from its records
//
// The row reads as compared once every record under it in both places has
// been, and as differing when any of them does.  A stand-in experiment is
// not a record in either place and is left as it is.
void US_DataModel::update_run_row( int index )
{
   if ( index < 0  ||  index >= runents.size() )   return;

   const RunEntry& entry = runents.at( index );

   if ( entry.orphans  ||  entry.row < 0 )         return;

   DataDesc& edesc = adescs[ entry.row ];

   if ( ( edesc.recState & REC_DB ) == 0  ||
        ( edesc.recState & REC_LO ) == 0 )
      return;                          // in one place only: nothing to say

   bool unverified = ! entry.verified;
   bool conflict   = false;

   for ( int row = entry.firstRow; row >= 0  &&  row <= entry.lastRow; row++ )
   {
      const DataDesc& desc = adescs.at( row );

      if ( ( desc.recState & REC_DB ) == 0  ||
           ( desc.recState & REC_LO ) == 0 )
         continue;

      SyncState state = sync_state( desc );

      if ( state == SYNC_UNVERIFIED )   unverified = true;
      if ( state == SYNC_CONFLICT   )   conflict   = true;
   }

   if ( unverified )
   {
      edesc.recState &= ~HV_DET;
      edesc.contents  = QString();
   }

   else
   {
      edesc.recState |= HV_DET;
      edesc.contents  = conflict ? QString( "differ 1 - -" ) : QString();
   }
}

// Start a job of the disk worker, waiting for the one before it to finish
bool US_DataModel::start_disk( const std::function< bool() >& job )
{
   if ( scanner == NULL )   return false;

   if ( job() )             return true;

   await_disk();            // it was still busy with the last one

   return job();
}


// The sync state of a record
US_DataModel::SyncState US_DataModel::sync_state( const DataDesc& desc )
{
   if ( desc.recState == NOSTAT  ||
        desc.description.contains( "-ARTIFICIAL" ) )
      return SYNC_DUMMY;

   bool isDba = ( ( desc.recState & REC_DB ) != 0 );
   bool isLoc = ( ( desc.recState & REC_LO ) != 0 );

   if ( isLoc  &&  ! isDba )   return SYNC_LOCAL;
   if ( isDba  &&  ! isLoc )   return SYNC_DB;
   if ( ! isDba )              return SYNC_DUMMY;

   if ( ( desc.recState & HV_DET ) == 0 )
      return SYNC_UNVERIFIED;

   if ( desc.recType == EXPERIMENT )
      return desc.contents.isEmpty() ? SYNC_INSYNC : SYNC_CONFLICT;

   QString cont1 = desc.contents.section( " ", 0, 1 ).simplified();
   QString cont2 = desc.contents.section( " ", 2, 4 ).simplified();

   // Compared, and one side had nothing to compare: that is a conflict
   if ( cont1.isEmpty()  ||  cont2.isEmpty()  ||
        cont1.startsWith( "-" )  ||  cont2.startsWith( "-" ) )
      return SYNC_CONFLICT;

   return ( cont1 == cont2 ) ? SYNC_INSYNC : SYNC_CONFLICT;
}

// Build the experiment record of one run entry
US_DataModel::DataDesc US_DataModel::run_datadesc( const RunEntry& entry )
{
   DataDesc desc;

   desc.recordID    = -1;
   desc.recType     = EXPERIMENT;
   desc.parentID    = -1;
   desc.recState    = NOSTAT;
   desc.subType     = "";
   desc.dataGUID    = "";
   desc.parentGUID  = "";
   desc.filename    = "";
   desc.contents    = "";
   desc.label       = entry.runID;
   desc.description = entry.runID;
   desc.filemodDate = "";
   desc.lastmodDate = "";

   if ( entry.dbIndex >= 0  &&  cat_db != NULL )
   {
      const US_DataCatalog::Run& run = cat_db->run( entry.dbIndex );

      desc.recordID    = run.id.toInt();
      desc.recState   |= REC_DB;
      desc.dataGUID    = run.guid.simplified();
      desc.parentGUID  = run.projectGUID.simplified();
      desc.parentID    = run.projectID.toInt();
      desc.subType     = run.expType;
      desc.lastmodDate = run.date;

      if ( ! run.label.isEmpty() )
         desc.description = run.label;
   }

   if ( entry.loIndex >= 0  &&  cat_lo != NULL )
   {
      const US_DataCatalog::Run& run = cat_lo->run( entry.loIndex );

      desc.recState   |= REC_LO;
      desc.filename    = run.dirPath;
      desc.filemodDate = run.date;

      if ( desc.dataGUID.isEmpty() )  desc.dataGUID = run.guid.simplified();
      if ( desc.subType .isEmpty() )  desc.subType  = run.expType;
   }

   return desc;
}

// The sub-type of a model, from its description and the size of its contents
QString US_DataModel::model_subtype( const QString& descript,
                                     const QString& recsize )
{
   const int _M_LARGE_ = 65000;   // Model size that indicates a custom grid

   QString subType = descript.section( ".", -2, -2 ).section( "_", 2, 2 );

   // Set as CUSTOMGRID if so marked or large non-MC
   if ( descript.contains( "CustomGrid" )  ||
        ( ! descript.contains( "_mc" )  &&  recsize.toLongLong() > _M_LARGE_ ) )
      subType = "CUSTOMGRID";

   // If empty subtype, mark as MANUAL
   else if ( subType.isEmpty() )
      subType = "MANUAL";

   return subType;
}

// The run type of an edit file:  runID.editID.TYPE.cell.channel.wl.xml
QString US_DataModel::edit_subtype( const QString& filename )
{
   return filename.section( "/", -1, -1 ).section( ".", -5, -5 );
}

// The label of an edit:  runID.editID.cell.channel.wavelength
QString US_DataModel::edit_label( const QString& runID,
                                  const QString& filename )
{
   QString fname  = filename.section( "/", -1, -1 );
   QString editID = fname.section( ".", -6, -6 );
   QString triple = fname.section( ".", -4, -2 );

   return runID + "." + editID + "." + triple;
}

// Parse a record date, as either source writes one
QDateTime US_DataModel::parse_date( const QString& text )
{
   QString dtext = text.simplified();

   if ( dtext.endsWith( "UTC" ) )
      dtext = dtext.left( dtext.length() - 3 ).simplified();

   if ( dtext.endsWith( "Z" ) )
      dtext.chop( 1 );

   dtext.replace( "T", " " );

   QDateTime dtime = QDateTime::fromString( dtext, "yyyy-MM-dd hh:mm:ss" );

   if ( ! dtime.isValid() )
      dtime = QDateTime::fromString( dtext, "yyyy-MM-dd hh:mm" );

   if ( ! dtime.isValid() )
      dtime = QDateTime::fromString( dtext, "yyyy-MM-dd hh:mm:ss.zzz" );

   if ( dtime.isValid() )
      dtime.setTimeZone( QTimeZone::utc() );

   return dtime;
}

// Turn the chain of one catalog run into description records
void US_DataModel::catalog_descs( US_DataCatalog* catalog, int index,
                                  int state, QVector< DataDesc >& descs )
{
   const US_DataCatalog::Run& run = catalog->run( index );

   bool tfilt = ( ! filt_triple.isEmpty()  &&  filt_triple != "ALL" );

   for ( int ii = 0; ii < run.raws.size(); ii++ )
   {
      const US_DataCatalog::Raw& raw = run.raws.at( ii );

      if ( tfilt  &&  raw.triple != filt_triple )   continue;

      append_raw( raw, run.runID, run.guid, run.id, state, descs );
   }
}

// One raw record, and everything under it
void US_DataModel::append_raw( const US_DataCatalog::Raw& raw,
                               const QString& runID, const QString& runGUID,
                               const QString& runDbID, int state,
                               QVector< DataDesc >& descs )
{
   bool isDb = ( state == REC_DB );

   DataDesc desc;
   desc.recordID    = isDb ? raw.id.toInt() : -1;
   desc.recType     = RAW;
   desc.subType     = "";
   desc.recState    = state;
   desc.dataGUID    = raw.guid.simplified();
   desc.parentGUID  = runGUID.simplified();
   desc.parentID    = isDb ? runDbID.toInt() : -1;
   desc.filename    = isDb ? raw.filename : raw.path;
   desc.contents    = raw.checksum.isEmpty() ? QString()
                      : raw.checksum + " " + raw.size;
   desc.label       = runID + "." + raw.triple;
   desc.description = raw.description.isEmpty()
                      ? raw.filename.section( ".", 0, -2 )
                      : raw.description;
   desc.filemodDate = isDb ? QString() : raw.lastUpdated;
   desc.lastmodDate = isDb ? raw.lastUpdated : QString();

   if ( desc.dataGUID.length() != 36  ||  desc.dataGUID == dmyGUID )
      desc.dataGUID = US_Util::new_guid();

   if ( desc.parentGUID.length() != 36 )
      desc.parentGUID = dmyGUID;

   descs << desc;

   for ( int jj = 0; jj < raw.edits.size(); jj++ )
      append_edit( raw.edits.at( jj ), runID, desc.dataGUID, raw.id,
                   state, descs );
}

// One edit record, and everything under it
void US_DataModel::append_edit( const US_DataCatalog::Edit& edit,
                                const QString& runID, const QString& rawGUID,
                                const QString& rawDbID, int state,
                                QVector< DataDesc >& descs )
{
   bool isDb = ( state == REC_DB );

   DataDesc edesc;
   edesc.recordID    = isDb ? edit.id.toInt() : -1;
   edesc.recType     = EDIT;
   edesc.subType     = edit_subtype( edit.filename );
   edesc.recState    = state;
   edesc.dataGUID    = edit.guid.simplified();
   edesc.parentGUID  = rawGUID.simplified();

   if ( edesc.parentGUID.length() != 36 )
      edesc.parentGUID = dmyGUID;
   edesc.parentID    = isDb ? rawDbID.toInt() : -1;
   edesc.filename    = isDb ? edit.filename : edit.path;
   edesc.contents    = edit.checksum.isEmpty() ? QString()
                       : edit.checksum + " " + edit.size;
   edesc.label       = edit_label( runID, edit.filename );
   edesc.description = edit.filename.section( "/", -1, -1 )
                                    .section( ".", 0, -2 );
   edesc.filemodDate = isDb ? QString() : edit.lastUpdated;
   edesc.lastmodDate = isDb ? edit.lastUpdated : QString();

   if ( edesc.dataGUID.length() != 36  ||  edesc.dataGUID == dmyGUID )
      edesc.dataGUID = US_Util::new_guid();

   descs << edesc;

   for ( int kk = 0; kk < edit.models.size(); kk++ )
      append_model( edit.models.at( kk ), edesc.dataGUID, edit.id,
                    state, descs );
}

// One model record, and its noise
void US_DataModel::append_model( const US_DataCatalog::Model& model,
                                 const QString& editGUID,
                                 const QString& editDbID, int state,
                                 QVector< DataDesc >& descs )
{
   bool isDb = ( state == REC_DB );

   // A listing without checksums has no size either; the size of a local
   // model is its file's, which costs nothing to ask for
   QString msize = model.size;

   if ( msize.isEmpty()  &&  ! isDb  &&  ! model.path.isEmpty() )
      msize = QString::number( QFileInfo( model.path ).size() );

   DataDesc mdesc;
   mdesc.recordID    = isDb ? model.id.toInt() : -1;
   mdesc.recType     = MODEL;
   mdesc.subType     = model_subtype( model.description, msize );
   mdesc.recState    = state;
   mdesc.dataGUID    = model.guid.simplified();
   mdesc.parentGUID  = editGUID.simplified();

   // A global model has no edit: one source says so with an empty GUID, the
   // other with the all-zero one, and the two copies have to pair
   if ( mdesc.parentGUID.length() != 36 )
      mdesc.parentGUID = dmyGUID;
   mdesc.parentID    = isDb ? editDbID.toInt() : -1;
   mdesc.filename    = isDb ? QString() : model.path;
   mdesc.contents    = model.checksum.isEmpty() ? QString()
                       : model.checksum + " " + model.size;
   mdesc.label       = short_label( model.description );
   mdesc.description = model.description;
   mdesc.filemodDate = isDb ? QString() : model.lastUpdated;
   mdesc.lastmodDate = isDb ? model.lastUpdated : QString();

   if ( mdesc.dataGUID.length() != 36  ||  mdesc.dataGUID == dmyGUID )
      mdesc.dataGUID = US_Util::new_guid();

   descs << mdesc;

   for ( int mm = 0; mm < model.noises.size(); mm++ )
      append_noise( model.noises.at( mm ), mdesc.dataGUID, model.id,
                    state, descs );
}

// One noise record
void US_DataModel::append_noise( const US_DataCatalog::Noise& noise,
                                 const QString& modelGUID,
                                 const QString& modelDbID, int state,
                                 QVector< DataDesc >& descs )
{
   bool isDb = ( state == REC_DB );

   DataDesc ndesc;
   ndesc.recordID    = isDb ? noise.id.toInt() : -1;
   ndesc.recType     = NOISE;
   ndesc.subType     = noise.noiseType.left( 2 ).toUpper();
   ndesc.recState    = state;
   ndesc.dataGUID    = noise.guid.simplified();
   ndesc.parentGUID  = modelGUID.simplified();

   if ( ndesc.parentGUID.length() != 36 )
      ndesc.parentGUID = dmyGUID;
   ndesc.parentID    = isDb ? modelDbID.toInt() : -1;
   ndesc.filename    = isDb ? QString() : noise.path;
   ndesc.contents    = noise.checksum.isEmpty() ? QString()
                       : noise.checksum + " " + noise.size;
   ndesc.label       = short_label( noise.description );
   ndesc.description = noise.description;
   ndesc.filemodDate = isDb ? QString() : noise.lastUpdated;
   ndesc.lastmodDate = isDb ? noise.lastUpdated : QString();

   if ( ndesc.dataGUID.length() != 36  ||  ndesc.dataGUID == dmyGUID )
      ndesc.dataGUID = US_Util::new_guid();

   descs << ndesc;
}

// The records of a catalog that attach to no experiment, as tree records
//
// Each one keeps the parent GUID it points at, which merge_descs() turns
// into a gray filler parent -- one per missing parent, shared by all of
// its orphans.  Each orphan is a branch of its own: it and what hangs off
// it, so it can be put somewhere as a whole.
void US_DataModel::orphan_descs( US_DataCatalog* catalog, int state,
                                 QVector< Branch >& branches )
{
   if ( catalog == NULL )   return;

   const US_DataCatalog::Orphans& orph = catalog->orphans();

   for ( int ii = 0; ii < orph.raws.size(); ii++ )
   {
      const US_DataCatalog::Raw& raw = orph.raws.at( ii );
      QString runID = raw.runID.isEmpty()
                    ? US_DataCatalog::runIdOfFile(
                         raw.filename.section( "/", -1, -1 ), false )
                    : raw.runID;

      Branch branch;
      append_raw( raw, runID, QString(), raw.experimentID, state, branch );
      branches << branch;
   }

   for ( int ii = 0; ii < orph.edits.size(); ii++ )
   {
      const US_DataCatalog::Edit& edit = orph.edits.at( ii );
      QString runID = edit.runID.isEmpty()
                    ? US_DataCatalog::runIdOfFile(
                         edit.filename.section( "/", -1, -1 ), true )
                    : edit.runID;

      Branch branch;
      append_edit( edit, runID, edit.rawGUID, QString(), state, branch );
      branches << branch;
   }

   for ( int ii = 0; ii < orph.models.size(); ii++ )
   {
      const US_DataCatalog::Model& model = orph.models.at( ii );

      Branch branch;
      append_model( model, model.editGUID, model.editID, state, branch );
      branches << branch;
   }

   for ( int ii = 0; ii < orph.noises.size(); ii++ )
   {
      const US_DataCatalog::Noise& noise = orph.noises.at( ii );

      Branch branch;
      append_noise( noise, noise.modelGUID, QString(), state, branch );
      branches << branch;
   }
}

// The key a branch is adopted by
QString US_DataModel::adopt_key( const DataDesc& head )
{
   if ( head.recType == RAW )          // its experiment is not a record here
      return QString::number( RAW ) + ":" + head.dataGUID;

   if ( head.recType <= RAW  ||  head.parentGUID.length() != 36  ||
        head.parentGUID == dmyGUID )
      return QString();                // a global model, or a broken link

   return QString::number( head.recType - 1 ) + ":" + head.parentGUID;
}

// How many orphans a catalog holds
int US_DataModel::orphan_size( US_DataCatalog* catalog )
{
   if ( catalog == NULL )   return 0;

   const US_DataCatalog::Orphans& orph = catalog->orphans();

   return orph.raws.size() + orph.edits.size() + orph.models.size()
        + orph.noises.size();
}

// What tells one orphan branch from another, from one index to the next.
// A record's GUID is not enough: one with a broken GUID is given a new one
// each time it is read.
QString US_DataModel::branch_key( const DataDesc& head )
{
   QString which = ( head.recordID >= 0 ) ? QString::number( head.recordID )
                 : head.filename;

   if ( which.isEmpty() )
      which = head.dataGUID;

   return QString::number( head.recType ) + ":" + which;
}

// The orphans of both catalogs, as branches indexed by their parent
//
// A read of one experiment at a time -- with a run filter, or when the
// whole-store read failed -- adds to the catalog the stray noise each
// experiment turns out to hold, so the index is made again whenever a
// catalog holds more orphans than it did, keeping what was adopted.
void US_DataModel::index_orphans( void )
{
   if ( use_lo )
      await_disk();

   US_DataCatalog* cats[ 2 ];
   cats[ 0 ] = ( use_db ) ? cat_db : NULL;
   cats[ 1 ] = ( use_lo ) ? cat_lo : NULL;

   if ( orph_indexed  &&
        orphan_size( cats[ 0 ] ) == orph_size[ 0 ]  &&
        orphan_size( cats[ 1 ] ) == orph_size[ 1 ] )
      return;

   orph_indexed = true;

   for ( int side = 0; side < 2; side++ )
   {
      orph_branches [ side ].clear();
      orph_by_parent[ side ].clear();
      orph_adopted  [ side ].clear();

      orphan_descs( cats[ side ], ( side == 0 ) ? REC_DB : REC_LO,
                    orph_branches[ side ] );
      orph_size[ side ] = orphan_size( cats[ side ] );
   }

   for ( int side = 0; side < 2; side++ )
   {
      orph_adopted[ side ].fill( false, orph_branches[ side ].size() );

      for ( int ii = 0; ii < orph_branches[ side ].size(); ii++ )
      {
         const DataDesc& head = orph_branches[ side ][ ii ].first();

         if ( orph_taken[ side ].contains( branch_key( head ) ) )
         {  // merged into an experiment already
            orph_adopted[ side ][ ii ] = true;
            continue;
         }

         QString key = adopt_key( head );

         if ( ! key.isEmpty() )
            orph_by_parent[ side ][ key ] << ii;
      }
   }
}

// Add the orphans of one source that attach to the other source's chain
void US_DataModel::adopt_orphans( int side, const QVector< DataDesc >& chain,
                                  QVector< DataDesc >& descs )
{
   if ( orph_by_parent[ side ].isEmpty() )   return;

   for ( int ii = 0; ii < chain.size(); ii++ )
   {
      const DataDesc& desc = chain.at( ii );
      QString key = QString::number( desc.recType ) + ":" + desc.dataGUID;
      const QList< int > found = orph_by_parent[ side ].value( key );

      for ( int jj = 0; jj < found.size(); jj++ )
      {
         int bx = found.at( jj );

         if ( orph_adopted[ side ][ bx ] )   continue;

         QString bkey = branch_key( orph_branches[ side ][ bx ].first() );
         orph_adopted[ side ][ bx ] = true;
         orph_taken  [ side ].insert( bkey );
         descs << orph_branches[ side ][ bx ];

         if ( merging_run >= 0 )
            run_taken[ merging_run ] << qMakePair( side, bkey );
      }
   }
}

// Put the records of no experiment under stand-in experiments
//
// Only a whole-store read can tell an orphan from a record whose parent was
// not read, so there are none after a read one experiment at a time, and
// none when the tree is narrowed to one run or triple.  An orphan adopted
// by an experiment (see do_merge_run) is not one any more.
int US_DataModel::collect_orphans( void )
{
   if ( ! filt_run.isEmpty()     &&  filt_run    != "ALL" )   return -1;
   if ( ! filt_triple.isEmpty()  &&  filt_triple != "ALL" )   return -1;

   BusyGuard           guard( busy );
   QVector< DataDesc > orph[ 2 ];

   index_orphans();

   for ( int side = 0; side < 2; side++ )
      for ( int ii = 0; ii < orph_branches[ side ].size(); ii++ )
         if ( ! orph_adopted[ side ][ ii ] )
            orph[ side ] << orph_branches[ side ][ ii ];

   return add_orphans( orph[ 0 ], orph[ 1 ] );
}

/* Merge the database and local records of one experiment.

   Records are paired by GUID within their parent -- a hash lookup, so it
   does not matter where either side has a record in its list, and one
   record never splits into a DB-only and a local-only row because the other
   side holds something the first does not.  The result is in tree order,
   each level sorted by label (raw and edit) or description (model, noise),
   and a record whose parent is missing hangs off a gray filler.
*/
QVector< US_DataModel::DataDesc > US_DataModel::merge_descs(
      const QVector< DataDesc >& dbs, const QVector< DataDesc >& los )
{
   struct Node
   {
      DataDesc       desc;
      QVector< int > kids;
   };

   QVector< Node > nodes;
   QVector< int >  roots;

   // A raw record's parent is the experiment the caller is merging, so a
   // raw is matched by its own GUID; anything else by GUID and parent
   auto pairKey = []( const DataDesc& desc )
   {
      return QString::number( desc.recType ) + ":"
           + ( desc.recType == RAW ? QString() : desc.parentGUID ) + ":"
           + desc.dataGUID;
   };

   // One record listed twice by its source (a noise under each of two
   // models that claim one GUID, say) is still one record
   QSet< QString > seen;
   auto sameRecord = [ & ]( const DataDesc& desc, const QString& where )
   {
      QString id = pairKey( desc ) + "|" + where + "|"
                 + ( desc.recordID >= 0 ? QString::number( desc.recordID )
                                        : desc.filename );

      if ( seen.contains( id ) )   return true;

      seen.insert( id );
      return false;
   };

   QHash< QString, QList< int > > dbByKey;

   for ( int ii = 0; ii < dbs.size(); ii++ )
   {
      if ( sameRecord( dbs.at( ii ), "D" ) )   continue;

      Node node;
      node.desc = dbs.at( ii );
      dbByKey[ pairKey( node.desc ) ] << nodes.size();
      nodes << node;
   }

   for ( int ii = 0; ii < los.size(); ii++ )
   {
      const DataDesc& descl = los.at( ii );
      QString         key   = pairKey( descl );

      if ( sameRecord( descl, "L" ) )   continue;

      if ( ! dbByKey.value( key ).isEmpty() )
      {  // the same record in both places:  one row
         int       jj    = dbByKey[ key ].takeFirst();
         DataDesc& descd = nodes[ jj ].desc;

         descd.recState    |= descl.recState;
         descd.filename     = descl.filename;     // the file is the local one
         descd.filemodDate  = descl.filemodDate;

         if ( ! descl.description.isEmpty() )
         {
            descd.description = descl.description;

            if ( ! descl.label.isEmpty() )
               descd.label    = descl.label;
         }

         // Both sides with a checksum have been compared; otherwise the
         // record is unverified until verify_run() reads them
         if ( ! descd.contents.isEmpty()  &&  ! descl.contents.isEmpty() )
         {
            descd.contents  = descd.contents + " " + descl.contents;
            descd.recState |= HV_DET;
         }

         else
            descd.contents  = QString();

         continue;
      }

      Node node;
      node.desc = descl;
      nodes << node;
   }

   // The parent of each record, by level and GUID
   QHash< QString, int > byGuid;
   auto guidKey = []( int recType, const QString& guid )
   {
      return QString::number( recType ) + ":" + guid;
   };

   int nreal = nodes.size();

   for ( int ii = 0; ii < nreal; ii++ )
   {
      QString key = guidKey( nodes[ ii ].desc.recType,
                             nodes[ ii ].desc.dataGUID );

      if ( ! byGuid.contains( key ) )
         byGuid.insert( key, ii );
   }

   // A filler parent for records whose parent is missing.  Fillers are
   // shared by GUID, so the orphans of one missing parent stay together.
   static const char* names[] = { "Experiment", "Raw", "Edit", "Model" };
   std::function< int( int, const QString& ) > filler;
   filler = [ & ]( int recType, const QString& guid ) -> int
   {
      QString pguid = ( guid.length() == 36 ) ? guid : dmyGUID;
      QString key   = guidKey( recType, pguid );

      if ( byGuid.contains( key ) )
         return byGuid.value( key );

      Node node;
      node.desc.recordID    = -1;
      node.desc.recType     = recType;
      node.desc.parentID    = -1;
      node.desc.recState    = NOSTAT;
      node.desc.subType     = "";
      node.desc.dataGUID    = pguid;
      node.desc.parentGUID  = dmyGUID;
      node.desc.filename    = "";
      node.desc.contents    = "";
      node.desc.label       = QString( "Dummy-%1-for-Orphans" )
                              .arg( names[ recType ] );
      node.desc.description = node.desc.label + "--ARTIFICIAL-RECORD";
      node.desc.lastmodDate = US_Util::toUTCDatetimeText(
                              QDateTime::currentDateTime().toUTC()
                              .toString( Qt::ISODate ), true );
      node.desc.filemodDate = node.desc.lastmodDate;

      int index = nodes.size();
      nodes << node;
      byGuid.insert( key, index );

      if ( recType == RAW )
         roots << index;
      else
         nodes[ filler( recType - 1, dmyGUID ) ].kids << index;

      return index;
   };

   for ( int ii = 0; ii < nreal; ii++ )
   {
      int ityp = nodes[ ii ].desc.recType;

      if ( ityp <= RAW )
      {
         roots << ii;
         continue;
      }

      QString pkey = guidKey( ityp - 1, nodes[ ii ].desc.parentGUID );
      int     prnt = byGuid.value( pkey, -1 );

      if ( prnt < 0  ||  prnt == ii )
         prnt = filler( ityp - 1, nodes[ ii ].desc.parentGUID );

      nodes[ prnt ].kids << ii;
   }

   // Each level in order of label (raw, edit) or description (model, noise)
   auto before = [ &nodes ]( int a, int b )
   {
      const DataDesc& da = nodes[ a ].desc;
      const DataDesc& db = nodes[ b ].desc;
      bool dummyA = ( da.recState == NOSTAT );
      bool dummyB = ( db.recState == NOSTAT );

      if ( dummyA != dummyB )   return dummyB;     // fillers go last

      QString ka = ( da.recType < MODEL ) ? da.label : da.description;
      QString kb = ( db.recType < MODEL ) ? db.label : db.description;

      if ( ka != kb )   return ka < kb;

      return da.dataGUID < db.dataGUID;
   };

   std::stable_sort( roots.begin(), roots.end(), before );

   for ( int ii = 0; ii < nodes.size(); ii++ )
      std::stable_sort( nodes[ ii ].kids.begin(), nodes[ ii ].kids.end(),
                        before );

   // Out in tree order, each record marked with where its parent is
   QVector< DataDesc > merged;
   std::function< void( int, int ) > emitNode;
   emitNode = [ & ]( int index, int pstate )
   {
      DataDesc desc = nodes[ index ].desc;

      if ( desc.recType == RAW )
         pstate = desc.recState;          // a raw is its own parent's state

      if ( desc.recState == NOSTAT  ||
           desc.description.contains( "-ARTIFICIAL" ) )
         desc.recState = NOSTAT;          // a filler

      else
      {
         if ( ( pstate & REC_DB ) != 0 )  desc.recState |= PAR_DB;
         if ( ( pstate & REC_LO ) != 0 )  desc.recState |= PAR_LO;
      }

      merged << desc;

      for ( int jj = 0; jj < nodes[ index ].kids.size(); jj++ )
         emitNode( nodes[ index ].kids[ jj ], desc.recState );
   };

   for ( int ii = 0; ii < roots.size(); ii++ )
      emitNode( roots[ ii ], NOSTAT );

   return merged;
}

// The records of a merged list that the source filter keeps
QVector< US_DataModel::DataDesc > US_DataModel::kept_records(
      const QVector< DataDesc >& mdescs ) const
{
   int nrecs = mdescs.size();
   QVector< int >  parent( nrecs, -1 );
   QVector< bool > keep  ( nrecs, false );
   QVector< bool > filler( nrecs, false );
   QVector< int >  stack;

   // The records are in tree order: a record's parent is the nearest one
   // above it of a lower level
   for ( int ii = 0; ii < nrecs; ii++ )
   {
      const DataDesc& desc = mdescs.at( ii );

      while ( ! stack.isEmpty()  &&
              mdescs.at( stack.last() ).recType >= desc.recType )
         stack.removeLast();

      parent[ ii ] = stack.isEmpty() ? -1 : stack.last();
      filler[ ii ] = ( desc.recState == NOSTAT );
      stack << ii;

      if ( filler[ ii ] )
         continue;                     // decided by what is under it

      int prnt = parent[ ii ];

      if ( prnt < 0  ||  filler[ prnt ] )
         keep[ ii ] = ! excluded_tree( desc.recState );   // a head
      else
         keep[ ii ] = keep[ prnt ];                       // its head's say
   }

   // A filler stays while anything under it does; a parent is above its
   // records, so going up the list reaches every filler above a record
   for ( int ii = nrecs - 1; ii >= 0; ii-- )
   {
      int prnt = parent[ ii ];

      if ( keep[ ii ]  &&  prnt >= 0  &&  filler[ prnt ] )
         keep[ prnt ] = true;
   }

   QVector< DataDesc > kept;

   for ( int ii = 0; ii < nrecs; ii++ )
      if ( keep[ ii ] )
         kept << mdescs.at( ii );

   return kept;
}

// ------------------------------------------------------------ orphans

/* Show records that attach to no experiment, under stand-in experiments.

   A stand-in is appended after everything else, like any experiment that
   is read: its row and its records' rows are its own range.  Global models
   -- a model with no edit at all is one by design, not a broken link --
   and their noise go under a stand-in of their own, so that removing "the
   orphans" never sweeps them up.
*/
int US_DataModel::add_orphans( const QVector< DataDesc >& dbOrphans,
                               const QVector< DataDesc >& loOrphans )
{
   QVector< DataDesc > orph[ 2 ];
   QVector< DataDesc > glob[ 2 ];

   for ( int side = 0; side < 2; side++ )
   {
      const QVector< DataDesc >& descs = ( side == 0 ) ? dbOrphans : loOrphans;
      QSet< QString > globals;         // GUIDs of the global models

      for ( int ii = 0; ii < descs.size(); ii++ )
      {
         const DataDesc& desc = descs.at( ii );
         bool global = false;

         if ( desc.recType == MODEL )
            global = ( desc.parentGUID.length() != 36  ||
                       desc.parentGUID == dmyGUID );

         else if ( desc.recType == NOISE )
            global = globals.contains( desc.parentGUID );

         if ( global  &&  desc.recType == MODEL )
            globals.insert( desc.dataGUID );

         if ( global )  glob[ side ] << desc;
         else           orph[ side ] << desc;
      }
   }

   int first = add_stand_in( tr( "(records of no experiment)" ),
                             tr( "Records whose experiment or parent is"
                                 " missing" ),
                             false, orph[ 0 ], orph[ 1 ] );

   int second = add_stand_in( tr( "(global models)" ),
                              tr( "Global models, which belong to no edit,"
                                  " and their noise" ),
                              true, glob[ 0 ], glob[ 1 ] );

   return ( first >= 0 ) ? first : second;
}

// Append one stand-in experiment and its records
int US_DataModel::add_stand_in( const QString& label,
                                const QString& description, bool globals,
                                const QVector< DataDesc >& dbs,
                                const QVector< DataDesc >& los )
{
   if ( dbs.isEmpty()  &&  los.isEmpty() )   return -1;

   // What the source filter excludes is left out, as it is from any
   // experiment, and a stand-in with nothing left is not shown
   QVector< DataDesc > mdescs = kept_records( merge_descs( dbs, los ) );

   if ( mdescs.isEmpty() )   return -1;

   RunEntry entry;
   entry.runID   = label;
   entry.orphans = true;
   entry.globals = globals;
   entry.loaded  = true;

   int index     = runents.size();
   int ndb       = 0;
   int nlo       = 0;

   for ( int ii = 0; ii < mdescs.size(); ii++ )
   {
      ndb += ( ( mdescs.at( ii ).recState & REC_DB ) != 0 ) ? 1 : 0;
      nlo += ( ( mdescs.at( ii ).recState & REC_LO ) != 0 ) ? 1 : 0;
   }

   // Where the records under it are, so the context menu offers what can
   // be done to them; the tree does not show it as a record of either place
   DataDesc edesc;
   edesc.recordID    = -1;
   edesc.recType     = EXPERIMENT;
   edesc.parentID    = -1;
   edesc.recState    = ( ndb == 0 ? 0 : REC_DB )
                     | ( nlo == 0 ? 0 : REC_LO );
   edesc.subType     = "";
   edesc.dataGUID    = dmyGUID;
   edesc.parentGUID  = "";
   edesc.filename    = "";
   edesc.contents    = "";
   edesc.label       = entry.runID;
   edesc.description = description;

   entry.row = adescs.size();
   append_row( edesc, index );

   int  base  = adescs.size();

   for ( int ii = 0; ii < mdescs.size(); ii++ )
      append_row( mdescs.at( ii ), index );

   entry.firstRow = base;
   entry.lastRow  = adescs.size() - 1;
   entry.dbCount  = ndb;
   entry.loCount  = nlo;

   kdb_recs      += ndb;
   klo_recs      += nlo;

   runents << entry;

   // Records that are in both places are compared like any others
   runents[ index ].verified = ! needs_verify( index );

   return index;
}

// ------------------------------------------------------------ duplicates

/* Rows that share a GUID, across the whole tree.

   A record in both places is one row.  A database row and a local row that
   share a GUID without being paired -- each under its own parent -- are
   still two copies of one record, which the tree shows as two rows; but
   two database rows, or two local files, that claim one GUID are two
   records that claim to be the same one, whichever experiments they are in.
*/
QList< US_DataModel::Duplicate > US_DataModel::find_duplicates( void ) const
{
   QHash< QString, QVector< int > > rowsByKey;
   QStringList keys;

   for ( int row = 0; row < adescs.size(); row++ )
   {
      const DataDesc& desc = adescs.at( row );

      if ( desc.recType < RAW  ||  desc.recState == NOSTAT )   continue;
      if ( row_run.at( row ) < 0 )   continue;   // left behind by a re-merge

      QString key = QString::number( desc.recType ) + ":" + desc.dataGUID;

      if ( ! rowsByKey.contains( key ) )
         keys << key;

      rowsByKey[ key ] << row;
   }

   QList< Duplicate > dups;

   for ( int ii = 0; ii < keys.size(); ii++ )
   {
      const QVector< int >& rows = rowsByKey[ keys[ ii ] ];

      if ( rows.size() < 2 )   continue;

      int ndb = 0;
      int nlo = 0;

      for ( int jj = 0; jj < rows.size(); jj++ )
      {
         int state = adescs.at( rows[ jj ] ).recState;
         ndb += ( ( state & REC_DB ) != 0 ) ? 1 : 0;
         nlo += ( ( state & REC_LO ) != 0 ) ? 1 : 0;
      }

      if ( ndb < 2  &&  nlo < 2 )   continue;   // one copy in each place

      Duplicate dup;
      dup.recType = keys[ ii ].section( ":", 0, 0 ).toInt();
      dup.guid    = keys[ ii ].section( ":", 1 );
      dup.rows    = rows;
      dups << dup;
   }

   return dups;
}

// Build a sample tree, shown before the first scan
void US_DataModel::dummy_data()
{
   if ( must_queue() )   return;          // never under a running scan

   close_catalogs();

   clear_rows();
   ddescs   .clear();
   ldescs   .clear();
   runents  .clear();
   run_queue.clear();
   ver_queue.clear();
   batch_runs.clear();
   stale_runs.clear();
   run_taken.clear();
   acted    .clear();
   db_gone  .clear();
   errors   .clear();
   db_failed  = false;
   stopped    = false;
   kdb_recs   = 0;
   klo_recs   = 0;

   for ( int side = 0; side < 2; side++ )
   {
      orph_by_parent[ side ].clear();
      orph_branches [ side ].clear();
      orph_adopted  [ side ].clear();
      orph_taken    [ side ].clear();
      orph_size     [ side ] = 0;
   }

   orph_indexed = false;

   RunEntry entry;
   entry.runID          = "demo1_veloc";

   entry.row            = 0;
   entry.firstRow       = 1;
   entry.loaded         = true;
   entry.verified       = true;

   cdesc.recType        = EXPERIMENT;
   cdesc.recState       = REC_DB | REC_LO | HV_DET;
   cdesc.subType        = "velocity";
   cdesc.label          = "demo1_veloc";
   cdesc.description    = "demo1_veloc";
   cdesc.dataGUID       = "demo1_exper";
   cdesc.parentGUID     = "";
   cdesc.contents       = "differ 1 - -";
   cdesc.parentID       = 1;
   cdesc.recordID       = 1;
   cdesc.filename       = "";
   append_row( cdesc, 0 );

   cdesc.recType        = RAW;
   cdesc.recState       = REC_DB | PAR_DB;
   cdesc.subType        = "";
   cdesc.label          = "item_1_2";
   cdesc.description    = "demo1_veloc";
   cdesc.dataGUID       = "demo1_veloc";
   cdesc.parentGUID     = "demo1_exper";
   cdesc.contents       = "";
   cdesc.parentID       = 1;
   cdesc.recordID       = 1;
   cdesc.filename       = "";
   append_row( cdesc, 0 );
   ddescs<<cdesc;

   cdesc.recType        = EDIT;
   cdesc.recState       = REC_DB | REC_LO | PAR_DB | PAR_LO | HV_DET;
   cdesc.subType        = "RA";
   cdesc.label          = "item_2_2";
   cdesc.description    = "demo1_veloc";
   cdesc.contents       = "AA 12 AA 12";
   cdesc.parentID       = 1;
   cdesc.recordID       = 2;
   cdesc.filename       = "demo1_veloc_edit.xml";
   append_row( cdesc, 0 );
   ddescs<<cdesc;
   ldescs<<cdesc;

   cdesc.recType        = MODEL;
   cdesc.recState       = REC_LO | PAR_LO;
   cdesc.subType        = "2DSA";
   cdesc.label          = "item_3_2";
   cdesc.description    = "demo1_veloc.sa2d.model.11";
   cdesc.contents       = "";
   cdesc.parentID       = 2;
   cdesc.recordID       = -1;
   cdesc.filename       = "demo1_veloc_model.xml";
   append_row( cdesc, 0 );
   ldescs<<cdesc;

   cdesc.recType        = NOISE;
   cdesc.recState       = REC_DB | REC_LO | PAR_DB | PAR_LO | HV_DET;
   cdesc.subType        = "TI";
   cdesc.label          = "item_4_2";
   cdesc.description    = "demo1_veloc.ti_noise";
   cdesc.contents       = "BB 12 AA 13";
   cdesc.parentID       = 2;
   cdesc.recordID       = 3;
   cdesc.filename       = "demo1_veloc_noise.xml";
   append_row( cdesc, 0 );
   ddescs<<cdesc;
   ldescs<<cdesc;

   cdesc.recType        = NOISE;
   cdesc.recState       = REC_DB | REC_LO | PAR_DB | PAR_LO;
   cdesc.subType        = "RI";
   cdesc.label          = "item_5_2";
   cdesc.description    = "demo1_veloc.ri_noise";
   cdesc.contents       = "";
   cdesc.parentID       = 2;
   cdesc.recordID       = 4;
   cdesc.filename       = "demo1_veloc_noise2.xml";
   append_row( cdesc, 0 );
   ddescs<<cdesc;
   ldescs<<cdesc;

   cdesc.recType        = EDIT;
   cdesc.recState       = NOSTAT;
   cdesc.subType        = "RA";
   cdesc.label          = "item_6_2";
   cdesc.description    = "demo1_veloc";
   cdesc.contents       = "CC 15";
   cdesc.recordID       = -1;
   cdesc.filename       = "";
   append_row( cdesc, 0 );

   entry.lastRow        = adescs.size() - 1;
   entry.dbCount        = 4;
   entry.loCount        = 3;
   entry.catVerified    = true;
   runents << entry;

   // The sample shows the states a record can be in, so its rows keep
   // their DB and Local flags; but none of them is a record anywhere, and
   // no ID of a real one may reach an action or a database read
   for ( int row = 0; row < adescs.size(); row++ )
   {
      adescs[ row ].recordID = -1;
      adescs[ row ].parentID = -1;
   }

   kdb_recs   = ddescs.size();
   klo_recs   = ldescs.size();
   demo       = true;
}

// What is wrong with the experiments that should not be acted on as they are
QStringList US_DataModel::run_problems( void ) const
{
   QStringList problems;

   for ( int ii = 0; ii < runents.size(); ii++ )
      if ( ! runents.at( ii ).problem.isEmpty() )
         problems << runents.at( ii ).runID + ":  " + runents.at( ii ).problem;

   return problems;
}
