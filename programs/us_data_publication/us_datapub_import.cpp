//! \file us_datapub_import.cpp
#include "us_datapub_import.h"
#include "us_datapub_hash.h"
#include "us_datapub_records.h"

#include "us_settings.h"
#include "us_util.h"
#include "us_datafiles.h"
#include "us_project.h"
#include "us_solution.h"
#include "us_buffer.h"
#include "us_analyte.h"
#include "us_model.h"
#include "us_noise.h"
#include "us_experiment.h"
#include "us_simparms.h"
#include "us_convert.h"
#include "us_hardware.h"
#include "us_rotor.h"
#include "us_time_state.h"

US_DataPubImporter::Options::Options()
{
   target       = US_DataPub::TargetDb;
   policy       = US_DataPub::PolicyReuse;
   dryRun       = false;
   verifyHashes = true;
   renameSuffix = QObject::tr( "imported" );
}

US_DataPub::ConflictPolicy US_DataPubImporter::Options::policyFor(
      US_DataPub::EntityType type ) const
{
   return typePolicies.value( int( type ), policy );
}

US_DataPubImporter::Result::Result()
{
   type       = US_DataPub::UnknownType;
   resolution = US_DataPub::ResolvedSkipped;
}

US_DataPubImporter::US_DataPubImporter( QObject* parent ) : QObject( parent )
{
   resolver   = nullptr;
   dbase      = nullptr;
   step_count = 0;
}

US_DataPubImporter::~US_DataPubImporter()
{
   closeTarget();
   bundle.cleanup();
}

void US_DataPubImporter::setResolver( US_DataPubConflictResolver* a_resolver )
{
   resolver = a_resolver;
}

const US_DataPubManifest& US_DataPubImporter::manifest( void ) const
{
   return mani;
}

QString US_DataPubImporter::bundleRoot( void ) const
{
   return bundle.rootPath();
}

QList< US_DataPubImporter::Result > US_DataPubImporter::results( void ) const
{
   return reslist;
}

QStringList US_DataPubImporter::log( void ) const
{
   return logtext;
}

void US_DataPubImporter::note( const QString& text )
{
   logtext << text;
   emit message( text );
}

bool US_DataPubImporter::inspect( const QString& bundlePath, QString& error )
{
   mani.clear();
   logtext.clear();
   reslist.clear();

   if ( ! bundle.unpack( bundlePath, error ) )  return false;

   QString manifestPath = bundle.rootPath() + "/"
                          + US_DataPubBundle::manifestName();

   if ( ! mani.read( manifestPath, error ) )  return false;

   if ( mani.version > US_DataPubManifest::currentVersion )
   {
      error = tr( "The bundle was written with manifest format %1;"
                  " this build understands up to format %2" )
              .arg( mani.version ).arg( US_DataPubManifest::currentVersion );
      return false;
   }

   if ( ! mani.validate( error ) )  return false;

   note( tr( "Bundle %1 holds %2 records, scope \"%3\", from the %4" )
         .arg( QFileInfo( bundlePath ).fileName() ).arg( mani.total() )
         .arg( mani.scope ).arg( mani.source ) );

   return true;
}

QString US_DataPubImporter::payloadPath( const US_DataPubEntity& entity ) const
{
   if ( entity.payload.isEmpty() )  return QString();

   return bundle.rootPath() + "/" + entity.payload;
}

QStringList US_DataPubImporter::verifyPayloads( void ) const
{
   QStringList               problems;
   QList< US_DataPubEntity > entities = mani.all();

   for ( int ii = 0; ii < entities.size(); ii++ )
   {
      const US_DataPubEntity& entity = entities[ ii ];
      QString                 path   = payloadPath( entity );

      if ( path.isEmpty() )
      {
         problems << tr( "%1 has no payload" ).arg( entity.label() );
         continue;
      }

      if ( ! QFile::exists( path ) )
      {
         problems << tr( "%1: the payload %2 is missing" )
                     .arg( entity.label() ).arg( entity.payload );
         continue;
      }

      if ( entity.sha256.isEmpty() )  continue;

      QString hash = US_DataPubHash::fileHash( path );

      if ( hash == entity.sha256 )  continue;

      problems << tr( "%1: the payload %2 does not match its digest" )
                  .arg( entity.label() ).arg( entity.payload );
   }

   return problems;
}

QString US_DataPubImporter::workCopy( const US_DataPubEntity& entity,
                                      QString& error )
{
   QString source = payloadPath( entity );

   if ( source.isEmpty()  ||  ! QFile::exists( source ) )
   {
      error = tr( "The payload of %1 is missing from the bundle" )
              .arg( entity.label() );
      return QString();
   }

   QString workDir = bundle.stagingPath() + "/.work";

   if ( ! QDir().mkpath( workDir ) )
   {
      error = tr( "Cannot create the work directory %1" ).arg( workDir );
      return QString();
   }

   QString target = workDir + "/" + QFileInfo( source ).fileName();

   if ( QFile::exists( target ) )  QFile::remove( target );

   if ( ! QFile::copy( source, target ) )
   {
      error = tr( "Cannot copy the payload of %1" ).arg( entity.label() );
      return QString();
   }

   QFile::setPermissions( target, QFileDevice::ReadOwner
                                  | QFileDevice::WriteOwner );
   error.clear();

   return target;
}

QString US_DataPubImporter::mappedGuid( const QString& guid ) const
{
   return guidMap.value( guid, guid );
}

QString US_DataPubImporter::mappedId( const QString& guid ) const
{
   return idMap.value( guid, QString( "-1" ) );
}

QString US_DataPubImporter::mappedName( const QString& guid ) const
{
   return nameMap.value( guid, QString() );
}

void US_DataPubImporter::record( const US_DataPubEntity& entity,
                                 US_DataPub::Resolution resolution,
                                 const QString& targetGUID,
                                 const QString& targetName,
                                 const QString& targetID,
                                 const QString& detail )
{
   Result result;
   result.type       = entity.type;
   result.guid       = entity.guid;
   result.targetGUID = targetGUID;
   result.name       = entity.name;
   result.targetName = targetName;
   result.targetID   = targetID;
   result.resolution = resolution;
   result.detail     = detail;

   reslist << result;

   guidMap.insert( entity.guid, targetGUID );
   idMap  .insert( entity.guid, targetID );
   nameMap.insert( entity.guid, targetName );

   step_count++;
   emit stepDone( step_count );

   note( tr( "%1 \"%2\" %3%4" )
         .arg( US_DataPub::typeText( entity.type ) )
         .arg( targetName.isEmpty() ? entity.label() : targetName )
         .arg( US_DataPub::resolutionText( resolution ) )
         .arg( detail.isEmpty() ? QString() : " -- " + detail ) );
}

// -------------------------------------------------------------------- target

bool US_DataPubImporter::openTarget( QString& error )
{
   closeTarget();

   if ( opts.target == US_DataPub::TargetDisk )
   {
      QString base = opts.outputDir.trimmed();

      if ( base.isEmpty() )  return true;

      if ( ! QDir().mkpath( base ) )
      {
         error = tr( "Cannot create the output directory %1" ).arg( base );
         return false;
      }

      return true;
   }

   dbase = new US_DB2( opts.dbPassword );

   if ( dbase->lastErrno() != US_DB2::OK )
   {
      error = tr( "Cannot connect to the database: %1" )
              .arg( dbase->lastError() );
      delete dbase;
      dbase = nullptr;
      return false;
   }

   error.clear();

   return true;
}

void US_DataPubImporter::closeTarget( void )
{
   if ( dbase != nullptr )  delete dbase;

   dbase = nullptr;
}

