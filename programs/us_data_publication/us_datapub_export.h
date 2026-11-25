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

            /*! runID -> the database ID of the experiment to export under
                that run ID.  A database can hold more than one experiment
                of an investigator under one run ID; such a run is refused
                unless this names one of them.  A disk export ignores it.
            */
            QMap< QString, QString > experimentIDs;
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
          without a solution, a rotor calibration or centerpiece it names
          that the source does not have, a time state without its field
          definitions, a payload of another GUID, an experiment XML
          of another run ID or GUID, a spectrum that is not all numbers --
          is left out with a warning instead, and what was staged for it is
          taken back.  So is a run whose experiment, raw data or edit
          another run already brought into the bundle (a copied run
          directory), and one with a value that is not a finite number in a
          record (US_DataPub::nonFiniteNumberInXml()).  The models of a
          left-out run, and their noise, go with it unless their edit is in
          the bundle after all; one asked for by GUID fails the export.  A
          model or noise record with a value that is not finite is left out
          with a warning, or fails the export when asked for by GUID.  What
          was left out is in \ref warnings(), \ref leftOutRuns() and
          \ref leftOutRecords().  Noise carried for a model with none of its own
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

      /*! \brief The name of the lock an exporter holds in its staging
          directory, which US_DataPubBundle::sweepStaleStaging() tells a
          killed export's directory by.  Every export and preview sweeps
          before it stages.
      */
      static QString stagingLockName( void );

      /*! \brief Everything an export does, but write no bundle

          This is what the user interface shows as the manifest preview and
          the command line's \c --dry-run: every record is read and staged
          in a staging directory that is removed afterwards, and every check
          of \ref exportBundle() is made, so the records listed, the
          digests, and what is left out -- a run whose payload turns out
          not to hold together, a copy of another run -- are what the
          export would write.  It takes as long as an export, less the
          packing.
          \param selection What would be exported
          \param error     Filled in with a message when the export would fail
          \returns True when the export would succeed
      */
      bool previewManifest( const Selection& selection, QString& error );

      //! \brief The manifest of the last export or preview
      const US_DataPubManifest& manifest( void ) const;

      //! \brief The messages of the last export or preview
      QStringList log( void ) const;

      /*! \brief The warnings of the last export or preview

          What was left out of the bundle and why, and what it holds that
          an importer should know about, without the "Warning: " the log
          puts before each.  A warning about a run that was left out after
          all replaces those the run's own records gave while it was
          staged.  They are part of \ref log() as well; a caller that does
          not show the log has to show these.
      */
      QStringList warnings( void ) const;

      //! \brief The runs the last export or preview left out of the bundle
      QStringList leftOutRuns( void ) const;

      /*! \brief The records the last export or preview left out

          The models and noise records that would have been exported but
          were left out -- with a run that was left out, for a record they
          need that is not in the bundle, or for a value that cannot be
          imported -- not counting the left-out runs' own records.
      */
      int leftOutRecords( void ) const;

      /*! \brief What the last export or preview left out, for a person

          Empty when there were no warnings.  Otherwise the runs left out,
          by run ID, the number of other records left out, and the
          warnings, one per line -- at most \a maxWarnings of them, with
          the number of those that are not shown.
      */
      QString warningReport( int maxWarnings = 20 ) const;

      //! \brief Remove the staging directory of the last export
      void cleanup( void );

      /*! \brief Ask the export or preview that is running to stop

          It stops at the next record, at the next run while the runs are
          read, between two files of a local store the catalog reads, and,
          from a database, before the catalog reads the records of a run,
          its experiment information or the noise of a model (what one of
          those reads has started -- a few queries -- is finished first).
          It fails with an error that says it was cancelled; no bundle is
          written and the staging directory is removed.  It is meant to be
          called from a slot connected to \ref message(), \ref steps() or
          \ref stepDone(), or from another thread.  A cancel made while no
          export or preview runs -- just before one starts, say -- stops the
          next one before it reads anything; once an export or preview has
          ended, a cancel made before or during it no longer counts, and
          the next one starts afresh.
      */
      void cancel( void );

      //! \brief True while an export or preview runs and has been asked to
      //!        stop, and afterwards when the last one was cancelled
      bool wasCancelled( void ) const;

   signals:
      //! \brief A human readable note about the export's progress
      //! \param message The note
      void message ( const QString& message );

      /*! \brief The number of steps of the current phase

          First the runs that are read from the source, one step each, then
          the records this export will deal with; the count of the second
          phase starts again from one.
          \param steps The number of steps
      */
      void steps   ( int steps );

      //! \brief The number of steps of the current phase finished so far
      //! \param step The number of steps finished
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
      QStringList        warntext;
      QStringList        left_out_runs;
      int                left_out_records;
      int                step_count;

      //! The scope the current export runs with
      US_DataPub::Scope  exp_scope;

      //! The noise records that were asked for by GUID
      QStringList        explicit_noises;

      //! noise GUID -> every exported model that borrows it (a model with
      //! no noise of its own is run with the noise of its edit)
      QMultiHash< QString, QString > noise_borrowers;

      //! The models that were asked for by GUID
      QStringList        explicit_models;

      //! Whether the runs came with the project rather than by name: a run
      //! that cannot be exported is then left out instead of failing it
      bool               from_project;

      //! Whether the last failure is the run's own -- its records do not
      //! hold together -- rather than the export's (a database or a disk
      //! that fails); only the former lets a project export go on
      bool               run_fault;

      //! Set by cancel(); the catalog reads it too.  It is cleared when an
      //! export or preview ends, not when one starts.
      QAtomicInt         cancel_flag;

      //! Whether the last export or preview that ended was cancelled
      bool               cancelled_run;

      //! \brief True, with \a error filled in, once cancel() was called
      bool cancelledError( QString& error ) const;

      /*! The end of an export or a preview: what it returns, with \a error
          saying that it was cancelled when it failed after a cancel; the
          request is cleared, so that the next one starts afresh
      */
      bool endRun( bool ok, QString& error );

      //! exportBundle() and previewManifest() without \ref endRun()
      bool exportRun ( const Selection&, const QString& bundlePath,
                       QString& error );
      bool previewRun( const Selection&, QString& error );

      /*! On disk, the runs of \a all whose experiment XML is missing or
          cannot be read: which project they belong to cannot be told, so
          a project export does not take them.  Each is named in a warning.
      */
      void warnRunsWithoutProject( const Selection&,
                                   const QList< US_DataPubCatalog::Run >& all );

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

      //! Collect and stage the records into a new staging directory
      bool build( const Selection&, QString& error );

      /*! \ref build(), then the manifest, its digest and the check of the
          staged tree: everything of an export but the packing
      */
      bool stageBundle( const Selection&, QString& error );

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

      /*! Whether every number of an XML payload is finite: false with
          the first one that is not in \a what, or with \a what empty when
          the file cannot be read
      */
      static bool numbersAreFinite( const QString& path, QString& what );

      //! Whether an edit payload carries its GUID and its raw data's
      static bool checkEditGuids( const QString& path, const QString& editGUID,
                                  const QString& rawGUID, QString& error );
      bool addRawData   ( const US_DataPubCatalog::Run&,
                          const US_DataPubCatalog::ExpInfo&, QString& );
      bool addTimeState ( const US_DataPubCatalog::Run&, QString& );
      bool addCalibration( const US_DataPubCatalog::ExpInfo&, QString& );

      //! \brief When a database calibration was last updated, as a UTC
      //!        ISO time stamp in \a instant; empty where that cannot be
      //!        told.  Read with the session on UTC, its zone put back
      //!        afterwards; false, with \a error, when it cannot be.
      bool calibrationInstant( int calID, QString& instant, QString& error );

      //! The calibration of the local rotor directory with this GUID
      static bool findDiskCalibration( const QString& guid,
                                       US_Rotor::RotorCalibration& );

      //! Name the rotor calibration's GUID in a staged experiment XML
      bool tagCalibration( const QString& path, const QString& calGUID,
                           QString& error );
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
      //! The variance and meniscus columns of a model's database row
      bool modelRowValues( const US_DataPubCatalog::Model&, double& variance,
                           double& meniscus, QString& error );
      //! Columns of the row a database query returns; none when it finds
      //! no row, false when it fails
      bool rowColumns( const QStringList& query, const QList< int >& columns,
                       QList< QVariant >& values, QString& error );

      //! Put the variance and meniscus into a staged model's XML at full
      //! precision
      bool stageModelValues( const QString& path, double variance,
                             double meniscus, QString& error );
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
      //! A note that is also one of the \ref warnings()
      void warn        ( const QString& );
      //! Leave a run out of the bundle, with a warning
      void leaveOutRun ( const QString& runID, const QString& why );
};
#endif
