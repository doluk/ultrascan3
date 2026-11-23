//! \file us_datapub_export.h
#ifndef US_DATAPUB_EXPORT_H
#define US_DATAPUB_EXPORT_H

#include <QtCore>

#include "us_datapub_defs.h"
#include "us_datapub_manifest.h"
#include "us_datapub_bundle.h"
#include "us_datapub_catalog.h"

#include "us_convert.h"
#include "us_experiment.h"
#include "us_hardware.h"
#include "us_buffer.h"

/*! \class US_DataPubExporter
    \brief Collects records into a data-publication bundle

    The exporter walks the dependency chain from the project down to the
    noise records, stages a payload file for every record it is asked to
    export, and writes the manifest that ties them together.  Whether the
    records are read from the database or from the local disk store is
    decided by US_DataPubSelection::fromDb; everything else about the run is
    the same either way.
*/
class US_DataPubExporter : public QObject
{
   Q_OBJECT

   public:
      /*! \class Selection
          \brief What an export run should put into the bundle
      */
      class Selection
      {
         public:
            Selection();

            bool        fromDb;      //!< Read the records from the database
            QString     dbPassword;  //!< Master password, for database access

            //! The deepest record type to export; everything before it in
            //! the dependency chain is exported as well
            US_DataPub::Scope scope;

            QString     projectGUID; //!< Export this project, if known
            QStringList runIDs;      //!< The runs to export

            QStringList rawGUIDs;    //!< Raw triples to export
            QStringList editGUIDs;   //!< Edits to export
            QStringList modelGUIDs;  //!< Models to export
            QStringList noiseGUIDs;  //!< Noise records to export

            bool        rawsExplicit;   //!< Use \ref rawGUIDs as given
            bool        editsExplicit;  //!< Use \ref editGUIDs as given
            bool        modelsExplicit; //!< Use \ref modelGUIDs as given
            bool        noisesExplicit; //!< Use \ref noiseGUIDs as given

            bool        includeTimeState; //!< Carry the runs' time state

            /*! \brief Carry the project the runs belong to

                A bundle that leaves the project out still names it in the
                run's experiment XML, so an import can attach the run to a
                project the target already has.  Leave it out when the
                project record itself is not yours to publish, or when the
                receiving installation keeps its own project list.
            */
            bool        includeProject;

            QString     comment;          //!< Free-text bundle comment

            //! \brief Select exactly these raw triples
            void setRaws  ( const QStringList& );
            //! \brief Select exactly these edits
            void setEdits ( const QStringList& );
            //! \brief Select exactly these models
            void setModels( const QStringList& );
            //! \brief Select exactly these noise records
            void setNoises( const QStringList& );

            /*! \brief Select what a tree of the runs has checked

                What the export tab shows: the runs, with their raw data,
                edits, models and noise checked or not.  When \a runsListed
                is false -- only a project was picked, and no run is shown
                -- nothing is narrowed: every run of the project goes, with
                everything it holds, as an export of the project on the
                command line does.
                \param runsListed Whether any run is listed
                \param raws       The raw data checked
                \param edits      The edits checked
                \param models     The models checked
                \param noises     The noise records checked
            */
            void setChecked( bool runsListed, const QStringList& raws,
                             const QStringList& edits,
                             const QStringList& models,
                             const QStringList& noises );

            /*! \brief The scope an export actually runs with

                Raw data cannot be re-created in a target installation
                without the solutions its cells and channels refer to, so a
                scope that reaches the raw data is raised to the solutions.
            */
            US_DataPub::Scope effectiveScope( void ) const;
      };

      //! \brief Construct an exporter
      //! \param parent The parent object, for Qt ownership
      explicit US_DataPubExporter( QObject* parent = nullptr );

      //! \brief Destroy the exporter and its staging directory
      ~US_DataPubExporter();

      /*! \brief Collect the records and write the bundle

          The export fails -- and writes nothing -- when there would be
          nothing in the bundle, when a GUID it was explicitly given matches
          no record, when a run named belongs to a project other than the
          one asked for, and when any record it needs cannot be read.  When
          the runs come with the project rather than by name, a run whose
          own records do not hold together while it is staged -- a channel
          without a solution, a payload of another GUID, an experiment XML
          of another run ID or GUID, a spectrum that is not all numbers --
          is left out with a warning instead, and what was staged for it is
          taken back.  Noise carried for a model with none of its own
          brings the model it was fitted to along.  Next to
          \c manifest.yaml the bundle carries \c manifest.sha256, the
          digest of the manifest file in \c sha256sum format.
          \param selection  What to export
          \param bundlePath The full path of the .tar.gz to write
          \param error      Filled in with a message when the export fails
          \returns True when the bundle was written
      */
      bool exportBundle( const Selection& selection,
                         const QString& bundlePath, QString& error );