QString US_DataPubImporter::diskDir( US_DataPub::EntityType type,
                                     bool create ) const
{
   QString base = opts.outputDir.trimmed().isEmpty()
                  ? US_Settings::dataDir()
                  : opts.outputDir.trimmed() + "/data";
   QString path;

   switch ( type )
   {
      case US_DataPub::Project:          path = base + "/projects";     break;
      case US_DataPub::Buffer:           path = base + "/buffers";      break;
      case US_DataPub::Analyte:          path = base + "/analytes";     break;
      case US_DataPub::Solution:         path = base + "/solutions";    break;
      case US_DataPub::Model:            path = base + "/models";       break;
      case US_DataPub::Noise:            path = base + "/noises";       break;
      case US_DataPub::RotorCalibration: path = base + "/rotors";       break;
      case US_DataPub::Centerpiece:      path = base + "/centerpieces"; break;
      default:                           path = base;                   break;
   }

   if ( create )  QDir().mkpath( path );

   return path;
}

QString US_DataPubImporter::diskResultDir( const QString& runID,
                                           bool create ) const
{
   QString base = opts.outputDir.trimmed().isEmpty()
                  ? US_Settings::resultDir()
                  : opts.outputDir.trimmed() + "/results";
   QString path = base + "/" + runID;

   if ( create )  QDir().mkpath( path );

   return path;
}

bool US_DataPubImporter::diskFilePresent( const US_DataPubEntity& entity,
                                          QString& path, bool& identical )
{
   path     .clear();
   identical = false;

   QString runID = runIdFor( entity );

   if ( runID.isEmpty() )  return false;

   path = diskResultDir( runID ) + "/" + renamedFile( entity );

   if ( ! QFile::exists( path ) )  return false;

   identical = ( ! entity.sha256.isEmpty()  &&
                 US_DataPubHash::fileHash( path ) == entity.sha256 );

   return true;
}

// ------------------------------------------------------------------ the run

bool US_DataPubImporter::runImport( const Options& options, QString& error )
{
   opts       = options;
   logtext.clear();
   reslist.clear();
   guidMap.clear();
   idMap  .clear();
   nameMap.clear();
   step_count = 0;

   if ( mani.total() < 1 )
   {
      error = tr( "No bundle has been inspected yet" );
      return false;
   }

   if ( opts.verifyHashes )
   {
      QStringList problems = verifyPayloads();

      if ( ! problems.isEmpty() )
      {
         error = tr( "The bundle is not intact:\n" ) + problems.join( "\n" );
         return false;
      }

      note( tr( "All %1 payloads match their digests" ).arg( mani.total() ) );
   }

   if ( ! openTarget( error ) )  return false;

   emit steps( mani.total() );

   // The manifest lists the records in the declared section order; they are
   // replayed in the order that satisfies the target's own dependencies.
   QList< US_DataPub::EntityType > types = importOrder();

   for ( int ii = 0; ii < types.size(); ii++ )
   {
      QList< US_DataPubEntity > entities = mani.section( types[ ii ] );

      for ( int jj = 0; jj < entities.size(); jj++ )
      {
         if ( importEntity( entities[ jj ], error ) )  continue;

         closeTarget();
         return false;
      }
   }

   closeTarget();

   note( opts.dryRun
         ? tr( "Dry run complete; nothing was written" )
         : tr( "Imported %1 records" ).arg( reslist.size() ) );

   return true;
}

bool US_DataPubImporter::importEntity( const US_DataPubEntity& entity,
                                       QString& error )
{
   US_DataPub::EntityType type = entity.type;

   // ---- a run-directory payload that is already on disk -------------------
   if ( opts.target == US_DataPub::TargetDisk  &&
        ( type == US_DataPub::RawData  ||  type == US_DataPub::EditedData  ||
          type == US_DataPub::TimeState ) )
   {
      QString present;
      bool    identical = false;

      if ( diskFilePresent( entity, present, identical ) )
      {
         if ( ! identical )
         {
            error = tr( "%1 already exists with different content;"
                        " the import will not overwrite it" ).arg( present );
            return false;
         }

         record( entity, US_DataPub::ResolvedReused, entity.guid,
                 entity.name, entity.id,
                 tr( "the same file is already in the run directory" ) );
         return true;
      }
   }

   // ---- already in the target under the same GUID? ------------------------
   QString existingID;
   QString existingName;

   if ( findByGuid( type, entity.guid, existingID, existingName ) )
   {
      record( entity, US_DataPub::ResolvedReused, entity.guid,
              existingName.isEmpty() ? entity.name : existingName,
              existingID, tr( "the target already holds this record" ) );
      return true;
   }

   // ---- is the name taken by a different record? --------------------------
   QString name     = entity.name;
   QString otherGUID;
   QString otherID;
   bool    conflict = ! name.isEmpty()
                      &&  findByName( type, name, otherGUID, otherID );

   US_DataPub::ConflictPolicy policy = opts.policyFor( type );

   if ( conflict )
   {
      QString payload     = payloadPath( entity );
      QString ourPrint    = US_DataPubHash::fingerprint( payload );
      QString theirPrint  = targetFingerprint( type, otherGUID, otherID );
      bool    identical   = ( ! ourPrint.isEmpty()  &&  ourPrint == theirPrint );
      QString details     = identical
                            ? tr( "the existing record has the same properties" )
                            : tr( "the existing record has different properties" );
      QString suggestion  = uniqueName( type, name );

      if ( resolver != nullptr )
         policy = resolver->resolve( type, name, identical, details,
                                     suggestion );

      if ( policy == US_DataPub::PolicyFail )
      {
         error = tr( "%1 \"%2\" already exists in the target and the"
                     " conflict policy is \"fail\"" )
                 .arg( US_DataPub::typeText( type ) ).arg( name );
         return false;
      }

      if ( policy == US_DataPub::PolicyReuse  &&  identical )
      {
         record( entity, US_DataPub::ResolvedReused, otherGUID, name, otherID,
                 details );
         return true;
      }

      // Reusing a record that differs would silently change its meaning,
      // so a differing record is always imported under a new name.  The
      // suggestion is already free unless a resolver replaced it with a
      // name of its own that the target happens to have.
      name = suggestion.isEmpty() ? entity.name : suggestion;

      QString takenGUID;
      QString takenID;

      if ( findByName( type, name, takenGUID, takenID ) )
         name = uniqueName( type, entity.name );

      if ( opts.dryRun )
      {
         record( entity, US_DataPub::ResolvedRenamed, entity.guid, name,
                 QString( "-1" ), details );
         return true;
      }

      QString workPath = workCopy( entity, error );

      if ( workPath.isEmpty() )  return false;

      if ( type != US_DataPub::RawData  &&  type != US_DataPub::EditedData  &&
           type != US_DataPub::TimeState  &&  type != US_DataPub::Centerpiece )
      {
         if ( ! US_DataPubRecords::setRecordName( workPath, type, name ) )
            note( tr( "The name of %1 could not be changed inside its"
                      " payload; it is imported as \"%2\"" )
                  .arg( entity.label() ).arg( entity.name ) );
      }

      QString newID;

      if ( ! createRecord( entity, workPath, name, newID, error ) )
         return false;

      record( entity, US_DataPub::ResolvedRenamed, entity.guid, name, newID,
              details );
      return true;
   }

   // ---- a plain new record ------------------------------------------------
   if ( opts.dryRun )
   {
      record( entity, US_DataPub::ResolvedCreated, entity.guid, name,
              QString( "-1" ), QString() );
      return true;
   }

   QString workPath = workCopy( entity, error );

   if ( workPath.isEmpty() )  return false;

   QString newID;

   if ( ! createRecord( entity, workPath, name, newID, error ) )  return false;

   record( entity, US_DataPub::ResolvedCreated, entity.guid, name, newID,
           QString() );

   return true;
}

bool US_DataPubImporter::createRecord( const US_DataPubEntity& entity,
                                       const QString& workPath,
                                       const QString& name, QString& newID,
                                       QString& error )
{
   newID = QString( "-1" );

   if ( opts.target == US_DataPub::TargetDb )
      return createDb( entity, workPath, name, newID, error );

   return createDisk( entity, workPath, name, newID, error );
}

// ------------------------------------------------- looking into the target

