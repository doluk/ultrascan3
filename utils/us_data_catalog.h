//! \file us_data_catalog.h
#ifndef US_DATA_CATALOG_H
#define US_DATA_CATALOG_H

#include <QtCore>

#include "us_extern.h"
#include "ius_db2.h"
#include "us_db2.h"

/*! \class US_DataCatalog
    \brief A layered view of the experiment record chain, from the database
           or from a local UltraScan3 data directory

    Every UltraScan3 data set is the same chain: an experiment holds raw
    data triples, a triple holds edit profiles, an edit holds models, and a
    model holds noise records.  Programs that browse, publish or
    synchronize data all have to walk that chain, and they should not each
    have their own idea of how to do it.

    \section catalog_layers Layers

    A catalog is read in two layers, because a multi-wavelength experiment
    can hold hundreds of triples and thousands of records below them, and
    reading all of that before showing anything is what makes browsing feel
    slow.

    \ref loadRuns reads the first layer: one entry per experiment, with the
    number of records hanging off it.  That is a single query against the
    database, and on disk it is one pass over the results directory.  It is
    enough to populate the top level of a tree.

    \ref loadRunDetail reads the second layer for one experiment: its
    triples, their edits, and the models and noise records fitted to them.
    A caller drives this one experiment at a time, so the user sees the
    tree fill in rather than waiting for all of it.  \ref loadAll reads the
    second layer of every experiment at once, which is the fast way to read
    a whole store, and is also what finds the \ref orphans.

    Every load builds what it reads aside and publishes it only when it has
    all of it: a load that fails leaves the catalog as it was.

    \section catalog_priority Priority

    \ref requestRun moves an experiment to the front of the queue.  When a
    user opens an experiment the program has not reached yet, that
    experiment is what gets read next.

    \section catalog_checksums Checksums and verification

    A record's checksum and size say whether a copy in one place matches
    the copy in the other, and reading them is the expensive part of a
    scan: the server hashes every data blob, or every local file is read.
    So they are read only when asked for:

    - With \ref setChecksums on (the default) every detail load reads
      them, and the experiment comes back verified.
    - With it off, no detail load reports a raw-data or edit checksum, and
      \ref verifyRun reads them for one experiment when they are wanted.
      Database models and noise records always carry theirs, since the
      server hashes their small XML as it lists them.
    - \ref loadAll against a database with the whole-store procedures
      never reads raw-data and edit checksums, whatever the setting.

    Run::verified is true exactly when the raw-data and edit checksums of
    the experiment's records are filled in.

    A local file whose checksum is wanted and that cannot be read -- gone
    since the store was indexed, say -- fails the read or the verify that
    wanted it, with an error naming the file; it is never given a checksum
    of its own making.

    \section catalog_sources Sources

    One catalog reads from one source.  A program that compares the
    database against the local disk -- which is what us_manage_data does --
    holds one catalog of each and matches them up by GUID; the catalog
    itself takes no view on what it means for a record to be in one and not
    the other.

    \section catalog_procedures Database procedures

    The database is read through three tiers of stored procedures, and a
    server may have any of them -- a partly upgraded server has some and
    not others.  Each tier falls back on its own, the first time one of its
    procedures turns out to be missing, on the procedures UltraScan has
    always had:

    | Tier            | Procedures                                   | Fall-back                                  |
    |-----------------|----------------------------------------------|--------------------------------------------|
    | ListingTier     | get_experiment_summary                       | get_experiment_desc, one info query per experiment |
    | ExperimentTier  | get_{rawData,editedData}_by_experiment, get_{model,noise}_desc_by_experiment | one query per record |
    | StoreTier       | get_{rawData,editedData}_by_person           | all_rawDataIDs, all_editedDataIDs          |

    Every database ID the catalog gives -- a record's own, and its
    parent's -- is the ID, or "-1" when there is none: the procedures give
    NULL or 0 for none, and those are not passed on as "" or "0".  Edit::editID,
    the edit's time stamp, is not an ID.

    \section catalog_noise_counts Where noise is counted

    A noise record is recorded against an edit (noise.editedDataID) and
    belongs to the model it was fitted with (noise.modelID).  The two are
    usually of one experiment, but a global fit's noise is recorded against
    the edit of each data set while its model is fitted to the first, or to
    the special UNASSIGNED edit (editedDataID 1, editGUID all zeros), which
    belongs to no experiment.  The catalog files noise under its model, so:

    - an experiment's \ref Run::noiseCount, once it is loaded, is the number
      of noise records whose model is under one of its edits, wherever the
      noise was recorded;
    - get_experiment_summary counts the same (noise -> model -> edit -> raw
      data -> experiment) in the current catalog procedures, so loading an
      experiment does not change its noise count.  The first version of
      those procedures counted noise by the edit it was recorded against;
      against such a server the count of an experiment holding such noise
      changes when it is loaded;
    - a per-experiment read (\ref loadRunDetail) gets the noise the server
      lists with the experiment.  When its count then differs from the
      listing's -- the first catalog procedures list noise by the edit it
      was recorded against, so noise of a model here recorded elsewhere is
      not among it -- the investigator's noise listing (get_noise_desc) is
      read, once, and kept, and the noise of this experiment's models is
      taken from it, so an experiment read alone has its full count;
    - noise of a model on the UNASSIGNED edit is counted under no
      experiment.  \ref loadAll lists that model in Orphans::models -- its
      edit is not in the store -- with its noise in Model::noises, and
      \ref noisesOfModel finds the noise by the model's GUID.  A
      per-experiment read (\ref loadRunDetail) that lists such noise with
      the edit it was recorded against keeps it aside, under no model.

    A procedure is missing when the server answers ER_SP_DOES_NOT_EXIST
    (1305).  ER_PROCACCESS_DENIED (1370) is treated the same way: the
    server gives it for a procedure the account may not run, and also for
    one that does not exist when the account has no execute right on the
    whole database, and in both cases the older procedures are the ones to
    use.  If those are refused as well, that error is reported.
*/
class US_UTIL_EXTERN US_DataCatalog : public QObject
{
   Q_OBJECT

