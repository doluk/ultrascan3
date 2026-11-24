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
      return QLocale::c().toString( when.toUTC(), "yyyy-MM-dd hh:mm:ss" )
             + " UTC";
   }

   QString utc_of_file( const QString& path )
   {
      return utc_text( QFileInfo( path ).lastModified() );
   }

   // The width of the model and noise description columns:
   // model.description and noise.description are VARCHAR(160) (us3.sql),
   // and a longer description is cut there
   const int desc_width = 160;

   // A database ID that names a record: the procedures give 0 or nothing
   // for none, and the catalog "-1"
   bool real_id( const QString& id )
   {
      return id.toInt() > 0;
   }

   // A database ID as the catalog gives it: the ID, or "-1" for none --
   // the procedures give NULL or 0 for none, which read as "" and "0"
   QString db_id( const QVariant& value )
   {
      const QString text = value.toString().trimmed();
      return real_id( text ) ? text : QString( "-1" );
   }

   // A noise record written with no description takes its model's, with
   // the noise type in place of ".model" -- the name UltraScan gives it
   QString noise_description_of( const QString& modelDescription,
                                 const QString& noiseType )
   {
      if ( modelDescription.isEmpty() )  return QString();

      QString ntype = ( noiseType == "ri" ) ? "ri_noise" : "ti_noise";
      return QString( modelDescription ).replace( ".model", "." + ntype );
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

   // The attributes of the first element of a name in an XML text
   QString peek_xml_attribute( const QString& text, const QString& element,
                               const QString& attribute )
   {
      QXmlStreamReader xml( text );

      while ( ! xml.atEnd() )
      {
         xml.readNext();

         if ( xml.isStartElement()  &&  xml.name().toString() == element )
            return xml.attributes().value( attribute ).toString();
      }

      return QString();
   }

   // How the platform compares file names: a run directory "runA" holding
   // RunA.*.auc is the right directory where "RunA" and "runA" are one name
   Qt::CaseSensitivity platform_file_case( void )
   {
#if defined( Q_OS_WIN ) || defined( Q_OS_MACOS )
      return Qt::CaseInsensitive;
#else
      return Qt::CaseSensitive;
#endif
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

US_DataCatalog::Noise::Noise()  { id = "-1"; modelID = "-1"; }
US_DataCatalog::Model::Model()  { id = "-1"; }
US_DataCatalog::Edit::Edit()    { id = "-1"; rawID = "-1"; }
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

      Database records also carry their parent's ID.  A record whose parent
      GUID is empty is attached by that ID, so every tier attaches it the
      same way -- the per-record procedures always found it under the
      parent they were asked about.  Noise is attached by modelID first:
      that is the column every noise listing joins modelPerson on, so the
      server has vouched for it, while noise.modelGUID may be null.
   */

   // Where each key is first found, and where each ID is
   class ParentIndex
   {
      public:
         QHash< QString, int > byGuid;
         QHash< QString, int > byId;

         void add( const QString& guid, const QString& id, int ix )
         {
            if ( ! guid.isEmpty()  &&  ! byGuid.contains( guid ) )
               byGuid.insert( guid, ix );

            if ( real_id( id )  &&  ! byId.contains( id ) )
               byId.insert( id, ix );
         }

         int byGuidOrId( const QString& guid, const QString& id ) const
         {
            if ( ! guid.isEmpty() )  return byGuid.value( guid, -1 );
            if ( real_id( id ) )     return byId  .value( id,   -1 );
            return -1;
         }

         int byIdOrGuid( const QString& id, const QString& guid ) const
         {
            int ix = real_id( id ) ? byId.value( id, -1 ) : -1;
            return ( ix >= 0  ||  guid.isEmpty() ) ? ix
                                                   : byGuid.value( guid, -1 );
         }
   };

   void attach_models( QList< Model >& models, QList< Noise >& noises,
                       QList< Noise >* orphans )
   {
      ParentIndex parents;

      for ( int ii = 0; ii < models.size(); ii++ )
         parents.add( models[ ii ].guid, models[ ii ].id, ii );

      for ( int ii = 0; ii < noises.size(); ii++ )
      {
         Noise noise = noises[ ii ];
         int   mx    = parents.byIdOrGuid( noise.modelID, noise.modelGUID );

         if ( mx < 0 )
         {
            if ( orphans != nullptr )  *orphans << noise;
            continue;
         }

         if ( noise.modelGUID.isEmpty() )
            noise.modelGUID = models[ mx ].guid;

         if ( noise.editGUID.isEmpty() )
            noise.editGUID = models[ mx ].editGUID;

         models[ mx ].noises << noise;
      }
   }

   void attach_edits( QList< Edit >& edits, QList< Model >& models,
                      QList< Model >* orphans )
   {
      ParentIndex parents;

      for ( int ii = 0; ii < edits.size(); ii++ )
         parents.add( edits[ ii ].guid, edits[ ii ].id, ii );

      for ( int ii = 0; ii < models.size(); ii++ )
      {
         int ex = parents.byGuidOrId( models[ ii ].editGUID,
                                      models[ ii ].editID );

         if ( ex < 0 )
         {
            if ( orphans != nullptr )  *orphans << models[ ii ];
            continue;
         }

         if ( models[ ii ].editGUID.isEmpty() )
            models[ ii ].editGUID = edits[ ex ].guid;

         for ( int nn = 0; nn < models[ ii ].noises.size(); nn++ )
            if ( models[ ii ].noises[ nn ].editGUID.isEmpty() )
               models[ ii ].noises[ nn ].editGUID = edits[ ex ].guid;

         edits[ ex ].models << models[ ii ];
      }
   }

   void attach_raws( QList< Raw >& raws, QList< Edit >& edits,
                     QList< Edit >* orphans )
   {
      ParentIndex parents;

      for ( int ii = 0; ii < raws.size(); ii++ )
         parents.add( raws[ ii ].guid, raws[ ii ].id, ii );

      for ( int ii = 0; ii < edits.size(); ii++ )
      {
         int rx = parents.byGuidOrId( edits[ ii ].rawGUID, edits[ ii ].rawID );

         if ( rx < 0 )
         {
            if ( orphans != nullptr )  *orphans << edits[ ii ];
            continue;
         }

         if ( edits[ ii ].rawGUID.isEmpty() )
            edits[ ii ].rawGUID = raws[ rx ].guid;

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

   // A model's description by its GUID and by its ID, and its GUID by its
   // ID, for noise that needs its model's description or GUID
   void add_model_description( QHash< QString, QString >& descs,
                               const Model& model )
   {
      if ( ! model.guid.isEmpty() )
         descs.insert( model.guid, model.description );

      if ( real_id( model.id ) )
      {
         descs.insert( "id:"      + model.id, model.description );
         descs.insert( "guid-of:" + model.id, model.guid );
      }
   }

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

   // A model row of get_model_desc and the procedures like it
   // (us3_model_procs.sql; get_model_desc_by_experiment in
   // us3_catalog_procs.sql has the same layout): modelID, modelGUID,
   // description, variance, meniscus, editGUID, editedDataID, lastUpdated,
   // checksum, size
   Model model_row( IUS_DB2* db )
   {
      Model model;
      model.id          = db_id( db->value( 0 ) );
      model.guid        = db->value( 1 ).toString();
      model.description = db->value( 2 ).toString();
      model.editGUID    = db->value( 5 ).toString();
      model.editID      = db_id( db->value( 6 ) );
      model.lastUpdated = US_DataCatalog::utcText( db->value( 7 ) );
      model.checksum    = db->value( 8 ).toString();
      model.size        = db->value( 9 ).toString();
      model.subType     = US_DataCatalog::modelSubType( model.description );
      return model;
   }

   // A noise row of get_noise_desc and the procedures like it
   // (us3_noise_procs.sql; get_noise_desc_by_experiment in
   // us3_catalog_procs.sql has the same layout): noiseID, noiseGUID,
   // editedDataID, modelID, noiseType, modelGUID, timeEntered, checksum,
   // size, description
   Noise noise_row( IUS_DB2* db )
   {
      Noise noise;
      noise.id          = db_id( db->value( 0 ) );
      noise.guid        = db->value( 1 ).toString();
      noise.modelID     = db_id( db->value( 3 ) );
      noise.noiseType   = db->value( 4 ).toString().left( 2 );
      noise.modelGUID   = db->value( 5 ).toString();
      noise.lastUpdated = US_DataCatalog::utcText( db->value( 6 ) );
      noise.checksum    = db->value( 7 ).toString();
      noise.size        = db->value( 8 ).toString();
      noise.description = db->value( 9 ).toString();
      return noise;
   }

   // A triple row of the catalog procedures get_rawData_by_experiment and
   // get_rawData_by_person (us3_catalog_procs.sql): rawDataID, rawDataGUID,
   // label, filename, comment, experimentID, solutionID, channelID,
   // lastUpdated, checksum, size
   Raw raw_row( IUS_DB2* db )
   {
      Raw raw;
      raw.id           = db_id( db->value(  0 ) );
      raw.guid         = db->value(  1 ).toString();
      raw.filename     = base_name( db->value( 3 ).toString() );
      raw.description  = db->value(  4 ).toString();
      raw.experimentID = db_id( db->value(  5 ) );
      raw.solutionID   = db_id( db->value(  6 ) );
      raw.lastUpdated  = US_DataCatalog::utcText( db->value( 8 ) );
      raw.checksum     = db->value(  9 ).toString();
      raw.size         = db->value( 10 ).toString();
      raw_names( raw );
      return raw;
   }

   // An edit row of the catalog procedures get_editedData_by_experiment and
   // get_editedData_by_person (us3_catalog_procs.sql): editedDataID,
   // rawDataID, rawDataGUID, editGUID, label, filename, comment,
   // lastUpdated, checksum, size
   Edit edit_row( IUS_DB2* db )
   {
      Edit edit;
      edit.id          = db_id( db->value( 0 ) );
      edit.rawID       = db_id( db->value( 1 ) );
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
   listed       = false;
   cancel_flag  = nullptr;
   file_case    = platform_file_case();
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
   load_tries  .clear();
   verify_tries.clear();
   foreign_noise.clear();
   orphan_list .clear();
   noise_cache .clear();
   desc_tried  .clear();
   noise_cached = false;
   listed       = false;
   modelsChanged();

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
   {  // The index of the local store already knows which noise is whose,
      // once it is built: a build that was cancelled is not an answer
      if ( ! diskIndexReady( error ) )  return list;

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

         if ( ! fileDigest( nf.path, noise.checksum, noise.size, error ) )
            return QList< Noise >();

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

   // Which model a record is of is worked out from the models read by now:
   // by modelID, as the chain attaches it, when that model has been read,
   // else by the record's modelGUID.  Models read after the noise was
   // listed count too: reading them drops the index, which is built again
   // here, once, and not at every call.
   const ModelIndex& index     = modelIndex();
   const QString     editGUID  = index.editOf.value( modelGUID );
   const QString     modelDesc = index.descs.value( modelGUID );

   for ( int nx : index.noiseOf.value( modelGUID ) )
   {
      Noise noise     = noise_cache[ nx ];
      noise.modelGUID = modelGUID;
      noise.editGUID  = editGUID;

      if ( noise.description.isEmpty() )
         noise.description = noise_description_of( modelDesc, noise.noiseType );

      list << noise;
   }

   return list;
}

bool US_DataCatalog::cacheNoise( QString& error )
{
   if ( noise_cached )  return true;

   QStringList query;
   query << "get_noise_desc" << QString::number( inv_id );

   if ( ! queryOk( query, error ) )  return false;

   QList< Noise > noises;

   while ( dbase->next() )
      noises << noise_row( dbase );

   // Descriptions that were cut are read in full now; one that is missing
   // is given its model's when it is looked up, since the model may not
   // have been read yet
   repairNoises( noises, QHash< QString, QString >() );

   noise_cache  = noises;
   noise_cached = true;
   noiseCacheChanged();

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

      if ( ! diskIndexReady( error ) )  return list;

      // The noise of an edit is the noise of every model of that edit
      QList< int > mdlxs = disk_index->modelsOf( editGUID );

      for ( int ii = 0; ii < mdlxs.size(); ii++ )
      {
         const DiskIndex::ModelFile& mf = disk_index->models[ mdlxs[ ii ] ];

         QList< Noise > found = noisesOfModel( mf.guid, error );

         if ( ! error.isEmpty() )  return QList< Noise >();

         list << found;
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

   // Only what this answer needs: the models' descriptions are the ones
   // read, and repaired, with them, so they are looked up, not read again
   bool needModels = false;

   for ( const Noise& noise : list )
      if ( noise.description.isEmpty() )  needModels = true;

   repairNoises( list, needModels ? modelIndex().descs
                                  : QHash< QString, QString >() );

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

bool US_DataCatalog::fileDigest( const QString& path, QString& checksum,
                                 QString& size, QString& error )
{
   checksum.clear();
   size    .clear();

   if ( ! do_checksums )  return true;

   // "0 0" is how md5sum_file says the file could not be read; an empty
   // file has the MD5 of nothing and size 0.  A checksum of 0 would compare
   // unequal to every copy and say only that the two differ.
   QString contents = US_Util::md5sum_file( path );

   if ( contents == "0 0" )
   {
      error = tr( "Could not read %1 to compute its checksum" ).arg( path );
      return false;
   }

   checksum = contents.section( " ", 0, 0 );
   size     = contents.section( " ", 1, 1 );

   return true;
}

bool US_DataCatalog::diskIndexReady( QString& error )
{
   buildDiskIndex();

   if ( disk_index->built )  return true;

   if ( ! cancelledError( error ) )
      error = tr( "The local data directory could not be indexed" );

   return false;
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

   // A caller holding positions into the listing must hear that they are
   // gone; loadAll() would otherwise empty it without a word
   emit listingDropped();

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

   // The source's own idea of one name: the database's runID column does
   // not tell case apart, and a local store does where its file system does
   Qt::CaseSensitivity cs = isDb() ? Qt::CaseInsensitive : file_case;

   if ( cs == Qt::CaseSensitive )  return -1;

   for ( int ii = 0; ii < run_list.size(); ii++ )
      if ( run_list[ ii ].runID.compare( runID, cs ) == 0 )  return ii;

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

      if ( loadRunDetail( next, error ) )  return true;

      // A read can fail for a reason that passes -- a dropped connection,
      // a file being written -- so it is tried once more, after the rest
      if ( load_tries.value( next ) < 1 )
      {
         load_tries[ next ]++;
         pending.removeAll( next );
         pending << next;
         error += " " + tr( "(%1 will be read again later)" )
                        .arg( run_list[ next ].runID );
      }

      emit message( error );
      return false;
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

   // The value is a UTC date and a UTC time, and is put together as one
   // directly: parsed as a local time and relabelled, a time that does not
   // exist locally -- the spring-forward gap -- would come out shifted
   QDate date;
   QTime time;

   if ( value.userType() == QMetaType::QDateTime )
   {
      QDateTime when = value.toDateTime();

      if ( when.timeSpec() != Qt::LocalTime )  when = when.toUTC();

      date = when.date();
      time = when.time();
   }

   else
   {  // "yyyy-MM-dd hh:mm:ss", with a 'T' or " UTC" now and then
      QString stamp = text;

      if ( stamp.endsWith( " UTC" ) )  stamp.chop( 4 );
      if ( stamp.endsWith( 'Z' ) )     stamp.chop( 1 );

      if ( stamp.length() >= 19  &&
           ( stamp.at( 10 ) == ' '  ||  stamp.at( 10 ) == 'T' ) )
      {
         date = QDate::fromString( stamp.left( 10 ),   "yyyy-MM-dd" );
         time = QTime::fromString( stamp.mid( 11, 8 ), "hh:mm:ss" );
      }
   }

   if ( ! date.isValid()  ||  ! time.isValid() )
      return US_Util::toUTCDatetimeText( text, true );

   return QLocale::c().toString( date, "yyyy-MM-dd" ) + " "
          + QLocale::c().toString( time, "hh:mm:ss" ) + " UTC";
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
   load_tries  .clear();
   verify_tries.clear();
   foreign_noise.clear();
   orphan_list .clear();
   noise_cache .clear();
   desc_tried  .clear();
   noise_cached = false;
   listed       = true;
   modelsChanged();

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
      run.id          = db_id( dbase->value(  0 ) );
      run.guid        = dbase->value(  1 ).toString();
      run.runID       = dbase->value(  2 ).toString();
      run.expType     = dbase->value(  3 ).toString();
      run.runType     = dbase->value(  4 ).toString();
      run.label       = dbase->value(  5 ).toString();
      run.projectID   = db_id( dbase->value(  6 ) );
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

/* The listing from the procedures UltraScan has always had.

   get_experiment_desc names each experiment by its experimentID, and that ID
   is what identifies it from here on: two experiments of one investigator
   can share a run ID, so looking one up by run ID would give both the first
   one's ID and GUID, and the records of the other would then belong to no
   experiment.  The rest comes from get_experiment_info, by ID:

   get_experiment_desc: experimentID, runID, type, runType, and for an
                        administrator also label, lastUpdated
   get_experiment_info: experimentGUID, projectID, runID, labID,
                        instrumentID, operatorID, rotorID, rotorCalibrationID,
                        type, runTemp, label, comment, centrifugeProtocol,
                        lastUpdated, personID, runType, RIProfile
*/
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
      run.id      = db_id( dbase->value( 0 ) );
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
      query << "get_experiment_info" << run.id;

      if ( ! queryOk( query, error ) )  return false;

      if ( dbase->next() )
      {
         QString type  = dbase->value(  8 ).toString();
         QString label = dbase->value( 10 ).toString();
         QString date  = utcText( dbase->value( 13 ) );

         run.guid      = dbase->value(  0 ).toString();
         run.projectID = db_id( dbase->value(  1 ) );

         if ( ! type .isEmpty() )  run.expType = type;
         if ( ! label.isEmpty() )  run.label   = label;
         if ( ! date .isEmpty() )  run.date    = date;
      }


      if ( ! projGUIDs.contains( run.projectID ) )
      {
         QString pguid;
         QString pdesc;

         if ( run.projectID.toInt() > 0 )
         {
            query.clear();
            query << "get_project_info" << run.projectID;

            if ( queryOk( query, error ) )
            {
               if ( dbase->next() )
               {
                  pguid = dbase->value(  1 ).toString();
                  pdesc = dbase->value( 10 ).toString();
               }
            }

            else if ( connectionLost() )
               return false;

            else
            {  // A project the server will not describe -- not this
               // person's, or gone -- leaves its experiments unattributed,
               // not the whole listing unread
               emit message( tr( "Project %1 could not be read (%2); its"
                                 " experiments are listed without it" )
                             .arg( run.projectID, error ) );
               error.clear();
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

bool US_DataCatalog::connectionLost( void )
{
   if ( dbase == nullptr  ||  ! dbase->isConnected() )   return true;
   if ( dbase->lastErrno() == IUS_DB2::NOT_CONNECTED )   return true;

   // The MySQL client's own errors -- server gone, connection lost -- are
   // numbered from 2000; the server's are below that
   int code = dbase->lastServerErrno();

   return code >= 2000  &&  code < 3000;
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

         if      ( name.endsWith( ".auc",  file_case ) )  rf.aucFiles << name;
         else if ( name.endsWith( ".tmst", file_case ) )  rf.tmstFile =  name;
         else if ( name.endsWith( ".xml",  file_case ) )  xmls        << name;
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

         if ( ! rf.aucFiles.isEmpty()  &&
              name.compare( expName, file_case ) == 0 )
         {
            rf.expXml = name;
            continue;
         }

         if ( ! name.startsWith( prefix, file_case ) )  continue;

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

   if ( ! diskIndexReady( error ) )  return false;

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

      if ( rf.runID.compare( rf.dirName, file_case ) != 0 )
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
   QList< Raw >   raws;
   QList< Noise > foreign;
   bool ok = isDb() ? loadDetailDb  ( run_list[ index ], raws, foreign, error )
                    : loadDetailDisk( run_list[ index ], raws, error );

   if ( ! ok )  return false;

   dropChecksums( raws );

   Run&      run         = run_list[ index ];
   const int listedNoise = run.noiseCount;
   run.raws     = raws;
   modelsChanged();
   run.verified = do_checksums;
   pending.removeAll( index );

   // Noise an experiment read earlier found to be of a model here, and
   // noise this one read that is of a model elsewhere
   placeForeignNoise( run, foreign_noise );
   finishRun( run );

   // The listing counts noise by its model's experiment (see
   // catalog_noise_counts), and so does the chain; a per-experiment
   // listing of noise by the edit it was recorded against misses the noise
   // of a model here recorded against an edit of another experiment.  When
   // the counts say something is missing, the investigator's noise listing
   // -- read once, and kept -- has it.
   if ( isDb()  &&  listedNoise >= 0  &&  run.noiseCount != listedNoise )
   {
      QString cacheError;

      if ( cacheNoise( cacheError ) )
      {
         QList< Noise > all = noise_cache;

         if ( placeForeignNoise( run, all ) )  finishRun( run );
      }

      else
         emit message( tr( "The noise records of %1 could not all be read: %2" )
                       .arg( run.runID, cacheError ) );
   }

   QList< int > changed;

   for ( int ii = 0; ii < run_list.size()  &&  ! foreign.isEmpty(); ii++ )
      if ( ii != index  &&  run_list[ ii ].isLoaded()  &&
           placeForeignNoise( run_list[ ii ], foreign ) )
      {
         finishRun( run_list[ ii ] );
         changed << ii;
      }

   foreign_noise << foreign;

   error.clear();

   emit runLoaded( index );

   for ( int ii : changed )
      emit runChanged( ii );

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
   // A listing read for another investigator is dropped here, and then
   // there is none: the records read now would be the new one's, hung
   // under the old one's experiments
   const bool dropped = refreshInvestigator();

   // Without a listing every record would read as an orphan, and a program
   // that cleans up would offer to delete what is attached
   if ( ! listed )
   {
      error = dropped
              ? tr( "A different investigator is selected now, so the"
                    " experiments listed for the previous one were dropped;"
                    " list the experiments again to read their records" )
              : tr( "The experiments have not been listed, so their records"
                    " cannot be read" );
      return false;
   }

   // Even with no experiment listed the store is read: what is in it then
   // is all orphans, and a program that cleans up has to see them
   QList< Run > fresh = run_list;
   Orphans      orphans;

   bool ok = isDb() ? loadAllDb  ( fresh, orphans, error )
                    : loadAllDisk( fresh, orphans, error );

   if ( ! ok )  return false;

   // Everything was read, so publish it.  An experiment already loaded --
   // and perhaps verified -- is kept as it is: what was read for it now
   // is the same chain, with less known about it.  Except for noise: noise
   // of its models recorded against an edit of an experiment not read then
   // was nowhere to be found when it was loaded, and is in the fresh copy.
   QList< int > loaded;
   QList< int > changed;

   for ( int ii = 0; ii < run_list.size(); ii++ )
   {
      if ( run_list[ ii ].isLoaded() )
      {
         QList< Noise > noises;

         for ( const Raw& raw : fresh[ ii ].raws )
            for ( const Edit& edit : raw.edits )
               for ( const Model& model : edit.models )
                  noises << model.noises;

         if ( placeForeignNoise( run_list[ ii ], noises ) )
         {
            finishRun( run_list[ ii ] );
            changed << ii;
         }

         continue;
      }

      run_list[ ii ] = fresh[ ii ];
      finishRun( run_list[ ii ] );
      pending.removeAll( ii );
      loaded << ii;
   }

   if ( proj_guid.isEmpty() )
      orphan_list = orphans;
   else
      orphan_list.clear();

   modelsChanged();

   // The whole store placed every noise record by its model
   foreign_noise.clear();

   for ( int ii = 0; ii < loaded.size(); ii++ )
      emit runLoaded( loaded[ ii ] );

   for ( int ii = 0; ii < changed.size(); ii++ )
      emit runChanged( changed[ ii ] );

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

   while ( dbase->next() )
      flat.noises << noise_row( dbase );

   repairModels( flat.models );

   QHash< QString, QString > modelDescs;

   for ( const Model& model : flat.models )
      add_model_description( modelDescs, model );

   repairNoises( flat.noises, modelDescs );

   // ---- attach them ------------------------------------------------------
   flat.assemble( &orphans );

   // Every noise record, for noisesOfModel; attaching gave a record with
   // no model GUID its model's
   QList< Noise > cache;

   for ( const Model& model : flat.models )
      cache << model.noises;

   cache << orphans.noises;

   QHash< QString, int > runById;

   for ( int ii = 0; ii < runs.size(); ii++ )
   {
      runs[ ii ].raws.clear();

      if ( real_id( runs[ ii ].id ) )
         runById.insert( runs[ ii ].id, ii );
   }

   for ( int ii = 0; ii < flat.raws.size(); ii++ )
   {
      Raw raw = flat.raws[ ii ];
      int rx  = real_id( raw.experimentID )
                ? runById.value( raw.experimentID, -1 ) : -1;

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
   noiseCacheChanged();

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
      raw.id           = db_id( dbase->value(  0 ) );
      raw.filename     = base_name( dbase->value( 2 ).toString() );
      raw.experimentID = db_id( dbase->value(  3 ) );
      raw.solutionID   = db_id( dbase->value(  4 ) );
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
      edit.id          = db_id( dbase->value( 0 ) );
      edit.filename    = base_name( dbase->value( 2 ).toString() );
      edit.lastUpdated = utcText( dbase->value( 5 ) );
      edit.checksum    = dbase->value( 6 ).toString();
      edit.size        = dbase->value( 7 ).toString();
      edit.guid        = dbase->value( 9 ).toString();

      // This procedure names the triple by its ID, not its GUID
      edit.rawID       = db_id( dbase->value( 3 ) );
      edit.rawGUID     = guidOfRaw.value( edit.rawID );
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
   if ( ! diskIndexReady( error ) )  return false;

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

         if ( ! fileDigest( edit.path, edit.checksum, edit.size, error ) )
            return false;
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
      model.subType     = modelSubType( mf.description );
      model.lastUpdated = utc_of_file( mf.path );
      if ( ! fileDigest( mf.path, model.checksum, model.size, error ) )
         return false;

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
      if ( ! fileDigest( nf.path, noise.checksum, noise.size, error ) )
         return false;

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

         if ( verifyRun( next, error ) )  return true;

         // Tried once more, after the rest; see loadNextPending
         if ( verify_tries.value( next ) < 1 )
         {
            verify_tries[ next ]++;
            verify_queue.removeAll( next );
            verify_queue << next;
            error += " " + tr( "(%1 will be verified again later)" )
                           .arg( run_list[ next ].runID );
         }

         emit message( error );
         return false;
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

      if ( ok )
      {
         run_list[ index ] = copy;
         modelsChanged();
      }
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

   // md5 and size, by "raw:" or "edit:" and then the GUID, and by "raw-id:"
   // or "edit-id:" and the ID: a record attached by ID, for want of a GUID,
   // is found by ID here too
   QHash< QString, QPair< QString, QString > > digests;

   QStringList query;
   query << "get_rawData_by_experiment" << run.id;

   if ( ! queryOk( query, error ) )
   {
      if ( ! missingProcedure() )  return false;

      dropTier( ExperimentTier, query[ 0 ] );
      return verifyRunDbLegacy( run, error );
   }

   while ( dbase->next() )
   {
      const QPair< QString, QString > digest( dbase->value(  9 ).toString(),
                                              dbase->value( 10 ).toString() );
      const QString guid = dbase->value( 1 ).toString();

      if ( ! guid.isEmpty() )  digests.insert( "raw:" + guid, digest );

      digests.insert( "raw-id:" + db_id( dbase->value( 0 ) ), digest );
   }

   query.clear();
   query << "get_editedData_by_experiment" << run.id;

   if ( ! queryOk( query, error ) )
   {
      if ( ! missingProcedure() )  return false;

      dropTier( ExperimentTier, query[ 0 ] );
      return verifyRunDbLegacy( run, error );
   }

   while ( dbase->next() )
   {
      const QPair< QString, QString > digest( dbase->value( 8 ).toString(),
                                              dbase->value( 9 ).toString() );
      const QString guid = dbase->value( 3 ).toString();

      if ( ! guid.isEmpty() )  digests.insert( "edit:" + guid, digest );

      digests.insert( "edit-id:" + db_id( dbase->value( 0 ) ), digest );
   }

   // By GUID where the record has one, as it was attached, else by ID.  A
   // record found neither way is no longer in the experiment, and the run
   // cannot be called verified with its checksum missing.
   auto lookup = [ & ]( const QString& kind, const QString& guid,
                        const QString& id, QString& checksum, QString& size )
   {
      QString key = guid.isEmpty() ? QString() : kind + ":" + guid;

      if ( key.isEmpty()  ||  ! digests.contains( key ) )
         key = real_id( id ) ? kind + "-id:" + id : QString();

      if ( key.isEmpty()  ||  ! digests.contains( key ) )  return false;

      checksum = digests.value( key ).first;
      size     = digests.value( key ).second;
      return true;
   };

   for ( int ii = 0; ii < run.raws.size(); ii++ )
   {
      Raw& raw = run.raws[ ii ];

      if ( ! lookup( "raw", raw.guid, raw.id, raw.checksum, raw.size ) )
      {
         error = tr( "The raw data %1 of %2 is no longer in the database;"
                     " read the experiment again" )
                 .arg( raw.filename, run.runID );
         return false;
      }

      for ( int jj = 0; jj < raw.edits.size(); jj++ )
      {
         Edit& edit = raw.edits[ jj ];

         if ( ! lookup( "edit", edit.guid, edit.id, edit.checksum, edit.size ) )
         {
            error = tr( "The edit %1 of %2 is no longer in the database;"
                        " read the experiment again" )
                    .arg( edit.filename, run.runID );
            return false;
         }
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

   // The run is a copy: on false it is dropped and the catalog keeps what
   // it had, so a file that cannot be read fails the whole verify
   bool ok = true;

   for ( int ii = 0; ii < run.raws.size()  &&  ok; ii++ )
   {
      if ( cancelledError( error ) )
      {
         ok = false;
         break;
      }

      Raw& raw = run.raws[ ii ];

      if ( ! raw.path.isEmpty() )
         ok = fileDigest( raw.path, raw.checksum, raw.size, error );

      for ( int jj = 0; jj < raw.edits.size()  &&  ok; jj++ )
      {
         Edit& edit = raw.edits[ jj ];

         if ( ! edit.path.isEmpty() )
            ok = fileDigest( edit.path, edit.checksum, edit.size, error );

         for ( int kk = 0; kk < edit.models.size()  &&  ok; kk++ )
         {
            Model& model = edit.models[ kk ];

            if ( ! model.path.isEmpty() )
               ok = fileDigest( model.path, model.checksum, model.size, error );

            for ( int mm = 0; mm < model.noises.size()  &&  ok; mm++ )
            {
               Noise& noise = model.noises[ mm ];

               if ( noise.path.isEmpty() )  continue;

               ok = fileDigest( noise.path, noise.checksum, noise.size, error );
            }
         }
      }
   }

   do_checksums = saved;

   if ( ok )  error.clear();

   return ok;
}

// ------------------------------------------------- one experiment, database

/* One experiment from the database.

   \a foreign is the noise read with it -- recorded against one of its
   edits -- that hangs under none of its models.  That noise is not lost:
   every procedure that lists noise joins modelPerson on noise.modelID, so
   its model is one of the investigator's and exists, and since it is not
   here it is in another experiment, or is a model whose edit is not the
   investigator's.  It goes to that model when the model is read.
*/
bool US_DataCatalog::loadDetailDb( const Run& run, QList< Raw >& raws,
                                   QList< Noise >& foreign, QString& error )
{
   if ( dbase == nullptr )
   {
      error = tr( "No database connection" );
      return false;
   }

   foreign.clear();

   if ( ( tiers() & ExperimentTier ) != 0 )
   {
      if ( loadDetailDbBulk( run, raws, foreign, error ) )  return true;

      if ( ( tiers() & ExperimentTier ) != 0 )  return false;  // real error

      raws   .clear();
      foreign.clear();
   }

   return loadDetailDbLegacy( run, raws, foreign, error );
}

/* Hang noise under the models of one experiment.

   A record already there is not added twice.  What is placed is taken out
   of \a noises; what is left found no model here.
*/
bool US_DataCatalog::placeForeignNoise( Run& run, QList< Noise >& noises )
{
   bool added = false;

   if ( noises.isEmpty() )  return false;

   for ( int ii = 0; ii < run.raws.size(); ii++ )
      for ( int jj = 0; jj < run.raws[ ii ].edits.size(); jj++ )
         for ( int kk = 0; kk < run.raws[ ii ].edits[ jj ].models.size(); kk++ )
         {
            Model& model = run.raws[ ii ].edits[ jj ].models[ kk ];

            for ( int nn = 0; nn < noises.size(); )
            {
               const Noise& cand = noises[ nn ];

               // By modelID where the record has one, as it was attached
               bool mine = real_id( cand.modelID )  &&  real_id( model.id )
                           ? cand.modelID == model.id
                           : ! model.guid.isEmpty()  &&
                             cand.modelGUID == model.guid;

               if ( ! mine )
               {
                  nn++;
                  continue;
               }

               Noise noise = noises.takeAt( nn );
               bool  known = false;

               for ( const Noise& have : model.noises )
                  if ( have.id == noise.id  &&  have.guid == noise.guid )
                     known = true;

               if ( known )  continue;

               if ( noise.modelGUID.isEmpty() )  noise.modelGUID = model.guid;

               // Its own experiment's read had no model to name it after
               if ( noise.description.isEmpty() )
                  noise.description = noise_description_of( model.description,
                                                            noise.noiseType );

               noise.editGUID = model.editGUID;
               model.noises << noise;
               added = true;
            }
         }

   return added;
}

/* One experiment in four queries.  Any one of them may turn out to be
   missing on a partly upgraded server, and then the experiment is read the
   old way instead -- this one and every one after it.
*/
bool US_DataCatalog::loadDetailDbBulk( const Run& run, QList< Raw >& raws,
                                       QList< Noise >& foreign, QString& error )
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

   repairDescriptions( flat.models, flat.noises );

   // A record whose parent is not in this experiment is not this
   // experiment's; only a whole-store read can call it an orphan.  Noise is
   // the exception: it was read because it was recorded against one of
   // this experiment's edits, and what hangs under none of its models is
   // of a model elsewhere (see loadDetailDb)
   attach_models( flat.models, flat.noises, &foreign );
   attach_edits ( flat.edits,  flat.models, nullptr );
   attach_raws  ( flat.raws,   flat.edits,  nullptr );

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
                                         QList< Noise >& foreign, QString& error )
{
   QString     invID = QString::number( inv_id );
   QStringList query;
   QStringList rawIDs;
   QStringList rawNames;

   query << "get_rawDataIDs" << run.id;

   if ( ! queryOk( query, error ) )  return false;

   while ( dbase->next() )
   {
      rawIDs   << db_id( dbase->value( 0 ) );
      rawNames << base_name( dbase->value( 2 ).toString() );
   }

   QList< Raw > list;

   // The models and noise of the whole experiment, and where each model was
   // read: noise is matched to its model across the experiment, as the
   // per-experiment procedures do, not only within one edit
   QList< Model >          models;
   QList< QPair< int, int > > placeOf;   // (raw, edit) each model was read under
   QList< Noise >          noises;

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
         raw.solutionID  = db_id( dbase->value( 5 ) );
         raw.lastUpdated = utcText( dbase->value( 7 ) );
         raw.checksum    = dbase->value( 8 ).toString();
         raw.size        = dbase->value( 9 ).toString();
      }

      QStringList edtIDs;

      query.clear();
      query << "get_editedDataIDs" << raw.id;

      if ( ! queryOk( query, error ) )  return false;

      while ( dbase->next() )
         edtIDs << db_id( dbase->value( 0 ) );

      for ( int jj = 0; jj < edtIDs.size(); jj++ )
      {
         Edit edit;
         edit.id      = edtIDs[ jj ];
         edit.rawID   = raw.id;
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

         query.clear();
         query << "get_model_desc_by_editID" << invID << edit.id;

         if ( ! queryOk( query, error ) )  return false;

         while ( dbase->next() )
         {
            Model model = model_row( dbase );

            if ( model.editGUID.isEmpty() )  model.editGUID = edit.guid;

            models  << model;
            placeOf << qMakePair( list.size(), raw.edits.size() );
         }

         query.clear();
         query << "get_noise_desc_by_editID" << invID << edit.id;

         if ( ! queryOk( query, error ) )  return false;

         while ( dbase->next() )
            noises << noise_row( dbase );

         raw.edits << edit;
      }

      list << raw;
   }

   repairDescriptions( models, noises );
   attach_models( models, noises, &foreign );

   for ( int ii = 0; ii < models.size(); ii++ )
      list[ placeOf[ ii ].first ].edits[ placeOf[ ii ].second ].models
         << models[ ii ];

   raws = list;

   return true;
}

int US_DataCatalog::modelIndexBuilds( void ) const
{
   return model_index_builds;
}

void US_DataCatalog::modelsChanged( void )
{
   model_index = ModelIndex();
}

void US_DataCatalog::noiseCacheChanged( void )
{
   model_index.noiseOf.clear();
   model_index.noiseBuilt = false;
}

const US_DataCatalog::ModelIndex& US_DataCatalog::modelIndex( void )
{
   if ( ! model_index.built )
   {
      model_index_builds++;
      model_index.descs = knownModelDescriptions();

      // The edit of a model: from the first experiment that has it, unless
      // it is an orphan
      for ( const Run& run : run_list )
         for ( const Raw& raw : run.raws )
            for ( const Edit& edit : raw.edits )
               for ( const Model& model : edit.models )
                  if ( ! model.guid.isEmpty()  &&  ! model_index.editOf.contains( model.guid ) )
                     model_index.editOf.insert( model.guid, model.editGUID );

      for ( const Model& model : orphan_list.models )
         if ( ! model.guid.isEmpty() )
            model_index.editOf.insert( model.guid, model.editGUID );

      model_index.built      = true;
      model_index.noiseBuilt = false;
      model_index.noiseOf.clear();
   }

   if ( ! model_index.noiseBuilt  &&  noise_cached )
   {
      for ( int ii = 0; ii < noise_cache.size(); ii++ )
      {
         const Noise& noise = noise_cache[ ii ];
         QString      owner = real_id( noise.modelID )
                              ? model_index.descs.value( "guid-of:" + noise.modelID ) : QString();

         if ( owner.isEmpty() )  owner = noise.modelGUID;
         if ( owner.isEmpty() )  continue;

         model_index.noiseOf[ owner ] << ii;
      }

      model_index.noiseBuilt = true;
   }

   return model_index;
}

// The description of every model the catalog has read, for a noise record
// that needs its model's, and the GUID of each by its ID.  Only the strings
// are collected: the chain itself is not copied.
QHash< QString, QString > US_DataCatalog::knownModelDescriptions( void ) const
{
   QHash< QString, QString > descs;

   for ( const Run& run : run_list )
      for ( const Raw& raw : run.raws )
         for ( const Edit& edit : raw.edits )
            for ( const Model& model : edit.models )
               add_model_description( descs, model );

   for ( const Model& model : orphan_list.models )
      add_model_description( descs, model );

   return descs;
}

/* Repair the descriptions the server gives.

   model.description and noise.description are VARCHAR(160) (us3.sql), so a
   longer description is cut at 160 characters; the record's XML holds it
   in full, so a record whose description is exactly that long is read
   again.  No schema in the us3_sql history had a narrower column (us_manage
   _data used to test for 80, which matched no column and only re-read the
   descriptions that happened to be 80 long).  Each record is read once:
   desc_tried remembers what the read gave -- a description that really is
   160 long, or one that could not be read again, keeps what the listing
   gave -- until the experiments are listed again.
*/
void US_DataCatalog::repairModels( QList< Model >& models )
{
   for ( int ii = 0; ii < models.size(); ii++ )
   {
      Model& model = models[ ii ];

      if ( model.description.length() != desc_width  ||  ! real_id( model.id ) )
         continue;

      const QString key = "model:" + model.id;

      if ( ! desc_tried.contains( key ) )
      {
         QString full;
         QStringList query;
         query << "get_model_info" << model.id;
         dbase->query( query );

         if ( dbase->lastErrno() == IUS_DB2::OK  &&  dbase->next() )
            full = peek_xml_attribute( dbase->value( 2 ).toString(),
                                       "model", "description" );

         desc_tried.insert( key, full );
      }

      const QString full = desc_tried.value( key );

      if ( full.length() > model.description.length() )
      {
         model.description = full;
         model.subType     = modelSubType( full );
      }
   }
}

/* A noise record written with no description takes its model's, with the
   noise type in place of ".model" -- the name UltraScan gives it.  These
   are a handful of records on an old store and none on a new one.
*/
void US_DataCatalog::repairNoises( QList< Noise >& noises,
                                   const QHash< QString, QString >& modelDescs )
{
   for ( int ii = 0; ii < noises.size(); ii++ )
   {
      Noise& noise = noises[ ii ];

      if ( noise.description.length() == desc_width  &&  real_id( noise.id ) )
      {
         const QString key = "noise:" + noise.id;

         if ( ! desc_tried.contains( key ) )
         {
            QString full;
            QStringList query;
            query << "get_noise_info" << noise.id;
            dbase->query( query );

            if ( dbase->lastErrno() == IUS_DB2::OK  &&  dbase->next() )
               full = peek_xml_attribute( dbase->value( 5 ).toString(),
                                          "noise", "description" );

            desc_tried.insert( key, full );
         }

         const QString full = desc_tried.value( key );

         if ( full.length() > noise.description.length() )
            noise.description = full;
      }

      if ( noise.description.isEmpty() )
      {
         QString mdesc = real_id( noise.modelID )
                         ? modelDescs.value( "id:" + noise.modelID )
                         : QString();

         if ( mdesc.isEmpty()  &&  ! noise.modelGUID.isEmpty() )
            mdesc = modelDescs.value( noise.modelGUID );

         noise.description = noise_description_of( mdesc, noise.noiseType );
      }
   }
}

void US_DataCatalog::repairDescriptions( QList< Model >& models,
                                         QList< Noise >& noises )
{
   repairModels( models );

   QHash< QString, QString > modelDescs;

   for ( const Model& model : models )
      add_model_description( modelDescs, model );

   repairNoises( noises, modelDescs );
}

QString US_DataCatalog::modelSubType( const QString& description )
{
   // runID.triple.e<editID>_a<analysisID>_<TYPE>_<...>.model
   return description.section( ".", -2, -2 ).section( "_", 2, 2 );
}

void US_DataCatalog::setFileNameCase( Qt::CaseSensitivity sensitivity )
{
   file_case = sensitivity;
}

Qt::CaseSensitivity US_DataCatalog::fileNameCase( void ) const
{
   return file_case;
}

// ------------------------------------------------- one experiment, disk

bool US_DataCatalog::loadDetailDisk( const Run& run, QList< Raw >& raws,
                                     QString& error )
{
   if ( ! diskIndexReady( error ) )  return false;

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

      if ( ! fileDigest( raw.path, raw.checksum, raw.size, error ) )
         return false;
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

         if ( ! fileDigest( edit.path, edit.checksum, edit.size, error ) )
            return false;
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
            model.subType     = modelSubType( mf.description );

            if ( ! fileDigest( mf.path, model.checksum, model.size, error ) )
               return false;
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

               if ( ! fileDigest( nf.path, noise.checksum, noise.size, error ) )
                  return false;
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
