//! \file us_data_catalog.cpp
#include "us_data_catalog.h"

#include "us_settings.h"
#include "us_util.h"
#include "us_dataIO.h"

// ---------------------------------------------------------------- helpers

namespace
{
   // Read the attributes of the first element of a given name.
   //
   // Identifying a model, a noise record or an edit only needs what is on
   // its first element.  Parsing the rest of the file -- every component of
   // a model, every value of a noise vector -- is what a store full of
   // those files cannot afford.
   QMap< QString, QString > peek_attributes( const QString& filename,
                                             const QString& element )
   {
      QMap< QString, QString > values;
      QFile                    file( filename );

      if ( ! file.open( QIODevice::ReadOnly | QIODevice::Text ) )
         return values;

      QXmlStreamReader xml( &file );

      while ( ! xml.atEnd() )
      {
         xml.readNext();

         if ( ! xml.isStartElement() )               continue;
         if ( xml.name().toString() != element )     continue;

         QXmlStreamAttributes attrs = xml.attributes();

         for ( int ii = 0; ii < attrs.size(); ii++ )
            values.insert( attrs.at( ii ).name ().toString(),
                           attrs.at( ii ).value().toString() );
         break;
      }

      file.close();

      return values;
   }

   // The GUIDs an edit XML carries, without reading the edit points
   void peek_edit_guids( const QString& filename, QString& editGUID,
                         QString& rawGUID )
   {
      editGUID.clear();
      rawGUID .clear();

      QFile file( filename );

      if ( ! file.open( QIODevice::ReadOnly | QIODevice::Text ) )  return;

      QXmlStreamReader xml( &file );

      while ( ! xml.atEnd() )
      {
         xml.readNext();

         if ( ! xml.isStartElement() )  continue;

         QString name = xml.name().toString();

         if ( name == "editGUID" )
            editGUID = xml.attributes().value( "value" ).toString();

         else if ( name == "rawDataGUID" )
            rawGUID  = xml.attributes().value( "value" ).toString();

         if ( ! editGUID.isEmpty()  &&  ! rawGUID.isEmpty() )  break;
      }

      file.close();
   }

   QString utc_of_file( const QString& path )
   {
      return US_Util::toUTCDatetimeText(
             QFileInfo( path ).lastModified().toUTC().toString( Qt::ISODate ),
             true );
   }
}

// ------------------------------------------------------------- disk index

/*! \class US_DataCatalog::DiskIndex
    \brief What one pass over the local data directory found

    A local store is read once.  The results directory gives one listing per
    run directory, and the model and noise directories give one listing
    each, whose files are peeked for the handful of attributes that say what
    they are.  Everything a later question asks -- which edits belong to
    this triple, which models were fitted to this edit -- is answered from
    this index rather than from the file system again.
*/
class US_DataCatalog::DiskIndex
{
   public:
      DiskIndex() : built( false ) {}

      class RunFiles
      {
         public:
            QString     dirPath;
            QString     runType;
            QString     expXml;      // base name of the experiment XML
            QString     tmstFile;    // base name of the time-state file
            QStringList aucFiles;    // base names
            QStringList editFiles;   // base names
      };

      class ModelFile
      {
         public:
            QString path;
            QString base;
            QString guid;
            QString editGUID;
            QString description;
      };

      class NoiseFile
      {
         public:
            QString path;
            QString base;
            QString guid;
            QString modelGUID;
            QString description;
            QString noiseType;
      };

      bool                          built;
      QMap< QString, RunFiles >     runs;
      QList< ModelFile >            models;
      QList< NoiseFile >            noises;
      QMultiHash< QString, int >    modelsByEdit;   // editGUID  -> models[]
      QMultiHash< QString, int >    noisesByModel;  // modelGUID -> noises[]
};

// ------------------------------------------------------------ the records

US_DataCatalog::Noise::Noise()  { id = "-1"; }
US_DataCatalog::Model::Model()  { id = "-1"; }
US_DataCatalog::Edit::Edit()    { id = "-1"; }
US_DataCatalog::Raw::Raw()      { id = "-1"; solutionID = "-1"; }

US_DataCatalog::Run::Run()
{
   id         = "-1";
   projectID  = "-1";
   rawCount   = -1;
   editCount  = -1;
   modelCount = -1;
   noiseCount = -1;
   detail     = US_DataCatalog::Listed;
   verified   = false;
}

bool US_DataCatalog::Run::isLoaded( void ) const
{
   return ( detail == US_DataCatalog::Loaded );
}

// ----------------------------------------------------------- the catalog

US_DataCatalog::US_DataCatalog( QObject* parent ) : QObject( parent )
{
   src     = US_DataCatalog::Disk;
   dbase   = nullptr;
   inv_id  = US_Settings::us_inv_ID();
   index        = new DiskIndex();
   bulk_ok      = true;
   bulk_all_ok  = true;
   owns_db      = false;
   do_checksums = true;
}

US_DataCatalog::~US_DataCatalog()
{
   close();

   delete index;
   index = nullptr;
}

bool US_DataCatalog::open( Source source, const QString& dbPassword,
                           QString& error )
{
   close();

   src         = source;
   inv_id      = US_Settings::us_inv_ID();
   bulk_ok     = true;
   bulk_all_ok = true;
   owns_db     = false;

   if ( src == US_DataCatalog::Disk )
   {
      QString rdir = US_Settings::resultDir();

      if ( ! QDir( rdir ).exists() )
      {
         error = tr( "The local results directory does not exist: %1" )
                 .arg( rdir );
         return false;
      }

      error.clear();
      return true;
   }

   dbase   = new US_DB2( dbPassword );
   owns_db = true;

   if ( dbase->lastErrno() != US_DB2::OK )
   {
      error = tr( "Cannot connect to the database: %1" )
              .arg( dbase->lastError() );
      delete dbase;
      dbase   = nullptr;
      owns_db = false;
      return false;
   }

   error.clear();

   return true;
}

bool US_DataCatalog::attach( US_DB2* a_db, QString& error )
{
   close();

   if ( a_db == nullptr )
   {
      error = tr( "No database connection was given" );
      return false;
   }

   src         = US_DataCatalog::Db;
   dbase       = a_db;
   owns_db     = false;
   inv_id      = US_Settings::us_inv_ID();
   bulk_ok     = true;
   bulk_all_ok = true;

   error.clear();

   return true;
}

void US_DataCatalog::close( void )
{
   if ( owns_db  &&  dbase != nullptr )  delete dbase;

   dbase   = nullptr;
   owns_db = false;

   clear();
}

void US_DataCatalog::clear( void )
{
   run_list.clear();
   pending .clear();

   if ( index != nullptr )
   {
      index->runs  .clear();
      index->models.clear();
      index->noises.clear();
      index->modelsByEdit .clear();
      index->noisesByModel.clear();
      index->built = false;
   }
}

