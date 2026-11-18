//! \file us_data_model.h
#ifndef US_DATA_MODEL_H
#define US_DATA_MODEL_H

#include "us_extern.h"
#include "us_widgets.h"
#include "us_db2.h"
#include "ius_db2.h"
#include "us_model.h"
#include "us_noise.h"
#include "us_help.h"
#include "us_dataIO.h"
#include "us_data_catalog.h"
#include "us_disk_scan.h"

#ifndef DbgLv
#define DbgLv(a) if(dbg_level>=a)qDebug()  //!< debug-level-conditioned qDebug()
#endif

//! \class US_DataModel
//! \brief Class to manage data model operations, including database interactions and data processing.
class US_DataModel : public QObject
{
    Q_OBJECT

    public:
        //! \brief Constructor for US_DataModel
        //! \param parent Optional parent widget
        US_DataModel( QWidget* parent = 0 );

        //! \brief Stop the disk worker and release the catalogs
        ~US_DataModel() override;

        //! \enum State
        //! \brief Enumeration for data record states
        //!
        //! HV_DET marks a record in both places whose contents have been
        //! compared: until then it reads as unverified, not as in sync.
        enum State { NOSTAT=0,  REC_DB=1,  REC_LO=2, PAR_DB=4, PAR_LO=8,
            HV_DET=16, IS_CON=32, ALL_OK=64 };

        //! \enum RecType
        //! \brief The levels of the record chain, outermost first
        //!
        //! The tree nests by this number, so an experiment has to sort
        //! before the raw data under it.
        enum RecType { EXPERIMENT=0, RAW=1, EDIT=2, MODEL=3, NOISE=4 };

        //! \enum SyncState
        //! \brief How a record stands between the database and local disk
        enum SyncState
        {
            SYNC_DUMMY = 0,    //!< A filler for a missing parent
            SYNC_LOCAL,        //!< On local disk only
            SYNC_DB,           //!< In the database only
            SYNC_UNVERIFIED,   //!< In both, contents not compared yet
            SYNC_INSYNC,       //!< In both, contents compared and equal
            SYNC_CONFLICT      //!< In both, contents differ, or one side
                               //!< has no checksum to compare
        };

        //! \class DataDesc
        //! \brief Class for describing data records
        class DataDesc
        {
            public:
                int       recordID;          //!< Record DB Identifier
                int       recType;           //!< Record type (0-4)=Experiment/Raw/Edit/Model/Noise
                int       parentID;          //!< Parent's DB Identifier
                int       recState;          //!< Record state flag
                QString   subType;           //!< Sub-type (e.g., TI, RI for noises)
                QString   dataGUID;          //!< This record data Global Identifier
                QString   parentGUID;        //!< Parent's GUID
                QString   filename;          //!< File name if on local disk
                QString   contents;          //!< md5sum() and length of data
                QString   label;             //!< Record identifying label
                QString   description;       //!< Record description string
                QString   filemodDate;       //!< Last modification date/time (file)
                QString   lastmodDate;       //!< Last modification date/time (DB/file)
        };

        //! \class RunEntry
        //! \brief One experiment of the scan, and how much of it was read
        //!
        //! The first layer of a scan produces one of these per experiment.
        //! The second layer reads the chain of one experiment at a time and
        //! fills in the row range its records occupy.  The experiment rows
        //! come first and each experiment's records are appended as it is
        //! read, so the records of an experiment are \ref firstRow to
        //! \ref lastRow, never the rows that follow \ref row.
        class RunEntry
        {
            public:
                RunEntry();

                QString   runID;       //!< The run identifier
                int       row;         //!< Row of the experiment record
                int       firstRow;    //!< First row below it, -1 while unread
                int       lastRow;     //!< Last row below it, -1 while unread
                int       dbIndex;     //!< Position in the database catalog
                int       loIndex;     //!< Position in the local catalog
                int       dbCount;     //!< Records the database says it holds,
                                       //!< or -1 when it cannot say yet
                int       loCount;     //!< Records the local store holds,
                                       //!< or -1 when it cannot say yet
                bool      loaded;      //!< True once the chain has been read