namespace
{
   // File name prefix, root element and GUID attribute of the file-backed
   // record types in an UltraScan3 local disk store
   class DiskLayout
   {
      public:
         DiskLayout() {}
         QString prefix;      // "P", "B", "A", "S", "M", "N", "C"
         QString element;     // The element carrying the GUID
         QString guidAttr;    // The attribute carrying the GUID
   };

   DiskLayout disk_layout( US_DataPub::EntityType type )
   {
      DiskLayout layout;

      switch ( type )
      {
         case US_DataPub::Project:
            layout.prefix = "P";  layout.element = "project";
            layout.guidAttr = "guid";           break;

         case US_DataPub::Buffer:
            layout.prefix = "B";  layout.element = "buffer";
            layout.guidAttr = "guid";           break;

         case US_DataPub::Analyte:
            layout.prefix = "A";  layout.element = "analyte";
            layout.guidAttr = "analyteGUID";    break;

         case US_DataPub::Solution:
            layout.prefix = "S";  layout.element = "solution";
            layout.guidAttr = "guid";           break;

         case US_DataPub::Model:
            layout.prefix = "M";  layout.element = "model";
            layout.guidAttr = "modelGUID";      break;

         case US_DataPub::Noise:
            layout.prefix = "N";  layout.element = "noise";
            layout.guidAttr = "noiseGUID";      break;

         case US_DataPub::RotorCalibration:
            layout.prefix = "C";  layout.element = "Calibration";
            layout.guidAttr = "guid";           break;

         default:
            break;
      }

      return layout;
   }

   // Attributes of the first matching element of an XML file
   QMap< QString, QString > first_attributes( const QString& filename,
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

         if ( ! xml.isStartElement() )                 continue;
         if ( xml.name().toString() != element )       continue;

         QXmlStreamAttributes attrs = xml.attributes();

         for ( int ii = 0; ii < attrs.size(); ii++ )
            values.insert( attrs.at( ii ).name ().toString(),
                           attrs.at( ii ).value().toString() );
         break;
      }

      file.close();

      return values;
   }
}

bool US_DataPubImporter::findByGuid( US_DataPub::EntityType type,
                                     const QString& guid, QString& id,
                                     QString& name )
{
   id  .clear();
   name.clear();

   if ( guid.isEmpty() )  return false;

   if ( opts.target == US_DataPub::TargetDb )
   {
      if ( dbase == nullptr )  return false;

      QStringList query;

      switch ( type )
      {
         case US_DataPub::Project:  query << "get_projectID_from_GUID"  << guid;  break;
         case US_DataPub::Buffer:   query << "get_bufferID"             << guid;  break;
         case US_DataPub::Analyte:  query << "get_analyteID"            << guid;  break;
         case US_DataPub::Solution: query << "get_solutionID_from_GUID" << guid;  break;
         case US_DataPub::RawData:  query << "get_rawDataID_from_GUID"  << guid;  break;
         case US_DataPub::EditedData: query << "get_editID"             << guid;  break;
         case US_DataPub::Model:    query << "get_modelID"              << guid;  break;
         case US_DataPub::Noise:    query << "get_noiseID"              << guid;  break;

         case US_DataPub::Experiment:
            // The database offers no experiment lookup by GUID.  An
            // experiment is recognized by its run identifier instead,
            // which findByName() does.
            return false;

         case US_DataPub::RotorCalibration:
         {
            QVector< US_Rotor::Lab > labs;
            US_Rotor::readLabsDB( labs, dbase );

            for ( int ii = 0; ii < labs.size(); ii++ )
            {
               QVector< US_Rotor::Rotor > rotors;
               US_Rotor::readRotorsFromDB( rotors, labs[ ii ].ID, dbase );

               for ( int jj = 0; jj < rotors.size(); jj++ )
               {
                  QVector< US_Rotor::RotorCalibration > cals;
                  US_Rotor::readCalibrationProfilesDB( cals, rotors[ jj ].ID,
                                                       dbase );

                  for ( int kk = 0; kk < cals.size(); kk++ )
                  {
                     if ( cals[ kk ].GUID != guid )  continue;

                     id   = QString::number( cals[ kk ].ID );
                     name = cals[ kk ].label;
                     return true;
                  }
               }
            }

            return false;
         }

         case US_DataPub::Centerpiece:
         {
            QList< US_AbstractCenterpiece > centerpieces;
            US_AbstractCenterpiece::read_centerpieces( dbase, centerpieces );

            for ( int ii = 0; ii < centerpieces.size(); ii++ )
            {
               if ( centerpieces[ ii ].guid != guid )  continue;

               id   = QString::number( centerpieces[ ii ].serial_number );
               name = centerpieces[ ii ].name;
               return true;
            }

            return false;
         }

         default:
            return false;
      }

      dbase->query( query );

      if ( dbase->lastErrno() != US_DB2::OK )  return false;
      if ( ! dbase->next() )                   return false;

      id = dbase->value( 0 ).toString();

      return ! id.isEmpty();
   }

   // ---- disk target -------------------------------------------------------
   if ( type == US_DataPub::Centerpiece )
   {
      QList< US_AbstractCenterpiece > centerpieces;
      US_AbstractCenterpiece::read_centerpieces( centerpieces );

      for ( int ii = 0; ii < centerpieces.size(); ii++ )
      {
         if ( centerpieces[ ii ].guid != guid )  continue;

         id   = QString::number( centerpieces[ ii ].serial_number );
         name = centerpieces[ ii ].name;
         return true;
      }

      return false;
   }

   if ( type == US_DataPub::Experiment )
   {
      QString base = opts.outputDir.trimmed().isEmpty()
                     ? US_Settings::resultDir()
                     : opts.outputDir.trimmed() + "/results";
      QStringList runs = QDir( base ).entryList(
                         QDir::AllDirs | QDir::NoDotAndDotDot, QDir::Name );

      for ( int ii = 0; ii < runs.size(); ii++ )
      {
         QDir        dir( base + "/" + runs[ ii ] );
         QStringList xmls = dir.entryList(
                            QStringList( runs[ ii ] + ".*.xml" ),
                            QDir::Files, QDir::Name );

         for ( int jj = 0; jj < xmls.size(); jj++ )
         {
            if ( xmls[ jj ].count( "." ) != 2 )  continue;

            QMap< QString, QString > attrs = first_attributes(
                  dir.absoluteFilePath( xmls[ jj ] ), "experiment" );

            if ( attrs.value( "guid" ) != guid )  continue;

            id   = attrs.value( "id", QString( "-1" ) );
            name = runs[ ii ];
            return true;
         }
      }

      return false;
   }

   if ( type == US_DataPub::RawData  ||  type == US_DataPub::EditedData  ||
        type == US_DataPub::TimeState )
      return false;              // These live in the per-run result directory

   DiskLayout layout = disk_layout( type );

   if ( layout.prefix.isEmpty() )  return false;

   QString     dir   = diskDir( type );
   QStringList files = QDir( dir ).entryList(
                       QStringList( layout.prefix + "???????.xml" ),
                       QDir::Files, QDir::Name );

   for ( int ii = 0; ii < files.size(); ii++ )
   {
      QString path  = dir + "/" + files[ ii ];
      QMap< QString, QString > attrs = first_attributes( path,
                                                         layout.element );

      if ( attrs.value( layout.guidAttr ) != guid )  continue;

      id   = attrs.value( "id", QString( "-1" ) );
      name = US_DataPubRecords::recordName( path, type );

      return true;
   }

   return false;
}

