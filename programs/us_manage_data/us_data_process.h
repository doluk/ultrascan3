//! \file us_data_process.h
#ifndef US_DATA_PROCESS_H
#define US_DATA_PROCESS_H

#include "us_extern.h"
#include "us_data_model.h"
#include "us_dataIO.h"
#include "us_db2.h"

//! \class US_DataProcess
//! \brief Class to manage data processing operations, including database interactions and file handling.
class US_DataProcess : public QObject
{
    Q_OBJECT

    public:
        //! \brief Statuses of a record action besides 0 and the errors
        enum Status
        {
            NO_PARENT  = 3062,  //!< The parent is not where the record goes
            UNSAFE_RUN = 3065,  //!< The run is marked "(!)"; see
                                //!< US_DataModel::RunEntry::unsafe and
                                //!< US_DataModel::RunEntry::copyOf
            IN_SYNC    = 3070   //!< Not an error: a DB replace found the
                                //!< database already holds the file
        };

        //! \brief The status of a delete_editedData that keeps the edit
        //!        and has changed nothing: the edit is not the caller's,
        //!        or, unless the caller is an administrator, removing it
        //!        would take along records of another investigator (noise
        //!        computed from it for their model of another edit, say) or
        //!        records the GMP framework refers to
        static const int EDIT_KEPT = IUS_DB2::NOTPERMITTED;

        //! \brief Constructor for US_DataProcess
        //! \param model Pointer to US_DataModel object
        //! \param parent Optional parent widget
        US_DataProcess( US_DataModel* model, QWidget* parent = 0 );

        /*! \brief Upload a record to the database

            Refused when its raw ancestor is not in both places, when its
            parent is not in the database, or when its run is unsafe.  A
            record already in the database is replaced only when the two
            copies differ: when they hold the same thing this returns
            \ref IN_SYNC and nothing is written, and when they differ a
            local copy older than the database copy (both dates read as
            UTC) is refused.  The local file is hashed again for this: a
            comparison made before the file changed says nothing about it.

            A model or a noise record is read from its local file, and is
            not written when that file cannot be read.  Its row then holds
            the database ID of the record the write created or updated,
            looked up by its GUID: a replace keeps the ID the row had.
            When that ID cannot be read (3047), the row still says the
            record is in the database, with no ID to act on it by (-1)
            until a new scan reads it.
            \param recordID The row of the record to be uploaded
            \return 0, \ref IN_SYNC, or an error status
        */
        int record_upload( int recordID );

        /*! \brief Download a record from the database

            Refused when its raw ancestor is not in both places, when its
            parent is not on local disk -- the file would be an orphan
            there; download the parent first, or download from the parent,
            which takes its descendants along -- or when its run is unsafe.
            \param recordID The row of the record to be downloaded
            \return Status code of the operation
        */
        int record_download( int recordID );

        /*! \brief Remove a record from the database

            The database removes what hangs off the record with it.  Refused
            (\ref UNSAFE_RUN) for a record of a copy of a run.

            An edit is removed in a transaction.  A patched
            delete_editedData removes with it all noise recorded against it,
            and changes nothing when it fails; unless the caller is an
            administrator it refuses (\ref EDIT_KEPT) when that would take
            along records of another investigator, or records the GMP
            framework refers to, and that is not tried again.  An older one does
            not remove noise of another edit's model recorded against the
            edit (a global fit, say): it removes the edit's models and noise
            and then fails on that noise (fk_noise_editDataID).  The rollback
            gives them back, and when that noise is the investigator's own it
            is removed first in a second transaction -- it was computed from
            the edit that goes -- and the edit asked for again; another
            investigator's is not listed, and keeps the edit there.  No
            other refusal is tried again: not a lock wait timeout or a
            deadlock, which another session's work caused and a second try
            would only wait on again, nor a lost connection.  A failure the
            transaction gave back as a whole -- the rollback succeeded, or
            the server rolled it back for a deadlock -- changed nothing,
            and the message says so.  After any other failure, when the
            outcome is not confirmed, or when the database says the edit is
            not there (510, or 508 from an older one), what the database
            holds is read back -- the edit, its models and noise, and that
            other noise -- and every row of a record found gone is marked so
            (\ref mark_removed_from_db); an edit found gone is removed.  The
            message then says what was removed all the same, if anything
            was, and when it could not all be read back, what to do: scan
            again, or after a lost connection, which this window does not
            make anew, close the window and open it again.  A connection
            lost while a removal runs or is committed leaves open whether
            it happened: the message says so (\ref outcome_unknown), also
            for a model or noise record, each of which is one statement.
            A rollback refused because the server had already ended the
            transaction (a deadlock, or a lock wait timeout with
            innodb_rollback_on_timeout) is not reported as a failed one.
            \param recordID The row of the record to be removed
            \return 0, or 2012 (the database kept the edit), 2016 (no
                    database ID), 2017 (the edit's noise could not be
                    listed), 2018 (that noise could not be removed), 2019
                    (no transaction), or another error status
        */
        int record_remove_db( int recordID );

