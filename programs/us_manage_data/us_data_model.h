//! \file us_data_model.h
#ifndef US_DATA_MODEL_H
#define US_DATA_MODEL_H

#include "us_extern.h"
#include "us_widgets.h"
#include "us_db2.h"
#include "us_model.h"
#include "us_noise.h"
#include "us_buffer.h"
#include "us_analyte.h"
#include "us_help.h"
#include "us_dataIO.h"
#include "us_data_catalog.h"

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

        //! \enum State
        //! \brief Enumeration for data record states
        enum State { NOSTAT=0,  REC_DB=1,  REC_LO=2, PAR_DB=4, PAR_LO=8,
            HV_DET=16, IS_CON=32, ALL_OK=64 };

        //! \enum RecType
        //! \brief The levels of the record chain, outermost first
        //!
        //! The tree nests by this number, so an experiment has to sort
        //! before the raw data under it.
        enum RecType { EXPERIMENT=0, RAW=1, EDIT=2, MODEL=3, NOISE=4 };

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
        //! fills in the row range its records occupy.
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

                //! \brief True when both sources hold this experiment
                bool      inBoth( void ) const;
        };

        //! \brief Set the database connection
        //! \param db Database connection pointer
        void setDatabase( US_DB2* db );

        //! \brief Set the progress bar and status label
        //! \param progress Progress bar
        //! \param status Status label
        void setProgress( QProgressBar* progress, QLabel* status );

        //! \brief Set sibling objects for synchronization
        //! \param proc Data processor object
        //! \param tree Data tree handler object
        void setSiblings( QObject* proc, QObject* tree );

        //! \brief Get the list of run IDs
        //! \param runIDs List of run IDs
        //! \param count Count of run IDs
        void getRunIDs( QStringList& runIDs, int& count );

        //! \brief Get the list of triples
        //! \param triples List of triples
        //! \param runID Run ID
        void getTriples( QStringList& triples, QString runID );

        //! \brief Set filters for data selection
        //! \param run Filter by run
        //! \param triple Filter by triple
        //! \param source Filter by source
        void setFilters( QString run, QString triple, QString source );

        //! \brief Get the database connection
        //! \return Database connection pointer
        US_DB2* dbase( void );

        //! \brief Get investigator text
        //! \return Investigator text
        QString invtext( void );

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

        //! \brief Browse data
        void browse_data( void );

        //! \brief Scan data
        //!
        //! This is the whole scan, both layers, for a caller that does not
        //! want to drive the layers itself.
        void scan_data( void );

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

        /*! \brief Read the chain of every experiment at once

            The second layer in bulk.  Against the database this is four
            queries for the whole store instead of four per experiment, and
            neither source reads a checksum -- see \ref verify_run.
            \returns False when the source cannot do it, and the caller
                     should fall back to \ref scan_run per experiment
        */
        bool scan_all( void );

        /*! \brief Merge one experiment's records into the tree

            The records have to be in the catalogs already, which \ref
            scan_all put them there.
            \param index The position of the experiment
        */
        bool merge_run( int index );

        /*! \brief Read the checksums of one experiment and re-compare

            This is the expensive half of a scan -- the database hashes the
            experiment's data blobs and the local files are read -- and all
            it answers is whether records that exist in both places still
            match, so it is only worth doing for an experiment that is in
            both.
            \param index The position of the experiment
        */
        bool verify_run( int index );

        //! \brief True when the checksums of an experiment have been read
        //! \param index The position of the experiment
        bool run_verified( int index ) const;

        //! \brief Queue every experiment that is worth verifying
        //! \returns The number of experiments queued
        int  queue_verifies( void );

        //! \brief Ask for an experiment to be verified before the others
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
        //! nothing.
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

        //! \brief Load dummy data
        void dummy_data( void );

        //! \brief Set the investigator
        //! \param investigator Investigator name
        void set_investigator( QString investigator );

        //! \brief Get investigator text
        //! \return Investigator text
        QString investigator_text( void );

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

    private:
        US_DB2*       db;               //!< Pointer to opened DB connection
        US_DataCatalog* cat_db;         //!< Catalog of the database records
        US_DataCatalog* cat_lo;         //!< Catalog of the local records
        bool          use_db;           //!< True when the database is scanned
        bool          use_lo;           //!< True when the local disk is scanned
        QProgressBar* progress;         //!< Progress bar on main window
        QLabel*       lb_status;        //!< Status label on main window
        QWidget*      parentw;          //!< Parent widget (main window)

        QTreeWidget*        tw_recs;    //!< Tree widget
        QTreeWidgetItem*    tw_item;    //!< Current tree widget item

        DataDesc            cdesc;      //!< Current record description
        QVector< DataDesc > ddescs;     //!< DB descriptions
        QVector< DataDesc > ldescs;     //!< Local-disk descriptions
        QVector< DataDesc > adescs;     //!< All (merged) descriptions
        QVector< DataDesc > mdescs;     //!< Merged descriptions of one run
        QVector< int >      chgrows;    //!< Changed rows
        QVector< RunEntry > runents;    //!< Experiment entries of the scan
        QList< int >        run_queue;  //!< Experiments still to be read
        QList< int >        ver_queue;  //!< Experiments still to be verified
        int                 kdb_recs;   //!< Database records merged so far
        int                 klo_recs;   //!< Local records merged so far

        QObject*            ob_process; //!< Data processor
        QObject*            ob_tree;    //!< Data tree handler
        QObject*            ob_exper;   //!< Experiment synchronizer

        US_Buffer     buffer;           //!< Buffer object
        US_Analyte    analyte;          //!< Analyte object

        int           personID;         //!< Person ID
        int           dbg_level;        //!< Debug level
        int           maxdlen;          //!< Maximum data length

        QString       invID;            //!< Investigator ID
        QString       run_name;         //!< Run name
        QString       cell;             //!< Cell
        QString       filt_run;         //!< Filter run
        QString       filt_triple;      //!< Filter triple
        QString       filt_source;      //!< Filter source

        QPoint        cur_pos;          //!< Current position

    private slots:
        //! \brief Slot to merge database and local data
        void merge_dblocal( void );

    private:
        //! \brief Open the catalogs the current source filter calls for
        void open_catalogs( void );

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

        //! \brief The sub-type of a model, from its description and size
        //! \param descript The model description
        //! \param recsize  The length of the model contents
        QString model_subtype( const QString& descript,
                               const QString& recsize );


        //! \brief Sort descriptions
        //! \param descs Vector of descriptions
        void sort_descs( QVector< DataDesc >& descs );

        //! \brief Review descriptions
        //! \param list List of strings
        //! \param descs Vector of descriptions
        //! \return true if successful, false otherwise
        bool review_descs( QStringList& list, QVector< DataDesc >& descs );

        //! \brief Get the index of a substring in a list
        //! \param str String to search for
        //! \param pos Position to start searching
        //! \param list List to search in
        //! \return Index of the substring
        int index_substring( QString str, int pos, QStringList& list );

        //! \brief Filter a list by substring
        //! \param str Substring to filter by
        //! \param pos Position to start filtering
        //! \param list List to filter
        //! \return Filtered list
        QStringList filter_substring( QString str, int pos, QStringList& list );

        //! \brief List orphans
        //! \param list1 First list
        //! \param list2 Second list
        //! \return List of orphans
        QStringList list_orphans( QStringList& list1, QStringList& list2 );

        //! \brief Get the record state flag
        //! \param desc Data description
        //! \param flag State flag
        //! \return Record state flag
        int record_state_flag( DataDesc desc, int flag );

        //! \brief Get the sort string for a data description
        //! \param desc Data description
        //! \param flag State flag
        //! \return Sort string
        QString sort_string( DataDesc desc, int flag );

};

#endif // US_DATA_MODEL_H