   public:
      //! \brief Where a catalog reads its records from
      enum Source
      {
         Disk = 0,   //!< A local UltraScan3 data directory
         Db          //!< The UltraScan3 database
      };

      //! \brief How much of an experiment has been read
      enum Detail
      {
         Listed = 0, //!< Only the experiment entry and its record counts
         Loaded      //!< The whole chain below the experiment
      };

      //! \brief The tiers of database procedures; see \ref catalog_procedures
      enum Tier
      {
         ListingTier    = 0x1,  //!< get_experiment_summary
         ExperimentTier = 0x2,  //!< the *_by_experiment procedures
         StoreTier      = 0x4,  //!< the *_by_person procedures
         AllTiers       = 0x7
      };

      //! \brief A noise record
      class Noise
      {
         public:
            Noise();

            QString id;           //!< noiseID; "-1" when read from disk
            QString guid;         //!< noiseGUID
            QString description;  //!< Noise description
            QString noiseType;    //!< "ti" or "ri"
            QString modelGUID;    //!< GUID of the model it belongs to
            //! modelID of the model it belongs to, when read from the
            //! database; "-1" on disk.  It is what the server joins the
            //! noise to its model by, so the catalog attaches database
            //! noise by it first, and by modelGUID only when it is not set.
            QString modelID;
            QString editGUID;     //!< GUID of the edit its model was fitted
                                  //!< to, when that edit is known
            QString filename;     //!< Base name of the local file, if any
            QString path;         //!< Full path of the local file, if any
            QString checksum;     //!< MD5 of the record contents
            QString size;         //!< Length of the record contents
            QString lastUpdated;  //!< Last-updated date-time, as
                                  //!< "yyyy-MM-dd hh:mm:ss UTC"
      };

      //! \brief A model fitted to one edit profile
      class Model
      {
         public:
            Model();

            QString id;           //!< modelID; "-1" when read from disk
            QString guid;         //!< modelGUID
            QString description;  //!< Model description
            QString editGUID;     //!< GUID of the edit it was fitted to
            QString editID;       //!< editedDataID, when read from the database
            QString filename;     //!< Base name of the local file, if any
            QString path;         //!< Full path of the local file, if any
            QString checksum;     //!< MD5 of the model XML
            QString size;         //!< Length of the model XML
            QString lastUpdated;  //!< Last-updated date-time, as
                                  //!< "yyyy-MM-dd hh:mm:ss UTC"
            //! The analysis type the description names, as
            //! \ref modelSubType gives it: "2DSA" for
            //! "run.1A280.e1234_a5678_2DSA_i01.model", empty for a
            //! description not in that form.  us_manage_data shows more
            //! (CUSTOMGRID, MANUAL), which it works out from this and the
            //! model's size.
            QString subType;

            QList< Noise > noises; //!< Noise records of this model
      };

      //! \brief One edit profile of one raw-data triple
      class Edit
      {
         public:
            Edit();

            QString id;           //!< editedDataID; "-1" when read from disk
            QString guid;         //!< editGUID
            QString editID;       //!< The edit time stamp part of the name
            QString filename;     //!< Base name of the edit XML file
            QString path;         //!< Full path of the edit XML file, on disk
            QString triple;       //!< cell.channel.wavelength
            QString runID;        //!< The run this edit belongs to
            QString rawGUID;      //!< GUID of the raw-data triple
            QString rawID;        //!< rawDataID, when read from the database
            QString label;        //!< A label for lists and trees
            QString checksum;     //!< MD5 of the edit contents
            QString size;         //!< Length of the edit contents
            QString lastUpdated;  //!< Last-updated date-time, as
                                  //!< "yyyy-MM-dd hh:mm:ss UTC"

