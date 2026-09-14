//! \file us_data_catalog.h
#ifndef US_DATA_CATALOG_H
#define US_DATA_CATALOG_H

#include <QtCore>

#include "us_extern.h"
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
    tree fill in rather than waiting for all of it.

    \section catalog_priority Priority

    \ref requestRun moves an experiment to the front of the queue.  When a
    user opens an experiment the program has not reached yet, that
    experiment is what gets read next.

    \section catalog_sources Sources

    One catalog reads from one source.  A program that compares the
    database against the local disk -- which is what us_manage_data does --
    holds one catalog of each and matches them up by GUID; the catalog
    itself takes no view on what it means for a record to be in one and not
    the other.
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
            QString editGUID;     //!< GUID of the edit its model was fitted to
            QString filename;     //!< Base name of the local file, if any
            QString path;         //!< Full path of the local file, if any
            QString checksum;     //!< MD5 of the record contents
            QString size;         //!< Length of the record contents
            QString lastUpdated;  //!< Last-updated date-time, UTC
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
            QString lastUpdated;  //!< Last-updated date-time, UTC
            QString subType;      //!< Analysis type, from the description

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
            QString path;         //!< Full path of the edit XML file
            QString triple;       //!< cell.channel.wavelength
            QString runID;        //!< The run this edit belongs to
            QString rawGUID;      //!< GUID of the raw-data triple
            QString label;        //!< A label for lists and trees
            QString checksum;     //!< MD5 of the edit contents
            QString size;         //!< Length of the edit contents
            QString lastUpdated;  //!< Last-updated date-time, UTC

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
            QString path;         //!< Full path of the .auc file
            QString triple;       //!< cell.channel.wavelength
            QString dataType;     //!< RA, RI, IP, FI, WA or WI
            QString runID;        //!< The run this triple belongs to
            QString description;  //!< Description from the .auc header
            QString solutionID;   //!< solutionID, when read from the database
            QString solutionGUID; //!< solutionGUID of the triple's channel
            QString checksum;     //!< MD5 of the .auc contents
            QString size;         //!< Length of the .auc contents
            QString lastUpdated;  //!< Last-updated date-time, UTC

            QList< Edit > edits;  //!< Edit profiles of this triple
      };

      //! \brief One experiment, the top of the chain
      class Run
      {
         public:
            Run();

            QString id;           //!< experimentID; "-1" when read from disk
            QString guid;         //!< experimentGUID
            QString runID;        //!< The run identifier, the run's name
            QString label;        //!< The experiment label
            QString expType;      //!< velocity, equilibrium, ...
            QString runType;      //!< RA, RI, IP, FI, WA or WI
            QString date;         //!< Last-updated date-time, UTC
            QString projectID;    //!< projectID of the owning project
            QString projectGUID;  //!< projectGUID of the owning project
            QString projectDesc;  //!< Description of the owning project
            QString dirPath;      //!< The run directory, when read from disk

            //! How many records hang off this experiment.  A count of -1
            //! means the source could not say without reading the chain,
            //! which is what \ref loadRunDetail does.
            int     rawCount;     //!< Number of raw-data triples
            int     editCount;    //!< Number of edit profiles
            int     modelCount;   //!< Number of models
            int     noiseCount;   //!< Number of noise records

            Detail  detail;       //!< How much of this experiment was read

            QList< Raw > raws;    //!< Raw-data triples, once loaded

            //! \brief True when the whole chain below this experiment is read
            bool    isLoaded( void ) const;
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
          at a scratch server -- should not open a second connection.  The
          catalog uses the connection but does not take ownership of it.
          \param db    An open database connection
          \param error Filled in with a message when the connection is unusable
          \returns True when the connection is usable
      */
      bool attach( US_DB2* db, QString& error );

      //! \brief Release the source
      void close( void );

      //! \brief True when the catalog has a usable source
      bool isOpen( void ) const;

      //! \brief Where this catalog reads from
      Source source( void ) const;

      //! \brief True when this catalog reads from the database
      bool isDb( void ) const;

      //! \brief The open database connection, or null for a disk catalog
      US_DB2* db( void );

      //! \brief The investigator whose records the catalog reads
      int investigatorID( void ) const;

      //! \brief Read the first layer: one entry per experiment
      //! \param projectGUID When not empty, only experiments of that project
      //! \param error       Filled in with a message when the scan fails
      //! \returns True when the experiments could be listed
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
      //! \param runID The run identifier to look for
      //! \returns -1 when there is no such experiment
      int  indexOfRun( const QString& runID ) const;

      //! \brief Read the second layer of one experiment
      //!
      //! Reading an experiment that is already read does nothing, so this
      //! is safe to call whenever a caller needs an experiment complete.
      //! \param index The position of the experiment
      //! \param error Filled in with a message when the scan fails
      //! \returns True when the experiment is loaded
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
          \param modelGUID The GUID of the model whose noise is wanted
          \param error     Filled in with a message when the lookup fails
      */
      QList< Noise > noisesOfModel( const QString& modelGUID,
                                    QString& error );

      //! \brief Forget everything that was read
      void clear( void );

      /*! \brief Whether to read the database with the per-experiment
                 catalog procedures

          They read a whole experiment in four queries instead of one query
          per record, and the catalog already falls back by itself when a
          server does not have them, so the only reason to turn them off is
          to exercise that fallback.
          \param on True to use them
      */
      void setBulkQueries( bool on );

      //! \brief True when the per-experiment catalog procedures are in use
      bool bulkQueries( void ) const;

      //! \brief The run identifier a file name belongs to
      //!
      //! A .auc file is named runID.type.cell.channel.wavelength.auc and an
      //! edit file runID.editID.type.cell.channel.wavelength.xml, and a run
      //! identifier may itself contain dots, so the run is what is left
      //! after the fixed trailing parts are removed.
      //! \param filename  The base name of the file
      //! \param isEditFile True for an edit XML, false for a .auc file
      static QString runIdOfFile( const QString& filename, bool isEditFile );

   signals:
      //! \brief The first layer has been read
      //! \param count The number of experiments listed
      void runsListed  ( int count );

      //! \brief The second layer of one experiment has been read
      //! \param index The position of the experiment
      void runLoaded   ( int index );

      //! \brief A human readable note about what the catalog is doing
      //! \param message The note
      void message     ( const QString& message );

   private:
      class DiskIndex;

      Source       src;
      US_DB2*      dbase;
      int          inv_id;
      QList< Run > run_list;
      QList< int > pending;
      DiskIndex*   index;
      bool         owns_db;
      QString      proj_guid;

      //! False once the database turns out not to have the per-experiment
      //! catalog procedures, so an older server still works -- one record
      //! at a time, the way it always did
      bool         bulk_ok;

      bool loadRunsDb      ( QString& );
      bool loadRunsDbBulk  ( QString& );
      bool loadRunsDbLegacy( QString& );
      bool loadRunsDisk    ( QString& );
      bool loadDetailDb      ( Run&, QString& );
      bool loadDetailDbBulk  ( Run&, QString& );
      bool loadDetailDbLegacy( Run&, QString& );
      bool loadDetailDisk    ( Run&, QString& );

      //! \brief True when the last query failed because the procedure is
      //!        not installed on this server
      bool missingProcedure( void ) const;

      void buildDiskIndex( void );
};
#endif