QList< US_DataCatalog::Noise > US_DataCatalog::noisesOfModel(
      const QString& modelGUID, QString& error )
{
   QList< Noise > list;

   error.clear();

   if ( modelGUID.isEmpty() )  return list;

   if ( ! isDb() )
   {  // The index of the local store already knows which noise is whose
      buildDiskIndex();

      QList< int > noixs = index->noisesByModel.values( modelGUID );
      std::sort( noixs.begin(), noixs.end() );

      for ( int ii = 0; ii < noixs.size(); ii++ )
      {
         const DiskIndex::NoiseFile& nf = index->noises[ noixs[ ii ] ];

         Noise noise;
         noise.guid        = nf.guid;
         noise.modelGUID   = nf.modelGUID;
         noise.description = nf.description;
         noise.noiseType   = nf.noiseType;
         noise.filename    = nf.base;
         noise.path        = nf.path;

         QString contents  = US_Util::md5sum_file( nf.path );
         noise.checksum    = contents.section( " ", 0, 0 );
         noise.size        = contents.section( " ", 1, 1 );
         noise.lastUpdated = utc_of_file( nf.path );

         list << noise;
      }

      return list;
   }

   if ( dbase == nullptr )
   {
      error = tr( "No database connection" );
      return list;
   }

   // The database offers noise lookups by investigator and by edit, but not
   // by model, so the investigator's noise records are listed and filtered
   QStringList query;
   query << "get_noise_desc" << QString::number( inv_id );
   dbase->query( query );

   int status = dbase->lastErrno();

   if ( status != US_DB2::OK  &&  status != US_DB2::NOROWS )
   {
      error = dbase->lastError();
      return list;
   }

   while ( dbase->next() )
   {
      if ( dbase->value( 5 ).toString() != modelGUID )  continue;

      Noise noise;
      noise.id          = dbase->value( 0 ).toString();
      noise.guid        = dbase->value( 1 ).toString();
      noise.noiseType   = dbase->value( 4 ).toString().left( 2 );
      noise.modelGUID   = modelGUID;
      noise.description = dbase->value( 9 ).toString();

      list << noise;
   }

   return list;
}

QList< US_DataCatalog::Noise > US_DataCatalog::noisesOfEdit(
      const QString& editGUID, const QString& editID, QString& error )
{
   QList< Noise > list;

   error.clear();

   if ( ! isDb() )
   {
      if ( editGUID.isEmpty() )  return list;

      buildDiskIndex();

      // The noise of an edit is the noise of every model of that edit
      QList< int > mdlxs = index->modelsByEdit.values( editGUID );
      std::sort( mdlxs.begin(), mdlxs.end() );

      for ( int ii = 0; ii < mdlxs.size(); ii++ )
      {
         const DiskIndex::ModelFile& mf = index->models[ mdlxs[ ii ] ];

         QString message;
         list << noisesOfModel( mf.guid, message );

         if ( ! message.isEmpty() )  error = message;
      }

      return list;
   }

   if ( dbase == nullptr )
   {
      error = tr( "No database connection" );
      return list;
   }

   if ( editID.isEmpty()  ||  editID.toInt() < 1 )  return list;

   QStringList query;
   query << "get_noise_desc_by_editID" << QString::number( inv_id ) << editID;
   dbase->query( query );

   int status = dbase->lastErrno();

   if ( status == US_DB2::NOROWS )  return list;

   if ( status != US_DB2::OK )
   {
      error = dbase->lastError();
      return list;
   }

   while ( dbase->next() )
   {
      Noise noise;
      noise.id          = dbase->value( 0 ).toString();
      noise.guid        = dbase->value( 1 ).toString();
      noise.noiseType   = dbase->value( 4 ).toString().left( 2 );
      noise.modelGUID   = dbase->value( 5 ).toString();
      noise.lastUpdated = dbase->value( 6 ).toString();
      noise.checksum    = dbase->value( 7 ).toString();
      noise.size        = dbase->value( 8 ).toString();
      noise.description = dbase->value( 9 ).toString();

      list << noise;
   }

   return list;
}

void US_DataCatalog::setChecksums( bool on )
{
   do_checksums = on;
}

bool US_DataCatalog::checksums( void ) const
{
   return do_checksums;
}

void US_DataCatalog::fileDigest( const QString& path, QString& checksum,
                                 QString& size )
{
   checksum.clear();
   size    .clear();

   if ( ! do_checksums )  return;

   QString contents = US_Util::md5sum_file( path );
   checksum = contents.section( " ", 0, 0 );
   size     = contents.section( " ", 1, 1 );
}

void US_DataCatalog::setBulkQueries( bool on )
{
   bulk_ok     = on;
   bulk_all_ok = on;
}

bool US_DataCatalog::bulkQueries( void ) const
{
   return bulk_ok;
}

bool US_DataCatalog::isOpen( void ) const
{
   return ( src == US_DataCatalog::Disk )  ||  ( dbase != nullptr );
}

US_DataCatalog::Source US_DataCatalog::source( void ) const
{
   return src;
}

bool US_DataCatalog::isDb( void ) const
{
   return ( src == US_DataCatalog::Db );
}

US_DB2* US_DataCatalog::db( void )
{
   return dbase;
}

int US_DataCatalog::investigatorID( void ) const
{
   return inv_id;
}

const QList< US_DataCatalog::Run >& US_DataCatalog::runs( void ) const
{
   return run_list;
}

int US_DataCatalog::runCount( void ) const
{
   return run_list.size();
}

const US_DataCatalog::Run& US_DataCatalog::run( int index ) const
{
   return run_list.at( index );
}

int US_DataCatalog::indexOfRun( const QString& runID ) const
{
   for ( int ii = 0; ii < run_list.size(); ii++ )
      if ( run_list[ ii ].runID == runID )  return ii;

   return -1;
}

bool US_DataCatalog::isRunLoaded( int index ) const
{
   if ( index < 0  ||  index >= run_list.size() )  return false;

   return run_list[ index ].isLoaded();
}

void US_DataCatalog::requestRun( int index )
{
   if ( index < 0  ||  index >= run_list.size() )  return;
   if ( run_list[ index ].isLoaded() )             return;

   pending.removeAll( index );
   pending.prepend  ( index );
}

int US_DataCatalog::pendingCount( void ) const
{
   return pending.size();
}

bool US_DataCatalog::loadNextPending( int& index, QString& error )
{
   index = -1;

   while ( ! pending.isEmpty() )
   {
      int next = pending.takeFirst();

      if ( next < 0  ||  next >= run_list.size() )  continue;
      if ( run_list[ next ].isLoaded() )            continue;

      index = next;

      return loadRunDetail( next, error );
   }

   error.clear();

   return false;
}

QString US_DataCatalog::runIdOfFile( const QString& filename, bool isEditFile )
{
   // runID.type.cell.channel.wavelength.auc, or
   // runID.editID.type.cell.channel.wavelength.xml
   return isEditFile ? filename.section( ".", 0, -7 )
                     : filename.section( ".", 0, -6 );
}

