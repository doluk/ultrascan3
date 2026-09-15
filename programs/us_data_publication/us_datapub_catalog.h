//! \file us_datapub_catalog.h
#ifndef US_DATAPUB_CATALOG_H
#define US_DATAPUB_CATALOG_H

#include <QtCore>

#include "us_datapub_defs.h"
#include "us_data_catalog.h"
#include "us_db2.h"

/*! \class US_DataPubCatalog
    \brief A read-only view of the records available for export

    The catalog answers the same questions against the UltraScan3 database
    and against a local UltraScan3 data directory, so that the export code
    -- and the export user interface -- does not have to care which of the
    two the user picked.

    Every record is reported with its ID, its GUID and the name a user
    recognizes it by.  Records read from a local disk store frequently have
    no meaningful ID (the placeholder "-1" is used then), so callers must
    key on the GUID.

    The record chain itself -- experiment, raw data, edit, model, noise --
    comes from \ref US_DataCatalog, so a bundle is built from the same walk
    every other program browses with.  What is added here is what only a
    publication needs: the projects, the experiment XML of a run, and its
    time state.
*/
class US_DataPubCatalog
{
   public:
      //! \brief A project available for export
      class Project
      {
         public:
            Project() : id( "-1" ) {}
            QString id;            //!< projectID
            QString guid;          //!< projectGUID
            QString description;   //!< Project description
            QString filename;      //!< Local file, when read from disk
      };

      /*! \class Noise
          \brief A noise record available for export

          The record itself is the catalog's.  What an export adds is where
          it came from when it is not the model's own: noise is fitted to an
          edit, and a model with none of its own travels with the noise of
          its edit that was there when it was made.
      */
      class Noise : public US_DataCatalog::Noise
      {
         public:
            Noise() {}
            Noise( const US_DataCatalog::Noise& base )
               : US_DataCatalog::Noise( base ) {}

            //! GUID of the model this record was really fitted to, when it
            //! is carried for a different model of the same edit
            QString borrowedFrom;
      };

      //! \brief A model record available for export
      typedef US_DataCatalog::Model Model;

      //! \brief An edit profile available for export
      typedef US_DataCatalog::Edit  Edit;

      //! \brief A raw-data triple available for export
      typedef US_DataCatalog::Raw   Raw;

      //! \brief An experiment (run) available for export
      typedef US_DataCatalog::Run   Run;

      /*! \class ExpInfo
          \brief The parts of a run's experiment XML an export needs

          The experiment XML file of a run is the only place a local disk
          store records which project, rotor calibration, centerpieces and
          solutions a run was made with, so it is read even when the export
          is scoped to the experiment alone.
      */
      class ExpInfo
      {
         public:
            ExpInfo();

            QString expID;         //!< experimentID
            QString expGUID;       //!< experimentGUID
            QString runID;         //!< The run identifier
            QString expType;       //!< velocity, equilibrium, ...
            QString label;         //!< The experiment label
            QString comments;      //!< Free-text comments
            QString projectID;     //!< projectID of the owning project
            QString projectGUID;   //!< projectGUID of the owning project
            QString projectDesc;   //!< Description of the owning project
            QString rotorID;       //!< rotorID used for the run
            QString rotorGUID;     //!< rotorGUID used for the run
            QString rotorSerial;   //!< Serial number of the rotor
            QString rotorName;     //!< Name of the rotor
            QString calibrationID; //!< rotorCalibrationID used for the run

            QStringList centerpieceIDs; //!< Centerpiece serial numbers used
            QStringList solutionIDs;    //!< solutionIDs used by the triples
            QStringList solutionGUIDs;  //!< solutionGUIDs used by the triples
            QStringList solutionDescs;  //!< Descriptions of those solutions

            //! "cell/channel" -> solutionGUID of that channel
            QMap< QString, QString > tripleSolutionGUIDs;

            //! "cell/channel" -> centerpiece serial number of that channel
            QMap< QString, QString > tripleCenterpieceIDs;

            //! \brief Read the experiment XML of a run
            //! \param filename The full path of the experiment XML file
            //! \returns True when the file could be read
            bool readFromFile( const QString& filename );
      };

      //! \brief Construct a catalog that is not connected to any source
      US_DataPubCatalog();

      //! \brief Release the database connection, if one was opened
      ~US_DataPubCatalog();

      //! \brief Connect the catalog to its source
      //! \param fromDb     True to read from the database, false for disk
      //! \param dbPassword The master password, for database access
      //! \param error      Filled in with a message when opening fails
      //! \returns True when the source is usable
      bool open( bool fromDb, const QString& dbPassword, QString& error );

