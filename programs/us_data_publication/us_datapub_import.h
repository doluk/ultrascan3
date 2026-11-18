//! \file us_datapub_import.h
#ifndef US_DATAPUB_IMPORT_H
#define US_DATAPUB_IMPORT_H

#include <QtCore>

#include "us_datapub_defs.h"
#include "us_datapub_manifest.h"
#include "us_datapub_bundle.h"
#include "ius_db2.h"
#include "us_experiment.h"
#include "us_convertio.h"
#include "us_project.h"
#include "us_buffer.h"

/*! \class US_DataPubConflictResolver
    \brief Asked what to do when an import runs into a name conflict

    The importer never overwrites anything.  When a record of the same name
    is already in the target it either points at that record or creates a
    renamed copy, and this interface lets a user interface decide which,
    one conflict at a time.  Without a resolver the conflict policy of the
    import options decides.
*/
class US_DataPubConflictResolver
{
   public:
      virtual ~US_DataPubConflictResolver() {}

      /*! \brief Decide what to do about one name conflict
          \param type      The record type of the conflict
          \param name      The name that is already taken in the target
          \param identical True when the two records have the same properties
          \param details   A description of how the records differ
          \param newName   The suggested new name; may be changed.  A run ID
                           has to pass \ref US_DataPubImporter::runIdIsUsable
                           or the import stops.
          \returns The policy to apply to this one conflict.  Returning
                   \ref US_DataPub::PolicyReuse when \a identical is false is
                   treated as \ref US_DataPub::PolicyRename, because reusing a
                   record that differs would silently change its meaning.
      */
      virtual US_DataPub::ConflictPolicy resolve(
            US_DataPub::EntityType type, const QString& name,
            bool identical, const QString& details, QString& newName ) = 0;

      //! \brief True when the last \ref US_DataPub::PolicyFail answer means
      //!        the user cancelled, rather than chose to fail
      virtual bool cancelled( void ) const { return false; }
};

/*! \class US_DataPubImporter
    \brief Replays a data-publication bundle into a database or a disk store

    The importer reads the manifest in dependency order and re-creates every
    record it names.  Numeric IDs are never carried over: the database
    allocates its own, a disk store gets -1 or the ID of the target record
    the import resolved, and the GUIDs in the bundle are what ties a record to
    its parents.  When a record has to be pointed at an existing record of a
    different GUID, the GUID map records the substitution, and every payload
    that names that record is rewritten to name the target's instead.

    Before anything is written the bundle is checked as a whole: every path
    in the manifest has to stay inside the bundle, every run ID has to be a
    usable one, every payload has to parse as its record type and carry the
    GUID the manifest gives it, and every record a solution, model or noise
    record names has to be in the bundle or in the target.

    A database import runs in one transaction on one connection: it is
    committed only when every record is in, and rolled back on any failure.
    It writes nothing outside the bundle's own staging directory.  A disk
    import writes files as it goes; on a failure the files already written
    stay, and \ref outcome lists them.
*/
class US_DataPubImporter : public QObject
{
   Q_OBJECT

   public:
      /*! \class Options
          \brief How an import run should behave
      */
      class Options
      {
         public:
            Options();

            US_DataPub::Target target;     //!< Write to the database or a disk store
            QString dbPassword;            //!< Master password, for database access
            QString outputDir;             //!< Disk store root; empty for the default

            /*! The project in the target the runs should belong to.

                A bundle names the project its runs came from, but the
                receiving installation usually has projects of its own, so
                an import can be told which one to attach the data to.  The
                bundle's own project record is then reused rather than
                created.  Empty means the bundle decides, as before.  In a
                database the project has to belong to the current
                investigator.
            */
            QString projectGUID;

            //! The policy for records whose name is already taken
            US_DataPub::ConflictPolicy policy;

            //! Per-record-type overrides of \ref policy
            QMap< int, US_DataPub::ConflictPolicy > typePolicies;

            bool    dryRun;                //!< Decide everything, write nothing