            QList< Model > models; //!< Models fitted to this edit
      };

      //! \brief One raw-data triple of an experiment
      class Raw
      {
         public:
            Raw();

            QString id;           //!< rawDataID; "-1" when read from disk
            QString guid;         //!< rawDataGUID
            QString filename;     //!< Base name of the .auc file
            QString path;         //!< Full path of the .auc file, on disk
            QString triple;       //!< cell.channel.wavelength
            QString dataType;     //!< RA, RI, IP, FI, WA or WI
            QString runID;        //!< The run this triple belongs to
            QString experimentID; //!< experimentID, when read from the database

            //! A free-text note, which is not the same note in the two
            //! sources: on the database it is the rawData comment, which an
            //! upload fills with the experiment's comment; on disk it is the
            //! description in the .auc header.  Do not compare the two.
            QString description;
            QString solutionID;   //!< solutionID, when read from the database
            QString checksum;     //!< MD5 of the .auc contents
            QString size;         //!< Length of the .auc contents
            QString lastUpdated;  //!< Last-updated date-time, as
                                  //!< "yyyy-MM-dd hh:mm:ss UTC"

            QList< Edit > edits;  //!< Edit profiles of this triple
      };

      //! \brief One experiment, the top of the chain
      class Run
      {
         public:
            Run();

            //! experimentID.  On disk, the ID the experiment XML recorded
            //! when the run was last saved from the database, or "-1".
            QString id;
            QString guid;         //!< experimentGUID
            QString runID;        //!< The run identifier, the run's name; on
                                  //!< disk, as the .auc file names give it
            QString label;        //!< The experiment label
            QString expType;      //!< velocity, equilibrium, ...
            QString runType;      //!< RA, RI, IP, FI, WA or WI
            QString date;         //!< Last-updated date-time, as
                                  //!< "yyyy-MM-dd hh:mm:ss UTC"
            QString projectID;    //!< projectID of the owning project
            QString projectGUID;  //!< projectGUID of the owning project
            QString projectDesc;  //!< Description of the owning project
            QString dirPath;      //!< The run directory, when read from disk

            //! False when this run should not be acted on as it stands: its
            //! run ID is not one UltraScan accepts any more (letters,
            //! digits, '_' and '-', at most 80 characters -- dotted IDs
            //! exist in old data and are still listed), or, on disk, its
            //! directory is not named after it.  Of the directories that
            //! hold one run ID, the one named after it is the run and stays
            //! usable; the others are copies of it (\ref copyOf) and are
            //! not.  When none is named after the run ID, none is usable.
            //! Where file names compare without case (\ref fileNameCase),
            //! so do run IDs here: "RunA" in a directory "Copy of RunA" is
            //! a copy of run "runA" in directory "runA".  A directory that
            //! holds the files of more than one run is the run it is named
            //! after when it holds that run, and is not usable.
            bool    usable;
            //! What is wrong with the run, when \ref usable is false
            QString problem;
            //! On disk, for a directory that holds a copy of a run whose own
            //! directory (named after the run ID) is there too: the path of
            //! that directory.  Such a copy is never the run: it is not
            //! \ref usable, \ref indexOfRun does not return it in place of
            //! the run, and the listing emits a \ref message about it.
            //! Empty otherwise.
            QString copyOf;

            //! How many records hang off this experiment.  A count of -1
            //! means the source could not say without reading the chain,
            //! which is what \ref loadRunDetail does.
            int     rawCount;     //!< Number of raw-data triples
            int     editCount;    //!< Number of edit profiles
            int     modelCount;   //!< Number of models
            int     noiseCount;   //!< Number of noise records

            //! How much of this experiment was read.  Once it is
            //! \ref Loaded the four counts above are what hangs under the
            //! experiment in the chain.  The listing counts the same, noise
            //! by its model's experiment (see \ref catalog_noise_counts),
            //! except on a server with the first catalog procedures, which
            //! count noise by the edit it was recorded against.
            Detail  detail;

            //! True once the raw-data and edit checksums of this
            //! experiment's records are filled in; see
            //! \ref catalog_checksums.
            bool    verified;

            QList< Raw > raws;    //!< Raw-data triples, once loaded

            //! \brief True when the whole chain below this experiment is read
            bool    isLoaded( void ) const;
      };