bool US_DataCatalog::missingProcedure( void ) const
{
   if ( dbase == nullptr )  return false;

   // A missing procedure fails the CALL itself rather than returning a
   // status, so the message is the only thing that says so
   return dbase->lastError().contains( "does not exist", Qt::CaseInsensitive );
}

// ------------------------------------------------------------- first layer

bool US_DataCatalog::loadRuns( QString& error )
{
   return loadRuns( QString(), error );
}

bool US_DataCatalog::loadRuns( const QString& projectGUID, QString& error )
{
   run_list.clear();
   pending .clear();
   proj_guid = projectGUID;

   bool ok = isDb() ? loadRunsDb( error ) : loadRunsDisk( error );

   if ( ! ok )  return false;

   for ( int ii = 0; ii < run_list.size(); ii++ )
      pending << ii;

   emit runsListed( run_list.size() );

   return true;
}

bool US_DataCatalog::loadRunsDb( QString& error )
{
   if ( dbase == nullptr )
   {
      error = tr( "No database connection" );
      return false;
   }

   if ( bulk_ok  &&  loadRunsDbBulk( error ) )  return true;

   if ( ! bulk_ok )
      return loadRunsDbLegacy( error );

   return false;
}

bool US_DataCatalog::loadRunsDbBulk( QString& error )
{
   QStringList query;
   query << "get_experiment_summary" << QString::number( inv_id );
   dbase->query( query );

   if ( missingProcedure() )
   {
      bulk_ok = false;
      emit message( tr( "This database does not have the per-experiment"
                        " catalog procedures; falling back to one query per"
                        " record" ) );
      return false;
   }

   int status = dbase->lastErrno();

   if ( status != US_DB2::OK  &&  status != US_DB2::NOROWS )
   {
      error = dbase->lastError();
      return false;
   }

   while ( dbase->next() )
   {
      Run run;
      run.id          = dbase->value(  0 ).toString();
      run.guid        = dbase->value(  1 ).toString();
      run.runID       = dbase->value(  2 ).toString();
      run.expType     = dbase->value(  3 ).toString();
      run.runType     = dbase->value(  4 ).toString();
      run.label       = dbase->value(  5 ).toString();
      run.projectID   = dbase->value(  6 ).toString();
      run.projectGUID = dbase->value(  7 ).toString();
      run.projectDesc = dbase->value(  8 ).toString();
      run.date        = dbase->value(  9 ).toString();
      run.rawCount    = dbase->value( 10 ).toInt();
      run.editCount   = dbase->value( 11 ).toInt();
      run.modelCount  = dbase->value( 12 ).toInt();
      run.noiseCount  = dbase->value( 13 ).toInt();

      if ( ! proj_guid.isEmpty()  &&  run.projectGUID != proj_guid )  continue;

      run_list << run;
   }

   error.clear();

   return true;
}

bool US_DataCatalog::loadRunsDbLegacy( QString& error )
{
   QString     invID = QString::number( inv_id );
   QStringList query;
   query << "get_experiment_desc" << invID;
   dbase->query( query );

   int status = dbase->lastErrno();

   if ( status != US_DB2::OK  &&  status != US_DB2::NOROWS )
   {
      error = dbase->lastError();
      return false;
   }

   QList< Run > headers;

   while ( dbase->next() )
   {
      Run run;
      run.id      = dbase->value( 0 ).toString();
      run.runID   = dbase->value( 1 ).toString();
      run.expType = dbase->value( 2 ).toString();
      run.runType = dbase->value( 3 ).toString();
      run.label   = dbase->value( 4 ).toString();
      run.date    = dbase->value( 5 ).toString();

      headers << run;
   }

   // The old procedures cannot say what an experiment belongs to or how
   // much is under it without another query per experiment
   QMap< QString, QString > projGUIDs;
   QMap< QString, QString > projDescs;

   for ( int ii = 0; ii < headers.size(); ii++ )
   {
      Run run = headers[ ii ];

      query.clear();
      query << "get_experiment_info_by_runID" << run.runID << invID;
      dbase->query( query );

      if ( dbase->next() )
      {
         run.projectID = dbase->value( 0 ).toString();
         run.id        = dbase->value( 1 ).toString();
         run.guid      = dbase->value( 2 ).toString();
         run.expType   = dbase->value( 8 ).toString();
         run.label     = dbase->value( 10 ).toString();
         run.date      = dbase->value( 13 ).toString();
      }

      if ( ! projGUIDs.contains( run.projectID ) )
      {
         QString pguid;
         QString pdesc;

         query.clear();
         query << "get_project_info" << run.projectID;
         dbase->query( query );

         if ( dbase->next() )
         {
            pguid = dbase->value(  1 ).toString();
            pdesc = dbase->value( 10 ).toString();
         }

         projGUIDs.insert( run.projectID, pguid );
         projDescs.insert( run.projectID, pdesc );
      }

      run.projectGUID = projGUIDs.value( run.projectID );
      run.projectDesc = projDescs.value( run.projectID );

      if ( ! proj_guid.isEmpty()  &&  run.projectGUID != proj_guid )  continue;

      run_list << run;
   }

   error.clear();

   return true;
}