bool US_DataPubImporter::findByName( US_DataPub::EntityType type,
                                     const QString& name, QString& guid,
                                     QString& id )
{
   guid.clear();
   id  .clear();

   if ( name.isEmpty() )  return false;

   if ( opts.target == US_DataPub::TargetDb )
   {
      if ( dbase == nullptr )  return false;

      QString     invID = QString::number( US_Settings::us_inv_ID() );
      QStringList query;

      switch ( type )
      {
         case US_DataPub::Project:
         {
            query << "get_project_desc" << invID;
            dbase->query( query );
            QStringList ids;

            while ( dbase->next() )
               ids << dbase->value( 0 ).toString();

            for ( int ii = 0; ii < ids.size(); ii++ )
            {
               query.clear();
               query << "get_project_info" << ids[ ii ];
               dbase->query( query );

               if ( ! dbase->next() )  continue;
               if ( dbase->value( 10 ).toString() != name )  continue;

               guid = dbase->value( 1 ).toString();
               id   = ids[ ii ];
               return true;
            }

            return false;
         }

         case US_DataPub::Experiment:
         {
            query << "get_experiment_info_by_runID" << name << invID;
            dbase->query( query );

            if ( ! dbase->next() )  return false;

            id   = dbase->value( 1 ).toString();
            guid = dbase->value( 2 ).toString();

            return ! id.isEmpty();
         }

         case US_DataPub::Solution:
         {
            query << "all_solutionIDs" << invID;
            dbase->query( query );
            QStringList ids;

            while ( dbase->next() )
               ids << dbase->value( 0 ).toString();

            for ( int ii = 0; ii < ids.size(); ii++ )
            {
               US_Solution solution;

               if ( solution.readFromDB( ids[ ii ].toInt(), dbase )
                    != US_DB2::OK )                        continue;
               if ( solution.solutionDesc != name )        continue;

               guid = solution.solutionGUID;
               id   = ids[ ii ];
               return true;
            }

            return false;
         }

         case US_DataPub::Buffer:
         {
            query << "get_buffer_desc" << invID;
            dbase->query( query );
            QString bufID;

            while ( dbase->next() )
            {
               if ( dbase->value( 1 ).toString() != name )  continue;

               bufID = dbase->value( 0 ).toString();
               break;
            }

            if ( bufID.isEmpty() )  return false;

            query.clear();
            query << "get_buffer_info" << bufID;
            dbase->query( query );

            if ( dbase->next() )
               guid = dbase->value( 0 ).toString();

            id = bufID;

            return true;
         }

         case US_DataPub::Analyte:
         {
            query << "get_analyte_desc" << invID;
            dbase->query( query );
            QString anaID;

            while ( dbase->next() )
            {
               if ( dbase->value( 1 ).toString() != name )  continue;

               anaID = dbase->value( 0 ).toString();
               break;
            }

            if ( anaID.isEmpty() )  return false;

            query.clear();
            query << "get_analyte_info" << anaID;
            dbase->query( query );

            if ( dbase->next() )
               guid = dbase->value( 0 ).toString();

            id = anaID;

            return true;
         }

         case US_DataPub::Model:
         {
            query << "get_model_desc" << invID;
            dbase->query( query );

            while ( dbase->next() )
            {
               if ( dbase->value( 2 ).toString() != name )  continue;

               id   = dbase->value( 0 ).toString();
               guid = dbase->value( 1 ).toString();
               return true;
            }

            return false;
         }

         case US_DataPub::Noise:
         {
            query << "get_noise_desc" << invID;
            dbase->query( query );

            while ( dbase->next() )
            {
               if ( dbase->value( 9 ).toString() != name )  continue;

               id   = dbase->value( 0 ).toString();
               guid = dbase->value( 1 ).toString();
               return true;
            }

            return false;
         }

         case US_DataPub::RotorCalibration:
         {
            QVector< US_Rotor::Lab > labs;
            US_Rotor::readLabsDB( labs, dbase );

            for ( int ii = 0; ii < labs.size(); ii++ )
            {
               QVector< US_Rotor::Rotor > rotors;
               US_Rotor::readRotorsFromDB( rotors, labs[ ii ].ID, dbase );

               for ( int jj = 0; jj < rotors.size(); jj++ )
               {
                  QVector< US_Rotor::RotorCalibration > cals;
                  US_Rotor::readCalibrationProfilesDB( cals, rotors[ jj ].ID,
                                                       dbase );

                  for ( int kk = 0; kk < cals.size(); kk++ )
                  {
                     if ( cals[ kk ].label != name )  continue;

                     guid = cals[ kk ].GUID;
                     id   = QString::number( cals[ kk ].ID );
                     return true;
                  }
               }
            }

            return false;
         }

         case US_DataPub::Centerpiece:
         {
            QList< US_AbstractCenterpiece > centerpieces;
            US_AbstractCenterpiece::read_centerpieces( dbase, centerpieces );

            for ( int ii = 0; ii < centerpieces.size(); ii++ )
            {
               if ( centerpieces[ ii ].name != name )  continue;

               guid = centerpieces[ ii ].guid;
               id   = QString::number( centerpieces[ ii ].serial_number );
               return true;
            }

            return false;
         }

         default:
            return false;       // Raw data and edits are named by their file
      }
   }

   // ---- disk target -------------------------------------------------------
   if ( type == US_DataPub::Experiment )
   {
      QString base = opts.outputDir.trimmed().isEmpty()
                     ? US_Settings::resultDir()
                     : opts.outputDir.trimmed() + "/results";
      QDir    dir( base + "/" + name );

      if ( ! dir.exists() )  return false;

      QStringList xmls = dir.entryList( QStringList( name + ".*.xml" ),
                                        QDir::Files, QDir::Name );

      for ( int ii = 0; ii < xmls.size(); ii++ )
      {
         if ( xmls[ ii ].count( "." ) != 2 )  continue;

         QMap< QString, QString > attrs = first_attributes(
               dir.absoluteFilePath( xmls[ ii ] ), "experiment" );
         guid = attrs.value( "guid" );
         id   = attrs.value( "id", QString( "-1" ) );

         return true;
      }

      // A run directory without an experiment XML still occupies the name
      guid = QString();
      id   = QString( "-1" );

      return true;
   }

   if ( type == US_DataPub::Centerpiece )
   {
      QList< US_AbstractCenterpiece > centerpieces;
      US_AbstractCenterpiece::read_centerpieces( centerpieces );

      for ( int ii = 0; ii < centerpieces.size(); ii++ )
      {
         if ( centerpieces[ ii ].name != name )  continue;

         guid = centerpieces[ ii ].guid;
         id   = QString::number( centerpieces[ ii ].serial_number );
         return true;
      }

      return false;
   }

   if ( type == US_DataPub::RawData  ||  type == US_DataPub::EditedData  ||
        type == US_DataPub::TimeState )
      return false;              // Named by their file inside the run directory

   DiskLayout layout = disk_layout( type );

   if ( layout.prefix.isEmpty() )  return false;

   QString     dir   = diskDir( type );
   QStringList files = QDir( dir ).entryList(
                       QStringList( layout.prefix + "???????.xml" ),
                       QDir::Files, QDir::Name );

   for ( int ii = 0; ii < files.size(); ii++ )
   {
      QString path = dir + "/" + files[ ii ];

      if ( US_DataPubRecords::recordName( path, type ) != name )  continue;

      QMap< QString, QString > attrs = first_attributes( path,
                                                         layout.element );
      guid = attrs.value( layout.guidAttr );
      id   = attrs.value( "id", QString( "-1" ) );

      return true;
   }

   return false;
}

