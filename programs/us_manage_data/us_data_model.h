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

#include <functional>

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

                //! True once the two catalogs' checksums of this
                //! experiment have been compared (apply_digests).  A record
                //! changed by an action after that is compared on its own,
                //! so the experiment is not hashed again.
                bool      catVerified;

                //! True for a stand-in experiment: the one that holds the
                //! records that attach to no experiment's chain, or the
                //! one that holds the global models
                bool      orphans;

                //! True for the stand-in experiment of the global models,
                //! which have no edit by design
                bool      globals;

                //! True when an upload or a download for this run would
                //! write somewhere else than where the run already is: its
                //! local directory is not named after it (a copy of a run,
                //! \ref copyOf, is one such).  Another directory holding the
                //! same run ID beside it does not make the run unsafe: the
                //! directory named after the run is the run, and the other
                //! is a copy.  Names are compared as the local
                //! catalog compares them (US_DataCatalog::fileNameCase).  A
                //! directory that also holds files of another run is safe
                //! for the run it is named after -- what is written for that
                //! run goes there -- and unsafe for the other, whose name it
                //! does not have.  Both actions are refused for such a run,
                //! and \ref problem then always says why.
                bool      unsafe;

                //! For a local directory that holds a copy of a run whose
                //! own directory -- the one named after the run -- is there
                //! too (US_DataCatalog::Run::copyOf): the path of that
                //! directory.  Such a copy is never the run: it is listed as
                //! an experiment of its own, never paired with the
                //! database's experiment, marked \ref unsafe, and \ref
                //! problem names the run's directory.  Nothing is uploaded,
                //! downloaded or removed for it: its edits share their GUIDs
                //! with the run's, so its models and noise are the run's
                //! own files.  It takes no orphans either: a record whose
                //! parent only the other place has is shown under the run's
                //! own directory, where it can be acted on, when that
                //! directory has the parent.  An edit made in the copy since
                //! it was copied has a GUID of its own, but the copy is
                //! refused all the same; a database record of such an edit
                //! -- a model of it uploaded from elsewhere, say -- has its
                //! parent in the copy only, and is shown under the stand-in
                //! experiment of the records of no experiment.  Empty for
                //! every other experiment.
                QString   copyOf;

                //! What is wrong with the run, when either source says it
                //! should not be acted on as it is (US_DataCatalog::Run::
                //! problem): a run ID UltraScan no longer accepts -- dotted
                //! run IDs are legacy -- a directory not named after its
                //! run, or a run ID two directories claim.  Empty when fine.
                QString   problem;

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
        void setDatabase( IUS_DB2* db );

        //! \brief How the local store compares file names
        //!
        //! The platform decides by default (see
        //! US_DataCatalog::setFileNameCase); this is for a store on a file
        //! system unlike the platform's, and for tests.  It applies from
        //! the next scan.
        //! \param sensitivity Qt::CaseInsensitive to compare without case
        void setFileNameCase( Qt::CaseSensitivity sensitivity );

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
        IUS_DB2* dbase( void );

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
            only queued (see \ref scan_run).  The window takes input between
            experiments only: a database query blocks it while it runs.
            The whole-store read of the local store runs on the disk
            worker, and the window is repainted, though it ignores input,
            while it waits for it; with a run filter the run is read on the
            window's own thread, and the window is not repainted until that
            read is done.

            With a run filter only that run's records are read, one run at
            a time; the whole-store read (\ref scan_all) is for an unfiltered
            scan, and it is made even when no experiment is listed, since
            the store may still hold records of no experiment.
        */
        void scan_data( void );

        //! \brief True while \ref scan_data is running
        bool scan_active( void ) const;

        //! \brief True while the model is reading or merging
        //!
        //! A request that arrives meanwhile, from an event the window
        //! processed, is only queued.
        bool is_busy( void ) const;

        /*! \brief Hold off reads and merges while an action asks and runs

            An action asks its questions in modal dialogs, and the window
            does its queued reads and comparisons from the event loop those
            run: a read can merge an experiment again, and the rows the
            action took would then be ones left behind.  Between this and
            the matching \ref end_hold the model is busy (\ref is_busy):
            what is asked for meanwhile is only queued, and done after.
            Holds nest.
        */
        void begin_hold( void );

        //! \brief End a hold; see \ref begin_hold
        void end_hold( void );

        /*! \brief A record is gone from the database

            Noted by its type and database ID, so that a merge from the
            catalogs -- which read the store before it went -- does not
            show it again: the record, and what the database removes with
            it (an edit's models, a model's noise), are left out of the
            database side of every merge after this.  This is what keeps
            the noise an edit removal took along in an experiment not read
            yet from coming back when that experiment is read.
            \param recType  The record type
            \param recordID Its database ID
        */
        void removed_from_db( int recType, int recordID );

        //! \brief Whether an error number says the connection to the
        //!        database server is gone, as IUS_DB2::isConnectionLoss has
        //!        it for that server (2006, 2013, 2026 -- a TLS connection
        //!        that was cut --, 2055, ...; 4031 on MySQL 8 only, where it
        //!        is an idle timeout, and not on MariaDB, where it is a
        //!        statement error): the window does not connect again
        //! \param serverErrno   lastServerErrno() of the statement
        //! \param serverVersion serverVersion() of its connection
        static bool connection_lost( int serverErrno,
                                     const QString& serverVersion );

        //! \brief True once the database listing was dropped since the
        //!        last scan; see \ref listingDropped
        bool listing_dropped( void ) const;

        /*! \brief Whether the investigator selected now differs from the
                   one whose database records were read

            Another window can change the investigator at any time, also
            after the scan.  This checks now: on a change the database
            listing is dropped, as when it changes during a read (\ref
            listingDropped), and true is returned -- also when the listing
            was dropped before.  Nothing may then be done to what the tree
            shows: it is the previous investigator's.
        */
        bool investigator_changed( void );

        //! \brief The investigator whose database records were read (the
        //!        selected one when no database was read)
        int scanned_investigator( void ) const;

        //! \brief Read the first layer: one record per experiment
        //!
        //! This is what makes the window usable straight away on a store
        //! full of multi-wavelength runs: it lists the experiments and how
        //! many records hang off each, without reading the chain.
        void scan_runs( void );

        //! \brief The number of experiments the first layer found, plus
        //!        the stand-in experiments added after the chain is read
        int  runCount( void ) const;

        //! \brief The number of real experiments: \ref runCount without
        //!        the stand-ins
        int  experimentCount( void ) const;

        //! \class Counts
        //! \brief Records by kind and by where they are
        class Counts
        {
            public:
                Counts();

                int db[ 5 ];      //!< Database records, [ RAW .. NOISE ]
                int lo[ 5 ];      //!< Local records, [ RAW .. NOISE ]

                //! \brief Add another count to this one
                void add( const Counts& other );
        };

        /*! \brief Count the records of one experiment, or of the tree

            The window keeps a running total from this as the experiments
            are merged, rather than counting the whole tree again each time.
            \param index The position of the experiment, or -1 for all
        */
        Counts count_records( int index = -1 ) const;

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
        //! Outside a scan this emits \ref workRequested, so whoever runs
        //! the comparisons knows there is one waiting.
        //! \param index The position of the experiment
        void request_verify( int index );

        //! \brief True when an experiment has records in both places that
        //!        are not compared yet: a real experiment in both sources,
        //!        or a stand-in holding a record found in both
        //! \param index The position of the experiment
        bool needs_verify( int index ) const;

        /*! \brief Do one piece of the work queued outside a scan

            A read asked for while the model was busy, and a comparison
            asked for when an experiment was opened or a record changed,
            are queued; this does the first of them -- a read before a
            comparison -- and emits \ref runMerged or \ref runCompared, as
            the scan would.  Whoever owns the event loop calls it once per
            turn after \ref workRequested, while it returns true.
            \returns True when something was done; false when nothing is
                     waiting, or the model is busy or scanning
        */
        bool pump_once( void );

        /*! \brief A record was uploaded or downloaded: compare it again

            Its two copies are not the ones compared before, so the record
            and its experiment read as unverified, and the experiment is
            queued to be compared again -- only the records changed since,
            not the whole experiment.
            \param row The row of the record
        */
        void record_changed( int row );

        /*! \brief Start a batch of record changes

            Until the matching \ref end_changes, \ref record_changed only
            notes which experiments changed; each of them is then brought
            up to date -- its row, its counts and its comparison request --
            once, however many of its records the batch changed.  Batches
            nest; the outermost one does the work.
        */
        void begin_changes( void );

        //! \brief End a batch of record changes; see \ref begin_changes
        //! \returns The experiments the batch changed, brought up to date
        //!          now -- those of the records acted on, and any other an
        //!          action reached, as the foreign noise an edit removal
        //!          takes along; empty while an outer batch is open
        QList< int > end_changes( void );

        //! \brief Ask the scan that is running, or the read or comparison
        //!        in progress outside a scan, to stop after the step it is
        //!        in, as a window that is closing does
        void stop_scan( void );

        //! \brief True when the last scan was stopped by \ref stop_scan
        bool scan_stopped( void ) const;

        //! \brief True while the tree is the sample shown before a scan
        //!
        //! Its records are made up, so no action may be taken on them.
        bool is_demo( void ) const;

        /*! \brief The checksum of one database record, read on its own

            "md5 length", as the catalogs give it: the server hashes a raw
            or edit blob; a model or noise record's XML is read and hashed
            here.
            \param dbP    The database connection
            \param desc   The record; its recordID is the database ID
            \param digest Filled in with "md5 length"
            \param error  Filled in with a message when the read fails
        */
        static bool db_digest( IUS_DB2* dbP, const DataDesc& desc,
                               QString& digest, QString& error );

        //! \brief The checksum of a local file, "md5 length", or empty
        //!        when it cannot be read
        static QString file_digest( const QString& path );

        //! \brief The checksum of a record's local file, "md5 length", as
        //!        it compares with the database copy, or empty when it
        //!        cannot be read
        //!
        //! A model or noise file is hashed with its line ends made LF, as
        //! the database holds the XML; a file written on Windows has CRLF
        //! ones, and is the same record all the same.  Other files are
        //! hashed as they are (\ref file_digest).
        static QString local_digest( const DataDesc& desc );

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
            be seen and removed.  Global models -- models with no edit, by
            design -- and their noise go under a stand-in of their own,
            "(global models)", which is added after the first.
            \param dbOrphans Orphan records read from the database
            \param loOrphans Orphan records read from local disk
            \returns The position of the first stand-in experiment added,
                     or -1 when there were no orphans
        */
        int  add_orphans( const QVector< DataDesc >& dbOrphans,
                          const QVector< DataDesc >& loOrphans );

        /*! \brief The records of a merged list the source filter keeps

            A tree goes or stays as a whole, judged by its head: a raw
            record, or the first real record under gray fillers -- an
            orphan model under the fillers of its missing edit and raw, say.
            A filler stays while anything under it does.
            \param mdescs Records in tree order, as \ref merge_descs gives
                          them
        */
        QVector< DataDesc > kept_records( const QVector< DataDesc >& mdescs ) const;

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

        //! \brief Rows that share a GUID, over the whole tree
        //!
        //! Detected once, after the scan, across every experiment.  Rows
        //! are duplicates when one source holds the GUID more than once; a
        //! database row and a local row with the same GUID are two copies
        //! of one record, not two records.  The rows of a copy of a run
        //! (RunEntry::copyOf) are left out: they hold the run's GUIDs by
        //! nature, and the copy is reported as a problem of its own.
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

        //! \brief The experiments with a problem, one line each
        //! \see RunEntry::problem
        QStringList run_problems( void ) const;

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

        //! \brief A read or a comparison was queued while no scan was
        //!        running; see \ref pump_once
        void workRequested( void );

        //! \brief The records of no experiment were put under a stand-in
        //! \param index The position of the stand-in experiment
        void orphansAdded( int index );

        //! \brief An experiment merged already was merged again, since the
        //!        database catalog hung more records under it
        //!
        //! Its rows are new: whoever shows them drops what it showed for
        //! the experiment and shows its rows again.
        //! \param index The position of the experiment
        void runReloaded( int index );

        /*! \brief The database listing was dropped: the investigator was
                   changed since it was read (US_DataCatalog::listingDropped)

            Everything shown of the database was read for the previous
            investigator, and the experiments' positions in the listing mean
            nothing now: the scan is stopped, nothing more is read or merged
            from it, and whoever shows the tree clears it.  A new scan lists
            the new investigator's experiments.
        */
        void listingDropped( void );

    private:
        IUS_DB2*      db;               //!< Pointer to opened DB connection
        US_DataCatalog* cat_db;         //!< Catalog of the database records

        //! Catalog of the local records.  It belongs to \ref scanner and
        //! is only looked at through \ref await_disk, which waits for the
        //! worker to be between jobs first.
        US_DataCatalog* cat_lo;

        US_DiskScan*    scanner;        //!< Reads the local store off-thread
        bool          use_db;           //!< True when the database is scanned
        bool          use_lo;           //!< True when the local disk is scanned
        bool          db_failed;        //!< The database could not be listed
        bool          db_dropped = false; //!< The listing was dropped
        bool          scanning;         //!< True while scan_data runs
        bool          stop_req;         //!< stop_scan() was called
        bool          stopped;          //!< The last scan was stopped
        bool          demo;             //!< The tree is the sample tree
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
        QSet< int >         touched;    //!< Rows an action changed since
                                        //!< their experiment was compared
        QStringList         errors;     //!< Errors of the last scan
        int                 kdb_recs;   //!< Database records merged so far
        int                 klo_recs;   //!< Local records merged so far

        QObject*            ob_process; //!< Data processor
        QObject*            ob_tree;    //!< Data tree handler

        int           dbg_level;        //!< Debug level

        //! The experiment of each row, by row: what \ref run_of_row reads,
        //! kept as the rows are appended
        QVector< int >      row_run;

        //! The parent row of each record below the raw level, by row, or
        //! -1: what \ref parent_row reads, found as the rows are appended
        QVector< int >      row_parent;
        int                 batch_depth;  //!< Nesting of begin_changes()
        QList< int >        stale_runs;   //!< Merged, then changed by the
                                          //!< catalog: to be merged again
        bool                remerging;    //!< merge_stale_runs() is running
        int                 merging_run;  //!< Experiment adopting orphans
        //! The orphan branches each experiment took: (side, branch_key)
        QHash< int, QList< QPair< int, QString > > > run_taken;
        QList< int >        batch_runs;   //!< Experiments a batch changed

        //! \brief A record as an action left it
        struct Acted
        {
            DataDesc desc;       //!< The record after the action
            QString  mergedFile; //!< Its local file as it was merged
            int      mergedID;   //!< Its database ID as it was merged
        };

        //! Every record an action changed since the scan, as the action
        //! left it, by \ref action_key: a merge from the catalogs, which
        //! actions do not change, puts it back (\ref apply_action) on the
        //! same record -- the same local file and the same database
        //! record -- not on a copy of it with the same GUID elsewhere
        QHash< QString, QList< Acted > > acted;

        //! The local file and the database ID of each row as it was
        //! merged, before what an action did was put back on it: what
        //! \ref acted knows the record by
        QVector< QString >  row_file;
        QVector< int >      row_dbid;

        //! Records gone from the database, by "type:ID"; see
        //! \ref removed_from_db
        QSet< QString >     db_gone;
        bool                case_set;     //!< setFileNameCase() was called
        Qt::CaseSensitivity file_case;    //!< What setFileNameCase() set

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

        //! \brief The database catalog changed an experiment it had read
        //! \param dbIndex The experiment's position in that catalog
        void catalog_run_changed( int dbIndex );

        //! \brief The database catalog dropped its listing
        void catalog_dropped( void );

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

        //! \brief Compare the checksums both catalogs hold for one experiment
        void apply_digests( int index );

        //! \brief Compare, record by record, the rows of one experiment
        //!        that are in both places and not compared yet
        //! \returns False when a record could not be read
        bool verify_rows( int index );

        //! \brief Set the experiment row's state from its records
        void update_run_row( int index );

        //! \brief Count again the records under an experiment
        void recount_run( int index );

        //! \brief Merge again the experiments noted by catalog_run_changed
        void merge_stale_runs( void );

        //! \brief Merge one experiment again; its old rows are left
        //!        behind, belonging to no experiment
        bool remerge_run( int index );

        //! \brief Bring an experiment up to date after its records changed
        void run_changed( int index );

        //! \brief What a record an action changed is known by, or empty
        //!        for a record with no GUID (a filler)
        static QString action_key( const DataDesc& desc );

        //! \brief Put back on a record merged from the catalogs what an
        //!        action did to it since the scan
        //! \param desc The record, as merged
        //! \returns True when an action had changed it
        bool apply_action( DataDesc& desc ) const;

        //! \brief Leave out of a catalog's database records those gone
        //!        from the database since it read them (\ref db_gone),
        //!        and what the database removed with them
        //! \param descs     The catalog's database records
        //! \param by_action When given, a gone record an action changed
        //!        since the scan (\ref acted_on_db_record) is kept, for
        //!        \ref apply_action to put the action back on, and its
        //!        "type:ID" is added here: what the action does not
        //!        match is still not shown in the database
        void drop_db_gone( QVector< DataDesc >& descs,
                           QSet< QString >* by_action = NULL );

        //! \brief Whether an action since the scan changed the row that
        //!        was merged with this database record (by type, GUID
        //!        and database ID)
        bool acted_on_db_record( const DataDesc& desc ) const;

        //! \brief Append a row of an experiment
        void append_row( const DataDesc& desc, int runIndex );

        //! \brief Clear the rows and everything kept about them
        void clear_rows( void );

        /*! \brief Make sure both sources have read an experiment's chain

            A side whose read fails is read once more, since what made it
            fail may pass -- a lock wait on the server, say; not after the
            database connection was lost, since US_DB2 does not connect
            again.  A side that still cannot be read is reported, and the
            experiment is left unread: merging what the other side holds
            would show its records as in that place only.
            \returns False when a side could not be read
        */
        bool load_sides( int index );

        //! \brief One orphan and what hangs off it, as tree records
        typedef QVector< DataDesc > Branch;

        /*! \brief The orphans of both catalogs, as branches

            Read once per scan.  A branch whose parent the other source has
            in an experiment's chain is merged into that experiment instead
            of being shown as an orphan (see adopt_orphans), so a local copy
            of a database record pairs with it.
        */
        void index_orphans( void );

        //! Orphan branches of one source, by the key of the parent they
        //! point at ("level:GUID"; a raw record by its own GUID)
        QHash< QString, QList< int > > orph_by_parent[ 2 ];
        QVector< Branch >  orph_branches[ 2 ];  //!< [0] database, [1] local
        QVector< bool >    orph_adopted [ 2 ];  //!< Merged into a run
        bool               orph_indexed;        //!< index_orphans() ran

        //! The branches of each source adopted so far, by \ref branch_key:
        //! an index made again (orphans read since) keeps what was adopted
        QSet< QString >    orph_taken   [ 2 ];

        //! How many orphans each catalog held when they were indexed.  A
        //! read of one experiment at a time adds the stray noise it finds,
        //! so the index is made again when this changes.
        int                orph_size    [ 2 ];

        //! \brief How many orphans a catalog holds now
        static int orphan_size( US_DataCatalog* catalog );

        //! \brief What tells one orphan branch from another, across indexes
        static QString branch_key( const DataDesc& head );

        //! \brief The key a branch is adopted by: its parent's level and
        //!        GUID, or for a raw record its own GUID
        static QString adopt_key( const DataDesc& head );

        //! \brief Add to \a descs the orphans of one source that attach to
        //!        a record of \a chain, the other source's experiment chain
        //! \param side  The source of the orphans: 0 database, 1 local
        //! \param chain The other source's records of the experiment
        //! \param descs The records of the orphans' source, added to
        void adopt_orphans( int side, const QVector< DataDesc >& chain,
                            QVector< DataDesc >& descs );

        //! \brief Append one stand-in experiment and its records
        //! \returns Its position, or -1 when it holds nothing
        int  add_stand_in( const QString& label, const QString& description,
                           bool globals, const QVector< DataDesc >& dbs,
                           const QVector< DataDesc >& los );

        //! \brief Start a disk worker job, after the one before it is done
        //! \param job Starts the job; false when the worker was busy
        bool start_disk( const std::function< bool() >& job );

        //! \brief Put the records of no experiment under a stand-in
        //! \returns The position of the stand-in, or -1 when there is none
        int  collect_orphans( void );

        //! \brief Turn a catalog's orphan records into tree records, one
        //!        branch per orphan, its descendants after it
        void orphan_descs( US_DataCatalog* catalog, int state,
                           QVector< Branch >& branches );

        //! \brief One raw record and everything under it, as tree records
        void append_raw( const US_DataCatalog::Raw& raw, const QString& runID,
                         const QString& runGUID, const QString& runDbID,
                         int state, QVector< DataDesc >& descs );

        //! \brief One edit record and everything under it
        void append_edit( const US_DataCatalog::Edit& edit,
                          const QString& runID, const QString& rawGUID,
                          const QString& rawDbID, int state,
                          QVector< DataDesc >& descs );

        //! \brief One model record and its noise
        void append_model( const US_DataCatalog::Model& model,
                           const QString& editGUID, const QString& editDbID,
                           int state, QVector< DataDesc >& descs );

        //! \brief One noise record
        void append_noise( const US_DataCatalog::Noise& noise,
                           const QString& modelGUID,
                           const QString& modelDbID, int state,
                           QVector< DataDesc >& descs );

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
        //! \param present Filled in with the GUID of every record of the
        //!                run, with a checksum or not, when not null
        //! \returns GUID -> "checksum length", for records that have one;
        //!          the GUIDs are simplified, as the rows' are
        QMap< QString, QString > run_digests( US_DataCatalog* catalog,
                                              int index,
                                              QSet< QString >* present = NULL );

        //! \brief Whether the source filter excludes this tree
        //! \param state The record state flags of the head of the tree
        bool excluded_tree( int state ) const;
};

#endif // US_DATA_MODEL_H