            /*! Check every payload against its digest first.  A payload
                without a digest fails the check; switching it off is the
                only way to import such a bundle.
            */
            bool    verifyHashes;

            QString renameSuffix;          //!< Suffix for auto-renamed records

            /*! Refuse to stand a different target record in for hardware
                the target does not have (rotor, calibration, centerpiece,
                instrument, operator), instead of substituting it with a
                warning.
            */
            bool    strictHardware;

            //! \brief The policy that applies to one record type
            //! \param type The record type to look up
            US_DataPub::ConflictPolicy policyFor(
                  US_DataPub::EntityType type ) const;
      };

      /*! \class Result
          \brief What became of one record of the bundle
      */
      class Result
      {
         public:
            Result();

            US_DataPub::EntityType type;   //!< The record type
            QString guid;                  //!< The GUID in the bundle
            QString targetGUID;            //!< The GUID in the target
            QString name;                  //!< The name in the bundle
            QString targetName;            //!< The name in the target
            QString targetID;              //!< The ID the target gave it
            US_DataPub::Resolution resolution; //!< What was done
            QString detail;                //!< A note about the decision
      };

      //! \brief Construct an importer
      //! \param parent The parent object, for Qt ownership
      explicit US_DataPubImporter( QObject* parent = nullptr );

      //! \brief Destroy the importer and its unpacked bundle
      ~US_DataPubImporter();

      /*! \brief Unpack a bundle, read its manifest and check its paths
          \param bundlePath The full path of the .tar.gz to read
          \param error      Filled in with a message when the bundle is bad
          \returns True when the bundle was unpacked, its manifest parsed
                   and every path and run ID in it is acceptable
      */
      bool inspect( const QString& bundlePath, QString& error );

      //! \brief The manifest of the inspected bundle
      const US_DataPubManifest& manifest( void ) const;

      //! \brief The directory the inspected bundle was unpacked into
      QString bundleRoot( void ) const;

      /*! \brief Check every payload against the digest in the manifest
          \returns One message per missing, undigested or altered payload,
                   including a time state's field definitions; empty when
                   the bundle is intact
      */
      QStringList verifyPayloads( void ) const;

      //! \brief Use this resolver instead of the conflict policy
      //! \param resolver The resolver to ask, or null to use the policy
      void setResolver( US_DataPubConflictResolver* resolver );

      /*! \brief Import into this database connection instead of opening one

          The connection stays the caller's: the importer neither connects
          nor deletes it, and \ref Options::dbPassword is not used.  The
          import still runs in a transaction of its own on it, so the
          connection must not have one open.
          \param db The connection to use, or null to open one per import
      */
      void setDatabase( IUS_DB2* db );

      //! \brief Replay the inspected bundle into the target
      //! \param options How the import should behave
      //! \param error   Filled in with a message when the import fails
      //! \returns True when every record was imported
      bool runImport( const Options& options, QString& error );

      //! \brief What became of each record of the last import
      QList< Result > results( void ) const;

      //! \brief The messages of the last import
      QStringList log( void ) const;

      //! \brief The substitutions and omissions of the last import
      QStringList warnings( void ) const;

      /*! \brief What the last import left in the target

          After a success, a count of what was written.  After a failure,
          whether anything was written: a database import says that its
          transaction was rolled back (or that the rollback failed), a disk
          import lists the files it had already written.
      */
      QString outcome( void ) const;

      /*! \brief True when a run ID can be used as one

          Letters, digits, the underscore and the hyphen, at most 80 of them:
          the rule US_ConvertIO applies to run directories.  A run ID ends up
          in directory and file names, so anything else is refused.
      */
      static bool runIdIsUsable( const QString& runID );

      //! \brief A human readable description of an import resolution,
      //!        including the ones this importer adds
      static QString resolutionText( US_DataPub::Resolution resolution );

   signals:
      //! \brief A human readable note about the import's progress
      //! \param message The note
      void message ( const QString& message );

