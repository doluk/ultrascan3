//! \file us_datapub_import.h
#ifndef US_DATAPUB_IMPORT_H
#define US_DATAPUB_IMPORT_H

#include <QtCore>
#include <QLockFile>
#include <functional>

#include "us_datapub_defs.h"
#include "us_datapub_manifest.h"
#include "us_datapub_bundle.h"
#include "ius_db2.h"
#include "us_experiment.h"
#include "us_convertio.h"
#include "us_project.h"
#include "us_buffer.h"
#include "us_analyte.h"
#include "us_rotor.h"
#include "us_hardware.h"

/*! \class US_DataPubImportState
    \brief What the import tab lets the user do, apart from the widgets

    The tab's controls follow a little state: the target (database or disk),
    whether a bundle has been inspected, whether an import or an inspection
    is running, and the project the runs are to be attached to.  It is kept
    here, without a widget in sight, so that the rules can be tested: a
    project is chosen from one target's list and means nothing in the
    other, so switching the target clears it -- but nothing else does, and
    in particular not the end of an import or an inspection.
*/
class US_DataPubImportState
{
   public:
      //! \brief A state for the given target, idle, with nothing inspected
      //! \param db True for the database, false for a disk store
      explicit US_DataPubImportState( bool db = true );

      /*! \brief The user picked a target
          \param db True for the database, false for a disk store
          \returns True when this cleared a chosen project, which happens
                   only when the target really changed
      */
      bool    setTarget  ( bool db );

      //! \brief An import or an inspection started (true) or ended (false)
      void    setBusy    ( bool busy );

      //! \brief A bundle was inspected (true) or is no longer (false)
      void    setInspected( bool inspected );

      //! \brief The project the runs are attached to
      void    setProject ( const QString& guid, const QString& desc );

      //! \brief Let the bundle decide the project again
      void    clearProject( void );

      bool    db         ( void ) const;   //!< The database is the target
      bool    busy       ( void ) const;   //!< Something is running
      bool    inspected  ( void ) const;   //!< A bundle has been inspected
      QString projectGUID( void ) const;   //!< The chosen project, or empty
      QString projectDesc( void ) const;   //!< Its description, or empty

      //! \brief The controls that change the bundle, target or options
      bool    controlsEnabled ( void ) const;

      //! \brief The import button
      bool    importEnabled   ( void ) const;

      //! \brief The disk target folder, which a database import has not
      bool    outputDirEnabled( void ) const;