      /*! \brief The records of a store that do not attach to the chain

          A record attaches to its parent by the parent's GUID.  A database
          record whose parent GUID is empty attaches by the parent's
          database ID instead -- an edit by rawDataID, a model by
          editedDataID -- on every tier, so the whole-store and the
          per-experiment reads agree; database noise attaches by modelID
          first, the column the server itself joins it by.  One whose
          parent is not in the store -- deleted, never uploaded, or written
          with an empty or broken GUID -- is listed here, so that a program
          can still show it and offer to clean it up.  Each list holds the top-most record of a broken branch, and
          that record carries whatever was found below it: a model whose
          edit is missing is in \a models, with its own noise records in
          Model::noises.

          - \a raws:   raw-data triples whose experiment (Raw::experimentID)
                       is not listed.  Database only: on disk a triple is
                       found in its experiment's directory.
          - \a edits:  edit profiles whose triple (Edit::rawGUID) is not in
                       the store.  On disk: edits in a run directory with
                       no .auc file, or with no .auc file of their triple.
          - \a models: models whose edit (Model::editGUID) is not in the
                       store, including those with no editGUID at all.
          - \a noises: noise records whose model (Noise::modelGUID) is not
                       in the store.  Local store only: noise.modelID is
                       NOT NULL in the database and every procedure that
                       lists noise joins modelPerson on it, so a database
                       noise record is only ever listed together with a
                       model of the investigator, which the model listings
                       return too.

          \ref loadAll fills this, since only a whole-store read can tell
          a missing parent from one not yet read, and only when no project
          filter is in effect: a parent outside the filter cannot be told
          apart from a missing one.  It reads the store even when no
          experiment is listed, and then everything in it is here.

          A model of a global fit on the UNASSIGNED edit (Model::editID "1")
          is here too, with its noise: it is not broken, and a program that
          cleans up should not offer to delete it as if it were (see
          \ref catalog_noise_counts).

          A database read of one experiment (\ref loadRunDetail) reads the
          noise the server lists with the experiment.  Noise of a model in
          another experiment is hung under that model, as loadAll hangs
          it -- at once if that experiment is read already, else when it is
          -- so an experiment read alone has the noise and the counts a
          whole-store read gives it.  Noise placed that way that has no
          description of its own is given its model's, as noise read with
          its model is.  \ref loadRuns empties the list.
      */
      class Orphans
      {
         public:
            QList< Raw >   raws;    //!< Triples with no experiment
            QList< Edit >  edits;   //!< Edits with no triple
            QList< Model > models;  //!< Models with no edit
            QList< Noise > noises;  //!< Noise records with no model

            //! \brief True when nothing is orphaned
            bool isEmpty( void ) const;

            //! \brief Forget everything
            void clear( void );
      };

      //! \brief Construct a catalog that is not connected to any source
      //! \param parent The parent object, for Qt ownership
      explicit US_DataCatalog( QObject* parent = nullptr );

      //! \brief Close the source and release the database connection
      ~US_DataCatalog();

      //! \brief Connect the catalog to its source
      //! \param source     Read from the database or from the local disk
      //! \param dbPassword The master password, for database access
      //! \param error      Filled in with a message when opening fails
      //! \returns True when the source is usable
      bool open( Source source, const QString& dbPassword, QString& error );

      /*! \brief Use a database connection the caller already has

          A program that is already connected -- and a test that is pointed
          at a scratch server, or at a mock -- should not open a second
          connection.  The catalog uses the connection but does not take
          ownership of it.
          \param db    An open database connection
          \param error Filled in with a message when there is no connection
                       or it is not connected
          \returns True when the connection is usable
      */
      bool attach( IUS_DB2* db, QString& error );

      //! \brief Release the source
      void close( void );

      //! \brief True when the catalog has a usable source
      bool isOpen( void ) const;

      //! \brief Where this catalog reads from
      Source source( void ) const;

      //! \brief True when this catalog reads from the database
      bool isDb( void ) const;

      //! \brief The database connection, or null for a disk catalog.  It
      //!        is a US_DB2 when the catalog opened it itself.
      IUS_DB2* db( void );

      //! \brief The investigator whose records the catalog reads
      int investigatorID( void ) const;

      //! \brief Pick up the investigator that is selected now
      //!
      //! The investigator can be changed from a dialog that any window may
      //! open, so the one this catalog was built with can be out of date by
      //! the time the next query goes out.  loadRuns() calls this, and a
      //! caller that caches a listing of its own should call it too.
      //!
      //! \returns True when a different person is selected now and that
      //!          changes what the catalog would read -- which is the case
      //!          for the database, whose records belong to one person at a
      //!          time.  The listing read for the previous person is then
      //!          dropped, as by \ref clear, so nothing of theirs is shown
      //!          as the new person's, and \ref listingDropped says so.  The
      //!          local results directory holds the same work whoever is
      //!          selected, so nothing there goes stale and nothing is
      //!          dropped.
      bool refreshInvestigator( void );