void US_DataCatalog::buildDiskIndex( void )
{
   if ( index->built )  return;

   emit message( tr( "Indexing the local data directory..." ) );

   QString     rdir    = US_Settings::resultDir();
   QStringList subdirs = QDir( rdir ).entryList(
                         QDir::AllDirs | QDir::NoDotAndDotDot, QDir::Name );

   for ( int ii = 0; ii < subdirs.size(); ii++ )
   {
      QString     subdir = rdir + "/" + subdirs[ ii ];
      QStringList files  = QDir( subdir ).entryList( QDir::Files, QDir::Name );

      DiskIndex::RunFiles rf;
      rf.dirPath = subdir;

      QStringList xmls;

      for ( int jj = 0; jj < files.size(); jj++ )
      {
         QString name = files[ jj ];

         if      ( name.endsWith( ".auc"  ) )  rf.aucFiles << name;
         else if ( name.endsWith( ".tmst" ) )  rf.tmstFile =  name;
         else if ( name.endsWith( ".xml"  ) )  xmls        << name;
      }

      if ( rf.aucFiles.isEmpty() )  continue;

      QString runID = runIdOfFile( rf.aucFiles.first(), false );
      rf.runType    = rf.aucFiles.first().section( ".", -5, -5 );

      if ( runID.isEmpty() )  runID = subdirs[ ii ];

      // Now that the run is known, the XML files can be told apart without
      // guessing: the experiment XML is runID.runType.xml and an edit file
      // is runID plus five more parts.
      QString expName = runID + "." + rf.runType + ".xml";
      QString prefix  = runID + ".";

      for ( int jj = 0; jj < xmls.size(); jj++ )
      {
         QString name = xmls[ jj ];

         if ( name == expName )
         {
            rf.expXml = name;
            continue;
         }

         if ( ! name.startsWith( prefix ) )  continue;

         if ( name.mid( prefix.length() ).count( "." ) == 5 )
            rf.editFiles << name;
      }

      index->runs.insert( runID, rf );
   }

   // The model and noise directories, peeked rather than parsed
   QString     mdir  = US_Settings::dataDir() + "/models";
   QStringList mfils = QDir( mdir ).entryList( QStringList( "M???????.xml" ),
                                               QDir::Files, QDir::Name );

   for ( int ii = 0; ii < mfils.size(); ii++ )
   {
      QString path  = mdir + "/" + mfils[ ii ];
      QMap< QString, QString > attrs = peek_attributes( path, "model" );

      if ( attrs.isEmpty() )  continue;

      DiskIndex::ModelFile mf;
      mf.path        = path;
      mf.base        = mfils[ ii ];
      mf.guid        = attrs.value( "modelGUID" );
      mf.editGUID    = attrs.value( "editGUID" );
      mf.description = attrs.value( "description" );

      index->modelsByEdit.insert( mf.editGUID, index->models.size() );
      index->models << mf;
   }

   QString     ndir  = US_Settings::dataDir() + "/noises";
   QStringList nfils = QDir( ndir ).entryList( QStringList( "N???????.xml" ),
                                               QDir::Files, QDir::Name );

   for ( int ii = 0; ii < nfils.size(); ii++ )
   {
      QString path  = ndir + "/" + nfils[ ii ];
      QMap< QString, QString > attrs = peek_attributes( path, "noise" );

      if ( attrs.isEmpty() )  continue;

      DiskIndex::NoiseFile nf;
      nf.path        = path;
      nf.base        = nfils[ ii ];
      nf.guid        = attrs.value( "noiseGUID" );
      nf.modelGUID   = attrs.value( "modelGUID" );
      nf.description = attrs.value( "description" );
      nf.noiseType   = attrs.value( "type" );

      index->noisesByModel.insert( nf.modelGUID, index->noises.size() );
      index->noises << nf;
   }

   index->built = true;

   emit message( tr( "Indexed %1 run(s), %2 model(s), %3 noise record(s)" )
                 .arg( index->runs.size() ).arg( index->models.size() )
                 .arg( index->noises.size() ) );
}

bool US_DataCatalog::loadRunsDisk( QString& error )
{
   buildDiskIndex();

   QList< QString > runIDs = index->runs.keys();

   for ( int ii = 0; ii < runIDs.size(); ii++ )
   {
      QString                    runID = runIDs[ ii ];
      const DiskIndex::RunFiles& rf    = index->runs[ runID ];

      Run run;
      run.runID     = runID;
      run.runType   = rf.runType;
      run.dirPath   = rf.dirPath;
      run.label     = runID;
      run.rawCount  = rf.aucFiles .size();
      run.editCount = rf.editFiles.size();

      if ( ! rf.expXml.isEmpty() )
      {
         QString expPath = rf.dirPath + "/" + rf.expXml;
         QMap< QString, QString > attrs = peek_attributes( expPath,
                                                           "experiment" );
         run.id      = attrs.value( "id", "-1" );
         run.guid    = attrs.value( "guid" );
         run.expType = attrs.value( "type" );

         QMap< QString, QString > proj = peek_attributes( expPath, "project" );
         run.projectID   = proj.value( "id", "-1" );
         run.projectGUID = proj.value( "guid" );
         run.projectDesc = proj.value( "desc" );
      }

      run.date = utc_of_file( rf.dirPath + "/" + rf.aucFiles.first() );

      // Which models and noise records belong to this run can only be said
      // for certain once the edits are read, but their descriptions start
      // with the run identifier, which is enough for a count at this layer.
      QString prefix = runID + ".";
      int     mknt   = 0;
      int     nknt   = 0;

      for ( int jj = 0; jj < index->models.size(); jj++ )
         if ( index->models[ jj ].description.startsWith( prefix ) )  mknt++;

      for ( int jj = 0; jj < index->noises.size(); jj++ )
         if ( index->noises[ jj ].description.startsWith( prefix ) )  nknt++;

      run.modelCount = mknt;
      run.noiseCount = nknt;

      if ( ! proj_guid.isEmpty()  &&  run.projectGUID != proj_guid )  continue;

      run_list << run;
   }

   error.clear();

   return true;
}

// ------------------------------------------------------------ second layer

bool US_DataCatalog::loadRunDetail( const QString& runID, QString& error )
{
   int index = indexOfRun( runID );

   if ( index < 0 )
   {
      error = tr( "No run \"%1\" was listed" ).arg( runID );
      return false;
   }

   return loadRunDetail( index, error );
}

bool US_DataCatalog::loadRunDetail( int index, QString& error )
{
   if ( index < 0  ||  index >= run_list.size() )
   {
      error = tr( "There is no experiment at position %1" ).arg( index );
      return false;
   }

   if ( run_list[ index ].isLoaded() )
   {
      error.clear();
      return true;
   }

   Run& run = run_list[ index ];
   bool ok  = isDb() ? loadDetailDb( run, error ) : loadDetailDisk( run, error );

   if ( ! ok )  return false;

   finishRun( run );
   pending.removeAll( index );

   emit runLoaded( index );

   return true;
}

// Once the chain of an experiment is read the counts are no longer what the
// first layer guessed, or -1 where it could not say: they are what is there.
void US_DataCatalog::finishRun( Run& run )
{
   run.rawCount   = run.raws.size();
   run.editCount  = 0;
   run.modelCount = 0;
   run.noiseCount = 0;

   for ( int ii = 0; ii < run.raws.size(); ii++ )
   {
      const Raw& raw = run.raws[ ii ];

      run.editCount += raw.edits.size();

      for ( int jj = 0; jj < raw.edits.size(); jj++ )
      {
         const Edit& edit = raw.edits[ jj ];

         run.modelCount += edit.models.size();

         for ( int kk = 0; kk < edit.models.size(); kk++ )
            run.noiseCount += edit.models[ kk ].noises.size();
      }
   }

   run.detail = US_DataCatalog::Loaded;
}

// ------------------------------------------------------- the whole store

namespace
{
   // Where a record sits in the tree of runs
   class Slot
   {
      public:
         Slot() : run( -1 ), raw( -1 ), edit( -1 ), model( -1 ) {}

         int run;
         int raw;
         int edit;
         int model;
   };
}

bool US_DataCatalog::loadAll( QString& error )
{
   if ( run_list.isEmpty() )
   {
      error.clear();
      return true;
   }

   if ( isDb()  &&  ! bulk_all_ok )
   {
      error = tr( "This database has no whole-store catalog procedures" );
      return false;
   }

   bool ok = isDb() ? loadAllDb( error ) : loadAllDisk( error );

   if ( ! ok )  return false;

   for ( int ii = 0; ii < run_list.size(); ii++ )
   {
      finishRun( run_list[ ii ] );
      pending.removeAll( ii );

      emit runLoaded( ii );
   }

   error.clear();

   return true;
}