QString US_DataPubImporter::targetFingerprint( US_DataPub::EntityType type,
                                               const QString& guid,
                                               const QString& id )
{
   QString workDir = bundle.stagingPath() + "/.target";
   QDir().mkpath( workDir );

   QString path = workDir + "/" + US_DataPub::typeKey( type ) + ".xml";

   if ( QFile::exists( path ) )  QFile::remove( path );

   if ( opts.target == US_DataPub::TargetDisk )
   {
      if ( type == US_DataPub::Centerpiece )
      {
         QList< US_AbstractCenterpiece > centerpieces;
         US_AbstractCenterpiece::read_centerpieces( centerpieces );

         for ( int ii = 0; ii < centerpieces.size(); ii++ )
         {
            if ( QString::number( centerpieces[ ii ].serial_number ) != id )
               continue;

            US_DataPubRecords::writeCenterpiece( centerpieces[ ii ], path );
            return US_DataPubHash::fingerprint( path );
         }

         return QString();
      }

      DiskLayout layout = disk_layout( type );

      if ( layout.prefix.isEmpty() )  return QString();

      QString     dir   = diskDir( type );
      QStringList files = QDir( dir ).entryList(
                          QStringList( layout.prefix + "???????.xml" ),
                          QDir::Files, QDir::Name );

      for ( int ii = 0; ii < files.size(); ii++ )
      {
         QString fpath = dir + "/" + files[ ii ];
         QMap< QString, QString > attrs = first_attributes( fpath,
                                                            layout.element );

         if ( ! guid.isEmpty()  &&  attrs.value( layout.guidAttr ) != guid )
            continue;

         return US_DataPubHash::fingerprint( fpath );
      }

      return QString();
   }

   if ( dbase == nullptr )  return QString();

   switch ( type )
   {
      case US_DataPub::Project:
      {
         US_Project project;

         if ( project.readFromDB( id.toInt(), dbase ) != US_DB2::OK )
            return QString();

         project.saveToFile( path );
         break;
      }

      case US_DataPub::Buffer:
      {
         US_Buffer buffer;

         if ( ! buffer.readFromDB( dbase, id ) )  return QString();

         buffer.writeToDisk( path );
         break;
      }

      case US_DataPub::Analyte:
      {
         US_Analyte analyte;

         if ( analyte.load( true, guid, dbase ) != US_DB2::OK )
            return QString();

         analyte.write( false, path );
         break;
      }

      case US_DataPub::Solution:
      {
         US_Solution solution;

         if ( solution.readFromDB( id.toInt(), dbase ) != US_DB2::OK )
            return QString();

         solution.saveToFile( path );
         break;
      }

      case US_DataPub::Model:
      {
         US_Model model;

         if ( model.load( id, dbase ) != US_DB2::OK )  return QString();

         model.write( path );
         break;
      }

      case US_DataPub::Noise:
      {
         US_Noise noise;

         if ( noise.load( id, dbase ) != US_DB2::OK )  return QString();

         noise.write( path );
         break;
      }

      case US_DataPub::RotorCalibration:
      {
         US_Rotor::RotorCalibration calibration;

         if ( calibration.readDB( id.toInt(), dbase ) != US_Rotor::ROTOR_OK )
            return QString();

         US_DataPubRecords::writeCalibration( calibration, path );
         break;
      }

      case US_DataPub::Centerpiece:
      {
         QList< US_AbstractCenterpiece > centerpieces;
         US_AbstractCenterpiece::read_centerpieces( dbase, centerpieces );

         bool found = false;

         for ( int ii = 0; ii < centerpieces.size()  &&  ! found; ii++ )
         {
            if ( QString::number( centerpieces[ ii ].serial_number ) != id )
               continue;

            US_DataPubRecords::writeCenterpiece( centerpieces[ ii ], path );
            found = true;
         }

         if ( ! found )  return QString();

         break;
      }

      default:
         return QString();
   }

   return US_DataPubHash::fingerprint( path );
}

QString US_DataPubImporter::uniqueName( US_DataPub::EntityType type,
                                        const QString& name )
{
   QString suffix = opts.renameSuffix.trimmed();

   if ( suffix.isEmpty() )  suffix = tr( "imported" );

   QString base = name.isEmpty() ? US_DataPub::typeText( type ) : name;
   QString guid;
   QString id;

   for ( int ii = 1; ii < 1000; ii++ )
   {
      QString candidate = ( ii == 1 )
                          ? QString( "%1 (%2)" ).arg( base ).arg( suffix )
                          : QString( "%1 (%2 %3)" ).arg( base ).arg( suffix )
                            .arg( ii );

      if ( type == US_DataPub::Experiment )
         candidate = ( ii == 1 )
                     ? QString( "%1-%2" ).arg( base ).arg( suffix )
                     : QString( "%1-%2-%3" ).arg( base ).arg( suffix ).arg( ii );

      if ( ! findByName( type, candidate, guid, id ) )  return candidate;
   }

   return base + "-" + US_Util::new_guid().left( 8 );
}

// ------------------------------------------------------- writing the target

namespace
{
   // Rewrite the identifying references of a run's experiment XML so that
   // it names the records of the target instead of those of the source.
   class ExpRemap
   {
      public:
         QString runID;            // The (possibly renamed) run identifier
         QString expID;            // The experiment ID in the target
         QString projectID;        // The project ID in the target
         QString projectGUID;      // The project GUID in the target
         QMap< QString, QString > solutionGUIDs;  // old GUID -> new GUID
         QMap< QString, QString > solutionIDs;    // new GUID -> new ID
         QMap< QString, QString > centerpieceIDs; // old serial -> new serial
         QString calibrationID;    // The rotor calibration ID in the target
   };

   bool remap_experiment_xml( const QString& path, const ExpRemap& remap )
   {
      QFile in( path );

      if ( ! in.open( QIODevice::ReadOnly ) )  return false;

      QByteArray data = in.readAll();
      in.close();

      QXmlStreamReader reader( data );
      QByteArray       out;
      QXmlStreamWriter writer( &out );
      writer.setAutoFormatting( false );

      while ( ! reader.atEnd() )
      {
         reader.readNext();

         if ( reader.hasError() )                                  return false;
         if ( reader.tokenType() == QXmlStreamReader::NoToken )     continue;
         if ( reader.tokenType() == QXmlStreamReader::Invalid )     break;

         if ( ! reader.isStartElement() )
         {
            writer.writeCurrentToken( reader );
            continue;
         }

         QString              ename = reader.name().toString();
         QXmlStreamAttributes attrs = reader.attributes();
         QMap< QString, QString > values;
         QStringList              order;

         for ( int ii = 0; ii < attrs.size(); ii++ )
         {
            QString aname = attrs.at( ii ).name().toString();
            order  << aname;
            values.insert( aname, attrs.at( ii ).value().toString() );
         }

         if ( ename == "experiment" )
         {
            if ( ! remap.runID.isEmpty() )  values.insert( "runID", remap.runID );
            if ( ! remap.expID.isEmpty() )  values.insert( "id",    remap.expID );
         }

         else if ( ename == "project" )
         {
            if ( ! remap.projectID  .isEmpty() )
               values.insert( "id",   remap.projectID   );
            if ( ! remap.projectGUID.isEmpty() )
               values.insert( "guid", remap.projectGUID );
         }

         else if ( ename == "calibration" )
         {
            if ( ! remap.calibrationID.isEmpty() )
               values.insert( "id", remap.calibrationID );
         }

         else if ( ename == "centerpiece" )
         {
            QString oldID = values.value( "id" );

            if ( remap.centerpieceIDs.contains( oldID ) )
               values.insert( "id", remap.centerpieceIDs.value( oldID ) );
         }

         else if ( ename == "solution" )
         {
            QString oldGUID = values.value( "guid" );
            QString newGUID = remap.solutionGUIDs.value( oldGUID, oldGUID );

            values.insert( "guid", newGUID );

            if ( remap.solutionIDs.contains( newGUID ) )
               values.insert( "id", remap.solutionIDs.value( newGUID ) );
         }

         writer.writeStartElement( ename );

         for ( int ii = 0; ii < order.size(); ii++ )
            writer.writeAttribute( order[ ii ], values.value( order[ ii ] ) );
      }

      QFile outFile( path );

      if ( ! outFile.open( QIODevice::WriteOnly | QIODevice::Truncate ) )
         return false;

      outFile.write( out );
      outFile.close();

      return true;
   }
}