                //! True once the checksums have been read.  Only an
                //! experiment that is in both the database and the local
                //! store is worth verifying: a record that is in one place
                //! only has nothing to be compared against.
                bool      verified;

                //! True for the stand-in experiment that holds the records
                //! that attach to no experiment's chain
                bool      orphans;

                //! \brief True when both sources hold this experiment
                bool      inBoth( void ) const;
        };

        //! \class Duplicate
        //! \brief Rows that share one GUID within one kind of record
        class Duplicate
        {
            public:
                int            recType;  //!< The kind of record
                QString        guid;     //!< The shared GUID
                QVector< int > rows;     //!< The rows holding it
        };

        //! \brief Set the database connection
        //! \param db Database connection pointer
        void setDatabase( US_DB2* db );

        //! \brief Set the progress bar and status label
        //! \param progress Progress bar of the work inside one experiment
        //! \param status Status label
        void setProgress( QProgressBar* progress, QLabel* status );

        //! \brief Set sibling objects for synchronization
        //! \param proc Data processor object
        //! \param tree Data tree handler object
        void setSiblings( QObject* proc, QObject* tree );

        /*! \brief Get the list of run IDs

            A cheap listing for the run selector: one query against the
            database and one directory listing, without reading the record
            chain or indexing the local store.
            \param runIDs Filled in with the run IDs
            \param source The source filter index (0=ALL, 1=DB Only,
                          2=Local Only, 3=Exclude Local-Only, 4=Exclude DB-Only)
        */
        void getRunIDs( QStringList& runIDs, int& source );

        /*! \brief Get the triples of one run

            One lookup of the experiment and one listing of its raw data
            records, plus a listing of the run directory: no record is read
            and no checksum is computed.
            \param triples Filled in with the triples
            \param runID   The run ID
        */
        void getTriples( QStringList& triples, QString runID );

        //! \brief The run IDs of the database experiments of one person
        //! \param dbP   The database connection
        //! \param invID The investigator
        //! \param error Filled in with a message when the query fails
        static QStringList db_run_ids( IUS_DB2* dbP, int invID,
                                       QString& error );

        //! \brief The run IDs of the local results directory
        //!
        //! A run is a directory holding at least one .auc file, and its run
        //! ID is the whole directory name, since a run ID may contain dots.
        static QStringList local_run_ids( void );

        //! \brief The triples of one database run
        //! \param dbP   The database connection
        //! \param runID The run ID
        //! \param invID The investigator
        //! \param error Filled in with a message when a query fails
        static QStringList db_triples( IUS_DB2* dbP, const QString& runID,
                                       int invID, QString& error );

        //! \brief The triples of one local run
        //! \param runID The run ID
        static QStringList local_triples( const QString& runID );

        //! \brief Set filters for data selection
        //! \param run Filter by run
        //! \param triple Filter by triple
        //! \param source Filter by source
        void setFilters( QString run, QString triple, QString source );

        //! \brief Get the database connection
        //! \return Database connection pointer
        US_DB2* dbase( void );

        //! \brief Get the progress bar
        //! \return Progress bar pointer
        QProgressBar* progrBar( void );

        //! \brief Get the status label
        //! \return Status label pointer
        QLabel* statlab( void );

        //! \brief Get the data processor object
        //! \return Data processor object pointer
        QObject* procobj( void );

        //! \brief Get the data tree handler object
        //! \return Data tree handler object pointer
        QObject* treeobj( void );

        /*! \brief Scan the database and the local disk

            The whole scan -- both layers and the comparison -- and the loop
            the window runs.  It reports through \ref scanPhase, \ref
            scanStep, \ref runsListed, \ref runMerged and \ref runCompared.
            While it runs \ref scan_active is true, and a request to read or
            compare an experiment that arrives from the window meanwhile is
            only queued (see \ref scan_run).  The window stays responsive
            between experiments.
        */
        void scan_data( void );

        //! \brief True while \ref scan_data is running
        bool scan_active( void ) const;

        //! \brief True while the model is reading or merging
        //!
        //! A request that arrives meanwhile, from an event the window
        //! processed, is only queued.
        bool is_busy( void ) const;