/* Read the whole store in four queries.

   Asking for the chain one experiment at a time is four round trips per
   experiment, and each of those asks the server to hash the experiment's
   data blobs.  The bulk procedures list everything the investigator can
   see without hashing anything, and the records are attached to their
   parents here by GUID -- the same thing the local index does with the
   file system.
*/
bool US_DataCatalog::loadAllDb( QString& error )
{
   if ( dbase == nullptr )
   {
      error = tr( "No database connection" );
      return false;
   }

   QString     invID = QString::number( inv_id );
   QStringList query;

   QHash< QString, int >   runById;      // experimentID -> run
   QHash< QString, Slot >  rawSlot;      // rawDataGUID  -> raw
   QHash< QString, Slot >  editSlot;     // editGUID     -> edit
   QHash< QString, Slot >  modelSlot;    // modelGUID    -> model

   for ( int ii = 0; ii < run_list.size(); ii++ )
   {
      run_list[ ii ].raws.clear();
      runById.insert( run_list[ ii ].id, ii );
   }

   // ---- the triples ------------------------------------------------------
   query << "get_rawData_by_person" << invID;
   dbase->query( query );

   if ( missingProcedure() )
   {
      // Only the whole-store procedures are missing.  The per-experiment
      // ones may well be there, so that is what the caller falls back to,
      // not the record-at-a-time path.
      bulk_all_ok = false;
      emit message( tr( "This database does not have the whole-store catalog"
                        " procedures; falling back to one experiment at a"
                        " time" ) );
      return false;
   }

   int status = dbase->lastErrno();

   if ( status != US_DB2::OK  &&  status != US_DB2::NOROWS )
   {
      error = dbase->lastError();
      return false;
   }

   while ( dbase->next() )
   {
      QString expID = dbase->value( 5 ).toString();

      if ( ! runById.contains( expID ) )  continue;   // not in this listing

      int  rx = runById.value( expID );
      Run& run = run_list[ rx ];

      Raw raw;
      raw.id          = dbase->value(  0 ).toString();
      raw.guid        = dbase->value(  1 ).toString();
      raw.filename    = dbase->value(  3 ).toString().replace( "\\", "/" )
                        .section( "/", -1, -1 );
      raw.description = dbase->value(  4 ).toString();
      raw.solutionID  = dbase->value(  6 ).toString();
      raw.lastUpdated = dbase->value(  8 ).toString();
      raw.checksum    = dbase->value(  9 ).toString();
      raw.size        = dbase->value( 10 ).toString();
      raw.runID       = run.runID;
      raw.dataType    = raw.filename.section( ".", -5, -5 );
      raw.triple      = raw.filename.section( ".", -4, -2 );

      run.raws << raw;

      Slot slot;
      slot.run = rx;
      slot.raw = run.raws.size() - 1;
      rawSlot.insert( raw.guid, slot );
   }

   // ---- the edits, attached to their triple by GUID ----------------------
   query.clear();
   query << "get_editedData_by_person" << invID;
   dbase->query( query );

   status = dbase->lastErrno();

   if ( status != US_DB2::OK  &&  status != US_DB2::NOROWS )
   {
      error = dbase->lastError();
      return false;
   }

   while ( dbase->next() )
   {
      QString rawGUID = dbase->value( 2 ).toString();

      if ( ! rawSlot.contains( rawGUID ) )  continue;

      Slot where = rawSlot.value( rawGUID );
      Raw& raw   = run_list[ where.run ].raws[ where.raw ];

      Edit edit;
      edit.id          = dbase->value( 0 ).toString();
      edit.rawGUID     = rawGUID;
      edit.guid        = dbase->value( 3 ).toString();
      edit.filename    = dbase->value( 5 ).toString().replace( "\\", "/" )
                         .section( "/", -1, -1 );
      edit.lastUpdated = dbase->value( 7 ).toString();
      edit.checksum    = dbase->value( 8 ).toString();
      edit.size        = dbase->value( 9 ).toString();
      edit.runID       = raw.runID;
      edit.editID      = edit.filename.section( ".", -6, -6 );
      edit.triple      = edit.filename.section( ".", -4, -2 );
      edit.label       = edit.editID + " (" + edit.triple + ")";

      raw.edits << edit;

      Slot slot  = where;
      slot.edit  = raw.edits.size() - 1;
      editSlot.insert( edit.guid, slot );
   }

   // ---- the models, attached to their edit by GUID -----------------------
   query.clear();
   query << "get_model_desc" << invID;
   dbase->query( query );

   status = dbase->lastErrno();

   if ( status != US_DB2::OK  &&  status != US_DB2::NOROWS )
   {
      error = dbase->lastError();
      return false;
   }

   while ( dbase->next() )
   {
      QString editGUID = dbase->value( 5 ).toString();

      if ( ! editSlot.contains( editGUID ) )  continue;

      Slot  where = editSlot.value( editGUID );
      Edit& edit  = run_list[ where.run ].raws[ where.raw ].edits[ where.edit ];

      Model model;
      model.id          = dbase->value( 0 ).toString();
      model.guid        = dbase->value( 1 ).toString();
      model.description = dbase->value( 2 ).toString();
      model.editGUID    = editGUID;
      model.editID      = dbase->value( 6 ).toString();
      model.lastUpdated = dbase->value( 7 ).toString();
      model.checksum    = dbase->value( 8 ).toString();
      model.size        = dbase->value( 9 ).toString();
      model.subType     = model.description.section( ".", -2, -2 );

      edit.models << model;

      Slot slot  = where;
      slot.model = edit.models.size() - 1;
      modelSlot.insert( model.guid, slot );
   }

   // ---- the noise records, attached to their model by GUID ---------------
   query.clear();
   query << "get_noise_desc" << invID;
   dbase->query( query );

   status = dbase->lastErrno();

   if ( status != US_DB2::OK  &&  status != US_DB2::NOROWS )
   {
      error = dbase->lastError();
      return false;
   }

   while ( dbase->next() )
   {
      QString modelGUID = dbase->value( 5 ).toString();

      if ( ! modelSlot.contains( modelGUID ) )  continue;

      Slot   where = modelSlot.value( modelGUID );
      Model& model = run_list[ where.run ].raws[ where.raw ]
                     .edits[ where.edit ].models[ where.model ];

      Noise noise;
      noise.id          = dbase->value( 0 ).toString();
      noise.guid        = dbase->value( 1 ).toString();
      noise.noiseType   = dbase->value( 4 ).toString().left( 2 );
      noise.modelGUID   = modelGUID;
      noise.lastUpdated = dbase->value( 6 ).toString();
      noise.checksum    = dbase->value( 7 ).toString();
      noise.size        = dbase->value( 8 ).toString();
      noise.description = dbase->value( 9 ).toString();
      noise.editGUID    = model.editGUID;

      model.noises << noise;
   }

   error.clear();

   return true;
}