      //! \brief Read the first layer: one entry per experiment
      //!
      //! This reads the source afresh: on disk the results directory is
      //! walked again, so runs written since the last listing are found.
      //! \param projectGUID When not empty, only experiments of that project
      //! \param error       Filled in with a message when the scan fails
      //! \returns True when the experiments could be listed; on false the
      //!          previous listing is kept
      bool loadRuns( const QString& projectGUID, QString& error );

      //! \brief Read the first layer for every experiment
      //! \param error Filled in with a message when the scan fails
      bool loadRuns( QString& error );

      //! \brief The experiments listed so far
      const QList< Run >& runs( void ) const;

      //! \brief The number of experiments listed
      int  runCount( void ) const;

      //! \brief One experiment, by position
      //! \param index The position of the experiment
      const Run& run( int index ) const;

      //! \brief The position of an experiment, by run identifier
      //!
      //! The run ID is compared the way the source compares it: without
      //! case on the database, whose runID column does not tell "RunA" from
      //! "runA", and on disk as \ref fileNameCase says.  Of several entries
      //! that match, the run's own directory (not a \ref Run::copyOf) wins,
      //! then a \ref Run::usable one, then the exact spelling, then the
      //! first in the list.
      //! \param runID The run identifier to look for
      //! \returns -1 when there is no such experiment
      int  indexOfRun( const QString& runID ) const;

      /*! \brief Read the whole chain of every experiment at once

          The second layer in bulk.  Against a database with the whole-store
          procedures this is four queries for the whole store rather than
          four per experiment, and none of them hashes a data blob; against
          an older server it is the four whole-store queries UltraScan has
          always had, which do hash them.  On disk it is the one pass
          \ref loadRunDetail would make anyway.

          Experiments already loaded are kept as they are, and gain the
          noise records this read found under their models that they did
          not have yet -- noise recorded against an edit of an experiment
          that was not read then; \ref runChanged says which.  This also
          fills \ref orphans -- and reads the store even when no experiment
          is listed, when everything in it is an orphan.

          It needs a listing: before \ref loadRuns has succeeded (or after
          \ref clear, or a change of investigator) nothing is known to be
          listed, every record would look like an orphan, and the read is
          refused.  The investigator is checked first, as by
          \ref refreshInvestigator, so a change made since the listing is
          caught here: the listing is dropped, \ref listingDropped is
          emitted, and the error says that the investigator changed and the
          experiments have to be listed again.
          \param error Filled in with a message when the scan fails
          \returns True when every experiment now has its records; on false
                   nothing has changed
      */
      bool loadAll( QString& error );

      //! \brief The records the last \ref loadAll found no parent for
      const Orphans& orphans( void ) const;

      /*! \brief Read the checksums of one experiment's records

          This is the expensive half of a scan and the only part that needs
          the record contents, so it is asked for one experiment at a time
          and only where it means something: a record that is in one place
          only has nothing to compare against.  The experiment must be
          loaded.  A database record is matched to its checksum by GUID,
          or by ID when it has no GUID; one the server no longer lists fails
          the verify, which leaves the experiment unverified.
          \param index The position of the experiment
          \param error Filled in with a message when the read fails
      */
      bool verifyRun( int index, QString& error );

      //! \brief True when the checksums of an experiment have been read
      //! \param index The position of the experiment
      bool isRunVerified( int index ) const;

      //! \brief Ask for an experiment to be verified before the others
      //! \param index The position of the experiment
      void requestVerify( int index );

      //! \brief Verify the next experiment that is still waiting
      //! \param index Filled in with the position that was read, or -1
      //! \param error Filled in with a message when the read fails
      //! A verify that fails is put back at the end of the queue once, as
      //! \ref loadNextPending does.
      //! \returns True when an experiment was verified; false when none is
      //!          left or the read failed -- \a error says which
      bool verifyNextPending( int& index, QString& error );

      //! \brief How many experiments are still waiting to be verified
      int  verifyPendingCount( void ) const;

      //! \brief Replace the verify queue
      //! \param indexes The positions of the experiments to verify, in the
      //!        order to verify them; the caller decides what is worth it
      void queueVerify( const QList< int >& indexes );

      /*! \brief Whether reading records also reads their checksums

          On by default, which is what a caller that wants a complete
          picture in one pass expects.  A caller that reads a whole store
          turns it off and asks \ref verifyRun for the experiments it
          actually compares.  See \ref catalog_checksums.
          \param on True to read the checksums as the records are read
      */
      void setChecksums( bool on );

      //! \brief True when reading records also reads their checksums
      bool checksums( void ) const;

      /*! \brief A flag that, once non-zero, stops a read of the local store

          Reading a large local store can take a while, and a program that
          is closing should not have to wait for it.  The catalog checks the
          flag -- which another thread may set -- between files, and a read
          it stops fails with an error that says it was cancelled, leaving
          the catalog as it was.  The flag belongs to the caller and must
          outlive the reads it can stop.  Database reads are not stopped.
          \param flag The flag, or null for none
      */
      void setCancelFlag( const QAtomicInt* flag );