        //! \brief Read the first layer: one record per experiment
        //!
        //! This is what makes the window usable straight away on a store
        //! full of multi-wavelength runs: it lists the experiments and how
        //! many records hang off each, without reading the chain.
        void scan_runs( void );

        //! \brief The number of experiments the first layer found
        int  runCount( void ) const;

        //! \brief One experiment entry, by position
        //! \param index The position of the experiment
        RunEntry run_entry( int index ) const;

        //! \brief The position of an experiment, by run identifier
        //! \param runID The run identifier to look for
        //! \returns -1 when there is no such experiment
        int  index_of_run( const QString& runID ) const;

        //! \brief The experiment a row belongs to
        //! \param row An experiment row or one of its records
        //! \returns The position of the experiment, or -1
        int  run_of_row( int row ) const;

        //! \brief The row of a record's parent
        //! \param row A record row
        //! \returns The parent row (the experiment row for a raw record),
        //!          or -1 for an experiment or an unknown row
        int  parent_row( int row ) const;

        /*! \brief The rows of every descendant of a row

            For an experiment these are its own row range, whatever its
            position; for a record, the records below it within its
            experiment.  An experiment not read yet has no descendants.
            \param row The row whose descendants are wanted
        */
        QVector< int > descendant_rows( int row ) const;

        //! \brief The run ID a row belongs to
        //!
        //! The experiment the row hangs under, or, for an orphan, the run
        //! its file name or description names.
        QString run_id_of_row( int row ) const;

        /*! \brief Read the chain of every experiment at once

            The second layer in bulk.  Against the database this is four
            queries for the whole store instead of four per experiment, and
            neither source reads a checksum -- see \ref verify_run.
            \returns False when the source cannot do it, and the caller
                     should fall back to \ref scan_run per experiment
        */
        bool scan_all( void );

        /*! \brief Merge one experiment's records into the tree

            The records have to be in the catalogs already: \ref scan_all
            put them there.  While the model is busy, or a scan is running,
            the experiment is only queued and false is returned.
            \param index The position of the experiment
        */
        bool merge_run( int index );

        /*! \brief Read the checksums of one experiment and re-compare

            This is the expensive half of a scan -- the database hashes the
            experiment's data blobs and the local files are read -- and all
            it answers is whether records that exist in both places still
            match, so it is only worth doing for an experiment that is in
            both.  While the model is busy, or a scan is running, the
            experiment is only queued and false is returned.
            \param index The position of the experiment
            \returns True when the experiment was compared
        */
        bool verify_run( int index );

        //! \brief True when the checksums of an experiment have been read
        //! \param index The position of the experiment
        bool run_verified( int index ) const;

        //! \brief Queue every experiment that is worth verifying
        //! \returns The number of experiments queued
        int  queue_verifies( void );

        //! \brief Ask for an experiment to be verified before the others
        //!
        //! Outside a scan this emits \ref verifyRequested, so whoever runs
        //! the comparisons knows there is one waiting.
        //! \param index The position of the experiment
        void request_verify( int index );

        //! \brief The next experiment waiting to be verified, or -1
        int  next_pending_verify( void ) const;

        //! \brief How many experiments are still waiting to be verified
        int  pending_verifies( void ) const;

        //! \brief Read the chain of one experiment and merge it in
        //!
        //! The records are appended, so rows already in the tree keep their
        //! position.  Reading an experiment that is already read does
        //! nothing.  While the model is busy, or a scan is running, the
        //! experiment is only queued and false is returned.
        //! \param index The position of the experiment
        //! \returns True when the experiment now has its records
        bool scan_run( int index );

        //! \brief Ask for an experiment to be read before the others
        //!
        //! This is what the tree calls when the user opens an experiment
        //! the background pass has not reached yet.
        //! \param index The position of the experiment
        void request_run( int index );

        //! \brief The next experiment waiting to be read, or -1
        int  next_pending_run( void ) const;

        //! \brief How many experiments are still waiting to be read
        int  pending_runs( void ) const;