bool US_DataCatalog::loadAllDisk( QString& error )
{
   buildDiskIndex();

   for ( int ii = 0; ii < run_list.size(); ii++ )
   {
      QString message;

      if ( ! loadDetailDisk( run_list[ ii ], message ) )
         emit US_DataCatalog::message( message );
   }

   error.clear();

   return true;
}

// ------------------------------------------------------------ verification

bool US_DataCatalog::isRunVerified( int index ) const
{
   if ( index < 0  ||  index >= run_list.size() )  return false;

   return run_list.at( index ).verified;
}

void US_DataCatalog::queueVerify( const QList< int >& indexes )
{
   verify_queue = indexes;
}

void US_DataCatalog::requestVerify( int index )
{
   if ( index < 0  ||  index >= run_list.size() )  return;
   if ( run_list.at( index ).verified )            return;

   verify_queue.removeAll( index );
   verify_queue.prepend  ( index );
}

int US_DataCatalog::verifyPendingCount( void ) const
{
   return verify_queue.size();
}

bool US_DataCatalog::verifyNextPending( int& index, QString& error )
{
   index = -1;

   while ( ! verify_queue.isEmpty() )
   {
      int next = verify_queue.first();

      if ( next >= 0  &&  next < run_list.size()  &&
           ! run_list.at( next ).verified )
      {
         index = next;
         return verifyRun( next, error );
      }

      verify_queue.removeFirst();
   }

   error.clear();

   return false;
}

bool US_DataCatalog::verifyRun( int index, QString& error )
{
   if ( index < 0  ||  index >= run_list.size() )
   {
      error = tr( "There is no experiment at position %1" ).arg( index );
      return false;
   }

   Run& run = run_list[ index ];

   if ( run.verified )
   {
      verify_queue.removeAll( index );
      error.clear();
      return true;
   }

   bool ok = isDb() ? verifyRunDb( run, error ) : verifyRunDisk( run, error );

   verify_queue.removeAll( index );

   if ( ! ok )  return false;

   run.verified = true;
   error.clear();

   emit runVerified( index );

   return true;
}

/* Read the checksums of one experiment from the database.

   This is what the bulk listing left out, and it is the expensive half:
   the server hashes the experiment's raw-data and edit blobs to answer it.
   Models and noise carry their checksums already -- their payloads are
   small XML, so the listing hashes those as it goes.
*/
bool US_DataCatalog::verifyRunDb( Run& run, QString& error )
{
   if ( dbase == nullptr )
   {
      error = tr( "No database connection" );
      return false;
   }

   QStringList query;
   query << "get_rawData_by_experiment" << run.id;
   dbase->query( query );

   int status = dbase->lastErrno();

   if ( status != US_DB2::OK  &&  status != US_DB2::NOROWS )
   {
      error = dbase->lastError();
      return false;
   }

   QHash< QString, QPair< QString, QString > > digests;   // GUID -> md5, size

   while ( dbase->next() )
      digests.insert( dbase->value( 1 ).toString(),
                      qMakePair( dbase->value(  9 ).toString(),
                                 dbase->value( 10 ).toString() ) );

   query.clear();
   query << "get_editedData_by_experiment" << run.id;
   dbase->query( query );

   status = dbase->lastErrno();

   if ( status != US_DB2::OK  &&  status != US_DB2::NOROWS )
   {
      error = dbase->lastError();
      return false;
   }

   while ( dbase->next() )
      digests.insert( dbase->value( 3 ).toString(),
                      qMakePair( dbase->value( 8 ).toString(),
                                 dbase->value( 9 ).toString() ) );

   for ( int ii = 0; ii < run.raws.size(); ii++ )
   {
      Raw& raw = run.raws[ ii ];

      if ( digests.contains( raw.guid ) )
      {
         raw.checksum = digests.value( raw.guid ).first;
         raw.size     = digests.value( raw.guid ).second;
      }

      for ( int jj = 0; jj < raw.edits.size(); jj++ )
      {
         Edit& edit = raw.edits[ jj ];

         if ( ! digests.contains( edit.guid ) )  continue;

         edit.checksum = digests.value( edit.guid ).first;
         edit.size     = digests.value( edit.guid ).second;
      }
   }

   error.clear();

   return true;
}

// Read the checksums of one experiment's local files
bool US_DataCatalog::verifyRunDisk( Run& run, QString& error )
{
   bool saved   = do_checksums;
   do_checksums = true;

   for ( int ii = 0; ii < run.raws.size(); ii++ )
   {
      Raw& raw = run.raws[ ii ];

      if ( ! raw.path.isEmpty() )
         fileDigest( raw.path, raw.checksum, raw.size );

      for ( int jj = 0; jj < raw.edits.size(); jj++ )
      {
         Edit& edit = raw.edits[ jj ];

         if ( ! edit.path.isEmpty() )
            fileDigest( edit.path, edit.checksum, edit.size );

         for ( int kk = 0; kk < edit.models.size(); kk++ )
         {
            Model& model = edit.models[ kk ];

            if ( ! model.path.isEmpty() )
               fileDigest( model.path, model.checksum, model.size );

            for ( int mm = 0; mm < model.noises.size(); mm++ )
            {
               Noise& noise = model.noises[ mm ];

               if ( noise.path.isEmpty() )  continue;

               fileDigest( noise.path, noise.checksum, noise.size );
            }
         }
      }
   }

   do_checksums = saved;
   error.clear();

   return true;
}

bool US_DataCatalog::loadDetailDb( Run& run, QString& error )
{
   if ( dbase == nullptr )
   {
      error = tr( "No database connection" );
      return false;
   }

   if ( bulk_ok  &&  loadDetailDbBulk( run, error ) )  return true;

   if ( ! bulk_ok )
      return loadDetailDbLegacy( run, error );

   return false;
}