      //! \brief True when the cancel flag is set
      bool cancelled( void ) const;

      //! \brief Read the second layer of one experiment
      //!
      //! Reading an experiment that is already read does nothing, so this
      //! is safe to call whenever a caller needs an experiment complete.
      //! \param index The position of the experiment
      //! \param error Filled in with a message when the scan fails
      //! \returns True when the experiment is loaded; on false it is as it
      //!          was
      bool loadRunDetail( int index, QString& error );

      //! \brief Read the second layer of one experiment, by run identifier
      //! \param runID The run identifier of the experiment
      //! \param error Filled in with a message when the scan fails
      bool loadRunDetail( const QString& runID, QString& error );

      //! \brief True when the whole chain of an experiment has been read
      //! \param index The position of the experiment
      bool isRunLoaded( int index ) const;

      //! \brief Ask for an experiment to be read before the others
      //!
      //! This is what a user interface calls when someone opens an
      //! experiment the background pass has not reached: it moves to the
      //! front of the queue \ref loadNextPending works through.
      //! \param index The position of the experiment
      void requestRun( int index );

      //! \brief Read the next experiment that is still waiting
      //! \param index Filled in with the position that was read, or -1
      //! \param error Filled in with a message when the scan fails
      //! A read that fails puts the experiment back at the end of the
      //! queue, once, since what made it fail may pass; \a error says so.
      //! A second failure drops it from the queue.
      //! \returns True when an experiment was read; false when none is left
      //!          or the scan failed -- \a error says which
      bool loadNextPending( int& index, QString& error );

      //! \brief How many experiments are still waiting to be read
      int  pendingCount( void ) const;

      /*! \brief The noise records of one model, by GUID

          The chain read by \ref loadRunDetail already carries the noise of
          every model under an experiment, and that is where noise normally
          comes from.  This answers for a model that is not in the chain --
          one picked by hand, whose edit was not among the ones read.

          The records carry what \ref noisesOfEdit gives: on the database
          every field the listing has, and Noise::editGUID when the model is
          one the catalog has read; on disk the checksum only when
          \ref checksums is on.  The database listing is read once and kept
          until the catalog is cleared or relisted.  Which model each record
          is of -- by modelID when that model has been read, as the chain
          attaches noise, else by the record's modelGUID -- is worked out
          once for all records and kept, with an index of the models read,
          until models are read again (\ref loadRuns, \ref loadRunDetail,
          \ref loadAll, a verify, \ref clear), so models read since count
          too.  A call then costs the noise of that model, not the catalog.
          \param modelGUID The GUID of the model whose noise is wanted
          \param error     Filled in with a message when the lookup fails
      */
      QList< Noise > noisesOfModel( const QString& modelGUID,
                                    QString& error );

      //! \brief How many times the index of models read, which
      //!        \ref noisesOfModel uses, has been built; for tests
      int modelIndexBuilds( void ) const;

      /*! \brief The noise records of one edit profile

          Noise is fitted to an edit and recorded against the model it was
          fitted with, so the noise of an edit is the noise of all of that
          edit's models.  A model with no noise of its own can still be run
          with the noise of its edit, and this is how a caller finds it.

          The two sources identify an edit differently: the database looks
          it up by its numeric editedDataID, a local store by its GUID, so
          both are taken and whichever one the open source needs is used.
          The records carry \a editGUID as Noise::editGUID.
          \param editGUID The editGUID, for a local store
          \param editID   The editedDataID, for the database
          \param error    Filled in with a message when the lookup fails
      */
      QList< Noise > noisesOfEdit( const QString& editGUID,
                                   const QString& editID, QString& error );

      //! \brief Forget everything that was read: the listing, both queues,
      //!        the orphans, and what was cached of the local store
      void clear( void );

      /*! \brief Which tiers of database procedures may be used

          A tier is used until a procedure of it turns out to be missing,
          and then the catalog falls back by itself; see
          \ref catalog_procedures.  Turning a tier off makes it fall back
          from the start, which is what a test of the fall-back needs.
          \param tiers An OR of \ref Tier values
      */
      void setTiers( int tiers );

      //! \brief The tiers still in use: those allowed and not found missing
      int  tiers( void ) const;

      //! \brief Turn every tier on, or every tier off
      //! \param on True for \ref AllTiers, false for none
      void setBulkQueries( bool on );

      //! \brief True when the per-experiment procedures are in use
      bool bulkQueries( void ) const;