QList< US_DataPub::EntityType > US_DataPubImporter::importOrder( void ) const
{
   QList< US_DataPub::EntityType > types;

   types << US_DataPub::Project
         << US_DataPub::RotorCalibration
         << US_DataPub::Centerpiece
         << US_DataPub::Buffer
         << US_DataPub::Analyte;

   // A database allocates the experiment ID that a new solution record is
   // first associated with, so the experiment goes in before the solutions
   // there.  A disk store has no such association, but it does have the
   // run's experiment XML naming its solutions, so there the solutions go
   // in first and the XML is rewritten to name them.
   if ( opts.target == US_DataPub::TargetDb )
      types << US_DataPub::Experiment << US_DataPub::Solution;
   else
      types << US_DataPub::Solution << US_DataPub::Experiment;

   types << US_DataPub::RawData
         << US_DataPub::TimeState
         << US_DataPub::EditedData
         << US_DataPub::Model
         << US_DataPub::Noise;

   return types;
}

QString US_DataPubImporter::runIdFor( const US_DataPubEntity& entity ) const
{
   QString expGUID = entity.depend( US_DataPub::Experiment );
   QString renamed = nameMap.value( expGUID, QString() );

   if ( ! renamed.isEmpty() )  return renamed;

   return entity.attrs.value( "runID" );
}

QString US_DataPubImporter::renamedFile( const US_DataPubEntity& entity ) const
{
   QString oldRun = entity.attrs.value( "runID" );
   QString newRun = runIdFor( entity );
   QString name   = entity.filename;

   if ( oldRun.isEmpty()  ||  newRun.isEmpty()  ||  oldRun == newRun )
      return name;

   if ( ! name.startsWith( oldRun ) )  return name;

   return newRun + name.mid( oldRun.size() );
}

bool US_DataPubImporter::createDisk( const US_DataPubEntity& entity,
                                     const QString& workPath,
                                     const QString& name, QString& newID,
                                     QString& error )
{
   newID = QString( "-1" );

   US_DataPub::EntityType type = entity.type;

   if ( type == US_DataPub::Centerpiece )
   {
      note( tr( "Centerpiece \"%1\" is not in the local centerpiece table;"
                " the run keeps its serial number %2" )
            .arg( entity.name ).arg( entity.id ) );
      newID = entity.id;
      return true;
   }

   if ( type == US_DataPub::Experiment  ||  type == US_DataPub::RawData  ||
        type == US_DataPub::EditedData  ||  type == US_DataPub::TimeState )
   {
      QString runID = ( type == US_DataPub::Experiment )
                      ? name : runIdFor( entity );

      if ( runID.isEmpty() )
      {
         error = tr( "%1 names no run and cannot be written to disk" )
                 .arg( entity.label() );
         return false;
      }

      QString dir  = diskResultDir( runID, true );
      QString base = ( type == US_DataPub::Experiment )
                     ? runID + "." + entity.attrs.value( "runType", "RA" )
                             + ".xml"
                     : renamedFile( entity );
      QString path = dir + "/" + base;

      if ( QFile::exists( path ) )
      {
         if ( US_DataPubHash::fileHash( path ) == entity.sha256  &&
              ! entity.sha256.isEmpty() )
         {
            note( tr( "%1 is already present in %2 and is left as it is" )
                  .arg( base ).arg( dir ) );
            newID = entity.id;
            return true;
         }

         error = tr( "The file %1 already exists with different content;"
                     " the import will not overwrite it" ).arg( path );
         return false;
      }

      if ( type == US_DataPub::Experiment )
      {
         ExpRemap remap;
         remap.runID       = runID;
         remap.expID       = QString( "-1" );
         remap.projectID   = mappedId  ( entity.depend( US_DataPub::Project ) );
         remap.projectGUID = mappedGuid( entity.depend( US_DataPub::Project ) );
         remap.calibrationID = mappedId(
               entity.depend( US_DataPub::RotorCalibration ) );

         QList< US_DataPubEntity > sols = mani.section( US_DataPub::Solution );

         for ( int ii = 0; ii < sols.size(); ii++ )
         {
            QString newGUID = mappedGuid( sols[ ii ].guid );
            remap.solutionGUIDs.insert( sols[ ii ].guid, newGUID );
            remap.solutionIDs  .insert( newGUID, mappedId( sols[ ii ].guid ) );
         }

         QList< US_DataPubEntity > cps = mani.section( US_DataPub::Centerpiece );

         for ( int ii = 0; ii < cps.size(); ii++ )
            remap.centerpieceIDs.insert( cps[ ii ].id, mappedId( cps[ ii ].guid ) );

         if ( ! remap_experiment_xml( workPath, remap ) )
            note( tr( "The experiment XML of run %1 could not be rewritten;"
                      " it is copied unchanged" ).arg( runID ) );
      }

      if ( ! QFile::copy( workPath, path ) )
      {
         error = tr( "Cannot write %1" ).arg( path );
         return false;
      }

      QFile::setPermissions( path, QFileDevice::ReadOwner
                                   | QFileDevice::WriteOwner
                                   | QFileDevice::ReadGroup
                                   | QFileDevice::ReadOther );

      // A time state carries a sibling file with its field definitions
      if ( type == US_DataPub::TimeState )
      {
         QString defs = entity.attrs.value( "definitionsPayload" );

         if ( ! defs.isEmpty() )
         {
            QString source = bundle.rootPath() + "/" + defs;
            QString target = QString( path ).replace( ".tmst", ".xml" );

            if ( QFile::exists( source )  &&  ! QFile::exists( target ) )
               QFile::copy( source, target );
         }
      }

      newID = entity.id;
      return true;
   }

   // ---- the file-backed records of the data directory ---------------------
   QString dir = diskDir( type, true );
   QString prefix;

   switch ( type )
   {
      case US_DataPub::Project:          prefix = "P";  break;
      case US_DataPub::Buffer:           prefix = "B";  break;
      case US_DataPub::Analyte:          prefix = "A";  break;
      case US_DataPub::Solution:         prefix = "S";  break;
      case US_DataPub::Model:            prefix = "M";  break;
      case US_DataPub::Noise:            prefix = "N";  break;
      case US_DataPub::RotorCalibration: prefix = "C";  break;
      default:                                          break;
   }

   if ( prefix.isEmpty() )
   {
      error = tr( "%1 cannot be written to a disk store" ).arg( entity.label() );
      return false;
   }

   bool    newFile = true;
   QString path    = US_DataFiles::get_filename( dir, QString(), prefix,
                                                 QString(), QString(),
                                                 newFile );

   if ( QFile::exists( path ) )
   {
      error = tr( "The file %1 already exists; the import will not"
                  " overwrite it" ).arg( path );
      return false;
   }

   if ( ! QFile::copy( workPath, path ) )
   {
      error = tr( "Cannot write %1" ).arg( path );
      return false;
   }

   QFile::setPermissions( path, QFileDevice::ReadOwner
                                | QFileDevice::WriteOwner
                                | QFileDevice::ReadGroup
                                | QFileDevice::ReadOther );

   newID = entity.id;

   return true;
}