bool US_DataCatalog::loadDetailDbBulk( Run& run, QString& error )
{
   QString     invID = QString::number( inv_id );
   QStringList query;

   // ---- the triples ------------------------------------------------------
   query << "get_rawData_by_experiment" << run.id;
   dbase->query( query );

   if ( missingProcedure() )
   {
      bulk_ok = false;
      emit message( tr( "This database does not have the per-experiment"
                        " catalog procedures; falling back to one query per"
                        " record" ) );
      return false;
   }

   int status = dbase->lastErrno();

   if ( status != US_DB2::OK  &&  status != US_DB2::NOROWS )
   {
      error = dbase->lastError();
      return false;
   }

   run.raws.clear();

   while ( dbase->next() )
   {
      Raw raw;
      raw.id          = dbase->value(  0 ).toString();
      raw.guid        = dbase->value(  1 ).toString();
      raw.filename    = dbase->value(  3 ).toString().replace( "\\", "/" )
                        .section( "/", -1, -1 );
      raw.description = dbase->value(  4 ).toString();
      raw.solutionID  = dbase->value(  6 ).toString();
      raw.lastUpdated = dbase->value(  8 ).toString();
      raw.checksum    = dbase->value(  9 ).toString();
      raw.size        = dbase->value( 10 ).toString();
      raw.runID       = run.runID;
      raw.dataType    = raw.filename.section( ".", -5, -5 );
      raw.triple      = raw.filename.section( ".", -4, -2 );

      run.raws << raw;
   }

   // ---- the edits, attached to their triple by GUID ----------------------
   QHash< QString, int > rawByGuid;

   for ( int ii = 0; ii < run.raws.size(); ii++ )
      rawByGuid.insert( run.raws[ ii ].guid, ii );

   query.clear();
   query << "get_editedData_by_experiment" << run.id;
   dbase->query( query );

   status = dbase->lastErrno();

   if ( status != US_DB2::OK  &&  status != US_DB2::NOROWS )
   {
      error = dbase->lastError();
      return false;
   }

   QHash< QString, QPair< int, int > > editByGuid;   // editGUID -> raw, edit

   while ( dbase->next() )
   {
      Edit edit;
      edit.id          = dbase->value( 0 ).toString();
      edit.rawGUID     = dbase->value( 2 ).toString();
      edit.guid        = dbase->value( 3 ).toString();
      edit.filename    = dbase->value( 5 ).toString().replace( "\\", "/" )
                         .section( "/", -1, -1 );
      edit.lastUpdated = dbase->value( 7 ).toString();
      edit.checksum    = dbase->value( 8 ).toString();
      edit.size        = dbase->value( 9 ).toString();
      edit.runID       = run.runID;
      edit.editID      = edit.filename.section( ".", -6, -6 );
      edit.triple      = edit.filename.section( ".", -4, -2 );
      edit.label       = edit.editID + " (" + edit.triple + ")";

      int rawx = rawByGuid.value( edit.rawGUID, -1 );

      if ( rawx < 0 )  continue;          // an edit of a triple we do not have

      run.raws[ rawx ].edits << edit;
      editByGuid.insert( edit.guid,
                         qMakePair( rawx, run.raws[ rawx ].edits.size() - 1 ) );
   }

   // ---- the models, attached to their edit by GUID -----------------------
   query.clear();
   query << "get_model_desc_by_experiment" << invID << run.id;
   dbase->query( query );

   status = dbase->lastErrno();

   if ( status != US_DB2::OK  &&  status != US_DB2::NOROWS )
   {
      error = dbase->lastError();
      return false;
   }

   QHash< QString, QPair< int, int > > modelPlace;  // modelGUID -> raw, edit
   QHash< QString, int >               modelIndex;  // modelGUID -> model slot

   while ( dbase->next() )
   {
      Model model;
      model.id          = dbase->value( 0 ).toString();
      model.guid        = dbase->value( 1 ).toString();
      model.description = dbase->value( 2 ).toString();
      model.editGUID    = dbase->value( 5 ).toString();
      model.editID      = dbase->value( 6 ).toString();
      model.lastUpdated = dbase->value( 7 ).toString();
      model.checksum    = dbase->value( 8 ).toString();
      model.size        = dbase->value( 9 ).toString();
      model.subType     = model.description.section( ".", -2, -2 );

      if ( ! editByGuid.contains( model.editGUID ) )  continue;

      QPair< int, int > place = editByGuid.value( model.editGUID );
      run.raws[ place.first ].edits[ place.second ].models << model;

      modelPlace.insert( model.guid, place );
      modelIndex.insert( model.guid,
            run.raws[ place.first ].edits[ place.second ].models.size() - 1 );
   }

   // ---- the noise records, attached to their model by GUID ---------------
   query.clear();
   query << "get_noise_desc_by_experiment" << invID << run.id;
   dbase->query( query );

   status = dbase->lastErrno();

   if ( status != US_DB2::OK  &&  status != US_DB2::NOROWS )
   {
      error = dbase->lastError();
      return false;
   }

   while ( dbase->next() )
   {
      Noise noise;
      noise.id          = dbase->value( 0 ).toString();
      noise.guid        = dbase->value( 1 ).toString();
      noise.noiseType   = dbase->value( 4 ).toString().left( 2 );
      noise.modelGUID   = dbase->value( 5 ).toString();
      noise.lastUpdated = dbase->value( 6 ).toString();
      noise.checksum    = dbase->value( 7 ).toString();
      noise.size        = dbase->value( 8 ).toString();
      noise.description = dbase->value( 9 ).toString();

      if ( ! modelPlace.contains( noise.modelGUID ) )  continue;

      QPair< int, int > place = modelPlace.value( noise.modelGUID );
      int               mdlx  = modelIndex.value( noise.modelGUID );

      noise.editGUID = run.raws[ place.first ].edits[ place.second ].guid;
      run.raws[ place.first ].edits[ place.second ].models[ mdlx ].noises
         << noise;
   }

   error.clear();

   return true;
}

