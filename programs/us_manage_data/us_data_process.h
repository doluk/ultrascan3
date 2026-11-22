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
                                //!< US_DataModel::RunEntry::unsafe
            IN_SYNC    = 3070   //!< Not an error: a DB replace found the
                                //!< database already holds the file
        };

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
            UTC) is refused.
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

        //! \brief Remove a record from the database
        //! \param recordID ID of the record to be removed
        //! \return Status code of the operation
        int record_remove_db( int recordID );

        //! \brief Remove a record from local storage
        //! \param recordID ID of the record to be removed
        //! \return Status code of the operation
        int record_remove_local( int recordID );

        //! \brief Get the last error message
        //! \return Last error message
        QString lastError( void );

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

        //! \brief Whether the two copies of a record hold the same thing
        //! \returns 1 when they do, 0 when they differ, -1 when one of
        //!          them could not be read
        int  same_contents( const US_DataModel::DataDesc& desc );

    private:
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
