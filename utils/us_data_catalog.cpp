//! \file us_data_catalog.cpp
#include "us_data_catalog.h"

#include "us_settings.h"
#include "us_util.h"
#include "us_dataIO.h"
#include "us_convertio.h"

// ---------------------------------------------------------------- helpers

namespace
{
   // Read the attributes of the first element of each of the given names.
   //
   // Identifying a model, a noise record or an edit only needs what is on
   // its first element.  Parsing the rest of the file -- every component of
   // a model, every value of a noise vector -- is what a store full of
   // those files cannot afford.  Reading stops once every name is found.
   QMap< QString, QMap< QString, QString > > peek_elements(
         const QString& filename, const QStringList& elements )
   {
      QMap< QString, QMap< QString, QString > > found;
      QFile                                     file( filename );

      if ( ! file.open( QIODevice::ReadOnly | QIODevice::Text ) )
         return found;

      QXmlStreamReader xml( &file );

      while ( ! xml.atEnd()  &&  found.size() < elements.size() )
      {
         xml.readNext();

         if ( ! xml.isStartElement() )                 continue;

         QString name = xml.name().toString();

         if ( ! elements.contains( name )  ||  found.contains( name ) )
            continue;

         QMap< QString, QString > values;
         QXmlStreamAttributes     attrs = xml.attributes();

         for ( int ii = 0; ii < attrs.size(); ii++ )
            values.insert( attrs.at( ii ).name ().toString(),
                           attrs.at( ii ).value().toString() );

         found.insert( name, values );
      }

      file.close();

      return found;
   }

   QMap< QString, QString > peek_attributes( const QString& filename,
                                             const QString& element )
   {
      return peek_elements( filename, QStringList( element ) ).value( element );
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

   // One form for every date the catalog gives
   QString utc_text( const QDateTime& when )
   {
      return when.toUTC().toString( "yyyy-MM-dd hh:mm:ss" ) + " UTC";
   }

   QString utc_of_file( const QString& path )
   {
      return utc_text( QFileInfo( path ).lastModified() );
   }

   // The base name of a path the database recorded, whichever separator
   QString base_name( const QString& path )
   {
      return QString( path ).replace( "\\", "/" ).section( "/", -1, -1 );
   }

   // The triple key of a .auc or edit file: type.cell.channel.wavelength
   QString triple_key( const QString& filename )
   {
      return filename.section( ".", -5, -2 );
   }
}

// ------------------------------------------------------------- disk index

/*! \class US_DataCatalog::DiskIndex
    \brief What one pass over the local data directory found

    A local store is read once per listing.  The results directory gives one
    listing per run directory, and the model and noise directories give one
    listing each, whose files are peeked for the handful of attributes that
    say what they are.  Everything a later question asks -- which edits
    belong to this triple, which models were fitted to this edit -- is
    answered from this index rather than from the file system again.

    Run directories are kept by path, not by run ID: two directories whose
    files carry the same run ID are two runs, and both are listed.
*/
class US_DataCatalog::DiskIndex
{
   public:
      DiskIndex() : built( false ) {}

      class RunFiles
      {
         public:
            QString     dirPath;
            QString     dirName;
            QString     runID;       // from the .auc names; the directory
                                     //  name when there is no .auc file
            QString     runType;
            QString     expXml;      // base name of the experiment XML
            QString     tmstFile;    // base name of the time-state file
            QStringList aucFiles;    // base names
            QStringList editFiles;   // base names
            bool        mixedRuns = false;  // .auc files of other runs too
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
      QList< RunFiles >             runs;           // by directory name
      QHash< QString, int >         runByDir;       // dirPath   -> runs[]
      QList< ModelFile >            models;
      QList< NoiseFile >            noises;
      QMultiHash< QString, int >    modelsByEdit;   // editGUID  -> models[]
      QHash< QString, int >         modelByGuid;    // modelGUID -> models[]
      QMultiHash< QString, int >    noisesByModel;  // modelGUID -> noises[]

      void clear( void )
      {
         runs         .clear();
         runByDir     .clear();
         models       .clear();
         noises       .clear();
         modelsByEdit .clear();
         modelByGuid  .clear();
         noisesByModel.clear();
         built = false;
      }

      // The positions of the models fitted to an edit, in file order
      QList< int > modelsOf( const QString& editGUID ) const
      {
         QList< int > list;

         if ( editGUID.isEmpty() )  return list;

         list = modelsByEdit.values( editGUID );
         std::sort( list.begin(), list.end() );
         return list;
      }

      // The positions of the noise records of a model, in file order
      QList< int > noisesOf( const QString& modelGUID ) const
      {
         QList< int > list;

         if ( modelGUID.isEmpty() )  return list;

         list = noisesByModel.values( modelGUID );
         std::sort( list.begin(), list.end() );
         return list;
      }
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
   usable     = true;
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

bool US_DataCatalog::Orphans::isEmpty( void ) const
{
   return raws.isEmpty()  &&  edits.isEmpty()  &&
          models.isEmpty()  &&  noises.isEmpty();
}

void US_DataCatalog::Orphans::clear( void )
{
   raws  .clear();
   edits .clear();
   models.clear();
   noises.clear();
}

// ------------------------------------------------- assembling the chain

namespace
{
   typedef US_DataCatalog::Raw   Raw;
   typedef US_DataCatalog::Edit  Edit;
   typedef US_DataCatalog::Model Model;
   typedef US_DataCatalog::Noise Noise;

   /* Attach flat lists of records to their parents by GUID, bottom up.

      Noise goes into its model, models into their edit, edits into their
      triple, and what then holds its whole branch is attached one level up.
      A record whose parent key is empty or matches nothing goes to the
      orphan list when one is given, and is dropped otherwise.  An empty key
      never matches: a record written without a GUID must not become the
      parent of every other such record.
   */
   void attach_models( QList< Model >& models, QList< Noise >& noises,
                       QList< Noise >* orphans )
   {
      QHash< QString, int > byGuid;

      for ( int ii = 0; ii < models.size(); ii++ )
         if ( ! models[ ii ].guid.isEmpty()  &&
              ! byGuid.contains( models[ ii ].guid ) )
            byGuid.insert( models[ ii ].guid, ii );

      for ( int ii = 0; ii < noises.size(); ii++ )
      {
         Noise noise = noises[ ii ];
         int   mx    = noise.modelGUID.isEmpty()
                       ? -1 : byGuid.value( noise.modelGUID, -1 );

         if ( mx < 0 )
         {
            if ( orphans != nullptr )  *orphans << noise;
            continue;
         }

         if ( noise.editGUID.isEmpty() )
            noise.editGUID = models[ mx ].editGUID;

         models[ mx ].noises << noise;
      }
   }

   void attach_edits( QList< Edit >& edits, QList< Model >& models,
                      QList< Model >* orphans )
   {
      QHash< QString, int > byGuid;

      for ( int ii = 0; ii < edits.size(); ii++ )
         if ( ! edits[ ii ].guid.isEmpty()  &&
              ! byGuid.contains( edits[ ii ].guid ) )
            byGuid.insert( edits[ ii ].guid, ii );

      for ( int ii = 0; ii < models.size(); ii++ )
      {
         int ex = models[ ii ].editGUID.isEmpty()
                  ? -1 : byGuid.value( models[ ii ].editGUID, -1 );

         if ( ex < 0 )
         {
            if ( orphans != nullptr )  *orphans << models[ ii ];
            continue;
         }

         edits[ ex ].models << models[ ii ];
      }
   }

