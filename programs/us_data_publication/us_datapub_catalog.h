//! \file us_datapub_catalog.h
#ifndef US_DATAPUB_CATALOG_H
#define US_DATAPUB_CATALOG_H

#include <QtCore>

#include <functional>

#include "us_datapub_defs.h"
#include "us_data_catalog.h"
#include "ius_db2.h"

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
            //! rotorCalibrationID used for the run; empty when the record
            //! names none (the rotor's first calibration is not assumed)
            QString calibrationID;

            /*! The GUID of that calibration, when the experiment XML names
                it (the \c guid of its \c calibration element): an export
                puts it there, so that a disk store the bundle is imported
                into -- where the calibration has no ID of its own -- still
                says which calibration the run was made with
            */
            QString calibrationGUID;

            QStringList centerpieceIDs; //!< Centerpiece serial numbers used
            QStringList solutionIDs;    //!< solutionIDs used by the triples
            QStringList solutionGUIDs;  //!< solutionGUIDs used by the triples
            QStringList solutionDescs;  //!< Descriptions of those solutions

            //! "cell/channel" -> solutionGUID of that channel
            QMap< QString, QString > tripleSolutionGUIDs;

            //! "cell/channel" -> centerpiece serial number of that channel
            QMap< QString, QString > tripleCenterpieceIDs;

            //! The columns of the experiment row that are NULL in the
            //! database: "centrifugeProtocol" and/or "RIProfile"
            //! (get_experiment_info_by_runID columns 12 and 16, which
            //! US_DB2::value() gives as a null string, and an empty one as
            //! an empty string).  Always empty for a disk store, whose XML
            //! cannot tell NULL from empty.
            QStringList nullFields;

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

      /*! \brief Read from a database connection the caller owns

          The queries this class makes itself -- the projects, a run's
          experiment information and its time state -- use the connection,
          and so does the record chain when the connection says it is
          connected (US_DataCatalog::attach()).  It lets those queries run
          against any IUS_DB2, a test double included.  The connection is
          not taken over: it has to outlive the catalog's use of it.
          \param db The connection to read from
      */
      void attachDb( IUS_DB2* db );

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

      /*! \brief A flag that, once set, makes the catalog's reads stop
          with an error that says they were cancelled

          The shared catalog checks it between the files of a local store
          (US_DataCatalog::setCancelFlag); this class checks it as well
          before each run it reads in detail and before each model whose
          noise it looks up, so that a database read stops too, between the
          reads of two records (the queries of one read are not stopped).
          It has to outlive the catalog's use of it.
          \param flag The flag, or null for none
      */
      void setCancelFlag( const QAtomicInt* flag );

      /*! \brief Where the notes of the shared catalog go

          US_DataCatalog says what it is doing while it lists and reads a
          source ("Indexing the local data directory...", the copies of a
          run it ignores, a database without the catalog procedures);
          \a handler gets each of those notes as it is made.  A handler
          set before replaces it; an empty one drops them.
          \param handler What to call with each note
      */
      void setMessageHandler(
            const std::function< void( const QString& ) >& handler );

      //! \brief The open database connection, or null for a disk catalog
      IUS_DB2* db( void );

      //! \brief All projects of the current investigator
      //! \param error Filled in with a message when the scan fails
      QList< Project > projects( QString& error );

      //! \brief All runs of the current investigator
      //! \param projectGUID When not empty, only runs of that project
      //! \param error       Filled in with a message when the scan fails
      QList< Run > runs( const QString& projectGUID, QString& error );

      /*! \brief Look up one run by its run identifier

          A local store can hold a run ID in more than one directory: a
          "Copy of ...", "Backup_..." or "..._old" directory made by hand
          holds the same files under the same names.  The directory named
          after the run ID is the run; the catalog marks the others as
          copies of it (Run::copyOf) and as not usable, and they are never
          taken for the run: the run is found, and \a copies names them so
          that the caller can warn about them.  When no directory is named
          after the run ID, which one is the run cannot be told: every one
          is not usable, and the run is refused with a message naming every
          directory.  A run whose directory is not named after it, or whose
          own directory is not usable, is refused too, and \a unusable then
          says that it is the run that is at fault, not the source.

          A database can hold more than one experiment of an investigator
          under one run ID.  Which of them is meant then cannot be told
          from the run ID: without \a experimentID the run is refused, as
          not usable, with a message naming every experiment ID; with it,
          the experiment of that ID is the run (a disk catalog ignores it).
          \param runID    The run identifier to look for
          \param found    Filled in with the run header when it was found
          \param error    Filled in with a message when the scan fails or
                          the run cannot be used
          \param unusable When given, set to true when the run is there but
                          is not one to act on
          \param copies   When given, filled in with the directories (native
                          separators, sorted) that hold copies of the run
                          found and were ignored
          \param experimentID When not empty, the database ID of the
                          experiment to take among those of the run ID
          \returns True when exactly one usable run has the run ID
      */
      bool runByID( const QString& runID, Run& found, QString& error,
                    bool* unusable = nullptr,
                    QStringList* copies = nullptr,
                    const QString& experimentID = QString() );

      //! \brief Fill in the raw-data triples and edits of a run
      //!
      //! A run without raw data is not an error here; whether that matters
      //! depends on what is being exported, which the caller knows.
      //! \param run   The run to complete, with its header already filled in
      //! \param error Filled in with a message when the scan fails
      //! \returns True when the details could be read
      bool loadRunDetails( Run& run, QString& error );

      /*! \brief Fill in where a model picked by GUID stands in the source

          A model the chain of the runs lists carries its ID, file and
          last-updated time; one picked by its GUID alone (outside the
          runs read, or the model a borrowed noise was fitted to) does
          not.  This looks them up from \a model.guid: in the database its
          modelID and lastUpdated (get_modelID, get_model_info), on disk
          its M*.xml file (filename, path) and that file's time, as the
          chain gives them.  Nothing is changed when the source has no
          such model.
          \param model The model, its GUID set
          \param error Filled in when the database cannot be asked
          \returns False only when the database cannot be asked
      */
      bool completeModel( Model& model, QString& error );

      //! \brief All models that belong to the edits of the given runs
      //! \param runs  The runs whose models are wanted
      //! \param error Filled in with a message when the scan fails
      QList< Model > models( const QList< Run >& runs, QString& error );

      /*! \brief All noise records that belong to the given models

          Each record is listed once.  A record a model with no noise of its
          own is carried with (see \ref noiseBefore()) is listed for the
          first model that names it, so a model that borrows a record
          another model of \a models lists already is not seen in the list:
          \a borrowers names every one.
          \param models    The models whose noise records are wanted
          \param error     Filled in with a message when the scan fails
          \param borrowers When given, filled in with noise GUID -> GUID of
                           every model of \a models that borrows it
      */
      QList< Noise > noises( const QList< Model >& models, QString& error,
                             QMultiHash< QString, QString >* borrowers
                                = nullptr );

      //! \brief What looking for the time state of a run found
      enum TimeStateStatus
      {
         TimeStateNone = 0,   //!< The run has no time state; that is normal
         TimeStateFound,      //!< The time state and its definitions exist
         TimeStateFailed,     //!< It could not be looked up or fetched
         //! There is one, but without its field definitions: the run's
         //! own records do not hold together
         TimeStateBroken
      };

      /*! \brief Find the time state of a run

          A time state is the binary \c .tmst together with the \c .xml of
          its field definitions; one without the other cannot be read, so a
          binary without its definitions is \ref TimeStateBroken, not a time
          state.  A source that fails -- a lookup or download that does not
          work -- is \ref TimeStateFailed.
          For a database catalog the files are downloaded into \a workDir
          when \a fetch is set, always as \c <runID>.time_state.tmst and
          its \c .xml whatever file name the record keeps; for a disk
          catalog the files in the run directory are used.
          \param run      The run whose time state is wanted
          \param workDir  A directory the files may be downloaded into
          \param fetch    Download the files (database only)
          \param tmstPath Filled in with the path of the .tmst file
          \param xdefPath Filled in with the path of its .xml sibling
          \param tmstID   Filled in with the time state's database ID, or -1
          \param error    Filled in when the result is \ref TimeStateFailed
                          or \ref TimeStateBroken
      */
      TimeStateStatus timeState( const Run& run, const QString& workDir,
                                 bool fetch, QString& tmstPath,
                                 QString& xdefPath, QString& tmstID,
                                 QString& error );

      /*! \brief The noise a model with none of its own is exported with

          Noise is fitted to an edit, and a model made afterwards is run
          with the noise that was already there, so what travels with such
          a model is the newest noise of its edit that existed when the
          model was made -- one record of each type, because a run can
          carry a time-invariant and a radially-invariant record at once.

          A candidate whose time stamp cannot be read is not ruled out by
          the model's, but it loses to any candidate that has one.  A
          candidate dated the same second as the model counts as made
          before it.  Two candidates of a type dated the same second (or
          both undated) are told apart by their database IDs, the higher
          one -- written last -- winning, and where either has none by
          their GUIDs, the higher one winning; the order of \a candidates
          never decides.  An import writes the records in the source's
          order, the target's clock moved on where a tie would decide
          otherwise (US_DataPubImporter::writeOrder()).  The time stamps
          are UTC, as the catalog gives them ("yyyy-MM-dd hh:mm:ss" with or
          without " UTC"), and are read as UTC, never as local times.
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
      //! equivalent information is filled in.  A database run is read by
      //! its experiment ID (Run::id) when it has one, so that of two
      //! experiments of one run ID the right one is read.
      //! \param run   The run whose experiment information is wanted
      //! \param info  Filled in with the experiment information
      //! \param error Filled in with a message when the read fails
      //! \param runFault When given, set to true when the failure is the
      //!        run's own -- its experiment XML is missing or is not XML,
      //!        or its record is gone -- rather than the source's
      //! \returns True when the information could be read
      bool expInfo( const Run& run, ExpInfo& info, QString& error,
                    bool* runFault = nullptr );

      //! \brief The path of the experiment XML file of a run on disk
      //! \param run The run whose experiment XML file is wanted
      //! \returns An empty string when the catalog does not read from disk
      static QString expFilePath( const Run& run );

   private:
      bool            from_db;
      IUS_DB2*        dbase;
      int             inv_id;
      US_DataCatalog* cat;
      QString         listed_project;
      bool            listed;
      const QAtomicInt* cancel_flag;
      QMetaObject::Connection message_link;

      //! \brief True, with \a error filled in, once the cancel flag is set
      bool cancelledError( QString& error ) const;

      //! \brief Drop what was listed when the investigator has changed
      void syncInvestigator( void );

      //! \brief The catalog positions of the runs of a run ID: the exact
      //!        matches, or failing those, the runs of the spelling the
      //!        source's own comparison finds (see indexOfRun())
      QList< int > indexesOfRun( const QString& runID ) const;

      //! \brief The catalog position of a run looked up before: the one of
      //!        its run ID in its directory, or -1
      int indexOfListedRun( const Run& run ) const;

      //! \brief Make sure the first layer of the shared catalog is read
      //! \param projectGUID When not empty, only runs of that project
      //! \param error       Filled in with a message when the scan fails
      bool ensureListed( const QString& projectGUID, QString& error );

      QList< Project > projectsDb  ( QString& );
      QList< Project > projectsDisk( QString& );
};
#endif