      //! \brief The bundle-relative name of the manifest digest file
      static QString manifestDigestName( void );

      /*! \brief Collect the record list without staging any payload

          This is what the user interface shows as the manifest preview: it
          answers "what would be exported" without copying a single byte of
          experimental data.
          \param selection What would be exported
          \param error     Filled in with a message when the scan fails
          \returns True when the record list could be built
      */
      bool previewManifest( const Selection& selection, QString& error );

      //! \brief The manifest of the last export or preview
      const US_DataPubManifest& manifest( void ) const;

      //! \brief The messages of the last export or preview
      QStringList log( void ) const;

      //! \brief Remove the staging directory of the last export
      void cleanup( void );

   signals:
      //! \brief A human readable note about the export's progress
      //! \param message The note
      void message ( const QString& message );

      //! \brief The number of records this export will deal with
      //! \param steps The number of records
      void steps   ( int steps );

      //! \brief The number of records dealt with so far
      //! \param step The number of records finished
      void stepDone( int step );

   public:
      /*! \brief The run type a run's payloads are named with

          The same answer for a preview and an export, from the database or
          from disk: the catalog's run type, or the data type of the run's
          first triple when the catalog has none.
      */
      static QString runTypeOf( const US_DataPubCatalog::Run& run );

      /*! \brief Whether an import would accept a run as the catalog lists it

          The rules of US_DataPub that a run's own records have to meet:
          its run ID and run type, the GUID of its experiment, and -- within
          \a scope -- the GUID and file name of each of its raw triples and
          edits (the file names have to start with the run ID).  A run of a
          project export that breaks one is left out with a warning; a run
          the user named fails the export.
          \param run   The run, with its details loaded and its triples
                       narrowed to those selected
          \param scope The scope of the export
          \param why   Filled in with the first rule the run breaks
      */
      static bool runIsAcceptable( const US_DataPubCatalog::Run& run,
                                   US_DataPub::Scope scope, QString& why );

   private:
      //! Lets the unit tests drive the steps of an export one at a time
      friend class US_DataPubExporterTest;

      US_DataPubManifest mani;
      US_DataPubBundle   bundle;
      US_DataPubCatalog  catalog;
      QStringList        logtext;
      bool               stage_payloads;
      int                step_count;

      //! The scope the current export runs with
      US_DataPub::Scope  exp_scope;

      //! The noise records that were asked for by GUID
      QStringList        explicit_noises;

      //! Whether the runs came with the project rather than by name: a run
      //! that cannot be exported is then left out instead of failing it
      bool               from_project;

      //! Whether the last failure is the run's own -- its records do not
      //! hold together -- rather than the export's (a database or a disk
      //! that fails); only the former lets a project export go on
      bool               run_fault;

      //! Every model the catalog lists for the runs, also those that were
      //! not selected: noise carried for one model may have been fitted to
      //! another, which then has to travel too
      QList< US_DataPubCatalog::Model > listed_models;

      //! runID -> GUID of the rotor calibration the run was made with
      QMap< QString, QString > cal_guids;

      //! rawDataGUID -> solutionGUID of the solution in its channel
      QMap< QString, QString > raw_solutions;

      //! runID -> GUIDs of the run's triples that were not selected
      QMap< QString, QStringList > dropped_raws;

      //! The abstract centerpiece table, read once per export
      QList< US_AbstractCenterpiece > cp_table;
      bool                            cp_loaded;

      //! The local buffer component table, read once per export
      QMap< QString, US_BufferComponent > hd_components;
      bool                                hd_comp_loaded;

      bool build( const Selection&, bool payloads, QString& error );

      /*! Stage the records of one run, within the export's scope: its
          rotor calibration and centerpieces, experiment, solutions, raw
          data, time state and edits.  A failure that is the run's own sets
          \ref run_fault.
      */
      bool stageRun     ( const Selection&, const US_DataPubCatalog::Run&,
                          const US_DataPubCatalog::ExpInfo&, QString& );

      //! Fail with \a text as a problem of the run itself
      bool runFault     ( QString& error, const QString& text );

      /*! The models the noise records were really fitted to: noise carried
          for a model that has none of its own belongs to another model of
          the same edit, which travels with it, or the noise stays out
      */
      bool addFittedModels( QList< US_DataPubCatalog::Model >&,
                            QList< US_DataPubCatalog::Noise >&, QString& );