        //! \brief Whether the outcome of the last \ref record_remove_db
        //!        that failed is not known: the connection was lost while
        //!        it ran or was committed, and what the database holds could
        //!        not be read back.  Its message then says so.
        bool outcome_unknown( void ) const { return unknown_out; }

        //! \brief Whether the connection to the database was seen lost:
        //!        every later database action fails too, since this window
        //!        does not connect again
        bool connection_lost( void ) const { return conn_lost; }

        //! \brief Remove a record from local storage
        //!
        //! Refused (\ref UNSAFE_RUN) for a record of a copy of a run
        //! (US_DataModel::RunEntry::copyOf): its models and noise are the
        //! run's own files.
        //! \param recordID ID of the record to be removed
        //! \return Status code of the operation
        int record_remove_local( int recordID );

        //! \brief Get the last error message
        //! \return Last error message
        QString lastError( void );

        //! \brief Replace the last error message
        //! \param message The message
        void setError( const QString& message );

        //! \brief Handle partial errors
        //! \param errCode Error code
        //! \param errCount Error count
        void partialError( int errCode, int errCount );

        //! \brief Append an error message to the log
        //! \param message Error message to append
        void appendError( QString message );

        //! \brief Check if the raw ancestor record is okay
        //! \param recordID ID of the record to check
        //! \return True if the raw ancestor record is okay, false otherwise
        bool raw_ancestor_ok( int recordID );

        //! \brief True when a record's parent is in the given place
        //! \param row   The row of the record
        //! \param place US_DataModel::REC_DB or REC_LO
        bool parent_in( int row, int place );

        //! \brief True, with the error message set, when the run of a
        //!        record is one nothing may be written for
        bool unsafe_run( int row );

        //! \brief True, with the error message set, when a record belongs
        //!        to a copy of a run (US_DataModel::RunEntry::copyOf):
        //!        nothing is uploaded, downloaded or removed for it
        bool copy_run  ( int row );

        //! \brief Whether the two copies of a record hold the same thing
        //!
        //! The local file is always hashed again; the database copy's
        //! checksum is the one the last comparison read, when there is one.
        //! \returns 1 when they do, 0 when they differ, -1 when one of
        //!          them could not be read
        int  same_contents( const US_DataModel::DataDesc& desc );

        //! \brief Mark a row as no longer in the database, as a removal or
        //!        a removal's cascade leaves it
        //!
        //! The model is told too (US_DataModel::removed_from_db), so that
        //! an experiment read later does not show it, or what the database
        //! removed with it, from what the catalogs read before.
        //! \param row The row
        void mark_removed_from_db( int row );

        //! \brief A noise record of another edit's model, recorded
        //!        against an edit
        class ForeignNoise
        {
            public:
                int     noiseID;      //!< Its database ID
                int     modelID;      //!< The database ID of its model
                QString guid;         //!< Its GUID
                QString description;  //!< Its description
        };

        //! \brief The noise recorded against an edit whose model belongs
        //!        to another edit, as the investigator's listing gives it
        //! \param editID The editedDataID
        //! \param noise  Filled in
        //! \returns False, with the message set, when it cannot be listed
        bool foreign_noise_of_edit( int editID, QList< ForeignNoise >& noise );

    private:
        //! \brief The row's record ID after a model or noise write
        //! \param proc "get_modelID" or "get_noiseID"
        //! \param guid The GUID the record was written with
        //! \returns 0, or an error status with the message set
        int  written_record_id( const QString& proc, const QString& guid );

        //! \brief Remove an edit, and the noise of other edits' models
        //!        recorded against it
        //!
        //! The edit is asked for first, as the database has it; only when
        //! an older database refuses it while the investigator's own noise
        //! of another edit's model refers to it is that noise removed
        //! first, in a second transaction.
        //! \returns 0, or an error status with the message set
        int  remove_edit( int row );

        //! \brief One transaction of an edit removal: the noise \a first,
        //!        then the edit
        //! \param why   Set to what went wrong
        //! \param clean Set false when a failure could not be rolled back
        //!              as a whole, or a commit was not confirmed
        //! \returns 0 once committed, 2012 (edit refused), 2018 (noise
        //!          refused) or 2019 (no transaction, or no commit)
        int  edit_attempt( int editID, const QList< ForeignNoise >& first,
                           QString& why, bool& clean );