      //! \brief The number of records this import will deal with
      //! \param steps The number of records
      void steps   ( int steps );

      //! \brief The number of records dealt with so far
      //! \param step The number of records finished
      void stepDone( int step );

   private:
      US_DataPubManifest         mani;
      US_DataPubBundle           bundle;
      US_DataPubConflictResolver* resolver;
      Options                    opts;
      IUS_DB2*                   dbase;
      IUS_DB2*                   ext_db;     // A connection the caller owns
      bool                       own_db;     // dbase is ours to delete
      bool                       in_txn;     // A transaction is open
      QStringList                logtext;
      QStringList                warnlist;
      QStringList                disk_written;
      QString                    outcome_text;
      QString                    lookup_error;
      QList< Result >            reslist;
      int                        step_count;

      // The project the import was told to attach the runs to
      QString                    tproj_guid;
      QString                    tproj_id;
      QString                    tproj_desc;

      //! \brief A record of the target, found by the name it is known by
      class Named
      {
         public:
            QString id;         //!< The record ID in the target
            QString guid;       //!< The record GUID, once it is known
            bool    resolved;   //!< True when the GUID has been looked up

            Named() { resolved = false; }
      };

      // bundle GUID -> target GUID / ID / name
      QMap< QString, QString >   guidMap;
      QMap< QString, QString >   idMap;
      QMap< QString, QString >   nameMap;

      //! The name indexes read so far, one per record type
      QMap< int, QHash< QString, Named > > held_names;

      //! Database experiments by GUID: GUID -> ( ID, run ID )
      QHash< QString, QPair< QString, QString > > exp_by_guid;
      bool                       exp_indexed;

      //! Bundle experiments this import created, as opposed to reused
      QSet< QString >            created_exps;

      //! Runs staged for a database import: bundle experiment GUID -> run
      QMap< QString, QSharedPointer< US_ConvertIO::DiskRun > > staged_runs;

      //! Raw data a database import wrote: bundle GUID -> rawData ID
      QMap< QString, QString >   raw_written;

      //! The target's buffer components by ID, once read
      QMap< QString, US_BufferComponent > tcomps;
      bool                       tcomps_read;

      bool    targetComponents( QString& error );
      bool    mapComponents   ( const US_DataPubEntity&, const QString& workPath,
                                bool create, QString& error );

      bool    openTarget ( QString& error );
      void    closeTarget( void );

      bool    beginWrite ( QString& error );
      bool    commitWrite( QString& error );
      void    abortWrite ( const QString& error );

      //! \brief Look up the project the import was told to use
      //! \param error Filled in when the target does not have it
      bool    findTargetProject( QString& error );
      void    note       ( const QString& );

      //! \brief Record a substitution or omission; fails when strict
      bool    warn       ( const QString& text, QString& error );

      //! \brief Whether the last database lookup failed outright
      bool    lookupFailed( const QString& what );

      // ---- checking the bundle -------------------------------------------
      bool    checkBundle ( QString& error ) const;
      bool    insideBundle( const QString& relPath, QString& absPath,
                            QString& error ) const;
      bool    preflight   ( QString& error );
      bool    checkPayload( const US_DataPubEntity&, QString& error );
      bool    availableAs ( US_DataPub::EntityType, const QString& guid,
                            QString& error );

      QString payloadPath( const US_DataPubEntity& ) const;
      QString workCopy   ( const US_DataPubEntity&, QString& error );
      bool    rewriteWork( const US_DataPubEntity&, const QString& workPath,
                           QString& error );

      bool    importEntity( const US_DataPubEntity&, QString& error );
      bool    importCenterpiece( const US_DataPubEntity&, QString& error );

      bool    findByGuid( US_DataPub::EntityType, const QString& guid,
                          QString& id, QString& name );
      bool    findByName( US_DataPub::EntityType, const QString& name,
                          QString& guid, QString& id );

      //! \brief Read the database's experiments into the GUID index
      void    indexExperiments( void );

