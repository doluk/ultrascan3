//! \file us_datapub_import.h
#ifndef US_DATAPUB_IMPORT_H
#define US_DATAPUB_IMPORT_H

#include <QtCore>

#include "us_datapub_defs.h"
#include "us_datapub_manifest.h"
#include "us_datapub_bundle.h"
#include "us_db2.h"
#include "us_experiment.h"

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
          \param newName   The suggested new name; may be changed
          \returns The policy to apply to this one conflict.  Returning
                   \ref US_DataPub::PolicyReuse when \a identical is false is
                   treated as \ref US_DataPub::PolicyRename, because reusing a
                   record that differs would silently change its meaning.
      */
      virtual US_DataPub::ConflictPolicy resolve(
            US_DataPub::EntityType type, const QString& name,
            bool identical, const QString& details, QString& newName ) = 0;
};

/*! \class US_DataPubImporter
    \brief Replays a data-publication bundle into a database or a disk store

    The importer reads the manifest in dependency order and re-creates every
    record it names.  Numeric IDs are never carried over: the database
    allocates its own, and the GUIDs in the bundle are what ties a record to
    its parents.  When a record has to be pointed at an existing record of a
    different GUID, the GUID map records the substitution so that everything
    depending on it follows along.
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

            //! The policy for records whose name is already taken
            US_DataPub::ConflictPolicy policy;

            //! Per-record-type overrides of \ref policy
            QMap< int, US_DataPub::ConflictPolicy > typePolicies;

            bool    dryRun;                //!< Decide everything, write nothing
            bool    verifyHashes;          //!< Check payload digests first
            QString renameSuffix;          //!< Suffix for auto-renamed records

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

      /*! \brief Unpack a bundle and read its manifest
          \param bundlePath The full path of the .tar.gz to read
          \param error      Filled in with a message when the bundle is bad
          \returns True when the bundle was unpacked and its manifest parsed
      */
      bool inspect( const QString& bundlePath, QString& error );

      //! \brief The manifest of the inspected bundle
      const US_DataPubManifest& manifest( void ) const;

      //! \brief The directory the inspected bundle was unpacked into
      QString bundleRoot( void ) const;

      /*! \brief Check every payload against the digest in the manifest
          \returns One message per missing or altered payload; empty when
                   the bundle is intact
      */
      QStringList verifyPayloads( void ) const;

      //! \brief Use this resolver instead of the conflict policy
      //! \param resolver The resolver to ask, or null to use the policy
      void setResolver( US_DataPubConflictResolver* resolver );

      //! \brief Replay the inspected bundle into the target
      //! \param options How the import should behave
      //! \param error   Filled in with a message when the import fails
      //! \returns True when every record was imported
      bool runImport( const Options& options, QString& error );

      //! \brief What became of each record of the last import
      QList< Result > results( void ) const;

      //! \brief The messages of the last import
      QStringList log( void ) const;

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
      US_DB2*                    dbase;
      QStringList                logtext;
      QList< Result >            reslist;
      int                        step_count;

      // bundle GUID -> target GUID / ID / name
      QMap< QString, QString >   guidMap;
      QMap< QString, QString >   idMap;
      QMap< QString, QString >   nameMap;

      bool    openTarget ( QString& error );
      void    closeTarget( void );
      void    note       ( const QString& );

      QString payloadPath( const US_DataPubEntity& ) const;
      QString workCopy   ( const US_DataPubEntity&, QString& error );

      bool    importEntity( const US_DataPubEntity&, QString& error );

      bool    findByGuid( US_DataPub::EntityType, const QString& guid,
                          QString& id, QString& name );
      bool    findByName( US_DataPub::EntityType, const QString& name,
                          QString& guid, QString& id );
      QString targetFingerprint( US_DataPub::EntityType, const QString& guid,
                                 const QString& id );
      QString uniqueName( US_DataPub::EntityType, const QString& name );

      bool    createRecord( const US_DataPubEntity&, const QString& workPath,
                            const QString& name, QString& newID,
                            QString& error );

      bool    createDb    ( const US_DataPubEntity&, const QString& workPath,
                            const QString& name, QString& newID,
                            QString& error );
      bool    createDisk  ( const US_DataPubEntity&, const QString& workPath,
                            const QString& name, QString& newID,
                            QString& error );

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
      bool    resolveHardware( US_Experiment&, const US_DataPubEntity& );

      QString mappedGuid  ( const QString& guid ) const;
      QString mappedId    ( const QString& guid ) const;
      QString mappedName  ( const QString& guid ) const;

      void    record( const US_DataPubEntity&, US_DataPub::Resolution,
                      const QString& targetGUID, const QString& targetName,
                      const QString& targetID, const QString& detail );
};
#endif