        /*! \brief Show records that attach to no experiment

            The records are put under a stand-in experiment, each under
            gray filler parents for the links of its chain that are
            missing, so a record whose edit or raw data is gone can still
            be seen and removed.
            \param dbOrphans Orphan records read from the database
            \param loOrphans Orphan records read from local disk
            \returns The position of the stand-in experiment, or -1 when
                     there were no orphans
        */
        int  add_orphans( const QVector< DataDesc >& dbOrphans,
                          const QVector< DataDesc >& loOrphans );

        /*! \brief Merge the database and local records of one experiment

            Records are paired by GUID within their parent, so a record in
            both places is one row whatever else either side holds.  The
            output is in tree order: each raw record, then its edits, each
            followed by its models and their noise.  A record whose parent
            is missing is given a gray filler parent.
            \param dbs The database records
            \param los The local records
            \returns The merged records
        */
        QVector< DataDesc > merge_descs( const QVector< DataDesc >& dbs,
                                         const QVector< DataDesc >& los );

        /*! \brief Repair database model and noise descriptions

            Older servers cut model descriptions to 80 characters and leave
            noise descriptions empty.  A cut model description is read in
            full from its model record; an empty noise description is made
            from its model's, and a cut one is read in full from its noise
            record.
            \param descs The database records of one experiment
            \param dbP   The database connection, or null to skip the reads
        */
        static void repair_descriptions( QVector< DataDesc >& descs,
                                         IUS_DB2* dbP );

        //! \brief Rows that share a GUID, over the whole tree
        //!
        //! Detected once, after the scan, across every experiment.
        QList< Duplicate > find_duplicates( void ) const;

        //! \brief The sync state of a record
        //! \param desc The record
        static SyncState sync_state( const DataDesc& desc );

        //! \brief The run type of an edit file, its SubType
        //! \param filename runID.editID.type.cell.channel.wavelength.xml
        static QString edit_subtype( const QString& filename );

        //! \brief The label of an edit: run ID, edit ID and triple
        //! \param runID    The run ID
        //! \param filename The edit file name
        static QString edit_label( const QString& runID,
                                   const QString& filename );

        //! \brief The sub-type of a model, from its description and size
        //! \param descript The model description
        //! \param recsize  The length of the model contents
        static QString model_subtype( const QString& descript,
                                      const QString& recsize );

        //! \brief Parse a record date, as either source writes one
        //! \returns An invalid date-time when the text is not a date
        static QDateTime parse_date( const QString& text );

        //! \brief Errors the last scan ran into
        QStringList scan_errors( void ) const;

        //! \brief True when the last scan could not list the database
        //!
        //! Every record then reads as local only, which is not true.
        bool db_listing_failed( void ) const;

        //! \brief Load dummy data
        void dummy_data( void );

        //! \brief Get data description for a specific row
        //! \param row Row index
        //! \return Data description for the specified row
        DataDesc row_datadesc( int row );

        //! \brief Get the current data description
        //! \return Current data description
        DataDesc current_datadesc( void );

        //! \brief Change data description for a specific row
        //! \param desc Data description
        //! \param row Row index
        void change_datadesc( DataDesc desc, int row );

        //! \brief Set the current data description row
        //! \param row Row index
        void setCurrent( int row );

        //! \brief Get the record count
        //! \return Record count
        int recCount( void );

        //! \brief Get the database record count
        //! \return Database record count
        int recCountDB( void );

        //! \brief Get the local record count
        //! \return Local record count
        int recCountLoc( void );

    signals:
        //! \brief A phase of the scan starts
        //! \param text  What the phase is doing
        //! \param steps How many steps it has
        void scanPhase( const QString& text, int steps );

        //! \brief A step of the running phase is done
        //! \param done How many steps are done
        void scanStep( int done );

        //! \brief The first layer is listed
        void runsListed( void );

        //! \brief An experiment's records are merged in
        //! \param index The position of the experiment
        void runMerged( int index );

        //! \brief An experiment's records have been compared
        //! \param index The position of the experiment
        void runCompared( int index );

        //! \brief A comparison was asked for while no scan was running
        void verifyRequested( void );

    private:
        US_DB2*       db;               //!< Pointer to opened DB connection
        US_DataCatalog* cat_db;         //!< Catalog of the database records