      /*! \brief The names of one record type that the target already holds

         The target has no "is this name taken" query, so the answer comes
         from a listing of every record of the type.  A rename tries one
         candidate name after another, so reading that listing per lookup
         meant reading the whole buffer, analyte or solution list again for
         every attempt.  It is read once per type per import instead, and
         kept up to date as records are created.
      */
      QHash< QString, Named >& namesHeld( US_DataPub::EntityType );

      //! \brief Read the target's records of one type into a name index
      void    readNames ( US_DataPub::EntityType, QHash< QString, Named >& );

      //! \brief Whether a type is looked up through the name index
      //!
      //! An experiment is not: the target can be asked about one run
      //! identifier directly, which beats listing every experiment.
      static bool indexedByName( US_DataPub::EntityType );

      //! \brief Look one name up in the target, without the index
      bool    findOneByName( US_DataPub::EntityType, const QString& name,
                             QString& guid, QString& id );

      //! \brief The GUID of a record the name index found by its ID
      QString guidOfRecord( US_DataPub::EntityType, const QString& id );

      //! \brief Note that a name is now taken in the target
      void    nameTaken ( US_DataPub::EntityType, const QString& name,
                          const QString& guid, const QString& id );

      QString targetFingerprint( US_DataPub::EntityType, const QString& guid,
                                 const QString& id );
      QString uniqueName( US_DataPub::EntityType, const QString& name );

      //! \brief The name a record goes by in the target before conflicts:
      //!        a model or noise record named after a renamed run follows it
      QString targetName( const US_DataPubEntity& ) const;

      bool    createRecord( const US_DataPubEntity&, const QString& workPath,
                            const QString& name, QString& newID,
                            QString& error );

      //! \brief Put a time state's field definitions beside its work copy
      //! \param entity   The time state being imported
      //! \param workPath The work copy of its binary payload
      //! \param error    Filled in when the definitions cannot be placed
      bool    stageTimeStateDefs( const US_DataPubEntity& entity,
                                  const QString& workPath, QString& error );

      bool    createDb    ( const US_DataPubEntity&, const QString& workPath,
                            const QString& name, QString& newID,
                            QString& error );
      bool    createDisk  ( const US_DataPubEntity&, const QString& workPath,
                            const QString& name, QString& newID,
                            QString& error );

      // ---- database writers ---------------------------------------------
      bool    createProjectDb   ( US_Project&, QString& error );
      bool    createExperimentDb( const US_DataPubEntity&, const QString& workPath,
                                  const QString& name, QString& newID,
                                  QString& error );
      bool    createSolutionDb  ( const US_DataPubEntity&, const QString& workPath,
                                  const QString& name, QString& newID,
                                  QString& error );
      bool    writeRunRawData   ( const QString& expGUID, QString& error );

      QString diskDir      ( US_DataPub::EntityType, bool create = false ) const;
      QString diskResultDir( const QString& runID, bool create = false ) const;

      //! \brief Whether a run-directory payload is already in the disk target
      //! \param entity    The record to look for
      //! \param path      Filled in with where the record would be written
      //! \param identical Set to true when the file there matches the payload
      //! \returns True when a file of that name is already there
      bool    diskFilePresent( const US_DataPubEntity& entity, QString& path,
                               bool& identical );

      QList< US_DataPub::EntityType > importOrder( void ) const;
      QString runIdFor    ( const US_DataPubEntity& ) const;
      QString renamedFile ( const US_DataPubEntity& ) const;
      bool    resolveHardware( US_Experiment&, const US_DataPubEntity&,
                               QString& error );

      QString mappedGuid  ( const QString& guid ) const;
      QString mappedId    ( const QString& guid ) const;
      QString mappedName  ( const QString& guid ) const;

      void    record( const US_DataPubEntity&, US_DataPub::Resolution,
                      const QString& targetGUID, const QString& targetName,
                      const QString& targetID, const QString& detail );
};
#endif
