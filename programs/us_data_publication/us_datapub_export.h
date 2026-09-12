//! \file us_datapub_export.h
#ifndef US_DATAPUB_EXPORT_H
#define US_DATAPUB_EXPORT_H

#include <QtCore>

#include "us_datapub_defs.h"
#include "us_datapub_manifest.h"
#include "us_datapub_bundle.h"
#include "us_datapub_catalog.h"

#include "us_convert.h"

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
            QString     comment;          //!< Free-text bundle comment

            //! \brief Select exactly these raw triples
            void setRaws  ( const QStringList& );
            //! \brief Select exactly these edits
            void setEdits ( const QStringList& );
            //! \brief Select exactly these models
            void setModels( const QStringList& );
            //! \brief Select exactly these noise records
            void setNoises( const QStringList& );

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

      //! \brief Collect the records and write the bundle
      //! \param selection  What to export
      //! \param bundlePath The full path of the .tar.gz to write
      //! \param error      Filled in with a message when the export fails
      //! \returns True when the bundle was written
      bool exportBundle( const Selection& selection,
                         const QString& bundlePath, QString& error );

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

   private:
      US_DataPubManifest mani;
      US_DataPubBundle   bundle;
      US_DataPubCatalog  catalog;
      QStringList        logtext;
      bool               stage_payloads;
      int                step_count;

      //! runID -> GUID of the rotor calibration the run was made with
      QMap< QString, QString > cal_guids;

      //! solutionID -> solutionGUID, for the solutions already collected
      QMap< QString, QString > sol_guids;

      bool build( const Selection&, bool payloads, QString& error );

      bool gatherRuns   ( const Selection&, QList< US_DataPubCatalog::Run >&,
                          QString& );
      bool addProject   ( const Selection&,
                          const QList< US_DataPubCatalog::ExpInfo >&,
                          QString& );
      bool addExperiment( const US_DataPubCatalog::Run&,
                          const US_DataPubCatalog::ExpInfo&, QString& );
      bool addRawData   ( const US_DataPubCatalog::Run&,
                          const US_DataPubCatalog::ExpInfo&, QString& );
      bool addTimeState ( const US_DataPubCatalog::Run&, QString& );
      bool addCalibration( const US_DataPubCatalog::ExpInfo&, QString& );
      bool addCenterpieces( const US_DataPubCatalog::ExpInfo&, QString& );
      bool addSolutions ( const US_DataPubCatalog::Run&,
                          const US_DataPubCatalog::ExpInfo&, QString& );
      bool addEdits     ( const US_DataPubCatalog::Run&, QString& );
      bool addModels    ( const QList< US_DataPubCatalog::Model >&, QString& );
      bool addNoises    ( const QList< US_DataPubCatalog::Noise >&, QString& );

      bool buildTriplesFromDb( const US_DataPubCatalog::Run&,
                               QList< US_Convert::TripleInfo >&, QString& );

      bool finishEntity( US_DataPubEntity&, const QString& stagedPath );
      bool copyPayload ( const QString& source, const QString& staged,
                         QString& error );
      void note        ( const QString& );
};
#endif