bool US_DataPubImporter::resolveHardware( US_Experiment& exper,
                                          const US_DataPubEntity& entity )
{
   bool complete = true;

   // ---- the rotor calibration, and through it the rotor and the lab ------
   QString calGUID = entity.depend( US_DataPub::RotorCalibration );
   int     calID   = mappedId( calGUID ).toInt();
   int     rotorID = 0;

   if ( calID > 0 )
   {
      US_Rotor::RotorCalibration calibration;

      if ( calibration.readDB( calID, dbase ) == US_Rotor::ROTOR_OK )
         rotorID = calibration.rotorID;
   }

   if ( rotorID < 1 )
   {
      US_DataPubEntity calEntity;
      QString          rotorGUID;

      if ( mani.entity( US_DataPub::RotorCalibration, calGUID, calEntity ) )
         rotorGUID = calEntity.attrs.value( "rotorGUID" );

      if ( rotorGUID.isEmpty() )  rotorGUID = exper.rotorGUID;

      if ( ! rotorGUID.isEmpty() )
      {
         QStringList query;
         query << "get_rotorID_from_GUID" << rotorGUID;
         dbase->query( query );

         if ( dbase->next() )
            rotorID = dbase->value( 0 ).toString().toInt();
      }
   }

   QVector< US_Rotor::Lab > labs;
   US_Rotor::readLabsDB( labs, dbase );

   if ( rotorID < 1 )
   {  // Fall back to the first rotor the target knows about
      for ( int ii = 0; ii < labs.size()  &&  rotorID < 1; ii++ )
      {
         QVector< US_Rotor::Rotor > rotors;
         US_Rotor::readRotorsFromDB( rotors, labs[ ii ].ID, dbase );

         if ( rotors.isEmpty() )  continue;

         rotorID  = rotors[ 0 ].ID;
         complete = false;
         note( tr( "The rotor of run %1 is not in the target database;"
                   " rotor \"%2\" is used instead" )
               .arg( exper.runID ).arg( rotors[ 0 ].name ) );
      }
   }

   if ( rotorID < 1 )  return false;

   exper.rotorID = rotorID;

   if ( calID < 1 )
   {  // Take the rotor's first calibration profile
      QVector< US_Rotor::RotorCalibration > cals;
      US_Rotor::readCalibrationProfilesDB( cals, rotorID, dbase );

      if ( cals.isEmpty() )  return false;

      calID    = cals[ 0 ].ID;
      complete = false;
      note( tr( "The rotor calibration of run %1 is not in the target"
                " database; calibration %2 is used instead" )
            .arg( exper.runID ).arg( calID ) );
   }

   exper.calibrationID = calID;

   // ---- the lab that owns the rotor --------------------------------------
   int labID = 0;

   for ( int ii = 0; ii < labs.size()  &&  labID < 1; ii++ )
   {
      QVector< US_Rotor::Rotor > rotors;
      US_Rotor::readRotorsFromDB( rotors, labs[ ii ].ID, dbase );

      for ( int jj = 0; jj < rotors.size(); jj++ )
      {
         if ( rotors[ jj ].ID != rotorID )  continue;

         labID = labs[ ii ].ID;
         break;
      }
   }

   if ( labID > 0 )  exper.labID = labID;

   // ---- the instrument ---------------------------------------------------
   QStringList query;
   query << "get_instrument_info" << QString::number( exper.instrumentID );
   dbase->query( query );

   if ( ! dbase->next() )
   {
      US_Rotor::Lab lab;

      if ( labID > 0 )  lab.readDB( labID, dbase );

      if ( ! lab.instruments.isEmpty() )
      {
         exper.instrumentID     = lab.instruments[ 0 ].ID;
         exper.instrumentSerial = lab.instruments[ 0 ].serial;
      }

      complete = false;
      note( tr( "The instrument of run %1 is not in the target database;"
                " instrument %2 is used instead" )
            .arg( exper.runID ).arg( exper.instrumentID ) );
   }

   // ---- the operator -----------------------------------------------------
   query.clear();
   query << "get_person_info" << QString::number( exper.operatorID );
   dbase->query( query );

   if ( ! dbase->next() )
   {
      exper.operatorID = US_Settings::us_inv_ID();
      complete         = false;
      note( tr( "The operator of run %1 is not in the target database;"
                " the current investigator is recorded instead" )
            .arg( exper.runID ) );
   }

   exper.invID = US_Settings::us_inv_ID();

   return complete;
}