   void attach_raws( QList< Raw >& raws, QList< Edit >& edits,
                     QList< Edit >* orphans )
   {
      QHash< QString, int > byGuid;

      for ( int ii = 0; ii < raws.size(); ii++ )
         if ( ! raws[ ii ].guid.isEmpty()  &&
              ! byGuid.contains( raws[ ii ].guid ) )
            byGuid.insert( raws[ ii ].guid, ii );

      for ( int ii = 0; ii < edits.size(); ii++ )
      {
         int rx = edits[ ii ].rawGUID.isEmpty()
                  ? -1 : byGuid.value( edits[ ii ].rawGUID, -1 );

         if ( rx < 0 )
         {
            if ( orphans != nullptr )  *orphans << edits[ ii ];
            continue;
         }

         edits[ ii ].runID = raws[ rx ].runID;
         raws[ rx ].edits << edits[ ii ];
      }
   }

   // The flat lists one database read produces
   class Flat
   {
      public:
         QList< Raw >   raws;
         QList< Edit >  edits;
         QList< Model > models;
         QList< Noise > noises;

         // Assemble everything below the triples
         void assemble( US_DataCatalog::Orphans* orphans )
         {
            attach_models( models, noises,
                           orphans ? &orphans->noises : nullptr );
            attach_edits ( edits,  models,
                           orphans ? &orphans->models : nullptr );
            attach_raws  ( raws,   edits,
                           orphans ? &orphans->edits  : nullptr );
         }
   };

   // A triple as the rawData procedures give it
   void raw_names( Raw& raw )
   {
      raw.dataType = raw.filename.section( ".", -5, -5 );
      raw.triple   = raw.filename.section( ".", -4, -2 );
      raw.runID    = US_DataCatalog::runIdOfFile( raw.filename, false );
   }

   // An edit as the editedData procedures give it
   void edit_names( Edit& edit )
   {
      edit.editID = edit.filename.section( ".", -6, -6 );
      edit.triple = edit.filename.section( ".", -4, -2 );
      edit.runID  = US_DataCatalog::runIdOfFile( edit.filename, true );
      edit.label  = edit.editID + " (" + edit.triple + ")";
   }

   // A model row of get_model_desc and the procedures like it:
   // modelID, modelGUID, description, -, -, editGUID, editedDataID,
   // lastUpdated, checksum, size
   Model model_row( IUS_DB2* db )
   {
      Model model;
      model.id          = db->value( 0 ).toString();
      model.guid        = db->value( 1 ).toString();
      model.description = db->value( 2 ).toString();
      model.editGUID    = db->value( 5 ).toString();
      model.editID      = db->value( 6 ).toString();
      model.lastUpdated = US_DataCatalog::utcText( db->value( 7 ) );
      model.checksum    = db->value( 8 ).toString();
      model.size        = db->value( 9 ).toString();
      model.subType     = model.description.section( ".", -2, -2 );
      return model;
   }

   // A noise row of get_noise_desc and the procedures like it:
   // noiseID, noiseGUID, editedDataID, modelID, noiseType, modelGUID,
   // lastUpdated, checksum, size, description
   Noise noise_row( IUS_DB2* db )
   {
      Noise noise;
      noise.id          = db->value( 0 ).toString();
      noise.guid        = db->value( 1 ).toString();
      noise.noiseType   = db->value( 4 ).toString().left( 2 );
      noise.modelGUID   = db->value( 5 ).toString();
      noise.lastUpdated = US_DataCatalog::utcText( db->value( 6 ) );
      noise.checksum    = db->value( 7 ).toString();
      noise.size        = db->value( 8 ).toString();
      noise.description = db->value( 9 ).toString();
      return noise;
   }

   // A triple row of the catalog procedures get_rawData_by_experiment and
   // get_rawData_by_person: rawDataID, rawDataGUID, label, filename,
   // comment, experimentID, solutionID, -, lastUpdated, checksum, size
   Raw raw_row( IUS_DB2* db )
   {
      Raw raw;
      raw.id           = db->value(  0 ).toString();
      raw.guid         = db->value(  1 ).toString();
      raw.filename     = base_name( db->value( 3 ).toString() );
      raw.description  = db->value(  4 ).toString();
      raw.experimentID = db->value(  5 ).toString();
      raw.solutionID   = db->value(  6 ).toString();
      raw.lastUpdated  = US_DataCatalog::utcText( db->value( 8 ) );
      raw.checksum     = db->value(  9 ).toString();
      raw.size         = db->value( 10 ).toString();
      raw_names( raw );
      return raw;
   }