        //! Catalog of the local records.  It belongs to \ref scanner and
        //! is only looked at through \ref await_disk, which waits for the
        //! worker to be between jobs first.
        US_DataCatalog* cat_lo;

        US_DiskScan*    scanner;        //!< Reads the local store off-thread
        bool          use_db;           //!< True when the database is scanned
        bool          use_lo;           //!< True when the local disk is scanned
        bool          db_failed;        //!< The database could not be listed
        bool          scanning;         //!< True while scan_data runs
        int           busy;             //!< Depth of reads in progress
        QProgressBar* progress;         //!< Progress bar on main window
        QLabel*       lb_status;        //!< Status label on main window
        QWidget*      parentw;          //!< Parent widget (main window)

        DataDesc            cdesc;      //!< Current record description
        QVector< DataDesc > ddescs;     //!< DB descriptions
        QVector< DataDesc > ldescs;     //!< Local-disk descriptions
        QVector< DataDesc > adescs;     //!< All (merged) descriptions
        QVector< RunEntry > runents;    //!< Experiment entries of the scan
        QList< int >        run_queue;  //!< Experiments still to be read
        QList< int >        ver_queue;  //!< Experiments still to be verified
        QStringList         errors;     //!< Errors of the last scan
        int                 kdb_recs;   //!< Database records merged so far
        int                 klo_recs;   //!< Local records merged so far

        QObject*            ob_process; //!< Data processor
        QObject*            ob_tree;    //!< Data tree handler

        int           dbg_level;        //!< Debug level

        QString       filt_run;         //!< Filter run
        QString       filt_triple;      //!< Filter triple
        QString       filt_source;      //!< Filter source

        //! \brief Counts a read in progress for as long as it lives
        class BusyGuard
        {
            public:
                explicit BusyGuard( int& depth ) : count( depth ) { count++; }
                ~BusyGuard() { count--; }
            private:
                int& count;
        };

    private slots:
        //! \brief Put a note from a catalog on the status line
        //! \param note The note
        void scanner_note( const QString& note );

    private:
        //! \brief Put a note on the status line, cut to what it can show
        void note( const QString& text );

        //! \brief Record an error of the scan
        void scan_error( const QString& text );

        //! \brief True when a read has to be queued instead of done now
        bool must_queue( void ) const;

        //! \brief Open the catalogs the current source filter calls for
        void open_catalogs( void );

        //! \brief Release the catalogs and the disk worker
        void close_catalogs( void );

        //! \brief Wait for the disk worker to be between jobs
        //!
        //! Everything read out of the local catalog goes through here
        //! first: the worker publishes what it read before it goes idle,
        //! so what is read afterwards is what it wrote.
        void await_disk( void );

        //! \brief Read and merge one experiment, without the queue check
        bool do_scan_run( int index );

        //! \brief Merge one experiment, without the queue check
        bool do_merge_run( int index );

        //! \brief Compare one experiment, without the queue check
        bool do_verify_run( int index );

        //! \brief Build the experiment record of one run entry
        //! \param entry The run entry to describe
        //! \returns The experiment description record
        DataDesc run_datadesc( const RunEntry& entry );

        //! \brief Turn one catalog run into description records
        //! \param catalog The catalog the run was read from
        //! \param index   The position of the run in that catalog
        //! \param state   REC_DB or REC_LO
        //! \param descs   The vector the records are appended to
        void catalog_descs( US_DataCatalog* catalog, int index, int state,
                            QVector< DataDesc >& descs );

        //! \brief How many records the first layer says an experiment holds
        //! \param run The catalog entry of the experiment
        //! \returns -1 when the source cannot say without walking the chain
        int run_record_count( const US_DataCatalog::Run& run );

        //! \brief The checksum of every record of one catalog run
        //! \param catalog The catalog the run was read from
        //! \param index   The position of the run in that catalog
        //! \returns GUID -> "checksum length", for records that have one
        QMap< QString, QString > run_digests( US_DataCatalog* catalog,
                                              int index );

        //! \brief Whether the source filter excludes this tree
        //! \param state The record state flags of the head of the tree
        bool excluded_tree( int state ) const;
};

#endif // US_DATA_MODEL_H