      //! \brief True when the catalog reads from the database
      bool isDb( void ) const;

      /*! \brief Whether to use the catalog procedures added for this

          They read a whole experiment in a handful of queries, and the
          catalog already falls back by itself on the procedures UltraScan
          has always had when a server does not have them, so the only
          reason to turn them off is to exercise that fallback.
          \param on True to use them
      */
      void setBulkQueries( bool on );

      //! \brief The open database connection, or null for a disk catalog
      US_DB2* db( void );

      //! \brief All projects of the current investigator
      //! \param error Filled in with a message when the scan fails
      QList< Project > projects( QString& error );

      //! \brief All runs of the current investigator
      //! \param projectGUID When not empty, only runs of that project
      //! \param error       Filled in with a message when the scan fails
      QList< Run > runs( const QString& projectGUID, QString& error );

      //! \brief Look up one run by its run identifier
      //! \param runID The run identifier to look for
      //! \param found Filled in with the run header when it was found
      //! \param error Filled in with a message when the scan fails
      //! \returns True when the run exists in the source
      bool runByID( const QString& runID, Run& found, QString& error );

      //! \brief Fill in the raw-data triples and edits of a run
      //! \param run   The run to complete, with its header already filled in
      //! \param error Filled in with a message when the scan fails
      //! \returns True when the details could be read
      bool loadRunDetails( Run& run, QString& error );

      //! \brief All models that belong to the edits of the given runs
      //! \param runs  The runs whose models are wanted
      //! \param error Filled in with a message when the scan fails
      QList< Model > models( const QList< Run >& runs, QString& error );

      //! \brief All noise records that belong to the given models
      //! \param models The models whose noise records are wanted
      //! \param error  Filled in with a message when the scan fails
      QList< Noise > noises( const QList< Model >& models, QString& error );

      //! \brief The path of the local time-state file of a run
      //!
      //! For a database catalog the file is downloaded into \a workDir
      //! first; for a disk catalog the file in the run directory is used.
      //! \param run     The run whose time state is wanted
      //! \param workDir A directory the file may be downloaded into
      //! \param tmstPath Filled in with the path of the .tmst file
      //! \param xdefPath Filled in with the path of its .xml sibling
      //! \returns True when a time state exists for the run
      bool timeState( const Run& run, const QString& workDir,
                      QString& tmstPath, QString& xdefPath );

      /*! \brief The noise a model with none of its own is exported with

          Noise is fitted to an edit, and a model made afterwards is run
          with the noise that was already there, so what travels with such
          a model is the newest noise of its edit that existed when the
          model was made -- one record of each type, because a run can
          carry a time-invariant and a radially-invariant record at once.

          A candidate whose time stamp cannot be read is not ruled out by
          the model's, but it loses to any candidate that has one.
          \param candidates The noise records of the model's edit
          \param modelStamp The model's time stamp
          \returns At most one record per noise type
      */
      static QList< US_DataCatalog::Noise > noiseBefore(
            const QList< US_DataCatalog::Noise >& candidates,
            const QString& modelStamp );

      //! \brief Read the attributes of the first element of an XML file
      //! \param filename The full path of the XML file to peek into
      //! \param element  The element name whose attributes are wanted
      //! \returns The attributes of the first such element, or an empty map
      static QMap< QString, QString > peekAttributes( const QString& filename,
                                                      const QString& element );

      //! \brief The investigator ID the catalog reads records for
      int investigatorID( void ) const;

      //! \brief The experiment information of a run
      //!
      //! For a disk catalog the run's experiment XML file is read; for a
      //! database catalog the record is read from the database and the
      //! equivalent information is filled in.
      //! \param run   The run whose experiment information is wanted
      //! \param info  Filled in with the experiment information
      //! \param error Filled in with a message when the read fails
      //! \returns True when the information could be read
      bool expInfo( const Run& run, ExpInfo& info, QString& error );

      //! \brief The path of the experiment XML file of a run on disk
      //! \param run The run whose experiment XML file is wanted
      //! \returns An empty string when the catalog does not read from disk
      static QString expFilePath( const Run& run );

   private:
      bool            from_db;
      US_DB2*         dbase;
      int             inv_id;
      US_DataCatalog* cat;
      QString         listed_project;
      bool            listed;

      //! \brief Make sure the first layer of the shared catalog is read
      //! \param projectGUID When not empty, only runs of that project
      //! \param error       Filled in with a message when the scan fails
      bool ensureListed( const QString& projectGUID, QString& error );

      QList< Project > projectsDb  ( QString& );
      QList< Project > projectsDisk( QString& );
};
#endif