   // An edit row of the catalog procedures get_editedData_by_experiment and
   // get_editedData_by_person: editedDataID, -, rawDataGUID, editGUID, -,
   // filename, -, lastUpdated, checksum, size
   Edit edit_row( IUS_DB2* db )
   {
      Edit edit;
      edit.id          = db->value( 0 ).toString();
      edit.rawGUID     = db->value( 2 ).toString();
      edit.guid        = db->value( 3 ).toString();
      edit.filename    = base_name( db->value( 5 ).toString() );
      edit.lastUpdated = US_DataCatalog::utcText( db->value( 7 ) );
      edit.checksum    = db->value( 8 ).toString();
      edit.size        = db->value( 9 ).toString();
      edit_names( edit );
      return edit;
   }
}

// ----------------------------------------------------------- the catalog

US_DataCatalog::US_DataCatalog( QObject* parent ) : QObject( parent )
{
   src          = US_DataCatalog::Disk;
   dbase        = nullptr;
   inv_id       = US_Settings::us_inv_ID();
   disk_index   = new DiskIndex();
   tier_mask    = AllTiers;
   allowed_tiers = AllTiers;
   owns_db      = false;
   do_checksums = true;
   noise_cached = false;
   cancel_flag  = nullptr;
}

US_DataCatalog::~US_DataCatalog()
{
   close();

   delete disk_index;
   disk_index = nullptr;
}

bool US_DataCatalog::open( Source source, const QString& dbPassword,
                           QString& error )
{
   close();

   src       = source;
   inv_id    = US_Settings::us_inv_ID();
   tier_mask = AllTiers;
   owns_db   = false;

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

   US_DB2* conn = new US_DB2( dbPassword );

   if ( conn->lastErrno() != US_DB2::OK  ||  ! conn->isConnected() )
   {
      error = tr( "Cannot connect to the database: %1" )
              .arg( conn->lastError() );
      delete conn;
      return false;
   }

   dbase   = conn;
   owns_db = true;

   error.clear();

   return true;
}

bool US_DataCatalog::attach( IUS_DB2* a_db, QString& error )
{
   close();

   if ( a_db == nullptr )
   {
      error = tr( "No database connection was given" );
      return false;
   }

   if ( ! a_db->isConnected() )
   {
      error = tr( "The database connection given is not connected" );
      return false;
   }

   src       = US_DataCatalog::Db;
   dbase     = a_db;
   owns_db   = false;
   inv_id    = US_Settings::us_inv_ID();
   tier_mask = AllTiers;

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
   run_list    .clear();
   pending     .clear();
   verify_queue.clear();
   orphan_list .clear();
   noise_cache .clear();
   noise_cached = false;

   if ( disk_index != nullptr )
      disk_index->clear();
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

      int     mx       = disk_index->modelByGuid.value( modelGUID, -1 );
      QString editGUID = ( mx < 0 ) ? QString()
                                    : disk_index->models[ mx ].editGUID;

      QList< int > noixs = disk_index->noisesOf( modelGUID );

      for ( int ii = 0; ii < noixs.size(); ii++ )
      {
         const DiskIndex::NoiseFile& nf = disk_index->noises[ noixs[ ii ] ];

         Noise noise;
         noise.guid        = nf.guid;
         noise.modelGUID   = nf.modelGUID;
         noise.editGUID    = editGUID;
         noise.description = nf.description;
         noise.noiseType   = nf.noiseType;
         noise.filename    = nf.base;
         noise.path        = nf.path;
         noise.lastUpdated = utc_of_file( nf.path );

         fileDigest( nf.path, noise.checksum, noise.size );

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
   // by model, so the investigator's noise records are listed once and kept
   if ( ! cacheNoise( error ) )  return list;

   list = noise_cache.value( modelGUID );

   // The edit is known when the model is one the catalog has read
   QString editGUID;

   for ( int ii = 0; ii < run_list.size()  &&  editGUID.isEmpty(); ii++ )
      for ( const Raw& raw : run_list[ ii ].raws )
         for ( const Edit& edit : raw.edits )
            for ( const Model& model : edit.models )
               if ( model.guid == modelGUID )  editGUID = model.editGUID;

   for ( const Model& model : orphan_list.models )
      if ( model.guid == modelGUID )  editGUID = model.editGUID;

   for ( int ii = 0; ii < list.size(); ii++ )
      list[ ii ].editGUID = editGUID;

   return list;
}

bool US_DataCatalog::cacheNoise( QString& error )
{
   if ( noise_cached )  return true;

   QStringList query;
   query << "get_noise_desc" << QString::number( inv_id );

   if ( ! queryOk( query, error ) )  return false;

   QHash< QString, QList< Noise > > cache;

   while ( dbase->next() )
   {
      Noise noise = noise_row( dbase );

      if ( noise.modelGUID.isEmpty() )  continue;

      cache[ noise.modelGUID ] << noise;
   }

   noise_cache  = cache;
   noise_cached = true;

   return true;
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
      QList< int > mdlxs = disk_index->modelsOf( editGUID );

      for ( int ii = 0; ii < mdlxs.size(); ii++ )
      {
         const DiskIndex::ModelFile& mf = disk_index->models[ mdlxs[ ii ] ];

         QString message;
         list << noisesOfModel( mf.guid, message );

         if ( ! message.isEmpty() )  error = message;
      }

      for ( int ii = 0; ii < list.size(); ii++ )
         list[ ii ].editGUID = editGUID;

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

   if ( ! queryOk( query, error ) )  return list;

   while ( dbase->next() )
   {
      Noise noise    = noise_row( dbase );
      noise.editGUID = editGUID;

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

void US_DataCatalog::setCancelFlag( const QAtomicInt* flag )
{
   cancel_flag = flag;
}

bool US_DataCatalog::cancelled( void ) const
{
   return cancel_flag != nullptr  &&  cancel_flag->loadAcquire() != 0;
}

bool US_DataCatalog::cancelledError( QString& error ) const
{
   if ( ! cancelled() )  return false;

   error = tr( "Reading the local data directory was cancelled" );
   return true;
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

void US_DataCatalog::dropChecksums( QList< Raw >& raws )
{
   if ( do_checksums )  return;

   for ( int ii = 0; ii < raws.size(); ii++ )
   {
      raws[ ii ].checksum.clear();
      raws[ ii ].size    .clear();

      for ( int jj = 0; jj < raws[ ii ].edits.size(); jj++ )
      {
         raws[ ii ].edits[ jj ].checksum.clear();
         raws[ ii ].edits[ jj ].size    .clear();
      }
   }
}

void US_DataCatalog::setTiers( int tiers )
{
   allowed_tiers = tiers & AllTiers;
}

int US_DataCatalog::tiers( void ) const
{
   return tier_mask & allowed_tiers;
}

void US_DataCatalog::setBulkQueries( bool on )
{
   setTiers( on ? AllTiers : 0 );
}

bool US_DataCatalog::bulkQueries( void ) const
{
   return ( tiers() & ExperimentTier ) != 0;
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

IUS_DB2* US_DataCatalog::db( void )
{
   return dbase;
}

int US_DataCatalog::investigatorID( void ) const
{
   return inv_id;
}

bool US_DataCatalog::refreshInvestigator( void )
{
   int now_id = US_Settings::us_inv_ID();

   if ( now_id == inv_id )  return false;

   inv_id = now_id;

   if ( ! isDb() )  return false;

   // Every database query names the person, so a listing read for the
   // previous one describes records the user is no longer looking at
   clear();

   return true;
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

bool US_DataCatalog::runIdIsUsable( const QString& runID )
{
   return US_ConvertIO::runIdIsUsable( runID );
}

QString US_DataCatalog::utcText( const QVariant& value )
{
   QString text = value.toString().trimmed();

   if ( text.isEmpty() )  return QString();

   QDateTime when;

   if ( value.userType() == QMetaType::QDateTime )
      when = value.toDateTime();

   else
   {  // The server gives "yyyy-MM-dd hh:mm:ss", already in UTC
      QString iso = text;

      if ( iso.length() > 10  &&  iso.at( 10 ) == ' ' )
         iso[ 10 ] = 'T';

      if ( iso.endsWith( " UTC" ) )  iso.chop( 4 );

      when = QDateTime::fromString( iso, Qt::ISODate );
   }

   if ( ! when.isValid() )
      return US_Util::toUTCDatetimeText( text, true );

   when.setTimeSpec( Qt::UTC );

   return utc_text( when );
}

bool US_DataCatalog::queryOk( const QStringList& query, QString& error )
{
   dbase->query( query );

   int status = dbase->lastErrno();

   if ( status == IUS_DB2::OK  ||  status == IUS_DB2::NOROWS )
      return true;

   error = dbase->lastError();

   if ( error.isEmpty() )
      error = tr( "%1 failed with status %2" )
              .arg( query.value( 0 ) ).arg( status );

   return false;
}

bool US_DataCatalog::missingProcedure( void )
{
   if ( dbase == nullptr )                    return false;
   if ( dbase->lastErrno() == IUS_DB2::OK )   return false;

   // A procedure the server does not have fails the CALL itself rather than
   // returning a status, and the server's error number is what says so, in
   // any language.  One the account may not run is as good as missing: the
   // older procedures are what to use, and if they are refused too, that is
   // the error the caller gets.
   int code = dbase->lastServerErrno();

   return code == IUS_DB2::serverNoSuchRoutine  ||
          code == IUS_DB2::serverRoutineDenied;
}

void US_DataCatalog::dropTier( int tier, const QString& procedure )
{
   tier_mask &= ~tier;

   QString what = ( tier == ListingTier )    ? tr( "experiment summary" )
                : ( tier == ExperimentTier ) ? tr( "per-experiment catalog" )
                :                              tr( "whole-store catalog" );

   emit message( tr( "This database cannot run %1, so the %2 procedures are"
                     " not used; falling back on the older ones" )
                 .arg( procedure, what ) );
}

// ------------------------------------------------------------- first layer

bool US_DataCatalog::loadRuns( QString& error )
{
   return loadRuns( QString(), error );
}

bool US_DataCatalog::loadRuns( const QString& projectGUID, QString& error )
{
   refreshInvestigator();

   QString      saved_guid = proj_guid;
   QList< Run > fresh;

   proj_guid = projectGUID;

   bool ok = false;

   if ( isDb() )
      ok = loadRunsDb( fresh, error );

   else
   {  // A listing reads the store as it is now, not as it was last time
      disk_index->clear();
      ok = loadRunsDisk( fresh, error );
   }

   if ( ! ok )
   {
      proj_guid = saved_guid;
      return false;
   }

   run_list = fresh;
   pending     .clear();
   verify_queue.clear();
   orphan_list .clear();
   noise_cache .clear();
   noise_cached = false;

   for ( int ii = 0; ii < run_list.size(); ii++ )
      pending << ii;

   error.clear();

   emit runsListed( run_list.size() );

   return true;
}

bool US_DataCatalog::loadRunsDb( QList< Run >& runs, QString& error )
{
   if ( dbase == nullptr )
   {
      error = tr( "No database connection" );
      return false;
   }

   if ( ( tiers() & ListingTier ) != 0 )
   {
      if ( loadRunsDbBulk( runs, error ) )  return true;

      if ( ( tiers() & ListingTier ) != 0 )  return false;   // a real error

      runs.clear();
   }

   return loadRunsDbLegacy( runs, error );
}

bool US_DataCatalog::loadRunsDbBulk( QList< Run >& runs, QString& error )
{
   QStringList query;
   query << "get_experiment_summary" << QString::number( inv_id );

   if ( ! queryOk( query, error ) )
   {
      if ( missingProcedure() )  dropTier( ListingTier, query[ 0 ] );
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
      run.date        = utcText( dbase->value( 9 ) );
      run.rawCount    = dbase->value( 10 ).toInt();
      run.editCount   = dbase->value( 11 ).toInt();
      run.modelCount  = dbase->value( 12 ).toInt();
      run.noiseCount  = dbase->value( 13 ).toInt();

      if ( ! proj_guid.isEmpty()  &&  run.projectGUID != proj_guid )  continue;

      if ( ! runIdIsUsable( run.runID ) )
      {
         run.usable  = false;
         run.problem = tr( "The run ID %1 is not one UltraScan accepts" )
                       .arg( run.runID );
      }

      runs << run;
   }

   return true;
}

bool US_DataCatalog::loadRunsDbLegacy( QList< Run >& runs, QString& error )
{
   QString     invID = QString::number( inv_id );
   QStringList query;
   query << "get_experiment_desc" << invID;

   if ( ! queryOk( query, error ) )  return false;

   QList< Run > headers;

   while ( dbase->next() )
   {
      Run run;
      run.id      = dbase->value( 0 ).toString();
      run.runID   = dbase->value( 1 ).toString();
      run.expType = dbase->value( 2 ).toString();
      run.runType = dbase->value( 3 ).toString();
      run.label   = dbase->value( 4 ).toString();
      run.date    = utcText( dbase->value( 5 ) );

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

      if ( ! queryOk( query, error ) )  return false;

      if ( dbase->next() )
      {
         run.projectID = dbase->value(  0 ).toString();
         run.id        = dbase->value(  1 ).toString();
         run.guid      = dbase->value(  2 ).toString();
         run.expType   = dbase->value(  8 ).toString();
         run.label     = dbase->value( 10 ).toString();
         run.date      = utcText( dbase->value( 13 ) );
      }

      if ( ! projGUIDs.contains( run.projectID ) )
      {
         QString pguid;
         QString pdesc;

         if ( run.projectID.toInt() > 0 )
         {
            query.clear();
            query << "get_project_info" << run.projectID;

            if ( ! queryOk( query, error ) )  return false;

            if ( dbase->next() )
            {
               pguid = dbase->value(  1 ).toString();
               pdesc = dbase->value( 10 ).toString();
            }
         }

         projGUIDs.insert( run.projectID, pguid );
         projDescs.insert( run.projectID, pdesc );
      }

      run.projectGUID = projGUIDs.value( run.projectID );
      run.projectDesc = projDescs.value( run.projectID );

      if ( ! proj_guid.isEmpty()  &&  run.projectGUID != proj_guid )  continue;

      if ( ! runIdIsUsable( run.runID ) )
      {
         run.usable  = false;
         run.problem = tr( "The run ID %1 is not one UltraScan accepts" )
                       .arg( run.runID );
      }

      runs << run;
   }

   return true;
}

void US_DataCatalog::buildDiskIndex( void )
{
   if ( disk_index->built )  return;

   disk_index->clear();

   emit message( tr( "Indexing the local data directory..." ) );

   QString     rdir    = US_Settings::resultDir();
   QStringList subdirs = QDir( rdir ).entryList(
                         QDir::AllDirs | QDir::NoDotAndDotDot, QDir::Name );

   for ( int ii = 0; ii < subdirs.size(); ii++ )
   {
      if ( cancelled() )  return;      // left unbuilt

      QString     subdir = rdir + "/" + subdirs[ ii ];
      QStringList files  = QDir( subdir ).entryList( QDir::Files, QDir::Name );

      DiskIndex::RunFiles rf;
      rf.dirPath = subdir;
      rf.dirName = subdirs[ ii ];

      QStringList xmls;

      for ( int jj = 0; jj < files.size(); jj++ )
      {
         QString name = files[ jj ];

         if      ( name.endsWith( ".auc"  ) )  rf.aucFiles << name;
         else if ( name.endsWith( ".tmst" ) )  rf.tmstFile =  name;
         else if ( name.endsWith( ".xml"  ) )  xmls        << name;
      }

      if ( rf.aucFiles.isEmpty() )
         rf.runID   = rf.dirName;   // only its edits can be of interest

      else
      {
         rf.runID   = runIdOfFile( rf.aucFiles.first(), false );
         rf.runType = rf.aucFiles.first().section( ".", -5, -5 );

         if ( rf.runID.isEmpty() )  rf.runID = rf.dirName;

         for ( int jj = 1; jj < rf.aucFiles.size(); jj++ )
            if ( runIdOfFile( rf.aucFiles[ jj ], false ) != rf.runID )
               rf.mixedRuns = true;
      }

      // Now that the run is known, the XML files can be told apart without
      // guessing: the experiment XML is runID.runType.xml and an edit file
      // is runID plus five more parts.
      QString expName = rf.runID + "." + rf.runType + ".xml";
      QString prefix  = rf.runID + ".";

      for ( int jj = 0; jj < xmls.size(); jj++ )
      {
         QString name = xmls[ jj ];

         if ( ! rf.aucFiles.isEmpty()  &&  name == expName )
         {
            rf.expXml = name;
            continue;
         }

         if ( ! name.startsWith( prefix ) )  continue;

         if ( name.mid( prefix.length() ).count( "." ) == 5 )
            rf.editFiles << name;
      }

      disk_index->runByDir.insert( rf.dirPath, disk_index->runs.size() );
      disk_index->runs << rf;
   }

   // The model and noise directories, peeked rather than parsed
   QString     mdir  = US_Settings::dataDir() + "/models";
   QStringList mfils = QDir( mdir ).entryList( QStringList( "M???????.xml" ),
                                               QDir::Files, QDir::Name );

   for ( int ii = 0; ii < mfils.size(); ii++ )
   {
      if ( cancelled() )  return;

      QString path  = mdir + "/" + mfils[ ii ];
      QMap< QString, QString > attrs = peek_attributes( path, "model" );

      if ( attrs.isEmpty() )  continue;

      DiskIndex::ModelFile mf;
      mf.path        = path;
      mf.base        = mfils[ ii ];
      mf.guid        = attrs.value( "modelGUID" );
      mf.editGUID    = attrs.value( "editGUID" );
      mf.description = attrs.value( "description" );

      int mx = disk_index->models.size();

      // An empty GUID is no key: it would join every record that lacks one
      if ( ! mf.editGUID.isEmpty() )
         disk_index->modelsByEdit.insert( mf.editGUID, mx );

      if ( ! mf.guid.isEmpty()  &&  ! disk_index->modelByGuid.contains( mf.guid ) )
         disk_index->modelByGuid.insert( mf.guid, mx );

      disk_index->models << mf;
   }

   QString     ndir  = US_Settings::dataDir() + "/noises";
   QStringList nfils = QDir( ndir ).entryList( QStringList( "N???????.xml" ),
                                               QDir::Files, QDir::Name );

   for ( int ii = 0; ii < nfils.size(); ii++ )
   {
      if ( cancelled() )  return;

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

      if ( ! nf.modelGUID.isEmpty() )
         disk_index->noisesByModel.insert( nf.modelGUID,
                                           disk_index->noises.size() );

      disk_index->noises << nf;
   }

   disk_index->built = true;

   emit message( tr( "Indexed %1 run directories, %2 model(s), %3 noise"
                     " record(s)" )
                 .arg( disk_index->runs.size() )
                 .arg( disk_index->models.size() )
                 .arg( disk_index->noises.size() ) );
}

namespace
{
   // The run a model or noise description names, by the longest dotted
   // prefix of it that is a run: "A.b.1A280.2dsa.model" belongs to run
   // "A.b" when there is one, and not also to run "A".
   QString run_of_description( const QString& description,
                               const QSet< QString >& runIDs )
   {
      QStringList parts = description.split( "." );

      for ( int kk = parts.size() - 1; kk > 0; kk-- )
      {
         QString prefix = QStringList( parts.mid( 0, kk ) ).join( "." );

         if ( runIDs.contains( prefix ) )  return prefix;
      }

      return QString();
   }
}

bool US_DataCatalog::loadRunsDisk( QList< Run >& runs, QString& error )
{
   QString rdir = US_Settings::resultDir();

   if ( ! QDir( rdir ).exists() )
   {  // Not an empty store: there is no store to list
      error = tr( "The local results directory does not exist: %1" )
              .arg( rdir );
      return false;
   }

   buildDiskIndex();

   if ( cancelledError( error ) )  return false;

   // The run IDs the store holds, and where each one is
   QSet< QString >                runIDs;
   QMultiHash< QString, QString > dirsOfRun;

   for ( const DiskIndex::RunFiles& rf : disk_index->runs )
   {
      if ( rf.aucFiles.isEmpty() )  continue;

      runIDs   .insert( rf.runID );
      dirsOfRun.insert( rf.runID, rf.dirName );
   }

   // Which models and noise records belong to a run can only be said for
   // certain once the edits are read, but their descriptions start with the
   // run identifier, which is enough for a count at this layer
   QHash< QString, int > modelKnt;
   QHash< QString, int > noiseKnt;

   for ( const DiskIndex::ModelFile& mf : disk_index->models )
   {
      QString runID = run_of_description( mf.description, runIDs );
      if ( ! runID.isEmpty() )  modelKnt[ runID ]++;
   }

   for ( const DiskIndex::NoiseFile& nf : disk_index->noises )
   {
      QString runID = run_of_description( nf.description, runIDs );
      if ( ! runID.isEmpty() )  noiseKnt[ runID ]++;
   }

   for ( const DiskIndex::RunFiles& rf : disk_index->runs )
   {
      if ( rf.aucFiles.isEmpty() )  continue;

      if ( cancelledError( error ) )  return false;

      Run run;
      run.runID     = rf.runID;
      run.runType   = rf.runType;
      run.dirPath   = rf.dirPath;
      run.label     = rf.runID;
      run.rawCount  = rf.aucFiles .size();
      run.editCount = rf.editFiles.size();

      if ( ! rf.expXml.isEmpty() )
      {
         QMap< QString, QMap< QString, QString > > elems =
            peek_elements( rf.dirPath + "/" + rf.expXml,
                           QStringList() << "experiment" << "project" );
         QMap< QString, QString > exp  = elems.value( "experiment" );
         QMap< QString, QString > proj = elems.value( "project" );

         run.id          = exp .value( "id", "-1" );
         run.guid        = exp .value( "guid" );
         run.expType     = exp .value( "type" );
         run.projectID   = proj.value( "id", "-1" );
         run.projectGUID = proj.value( "guid" );
         run.projectDesc = proj.value( "desc" );
      }

      run.date       = utc_of_file( rf.dirPath + "/" + rf.aucFiles.first() );
      run.modelCount = modelKnt.value( rf.runID );
      run.noiseCount = noiseKnt.value( rf.runID );

      // What would trip up a program that acts on this run
      QStringList problems;

      if ( ! runIdIsUsable( rf.runID ) )
         problems << tr( "The run ID %1 is not one UltraScan accepts" )
                     .arg( rf.runID );

      if ( rf.runID != rf.dirName )
         problems << tr( "The directory %1 holds the files of run %2" )
                     .arg( rf.dirName, rf.runID );

      if ( rf.mixedRuns )
         problems << tr( "The directory %1 holds the files of more than one"
                         " run" ).arg( rf.dirName );

      QStringList others = dirsOfRun.values( rf.runID );
      others.removeAll( rf.dirName );
      others.sort();

      if ( ! others.isEmpty() )
         problems << tr( "Run %1 is also in %2" )
                     .arg( rf.runID, others.join( ", " ) );

      if ( ! problems.isEmpty() )
      {
         run.usable  = false;
         run.problem = problems.join( "; " );
      }

      if ( ! proj_guid.isEmpty()  &&  run.projectGUID != proj_guid )  continue;

      runs << run;
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

   // Read aside, so a failure leaves the experiment as it was
   QList< Raw > raws;
   bool ok = isDb() ? loadDetailDb  ( run_list[ index ], raws, error )
                    : loadDetailDisk( run_list[ index ], raws, error );

   if ( ! ok )  return false;

   dropChecksums( raws );

   Run& run     = run_list[ index ];
   run.raws     = raws;
   run.verified = do_checksums;
   finishRun( run );
   pending.removeAll( index );

   error.clear();

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
      Raw& raw = run.raws[ ii ];
      raw.runID = run.runID;

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

bool US_DataCatalog::loadAll( QString& error )
{
   if ( run_list.isEmpty() )
   {
      orphan_list.clear();
      error.clear();
      return true;
   }

   QList< Run > fresh = run_list;
   Orphans      orphans;

   bool ok = isDb() ? loadAllDb  ( fresh, orphans, error )
                    : loadAllDisk( fresh, orphans, error );

   if ( ! ok )  return false;

   // Everything was read, so publish it.  An experiment already loaded --
   // and perhaps verified -- is kept as it is: what was read for it now
   // is the same chain, with less known about it.
   QList< int > loaded;

   for ( int ii = 0; ii < run_list.size(); ii++ )
   {
      if ( run_list[ ii ].isLoaded() )  continue;

      run_list[ ii ] = fresh[ ii ];
      finishRun( run_list[ ii ] );
      pending.removeAll( ii );
      loaded << ii;
   }

   if ( proj_guid.isEmpty() )
      orphan_list = orphans;
   else
      orphan_list.clear();

   for ( int ii = 0; ii < loaded.size(); ii++ )
      emit runLoaded( loaded[ ii ] );

   error.clear();

   return true;
}

const US_DataCatalog::Orphans& US_DataCatalog::orphans( void ) const
{
   return orphan_list;
}

/* Read the whole store at once.

   Asking for the chain one experiment at a time is four round trips per
   experiment.  The whole-store procedures list everything the investigator
   can see in four queries, and the records are attached to their parents
   here by GUID -- the same thing the local index does with the file system.
*/
bool US_DataCatalog::loadAllDb( QList< Run >& runs, Orphans& orphans,
                                QString& error )
{
   if ( dbase == nullptr )
   {
      error = tr( "No database connection" );
      return false;
   }

   QString invID = QString::number( inv_id );
   Flat    flat;
   bool    hashed = false;     // whether the triples came with checksums

   // ---- the triples and the edits ----------------------------------------
   bool done = false;

   if ( ( tiers() & StoreTier ) != 0 )
   {
      done = loadStoreDbNew( flat.raws, flat.edits, error );

      if ( ! done  &&  ( tiers() & StoreTier ) != 0 )  return false;
   }

   if ( ! done )
   {
      flat.raws .clear();
      flat.edits.clear();

      if ( ! loadStoreDbLegacy( flat.raws, flat.edits, error ) )  return false;

      hashed = true;            // the old procedures hash every blob
   }

   // ---- the models and the noise -----------------------------------------
   QStringList query;
   query << "get_model_desc" << invID;

   if ( ! queryOk( query, error ) )  return false;

   while ( dbase->next() )
      flat.models << model_row( dbase );

   query.clear();
   query << "get_noise_desc" << invID;

   if ( ! queryOk( query, error ) )  return false;

   QHash< QString, QList< Noise > > cache;

   while ( dbase->next() )
   {
      Noise noise = noise_row( dbase );

      flat.noises << noise;

      if ( ! noise.modelGUID.isEmpty() )
         cache[ noise.modelGUID ] << noise;
   }

   // ---- attach them ------------------------------------------------------
   flat.assemble( &orphans );

   QHash< QString, int > runById;

   for ( int ii = 0; ii < runs.size(); ii++ )
   {
      runs[ ii ].raws.clear();

      if ( ! runs[ ii ].id.isEmpty() )
         runById.insert( runs[ ii ].id, ii );
   }

   for ( int ii = 0; ii < flat.raws.size(); ii++ )
   {
      Raw raw = flat.raws[ ii ];
      int rx  = raw.experimentID.isEmpty()
                ? -1 : runById.value( raw.experimentID, -1 );

      if ( rx < 0 )
      {
         orphans.raws << raw;
         continue;
      }

      raw.runID = runs[ rx ].runID;

      for ( int jj = 0; jj < raw.edits.size(); jj++ )
         raw.edits[ jj ].runID = raw.runID;

      runs[ rx ].raws << raw;
   }

   for ( int ii = 0; ii < runs.size(); ii++ )
   {
      dropChecksums( runs[ ii ].raws );
      runs[ ii ].verified = hashed  &&  do_checksums;
   }

   dropChecksums( orphans.raws );

   noise_cache  = cache;
   noise_cached = true;

   error.clear();

   return true;
}

// The triples and edits from the whole-store catalog procedures, which do
// not hash the data blobs
bool US_DataCatalog::loadStoreDbNew( QList< Raw >& raws, QList< Edit >& edits,
                                     QString& error )
{
   QString     invID = QString::number( inv_id );
   QStringList query;
   query << "get_rawData_by_person" << invID;

   if ( ! queryOk( query, error ) )
   {
      if ( missingProcedure() )  dropTier( StoreTier, query[ 0 ] );
      return false;
   }

   while ( dbase->next() )
   {
      Raw raw = raw_row( dbase );
      raw.checksum.clear();      // not hashed by this procedure
      raw.size    .clear();
      raws << raw;
   }

   query.clear();
   query << "get_editedData_by_person" << invID;

   if ( ! queryOk( query, error ) )
   {
      if ( missingProcedure() )  dropTier( StoreTier, query[ 0 ] );
      return false;
   }

   while ( dbase->next() )
   {
      Edit edit = edit_row( dbase );
      edit.checksum.clear();
      edit.size    .clear();
      edits << edit;
   }

   return true;
}

/* The triples and edits from the whole-store procedures UltraScan has always
   had.  Each is one query, as before the catalog procedures existed, and
   both hash every data blob as they list it.

   all_rawDataIDs:    rawDataID, label, filename, experimentID, solutionID,
                      lastUpdated, checksum, size, personID, rawDataGUID,
                      comment, experimentGUID
   all_editedDataIDs: editedDataID, label, filename, rawDataID,
                      experimentID, lastUpdated, checksum, size,
                      experiment type, editGUID
*/
bool US_DataCatalog::loadStoreDbLegacy( QList< Raw >& raws,
                                        QList< Edit >& edits, QString& error )
{
   QString     invID = QString::number( inv_id );
   QStringList query;
   query << "all_rawDataIDs" << invID;

   if ( ! queryOk( query, error ) )  return false;

   QHash< QString, QString > guidOfRaw;     // rawDataID -> rawDataGUID

   while ( dbase->next() )
   {
      Raw raw;
      raw.id           = dbase->value(  0 ).toString();
      raw.filename     = base_name( dbase->value( 2 ).toString() );
      raw.experimentID = dbase->value(  3 ).toString();
      raw.solutionID   = dbase->value(  4 ).toString();
      raw.lastUpdated  = utcText( dbase->value( 5 ) );
      raw.checksum     = dbase->value(  6 ).toString();
      raw.size         = dbase->value(  7 ).toString();
      raw.guid         = dbase->value(  9 ).toString();
      raw.description  = dbase->value( 10 ).toString();
      raw_names( raw );

      if ( ! raw.guid.isEmpty() )  guidOfRaw.insert( raw.id, raw.guid );

      raws << raw;
   }

   query.clear();
   query << "all_editedDataIDs" << invID;

   if ( ! queryOk( query, error ) )  return false;

   while ( dbase->next() )
   {
      Edit edit;
      edit.id          = dbase->value( 0 ).toString();
      edit.filename    = base_name( dbase->value( 2 ).toString() );
      edit.lastUpdated = utcText( dbase->value( 5 ) );
      edit.checksum    = dbase->value( 6 ).toString();
      edit.size        = dbase->value( 7 ).toString();
      edit.guid        = dbase->value( 9 ).toString();

      // This procedure names the triple by its ID, not its GUID
      edit.rawGUID     = guidOfRaw.value( dbase->value( 3 ).toString() );
      edit_names( edit );

      edits << edit;
   }

   return true;
}

/* Read the whole local store: the one pass loadRunDetail makes, for every
   run, and then what that pass did not reach.
*/
bool US_DataCatalog::loadAllDisk( QList< Run >& runs, Orphans& orphans,
                                  QString& error )
{
   buildDiskIndex();

   QSet< QString > attachedEdits;

   for ( int ii = 0; ii < runs.size(); ii++ )
   {
      if ( cancelledError( error ) )  return false;

      if ( ! runs[ ii ].isLoaded() )
      {
         QList< Raw > raws;

         if ( ! loadDetailDisk( runs[ ii ], raws, error ) )  return false;

         runs[ ii ].raws     = raws;
         runs[ ii ].verified = do_checksums;
      }

      for ( const Raw& raw : runs[ ii ].raws )
         for ( const Edit& edit : raw.edits )
            if ( ! edit.guid.isEmpty() )  attachedEdits.insert( edit.guid );
   }

   if ( ! proj_guid.isEmpty() )
   {
      error.clear();
      return true;
   }

   // ---- edits with no triple ---------------------------------------------
   // in a run directory with no .auc file, or with none of their triple
   QList< Edit >         loose;
   QHash< QString, int > looseByGuid;

   for ( const DiskIndex::RunFiles& rf : disk_index->runs )
   {
      QSet< QString > triples;

      for ( const QString& auc : rf.aucFiles )
         triples.insert( triple_key( auc ) );

      for ( const QString& name : rf.editFiles )
      {
         if ( triples.contains( triple_key( name ) ) )  continue;

         if ( cancelledError( error ) )  return false;

         Edit edit;
         edit.filename = name;
         edit.path     = rf.dirPath + "/" + name;
         edit_names( edit );

         peek_edit_guids( edit.path, edit.guid, edit.rawGUID );

         if ( ! edit.guid.isEmpty()  &&  attachedEdits.contains( edit.guid ) )
            continue;           // a copy of an edit that is attached

         fileDigest( edit.path, edit.checksum, edit.size );
         edit.lastUpdated = utc_of_file( edit.path );

         if ( ! edit.guid.isEmpty()  &&  ! looseByGuid.contains( edit.guid ) )
            looseByGuid.insert( edit.guid, loose.size() );

         loose << edit;
      }
   }

   // ---- models with no edit in the chain ---------------------------------
   QSet< QString > chainModels;         // GUIDs of models in the chain

   for ( const DiskIndex::ModelFile& mf : disk_index->models )
      if ( ! mf.editGUID.isEmpty()  &&  attachedEdits.contains( mf.editGUID )
           &&  ! mf.guid.isEmpty() )
         chainModels.insert( mf.guid );

   QList< Model > models;

   for ( int ii = 0; ii < disk_index->models.size(); ii++ )
   {
      const DiskIndex::ModelFile& mf = disk_index->models[ ii ];

      if ( ! mf.editGUID.isEmpty()  &&  attachedEdits.contains( mf.editGUID ) )
         continue;

      Model model;
      model.guid        = mf.guid;
      model.editGUID    = mf.editGUID;
      model.description = mf.description;
      model.filename    = mf.base;
      model.path        = mf.path;
      model.subType     = mf.description.section( ".", -2, -2 );
      model.lastUpdated = utc_of_file( mf.path );
      fileDigest( mf.path, model.checksum, model.size );

      models << model;
   }

   // ---- noise with no model in the chain ---------------------------------
   // goes to the model outside the chain it belongs to, if there is one
   QList< Noise > noises;

   for ( const DiskIndex::NoiseFile& nf : disk_index->noises )
   {
      if ( ! nf.modelGUID.isEmpty()  &&  chainModels.contains( nf.modelGUID ) )
         continue;              // in the chain already

      Noise noise;
      noise.guid        = nf.guid;
      noise.modelGUID   = nf.modelGUID;
      noise.description = nf.description;
      noise.noiseType   = nf.noiseType;
      noise.filename    = nf.base;
      noise.path        = nf.path;
      noise.lastUpdated = utc_of_file( nf.path );
      fileDigest( nf.path, noise.checksum, noise.size );

      noises << noise;
   }

   if ( cancelledError( error ) )  return false;

   attach_models( models, noises, &orphans.noises );
   attach_edits ( loose,  models, &orphans.models );
   orphans.edits = loose;

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

   if ( run_list[ index ].verified )
   {
      verify_queue.removeAll( index );
      error.clear();
      return true;
   }

   bool ok = true;

   if ( ! run_list[ index ].isLoaded() )
   {  // Nothing is read yet, so read it with its checksums
      bool saved   = do_checksums;
      do_checksums = true;
      ok           = loadRunDetail( index, error );
      do_checksums = saved;
   }

   else
   {
      Run copy = run_list[ index ];

      ok = isDb() ? verifyRunDb( copy, error ) : verifyRunDisk( copy, error );

      if ( ok )  run_list[ index ] = copy;
   }

   verify_queue.removeAll( index );

   if ( ! ok )  return false;

   run_list[ index ].verified = true;
   error.clear();

   emit runVerified( index );

   return true;
}

/* Read the checksums of one experiment from the database.

   This is what the whole-store listing left out, and it is the expensive
   half: the server hashes the experiment's raw-data and edit blobs to
   answer it.  Models and noise carry their checksums already -- their
   payloads are small XML, so the listing hashes those as it goes.
*/
bool US_DataCatalog::verifyRunDb( Run& run, QString& error )
{
   if ( dbase == nullptr )
   {
      error = tr( "No database connection" );
      return false;
   }

   if ( ( tiers() & ExperimentTier ) == 0 )
      return verifyRunDbLegacy( run, error );

   QHash< QString, QPair< QString, QString > > digests;   // GUID -> md5, size

   QStringList query;
   query << "get_rawData_by_experiment" << run.id;

   if ( ! queryOk( query, error ) )
   {
      if ( ! missingProcedure() )  return false;

      dropTier( ExperimentTier, query[ 0 ] );
      return verifyRunDbLegacy( run, error );
   }

   while ( dbase->next() )
      digests.insert( dbase->value( 1 ).toString(),
                      qMakePair( dbase->value(  9 ).toString(),
                                 dbase->value( 10 ).toString() ) );

   query.clear();
   query << "get_editedData_by_experiment" << run.id;

   if ( ! queryOk( query, error ) )
   {
      if ( ! missingProcedure() )  return false;

      dropTier( ExperimentTier, query[ 0 ] );
      return verifyRunDbLegacy( run, error );
   }

   while ( dbase->next() )
      digests.insert( dbase->value( 3 ).toString(),
                      qMakePair( dbase->value( 8 ).toString(),
                                 dbase->value( 9 ).toString() ) );

   for ( int ii = 0; ii < run.raws.size(); ii++ )
   {
      Raw& raw = run.raws[ ii ];

      if ( ! raw.guid.isEmpty()  &&  digests.contains( raw.guid ) )
      {
         raw.checksum = digests.value( raw.guid ).first;
         raw.size     = digests.value( raw.guid ).second;
      }

      for ( int jj = 0; jj < raw.edits.size(); jj++ )
      {
         Edit& edit = raw.edits[ jj ];

         if ( edit.guid.isEmpty()  ||  ! digests.contains( edit.guid ) )
            continue;

         edit.checksum = digests.value( edit.guid ).first;
         edit.size     = digests.value( edit.guid ).second;
      }
   }

   error.clear();

   return true;
}

// The same from an older server, whose per-record procedures report the
// checksum of the one record they read
bool US_DataCatalog::verifyRunDbLegacy( Run& run, QString& error )
{
   QStringList query;

   for ( int ii = 0; ii < run.raws.size(); ii++ )
   {
      Raw& raw = run.raws[ ii ];

      if ( raw.id.toInt() > 0 )
      {
         query.clear();
         query << "get_rawData" << raw.id;

         if ( ! queryOk( query, error ) )  return false;

         if ( dbase->next() )
         {
            raw.checksum = dbase->value( 8 ).toString();
            raw.size     = dbase->value( 9 ).toString();
         }
      }

      for ( int jj = 0; jj < raw.edits.size(); jj++ )
      {
         Edit& edit = raw.edits[ jj ];

         if ( edit.id.toInt() < 1 )  continue;

         query.clear();
         query << "get_editedData" << edit.id;

         if ( ! queryOk( query, error ) )  return false;

         if ( dbase->next() )
         {
            edit.checksum = dbase->value( 6 ).toString();
            edit.size     = dbase->value( 7 ).toString();
         }
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
      if ( cancelledError( error ) )
      {
         do_checksums = saved;
         return false;
      }

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

// ------------------------------------------------- one experiment, database

bool US_DataCatalog::loadDetailDb( const Run& run, QList< Raw >& raws,
                                   QString& error )
{
   if ( dbase == nullptr )
   {
      error = tr( "No database connection" );
      return false;
   }

   if ( ( tiers() & ExperimentTier ) != 0 )
   {
      if ( loadDetailDbBulk( run, raws, error ) )  return true;

      if ( ( tiers() & ExperimentTier ) != 0 )  return false;  // real error

      raws.clear();
   }

   return loadDetailDbLegacy( run, raws, error );
}

/* One experiment in four queries.  Any one of them may turn out to be
   missing on a partly upgraded server, and then the experiment is read the
   old way instead -- this one and every one after it.
*/
bool US_DataCatalog::loadDetailDbBulk( const Run& run, QList< Raw >& raws,
                                       QString& error )
{
   QString     invID = QString::number( inv_id );
   QStringList query;
   Flat        flat;

   // ---- the triples ------------------------------------------------------
   query << "get_rawData_by_experiment" << run.id;

   if ( ! queryOk( query, error ) )
   {
      if ( missingProcedure() )  dropTier( ExperimentTier, query[ 0 ] );
      return false;
   }

   while ( dbase->next() )
      flat.raws << raw_row( dbase );

   // ---- the edits --------------------------------------------------------
   query.clear();
   query << "get_editedData_by_experiment" << run.id;

   if ( ! queryOk( query, error ) )
   {
      if ( missingProcedure() )  dropTier( ExperimentTier, query[ 0 ] );
      return false;
   }

   while ( dbase->next() )
      flat.edits << edit_row( dbase );

   // ---- the models -------------------------------------------------------
   query.clear();
   query << "get_model_desc_by_experiment" << invID << run.id;

   if ( ! queryOk( query, error ) )
   {
      if ( missingProcedure() )  dropTier( ExperimentTier, query[ 0 ] );
      return false;
   }

   while ( dbase->next() )
      flat.models << model_row( dbase );

   // ---- the noise records ------------------------------------------------
   query.clear();
   query << "get_noise_desc_by_experiment" << invID << run.id;

   if ( ! queryOk( query, error ) )
   {
      if ( missingProcedure() )  dropTier( ExperimentTier, query[ 0 ] );
      return false;
   }

   while ( dbase->next() )
      flat.noises << noise_row( dbase );

   // A record whose parent is not in this experiment is not this
   // experiment's; only a whole-store read can call it an orphan
   flat.assemble( nullptr );

   for ( int ii = 0; ii < flat.raws.size(); ii++ )
   {
      flat.raws[ ii ].runID = run.runID;

      for ( int jj = 0; jj < flat.raws[ ii ].edits.size(); jj++ )
         flat.raws[ ii ].edits[ jj ].runID = run.runID;
   }

   raws = flat.raws;

   return true;
}

// One experiment the way UltraScan always read it: a query per record
bool US_DataCatalog::loadDetailDbLegacy( const Run& run, QList< Raw >& raws,
                                         QString& error )
{
   QString     invID = QString::number( inv_id );
   QStringList query;
   QStringList rawIDs;
   QStringList rawNames;

   query << "get_rawDataIDs" << run.id;

   if ( ! queryOk( query, error ) )  return false;

   while ( dbase->next() )
   {
      rawIDs   << dbase->value( 0 ).toString();
      rawNames << base_name( dbase->value( 2 ).toString() );
   }

   QList< Raw > list;

   for ( int ii = 0; ii < rawIDs.size(); ii++ )
   {
      Raw raw;
      raw.id           = rawIDs  [ ii ];
      raw.filename     = rawNames[ ii ];
      raw.experimentID = run.id;
      raw_names( raw );
      raw.runID        = run.runID;

      query.clear();
      query << "get_rawData" << raw.id;

      if ( ! queryOk( query, error ) )  return false;

      if ( dbase->next() )
      {
         raw.guid        = dbase->value( 0 ).toString();
         raw.description = dbase->value( 3 ).toString();
         raw.solutionID  = dbase->value( 5 ).toString();
         raw.lastUpdated = utcText( dbase->value( 7 ) );
         raw.checksum    = dbase->value( 8 ).toString();
         raw.size        = dbase->value( 9 ).toString();
      }

      QStringList edtIDs;

      query.clear();
      query << "get_editedDataIDs" << raw.id;

      if ( ! queryOk( query, error ) )  return false;

      while ( dbase->next() )
         edtIDs << dbase->value( 0 ).toString();

      for ( int jj = 0; jj < edtIDs.size(); jj++ )
      {
         Edit edit;
         edit.id      = edtIDs[ jj ];
         edit.rawGUID = raw.guid;

         query.clear();
         query << "get_editedData" << edit.id;

         if ( ! queryOk( query, error ) )  return false;

         if ( ! dbase->next() )  continue;

         edit.guid        = dbase->value( 1 ).toString();
         edit.filename    = base_name( dbase->value( 3 ).toString() );
         edit.lastUpdated = utcText( dbase->value( 5 ) );
         edit.checksum    = dbase->value( 6 ).toString();
         edit.size        = dbase->value( 7 ).toString();
         edit_names( edit );
         edit.runID       = run.runID;

         QList< Model > models;

         query.clear();
         query << "get_model_desc_by_editID" << invID << edit.id;

         if ( ! queryOk( query, error ) )  return false;

         while ( dbase->next() )
         {
            Model model = model_row( dbase );

            if ( model.editGUID.isEmpty() )  model.editGUID = edit.guid;

            models << model;
         }

         query.clear();
         query << "get_noise_desc_by_editID" << invID << edit.id;

         if ( ! queryOk( query, error ) )  return false;

         QList< Noise > noises;

         while ( dbase->next() )
         {
            Noise noise    = noise_row( dbase );
            noise.editGUID = edit.guid;

            noises << noise;
         }

         attach_models( models, noises, nullptr );

         edit.models = models;
         raw.edits  << edit;
      }

      list << raw;
   }

   raws = list;

   return true;
}

// ------------------------------------------------- one experiment, disk

bool US_DataCatalog::loadDetailDisk( const Run& run, QList< Raw >& raws,
                                     QString& error )
{
   buildDiskIndex();

   if ( cancelledError( error ) )  return false;

   int rfx = disk_index->runByDir.value( run.dirPath, -1 );

   if ( rfx < 0 )
   {
      error = tr( "The run directory of %1 is no longer there" )
              .arg( run.runID );
      return false;
   }

   const DiskIndex::RunFiles& rf = disk_index->runs[ rfx ];

   // Bucket this run's edit files by triple, once, rather than asking the
   // file system for them again for every triple
   QMultiHash< QString, QString > editsOfTriple;

   for ( int ii = 0; ii < rf.editFiles.size(); ii++ )
   {
      QString name = rf.editFiles[ ii ];
      editsOfTriple.insert( triple_key( name ), name );
   }

   QList< Raw > list;

   for ( int ii = 0; ii < rf.aucFiles.size(); ii++ )
   {
      if ( cancelledError( error ) )  return false;

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

      QStringList edits = editsOfTriple.values( triple_key( aucName ) );
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
         QList< int > mdlxs = disk_index->modelsOf( edit.guid );

         for ( int kk = 0; kk < mdlxs.size(); kk++ )
         {
            const DiskIndex::ModelFile& mf = disk_index->models[ mdlxs[ kk ] ];

            Model model;
            model.guid        = mf.guid;
            model.editGUID    = mf.editGUID;
            model.description = mf.description;
            model.filename    = mf.base;
            model.path        = mf.path;
            model.subType     = mf.description.section( ".", -2, -2 );

            fileDigest( mf.path, model.checksum, model.size );
            model.lastUpdated = utc_of_file( mf.path );

            QList< int > noixs = disk_index->noisesOf( mf.guid );

            for ( int mm = 0; mm < noixs.size(); mm++ )
            {
               const DiskIndex::NoiseFile& nf = disk_index->noises[ noixs[ mm ] ];

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
            }

            edit.models << model;
         }

         raw.edits << edit;
      }

      list << raw;
   }

   raws = list;

   error.clear();

   return true;
}
