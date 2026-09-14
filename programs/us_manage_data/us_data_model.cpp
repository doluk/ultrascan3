//! \file us_data_model.cpp

#include "us_data_model.h"
#include "us_data_process.h"
#include "us_data_tree.h"
#include "us_util.h"
#include "us_settings.h"
#include "us_model.h"
#include "us_noise.h"
#include "us_editor.h"

#define timeFmt QString("hh:mm:ss")
#define nowTime() "T="+QDateTime::currentDateTime().toString(timeFmt)

// Scan the database and local disk for R/E/M/N data sets
US_DataModel::US_DataModel( QWidget* parwidg /*=0*/ )
{
   parentw    = parwidg;   // parent (main manage_data) widget

   ddescs .clear();        // db descriptions
   ldescs .clear();        // local descriptions
   adescs .clear();        // all descriptions
   mdescs .clear();        // merged descriptions of one experiment
   chgrows.clear();        // changed rows
   runents.clear();        // experiment entries
   run_queue.clear();      // experiments still to read

   db         = NULL;
   cat_db     = NULL;
   cat_lo     = NULL;
   use_db     = false;
   use_lo     = false;
   kdb_recs   = 0;
   klo_recs   = 0;

   dbg_level  = US_Settings::us_debug();
}

// Set database related pointers
void US_DataModel::setDatabase( US_DB2* a_db )
{
   db         = a_db;      // pointer to opened db connection
   invID      = QString::number( US_Settings::us_inv_ID() );
DbgLv(1) << "DMod:setDB: invID" << invID;
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
US_DB2* US_DataModel::dbase()
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
// Scan the database and local disk for run identifiers
//
// The catalog answers this from its first layer, which is one query against
// the database and one pass over the results directory -- the same pass the
// scan itself uses, so a store is not walked twice to fill in a combo box.
void US_DataModel::getRunIDs( QStringList& runIDs, int& source )
{
   runIDs.clear();

   QString error;

   // source: 0=ALL, 1=DB Only, 2=Local Only,
   //         3=Exclude Local-Only Trees, 4=Exclude DB-Only Trees
   if ( source != 2  &&  db != NULL )
   {
      US_DataCatalog catalog( this );

      if ( catalog.attach( db, error )  &&  catalog.loadRuns( error ) )
      {
         for ( int ii = 0; ii < catalog.runCount(); ii++ )
         {
            QString runID = catalog.run( ii ).runID;

            if ( ! runIDs.contains( runID ) )
               runIDs << runID;
         }
      }

      else
         DbgLv(1) << "gRI: db" << error;
   }
DbgLv(1) << "gRI: db runs" << runIDs.size();

   if ( source != 1 )
   {
      US_DataCatalog catalog( this );

      if ( catalog.open( US_DataCatalog::Disk, "", error )  &&
           catalog.loadRuns( error ) )
      {
         for ( int ii = 0; ii < catalog.runCount(); ii++ )
         {
            const US_DataCatalog::Run& run = catalog.run( ii );

            if ( run.rawCount < 1 )                 continue;
            if ( runIDs.contains( run.runID ) )     continue;
            if ( source == 4 )                      continue;

            runIDs << run.runID;
         }
      }

      else
         DbgLv(1) << "gRI: local" << error;
   }
DbgLv(1) << "gRI: db+local runs" << runIDs.size();

   runIDs.sort();
}

// Scan the database and local for triples in a runID then return to caller
void US_DataModel::getTriples( QStringList& triples, QString runID )
{
   triples.clear();

   QString error;

   if ( db != NULL )
   {
      US_DataCatalog catalog( this );

      if ( catalog.attach( db, error )  &&  catalog.loadRuns( error ) )
      {
         int index = catalog.indexOfRun( runID );

         if ( index >= 0  &&  catalog.loadRunDetail( index, error ) )
         {
            const US_DataCatalog::Run& run = catalog.run( index );

            for ( int ii = 0; ii < run.raws.size(); ii++ )
               if ( ! triples.contains( run.raws.at( ii ).triple ) )
                  triples << run.raws.at( ii ).triple;
         }
      }
   }
DbgLv(1) << "gTr: db triples" << triples.size();

   // Add any local triples not already represented
   QStringList aucfilt;
   aucfilt << "*.auc";
   QStringList aucfiles = QDir( US_Settings::resultDir() + "/" + runID )
      .entryList( aucfilt, QDir::Files, QDir::Name );

   for ( int ii = 0; ii < aucfiles.size(); ii++ )
   {
      QString triple = aucfiles.at( ii ).section( ".", -4, -2 );

      if ( ! triples.contains( triple ) )
         triples << triple;
   }
DbgLv(1) << "gTr: db+local triples" << triples.size();

   triples.sort();
}

// Set run and triple filters
void US_DataModel::setFilters( QString a_runf, QString a_tripf, QString a_srcf )
{
   if ( filt_run != a_runf  ||
        filt_triple != a_tripf  ||
        filt_source != a_srcf )
      chgrows.clear();     // Reset changed rows if any filters changed

   filt_run    = a_runf;   // Filter string for runID
   filt_triple = a_tripf;  // Filter string for triple
   filt_source = a_srcf;   // Filter string for source (DB/local)
}

// Scan the database and local disk for R/E/M/N data sets
void US_DataModel::scan_data()
{
DbgLv(1) << "ScnD: start scan   " << nowTime();
   scan_runs();            // First layer:  list the experiments
DbgLv(1) << "ScnD: runs listed  " << nowTime();

   int nruns  = runents.size();

   progress->setMaximum( qMax( nruns, 1 ) );
   progress->setValue  ( 0 );

   for ( int ii = 0; ii < nruns; ii++ )
   {  // Second layer:  read one experiment at a time
      int index  = next_pending_run();

      if ( index < 0 )   break;

      lb_status->setText( tr( "Reading %1 ..." )
                          .arg( runents.at( index ).runID ) );
      scan_run( index );

      progress->setValue( ii + 1 );
      qApp->processEvents();
   }

   lb_status->setText( tr( "Data Scan Complete" ) );
   qApp->processEvents();
DbgLv(1) << "ScnD: scan done    " << nowTime();
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
   chgrows << row;
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
}

// Open the catalogs the current source filter calls for
void US_DataModel::open_catalogs( )
{
   delete cat_db;
   delete cat_lo;
   cat_db     = NULL;
   cat_lo     = NULL;

   use_db     = ( db != NULL  &&  filt_source != "Local Only" );
   use_lo     = ( filt_source != "DB Only" );

   QString error;

   if ( use_db )
   {
      cat_db     = new US_DataCatalog( this );

      if ( ! cat_db->attach( db, error ) )
      {
         DbgLv(1) << "ScnR: db catalog" << error;
         delete cat_db;
         cat_db     = NULL;
         use_db     = false;
      }
   }

   if ( use_lo )
   {
      cat_lo     = new US_DataCatalog( this );

      if ( ! cat_lo->open( US_DataCatalog::Disk, "", error ) )
      {
         DbgLv(1) << "ScnR: local catalog" << error;
         delete cat_lo;
         cat_lo     = NULL;
         use_lo     = false;
      }
   }
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
   ddescs   .clear();
   ldescs   .clear();
   adescs   .clear();
   runents  .clear();
   run_queue.clear();
   kdb_recs   = 0;
   klo_recs   = 0;

   open_catalogs();

   lb_status->setText( tr( "Listing Experiments..." ) );
   progress ->setMaximum( 1 );
   progress ->setValue  ( 0 );
   qApp->processEvents();

   bool    rfilt = ( ! filt_run.isEmpty()  &&  filt_run != "ALL" );
   QString error;

   // Both sources meet in one map, keyed by run identifier, which also
   // gives the experiments a stable alphabetic order
   QMap< QString, RunEntry > byRun;

   if ( use_db )
   {
      if ( cat_db->loadRuns( error ) )
      {
         for ( int ii = 0; ii < cat_db->runCount(); ii++ )
         {
            const US_DataCatalog::Run& run = cat_db->run( ii );

            if ( rfilt  &&  run.runID != filt_run )   continue;

            RunEntry entry  = byRun.value( run.runID );
            entry.runID     = run.runID;
            entry.dbIndex   = ii;
            entry.dbCount   = run_record_count( run );
            byRun.insert( run.runID, entry );
         }
      }

      else
         DbgLv(1) << "ScnR: db runs" << error;
   }

   if ( use_lo )
   {
      if ( cat_lo->loadRuns( error ) )
      {
         for ( int ii = 0; ii < cat_lo->runCount(); ii++ )
         {
            const US_DataCatalog::Run& run = cat_lo->run( ii );

            if ( rfilt  &&  run.runID != filt_run )   continue;

            RunEntry entry  = byRun.value( run.runID );
            entry.runID     = run.runID;
            entry.loIndex   = ii;
            entry.loCount   = run_record_count( run );
            byRun.insert( run.runID, entry );
         }
      }

      else
         DbgLv(1) << "ScnR: local runs" << error;
   }

   QStringList runIDs = byRun.keys();

   for ( int ii = 0; ii < runIDs.size(); ii++ )
   {
      RunEntry entry = byRun.value( runIDs[ ii ] );
      int      state = ( entry.dbIndex >= 0 ? REC_DB : 0 )
                     | ( entry.loIndex >= 0 ? REC_LO : 0 );

      if ( excluded_tree( state ) )   continue;

      entry.row      = adescs.size();

      adescs    << run_datadesc( entry );
      runents   << entry;
      run_queue << ( runents.size() - 1 );
   }

   progress ->setMaximum( qMax( runents.size(), 1 ) );
   progress ->setValue  ( runents.size() );
   lb_status->setText( tr( "%1 Experiments Listed" ).arg( runents.size() ) );
   qApp->processEvents();
DbgLv(1) << "ScnR: experiments" << runents.size() << nowTime();
}

// Number of experiments the first layer found
int US_DataModel::runCount( ) const
{
   return runents.size();
}

// One experiment entry, by position
US_DataModel::RunEntry US_DataModel::run_entry( int index ) const
{
   if ( index < 0  ||  index >= runents.size() )
      return RunEntry();

   return runents.at( index );
}

// Position of an experiment, by run identifier
int US_DataModel::index_of_run( const QString& runID ) const
{
   for ( int ii = 0; ii < runents.size(); ii++ )
      if ( runents.at( ii ).runID == runID )
         return ii;

   return -1;
}

// Ask for an experiment to be read before the others
void US_DataModel::request_run( int index )
{
   if ( index < 0  ||  index >= runents.size() )   return;
   if ( runents.at( index ).loaded )               return;

   run_queue.removeAll( index );
   run_queue.prepend  ( index );
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

// Read the second layer of one experiment and merge it into the whole
bool US_DataModel::scan_run( int index )
{
   if ( index < 0  ||  index >= runents.size() )   return false;

   RunEntry entry = runents.at( index );

   if ( entry.loaded )
   {
      run_queue.removeAll( index );
      return true;
   }

   QString error;

   ddescs.clear();
   ldescs.clear();

   if ( entry.dbIndex >= 0  &&  cat_db != NULL )
   {
      if ( cat_db->loadRunDetail( entry.dbIndex, error ) )
         catalog_descs( cat_db, entry.dbIndex, REC_DB, ddescs );
      else
         DbgLv(1) << "ScnR: db detail" << entry.runID << error;
   }

   if ( entry.loIndex >= 0  &&  cat_lo != NULL )
   {
      if ( cat_lo->loadRunDetail( entry.loIndex, error ) )
         catalog_descs( cat_lo, entry.loIndex, REC_LO, ldescs );
      else
         DbgLv(1) << "ScnR: local detail" << entry.runID << error;
   }

   sort_descs( ddescs );
   sort_descs( ldescs );

   kdb_recs      += ddescs.size();
   klo_recs      += ldescs.size();

   // Merge this experiment's records, then append them.  Rows already in
   // the tree keep their position, so the tree can be filled in as the
   // experiments arrive.
   int  base  = adescs.size();
   bool exctr = false;

   merge_dblocal();

   for ( int ii = 0; ii < mdescs.size(); ii++ )
   {
      DataDesc desc = mdescs.at( ii );

      if ( desc.recType == RAW )        // heads of the trees under the run
         exctr = excluded_tree( desc.recState );

      if ( exctr )   continue;

      adescs << desc;
   }

   entry.firstRow = ( adescs.size() > base ) ? base : -1;
   entry.lastRow  = adescs.size() - 1;
   entry.loaded   = true;
   runents[ index ] = entry;

   run_queue.removeAll( index );

   return true;
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
        ( ! descript.contains( "_mc" )  &&  recsize.toInt() > _M_LARGE_ ) )
      subType = "CUSTOMGRID";

   // If empty subtype, mark as MANUAL
   else if ( subType.isEmpty() )
      subType = "MANUAL";

   return subType;
}

// Turn the chain of one catalog run into description records
void US_DataModel::catalog_descs( US_DataCatalog* catalog, int index,
                                  int state, QVector< DataDesc >& descs )
{
   const QString dmyGUID = "00000000-0000-0000-0000-000000000000";

   const US_DataCatalog::Run& run = catalog->run( index );

   bool isDb = ( state == REC_DB );
   bool tfilt = ( ! filt_triple.isEmpty()  &&  filt_triple != "ALL" );

   for ( int ii = 0; ii < run.raws.size(); ii++ )
   {
      const US_DataCatalog::Raw& raw = run.raws.at( ii );

      if ( tfilt  &&  raw.triple != filt_triple )   continue;

      DataDesc desc;
      desc.recordID    = isDb ? raw.id.toInt() : -1;
      desc.recType     = RAW;
      desc.subType     = "";
      desc.recState    = state;
      desc.dataGUID    = raw.guid.simplified();
      desc.parentGUID  = run.guid.simplified();
      desc.parentID    = isDb ? run.id.toInt() : -1;
      desc.filename    = isDb ? raw.filename : raw.path;
      desc.contents    = raw.checksum + " " + raw.size;
      desc.label       = run.runID + "." + raw.triple;
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
      {
         const US_DataCatalog::Edit& edit = raw.edits.at( jj );

         DataDesc edesc;
         edesc.recordID    = isDb ? edit.id.toInt() : -1;
         edesc.recType     = EDIT;
         edesc.subType     = edit.filename.section( ".", -4, -4 );
         edesc.recState    = state;
         edesc.dataGUID    = edit.guid.simplified();
         edesc.parentGUID  = desc.dataGUID;
         edesc.parentID    = isDb ? raw.id.toInt() : -1;
         edesc.filename    = isDb ? edit.filename : edit.path;
         edesc.contents    = edit.checksum + " " + edit.size;
         edesc.label       = run.runID + "." + edit.filename.section( ".", 1, 3 );
         edesc.description = edit.filename.section( ".", 0, -2 );
         edesc.filemodDate = isDb ? QString() : edit.lastUpdated;
         edesc.lastmodDate = isDb ? edit.lastUpdated : QString();

         if ( edesc.dataGUID.length() != 36  ||  edesc.dataGUID == dmyGUID )
            edesc.dataGUID = US_Util::new_guid();

         descs << edesc;

         for ( int kk = 0; kk < edit.models.size(); kk++ )
         {
            const US_DataCatalog::Model& model = edit.models.at( kk );

            QString label = model.description.section( ".", 0, -2 );

            if ( label.length() > 40 )
               label = label.left( 13 ) + "..." + label.right( 24 );

            DataDesc mdesc;
            mdesc.recordID    = isDb ? model.id.toInt() : -1;
            mdesc.recType     = MODEL;
            mdesc.subType     = model_subtype( model.description, model.size );
            mdesc.recState    = state;
            mdesc.dataGUID    = model.guid.simplified();
            mdesc.parentGUID  = edesc.dataGUID;
            mdesc.parentID    = isDb ? edit.id.toInt() : -1;
            mdesc.filename    = isDb ? QString() : model.path;
            mdesc.contents    = model.checksum + " " + model.size;
            mdesc.label       = label;
            mdesc.description = model.description;
            mdesc.filemodDate = isDb ? QString() : model.lastUpdated;
            mdesc.lastmodDate = isDb ? model.lastUpdated : QString();

            if ( mdesc.dataGUID.length() != 36  ||  mdesc.dataGUID == dmyGUID )
               mdesc.dataGUID = US_Util::new_guid();

            descs << mdesc;

            for ( int mm = 0; mm < model.noises.size(); mm++ )
            {
               const US_DataCatalog::Noise& noise = model.noises.at( mm );

               QString nlabel = noise.description.section( ".", 0, -2 );

               if ( nlabel.length() > 40 )
                  nlabel = nlabel.left( 13 ) + "..." + nlabel.right( 24 );

               DataDesc ndesc;
               ndesc.recordID    = isDb ? noise.id.toInt() : -1;
               ndesc.recType     = NOISE;
               ndesc.subType     = noise.noiseType.left( 2 ).toUpper();
               ndesc.recState    = state;
               ndesc.dataGUID    = noise.guid.simplified();
               ndesc.parentGUID  = mdesc.dataGUID;
               ndesc.parentID    = isDb ? model.id.toInt() : -1;
               ndesc.filename    = isDb ? QString() : noise.path;
               ndesc.contents    = noise.checksum + " " + noise.size;
               ndesc.label       = nlabel;
               ndesc.description = noise.description;
               ndesc.filemodDate = isDb ? QString() : noise.lastUpdated;
               ndesc.lastmodDate = isDb ? noise.lastUpdated : QString();

               if ( ndesc.dataGUID.length() != 36  ||
                    ndesc.dataGUID == dmyGUID )
                  ndesc.dataGUID = US_Util::new_guid();

               descs << ndesc;
            }
         }
      }
   }
}

// merge the database and local description vectors of one experiment
//
// Both inputs hold the records of a single experiment, sorted, and the
// output is that experiment's part of the tree.  Merging one experiment at
// a time is what lets the tree be filled in as the scan goes.
void US_DataModel::merge_dblocal( )
{
   mdescs.clear();

   int nddes = ddescs.size();
   int nldes = ldescs.size();
   int nstep = ( ( nddes + nldes ) * 5 ) / 8;

   int jdr   = 0;
   int jlr   = 0;
   int kar   = 1;

   DataDesc  descd = ( nddes > 0 ) ? ddescs.at( 0 ) : DataDesc();
   DataDesc  descl = ( nldes > 0 ) ? ldescs.at( 0 ) : DataDesc();
DbgLv(1) << "MERGE: nd nl dlab llab"
 << nddes << nldes << descd.label << descl.label;

   lb_status->setText( tr( "Merging Data ..." ) );
   progress->setMaximum( nstep );
   qApp->processEvents();

   while ( jdr < nddes  &&  jlr < nldes )
   {  // main loop to merge records until one is exhausted

      progress->setValue( kar );           // report progress

      if ( kar > nstep )
      {  // if count beyond max, bump max by one eighth
         nstep = ( kar * 9 ) / 8;
         progress->setMaximum( nstep );
      }
      qApp->processEvents();

      while ( descd.dataGUID == descl.dataGUID )
      {  // records match in GUID:  merge them into one
         descd.recState    |= descl.recState;     // OR states
         descd.filename     = descl.filename;     // filename from local
         descd.filemodDate  = descl.filemodDate;  // file last mod from local
         descd.description  = descl.description;  // description from local
//if ( descl.recType == 3 && descd.contents != descl.contents ) {
// US_Model modell;
// US_Model modeld;
// modell.load( descl.filename );
// modeld.load( QString::number(descd.recordID), db );
// DbgLv(1) << " ++LOCAL Model:";
// modell.debug();
// DbgLv(1) << " ++DB Model:";
// modeld.debug(); }
         descd.contents     = descd.contents + " " + descl.contents;

         mdescs << descd;                  // output combo record
DbgLv(2) << "MERGE:  kar jdr jlr (1)GID" << kar << jdr << jlr << descd.dataGUID;
         kar++;

         if ( ++jdr < nddes )              // bump db count and test if done
            descd = ddescs.at( jdr );      // get next db record

         else
         {
            if ( ++jlr < nldes )
               descl = ldescs.at( jlr );   // get next local record
            break;
         }


         if ( ++jlr < nldes )              // bump local count and test if done
            descl = ldescs.at( jlr );      // get next local record
         else
            break;
      }

      if ( jdr >= nddes  ||  jlr >= nldes )
         break;

      while ( descd.recType > descl.recType )
      {  // output db records that are left-over children
         mdescs << descd;
DbgLv(2) << "MERGE:  kar jdr jlr (2)GID" << kar << jdr << jlr << descd.dataGUID;
         kar++;

         if ( ++jdr < nddes )
            descd = ddescs.at( jdr );
         else
            break;
      }

      if ( jdr >= nddes  ||  jlr >= nldes )
         break;

      while ( descl.recType > descd.recType )
      {  // output local records that are left-over children
         mdescs << descl;
DbgLv(2) << "MERGE:  kar jdr jlr (3)GID" << kar << jdr << jlr << descl.dataGUID;
         kar++;

         if ( ++jlr < nldes )
            descl = ldescs.at( jlr );
         else
            break;
      }

      if ( jdr >= nddes  ||  jlr >= nldes )
         break;

      // If we've reached another matching pair or if we are not at
      // the same level, go back up to the start of the main loop.
      if ( descd.dataGUID == descl.dataGUID  ||
           descd.recType  != descl.recType  )
         continue;

      // If we are here, we have records at the same level,
      // but with different GUIDs. Output one of them, based on
      // an alphanumeric comparison of label values.

      QString dlabel = descd.label;
      QString llabel = descl.label;
      if ( descd.recType > 2 )
      {
         dlabel = descd.description;
         llabel = descl.description;
      }
DbgLv(2) << "MERGE: rtype dlabel llabel" << descd.recType << dlabel << llabel;

      if ( dlabel < llabel )
      {  // output db record first based on alphabetic label sort
         mdescs << descd;
DbgLv(2) << "MERGE:  kar jdr jlr (4)GID" << kar << jdr << jlr << descd.dataGUID;
         kar++;

         if ( ++jdr < nddes )
            descd = ddescs.at( jdr );
         else
            break;
      }

      else
      {  // output local record first based on alphabetic label sort
         mdescs << descl;
DbgLv(2) << "MERGE:  kar jdr jlr (5)GID" << kar << jdr << jlr << descl.dataGUID;
         kar++;

         if ( ++jlr < nldes )
            descl = ldescs.at( jlr );
         else
            break;
      }

   }  // end of main merge loop;

   // after breaking from main loop, output any records left from one
   // source (db/local) or the other.
   nstep += ( nddes - jdr + nldes - jlr );
   progress->setMaximum( nstep );
   qApp->processEvents();

   while ( jdr < nddes )
   {
      mdescs << ddescs.at( jdr++ );
descd=ddescs.at(jdr-1);
DbgLv(2) << "MERGE:  kar jdr jlr (8)GID" << kar << jdr << jlr << descd.dataGUID;
      progress->setValue( ++kar );
      qApp->processEvents();
   }

   while ( jlr < nldes )
   {
      mdescs << ldescs.at( jlr++ );
descl=ldescs.at(jlr-1);
DbgLv(2) << "MERGE:  kar jdr jlr (9)GID" << kar << jdr << jlr << descl.dataGUID;
      progress->setValue( ++kar );
      qApp->processEvents();
   }

DbgLv(2) << "MERGE: nddes nldes kar" << nddes << nldes << --kar;
DbgLv(2) << " a/d/l sizes" << mdescs.size() << ddescs.size() << ldescs.size();

   progress->setValue( nstep );
   lb_status->setText( tr( "Data Merge Complete" ) );
   qApp->processEvents();
}

// sort a data-set description vector
void US_DataModel::sort_descs( QVector< DataDesc >& descs )
{
   QVector< DataDesc > tdess;                 // temporary descr. vector
   DataDesc            desct;                 // temporary descr. entry
   QStringList         sortr;                 // sort string lists
   QStringList         sorte;
   QStringList         sortm;
   QStringList         sortn;
   int                 nrecs = descs.size();  // number of descr. records

   lb_status->setText( tr( "Sorting Descriptions..." ) );
   qApp->processEvents();
DbgLv(1) << "sort_desc: nrecs" << nrecs;
   if ( nrecs == 0 )
      return;

   tdess.resize( nrecs );
   // Determine maximum description string length
   maxdlen = 0;
   for ( int ii = 0; ii < nrecs; ii++ )
      maxdlen = qMax( maxdlen, descs[ ii ].description.length() );

   for ( int ii = 0; ii < nrecs; ii++ )
   {  // build sort strings for Raw,Edit,Model,Noise; copy unsorted vector
      desct        = descs[ ii ];

      if (      desct.recType == 1 )
         sortr << sort_string( desct, ii );

      else if ( desct.recType == 2 )
         sorte << sort_string( desct, ii );

      else if ( desct.recType == 3 )
         sortm << sort_string( desct, ii );

      else if ( desct.recType == 4 )
         sortn << sort_string( desct, ii );

      tdess[ ii ]  = desct;
   }
DbgLv(2) << "SrtD:  nrecs" << nrecs << nowTime();

   // sort the string lists for each type
   sortr.sort();
   sorte.sort();
   sortm.sort();
   sortn.sort();
DbgLv(2) << "SrtD:  sort[remn]" << nowTime();

   lb_status->setText( tr( "Finding Duplicates..." ) );
   qApp->processEvents();
   // review each type for duplicate GUIDs
   if ( review_descs( sortr, tdess ) )
      return;
DbgLv(2) << "SrtD:  RD(r)" << nowTime();
   if ( review_descs( sorte, tdess ) )
      return;
DbgLv(2) << "SrtD:  RD(e)" << nowTime();
   if ( review_descs( sortm, tdess ) )
      return;
DbgLv(2) << "SrtD:  RD(m)" << nowTime();
   if ( review_descs( sortn, tdess ) )
      return;
DbgLv(2) << "SrtD:  RD(n)" << nowTime();

   lb_status->setText( tr( "Finding Orphans..." ) );
   qApp->processEvents();

   // create list of noise,model,edit orphans
   QStringList orphn = list_orphans( sortn, sortm );
   QStringList orphm = list_orphans( sortm, sorte );
   QStringList orphe = list_orphans( sorte, sortr );
DbgLv(2) << "SrtD:  Orph(nme)" << nowTime();

   QString dmyGUID = "00000000-0000-0000-0000-000000000000";
   QString dsorts;
   QString dlabel;
   QString dindex;
   QString ddGUID;
   QString dpGUID;
   QString ppGUID;
   int kndx = tdess.size();
   int jndx;
   int kk;
   int ndmy = 0;     // flag of duplicate dummies

   // Create lists of parent GUIDs
   QStringList guidsr;
   QStringList guidse;
   QStringList guidsm;

   for ( int ii = 0; ii < sortr.size(); ii++ )
      guidsr << sortr.at( ii ).section( ":", 2, 2 ).simplified();

   for ( int ii = 0; ii < sorte.size(); ii++ )
      guidse << sorte.at( ii ).section( ":", 2, 2 ).simplified();

   for ( int ii = 0; ii < sortm.size(); ii++ )
      guidsm << sortm.at( ii ).section( ":", 2, 2 ).simplified();

   // create dummy records to parent each orphan

   int nstep   = orphn.size() + orphm.size() + orphe.size();
   int istep   = 0;
   progress->setMaximum( nstep );
   progress->setValue  ( istep );
   qApp->processEvents();
DbgLv(2) << "(1) orphan: N size M size" << orphn.size() << orphm.size();
   for ( int ii = 0; ii < orphn.size(); ii++ )
   {  // for each orphan noise, create a dummy model
      dsorts = orphn.at( ii );
      dlabel = dsorts.section( ":", 0, 0 );
      dindex = dsorts.section( ":", 1, 1 );
      ddGUID = dsorts.section( ":", 2, 2 ).simplified();
      dpGUID = dsorts.section( ":", 3, 3 ).simplified();
      jndx   = dindex.toInt();
      cdesc  = tdess[ jndx ];

      if ( dpGUID.length() < 2  ||  dpGUID == dmyGUID )
      { // handle case where there is no valid parentGUID
         if ( ndmy == 0 )      // first time:  create one
            dpGUID = dmyGUID;
         else
            dpGUID = ppGUID;   // afterwards:  re-use same parent

         kk     = sortn.indexOf( dsorts );  // find index in full list
         dsorts = dlabel + ":" + dindex + ":" + ddGUID + ":" + dpGUID;

         if ( kk >= 0 )
         {  // replace present record for new parentGUID
            sortn.replace( kk, dsorts );
            cdesc.parentGUID  = dpGUID;
            tdess[ jndx ]     = cdesc;
         }

         if ( ndmy > 0 )       // after 1st time, skip creating new parent
            continue;

         ndmy++;               // flag that we have a parent for invalid ones
         ppGUID = dpGUID;      // save the GUID for new dummy parent
      }

      // If this record is no longer an orphan, skip creating new parent
      if ( guidsm.indexOf( dpGUID ) >= 0 )
         continue;

      if ( dpGUID == dmyGUID )
         cdesc.label       = "Dummy-Model-for-Orphans";

      cdesc.parentID    = cdesc.recordID;
      cdesc.recordID    = -1;
      cdesc.recType     = 3;
      cdesc.subType     = "";
      cdesc.recState    = NOSTAT;
      cdesc.dataGUID    = dpGUID;
      cdesc.parentGUID  = dmyGUID;
      cdesc.parentID    = -1;
      cdesc.filename    = "";
      cdesc.contents    = "";
      cdesc.label       = cdesc.label.section( ".", 0, 0 );
      cdesc.description = cdesc.label + "--ARTIFICIAL-RECORD";
      cdesc.filemodDate = US_Util::toUTCDatetimeText(
                          QDateTime::currentDateTime().toUTC()
                          .toString( Qt::ISODate ), true );
      cdesc.lastmodDate = cdesc.filemodDate;

      dlabel = dlabel.section( ".", 0, 0 );
      dindex = QString::asprintf( "%4.4d", kndx++ );
      ddGUID = cdesc.dataGUID;
      dpGUID = cdesc.parentGUID;
      dsorts = dlabel + ":" + dindex + ":" + ddGUID + ":" + dpGUID;

      sortm  << dsorts;
      orphm  << dsorts;
      guidsm << ddGUID;
      tdess  << cdesc;
DbgLv(2) << "N orphan:" << orphn.at( ii ) << ii;
DbgLv(2) << "  M dummy:" << dsorts;
      progress->setValue( ++istep );
      qApp->processEvents();
   }
DbgLv(2) << "(2) orphan: N size M size" << orphn.size() << orphm.size();
DbgLv(2) << "SrtD:  Orph(N)" << nowTime();

   ndmy   = 0;

   for ( int ii = 0; ii < orphm.size(); ii++ )
   {  // for each orphan model, create a dummy edit
      dsorts = orphm.at( ii );
      dlabel = dsorts.section( ":", 0, 0 );
      dindex = dsorts.section( ":", 1, 1 );
      ddGUID = dsorts.section( ":", 2, 2 ).simplified();
      dpGUID = dsorts.section( ":", 3, 3 ).simplified();
      jndx   = dindex.toInt();
      cdesc  = tdess[ jndx ];

      if ( dpGUID.length() < 16  ||  dpGUID == dmyGUID )
      { // handle case where there is no valid parentGUID
         if ( ndmy == 0 )      // first time:  create one
            dpGUID = dmyGUID;
         else
            dpGUID = ppGUID;   // afterwards:  re-use same parent

         kk     = sortm.indexOf( dsorts );  // find index in full list
         dsorts = dlabel + ":" + dindex + ":" + ddGUID + ":" + dpGUID;

         if ( kk >= 0 )
         {  // replace present record for new parentGUID
            sortm.replace( kk, dsorts );
            cdesc.parentGUID  = dpGUID;
            tdess[ jndx ]     = cdesc;
         }

         if ( ndmy > 0 )       // after 1st time, skip creating new parent
            continue;

         ndmy++;               // flag that we have a parent for invalid ones
         ppGUID = dpGUID;      // save the GUID for new dummy parent
      }

      // If this record is no longer an orphan, skip creating new parent
      if ( guidse.indexOf( dpGUID ) >= 0 )
         continue;

      if ( dpGUID == dmyGUID )
         cdesc.label       = "Dummy-Edit-for-Orphans";

      cdesc.parentID    = cdesc.recordID;
      cdesc.recordID    = -1;
      cdesc.recType     = 2;
      cdesc.subType     = "";
      cdesc.recState    = NOSTAT;
      cdesc.dataGUID    = dpGUID;
      cdesc.parentGUID  = dmyGUID;
      cdesc.parentID    = -1;
      cdesc.filename    = "";
      cdesc.contents    = "";
      cdesc.label       = cdesc.label.section( ".", 0, 0 );
      cdesc.description = cdesc.label + "--ARTIFICIAL-RECORD";
      cdesc.lastmodDate = US_Util::toUTCDatetimeText(
                          QDateTime::currentDateTime().toUTC()
                          .toString( Qt::ISODate ), true );
      cdesc.filemodDate = cdesc.lastmodDate;

      dlabel = dlabel.section( ".", 0, 0 );
      dindex = QString::asprintf( "%4.4d", kndx++ );
      ddGUID = cdesc.dataGUID;
      dpGUID = cdesc.parentGUID;
      dsorts = dlabel + ":" + dindex + ":" + ddGUID + ":" + dpGUID;

      sorte  << dsorts;
      orphe  << dsorts;
      guidse << ddGUID;
      tdess  << cdesc;
DbgLv(2) << "M orphan:" << orphm.at( ii ) << ii;
DbgLv(2) << "  E dummy:" << dsorts;
      progress->setValue( ++istep );
      qApp->processEvents();
   }

DbgLv(2) << "(3) orphan: N size M size" << orphn.size() << orphm.size();
DbgLv(2) << "SrtD:  Orph(M)" << nowTime();
   ndmy   = 0;
DbgLv(2) << "(4) orphan: M size E size" << orphm.size() << orphe.size();

   for ( int ii = 0; ii < orphe.size(); ii++ )
   {  // for each orphan edit, create a dummy raw
      dsorts = orphe.at( ii );
      dlabel = dsorts.section( ":", 0, 0 );
      dindex = dsorts.section( ":", 1, 1 );
      ddGUID = dsorts.section( ":", 2, 2 ).simplified();
      dpGUID = dsorts.section( ":", 3, 3 ).simplified();
      jndx   = dindex.toInt();
      cdesc  = tdess[ jndx ];

      if ( dpGUID.length() < 2 )
      { // handle case where there is no valid parentGUID
         if ( ndmy == 0 )      // first time:  create one
            dpGUID = dmyGUID;
         else
            dpGUID = ppGUID;   // afterwards:  re-use same parent

         kk     = sorte.indexOf( dsorts );  // find index in full list
         dsorts = dlabel + ":" + dindex + ":" + ddGUID + ":" + dpGUID;

         if ( kk >= 0 )
         {  // replace present record for new parentGUID
            sorte.replace( kk, dsorts );
            cdesc.parentGUID  = dpGUID;
            tdess[ jndx ]     = cdesc;
         }

         if ( ndmy > 0 )       // after 1st time, skip creating new parent
           continue;

         ndmy++;               // flag that we have a parent for invalid ones
         ppGUID = dpGUID;      // save the GUID for new dummy parent
      }

      // If this record is no longer an orphan, skip creating new parent
      if ( guidsr.indexOf( dpGUID ) >= 0 )
         continue;

      if ( dpGUID == dmyGUID )
         cdesc.label       = "Dummy-Raw-for-Orphans";

      cdesc.parentID    = cdesc.recordID;
      cdesc.recordID    = -1;
      cdesc.recType     = 1;
      cdesc.subType     = "";
      cdesc.recState    = NOSTAT;
      cdesc.dataGUID    = dpGUID;
      cdesc.parentGUID  = dmyGUID;
      cdesc.parentID    = -1;
      cdesc.filename    = "";
      cdesc.contents    = "";
      cdesc.label       = cdesc.label.section( ".", 0, 0 );
      cdesc.description = cdesc.label + "--ARTIFICIAL-RECORD";
      cdesc.lastmodDate = US_Util::toUTCDatetimeText(
                          QDateTime::currentDateTime().toUTC()
                          .toString( Qt::ISODate ), true );
      cdesc.filemodDate = cdesc.lastmodDate;

      dlabel = dlabel.section( ".", 0, 0 );
      dindex = QString::asprintf( "%4.4d", kndx++ );
      ddGUID = cdesc.dataGUID;
      dpGUID = cdesc.parentGUID;
      dsorts = dlabel + ":" + dindex + ":" + ddGUID + ":" + dpGUID;

      sortr  << dsorts;
      guidsr << ddGUID;
      tdess  << cdesc;
DbgLv(2) << "E orphan:" << orphe.at( ii );
DbgLv(2) << "  R dummy:" << dsorts;
      progress->setValue( ++istep );
      qApp->processEvents();
   }
DbgLv(2) << "(5) orphan: M size E size" << orphm.size() << orphe.size();
DbgLv(2) << "SrtD:  Orph(E)" << nowTime();

//for ( int ii = 0; ii < sortr.size(); ii++ )
// DbgLv(2) << "R entry:" << sortr.at( ii );
   int countR = sortr.size();    // count of each kind in sorted lists
   int countE = sorte.size();
   int countM = sortm.size();
   int countN = sortn.size();

   //sortr.sort();                 // re-sort for dummy additions
   //sorte.sort();
   //sortm.sort();
   //sortn.sort();
DbgLv(1) << "sort/dumy: count REMN" << countR << countE << countM << countN;
//for(int ii=0;ii<countM;ii++) DbgLv(2) << "sm" << ii << "++ " << sortm[ii];

   int noutR  = 0;               // count of each kind in hierarchical output
   int noutE  = 0;
   int noutM  = 0;
   int noutN  = 0;
   int indx;
   int pstate = REC_LO | PAR_LO;

   descs.clear();                // reset input vector to become sorted output
   lb_status->setText( tr( "Building Sorted Trees..." ) );
   istep       = 0;
   progress->setMaximum( countR );
   progress->setValue  ( istep  );
   qApp->processEvents();

   // rebuild the description vector with sorted trees
   for ( int ii = 0; ii < countR; ii++ )
   {  // loop to output sorted Raw records
      QString recr = sortr[ ii ];
      QString didr = recr.section( ":", 2, 2 );
      QString pidr = recr.section( ":", 3, 3 );
      indx         = recr.section( ":", 1, 1 ).toInt();
      cdesc        = tdess.at( indx );

      // set up a default parent state flag
      pstate = cdesc.recState;
      pstate = ( pstate & REC_DB ) != 0 ? ( pstate | PAR_DB ) : pstate;
      pstate = ( pstate & REC_LO ) != 0 ? ( pstate | PAR_LO ) : pstate;

      // new state is the default,  or NOSTAT if this is a dummy record
      cdesc.recState = record_state_flag( cdesc, pstate );

      descs << cdesc;                   // output Raw rec
      noutR++;

      // set up parent state for children to follow
      int rpstate    = cdesc.recState;

      for ( int jj = 0; jj < countE; jj++ )
      {  // loop to output sorted Edit records for the above Raw
         QString rece   = sorte[ jj ];
         QString pide   = rece.section( ":", 3, 3 );

         if ( pide != didr )            // skip if current Raw not parent
            continue;

         QString dide   = rece.section( ":", 2, 2 );
         indx           = rece.section( ":", 1, 1 ).toInt();
         cdesc          = tdess.at( indx );
         cdesc.recState = record_state_flag( cdesc, rpstate );

         descs << cdesc;                // output Edit rec
         noutE++;

         // set up parent state for children to follow
         int epstate    = cdesc.recState;

         for ( int mm = 0; mm < countM; mm++ )
         {  // loop to output sorted Model records for above Edit
            QString recm   = sortm[ mm ];
            QString pidm   = recm.section( ":", 3, 3 );

            if ( pidm != dide )         // skip if current Edit not parent
               continue;

            QString didm   = recm.section( ":", 2, 2 );
            indx           = recm.section( ":", 1, 1 ).toInt();
            cdesc          = tdess.at( indx );
            cdesc.recState = record_state_flag( cdesc, epstate );

            descs << cdesc;             // output Model rec

            noutM++;

            // set up parent state for children to follow
            int mpstate    = cdesc.recState;

            for ( int nn = 0; nn < countN; nn++ )
            {  // loop to output sorted Noise records for above Model
               QString recn   = sortn[ nn ];
               QString pidn   = recn.section( ":", 3, 3 );

               if ( pidn != didm )      // skip if current Model not parent
                  continue;

               indx           = recn.section( ":", 1, 1 ).toInt();
               cdesc          = tdess.at( indx );
               cdesc.recState = record_state_flag( cdesc, mpstate );

               descs << cdesc;          // output Noise rec

               noutN++;
            }
         }
      }
      progress->setValue( ++istep );
      qApp->processEvents();
   }
DbgLv(2) << "SrtD:  END" << nowTime();

   if ( noutR != countR  ||  noutE != countE  ||
        noutM != countM  ||  noutN != countN )
   {  // not all accounted for, so we will need some dummy parents
      DbgLv(1) << "sort_desc: count REMN"
         << countR << countE << countM << countN;
      DbgLv(1) << "sort_desc:   nout REMN"
         << noutR << noutE << noutM << noutN;
   }
}

// review sorted string lists for duplicate GUIDs
bool US_DataModel::review_descs( QStringList& sorts,
      QVector< DataDesc >& descv )
{
   bool           abort = false;
   int            nrecs = sorts.size();
   int            nmult = 0;
   int            kmult = 0;
   int            ityp;
   QString        cGUID;
   QString        pGUID;
   QString        rtyp;
   QVector< int > multis;
   const char* rtyps[] = { "RawData", "EditedData", "Model", "Noise" };
   QStringList    tGUIDs;

   if ( nrecs < 1 )
      return abort;

   int ii   = sorts[ 0 ].section( ":", 1, 1 ).toInt();
   ityp     = descv[ ii ].recType;
   rtyp     = QString( rtyps[ ityp - 1 ] );

   if ( descv[ ii ].recordID >= 0 )
      rtyp     = "DB " + rtyp;
   else
      rtyp     = "Local " + rtyp;
DbgLv(2) << "RvwD: ii ityp rtyp nrecs" << ii << ityp << rtyp << nrecs;
   cGUID    = sorts[ 0 ].section( ":", 2, 2 );
   tGUIDs << cGUID;

   for ( int ii = 1; ii < nrecs; ii++ )
   {  // do a pass to determine if there are duplicate GUIDs
      cGUID    = sorts[ ii ].section( ":", 2, 2 );     // current rec GUID
      kmult    = 0;                                    // flag no multiples yet

      int jj = tGUIDs.indexOf( cGUID );

      if ( jj >= 0 )
      {
         kmult++;
         if ( ! multis.contains( jj ) )
         {  // not yet marked, so mark previous as multiple
            multis << jj;    // save index
            nmult++;         // bump count
         }
      }

      if ( kmult > 0 )
      {  // this pass found a duplicate:  save the index and bump count
         multis << ii;
         nmult++;
DbgLv(1) << "RvwD: nmult" << nmult << "ii,jj" << ii << jj
 << "cGUID,pGUID" << cGUID << tGUIDs[jj];
      }

      tGUIDs << cGUID;
//DbgLv(2) << "RvwD:   ii kmult nmult" << ii << kmult << nmult;
   }

DbgLv(2) << "RvwD:      GUID nmult" << nmult << nowTime();
   if ( nmult > 0 )
   {  // there were multiple instances of the same GUID
      QMessageBox msgBox;
      QString     msg;

      // format a message for the warning pop-up
      msg  =
         tr( "There are %1 %2 records that have\n" ).arg( nmult ).arg( rtyp ) +
         tr( "the same GUID as another.\n" ) +
         tr( "You should correct the situation before proceeding.\n" ) +
         tr( "  Click \"Ok\" to see details, then abort.\n" ) +
         tr( "  Click \"Ignore\" to proceed to further review.\n" );
      msgBox.setWindowTitle( tr( "Duplicate %1 Records" ).arg( rtyp ) );
      msgBox.setText( msg );
      msgBox.setStandardButtons( QMessageBox::Ok | QMessageBox::Ignore );
      msgBox.setDefaultButton( QMessageBox::Ok );

      if ( msgBox.exec() == QMessageBox::Ok )
      {  // user wants details, so display them
         QString fileexts = tr( "Text,Log files (*.txt *.log);;" )
            + tr( "All files (*)" );
         QString pGUID = "";
         QString cGUID;
         QString label;

         msg =
            tr( "Review the details below on duplicate records.\n" ) +
            tr( "Save or Print the contents of this message.\n" ) +
            tr( "Decide which of the duplicates should be removed.\n" ) +
            tr( "Close the main US_DataModel window after exiting here.\n" ) +
            tr( "\nSummary of Duplicates:\n\n" );

         for ( int ii = 0; ii < nmult; ii++ )
         {  // add summary lines on duplicates
            int jj = multis.at( ii );
            cGUID  = sorts.at( jj ).section( ":", 2, 2 );
            label  = sorts.at( jj ).section( ":", 0, 0 );

            if ( cGUID != pGUID )
            {  // first instance of this GUID:  show GUID
               msg  += tr( "GUID:  " ) + cGUID + "\n";
               pGUID = cGUID;
            }

            // one label line for each multiple
            msg  += tr( "  Label:  " ) + label + "\n";
         }

         msg += tr( "\nDetails of Duplicates:\n\n" );

         for ( int ii = 0; ii < nmult; ii++ )
         {  // add detail lines
            int jj = multis.at( ii );
            cGUID  = sorts.at( jj ).section( ":", 2, 2 );
            pGUID  = sorts.at( jj ).section( ":", 3, 3 );
            label  = sorts.at( jj ).section( ":", 0, 0 );
            int kk = sorts.at( jj ).section( ":", 1, 1 ).toInt();
            cdesc  = descv[ kk ];

            msg   += tr( "GUID:  " ) + cGUID + "\n" +
               tr( "  ParentGUID:  " ) + pGUID + "\n" +
               tr( "  Label:  " ) + label + "\n" +
               tr( "  Description:  " ) + cdesc.description + "\n" +
               tr( "  DB record ID:  %1" ).arg( cdesc.recordID ) + "\n" +
               tr( "  File Directory:  " ) +
               cdesc.filename.section( "/",  0, -2 ) + "\n" +
               tr( "  File Name:  " ) +
               cdesc.filename.section( "/", -1, -1 ) + "\n" +
               tr( "  File Last Mod:  " ) +
               cdesc.filemodDate + "\n" +
               tr( "  Last Mod Date:  " ) +
               cdesc.lastmodDate + "\n";
         }

         // pop up text dialog
         US_Editor* editd = new US_Editor( US_Editor::LOAD, true, fileexts );
         editd->setWindowTitle( tr( "Data Set Duplicate GUID Details" ) );
         editd->move( QCursor::pos() + QPoint( 200, 200 ) );
         editd->resize( 600, 500 );
         editd->e->setFont( QFont( US_Widgets::fixedFont().family(),
                            US_GuiSettings::fontSize() ) );
         editd->e->setText( msg );
         editd->show();

         abort = true;      // tell caller to abort data tree build
      }

      else
      {
         abort = false;     // signal to proceed with data tree build
      }
DbgLv(2) << "review_descs   abort flag:" << abort;
   }

   return abort;
}

// find index of substring at given position in strings of string list
int US_DataModel::index_substring( QString ss, int ixs, QStringList& sl )
{
   QString sexp = "XXX";
   QRegularExpression rexp;

   if ( ixs == 0 )
      sexp = ss + ":*";        // label at beginning of strings in list

   else if ( ixs == 1  ||  ixs == 2 )
      sexp = "*:" + ss + ":*"; // RecIndex/recGUID in middle of list strings

   else if ( ixs == 3 )
      sexp = "*:" + ss;        // parentGUID at end of strings in list

   rexp = QRegularExpression( QRegularExpression::wildcardToRegularExpression( sexp ) );

   return sl.indexOf( rexp );
}

// Get sublist from string list of substring matches at given string position
QStringList US_DataModel::filter_substring( QString ss, int ixs,
   QStringList& sl )
{
   QStringList subl;

   if ( ixs == 0 )
      // match label at beginning of strings in list
      subl = sl.filter( QRegularExpression( "^" + ss + ":" ) );

   else if ( ixs == 1  ||  ixs == 2 )
      // match RecIndex or recGUID in middle of strings in list
      subl = sl.filter( ":" + ss + ":" );

   else if ( ixs == 3 )
      // match parentGUID at end of strings in list
      subl = sl.filter( QRegularExpression( ":" + ss + "$" ) );

   return subl;
}

// List orphans of a record type (in rec list, no tie to parent list)
QStringList US_DataModel::list_orphans( QStringList& rlist,
   QStringList& plist )
{
   QStringList olist;
   QStringList tlist;

   for ( int ii = 0; ii < plist.size(); ii++ )
   {  // Build test-parent-GUID list
      tlist << plist.at( ii ).section( ":", 2, 2 );
   }

   for ( int ii = 0; ii < rlist.size(); ii++ )
   {  // examine parentGUID for each record in the list
      QString pReco = rlist.at( ii );
      QString pGUID = pReco.section( ":", 3, 3 );

      // see if it is the recordGUID of any in the potential parent list
      if ( ! tlist.contains( pGUID ) )
         olist << pReco;          // no parent found, so add to the orphan list
   }

   return olist;
}

// return a record state flag with parent state ORed in
int US_DataModel::record_state_flag( DataDesc descr, int pstate )
{
   int state = descr.recState;

   if ( descr.recState == NOSTAT  ||
        descr.description.contains( "-ARTIFICIAL" ) )
      state = NOSTAT;                    // mark a dummy record

   else
   {  // detect and mark parentage of non-dummy
      if ( ( pstate & REC_DB ) != 0 )
         state = state | PAR_DB;         // mark a record with db parent

      if ( ( pstate & REC_LO ) != 0 )
         state = state | PAR_LO;         // mark a record with local parent
   }

   return state;
}

// compose concatenation on which to sort (label:index:dataGUID:parentGUID)
QString US_DataModel::sort_string( DataDesc ddesc, int indx )
{  // create string for ascending sort on label
   QString label  = ( ddesc.recType < 3 ) ? ddesc.label : ddesc.description;
   int     lablen = label.length();
   if ( lablen < maxdlen )
      label = label.leftJustified( maxdlen, ' ' );

   QString ostr  = label                              // label to sort on
      + ":"      + QString::asprintf( "%4.4d", indx ) // index in desc. vector
      + ":"      + ddesc.dataGUID                     // data GUID
      + ":"      + ddesc.parentGUID;                  // parent GUID
   return ostr;
}

// Build a sample tree, shown before the first scan
void US_DataModel::dummy_data()
{
   adescs   .clear();
   ddescs   .clear();
   ldescs   .clear();
   runents  .clear();
   run_queue.clear();
   kdb_recs   = 0;
   klo_recs   = 0;

   RunEntry entry;
   entry.runID          = "demo1_veloc";
   entry.row            = 0;
   entry.firstRow       = 1;
   entry.loaded         = true;

   cdesc.recType        = EXPERIMENT;
   cdesc.recState       = REC_DB | REC_LO;
   cdesc.subType        = "velocity";
   cdesc.label          = "demo1_veloc";
   cdesc.description    = "demo1_veloc";
   cdesc.dataGUID       = "demo1_exper";
   cdesc.parentGUID     = "";
   cdesc.contents       = "";
   cdesc.parentID       = 1;
   cdesc.recordID       = 1;
   cdesc.filename       = "";
   adescs<<cdesc;

   cdesc.recType        = RAW;
   cdesc.recState       = REC_DB | PAR_DB;
   cdesc.subType        = "";
   cdesc.label          = "item_1_2";
   cdesc.description    = "demo1_veloc";
   cdesc.dataGUID       = "demo1_veloc";
   cdesc.parentGUID     = "demo1_exper";
   cdesc.parentID       = 1;
   cdesc.recordID       = 1;
   cdesc.filename       = "";
   adescs<<cdesc;
   ddescs<<cdesc;

   cdesc.recType        = EDIT;
   cdesc.recState       = REC_DB | REC_LO | PAR_DB | PAR_LO;
   cdesc.subType        = "RA";
   cdesc.label          = "item_2_2";
   cdesc.description    = "demo1_veloc";
   cdesc.contents       = "AA 12 AA 12";
   cdesc.parentID       = 1;
   cdesc.recordID       = 2;
   cdesc.filename       = "demo1_veloc_edit.xml";
   adescs<<cdesc;
   ddescs<<cdesc;
   ldescs<<cdesc;

   cdesc.recType        = MODEL;
   cdesc.recState       = REC_LO | PAR_LO;
   cdesc.subType        = "2DSA";
   cdesc.label          = "item_3_2";
   cdesc.description    = "demo1_veloc.sa2d.model.11";
   cdesc.parentID       = 2;
   cdesc.recordID       = -1;
   cdesc.filename       = "demo1_veloc_model.xml";
   adescs<<cdesc;
   ldescs<<cdesc;

   cdesc.recType        = NOISE;
   cdesc.recState       = REC_DB | REC_LO | PAR_DB | PAR_LO;
   cdesc.subType        = "TI";
   cdesc.label          = "item_4_2";
   cdesc.description    = "demo1_veloc.ti_noise";
   cdesc.contents       = "BB 12 AA 13";
   cdesc.parentID       = 2;
   cdesc.recordID       = 3;
   cdesc.filename       = "demo1_veloc_noise.xml";
   adescs<<cdesc;
   ddescs<<cdesc;
   ldescs<<cdesc;

   cdesc.recType        = EDIT;
   cdesc.recState       = NOSTAT;
   cdesc.subType        = "RA";
   cdesc.label          = "item_5_2";
   cdesc.description    = "demo1_veloc";
   cdesc.contents       = "CC 15";
   cdesc.recordID       = -1;
   cdesc.filename       = "";
   adescs<<cdesc;

   entry.lastRow        = adescs.size() - 1;
   runents << entry;

   kdb_recs   = ddescs.size();
   klo_recs   = ldescs.size();
}