        /*! \brief Read back what the database holds after an edit removal
                   that failed and was not given back as a whole, whose
                   outcome is not known, or of an edit the database says
                   is not there

            Every row of a record found gone -- the edit's own models and
            noise, and the foreign noise -- is marked as gone.
            \param row     The row of the edit
            \param foreign The foreign noise the removal took on
            \param stat    The status of the removal so far
            \param why     What went wrong
            \returns 0 when the edit is gone after all, else an error
                     status with the message set
        */
        int  settle_edit( int row, const QList< ForeignNoise >& foreign,
                          int stat, const QString& why );

        //! \brief Whether a record is still in the database
        //! \param proc   "get_editedData", "get_model_info", ...
        //! \param id     Its database ID
        //! \param gone   The status that says it is not there
        //! \returns 1 present, 0 gone, -1 when the database did not say
        int  db_has( const QString& proc, int id, int gone );

        //! \brief Mark as gone every noise row of a database ID, and the
        //!        record itself for merges to come, shown yet or not
        void mark_noise_removed( int noiseID );

        //! \brief What kind of failure an edit removal's last write had
        enum EditFailure
        {
            EDIT_REFUSED,      //!< Refused, for no reason of those below
            EDIT_KEPT_BY_DB,   //!< \ref EDIT_KEPT
            EDIT_FK,           //!< Another record refers to the edit (an
                               //!< older database: fk_noise_editDataID)
            EDIT_MISSING,      //!< No such edit, or no experiment of it
            EDIT_DEADLOCK,     //!< A deadlock (1213): the server rolled
                               //!< back the whole transaction
            EDIT_TIMEOUT,      //!< A lock wait timeout (1205)
            EDIT_LOST          //!< The connection was lost
        };

        //! \brief The kind of failure a write's status, the server's error
        //!        number and the message say -- as an older procedure has
        //!        the server report it, or a patched one reports it itself
        EditFailure failure_kind( int status, int serverErrno,
                                  const QString& error );

        //! \brief What a kind of failure means, to append to the message
        QString     failure_text( EditFailure kind );

        //! \brief What to do when what the database holds could not be
        //!        read back: scan again, or, after a lost connection, open
        //!        the window again
        QString     read_back_advice( void ) const;

        //! \brief Whether the connection to the database is gone: the last
        //!        error says so (US_DB2::isConnectionLoss), or the
        //!        connection does
        bool        lost_now( void );

        //! \brief What to do once the connection to the database is lost
        QString     lost_text( void ) const;

        //! \brief The message of a model or noise removal that failed,
        //!        naming the record; after a lost connection, that whether
        //!        it was removed is not known (\ref outcome_unknown)
        QString     removal_error( const QString& proc, int status );

        //! \brief Whether a refused rollback's message says the server had
        //!        already ended the transaction
        static bool ended_by_server( const QString& error );

        //! \brief Append \ref lost_text to the message of a failure on a
        //!        lost connection, or of any failure after one
        //! \returns \a stat
        int         note_lost( int stat );

        int                     edit_status = 0;  //!< delete_editedData's
                                                  //!< status, last try
        EditFailure             edit_failure = EDIT_REFUSED; //!< ... its kind
        bool                    conn_lost = false; //!< The connection was
                                                   //!< seen lost
        bool                    unknown_out = false; //!< See
                                                     //!< outcome_unknown()
        QString                 errMsg;     //!< Message from the last error
        QWidget*                parentw;    //!< Parent widget
        IUS_DB2*                db;         //!< Pointer to database connection
        US_DataModel*           da_model;   //!< Data model object
        US_DataModel::DataDesc  cdesc;      //!< Current record description
        int                     dbg_level;  //!< Debug level

    private slots:
        //! \brief Get the filename of a model record
        //! \param desc Pointer to DataDesc object
        //! \return Filename of the model record
        QString get_model_filename( US_DataModel::DataDesc* desc );

        //! \brief Get the filename of a model record
        //! \param guid GUID of the model record
        //! \return Filename of the model record
        QString get_model_filename( QString guid );

        //! \brief Get the filename of a noise record
        //! \param desc Pointer to DataDesc object
        //! \return Filename of the noise record
        QString get_noise_filename( US_DataModel::DataDesc* desc );

        //! \brief Get the filename of a noise record
        //! \param guid GUID of the noise record
        //! \return Filename of the noise record
        QString get_noise_filename( QString guid );
};

#endif // US_DATA_PROCESS_H