   private:
      bool    is_db;
      bool    is_busy;
      bool    is_inspected;
      QString proj_guid;
      QString proj_desc;
};

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

    In a database, a record the import reuses or attaches anything to has to
    be in the bundle or be the investigator's own: the GUID lookups of
    edits, models and noise are not limited to one investigator (nor, for
    an admin, those of projects, solutions and raw data), so each such
    record is checked against the investigator's listing.

    A database import runs in one transaction on one connection: it is
    committed only when every record is in, and rolled back on any failure.
    Every target lookup that can be made in advance is made before the
    transaction opens (\ref lookUpInAdvance); inside it run only the writes,
    the queries the library writers make themselves, lookups of what the
    import has just written, the name lookups of model and noise records of
    a run it renamed, and a last check that each new run ID is still free.
    The import writes the records with the procedures the library writers
    use, but never the copy in the local disk store some of those writers
    make, so it writes nothing outside the bundle's own staging directory.  A disk import
    writes files as it goes; on a failure the files already written stay,
    and \ref outcome lists them.  A disk import never changes the
    installation's buffer component table: a buffer's components have to be
    in it already.  A database import adds a component its table lacks, and
    finds the new row again by its definition.
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

            /*! Decide everything, write nothing.  Into a database, a dry
                run makes the decisions the writers make as they go: it
                stages each run and checks its triples, resolves the
                project and the hardware (so --strict-hardware fails it as
                it would fail the import), leaves out a calibration whose
                rotor the target lacks, and checks that every solution, time
                state and triple has what it needs.  Like the import's
                preflight it checks the rules the server enforces on the
                writes (see checkServerRules(): calibrations need an
                administrator, components level 2, analytes and private
                buffers of others are copied, channel 1, an edit for every
                noise record; and, as runs are prepared, the operator's
                permit).  Only what the writes themselves can find -- a lost
                connection, a record someone else writes in the meantime --
                is left.
            */
            bool    dryRun;

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

            /*! The buffer component table a disk import maps components
                to; empty for the installation's own
                etc/bufferComponents.xml, which is where US_Buffer looks a
                component up whatever data directory it reads.  The import
                only reads it: a component it lacks fails the import before
                anything is written.
            */
            QString componentTable;

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


      /*! \brief The first character of a text that a MySQL character set
                 cannot store

          latin1 is MySQL's (Windows-1252), utf8/utf8mb3 and ucs2 hold the
          basic multilingual plane, utf8mb4, utf16 and utf32 everything,
          ascii ASCII; of a set this does not know only ASCII is taken as
          safe.  binary, or no set at all, holds anything.
          \param charset The character set, as information_schema names it
          \param text    The text to check
          \returns The character (one code point), or an empty string when
                   the whole text fits
      */
      static QString unstorableCharacter( const QString& charset,
                                          const QString& text );

      /*! \brief A sequence without the framing US_Analyte's old writer
                 put around and into it, for comparing analytes only

          That writer wrote a sequence as a newline, the sequence in pieces
          of 80 characters each followed by a newline, and a last newline,
          and its reader read all of that back, so a sequence grew with
          each round trip through a file.  Text that is exactly such a
          framing -- however many round trips made it -- loses it; any other
          text, a FASTA header, residue numbers and line breaks of its own
          included, comes back unchanged.  An import stores the sequence as
          the bundle holds it; this serves the fingerprints that decide
          whether a target's analyte is the bundle's.
      */
      static QString unframedSequence( const QString& fileText );

      /*! \brief The order the record types of a bundle are replayed in

          A type comes after every type it depends on in the target.  The
          dependencies are those of the target -- in a database a solution
          is created with the ID of its run, so the run goes first; in a disk
          store the run's experiment XML names its solutions, so they go
          first -- together with every dependency the manifest's records
          name, apart from a solution's on its run in a disk store.
          \param manifest The manifest of the bundle
          \param target   Where the bundle is imported to
          \param error    Filled in when the dependencies form a cycle
          \returns Every record type, dependencies first; empty on a cycle
      */
      static QList< US_DataPub::EntityType > importOrder(
            const US_DataPubManifest& manifest, US_DataPub::Target target,
            QString& error );

      //! \brief A model or noise record of an edit, in the order an
      //!        import writes them (writeOrder())
      class OrderStep
      {
         public:
            US_DataPubEntity entity;     //!< The model or noise record
            QString          editGUID;   //!< The edit it belongs to
            QString          noiseType;  //!< "ti" or "ri" for noise
            bool             isNoise;    //!< A noise record, not a model
            //! A model without noise of its own in the bundle: a catalog
            //! tells which noise of its edit it was run with by time
            //! stamps alone (US_DataPubCatalog::noiseBefore())
            bool             borrows;

            OrderStep() { isNoise = false;  borrows = false; }
      };

      /*! \brief The order the models and noise records of a bundle are
                 written in, edit by edit

          The source's: the analysis programs load the noise of the
          latest model with noise of an edit (US_LoadableNoise::
          count_noise(): the highest model ID in a database, the last
          M*.xml file on disk), and a catalog tells which noise a model
          without its own borrows by time stamps
          (US_DataPubCatalog::noiseBefore()); written in the source's order
          -- with the target's clock moved on where two records have to be
          dated apart -- a target keeps both.

          - The models of an edit that have noise keep the source's order
            among themselves -- the source's ID or file number
            (sourceIndex; else the manifest's modelID of a database export
            or the number of a disk export's file name), else the
            manifest's order, which a database export lists newest first.
            That order is all count_noise() reads.
          - The noise records of each type keep the source's time order
            (sourceLastUpdated; a tie to the second goes to the higher
            database ID, on disk to the higher GUID, as noiseBefore()
            breaks it), else the source's order like the models.
          - A noise record comes after the model it was fitted to.
          - A model without noise of its own is in no chain: it comes after
            the record it borrows of each type (borrowedByModelGUIDs, or an
            older bundle's borrowedByModelGUID) and before the next record
            of that type -- before the first, for a type it borrows none
            of.
          - Within that, the source's time stamps decide, a noise record
            going first on a tie; a bundle without them puts each noise
            record as early as it can.

          \param manifest The manifest of the bundle
          \param problems Filled in with what the bundle's borrowing cannot
                          keep: a borrower named in another edit, a model
                          named for two records of a type, or a model
                          whose borrowing contradicts the source's order
                          and the rest of the borrowing (only that model's
                          borrowing is then left out of the order)
          \returns The steps of each edit, in the order of the edits'
                   first appearance in the bundle
      */
      static QList< QList< OrderStep > > writeOrder(
            const US_DataPubManifest& manifest, QStringList& problems );

      /*! \brief True when a text is a GUID: 36 characters, hexadecimal
          digits in groups of 8-4-4-4-12 and nothing else, not even a
          trailing newline
      */
      static bool guidIsUsable( const QString& guid );

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
            //! A record this import wrote (or, in a dry run, would write)
            bool    ours;

            Named() { resolved = false;  ours = false; }
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

      //! The component definitions each bundle buffer carries, read in the
      //! preflight: buffer GUID -> component ID -> definition
      QMap< QString, QMap< QString, US_BufferComponent > > comp_defs;

      //! True once the bundle's experiments were looked up in the target
      bool                       exp_resolved;

      //! The narrowest character set of the target's text parameters and
      //! columns: "p:procedure" or "t:table" -> set
      QMap< QString, QString >   target_charsets;

      //! The set each record type's text has to fit, by type
      QMap< int, QString >       type_charsets;

      //! The schema's default, when information_schema said nothing
      QString                    default_charset;

      //! The element path of the XML checkValues() reads
      QStringList                elements;

      //! What the target's run ID lookup said: run ID -> ( ID, GUID ) of the
      //! investigator's run of that ID, both empty when there is none
      QHash< QString, QPair< QString, QString > > run_lookup;

      // ---- what a database target holds, read before the transaction ----

      //! The investigator's own records of one type
      class Owned
      {
         public:
            QSet< QString >          ids;        //!< Their record IDs
            QSet< QString >          guids;      //!< Their GUIDs, lower case
            QHash< QString, QString > edits;     //!< A model's edit ID
      };

      //! The investigator's records, per type, once listed
      QMap< int, Owned >         owned;

      //! GUID lookups made so far: "type:GUID" -> ( ID, name ), the GUIDs
      //! a lookup did not find, and those a permission-checked lookup found
      //! but refused, because the record is someone else's
      QHash< QString, QPair< QString, QString > > guid_found;
      QSet< QString >            guid_missing;
      QSet< QString >            guid_foreign;

      //! The target's labs (with their instruments), the rotors of each lab
      //! and the calibrations of each rotor, read once
      bool                       hw_read;
      QVector< US_Rotor::Lab >   hw_labs;
      QList< QVector< US_Rotor::Rotor > > hw_rotors;
      QMap< int, QVector< US_Rotor::RotorCalibration > > hw_cals;

      //! The target database's centerpieces, once read
      bool                       cp_read;
      QList< US_AbstractCenterpiece > cp_list;

      //! Rotor and person IDs by GUID, as looked up (0: not there)
      QHash< QString, int >      rotor_ids;
      QHash< QString, int >      person_ids;

      //! The rotor calibrations this import created: ID -> rotor ID
      QMap< int, int >           created_cals;

      //! The fingerprints of target records, by "type:GUID"
      QHash< QString, QString >  print_cache;

      //! Runs whose raw data a dry run has checked
      QSet< QString >            raw_checked;

      //! Every component ID the target's component table listed, parsed or
      //! not, and those this import created: what a server that gives a
      //! new component the row count plus one would collide with
      QSet< QString >            tcomp_ids;

      //! The user level of the person logged in to the target database,
      //! once read (-1 before)
      int                        user_level;

      /*! Buffers and analytes of the bundle the target holds under their
          GUID for someone else, where the import may not use them: bundle
          GUID -> the GUID of the copy the import makes instead
      */
      QMap< QString, QString >   fresh_guids;

      //! Rotor calibrations the person logged in may not create (the
      //! procedure needs an administrator), decided in the preflight
      QSet< QString >            cal_refused;

      //! Whether a buffer or analyte of the target may be used, by
      //! "type:GUID", once asked
      QHash< QString, bool >     shared_ok;

      //! Projects a dry run would create from what a run names
      QSet< QString >            implicit_projs;

      //! \brief Read the user level of the person logged in
      bool    readUserLevel( QString& error );

      //! \brief The person logged in is an administrator (level 3)
      bool    isAdmin( void ) const;

      /*! \brief Whether a buffer or analyte the target holds may be named by
                 the investigator's records

          A buffer may when it is the investigator's own or public; an
          analyte when it is the investigator's own (analytePerson), or the
          person logged in is an administrator -- the rule new_solutionAnalyte
          applies.  False with \a error empty when it may not; false with
          \a error set when the target could not be asked.
      */
      bool    sharedUsable( US_DataPub::EntityType, const QString& guid,
                            const QString& id, QString& error );

      /*! \brief Check, before anything is written, the rules the server
                 enforces only when the writes are made

          Which calibrations the person may create (administrators only),
          whether the person may add buffer components (level 2), whether
          the target has the channel row the raw data writers name, and
          whether every noise record will have an edit to be filed under.
      */
      bool    checkServerRules( QString& error );

      //! \brief Add a buffer component to the database and find its row
      bool    createComponentDb( const US_BufferComponent& want,
                                 QString& newID, QString& error );

      /*! \brief Whether the target's create_buffer_component may be the
                 one that gives a new row the ID of the row count plus one

          False when the server says it is not: its catalog version marker
          (SELECT us3_catalog_version()) answers 1 or more, or the
          procedure's definition is readable and counts no rows.  True
          otherwise -- the procedure servers have run for years, or one
          that cannot be told apart.
      */
      bool    componentIdsMayBeCounted( void );

      //! \brief The target's clock in seconds since the epoch: the
      //!        server's (UNIX_TIMESTAMP()) for a database; for a disk
      //!        store the time the file system gives a file written in
      //!        \a dir (the local clock where none can be written)
      qint64  targetSecond( const QString& dir = QString() );

      //! \brief Wait until the target's clock is past \a second (at most
      //!        five seconds), the clock of the file system of \a dir for
      //!        a disk store
      //! \returns False, with \a error filled in, when it does not get there
      bool    waitForClock( qint64 second, const QString& dir,
                            QString& error );

      //! \brief The second the record the import last wrote is dated in:
      //!        its file's for a disk store, the server's clock after the
      //!        write for a database; -1 when nothing was written
      qint64  writtenSecond( const OrderStep& step );

      //! \brief Write the models and noise records of the bundle in
      //!        writeOrder()'s order, the edits taken in turn
      bool    writeModelsAndNoise( const QList< QList< OrderStep > >& order,
                                   std::function< bool(
                                      const US_DataPubEntity& ) > writeOne,
                                   QString& error );

      //! The server did not tell its time: the local clock stands in
      bool                       clock_local;

      //! \brief The target's us3_catalog_version(), asked once: 0 for a
      //!        server without it
      int     catalogVersion( void );

      //! \brief Note something of the source a target whose procedures
      //!        are older than version 2 cannot keep
      void    cannotKeep( const QString& what );

      //! \brief One warning naming everything cannotKeep() was told of
      void    unkeptWarning( void );

      //! \brief The NULL fields a run's manifest entry names ("nullFields":
      //!        centrifugeProtocol, RIProfile)
      void    nullFieldsOf( const US_DataPubEntity& entity, bool& protocol,
                            bool& riProfile ) const;

      //! \brief What cannotKeep() is told of NULL fields of a run
      QString unkeptFields( const QString& run, bool protocol,
                            bool riProfile ) const;

      //! \brief What cannotKeep() is told of a calibration's date
      QString unkeptDate( const QString& name, const QString& guid,
                          const QString& date ) const;

      //! \brief When a calibration of the bundle was last updated, in
      //!        seconds since the epoch, and as a message names it
      //! \returns False when neither the bundle nor the payload says
      bool    calibrationInstant( const US_DataPubEntity& entity,
                                  const US_Rotor::RotorCalibration& cal,
                                  qint64& seconds, QString& shown ) const;

      //! \brief Whether that instant fits the target's TIMESTAMP column;
      //!        a warning says so where it does not
      bool    calibrationDateFits( const QString& name, const QString& guid,
                                   qint64 seconds, const QString& shown );

      //! \brief Give a calibration of the target the instant, in UTC
      //!        whatever the session's time zone (version 2 procedures)
      bool    storeCalibrationDate( int calID, qint64 seconds,
                                    QString& error );

      //! What a target older than version 2 cannot keep, for unkeptWarning()
      QStringList                unkept;

      //! The target's us3_catalog_version(): 0 for a server without it,
      //! -1 before it was asked
      int                        catalog_version;

      //! What the marker answers and lacks, when catalogVersion() took a
      //! version 2 marker without its procedures as version 1
      QString                    catalog_stale;

      /*! \brief The value of a manifest field every raw data record of a run
                 carries, the same in all of them

          False when none carries it, or they do not agree (which is
          noted): the raw data is then written with the run's own.
      */
      bool    rawFieldOfRun( const QString& expGUID, const QString& key,
                             QString& value );

      //! \brief The GUID a bundle record is written under in the target
      QString targetGuidOf( const US_DataPubEntity& ) const;

      //! \brief The note a record the import copies under a GUID of its own
      //!        is reported with; empty for any other
      QString copyNote( const US_DataPubEntity& ) const;

      //! \brief The fingerprint of the record the import would write, in
      //!        the terms \ref targetFingerprint writes the target's in
      QString ourFingerprint( const US_DataPubEntity&, const QString& workPath,
                              QString& error );

      //! \brief The fingerprint of an analyte as it is stored: without the
      //!        line breaks of its file's sequence, and without the 280 nm
      //!        value US_Analyte computes from a protein's sequence when
      //!        that is the value it holds.  Both sides of a comparison.
      QString analyteFingerprint( US_Analyte& analyte, const QString& fileName );

      //! \brief The edit a noise record is filed under; empty when there
      //!        is none.  \a pending is set for an edit this import creates.
      bool    noiseEditOf( const US_DataPubEntity&, const QString& workPath,
                           QString& editID, bool& pending, QString& error );

      //! \brief Report a project the import creates from what a run names,
      //!        which no record of the bundle stands for
      void    recordImplicitProject( const QString& guid, const QString& desc,
                                     const QString& id, const QString& run );

      //! \brief Read the target database's hardware into the caches
      bool    readHardwareDb( void );

      //! \brief The target's rotor of a GUID; false when the lookup failed
      bool    rotorIdOf ( const QString& rotorGUID, int& rotorID );

      //! \brief The target's person of a GUID; false when the lookup failed
      bool    personIdOf( const QString& personGUID, int& personID );

      /*! \brief Make every target lookup that can be made in advance

          Run in the preflight of a database import, so that the transaction
          finds the answers waiting: the GUID of every record of the bundle,
          the investigator's listings and name indexes, the buffer
          components, the hardware, the rotors and operators the runs name,
          and the fingerprints of the records a name conflict compares with.
      */
      bool    lookUpInAdvance( QString& error );

      //! \brief Whether the investigator's own records of a type are checked
      //!        before the import reuses or attaches to one
      static bool ownedType( US_DataPub::EntityType );

      //! \brief List the investigator's records of a type; false on a
      //!        failed lookup
      bool    readOwned ( US_DataPub::EntityType );

      /*! \brief Whether a database record the import found may be used

          The GUID lookups of buffers, analytes, edits, models and noise are
          not limited to one investigator, and an admin's project, solution
          and raw data lookups are not either.  A record of one of the types
          \ref ownedType names has to be the investigator's own to be reused
          or named as a parent.
          \returns True when it is the investigator's; false, with \a error
                   set, when it is someone else's or the listing failed
      */
      bool    checkOwner( US_DataPub::EntityType, const QString& guid,
                          const QString& id, QString& error );

      //! \brief The refusal for a record of another investigator
      QString foreignText( US_DataPub::EntityType, const QString& guid ) const;

      //! \brief Refuse a bundle record that the target holds for someone else
      bool    checkReuse( QString& error );

      //! \brief The GUIDs of the solutions an experiment XML names
      static QStringList solutionsNamed( const QByteArray& xml );

      /*! \brief Stage a run and decide everything about it but the writes

          The experiment XML and raw data are staged and read, every triple
          is checked against the bundle, the RI profile is read, the project
          and the hardware are resolved.  A dry run stops here; a real import
          then writes the run.
          \param projID  Filled in with the project the run belongs to; 0
                         when a project is still to be created from what the
                         run names, which only a real import does
      */
      bool    prepareRunDb( const US_DataPubEntity&, const QString& name,
                            QByteArray& RIxml, int& projID, QString& error );

      //! \brief Map a staged run's solutions and centerpieces to the target's
      bool    prepareRunRawData( const QString& expGUID, QString& error );

      /*! \brief What a dry run into a database checks of one record

          Everything the writer would decide, without writing: a calibration
          whose rotor the target lacks is omitted, a run is staged and its
          hardware resolved, a solution has to have a run of this import, a
          run's triples their solutions.
          \param omitted Set when the record was recorded as omitted
      */
      bool    checkDryDb( const US_DataPubEntity&, const QString& workPath,
                          const QString& name, bool& omitted,
                          QString& error );

      /*! \brief The extinction spectrum a buffer or analyte payload carries

          Only the values the bundle holds: US_Analyte's reader adds the 280
          nm value it computes from a protein's sequence, which is not part
          of the record and is not stored with it.
      */
      bool    payloadSpectrum( const US_DataPubEntity&, const QString& workPath,
                               QMap< double, double >& spectrum,
                               QString& error ) const;

      bool    targetComponents( QString& error );

      //! \brief Read and check the component definitions of a buffer, and on
      //!        a disk target that the installation has every one of them
      bool    checkComponents ( const US_DataPubEntity&,
                                const US_Buffer& buffer, QString& error );

      /*! \brief Look the bundle's experiments up in the target database

          By run ID first, which is one query per run and settles a run the
          investigator already has; then, only for a run that settles
          nothing, in the investigator's whole experiment list.  A run the
          investigator does not have but whose raw data the database holds
          (or refuses to show) belongs to someone else, and is refused.
          Done before the transaction opens, so no lookup runs inside it.
      */
      bool    resolveExperiments( QString& error );

      //! \brief Every rotor calibration of the target database; false, with
      //!        the lookup error set, when any of the listings fails
      bool    readCalibrationsDb( QVector< US_Rotor::RotorCalibration >& );

      //! \brief The target's centerpieces; false, with the lookup error set,
      //!        when they cannot be read.  A database's are read from the
      //!        database alone, never from the local fallback file.
      bool    readCenterpieces( QList< US_AbstractCenterpiece >& );

      //! \brief Record that a lookup failed, whatever the last status says
      void    failLookup( const QString& what );
      bool    mapComponents   ( const US_DataPubEntity&, const QString& workPath,
                                bool create, QString& error );

      bool    openTarget ( QString& error );
      void    closeTarget( void );

      bool    beginWrite ( QString& error );
      bool    commitWrite( QString& error );
      void    abortWrite ( const QString& error );
      static QString commitUnknownPrefix( void );

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

      //! \brief Read the character sets of what the import writes into
      bool    readTargetCharsets( void );

      /*! \brief Refuse a record with a number that is not finite, or, for a
                 database, text the target cannot store

          What is checked follows what reaches the target's text: the
          manifest's fields of every record (also raw data and edits, whose
          label and comment are columns); the attributes and text of an XML
          payload the library writers pass on; the whole text of a payload
          the database stores as it is (a model's and a noise record's XML,
          a time state's field definitions), comments and processing
          instructions included; and nothing of the main payload of an edit,
          raw data or time state, which go into a BLOB.
          \param entity The record
          \param file   The XML payload read, or the main payload's path
          \param data   Its contents; a null array checks the manifest's
                        fields of the record instead
          \param key    The manifest key that names the payload ("payload"
                        for the main one, "definitionsPayload", ...)
          \param error  Names the record, the field, its place and the value
                        or character when the record is refused
      */
      bool    checkValues ( const US_DataPubEntity& entity, const QString& file,
                            const QByteArray& data, const QString& key,
                            QString& error );
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
      //! \brief The record of a name in the target
      //! \param ours Set, when given, to whether the record is one this
      //!             import wrote or (dry run) would write; only records
      //!             of a type indexed by name are told apart
      bool    findByName( US_DataPub::EntityType, const QString& name,
                          QString& guid, QString& id, bool* ours = nullptr );

      //! \brief Whether a name is taken in the target, without asking which
      //!        record has it
      bool    nameInUse ( US_DataPub::EntityType, const QString& name );

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

      //! \brief Note that the name a record the import reuses by its GUID
      //!        goes under is the bundle's record's (see nameTaken())
      void    reusedName( US_DataPub::EntityType, const QString& name,
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
      //!                  -- for a time state, its field definitions too
      //! \param error     Filled in when the payload could not be compared
      //! \returns True when a file of that name is already there
      bool    diskFilePresent( const US_DataPubEntity& entity, QString& path,
                               bool& identical, QString& error );

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