      //! \brief The run identifier a file name belongs to
      //!
      //! A .auc file is named runID.type.cell.channel.wavelength.auc and an
      //! edit file runID.editID.type.cell.channel.wavelength.xml, so the
      //! run is what is left after the fixed trailing parts are removed.
      //! That keeps working for the dotted run IDs old data still has,
      //! although UltraScan no longer accepts them for new runs (see
      //! Run::usable).
      //! \param filename  The base name of the file
      //! \param isEditFile True for an edit XML, false for a .auc file
      static QString runIdOfFile( const QString& filename, bool isEditFile );

      //! \brief True when a run ID is one UltraScan accepts for a new run:
      //!        letters, digits, '_' and '-', at most 80 characters.  It is
      //!        US_ConvertIO::runIdIsUsable(), the rule readDiskRun applies.
      static bool runIdIsUsable( const QString& runID );

      //! \brief The analysis type a model description names: the third
      //!        '_' field of its second-to-last '.' part, as UltraScan has
      //!        always read it ("2DSA" from
      //!        "run.1A280.e1234_a5678_2DSA_i01.model")
      //! \param description The model description
      static QString modelSubType( const QString& description );

      /*! \brief How the local store compares file names

          A run directory is named after its run, and its files are found
          by suffix.  Where the file system does not tell "RunA" from
          "runA" -- Windows, and macOS as installed -- neither does the
          catalog; elsewhere names must match exactly.  The platform
          decides by default; this is for a store on a file system that is
          not like the platform's, and for tests.
          \param sensitivity Qt::CaseInsensitive to compare without case
      */
      void setFileNameCase( Qt::CaseSensitivity sensitivity );

      //! \brief How the local store compares file names
      Qt::CaseSensitivity fileNameCase( void ) const;

      //! \brief A database date-time in the form the catalog uses for every
      //!        date, "yyyy-MM-dd hh:mm:ss UTC" -- the form us_manage_data
      //!        has always shown, and the one local files are given
      //!
      //! The procedures give dates through timestamp2UTC, which converts
      //! them to UTC on the server, as "yyyy-MM-dd hh:mm:ss".  The value is
      //! taken as that UTC date and time as it stands; it is never read as
      //! a local time, which could shift it (a time in the client's
      //! spring-forward gap does not exist locally).
      //! \param value A date-time the server gave, already in UTC
      static QString utcText( const QVariant& value );

   signals:
      //! \brief The first layer has been read
      //! \param count The number of experiments listed
      void runsListed  ( int count );

      //! \brief The second layer of one experiment has been read
      //! \param index The position of the experiment
      void runLoaded   ( int index );

      //! \brief The checksums of one experiment have been read
      //! \param index The position of the experiment
      void runVerified ( int index );

      //! \brief An experiment that was loaded already gained records:
      //!        noise read with another experiment, or by \ref loadAll,
      //!        was hung under one of its models and its counts changed
      //! \param index The position of the experiment
      void runChanged  ( int index );

      //! \brief The listing was dropped because a different investigator
      //!        is selected now (\ref refreshInvestigator, which \ref loadRuns
      //!        and \ref loadAll call): \ref runCount is 0, every experiment
      //!        position given before means nothing, and nothing is listed
      //!        until \ref loadRuns succeeds again.  Not emitted by \ref clear,
      //!        which the caller asks for itself.
      void listingDropped( void );

      //! \brief A human readable note about what the catalog is doing
      //! \param message The note
      void message     ( const QString& message );

   private:
      class DiskIndex;

      Source       src;
      IUS_DB2*     dbase;
      int          inv_id;
      QList< Run > run_list;
      QList< int > pending;
      QList< int > verify_queue;
      DiskIndex*   disk_index;
      bool         owns_db;
      QString      proj_guid;
      Orphans      orphan_list;

      //! True once loadRuns() has succeeded, until the listing is dropped
      bool         listed;

      //! The tiers of procedures the server has not been found to lack,
      //! reset by open() and attach(); see \ref catalog_procedures
      int          tier_mask;
      //! The tiers the caller allows, by setTiers()
      int          allowed_tiers;

      bool         do_checksums;

      //! How file names of the local store compare; see setFileNameCase()
      Qt::CaseSensitivity file_case;

      //! Failed reads and verifies so far, by position; each is retried once
      QHash< int, int > load_tries;
      QHash< int, int > verify_tries;

      //! Set by another thread to stop a read of the local store
      const QAtomicInt* cancel_flag;

      //! \brief True, with \a error filled in, when the read is cancelled
      bool cancelledError( QString& error ) const;

      //! The investigator's noise records, for noisesOfModel, which works
      //! out whose each one is when it is asked
      QList< Noise > noise_cache;
      bool         noise_cached;