      bool gatherRuns   ( const Selection&, QList< US_DataPubCatalog::Run >&,
                          QString& );
      bool addProjects  ( const Selection&,
                          const QList< US_DataPubCatalog::ExpInfo >&,
                          QString& );
      bool addProject   ( const QString& guid, const QString& id, QString& );
      bool addExperiment( const US_DataPubCatalog::Run&,
                          const US_DataPubCatalog::ExpInfo&, QString& );
      bool readExperimentDb( const US_DataPubCatalog::Run&, US_Experiment&,
                             QVector< US_SimulationParameters::SpeedProfile >&,
                             QString& );
      bool stageExperimentXml( const US_DataPubCatalog::Run&,
                               const QString& source, const QString& staged,
                               QString& );

      //! False with an empty error when the profile is left out
      bool stageRiProfile( const US_DataPubCatalog::Run&, const QByteArray&,
                           const QString& rel, QString& );

      //! Whether a raw data payload carries the GUID it is exported under
      static bool checkRawGuid  ( const QString& path, const QString& guid,
                                  QString& error );

      /*! Whether a staged experiment XML names the run and the GUID it is
          exported under; an import refuses one that does not
      */
      static bool checkExperimentXml( const QString& path,
                                      const QString& runID,
                                      const QString& guid, QString& error );

      /*! Whether every wavelength and value of the spectra is a finite
          number; an import refuses a spectrum that is not
      */
      static bool spectraAreFinite(
            const QMap< QString, QMap< double, double > >&, QString& why );

      //! Whether an edit payload carries its GUID and its raw data's
      static bool checkEditGuids( const QString& path, const QString& editGUID,
                                  const QString& rawGUID, QString& error );
      bool addRawData   ( const US_DataPubCatalog::Run&,
                          const US_DataPubCatalog::ExpInfo&, QString& );
      bool addTimeState ( const US_DataPubCatalog::Run&, QString& );
      bool addCalibration( const US_DataPubCatalog::ExpInfo&, QString& );
      bool addCenterpieces( const US_DataPubCatalog::Run&,
                            const US_DataPubCatalog::ExpInfo&, QString& );
      bool addSolutions ( const US_DataPubCatalog::Run&,
                          const US_DataPubCatalog::ExpInfo&, QString& );
      bool addBuffer    ( US_Buffer&, QString& );
      bool addAnalyte   ( US_Analyte&, QString& );
      //! A solution with its buffer and analytes, in a run of \a expGUID
      bool addSolution  ( US_Solution&, const QString& expGUID, QString& );
      bool addEdits     ( const US_DataPubCatalog::Run&, QString& );
      bool collectModels( const Selection&,
                          const QList< US_DataPubCatalog::Run >&,
                          QList< US_DataPubCatalog::Model >&, QString& );
      bool addModels    ( const QList< US_DataPubCatalog::Model >&, QString& );
      //! \a leftOut are the models of runs a project export left out:
      //! noise of theirs that was asked for by GUID goes with them
      bool collectNoises( const Selection&,
                          const QList< US_DataPubCatalog::Model >&,
                          QList< US_DataPubCatalog::Noise >&, QString&,
                          const QList< US_DataPubCatalog::Model >& leftOut
                             = QList< US_DataPubCatalog::Model >() );
      bool addNoises    ( const QList< US_DataPubCatalog::Noise >&, QString& );

      bool buildTriplesFromDb( const US_DataPubCatalog::Run&,
                               QList< US_Convert::TripleInfo >&, QString& );

      //! Fails with the reason in \a error when a payload cannot be digested
      bool finishEntity( US_DataPubEntity&, const QString& stagedPath,
                         QString& error );
      //! Stage the spectra of a record as its \c spectraPayload, when it
      //! has any -- or, with \a always, also an empty one, which says that
      //! the record has no measured spectrum
      bool stageSpectra( US_DataPubEntity&,
                         const QMap< QString, QMap< double, double > >&,
                         QString& error, bool always = false );
      /*! The RI intensity profile column of a database run's experiment
          record, as stored; empty when there is none or it cannot be read
      */
      QByteArray storedRiProfile( const US_DataPubCatalog::Run& );
      /*! Check, just before packing, that the staged tree is exactly the
          manifest, its digest file and the payloads the manifest names,
          that every payload still matches its digest and that every XML
          payload names only usable GUIDs
      */
      bool checkStagedTree( QString& error );
      bool copyPayload ( const QString& source, const QString& staged,
                         QString& error );
      void note        ( const QString& );
};
#endif