bool US_DataPubImporter::createDb( const US_DataPubEntity& entity,
                                   const QString& workPath,
                                   const QString& name, QString& newID,
                                   QString& error )
{
   newID = QString( "-1" );

   if ( dbase == nullptr )
   {
      error = tr( "No database connection" );
      return false;
   }

   switch ( entity.type )
   {
      case US_DataPub::Project:
      {
         US_Project project;

         if ( project.readFromFile( workPath ) != US_DB2::OK )
         {
            error = tr( "The project payload could not be read" );
            return false;
         }

         project.projectID = 0;

         if ( project.saveToDB( dbase ) != US_DB2::OK )
         {
            error = tr( "The project \"%1\" could not be written to the"
                        " database: %2" ).arg( name ).arg( dbase->lastError() );
            return false;
         }

         newID = QString::number( project.projectID );
         return true;
      }

      case US_DataPub::RotorCalibration:
      {
         US_Rotor::RotorCalibration calibration;

         if ( ! US_DataPubRecords::readCalibration( workPath, calibration ) )
         {
            error = tr( "The rotor calibration payload could not be read" );
            return false;
         }

         QStringList query;
         query << "get_rotorID_from_GUID" << calibration.rotorGUID;
         dbase->query( query );

         int rotorID = 0;

         if ( dbase->next() )
            rotorID = dbase->value( 0 ).toString().toInt();

         if ( rotorID < 1 )
         {
            note( tr( "Rotor %1 is not in the target database, so the rotor"
                      " calibration \"%2\" is not imported" )
                  .arg( calibration.rotorGUID ).arg( name ) );
            return true;
         }

         calibration.ID                      = 0;
         calibration.rotorID                 = rotorID;
         calibration.calibrationExperimentID = 0;

         if ( calibration.saveDB( rotorID, dbase ) != US_DB2::OK )
         {
            error = tr( "The rotor calibration \"%1\" could not be written"
                        " to the database: %2" ).arg( name )
                    .arg( dbase->lastError() );
            return false;
         }

         newID = QString::number( calibration.ID );
         return true;
      }

      case US_DataPub::Centerpiece:
      {
         note( tr( "Centerpiece \"%1\" is not in the target centerpiece"
                   " table; the run keeps its serial number %2" )
               .arg( entity.name ).arg( entity.id ) );
         newID = entity.id;
         return true;
      }

      case US_DataPub::Buffer:
      {
         US_Buffer buffer;

         if ( ! buffer.readFromDisk( workPath ) )
         {
            error = tr( "The buffer payload could not be read" );
            return false;
         }

         buffer.bufferID = QString( "-1" );

         if ( buffer.saveToDB( dbase, QString( "1" ) ) < 0 )
         {
            error = tr( "The buffer \"%1\" could not be written to the"
                        " database: %2" ).arg( name ).arg( dbase->lastError() );
            return false;
         }

         newID = buffer.bufferID;
         return true;
      }

      case US_DataPub::Analyte:
      {
         US_Analyte analyte;

         if ( analyte.readFromFile( workPath ) != US_DB2::OK )
         {
            error = tr( "The analyte payload could not be read" );
            return false;
         }

         analyte.analyteID = QString( "-1" );

         if ( analyte.write( true, QString(), dbase ) != US_DB2::OK )
         {
            error = tr( "The analyte \"%1\" could not be written to the"
                        " database: %2" ).arg( name ).arg( analyte.message );
            return false;
         }

         newID = analyte.analyteID;
         return true;
      }

      case US_DataPub::Solution:
      {
         US_Solution solution;

         if ( solution.readFromFile( workPath, false ) != US_DB2::OK )
         {
            error = tr( "The solution payload could not be read" );
            return false;
         }

         // Point the solution at the buffer and analytes already imported
         QString bufGUID = entity.depend( US_DataPub::Buffer );

         if ( ! bufGUID.isEmpty() )
         {
            solution.buffer.GUID     = mappedGuid( bufGUID );
            solution.buffer.bufferID = mappedId  ( bufGUID );
            solution.buffer.readFromDB( dbase, solution.buffer.bufferID );
         }

         for ( int ii = 0; ii < solution.analyteInfo.size(); ii++ )
         {
            QString anaGUID = solution.analyteInfo[ ii ].analyte.analyteGUID;
            QString newGUID = mappedGuid( anaGUID );
            US_Analyte analyte;

            if ( analyte.load( true, newGUID, dbase ) == US_DB2::OK )
            {
               double amount = solution.analyteInfo[ ii ].amount;
               solution.analyteInfo[ ii ].analyte = analyte;
               solution.analyteInfo[ ii ].amount  = amount;
            }
         }

         solution.solutionID   = 0;
         solution.solutionDesc = name;

         int expID = mappedId( entity.depend( US_DataPub::Experiment ) ).toInt();

         if ( expID < 1 )  expID = 1;

         if ( solution.saveToDB( expID, 1, dbase ) != US_DB2::OK )
         {
            error = tr( "The solution \"%1\" could not be written to the"
                        " database: %2" ).arg( name ).arg( dbase->lastError() );
            return false;
         }

         newID = QString::number( solution.solutionID );
         return true;
      }

      case US_DataPub::Experiment:
      {
         US_Experiment exper;
         QList< US_Convert::TripleInfo > triples;
         QString runType = entity.attrs.value( "runType", "RA" );
         QString workDir = QFileInfo( workPath ).absolutePath();

         if ( exper.readFromDisk( triples, runType, entity.name,
                                  workDir + "/" ) != US_Convert::OK )
         {
            error = tr( "The experiment payload of run %1 could not be read" )
                    .arg( entity.name );
            return false;
         }

         QString projGUID = entity.depend( US_DataPub::Project );

         exper.project.clear();

         if ( ! projGUID.isEmpty() )
            exper.project.readFromDB( mappedId( projGUID ).toInt(), dbase );

         if ( exper.project.projectID < 1 )
         {
            error = tr( "The project of run %1 is not in the target database" )
                    .arg( entity.name );
            return false;
         }

         if ( ! resolveHardware( exper, entity ) )
         {
            if ( exper.rotorID < 1  ||  exper.calibrationID < 1 )
            {
               error = tr( "Run %1 cannot be imported: the target database"
                           " has no rotor with a calibration profile" )
                       .arg( entity.name );
               return false;
            }
         }

         exper.runID = name;
         exper.expID = 0;

         QVector< US_SimulationParameters::SpeedProfile > speedsteps;
         int status = exper.saveToDB( false, dbase, speedsteps );

         if ( status != US_DB2::OK )
         {
            error = tr( "Run %1 could not be written to the database"
                        " (status %2): %3" ).arg( name ).arg( status )
                    .arg( dbase->lastError() );
            return false;
         }

         newID = QString::number( exper.expID );
         return true;
      }

      case US_DataPub::RawData:
      {
         QString expID = mappedId( entity.depend( US_DataPub::Experiment ) );
         QString solID = mappedId( entity.depend( US_DataPub::Solution ) );

         if ( expID.toInt() < 1 )
         {
            error = tr( "The experiment of %1 was not imported" )
                    .arg( entity.filename );
            return false;
         }

         QString runID   = runIdFor( entity );
         QString base    = renamedFile( entity );
         QString cell    = entity.attrs.value( "triple" ).section( ".", 0, 0 );
         QString channel = entity.attrs.value( "triple" ).section( ".", 1, 1 );
         QString cpSer   = entity.attrs.value( "centerpieceID" );

         QList< US_DataPubEntity > cps = mani.section( US_DataPub::Centerpiece );

         for ( int ii = 0; ii < cps.size(); ii++ )
            if ( cps[ ii ].id == cpSer )
               cpSer = mappedId( cps[ ii ].guid ) == "-1"
                       ? cpSer : mappedId( cps[ ii ].guid );

         QStringList query;
         query << "new_rawData"
               << entity.guid
               << runID
               << base
               << entity.attrs.value( "triple" )
               << expID
               << ( solID.toInt() > 0 ? solID : QString( "0" ) )
               << QString( "1" );

         int status = dbase->statusQuery( query );

         if ( status != US_DB2::OK )
         {
            error = tr( "The raw data record %1 could not be created: %2" )
                    .arg( base ).arg( dbase->lastError() );
            return false;
         }

         int rawDataID = dbase->lastInsertID();

         if ( dbase->writeBlobToDB( workPath, QString( "upload_aucData" ),
                                    rawDataID ) != US_DB2::OK )
         {
            error = tr( "The raw data of %1 could not be uploaded: %2" )
                    .arg( base ).arg( dbase->lastError() );
            return false;
         }

         // Record the cell/channel/centerpiece of the triple
         QString letters( "SABCDEFGH" );
         int     channelNum = letters.indexOf( channel );

         query.clear();
         query << "new_cell_experiment"
               << US_Util::new_guid()
               << cell
               << QString::number( qMax( 0, channelNum ) )
               << ( cpSer.isEmpty() ? QString( "0" ) : cpSer )
               << expID;
         dbase->statusQuery( query );

         if ( solID.toInt() > 0 )
         {
            query.clear();
            query << "new_experiment_solution" << expID << solID
                  << QString( "1" );
            dbase->statusQuery( query );
         }

         newID = QString::number( rawDataID );
         return true;
      }

      case US_DataPub::TimeState:
      {
         QString expID = mappedId( entity.depend( US_DataPub::Experiment ) );

         if ( expID.toInt() < 1 )
         {
            error = tr( "The experiment of the time state was not imported" );
            return false;
         }

         int tmstID = US_TimeState::dbCreate( dbase, expID.toInt(), workPath );

         if ( tmstID < 0 )
         {
            error = tr( "The time state of run %1 could not be written to"
                        " the database" ).arg( runIdFor( entity ) );
            return false;
         }

         newID = QString::number( tmstID );
         return true;
      }

      case US_DataPub::EditedData:
      {
         QString rawGUID = mappedGuid( entity.depend( US_DataPub::RawData ) );
         QString rawID   = mappedId  ( entity.depend( US_DataPub::RawData ) );

         if ( rawID.toInt() < 1 )
         {
            error = tr( "The raw data of edit %1 was not imported" )
                    .arg( entity.filename );
            return false;
         }

         QString runID = runIdFor( entity );
         QString base  = renamedFile( entity );

         QStringList query;
         query << "new_editedData" << rawID << entity.guid << runID
               << base << entity.attrs.value( "triple" );

         dbase->query( query );

         int editID = dbase->lastInsertID();

         if ( editID < 1 )
         {
            error = tr( "The edit record %1 could not be created: %2" )
                    .arg( base ).arg( dbase->lastError() );
            return false;
         }

         if ( dbase->writeBlobToDB( workPath, QString( "upload_editData" ),
                                    editID ) != US_DB2::OK )
         {
            error = tr( "The edit data of %1 could not be uploaded: %2" )
                    .arg( base ).arg( dbase->lastError() );
            return false;
         }

         Q_UNUSED( rawGUID );

         newID = QString::number( editID );
         return true;
      }

      case US_DataPub::Model:
      {
         US_Model model;

         if ( model.load( workPath ) != US_DB2::OK )
         {
            error = tr( "The model payload could not be read" );
            return false;
         }

         model.editGUID = mappedGuid( entity.depend( US_DataPub::EditedData ) );

         if ( model.write( dbase ) != US_DB2::OK )
         {
            error = tr( "The model \"%1\" could not be written to the"
                        " database: %2" ).arg( name ).arg( model.message );
            return false;
         }

         QStringList query;
         query << "get_modelID" << model.modelGUID;
         dbase->query( query );

         if ( dbase->next() )
            newID = dbase->value( 0 ).toString();

         return true;
      }

      case US_DataPub::Noise:
      {
         US_Noise noise;

         if ( noise.load( workPath ) != US_DB2::OK )
         {
            error = tr( "The noise payload could not be read" );
            return false;
         }

         noise.modelGUID = mappedGuid( entity.depend( US_DataPub::Model ) );

         if ( noise.write( dbase ) != US_DB2::OK )
         {
            error = tr( "The noise record \"%1\" could not be written to"
                        " the database: %2" ).arg( name ).arg( noise.message );
            return false;
         }

         QStringList query;
         query << "get_noiseID" << noise.noiseGUID;
         dbase->query( query );

         if ( dbase->next() )
            newID = dbase->value( 0 ).toString();

         return true;
      }

      default:
         break;
   }

   error = tr( "%1 cannot be imported into a database" ).arg( entity.label() );

   return false;
}