      //! What noisesOfModel needs of the models read, built when first
      //! asked for after a change (see modelsChanged())
      struct ModelIndex
      {
         //! As knownModelDescriptions() gives them
         QHash< QString, QString > descs;
         //! The editGUID of each model, by modelGUID
         QHash< QString, QString > editOf;
         //! The positions in noise_cache of each model's noise, by the
         //! model's GUID, once noise_cache is read
         QHash< QString, QList< int > > noiseOf;
         bool                           built      = false;
         bool                           noiseBuilt = false;
      };
      ModelIndex   model_index;
      int          model_index_builds = 0;

      //! \brief Drop the model index: the models read have changed
      void modelsChanged( void );
      //! \brief Drop the noise part of it: noise_cache has changed
      void noiseCacheChanged( void );
      //! \brief The model index, built if it is not
      const ModelIndex& modelIndex( void );

      bool loadAllDb  ( QList< Run >&, Orphans&, QString& );
      bool loadAllDisk( QList< Run >&, Orphans&, QString& );
      bool loadStoreDbNew   ( QList< Raw >&, QList< Edit >&, QString& );
      bool loadStoreDbLegacy( QList< Raw >&, QList< Edit >&, QString& );
      bool verifyRunDb      ( Run&, QString& );
      bool verifyRunDbLegacy( Run&, QString& );
      bool verifyRunDisk    ( Run&, QString& );

      //! \brief The checksum and length of a local file, when wanted
      //! \returns False, with \a error filled in, when the file could not
      //!          be read; true when it was, or checksums are not wanted
      bool fileDigest( const QString& path, QString& checksum, QString& size,
                       QString& error );

      //! \brief Build the local index, and say when it could not be built
      //! \returns False, with \a error filled in, when the build was
      //!          cancelled and the index is not there to be read
      bool diskIndexReady( QString& error );

      //! \brief Recompute an experiment's counts from the chain just read
      //! \param run The experiment to finish
      void finishRun( Run& run );

      //! \brief Clear raw-data and edit checksums when they are not wanted
      void dropChecksums( QList< Raw >& raws );

      bool loadRunsDb      ( QList< Run >&, QString& );
      bool loadRunsDbBulk  ( QList< Run >&, QString& );
      bool loadRunsDbLegacy( QList< Run >&, QString& );
      bool loadRunsDisk    ( QList< Run >&, QString& );
      bool loadDetailDb      ( const Run&, QList< Raw >&, QList< Noise >&,
                               QString& );
      bool loadDetailDbBulk  ( const Run&, QList< Raw >&, QList< Noise >&,
                               QString& );
      bool loadDetailDbLegacy( const Run&, QList< Raw >&, QList< Noise >&,
                               QString& );

      //! \brief Hang noise under the models of one experiment
      //! \param run    The experiment
      //! \param noises The noise to place; what is placed is taken out
      //! \returns True when a record was added to the experiment
      bool placeForeignNoise( Run& run, QList< Noise >& noises );

      //! Noise read with one experiment whose model is in another not yet
      //! read; it is hung under that model when that one is read
      QList< Noise > foreign_noise;

      //! \brief The description of every model the catalog has read, by
      //!        modelGUID and by "id:" plus modelID, and each model's GUID
      //!        by "guid-of:" plus modelID
      QHash< QString, QString > knownModelDescriptions( void ) const;

      //! \brief Read in full the model descriptions the server cut at the
      //!        width of its description column
      void repairModels( QList< Model >& models );

      //! \brief The same for noise, and give noise with no description
      //!        its model's
      //! \param noises     The noise records to repair
      //! \param modelDescs Model descriptions, as knownModelDescriptions()
      //!        gives them
      void repairNoises( QList< Noise >& noises,
                         const QHash< QString, QString >& modelDescs );

      //! \brief repairModels(), then repairNoises() with those models
      void repairDescriptions( QList< Model >& models, QList< Noise >& noises );

      //! The full description read for a record whose listed one may have
      //! been cut, by "model:" or "noise:" plus its ID.  Each record is
      //! read once, whatever that read gave, until the listing is redone.
      QHash< QString, QString > desc_tried;

      //! \brief True when the last query failed because the connection to
      //!        the server is gone, not because of what it asked: the
      //!        connection says so, or its last error does by
      //!        IUS_DB2::isConnectionLoss for its server
      bool connectionLost( void );
      bool loadDetailDisk    ( const Run&, QList< Raw >&, QString& );

      //! \brief Run one query and say whether it may be read
      //! \returns True when the query succeeded or returned no rows; false
      //!          with \a error filled in otherwise
      bool queryOk( const QStringList& query, QString& error );

      //! \brief True when the last query failed because the procedure is
      //!        not there to be used: missing, or not permitted
      bool missingProcedure( void );

      //! \brief Note that a tier turned out to be missing
      void dropTier( int tier, const QString& procedure );

      void buildDiskIndex( void );

      //! \brief Read the investigator's noise records for noisesOfModel
      bool cacheNoise( QString& error );
};
#endif