bool US_DataCatalog::loadDetailDbLegacy( Run& run, QString& error )
{
   QString     invID = QString::number( inv_id );
   QStringList query;
   QStringList rawIDs;
   QStringList rawNames;

   query << "get_rawDataIDs" << run.id;
   dbase->query( query );

   int status = dbase->lastErrno();

   if ( status != US_DB2::OK  &&  status != US_DB2::NOROWS )
   {
      error = dbase->lastError();
      return false;
   }

   while ( dbase->next() )
   {
      rawIDs   << dbase->value( 0 ).toString();
      rawNames << dbase->value( 2 ).toString().replace( "\\", "/" )
                  .section( "/", -1, -1 );
   }

   run.raws.clear();

   for ( int ii = 0; ii < rawIDs.size(); ii++ )
   {
      Raw raw;
      raw.id       = rawIDs  [ ii ];
      raw.filename = rawNames[ ii ];
      raw.runID    = run.runID;
      raw.dataType = raw.filename.section( ".", -5, -5 );
      raw.triple   = raw.filename.section( ".", -4, -2 );

      query.clear();
      query << "get_rawData" << raw.id;
      dbase->query( query );

      if ( dbase->next() )
      {
         raw.guid        = dbase->value( 0 ).toString();
         raw.description = dbase->value( 3 ).toString();
         raw.solutionID  = dbase->value( 5 ).toString();
         raw.lastUpdated = dbase->value( 7 ).toString();
         raw.checksum    = dbase->value( 8 ).toString();
         raw.size        = dbase->value( 9 ).toString();
      }

      QStringList edtIDs;

      query.clear();
      query << "get_editedDataIDs" << raw.id;
      dbase->query( query );

      while ( dbase->next() )
         edtIDs << dbase->value( 0 ).toString();

      for ( int jj = 0; jj < edtIDs.size(); jj++ )
      {
         Edit edit;
         edit.id      = edtIDs[ jj ];
         edit.runID   = run.runID;
         edit.rawGUID = raw.guid;

         query.clear();
         query << "get_editedData" << edit.id;
         dbase->query( query );

         if ( ! dbase->next() )  continue;

         edit.guid        = dbase->value( 1 ).toString();
         edit.filename    = dbase->value( 3 ).toString().replace( "\\", "/" )
                            .section( "/", -1, -1 );
         edit.lastUpdated = dbase->value( 5 ).toString();
         edit.checksum    = dbase->value( 6 ).toString();
         edit.size        = dbase->value( 7 ).toString();
         edit.editID      = edit.filename.section( ".", -6, -6 );
         edit.triple      = edit.filename.section( ".", -4, -2 );
         edit.label       = edit.editID + " (" + edit.triple + ")";

         QList< Model > models;

         query.clear();
         query << "get_model_desc_by_editID" << invID << edit.id;
         dbase->query( query );

         while ( dbase->next() )
         {
            Model model;
            model.id          = dbase->value( 0 ).toString();
            model.guid        = dbase->value( 1 ).toString();
            model.description = dbase->value( 2 ).toString();
            model.editGUID    = dbase->value( 5 ).toString();
            model.editID      = dbase->value( 6 ).toString();
            model.lastUpdated = dbase->value( 7 ).toString();
            model.checksum    = dbase->value( 8 ).toString();
            model.size        = dbase->value( 9 ).toString();
            model.subType     = model.description.section( ".", -2, -2 );

            if ( model.editGUID.isEmpty() )  model.editGUID = edit.guid;

            models << model;
         }

         query.clear();
         query << "get_noise_desc_by_editID" << invID << edit.id;
         dbase->query( query );

         QList< Noise > noises;

         while ( dbase->next() )
         {
            Noise noise;
            noise.id          = dbase->value( 0 ).toString();
            noise.guid        = dbase->value( 1 ).toString();
            noise.noiseType   = dbase->value( 4 ).toString().left( 2 );
            noise.modelGUID   = dbase->value( 5 ).toString();
            noise.lastUpdated = dbase->value( 6 ).toString();
            noise.checksum    = dbase->value( 7 ).toString();
            noise.size        = dbase->value( 8 ).toString();
            noise.description = dbase->value( 9 ).toString();
            noise.editGUID    = edit.guid;

            noises << noise;
         }

         for ( int kk = 0; kk < models.size(); kk++ )
            for ( int mm = 0; mm < noises.size(); mm++ )
               if ( noises[ mm ].modelGUID == models[ kk ].guid )
                  models[ kk ].noises << noises[ mm ];

         edit.models = models;
         raw.edits  << edit;
      }

      run.raws << raw;
   }

   error.clear();

   return true;
}

bool US_DataCatalog::loadDetailDisk( Run& run, QString& error )
{
   buildDiskIndex();

   if ( ! index->runs.contains( run.runID ) )
   {
      error = tr( "The run directory of %1 is no longer there" )
              .arg( run.runID );
      return false;
   }

   const DiskIndex::RunFiles& rf = index->runs[ run.runID ];

   // Bucket this run's edit files by triple, once, rather than asking the
   // file system for them again for every triple
   QMultiHash< QString, QString > editsOfTriple;

   for ( int ii = 0; ii < rf.editFiles.size(); ii++ )
   {
      QString name = rf.editFiles[ ii ];
      editsOfTriple.insert( name.section( ".", -5, -2 ), name );
   }

   run.raws.clear();

   int editKnt  = 0;
   int modelKnt = 0;
   int noiseKnt = 0;

   for ( int ii = 0; ii < rf.aucFiles.size(); ii++ )
   {
      QString aucName = rf.aucFiles[ ii ];
      Raw     raw;
      raw.filename = aucName;
      raw.path     = rf.dirPath + "/" + aucName;
      raw.runID    = run.runID;
      raw.dataType = aucName.section( ".", -5, -5 );
      raw.triple   = aucName.section( ".", -4, -2 );

      // The GUID and the description are in the .auc header; the scan data
      // below it is not wanted here
      US_DataIO::RawData header;

      if ( US_DataIO::readRawHeader( raw.path, header ) == US_DataIO::OK )
      {
         raw.guid        = US_Util::uuid_unparse(
                           (unsigned char*)header.rawGUID );
         raw.description = header.description;
      }

      fileDigest( raw.path, raw.checksum, raw.size );
      raw.lastUpdated  = utc_of_file( raw.path );

      QStringList edits = editsOfTriple.values( aucName.section( ".", -5, -2 ) );
      edits.sort();

      for ( int jj = 0; jj < edits.size(); jj++ )
      {
         QString edtName = edits[ jj ];
         Edit    edit;
         edit.filename = edtName;
         edit.path     = rf.dirPath + "/" + edtName;
         edit.runID    = run.runID;
         edit.editID   = edtName.section( ".", -6, -6 );
         edit.triple   = raw.triple;

         peek_edit_guids( edit.path, edit.guid, edit.rawGUID );

         if ( edit.rawGUID.isEmpty() )  edit.rawGUID = raw.guid;
         if ( raw.guid.isEmpty()     )  raw.guid     = edit.rawGUID;

         edit.label    = edit.editID + " (" + edit.triple + ")";

         fileDigest( edit.path, edit.checksum, edit.size );
         edit.lastUpdated = utc_of_file( edit.path );

         // The models fitted to this edit, from the index
         QList< int > mdlxs = index->modelsByEdit.values( edit.guid );
         std::sort( mdlxs.begin(), mdlxs.end() );

         for ( int kk = 0; kk < mdlxs.size(); kk++ )
         {
            const DiskIndex::ModelFile& mf = index->models[ mdlxs[ kk ] ];

            Model model;
            model.guid        = mf.guid;
            model.editGUID    = mf.editGUID;
            model.description = mf.description;
            model.filename    = mf.base;
            model.path        = mf.path;
            model.subType     = mf.description.section( ".", -2, -2 );

            fileDigest( mf.path, model.checksum, model.size );
            model.lastUpdated = utc_of_file( mf.path );

            QList< int > noixs = index->noisesByModel.values( mf.guid );
            std::sort( noixs.begin(), noixs.end() );

            for ( int mm = 0; mm < noixs.size(); mm++ )
            {
               const DiskIndex::NoiseFile& nf = index->noises[ noixs[ mm ] ];

               Noise noise;
               noise.guid        = nf.guid;
               noise.modelGUID   = nf.modelGUID;
               noise.editGUID    = edit.guid;
               noise.description = nf.description;
               noise.noiseType   = nf.noiseType;
               noise.filename    = nf.base;
               noise.path        = nf.path;

               fileDigest( nf.path, noise.checksum, noise.size );
               noise.lastUpdated = utc_of_file( nf.path );

               model.noises << noise;
               noiseKnt++;
            }

            edit.models << model;
            modelKnt++;
         }

         raw.edits << edit;
         editKnt++;
      }

      run.raws << raw;
   }

   run.rawCount   = run.raws.size();
   run.editCount  = editKnt;
   run.modelCount = modelKnt;
   run.noiseCount = noiseKnt;

   error.clear();

   return true;
}
