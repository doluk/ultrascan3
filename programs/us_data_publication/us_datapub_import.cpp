//! \file us_datapub_import.cpp

// The triples of a run hold whole US_Solution values, and US_Solution
// declares a copy assignment without a copy constructor; any change to a
// QList of triples instantiates the implicit one.  That is the library's to
// fix, not something this file can avoid while it edits triples.
#if defined( __GNUC__ )  &&  ! defined( __clang__ )
#pragma GCC diagnostic ignored "-Wdeprecated-copy"
#endif

#include "us_datapub_import.h"
#include "us_datapub_hash.h"
#include "us_datapub_records.h"
#include "us_datapub_export.h"

#include <functional>


#include "us_settings.h"
#include "us_util.h"
#include "us_datafiles.h"
#include "us_dataIO.h"
#include "us_project.h"
#include "us_solution.h"
#include "us_buffer.h"
#include "us_analyte.h"
#include "us_model.h"
#include "us_noise.h"
#include "us_experiment.h"
#include "us_simparms.h"
#include "us_convert.h"
#include "us_convertio.h"
#include "us_hardware.h"
#include "us_rotor.h"
#include "us_time_state.h"
#include "us_eprofile.h"

US_DataPubImporter::Options::Options()
{
   target         = US_DataPub::TargetDb;
   policy         = US_DataPub::PolicyReuse;
   dryRun         = false;
   verifyHashes   = true;
   strictHardware = false;
   renameSuffix   = QObject::tr( "imported" );
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
   resolver    = nullptr;
   dbase       = nullptr;
   ext_db      = nullptr;
   own_db      = false;
   in_txn      = false;
   exp_indexed = false;
   exp_resolved = false;
   tcomps_read = false;
   step_count  = 0;
}

// ------------------------------------------------------ the import tab state

US_DataPubImportState::US_DataPubImportState( bool db )
{
   is_db        = db;
   is_busy      = false;
   is_inspected = false;
}

bool US_DataPubImportState::setTarget( bool db )
{
   if ( db == is_db )  return false;

   is_db = db;

   // A project chosen from the other target's list means nothing here
   if ( proj_guid.isEmpty() )  return false;

   clearProject();
   return true;
}

void US_DataPubImportState::setBusy( bool busy )
{
   is_busy = busy;
}

void US_DataPubImportState::setInspected( bool inspected )
{
   is_inspected = inspected;
}

void US_DataPubImportState::setProject( const QString& guid,
                                        const QString& desc )
{
   proj_guid = guid;
   proj_desc = desc;
}

void US_DataPubImportState::clearProject( void )
{
   proj_guid.clear();
   proj_desc.clear();
}

bool    US_DataPubImportState::db         ( void ) const { return is_db; }
bool    US_DataPubImportState::busy       ( void ) const { return is_busy; }
bool    US_DataPubImportState::inspected  ( void ) const { return is_inspected; }
QString US_DataPubImportState::projectGUID( void ) const { return proj_guid; }
QString US_DataPubImportState::projectDesc( void ) const { return proj_desc; }

bool US_DataPubImportState::controlsEnabled( void ) const
{
   return ! is_busy;
}

bool US_DataPubImportState::importEnabled( void ) const
{
   return ! is_busy  &&  is_inspected;
}

bool US_DataPubImportState::outputDirEnabled( void ) const
{
   return ! is_busy  &&  ! is_db;
}

// ----------------------------------------------------------------- importer

US_DataPubImporter::~US_DataPubImporter()
{
   if ( in_txn )  abortWrite( tr( "the importer was destroyed" ) );

   closeTarget();
   bundle.cleanup();
}

void US_DataPubImporter::setResolver( US_DataPubConflictResolver* a_resolver )
{
   resolver = a_resolver;
}

void US_DataPubImporter::setDatabase( IUS_DB2* db )
{
   ext_db = db;
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

QStringList US_DataPubImporter::warnings( void ) const
{
   return warnlist;
}

QString US_DataPubImporter::outcome( void ) const
{
   return outcome_text;
}

void US_DataPubImporter::note( const QString& text )
{
   logtext << text;
   emit message( text );
}

bool US_DataPubImporter::warn( const QString& text, QString& error )
{
   if ( opts.strictHardware )
   {
      error = text + tr( "; substitutions are not allowed in this import" );
      return false;
   }

   warnlist << text;
   note( tr( "Warning: " ) + text );

   return true;
}

bool US_DataPubImporter::runIdIsUsable( const QString& runID )
{
   // The rule of US_ConvertIO::readDiskRun(), which keeps it to itself
   return US_DataPub::runIdIsUsable( runID );
}

bool US_DataPubImporter::guidIsUsable( const QString& guid )
{
   // Anchored on the whole text: "$" would let a trailing newline through
   static const QRegularExpression rx( QRegularExpression::anchoredPattern(
      "[0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}"
      "-[0-9A-Fa-f]{12}" ) );

   return rx.match( guid ).hasMatch();
}

QString US_DataPubImporter::resolutionText( US_DataPub::Resolution resolution )
{
   return US_DataPub::resolutionText( resolution );
}

// ------------------------------------------------------------ small helpers

namespace
{
   // Read a whole (XML) file, refusing one larger than the fingerprint cap
   bool read_capped( const QString& path, QByteArray& data, QString& error )
   {
      QFile file( path );

      if ( ! file.open( QIODevice::ReadOnly ) )
      {
         error = QObject::tr( "Cannot read %1" ).arg( path );
         return false;
      }

      data = file.read( US_DataPubHash::maxXmlSize + 1 );
      file.close();

      if ( data.size() > US_DataPubHash::maxXmlSize )
      {
         error = QObject::tr( "%1 is larger than the %2 MiB an XML record"
                              " may be" ).arg( path )
                 .arg( US_DataPubHash::maxXmlSize / ( 1024 * 1024 ) );
         data.clear();
         return false;
      }

      return true;
   }

   // Edit the attributes of an XML file in place.  The callback sees every
   // start element with its attributes and may change their values; the file
   // is rewritten only when it changed one, so a payload nothing had to be
   // done to stays byte for byte what the bundle carried.
   typedef std::function< void( const QString& element,
                                QMap< QString, QString >& values ) > AttrEdit;

   bool rewrite_xml( const QString& path, AttrEdit edit, QString& error )
   {
      QByteArray data;

      if ( ! read_capped( path, data, error ) )  return false;

      QXmlStreamReader reader( data );
      QByteArray       out;
      QXmlStreamWriter writer( &out );
      writer.setAutoFormatting( false );
      bool             changed = false;

      while ( ! reader.atEnd() )
      {
         reader.readNext();

         if ( reader.hasError() )
         {
            error = QObject::tr( "%1 is not well-formed XML: %2" )
                    .arg( QFileInfo( path ).fileName() )
                    .arg( reader.errorString() );
            return false;
         }

         if ( reader.tokenType() == QXmlStreamReader::NoToken )   continue;
         if ( reader.tokenType() == QXmlStreamReader::Invalid )   break;

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

         QMap< QString, QString > before = values;
         edit( ename, values );

         if ( values != before )  changed = true;

         writer.writeStartElement( ename );

         for ( int ii = 0; ii < order.size(); ii++ )
            writer.writeAttribute( order[ ii ], values.value( order[ ii ] ) );

         // An edit may only change what is there; anything it added goes last
         QStringList keys = values.keys();

         for ( int ii = 0; ii < keys.size(); ii++ )
            if ( ! order.contains( keys[ ii ] ) )
               writer.writeAttribute( keys[ ii ], values.value( keys[ ii ] ) );
      }

      if ( ! changed )  return true;

      QFile outFile( path );

      if ( ! outFile.open( QIODevice::WriteOnly | QIODevice::Truncate ) )
      {
         error = QObject::tr( "Cannot rewrite %1" ).arg( path );
         return false;
      }

      bool ok = ( outFile.write( out ) == out.size() );
      outFile.close();

      if ( ! ok )  error = QObject::tr( "Cannot rewrite %1" ).arg( path );

      return ok;
   }

   // The element and attribute holding a record's own GUID in its payload
   void own_identity( US_DataPub::EntityType type, QString& element,
                      QString& attr )
   {
      element.clear();
      attr   .clear();

      switch ( type )
      {
         case US_DataPub::Project:
            element = "project";              attr = "guid";         break;
         case US_DataPub::Buffer:
            element = "buffer";               attr = "guid";         break;
         case US_DataPub::Analyte:
            element = "analyte";              attr = "analyteGUID";  break;
         case US_DataPub::Solution:
            element = "solution";             attr = "guid";         break;
         case US_DataPub::Model:
            element = "model";                attr = "modelGUID";    break;
         case US_DataPub::Noise:
            element = "noise";                attr = "noiseGUID";    break;
         case US_DataPub::RotorCalibration:
            element = "Calibration";          attr = "guid";         break;
         case US_DataPub::Centerpiece:
            element = "abstractCenterpiece";  attr = "guid";         break;
         default:
            break;
      }
   }

   // The labs of a database, read as US_Rotor::readLabsDB() reads them but
   // without the copy that call writes into the local disk store: an import
   // into a database leaves the local store alone
   US_Rotor::Status read_labs_db( QVector< US_Rotor::Lab >& labs, IUS_DB2* db )
   {
      labs.clear();

      QStringList query( "get_lab_names" );
      db->query( query );

      int status = db->lastErrno();

      if ( status != IUS_DB2::OK  &&  status != IUS_DB2::NOROWS )
         return US_Rotor::MISC_ERROR;

      QStringList ids;

      while ( status == IUS_DB2::OK  &&  db->next() )
         ids << db->value( 0 ).toString();

      for ( int ii = 0; ii < ids.size(); ii++ )
      {
         US_Rotor::Lab    lab;
         US_Rotor::Status lstat = lab.readDB( ids[ ii ].toInt(), db );

         if ( lstat != US_Rotor::ROTOR_OK )  return US_Rotor::MISC_ERROR;

         labs << lab;
      }

      return labs.isEmpty() ? US_Rotor::NOT_FOUND : US_Rotor::ROTOR_OK;
   }

   // A single file name that is safe on every platform: no directory part,
   // no control or invisible character, no device name (the rule of
   // US_DataPub::isSafeFileName(), which every name the import writes --
   // a renamed run's files included -- has to pass)
   bool plain_name( const QString& name )
   {
      return US_DataPub::isSafeFileName( name );
   }

   // The value of the "value" attribute of the first element of a name
   QString element_value( const QByteArray& xml, const QString& element,
                          const QString& attr, bool& wellFormed )
   {
      QXmlStreamReader reader( xml );
      QString          value;
      bool             found = false;

      while ( ! reader.atEnd() )
      {
         reader.readNext();

         if ( ! found  &&  reader.isStartElement()  &&
              reader.name().toString() == element )
         {
            value = reader.attributes().value( attr ).toString();
            found = true;
         }
      }

      wellFormed = ! reader.hasError();

      return value;
   }
}

// --------------------------------------------------------- reading a bundle

bool US_DataPubImporter::inspect( const QString& bundlePath, QString& error )
{
   mani.clear();
   logtext.clear();
   warnlist.clear();
   reslist.clear();
   outcome_text.clear();

   if ( ! bundle.unpack( bundlePath, error ) )  return false;

   QString manifestPath = bundle.rootPath() + "/"
                          + US_DataPubBundle::manifestName();

   if ( ! mani.read( manifestPath, error ) )  return false;

   if ( mani.version > US_DataPubManifest::currentVersion )
   {
      error = tr( "The bundle was written with manifest format %1;"
                  " this build understands up to format %2" )
              .arg( mani.version ).arg( US_DataPubManifest::currentVersion );
      mani.clear();
      return false;
   }

   if ( ! mani.validate( error )  ||  ! checkBundle( error ) )
   {
      mani.clear();
      return false;
   }

   note( tr( "Bundle %1 holds %2 records, scope \"%3\", from the %4" )
         .arg( QFileInfo( bundlePath ).fileName() ).arg( mani.total() )
         .arg( mani.scope ).arg( mani.source ) );

   return true;
}

/* Resolve a bundle-relative path, refusing anything that leaves the bundle.

   The manifest is data from outside: a payload of "../../.ssh/keys" or
   "/etc/passwd" would otherwise be read, copied and uploaded like any other.
   The check is made on the text (relative, no "..", no empty or "." parts)
   and, for a path that exists, again on where it really leads, since a
   bundle could carry a symbolic link.
*/
bool US_DataPubImporter::insideBundle( const QString& relPath,
                                       QString& absPath,
                                       QString& error ) const
{
   absPath.clear();

   if ( relPath.isEmpty()  ||  relPath.startsWith( '/' )  ||
        relPath.contains( '\\' )  ||  relPath.contains( ':' )  ||
        relPath.contains( QChar( 0 ) )  ||  QDir::isAbsolutePath( relPath ) )
   {
      error = tr( "The bundle path \"%1\" is not a relative path inside the"
                  " bundle" ).arg( relPath );
      return false;
   }

   QStringList parts = relPath.split( '/' );

   for ( int ii = 0; ii < parts.size(); ii++ )
   {
      if ( parts[ ii ].isEmpty()  ||  parts[ ii ] == "."  ||
           parts[ ii ] == ".." )
      {
         error = tr( "The bundle path \"%1\" leaves the bundle or is not"
                     " plain" ).arg( relPath );
         return false;
      }
   }

   QString root = QDir::cleanPath( bundle.rootPath() );
   QString path = QDir::cleanPath( root + "/" + relPath );

   if ( ! path.startsWith( root + "/" ) )
   {
      error = tr( "The bundle path \"%1\" leaves the bundle" ).arg( relPath );
      return false;
   }

   QFileInfo info( path );

   if ( info.exists()  ||  info.isSymLink() )
   {
      QString realRoot = QFileInfo( root ).canonicalFilePath();
      QString realPath = info.canonicalFilePath();

      if ( info.isSymLink()  ||  realPath.isEmpty()  ||
           ! realPath.startsWith( realRoot + "/" ) )
      {
         error = tr( "The bundle path \"%1\" leads outside the bundle" )
                 .arg( relPath );
         return false;
      }
   }

   absPath = path;

   return true;
}

/* Check what the manifest names before anything is read through it.

   Every path field has to stay inside the bundle, every run ID has to be one
   a run directory can be called, and every file that goes into a run
   directory has to be a plain file name that starts with its run ID.  A
   bundle that breaks any of these is refused as a whole.
*/
bool US_DataPubImporter::checkBundle( QString& error ) const
{
   QList< US_DataPubEntity > entities = mani.all();
   QMap< QString, QString >  expRuns;          // experiment GUID -> run ID

   QList< US_DataPubEntity > exps = mani.section( US_DataPub::Experiment );

   for ( int ii = 0; ii < exps.size(); ii++ )
      expRuns.insert( exps[ ii ].guid, exps[ ii ].name );

   // Anchored on the whole text: "^...$" would take "RA\n" for "RA"
   static const QRegularExpression runTypeRx(
      QRegularExpression::anchoredPattern( "[A-Za-z0-9]{1,8}" ) );

   for ( int ii = 0; ii < entities.size(); ii++ )
   {
      const US_DataPubEntity& entity = entities[ ii ];
      QString                 path;

      if ( ! insideBundle( entity.payload, path, error ) )
      {
         error = tr( "%1: %2" ).arg( entity.label() ).arg( error );
         return false;
      }

      if ( ! entity.filename.isEmpty()  &&  ! plain_name( entity.filename ) )
      {
         error = tr( "%1: the file name \"%2\" is not a plain file name" )
                 .arg( entity.label() ).arg( entity.filename );
         return false;
      }

      US_DataPub::EntityType type = entity.type;

      // A GUID ends up in procedure arguments, file contents and lookups;
      // the manifest's own and every one it names have to be real GUIDs
      if ( ! guidIsUsable( entity.guid ) )
      {
         error = tr( "%1: \"%2\" is not a GUID" ).arg( entity.label() )
                 .arg( entity.guid );
         return false;
      }

      QList< QString > dkeys = entity.depends.keys();

      for ( int jj = 0; jj < dkeys.size(); jj++ )
      {
         QString dguid = entity.depends.value( dkeys[ jj ] );

         if ( guidIsUsable( dguid ) )  continue;

         error = tr( "%1: the %2 it depends on, \"%3\", is not a GUID" )
                 .arg( entity.label() ).arg( dkeys[ jj ] ).arg( dguid );
         return false;
      }

      if ( type == US_DataPub::Experiment )
      {
         if ( ! runIdIsUsable( entity.name ) )
         {
            error = tr( "The run ID \"%1\" in the bundle is not usable: a run"
                        " ID may hold only letters, digits, the underscore"
                        " and the hyphen, and at most 80 of them" )
                    .arg( entity.name );
            return false;
         }

         QString runType = entity.attrs.value( "runType", "RA" );

         if ( ! runTypeRx.match( runType ).hasMatch() )
         {
            error = tr( "Run %1 has the unusable run type \"%2\"" )
                    .arg( entity.name ).arg( runType );
            return false;
         }
      }

      if ( type == US_DataPub::RawData  ||  type == US_DataPub::EditedData  ||
           type == US_DataPub::TimeState )
      {
         QString runID = entity.attrs.value( "runID" );

         if ( ! runIdIsUsable( runID ) )
         {
            error = tr( "%1 names the unusable run ID \"%2\"" )
                    .arg( entity.label() ).arg( runID );
            return false;
         }

         QString expGUID = entity.depend( US_DataPub::Experiment );

         if ( expRuns.contains( expGUID )  &&  expRuns.value( expGUID ) != runID )
         {
            error = tr( "%1 names run %2, but its experiment is run %3" )
                    .arg( entity.label() ).arg( runID )
                    .arg( expRuns.value( expGUID ) );
            return false;
         }

         if ( ! plain_name( entity.filename )  ||
              ! entity.filename.startsWith( runID + "." ) )
         {
            error = tr( "%1: the file name \"%2\" does not start with its run"
                        " ID %3" ).arg( entity.label() )
                    .arg( entity.filename ).arg( runID );
            return false;
         }

         // A time state is always the pair <runID>.time_state.tmst/.xml
         if ( type == US_DataPub::TimeState  &&
              entity.filename != runID + ".time_state.tmst" )
         {
            error = tr( "%1: the file name \"%2\" is not %3.time_state.tmst" )
                    .arg( entity.label() ).arg( entity.filename ).arg( runID );
            return false;
         }
      }

      // Every extra payload -- a time state's definitions, an RI profile,
      // a buffer's components -- is a path into the bundle as well
      QStringList keys = entity.payloadKeys();

      for ( int jj = 0; jj < keys.size(); jj++ )
      {
         if ( keys[ jj ] == "payload" )  continue;

         if ( ! insideBundle( entity.attrs.value( keys[ jj ] ), path, error ) )
         {
            error = tr( "%1: %2" ).arg( entity.label() ).arg( error );
            return false;
         }
      }

      if ( type == US_DataPub::TimeState  &&
           entity.attrs.value( "definitionsPayload" ).isEmpty() )
      {
         error = tr( "%1 carries no field definitions" ).arg( entity.label() );
         return false;
      }
   }

   error.clear();

   return true;
}

QString US_DataPubImporter::payloadPath( const US_DataPubEntity& entity ) const
{
   QString path;
   QString error;

   // checkBundle() refused anything else already; this is the second line
   if ( ! insideBundle( entity.payload, path, error ) )  return QString();

   return path;
}

QStringList US_DataPubImporter::verifyPayloads( void ) const
{
   QStringList               problems;
   QList< US_DataPubEntity > entities = mani.all();

   // The manifest itself first: manifest.sha256 holds its digest in the
   // format sha256sum reads
   QString root     = bundle.rootPath();
   QString digests  = root + "/" + US_DataPubExporter::manifestDigestName();
   QString manifest = root + "/" + US_DataPubBundle::manifestName();
   QFile   dfile( digests );

   if ( ! dfile.open( QIODevice::ReadOnly | QIODevice::Text ) )
      problems << tr( "The bundle carries no %1, so its manifest cannot be"
                      " verified" ).arg( US_DataPubExporter::manifestDigestName() );

   else
   {
      QString line   = QString::fromUtf8( dfile.readLine( 4096 ) ).trimmed();
      QString digest = line.section( ' ', 0, 0 ).toLower();
      QString named  = line.section( ' ', -1 ).remove( '*' );
      dfile.close();

      if ( digest.isEmpty()  ||  named != US_DataPubBundle::manifestName() )
         problems << tr( "%1 does not name the manifest's digest" )
                     .arg( US_DataPubExporter::manifestDigestName() );

      else if ( US_DataPubHash::fileHash( manifest ) != digest )
         problems << tr( "The manifest does not match its digest" );
   }

   for ( int ii = 0; ii < entities.size(); ii++ )
   {
      const US_DataPubEntity& entity = entities[ ii ];
      QStringList             keys   = entity.payloadKeys();

      if ( ! keys.contains( "payload" ) )
         problems << tr( "%1 has no payload" ).arg( entity.label() );

      // The main payload and every extra one, each against its own digest
      for ( int jj = 0; jj < keys.size(); jj++ )
      {
         bool    main   = ( keys[ jj ] == "payload" );
         QString rel    = main ? entity.payload
                               : entity.attrs.value( keys[ jj ] );
         QString digest = main ? entity.sha256
                               : entity.attrs.value(
                                 US_DataPub::digestKeyOf( keys[ jj ] ) );
         QString path;
         QString error;

         if ( ! insideBundle( rel, path, error ) )
         {
            problems << tr( "%1: %2" ).arg( entity.label() ).arg( error );
            continue;
         }

         if ( ! QFile::exists( path ) )
         {
            problems << tr( "%1: the payload %2 is missing" )
                        .arg( entity.label() ).arg( rel );
            continue;
         }

         // A payload without a digest cannot be verified, which is not the
         // same as being intact.  A digest is hexadecimal, in either case.
         if ( digest.isEmpty() )
            problems << tr( "%1: the payload %2 carries no digest" )
                        .arg( entity.label() ).arg( rel );

         else if ( US_DataPubHash::fileHash( path ).toLower()
                   != digest.toLower() )
            problems << tr( "%1: the payload %2 does not match its digest" )
                        .arg( entity.label() ).arg( rel );
      }

      if ( entity.type == US_DataPub::TimeState  &&
           entity.attrs.value( "definitionsPayload" ).isEmpty() )
         problems << tr( "%1 carries no field definitions" )
                     .arg( entity.label() );
   }

   return problems;
}

/* Parse every payload as what the manifest says it is, before anything is
   written.

   The library writers update a record by the GUID inside the payload, so a
   payload whose GUID differs from the manifest's could overwrite some other
   record of the target.  A payload that does not parse, or names a parent
   the import cannot provide, would otherwise be found half way through.
*/
bool US_DataPubImporter::preflight( QString& error )
{
   QList< US_DataPubEntity > entities = mani.all();

   for ( int ii = 0; ii < entities.size(); ii++ )
   {
      if ( ! checkPayload( entities[ ii ], error ) )  return false;

      if ( ! lookup_error.isEmpty() )
      {
         error = lookup_error;
         return false;
      }
   }

   note( tr( "All %1 payloads parse and carry the identity the manifest"
             " gives them" ).arg( entities.size() ) );

   // The runs are looked up now, before any transaction is open
   if ( ! resolveExperiments( error ) )  return false;

   return true;
}

namespace
{
   // The first GUID of an XML payload that is not one: an attribute named
   // "guid" or "...GUID", or the "value" of an element named "...GUID" (the
   // way an edit names itself and its raw data).  Empty values are left to
   // the record's own checks; an empty result means every GUID is usable.
   QString bad_guid_in( const QByteArray& xml )
   {
      QXmlStreamReader reader( xml );

      while ( ! reader.atEnd() )
      {
         reader.readNext();

         if ( ! reader.isStartElement() )  continue;

         QString              ename = reader.name().toString();
         QXmlStreamAttributes attrs = reader.attributes();

         for ( int ii = 0; ii < attrs.size(); ii++ )
         {
            QString aname = attrs.at( ii ).name().toString();
            QString value = attrs.at( ii ).value().toString();
            bool    isRef = ( aname == "guid"  ||  aname.endsWith( "GUID" )  ||
                              ( aname == "value"  &&  ename.endsWith( "GUID" ) ) );

            if ( isRef  &&  ! value.isEmpty()  &&
                 ! US_DataPubImporter::guidIsUsable( value ) )
               return value;
         }
      }

      return QString();
   }

   // A GUID of all zeros names no record
   bool null_guid( const QString& guid )
   {
      return guid.isEmpty()  ||
             guid == QString( "00000000-0000-0000-0000-000000000000" );
   }
}

/* Find the bundle's runs in the target database.

   No procedure looks an experiment up by its GUID, and the listings of
   experiments are the investigator's own.  So a run is asked for by its run
   ID first -- one query, which settles a run the investigator already holds
   -- and only a run that this leaves open is looked for in the whole list
   of the investigator's experiments (a run that was renamed in the target).
   A run that is not the investigator's, but whose raw data the database
   holds (the raw data lookup is not limited to one investigator), belongs
   to someone else: the import refuses it, rather than create a second
   experiment of that GUID or attach the data to another person's records.
*/
bool US_DataPubImporter::resolveExperiments( QString& error )
{
   if ( exp_resolved )  return true;

   if ( opts.target != US_DataPub::TargetDb  ||  dbase == nullptr )
   {
      exp_resolved = true;
      return true;
   }

   QList< US_DataPubEntity > exps = mani.section( US_DataPub::Experiment );
   QList< US_DataPubEntity > raws = mani.section( US_DataPub::RawData );
   QString                   invID = QString::number( US_Settings::us_inv_ID() );

   for ( int ii = 0; ii < exps.size(); ii++ )
   {
      const US_DataPubEntity& exp = exps[ ii ];

      // ---- by its run ID, among the investigator's runs ------------------
      QStringList query;
      query << "get_experiment_info_by_runID" << exp.name << invID;
      dbase->query( query );

      if ( lookupFailed( tr( "run %1" ).arg( exp.name ) ) )
      {
         error = lookup_error;
         return false;
      }

      bool first = true;

      run_lookup.insert( exp.name, QPair< QString, QString >() );

      while ( dbase->next() )
      {
         if ( first )
            run_lookup.insert( exp.name,
                               qMakePair( dbase->value( 1 ).toString(),
                                          dbase->value( 2 ).toString() ) );

         first = false;

         if ( dbase->value( 2 ).toString().toLower() != exp.guid.toLower() )
            continue;

         exp_by_guid.insert( exp.guid,
                             qMakePair( dbase->value( 1 ).toString(),
                                        exp.name ) );
      }

      if ( exp_by_guid.contains( exp.guid ) )  continue;

      // ---- in the investigator's whole list --------------------------------
      indexExperiments();

      if ( ! lookup_error.isEmpty() )
      {
         error = lookup_error;
         return false;
      }

      if ( exp_by_guid.contains( exp.guid ) )  continue;

      // ---- held by someone else? -------------------------------------------
      for ( int jj = 0; jj < raws.size(); jj++ )
      {
         if ( raws[ jj ].depend( US_DataPub::Experiment ) != exp.guid )
            continue;

         query.clear();
         query << "get_rawDataID_from_GUID" << raws[ jj ].guid;
         dbase->query( query );

         if ( lookupFailed( tr( "raw data %1" ).arg( raws[ jj ].guid ) ) )
         {
            error = lookup_error;
            return false;
         }

         if ( ! dbase->next() )  continue;

         error = tr( "Run %1 (experiment %2) is already in the target"
                     " database, but not among the runs of investigator %3:"
                     " its raw data %4 belongs to another investigator's run."
                     "  The import neither creates a second copy of that run"
                     " nor attaches anything to someone else's records; import"
                     " as the investigator who owns the run, or ask them to"
                     " share it" )
                 .arg( exp.name ).arg( exp.guid ).arg( invID )
                 .arg( raws[ jj ].filename );
         return false;
      }
   }

   exp_resolved = true;

   return true;
}

// True when a record of a type is in the bundle or already in the target
bool US_DataPubImporter::availableAs( US_DataPub::EntityType type,
                                      const QString& guid, QString& error )
{
   US_DataPubEntity entity;

   if ( mani.entity( type, guid, entity ) )  return true;

   QString id;
   QString name;

   if ( findByGuid( type, guid, id, name ) )  return true;

   if ( ! lookup_error.isEmpty() )
      error = lookup_error;
   else
      error = tr( "%1 %2 is neither in the bundle nor in the target" )
              .arg( US_DataPub::typeText( type ) ).arg( guid );

   return false;
}

bool US_DataPubImporter::checkPayload( const US_DataPubEntity& entity,
                                       QString& error )
{
   QString path = payloadPath( entity );
   QString label = entity.label();

   if ( path.isEmpty()  ||  ! QFile::exists( path ) )
   {
      error = tr( "The payload of %1 is missing from the bundle" ).arg( label );
      return false;
   }

   // The library readers read an XML record whole; one far beyond any real
   // record is refused before they do
   if ( US_DataPubHash::isXmlFile( path )  &&
        QFileInfo( path ).size() > US_DataPubHash::maxXmlSize )
   {
      error = tr( "The payload of %1 is larger than the %2 MiB an XML record"
                  " may be" ).arg( label )
              .arg( US_DataPubHash::maxXmlSize / ( 1024 * 1024 ) );
      return false;
   }

   // Every GUID a payload names -- its own and its parents' -- goes into a
   // lookup or a procedure; one that is not a GUID refuses the bundle
   if ( US_DataPubHash::isXmlFile( path ) )
   {
      QByteArray data;

      if ( ! read_capped( path, data, error ) )  return false;

      QString bad = bad_guid_in( data );

      if ( ! bad.isEmpty() )
      {
         error = tr( "The payload of %1 names \"%2\", which is not a GUID;"
                     " the bundle is refused" ).arg( label ).arg( bad );
         return false;
      }
   }

   QString found;                  // The GUID inside the payload
   QString what;                   // What the payload failed to parse as

   switch ( entity.type )
   {
      case US_DataPub::Project:
      {
         US_Project project;

         // US_Project reads a file without a project element as an empty
         // project, so the element is looked for first
         QByteArray data;
         bool       wellFormed = false;

         if ( ! read_capped( path, data, error ) )  return false;

         element_value( data, "project", "guid", wellFormed );

         if ( ! wellFormed  ||  ! data.contains( "<project" )  ||
              project.readFromFile( path ) != IUS_DB2::OK )
            what = tr( "a project" );

         found = project.projectGUID;
         break;
      }

      case US_DataPub::Buffer:
      {
         US_Buffer buffer;

         if ( ! buffer.readFromDisk( path ) )
         {
            what = tr( "a buffer" );
            break;
         }

         found = buffer.GUID;

         if ( found == entity.guid  &&
              ! checkComponents( entity, buffer, error ) )
            return false;

         break;
      }

      case US_DataPub::Analyte:
      {
         US_Analyte analyte;

         if ( analyte.readFromFile( path ) != IUS_DB2::OK )
            what = tr( "an analyte" );

         found = analyte.analyteGUID;
         break;
      }

      case US_DataPub::Solution:
      {
         US_Solution solution;

         if ( solution.readFromFile( path, false ) != IUS_DB2::OK )
         {
            what = tr( "a solution" );
            break;
         }

         found = solution.solutionGUID;

         // Everything a solution names has to be there when it is written
         QString bufGUID = entity.depend( US_DataPub::Buffer );

         if ( ! bufGUID.isEmpty()  &&  solution.buffer.GUID != bufGUID )
         {
            error = tr( "%1 names buffer %2, but its manifest entry buffer %3" )
                    .arg( label ).arg( solution.buffer.GUID ).arg( bufGUID );
            return false;
         }

         if ( ! solution.buffer.GUID.isEmpty()  &&
              ! availableAs( US_DataPub::Buffer, solution.buffer.GUID, error ) )
         {
            error = tr( "%1: %2" ).arg( label ).arg( error );
            return false;
         }

         for ( int jj = 0; jj < solution.analyteInfo.size(); jj++ )
         {
            QString anaGUID = solution.analyteInfo[ jj ].analyte.analyteGUID;

            if ( availableAs( US_DataPub::Analyte, anaGUID, error ) )  continue;

            error = tr( "%1: %2" ).arg( label ).arg( error );
            return false;
         }

         break;
      }

      case US_DataPub::Model:
      {
         US_Model model;

         if ( model.load( path ) != IUS_DB2::OK )
         {
            what = tr( "a model" );
            break;
         }

         found = model.modelGUID;
         QString editGUID = entity.depend( US_DataPub::EditedData );

         if ( ! editGUID.isEmpty()  &&  model.editGUID != editGUID )
         {
            error = tr( "%1 names edit %2, but its manifest entry edit %3" )
                    .arg( label ).arg( model.editGUID ).arg( editGUID );
            return false;
         }

         // An edit the manifest does not tie it to has to be in the bundle
         // or the target all the same, as a noise record's model has to
         if ( editGUID.isEmpty()  &&  ! null_guid( model.editGUID )  &&
              ! availableAs( US_DataPub::EditedData, model.editGUID, error ) )
         {
            error = tr( "%1: %2" ).arg( label ).arg( error );
            return false;
         }

         break;
      }

      case US_DataPub::Noise:
      {
         US_Noise noise;

         if ( noise.load( path ) != IUS_DB2::OK )
         {
            what = tr( "a noise record" );
            break;
         }

         found = noise.noiseGUID;
         QString modelGUID = entity.depend( US_DataPub::Model );

         if ( ! modelGUID.isEmpty()  &&  noise.modelGUID != modelGUID )
         {
            error = tr( "%1 names model %2, but its manifest entry model %3" )
                    .arg( label ).arg( noise.modelGUID ).arg( modelGUID );
            return false;
         }

         if ( modelGUID.isEmpty()  &&  ! noise.modelGUID.isEmpty()  &&
              ! availableAs( US_DataPub::Model, noise.modelGUID, error ) )
         {
            error = tr( "%1: %2" ).arg( label ).arg( error );
            return false;
         }

         break;
      }

      case US_DataPub::RotorCalibration:
      {
         US_Rotor::RotorCalibration calibration;

         if ( ! US_DataPubRecords::readCalibration( path, calibration ) )
            what = tr( "a rotor calibration" );

         found = calibration.GUID;
         break;
      }

      case US_DataPub::Centerpiece:
      {
         US_AbstractCenterpiece centerpiece;

         if ( ! US_DataPubRecords::readCenterpiece( path, centerpiece ) )
            what = tr( "a centerpiece" );

         found = centerpiece.guid;
         break;
      }

      case US_DataPub::Experiment:
      {
         QByteArray data;
         bool       wellFormed = false;

         if ( ! read_capped( path, data, error ) )  return false;

         found = element_value( data, "experiment", "guid", wellFormed );
         QString runID = element_value( data, "experiment", "runID",
                                        wellFormed );

         if ( ! wellFormed  ||  ! data.contains( "<experiment" ) )
         {
            what = tr( "an experiment" );
            break;
         }

         if ( runID != entity.name )
         {
            error = tr( "The experiment XML of run %1 names run \"%2\"" )
                    .arg( entity.name ).arg( runID );
            return false;
         }

         // An RI run's intensity profile travels beside its XML
         QString rip = entity.attrs.value( "riProfilePayload" );

         if ( ! rip.isEmpty() )
         {
            QString    rpath;
            QByteArray rdata;
            QString    why;

            if ( ! insideBundle( rip, rpath, error )  ||
                 ! read_capped( rpath, rdata, error ) )
               return false;

            if ( ! US_DataPubRecords::riProfileIsUsable( rdata, &why ) )
            {
               error = tr( "The RI intensity profile of run %1 cannot be"
                           " used: %2" ).arg( entity.name ).arg( why );
               return false;
            }
         }

         break;
      }

      case US_DataPub::RawData:
      {
         US_DataIO::RawData data;

         if ( US_DataIO::readRawData( path, data ) != US_DataIO::OK )
         {
            what = tr( "raw data" );
            break;
         }

         found = US_Util::uuid_unparse( (unsigned char*)data.rawGUID );
         break;
      }

      case US_DataPub::EditedData:
      {
         QByteArray data;
         bool       wellFormed = false;

         if ( ! read_capped( path, data, error ) )  return false;

         found = element_value( data, "editGUID", "value", wellFormed );
         QString rawGUID = element_value( data, "rawDataGUID", "value",
                                          wellFormed );

         if ( ! wellFormed )
         {
            what = tr( "an edit" );
            break;
         }

         QString wanted = entity.depend( US_DataPub::RawData );

         if ( ! wanted.isEmpty()  &&  rawGUID != wanted )
         {
            error = tr( "%1 belongs to raw data %2, but its manifest entry"
                        " to %3" ).arg( label ).arg( rawGUID ).arg( wanted );
            return false;
         }

         break;
      }

      case US_DataPub::TimeState:
      {
         QString defs;
         QString dpath;

         defs = entity.attrs.value( "definitionsPayload" );

         if ( defs.isEmpty()  ||  ! insideBundle( defs, dpath, error )  ||
              ! QFile::exists( dpath ) )
         {
            error = tr( "The field definitions of %1 are missing from the"
                        " bundle" ).arg( label );
            return false;
         }

         QByteArray data;
         bool       wellFormed = false;

         if ( ! read_capped( dpath, data, error ) )  return false;

         element_value( data, "TimeState", "version", wellFormed );

         if ( ! wellFormed  ||  ! data.contains( "<TimeState" )  ||
              QFileInfo( path ).size() < 1 )
            what = tr( "a time state" );

         found = entity.guid;    // A time state carries no GUID of its own
         break;
      }

      default:
         what = tr( "a known record type" );
         break;
   }

   if ( ! what.isEmpty() )
   {
      error = tr( "The payload of %1 does not parse as %2" )
              .arg( label ).arg( what );
      return false;
   }

   if ( found != entity.guid )
   {
      error = tr( "The payload of %1 carries the GUID \"%2\", not the GUID"
                  " %3 the manifest gives it; the bundle is refused" )
              .arg( label ).arg( found ).arg( entity.guid );
      return false;
   }

   return true;
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

   QString workDir = bundle.workPath() + "/import";

   if ( ! QDir().mkpath( workDir ) )
   {
      error = tr( "Cannot create the work directory %1" ).arg( workDir );
      return QString();
   }

   // A file of a run directory is worked on under the name it will have in
   // the target, which matters to the writers that store the name
   US_DataPub::EntityType type = entity.type;
   QString name = ( type == US_DataPub::RawData     ||
                    type == US_DataPub::EditedData  ||
                    type == US_DataPub::TimeState )
                  ? renamedFile( entity ) : QFileInfo( source ).fileName();

   if ( ! plain_name( name ) )
   {
      error = tr( "%1 has no usable file name" ).arg( entity.label() );
      return QString();
   }

   QString target = workDir + "/" + name;

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

/* Rewrite a work copy so it names the records of the target.

   A record that was matched to a target record of a different GUID is known
   by the target's GUID from then on; every payload naming it has to follow,
   or the imported record would point at nothing (or at the wrong thing).
   A disk store gets no numeric IDs from the source either: the record's own
   ID is -1, and a parent's is the one the target has.
*/
bool US_DataPubImporter::rewriteWork( const US_DataPubEntity& entity,
                                      const QString& workPath,
                                      QString& error )
{
   US_DataPub::EntityType type = entity.type;
   bool                   disk = ( opts.target == US_DataPub::TargetDisk );
   QString                oldRun = entity.attrs.value( "runID" );
   QString                newRun = runIdFor( entity );
   QString                ownElem;
   QString                ownAttr;

   own_identity( type, ownElem, ownAttr );

   // Point a reference at the target's record: GUID, ID and name
   auto remapRef = [ this, disk ]( QMap< QString, QString >& values,
                                   const QString& guidKey,
                                   const QString& idKey,
                                   const QString& nameKey )
   {
      QString oldGUID = values.value( guidKey );

      if ( oldGUID.isEmpty()  ||  ! guidMap.contains( oldGUID ) )  return;

      values.insert( guidKey, mappedGuid( oldGUID ) );

      if ( ! idKey.isEmpty()  &&  values.contains( idKey ) )
         values.insert( idKey, disk ? mappedId( oldGUID ) : QString( "-1" ) );

      QString newName = mappedName( oldGUID );

      if ( ! nameKey.isEmpty()  &&  values.contains( nameKey )  &&
           ! newName.isEmpty() )
         values.insert( nameKey, newName );
   };

   bool    ownSeen = false;
   AttrEdit edit;

   switch ( type )
   {
      case US_DataPub::Project:
      case US_DataPub::Buffer:
      case US_DataPub::Analyte:
      case US_DataPub::Centerpiece:
         edit = [ & ]( const QString& element, QMap< QString, QString >& values )
         {
            if ( disk  &&  ! ownSeen  &&  element == ownElem  &&
                 values.contains( "id" ) )
            {
               values.insert( "id", "-1" );
               ownSeen = true;
            }
         };
         break;

      case US_DataPub::Solution:
         edit = [ & ]( const QString& element, QMap< QString, QString >& values )
         {
            if ( element == "solution" )
            {
               if ( disk  &&  values.contains( "id" ) )
                  values.insert( "id", "-1" );
            }

            else if ( element == "buffer"  ||  element == "analyte" )
               remapRef( values, "guid", "id", "desc" );
         };
         break;

      case US_DataPub::Model:
         edit = [ & ]( const QString& element, QMap< QString, QString >& values )
         {
            if ( element == "model" )
            {
               remapRef( values, "editGUID", QString(), QString() );

               QString desc = values.value( "description" );

               if ( oldRun != newRun  &&  desc.startsWith( oldRun + "." ) )
                  values.insert( "description",
                                 newRun + desc.mid( oldRun.size() ) );
            }

            else if ( values.contains( "analyteGUID" ) )
               remapRef( values, "analyteGUID", QString(), QString() );
         };
         break;

      case US_DataPub::Noise:
         edit = [ & ]( const QString& element, QMap< QString, QString >& values )
         {
            if ( element != "noise" )  return;

            remapRef( values, "modelGUID", QString(), QString() );

            QString desc = values.value( "description" );

            if ( oldRun != newRun  &&  desc.startsWith( oldRun + "." ) )
               values.insert( "description",
                              newRun + desc.mid( oldRun.size() ) );
         };
         break;

      case US_DataPub::RotorCalibration:
      {
         // The rotor of a calibration is known on disk by the ID of the
         // target's own rotor file of that GUID, or by none
         QString rotorID = "-1";

         if ( disk )
         {
            US_Rotor::RotorCalibration calibration;
            US_DataPubRecords::readCalibration( workPath, calibration );

            QString     dir   = diskDir( US_DataPub::RotorCalibration );
            QStringList files = QDir( dir ).entryList(
                                QStringList( "R*.xml" ), QDir::Files, QDir::Name );

            for ( int ii = 0; ii < files.size()  &&  rotorID == "-1"; ii++ )
            {
               QFile file( dir + "/" + files[ ii ] );

               if ( ! file.open( QIODevice::ReadOnly ) )  continue;

               QXmlStreamReader xml( &file );

               while ( ! xml.atEnd() )
               {
                  xml.readNext();

                  if ( ! xml.isStartElement()  ||  xml.name() != QString( "Rotor" ) )
                     continue;

                  if ( ! calibration.rotorGUID.isEmpty()  &&
                       xml.attributes().value( "guid" ).toString()
                       == calibration.rotorGUID )
                     rotorID = xml.attributes().value( "id" ).toString();

                  break;
               }
            }

            if ( rotorID == "-1" )
               note( tr( "The rotor of calibration \"%1\" is not in the target"
                         " disk store; the calibration names no rotor ID" )
                     .arg( entity.name ) );
         }

         edit = [ disk, rotorID ]( const QString& element,
                                   QMap< QString, QString >& values )
         {
            if ( ! disk  ||  element != "Calibration" )  return;

            if ( values.contains( "id" ) )       values.insert( "id", "-1" );
            if ( values.contains( "rotorID" ) )  values.insert( "rotorID", rotorID );
         };
         break;
      }

      case US_DataPub::EditedData:
         edit = [ & ]( const QString& element, QMap< QString, QString >& values )
         {
            if ( element == "runid"  &&  oldRun != newRun  &&
                 values.value( "value" ) == oldRun )
               values.insert( "value", newRun );

            else if ( element == "rawDataGUID" )
               remapRef( values, "value", QString(), QString() );
         };
         break;

      default:
         return true;       // Binary payloads, and the experiment XML, which
                            // createDisk() and createExperimentDb() rewrite
   }

   if ( ! rewrite_xml( workPath, edit, error ) )
   {
      error = tr( "The payload of %1 could not be rewritten for the target:"
                  " %2" ).arg( entity.label() ).arg( error );
      return false;
   }

   // A buffer names its components by the source's IDs
   if ( type == US_DataPub::Buffer )
      return mapComponents( entity, workPath, false, error );

   return true;
}

namespace
{
   // Two buffer component definitions describe the same component: the
   // same name, unit, range and gradient forming, and coefficients that
   // agree to the precision the database and the component table store
   // them at
   bool same_component( const US_BufferComponent& one,
                        const US_BufferComponent& two )
   {
      if ( one.name != two.name  ||  one.unit != two.unit )  return false;

      if ( one.range.trimmed() != two.range.trimmed() )      return false;

      if ( one.grad_form != two.grad_form )                  return false;

      for ( int ii = 0; ii < 6; ii++ )
      {
         if ( QString::number( one.dens_coeff[ ii ], 'f', 5 ) !=
              QString::number( two.dens_coeff[ ii ], 'f', 5 ) )  return false;

         if ( QString::number( one.visc_coeff[ ii ], 'f', 5 ) !=
              QString::number( two.visc_coeff[ ii ], 'f', 5 ) )  return false;
      }

      return true;
   }
}

/* The target's buffer component table: the database's, or for a disk store
   the installation's etc/bufferComponents.xml (or the table the options
   name), which is where US_Buffer looks a component ID up.  Read once per
   import, and never written by a disk import.
*/
bool US_DataPubImporter::targetComponents( QString& error )
{
   if ( tcomps_read )  return true;

   tcomps.clear();

   if ( opts.target == US_DataPub::TargetDisk )
   {
      QString table = opts.componentTable.trimmed().isEmpty()
                      ? US_Settings::appBaseDir() + "/etc/bufferComponents.xml"
                      : opts.componentTable.trimmed();

      // No table at all is an installation without components: a buffer
      // that names one cannot be imported, which is said where it is
      if ( QFile::exists( table )  &&
           ! US_DataPubRecords::readBufferComponents( table, tcomps ) )
      {
         error = tr( "The buffer component table %1 cannot be read" )
                 .arg( table );
         tcomps.clear();
         return false;
      }

      tcomps_read = true;
      return true;
   }

   QStringList query( "get_buffer_component_desc" );
   QStringList ids;
   dbase->query( query );

   if ( lookupFailed( tr( "its buffer components" ) ) )
   {
      error = lookup_error;
      return false;
   }

   while ( dbase->next() )
      ids << dbase->value( 0 ).toString();

   int skipped = 0;

   for ( int ii = 0; ii < ids.size(); ii++ )
   {
      US_BufferComponent comp;
      comp.componentID = ids[ ii ];

      if ( ! comp.getInfoFromDB( dbase ) )
      {
         // A failed query is a failed lookup; a row that does not hold six
         // coefficients per field is a component nothing can be matched to,
         // and has nothing to do with the buffers of this bundle
         if ( lookupFailed( tr( "buffer component %1" ).arg( ids[ ii ] ) ) )
         {
            error = lookup_error;
            return false;
         }

         note( tr( "Buffer component %1 of the target cannot be parsed; no"
                   " component of the bundle is matched to it" )
               .arg( ids[ ii ] ) );
         skipped++;
         continue;
      }

      tcomps.insert( ids[ ii ], comp );
   }

   if ( skipped > 0 )
      note( tr( "%1 of the target's %2 buffer components were skipped" )
            .arg( skipped ).arg( ids.size() ) );

   tcomps_read = true;

   return true;
}

/* Read the component definitions a buffer carries, and check them.

   A buffer names its components by the source's IDs, and the bundle
   carries the definitions beside it.  A bundle without them (written before
   the components were carried) cannot say what a component ID means, so a
   buffer that names any is refused up front, by name.  A disk store's
   components are those of the installation, which an import does not
   change: each has to be in its table already, exactly, or the import is
   refused before anything is written.
*/
bool US_DataPubImporter::checkComponents( const US_DataPubEntity& entity,
                                          const US_Buffer& buffer,
                                          QString& error )
{
   QString rel = entity.attrs.value( "componentsPayload" );
   QMap< QString, US_BufferComponent > defs;

   if ( rel.isEmpty() )
   {
      if ( buffer.componentIDs.isEmpty() )  return true;

      error = tr( "%1 names %2 buffer components, but the bundle carries no"
                  " component definitions (componentsPayload): it was"
                  " written in the older bundle format, in which a"
                  " component is known only by the exporting installation's"
                  " ID.  Export the bundle again with this version." )
              .arg( entity.label() ).arg( buffer.componentIDs.size() );
      return false;
   }

   QString path;

   if ( ! insideBundle( rel, path, error )  ||  ! QFile::exists( path )  ||
        ! US_DataPubRecords::readBufferComponents( path, defs ) )
   {
      error = tr( "The component definitions of %1 could not be read" )
              .arg( entity.label() );
      return false;
   }

   for ( int ii = 0; ii < buffer.componentIDs.size(); ii++ )
   {
      if ( defs.contains( buffer.componentIDs[ ii ] ) )  continue;

      error = tr( "%1 names component %2, which the bundle does not define" )
              .arg( entity.label() ).arg( buffer.componentIDs[ ii ] );
      return false;
   }

   comp_defs.insert( entity.guid, defs );

   if ( opts.target != US_DataPub::TargetDisk )  return true;

   // A buffer the disk store already holds is reused, components and all
   QString id;
   QString name;

   if ( buffer.componentIDs.isEmpty()  ||
        findByGuid( US_DataPub::Buffer, entity.guid, id, name ) )
      return true;

   if ( ! targetComponents( error ) )  return false;

   QStringList missing;
   QList< QString > ids = tcomps.keys();

   for ( int ii = 0; ii < buffer.componentIDs.size(); ii++ )
   {
      const US_BufferComponent& want = defs[ buffer.componentIDs[ ii ] ];
      bool                      have = false;

      for ( int jj = 0; jj < ids.size()  &&  ! have; jj++ )
         have = same_component( tcomps[ ids[ jj ] ], want );

      if ( ! have )  missing << QString( "\"%1\"" ).arg( want.name );
   }

   if ( missing.isEmpty() )  return true;

   error = tr( "%1 needs the buffer components %2, which this installation's"
               " component table does not hold with the same definition.  A"
               " disk import does not change that table; add the components"
               " there first (with the buffer editor), or import into a"
               " database, which takes them along." )
           .arg( entity.label() ).arg( missing.join( ", " ) );

   return false;
}

/* Point a buffer's components at the target's.

   The bundle carries the definitions of the components a buffer names, so
   each is matched by what it is -- name, unit, range, gradient forming and
   coefficients -- in the target's table.  A component a database lacks is
   created there when the buffer is written (create), and until then named
   "new:<ID>", so that no target buffer can look the same as this one.  A
   disk store's table is the installation's and is never written; the
   preflight made sure it has them all.
*/
bool US_DataPubImporter::mapComponents( const US_DataPubEntity& entity,
                                        const QString& workPath, bool create,
                                        QString& error )
{
   if ( ! comp_defs.contains( entity.guid )  &&
        ! entity.attrs.value( "componentsPayload" ).isEmpty() )
   {  // Read in the preflight; this is the second line
      US_Buffer buffer;

      if ( ! buffer.readFromDisk( workPath )  ||
           ! checkComponents( entity, buffer, error ) )
         return false;
   }

   const QMap< QString, US_BufferComponent > source =
         comp_defs.value( entity.guid );

   QString  failure;
   AttrEdit edit = [ & ]( const QString& element,
                          QMap< QString, QString >& values )
   {
      if ( element != "component"  ||  ! failure.isEmpty() )  return;

      QString id    = values.value( "id" );
      bool    fresh = id.startsWith( "new:" );
      QString srcID = fresh ? id.mid( 4 ) : id;

      // The writing pass comes after rewriteWork() has mapped every ID it
      // could: what is left to do is the components the target lacks
      if ( create  &&  ! fresh )  return;

      if ( ! source.contains( srcID ) )
      {
         failure = tr( "%1 names component %2, which the bundle does not"
                       " define" ).arg( entity.label() ).arg( srcID );
         return;
      }

      const US_BufferComponent& want = source[ srcID ];

      if ( ! targetComponents( failure ) )  return;

      QList< QString > ids = tcomps.keys();

      for ( int ii = 0; ii < ids.size(); ii++ )
      {
         if ( ! same_component( tcomps[ ids[ ii ] ], want ) )  continue;

         values.insert( "id", ids[ ii ] );
         return;
      }

      if ( ! create )
      {
         values.insert( "id", "new:" + srcID );
         return;
      }

      if ( opts.target != US_DataPub::TargetDb )
      {
         failure = tr( "Buffer component \"%1\" of %2 is not in this"
                       " installation's component table, which a disk import"
                       " does not change" ).arg( want.name )
                   .arg( entity.label() );
         return;
      }

      // The database does not have it: it is added, inside the import's
      // transaction
      US_BufferComponent added = want;
      added.componentID.clear();

      if ( added.saveToDB( dbase ) < 1 )
      {
         failure = tr( "Buffer component \"%1\" could not be added to the"
                       " database: %2" ).arg( want.name )
                   .arg( dbase->lastError() );
         return;
      }

      QString newID = added.componentID;
      tcomps.insert( newID, added );
      note( tr( "Buffer component \"%1\" was added to the target's"
                " component table as %2" ).arg( want.name ).arg( newID ) );
      values.insert( "id", newID );
   };

   if ( ! rewrite_xml( workPath, edit, error ) )
   {
      error = tr( "The components of %1 could not be pointed at the"
                  " target's: %2" ).arg( entity.label() ).arg( error );
      return false;
   }

   if ( ! failure.isEmpty() )
   {
      error = failure;
      return false;
   }

   return true;
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

   // A run this import makes (or, in a dry run, would make) takes its data
   if ( entity.type == US_DataPub::Experiment  &&
        ( resolution == US_DataPub::ResolvedCreated  ||
          resolution == US_DataPub::ResolvedRenamed ) )
      created_exps << entity.guid;

   // A record that was not imported maps to nothing; whatever names it keeps
   // its own reference (and is told about it where that matters)
   // A time state goes by its run's GUID and is nobody's parent, so it must
   // not replace what the maps say about the run itself
   if ( resolution != US_DataPub::ResolvedOmitted  &&
        entity.type != US_DataPub::TimeState )
   {
      guidMap.insert( entity.guid, targetGUID );
      idMap  .insert( entity.guid, targetID );
      nameMap.insert( entity.guid, targetName );
   }

   step_count++;
   emit stepDone( step_count );

   note( tr( "%1 \"%2\" %3%4" )
         .arg( US_DataPub::typeText( entity.type ) )
         .arg( targetName.isEmpty() ? entity.label() : targetName )
         .arg( resolutionText( resolution ) )
         .arg( detail.isEmpty() ? QString() : " -- " + detail ) );
}

// -------------------------------------------------------------------- target

bool US_DataPubImporter::openTarget( QString& error )
{
   closeTarget();

   if ( opts.target == US_DataPub::TargetDisk )
   {
      QString base = opts.outputDir.trimmed();

      // A dry run writes nothing, not even the directory
      if ( base.isEmpty()  ||  opts.dryRun )  return true;

      if ( ! QDir().mkpath( base ) )
      {
         error = tr( "Cannot create the output directory %1" ).arg( base );
         return false;
      }

      return true;
   }

   if ( ext_db != nullptr )
   {
      dbase  = ext_db;
      own_db = false;

      if ( ! dbase->isConnected() )
      {
         error = tr( "The database connection is not open" );
         dbase = nullptr;
         return false;
      }

      return true;
   }

   US_DB2* db = new US_DB2( opts.dbPassword );

   if ( db->lastErrno() != US_DB2::OK )
   {
      error = tr( "Cannot connect to the database: %1" ).arg( db->lastError() );
      delete db;
      return false;
   }

   dbase  = db;
   own_db = true;
   error.clear();

   return true;
}

void US_DataPubImporter::closeTarget( void )
{
   if ( own_db  &&  dbase != nullptr )  delete dbase;

   dbase  = nullptr;
   own_db = false;
}

bool US_DataPubImporter::lookupFailed( const QString& what )
{
   if ( dbase == nullptr )  return false;

   int status = dbase->lastErrno();

   if ( status == IUS_DB2::OK  ||  status == IUS_DB2::NOROWS )  return false;

   if ( lookup_error.isEmpty() )
      lookup_error = tr( "The target database could not be asked about %1"
                         " (status %2): %3" ).arg( what ).arg( status )
                     .arg( dbase->lastError() );

   return true;
}

/* The writing half of a database import is one transaction.

   Everything the import writes goes through this one connection -- the
   library writers it uses (US_Buffer, US_Analyte, US_Noise, rotor
   calibrations, time states and US_ConvertIO) all take it as an argument --
   so a rollback undoes all of it, within what IUS_DB2::beginTransaction()
   documents: InnoDB tables, and no procedure that commits by itself.
*/
bool US_DataPubImporter::beginWrite( QString& error )
{
   if ( opts.target != US_DataPub::TargetDb  ||  opts.dryRun )  return true;

   QString detail;

   if ( ! dbase->beginTransaction( detail ) )
   {
      error = tr( "Could not begin a database transaction: %1" ).arg( detail );
      return false;
   }

   in_txn = true;

   return true;
}

bool US_DataPubImporter::commitWrite( QString& error )
{
   if ( ! in_txn )  return true;

   QString detail;

   if ( ! dbase->commitTransaction( detail ) )
   {
      error = tr( "Could not commit the import: %1" ).arg( detail );
      abortWrite( error );
      return false;
   }

   in_txn = false;

   return true;
}

void US_DataPubImporter::abortWrite( const QString& reason )
{
   if ( in_txn  &&  dbase != nullptr )
   {
      QString detail;
      in_txn = false;

      if ( dbase->rollbackTransaction( detail ) )
         outcome_text = tr( "Nothing was imported: the database transaction"
                            " was rolled back after %1" ).arg( reason );
      else
         outcome_text = tr( "The import failed (%1) and its rollback failed"
                            " too (%2); some records may have been kept" )
                        .arg( reason ).arg( detail );

      note( outcome_text );
      return;
   }

   if ( opts.target == US_DataPub::TargetDisk  &&  ! opts.dryRun )
   {
      outcome_text = disk_written.isEmpty()
         ? tr( "Nothing was written to the disk store" )
         : tr( "The disk store keeps the %1 files written before the"
               " failure:\n  %2" ).arg( disk_written.size() )
           .arg( disk_written.join( "\n  " ) );
   }

   else
      outcome_text = tr( "Nothing was written" );

   note( outcome_text );
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

   if ( create  &&  ! opts.dryRun )  QDir().mkpath( path );

   return path;
}

QString US_DataPubImporter::diskResultDir( const QString& runID,
                                           bool create ) const
{
   QString base = opts.outputDir.trimmed().isEmpty()
                  ? US_Settings::resultDir()
                  : opts.outputDir.trimmed() + "/results";

   // A run ID is a single, plain path component; anything else never gets
   // this far, but a directory outside the results is not made for it
   if ( ! runIdIsUsable( runID ) )  return QString();

   QString path = base + "/" + runID;

   if ( create  &&  ! opts.dryRun )  QDir().mkpath( path );

   return path;
}

bool US_DataPubImporter::diskFilePresent( const US_DataPubEntity& entity,
                                          QString& path, bool& identical )
{
   path     .clear();
   identical = false;

   QString runID = runIdFor( entity );
   QString dir   = diskResultDir( runID );

   if ( dir.isEmpty() )  return false;

   path = dir + "/" + renamedFile( entity );

   if ( ! QFile::exists( path ) )  return false;

   // Compared with what the import would write, which is the bundle's
   // payload rewritten for the target
   QString error;
   QString work = workCopy( entity, error );

   if ( work.isEmpty()  ||  ! rewriteWork( entity, work, error ) )  return true;

   identical = ( US_DataPubHash::fileHash( path )
                 == US_DataPubHash::fileHash( work ) );

   return true;
}

// ------------------------------------------------------------------ the run

bool US_DataPubImporter::runImport( const Options& options, QString& error )
{
   opts       = options;
   logtext.clear();
   warnlist.clear();
   disk_written.clear();
   outcome_text.clear();
   lookup_error.clear();
   reslist.clear();
   guidMap.clear();
   idMap  .clear();
   nameMap.clear();
   held_names.clear();
   exp_by_guid.clear();
   exp_indexed = false;
   created_exps.clear();
   staged_runs.clear();
   tcomps.clear();
   tcomps_read = false;
   comp_defs.clear();
   exp_resolved = false;
   run_lookup.clear();
   raw_written.clear();
   step_count = 0;

   if ( mani.total() < 1 )
   {
      error = tr( "No bundle has been inspected yet" );
      return false;
   }

   // The paths were checked when the bundle was inspected; the manifest is
   // the importer's own since, but checking again costs nothing
   if ( ! checkBundle( error ) )  return false;

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

   else
      note( tr( "The payload digests were not checked" ) );

   if ( ! opts.renameSuffix.trimmed().isEmpty() )
   {
      QString probe = QString( "run-" ) + opts.renameSuffix.trimmed();

      if ( ! runIdIsUsable( probe )  &&
           ! mani.section( US_DataPub::Experiment ).isEmpty() )
         note( tr( "The rename suffix \"%1\" cannot be part of a run ID; a"
                   " renamed run uses it with the unusable characters"
                   " replaced" ).arg( opts.renameSuffix ) );
   }

   // The order the records go in, which a bundle whose dependencies go
   // round in a circle has none of
   QList< US_DataPub::EntityType > types = importOrder( mani, opts.target,
                                                        error );

   if ( types.isEmpty() )  return false;

   if ( ! openTarget( error ) )  return false;

   tproj_guid.clear();
   tproj_id  .clear();
   tproj_desc.clear();

   if ( ! findTargetProject( error )  ||  ! preflight( error ) )
   {
      outcome_text = tr( "Nothing was written" );
      closeTarget();
      return false;
   }

   if ( ! beginWrite( error ) )
   {
      outcome_text = tr( "Nothing was written" );
      closeTarget();
      return false;
   }

   emit steps( mani.total() );

   // The manifest lists the records in the declared section order; they are
   // replayed in the order that satisfies the target's own dependencies.
   for ( int ii = 0; ii < types.size(); ii++ )
   {
      QList< US_DataPubEntity > entities = mani.section( types[ ii ] );

      for ( int jj = 0; jj < entities.size(); jj++ )
      {
         if ( importEntity( entities[ jj ], error )  &&
              lookup_error.isEmpty() )
            continue;

         if ( error.isEmpty() )  error = lookup_error;

         error = tr( "%1: %2" ).arg( entities[ jj ].label() ).arg( error );
         abortWrite( error );
         closeTarget();
         return false;
      }
   }

   if ( ! commitWrite( error ) )
   {
      closeTarget();
      return false;
   }

   closeTarget();

   int written = 0;

   for ( int ii = 0; ii < reslist.size(); ii++ )
      if ( reslist[ ii ].resolution == US_DataPub::ResolvedCreated  ||
           reslist[ ii ].resolution == US_DataPub::ResolvedRenamed )
         written++;

   outcome_text = opts.dryRun
      ? tr( "Dry run complete; nothing was written" )
      : tr( "%1 of %2 records were written to the target; the rest were"
            " already there or could not be used" )
        .arg( written ).arg( reslist.size() );

   if ( ! warnlist.isEmpty() )
      outcome_text += tr( " (%1 warnings)" ).arg( warnlist.size() );

   note( outcome_text );

   return true;
}

/* Look up the project the import was told to attach the runs to.

   The bundle names the project its runs came from, but the receiving
   installation usually keeps its own project list, so a user can point an
   import at one of theirs.  It has to exist in the target already: creating
   it here would defeat the point of choosing it.  In a database it also has
   to belong to the investigator the import runs as.
*/
bool US_DataPubImporter::findTargetProject( QString& error )
{
   QString wanted = opts.projectGUID.trimmed();

   if ( wanted.isEmpty() )  return true;

   if ( opts.target == US_DataPub::TargetDb )
   {
      if ( dbase == nullptr )
      {
         error = tr( "No database connection" );
         return false;
      }

      QStringList query;
      query << "get_projectID_from_GUID" << wanted;
      dbase->query( query );

      if ( lookupFailed( tr( "project %1" ).arg( wanted ) ) )
      {
         error = lookup_error;
         return false;
      }

      if ( dbase->next() )
         tproj_id = dbase->value( 0 ).toString();

      if ( tproj_id.isEmpty()  ||  tproj_id.toInt() < 1 )
      {
         error = tr( "The project %1 the import was told to use is not in"
                     " the target database" ).arg( wanted );
         return false;
      }

      // Only a project of the current investigator is offered by the
      // project dialog, and only one of those may be imported into
      QString     invID = QString::number( US_Settings::us_inv_ID() );
      bool        owned = false;
      query.clear();
      query << "get_project_desc" << invID;
      dbase->query( query );

      if ( lookupFailed( tr( "the investigator's projects" ) ) )
      {
         error = lookup_error;
         return false;
      }

      while ( dbase->next() )
         if ( dbase->value( 0 ).toString() == tproj_id )  owned = true;

      if ( ! owned )
      {
         error = tr( "The project %1 the import was told to use does not"
                     " belong to investigator %2" ).arg( wanted ).arg( invID );
         return false;
      }

      US_Project project;

      if ( project.readFromDB( tproj_id.toInt(), dbase ) != IUS_DB2::OK )
      {
         error = tr( "The project %1 could not be read from the target"
                     " database" ).arg( wanted );
         return false;
      }

      tproj_guid = project.projectGUID;
      tproj_desc = project.projectDesc;
   }

   else
   {
      QString     path  = diskDir( US_DataPub::Project );
      QStringList files = QDir( path ).entryList(
                          QStringList( "P???????.xml" ), QDir::Files,
                          QDir::Name );

      for ( int ii = 0; ii < files.size(); ii++ )
      {
         US_Project project;

         if ( project.readFromFile( path + "/" + files[ ii ] ) != IUS_DB2::OK )
            continue;

         if ( project.projectGUID != wanted )  continue;

         tproj_guid = project.projectGUID;
         tproj_desc = project.projectDesc;
         tproj_id   = QString::number( project.projectID );
         break;
      }

      if ( tproj_guid.isEmpty() )
      {
         error = tr( "The project %1 the import was told to use is not in"
                     " the target disk store" ).arg( wanted );
         return false;
      }
   }

   note( tr( "The imported runs are attached to the project \"%1\"" )
         .arg( tproj_desc.isEmpty() ? tproj_guid : tproj_desc ) );

   return true;
}

QString US_DataPubImporter::targetName( const US_DataPubEntity& entity ) const
{
   if ( entity.type != US_DataPub::Model  &&  entity.type != US_DataPub::Noise )
      return entity.name;

   // A model or noise record is named after the run it was fitted to
   QList< US_DataPubEntity > exps = mani.section( US_DataPub::Experiment );

   for ( int ii = 0; ii < exps.size(); ii++ )
   {
      QString oldRun = exps[ ii ].name;
      QString newRun = mappedName( exps[ ii ].guid );

      if ( newRun.isEmpty()  ||  newRun == oldRun )   continue;
      if ( ! entity.name.startsWith( oldRun + "." ) )  continue;

      return newRun + entity.name.mid( oldRun.size() );
   }

   return entity.name;
}

bool US_DataPubImporter::importEntity( const US_DataPubEntity& entity,
                                       QString& error )
{
   US_DataPub::EntityType type = entity.type;
   bool                   db   = ( opts.target == US_DataPub::TargetDb );

   // ---- a project the import was told to use ------------------------------
   if ( type == US_DataPub::Project  &&  ! tproj_guid.isEmpty() )
   {
      record( entity, US_DataPub::ResolvedReused, tproj_guid, tproj_desc,
              tproj_id, tr( "the import attaches the data to this project" ) );
      return true;
   }

   // ---- reference hardware is matched, never created ----------------------
   if ( type == US_DataPub::Centerpiece )
      return importCenterpiece( entity, error );

   // ---- raw data this import already wrote with its run -------------------
   if ( raw_written.contains( entity.guid ) )
   {
      record( entity, US_DataPub::ResolvedCreated, entity.guid,
              renamedFile( entity ), raw_written.value( entity.guid ),
              QString() );
      return true;
   }

   // ---- a run-directory payload that is already on disk -------------------
   if ( ! db  &&
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
                 renamedFile( entity ), QString( "-1" ),
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

   if ( ! lookup_error.isEmpty() )
   {
      error = lookup_error;
      return false;
   }

   // A run the target already had is not added to
   QString expGUID = entity.depend( US_DataPub::Experiment );

   if ( db  &&  ( type == US_DataPub::RawData  ||  type == US_DataPub::TimeState
                  ||  type == US_DataPub::Solution )
        &&  ! expGUID.isEmpty()  &&  guidMap.contains( expGUID )  &&
        ! created_exps.contains( expGUID ) )
   {
      if ( type == US_DataPub::TimeState )
      {
         QStringList query;
         query << "get_experiment_timestate" << mappedId( expGUID );
         dbase->query( query );

         if ( lookupFailed( tr( "the time state of run %1" )
                            .arg( runIdFor( entity ) ) ) )
         {
            error = lookup_error;
            return false;
         }

         if ( dbase->next() )
         {
            record( entity, US_DataPub::ResolvedReused, entity.guid,
                    renamedFile( entity ), dbase->value( 0 ).toString(),
                    tr( "the run in the target already has a time state" ) );
            return true;
         }

         error = tr( "run %1 is already in the target database without a"
                     " time state; the import does not add data to a run it"
                     " did not create" ).arg( runIdFor( entity ) );
         return false;
      }

      if ( type == US_DataPub::Solution )
      {
         error = tr( "run %1 is already in the target database without this"
                     " solution; the import does not add a solution to a run"
                     " it did not create" ).arg( mappedName( expGUID ) );
         return false;
      }

      error = tr( "run %1 is already in the target database without this"
                  " raw data; the import does not add data to a run it did"
                  " not create" ).arg( runIdFor( entity ) );
      return false;
   }

   // ---- the copy that would be written, put in terms of the target --------
   QString workPath;

   if ( type != US_DataPub::Experiment )
   {
      workPath = workCopy( entity, error );

      if ( workPath.isEmpty()  ||  ! rewriteWork( entity, workPath, error ) )
         return false;
   }

   // ---- is the name taken by a different record? --------------------------
   QString name     = targetName( entity );
   QString otherGUID;
   QString otherID;
   bool    conflict = ! name.isEmpty()
                      &&  findByName( type, name, otherGUID, otherID );

   if ( ! lookup_error.isEmpty() )
   {
      error = lookup_error;
      return false;
   }

   US_DataPub::ConflictPolicy policy = opts.policyFor( type );

   // The same record under its own GUID, found by name where the GUID
   // lookup could not see it
   if ( conflict  &&  ! otherGUID.isEmpty()  &&  otherGUID == entity.guid )
   {
      record( entity, US_DataPub::ResolvedReused, entity.guid, name, otherID,
              tr( "the target already holds this record" ) );
      return true;
   }

   if ( conflict )
   {
      QString ownElem;
      QString ownAttr;
      own_identity( type, ownElem, ownAttr );

      QString ourPrint    = workPath.isEmpty() ? QString()
                            : US_DataPubHash::fingerprint( workPath, ownElem,
                                                           ownAttr );
      QString theirPrint  = targetFingerprint( type, otherGUID, otherID );

      // A record the target holds without a GUID cannot be pointed at
      bool    identical   = ( ! ourPrint.isEmpty()  &&  ! otherGUID.isEmpty()
                              &&  ourPrint == theirPrint );
      QString details     = identical
                            ? tr( "the existing record has the same properties" )
                            : tr( "the existing record has different properties" );
      QString suggestion  = uniqueName( type, name );

      if ( ! lookup_error.isEmpty() )
      {
         error = lookup_error;
         return false;
      }

      if ( resolver != nullptr )
         policy = resolver->resolve( type, name, identical, details,
                                     suggestion );

      if ( policy == US_DataPub::PolicyFail )
      {
         if ( resolver != nullptr  &&  resolver->cancelled() )
            error = tr( "The import was cancelled at the conflict over %1"
                        " \"%2\"" ).arg( US_DataPub::typeText( type ) )
                    .arg( name );
         else
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
      QString origName = name;
      name = suggestion.trimmed().isEmpty() ? origName : suggestion.trimmed();

      if ( type == US_DataPub::Experiment  &&  ! runIdIsUsable( name ) )
      {
         error = tr( "\"%1\" cannot be a run ID: a run ID may hold only"
                     " letters, digits, the underscore and the hyphen, and at"
                     " most 80 of them" ).arg( name );
         return false;
      }

      QString takenGUID;
      QString takenID;

      if ( name == origName  ||  findByName( type, name, takenGUID, takenID ) )
         name = uniqueName( type, origName );

      if ( ! lookup_error.isEmpty() )
      {
         error = lookup_error;
         return false;
      }

      if ( opts.dryRun )
      {
         nameTaken( type, name, entity.guid, QString( "-1" ) );
         record( entity, US_DataPub::ResolvedRenamed, entity.guid, name,
                 QString( "-1" ), details );
         return true;
      }

      if ( type != US_DataPub::RawData  &&  type != US_DataPub::EditedData  &&
           type != US_DataPub::TimeState  &&  type != US_DataPub::Experiment )
      {
         if ( ! US_DataPubRecords::setRecordName( workPath, type, name ) )
         {
            error = tr( "The name of %1 could not be changed inside its"
                        " payload" ).arg( entity.label() );
            return false;
         }
      }

      QString newID;

      if ( ! createRecord( entity, workPath, name, newID, error ) )
         return false;

      if ( newID == "omitted" )  return true;

      nameTaken( type, name, entity.guid, newID );
      record( entity, US_DataPub::ResolvedRenamed, entity.guid, name, newID,
              details );
      return true;
   }

   // ---- a plain new record ------------------------------------------------
   if ( opts.dryRun )
   {
      nameTaken( type, name, entity.guid, QString( "-1" ) );
      record( entity, US_DataPub::ResolvedCreated, entity.guid, name,
              QString( "-1" ), QString() );
      return true;
   }

   if ( name != entity.name  &&  ! workPath.isEmpty()  &&
        ! US_DataPubRecords::setRecordName( workPath, type, name ) )
   {
      error = tr( "The name of %1 could not be changed inside its payload" )
              .arg( entity.label() );
      return false;
   }

   QString newID;

   if ( ! createRecord( entity, workPath, name, newID, error ) )  return false;

   // A record the target could not take is reported as such, not as created
   if ( newID == "omitted" )  return true;

   nameTaken( type, name, entity.guid, newID );
   record( entity, US_DataPub::ResolvedCreated, entity.guid, name, newID,
           QString() );

   return true;
}

/* A centerpiece is reference hardware.  The target's own table is what the
   runs have to name, so a centerpiece is matched -- by GUID, then by an
   identical record of the same name -- and never created.  When neither
   matches, another of the target's centerpieces stands in, with a warning,
   unless the import is strict about hardware.
*/
bool US_DataPubImporter::importCenterpiece( const US_DataPubEntity& entity,
                                            QString& error )
{
   QString id;
   QString name;

   if ( findByGuid( US_DataPub::Centerpiece, entity.guid, id, name ) )
   {
      record( entity, US_DataPub::ResolvedReused, entity.guid, name, id,
              tr( "the target already holds this centerpiece" ) );
      return true;
   }

   if ( ! lookup_error.isEmpty() )
   {
      error = lookup_error;
      return false;
   }

   QList< US_AbstractCenterpiece > centerpieces;

   if ( ! readCenterpieces( centerpieces ) )
   {
      error = lookup_error.isEmpty() ? tr( "the target's centerpieces could"
                                           " not be read" )
                                     : lookup_error;
      return false;
   }

   QString path = payloadPath( entity );
   QString print = US_DataPubHash::fingerprint( path, "abstractCenterpiece",
                                                "guid" );
   QString workDir = bundle.workPath() + "/target";
   QDir().mkpath( workDir );
   QString theirs = workDir + "/centerpiece.xml";

   for ( int ii = 0; ii < centerpieces.size(); ii++ )
   {
      if ( centerpieces[ ii ].name != entity.name )  continue;

      QFile::remove( theirs );
      US_DataPubRecords::writeCenterpiece( centerpieces[ ii ], theirs );

      if ( print.isEmpty()  ||  centerpieces[ ii ].guid.isEmpty()  ||
           US_DataPubHash::fingerprint( theirs, "abstractCenterpiece", "guid" )
           != print )
         continue;

      record( entity, US_DataPub::ResolvedReused, centerpieces[ ii ].guid,
              centerpieces[ ii ].name,
              QString::number( centerpieces[ ii ].serial_number ),
              tr( "the target's centerpiece of that name is the same" ) );
      return true;
   }

   if ( centerpieces.isEmpty() )
   {
      if ( opts.target == US_DataPub::TargetDb )
      {
         error = tr( "the target database has no centerpieces, so the runs"
                     " cannot name one" );
         return false;
      }

      if ( ! warn( tr( "Centerpiece \"%1\" is not in the local centerpiece"
                       " table, which is empty; the runs name centerpiece 0" )
                   .arg( entity.name ), error ) )
         return false;

      record( entity, US_DataPub::ResolvedOmitted, QString(), entity.name,
              QString( "0" ), tr( "the target has no centerpieces" ) );
      guidMap.insert( entity.guid, QString() );
      idMap  .insert( entity.guid, QString( "0" ) );
      return true;
   }

   const US_AbstractCenterpiece& stand = centerpieces[ 0 ];

   if ( ! warn( tr( "Centerpiece \"%1\" is not in the target; centerpiece"
                    " \"%2\" (serial %3) is named instead" )
                .arg( entity.name ).arg( stand.name )
                .arg( stand.serial_number ), error ) )
      return false;

   record( entity, US_DataPub::ResolvedSubstituted, stand.guid, stand.name,
           QString::number( stand.serial_number ),
           tr( "the target does not have this centerpiece" ) );

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

   // The experiment XML of a run directory: "<runID>.<runType>.xml", where
   // neither part has a dot.  An edit is "<runID>.<editID>.<runType>...xml"
   // and a time state's definitions "<runID>.time_state.xml", so the count
   // of parts tells them apart -- for a usable run ID, which has no dots.
   QString experiment_xml( const QDir& dir, const QString& runID )
   {
      if ( ! US_DataPubImporter::runIdIsUsable( runID ) )  return QString();

      QStringList xmls = dir.entryList( QStringList( runID + ".*.xml" ),
                                        QDir::Files, QDir::Name );

      for ( int ii = 0; ii < xmls.size(); ii++ )
      {
         QStringList parts = xmls[ ii ].split( '.' );

         if ( parts.size() != 3  ||  parts[ 0 ] != runID )  continue;
         if ( parts[ 1 ] == "time_state" )                   continue;

         return dir.absoluteFilePath( xmls[ ii ] );
      }

      return QString();
   }
}

/* The investigator's experiments by GUID.

   The listing carries no GUID, so every experiment is asked for its own:
   one query per run of the investigator.  resolveExperiments() reads it only
   for a run its run ID did not settle, and before the transaction opens.
*/
void US_DataPubImporter::indexExperiments( void )
{
   if ( exp_indexed  ||  dbase == nullptr )  return;

   exp_indexed = true;

   QStringList query;
   QStringList ids;
   QStringList runs;
   query << "get_experiment_desc" << QString::number( US_Settings::us_inv_ID() );
   dbase->query( query );

   if ( lookupFailed( tr( "its experiments" ) ) )  return;

   while ( dbase->next() )
   {
      ids  << dbase->value( 0 ).toString();
      runs << dbase->value( 1 ).toString();
   }

   for ( int ii = 0; ii < ids.size(); ii++ )
   {
      query.clear();
      query << "get_experiment_info" << ids[ ii ];
      dbase->query( query );

      if ( lookupFailed( tr( "experiment %1" ).arg( ids[ ii ] ) ) )  return;

      if ( ! dbase->next() )  continue;

      QString guid = dbase->value( 0 ).toString();

      if ( ! guid.isEmpty()  &&  ! exp_by_guid.contains( guid ) )
         exp_by_guid.insert( guid, qMakePair( ids[ ii ], runs[ ii ] ) );
   }
}

void US_DataPubImporter::failLookup( const QString& what )
{
   if ( lookupFailed( what ) )  return;

   // The status may say OK although a reader gave up on a row it could not
   // read; that is a failed lookup all the same
   if ( lookup_error.isEmpty() )
      lookup_error = tr( "The target could not be asked about %1" ).arg( what );
}

bool US_DataPubImporter::readCalibrationsDb(
      QVector< US_Rotor::RotorCalibration >& all )
{
   all.clear();

   QVector< US_Rotor::Lab > labs;

   if ( read_labs_db( labs, dbase ) == US_Rotor::MISC_ERROR )
   {
      failLookup( tr( "its labs" ) );
      return false;
   }

   for ( int ii = 0; ii < labs.size(); ii++ )
   {
      QVector< US_Rotor::Rotor > rotors;

      if ( US_Rotor::readRotorsFromDB( rotors, labs[ ii ].ID, dbase )
           == US_Rotor::MISC_ERROR )
      {
         failLookup( tr( "the rotors of lab %1" ).arg( labs[ ii ].ID ) );
         return false;
      }

      for ( int jj = 0; jj < rotors.size(); jj++ )
      {
         QVector< US_Rotor::RotorCalibration > cals;

         if ( US_Rotor::readCalibrationProfilesDB( cals, rotors[ jj ].ID,
                                                   dbase )
              == US_Rotor::MISC_ERROR )
         {
            failLookup( tr( "the calibrations of rotor %1" )
                        .arg( rotors[ jj ].ID ) );
            return false;
         }

         all << cals;
      }
   }

   return true;
}

bool US_DataPubImporter::readCenterpieces(
      QList< US_AbstractCenterpiece >& centerpieces )
{
   centerpieces.clear();

   if ( opts.target != US_DataPub::TargetDb )
   {
      // The installation's table; one that cannot be read is no table
      if ( ! US_AbstractCenterpiece::read_centerpieces( centerpieces ) )
      {
         centerpieces.clear();

         if ( lookup_error.isEmpty() )
            lookup_error = tr( "The local centerpiece table %1 cannot be"
                               " read" ).arg( US_Settings::appBaseDir()
                                              + "/etc/abstractCenterpieces.xml" );
         return false;
      }

      return true;
   }

   // The library reader falls back to the local file when the database
   // fails, which would put local serial numbers into database records
   if ( dbase == nullptr )  return false;

   QStringList query( "get_abstractCenterpiece_names" );
   QList< int > ids;
   dbase->query( query );

   if ( lookupFailed( tr( "its centerpieces" ) ) )  return false;

   while ( dbase->next() )
      ids << dbase->value( 0 ).toString().toInt();

   std::sort( ids.begin(), ids.end() );

   for ( int ii = 0; ii < ids.size(); ii++ )
   {
      query.clear();
      query << "get_abstractCenterpiece_info" << QString::number( ids[ ii ] );
      dbase->query( query );

      if ( lookupFailed( tr( "centerpiece %1" ).arg( ids[ ii ] ) ) )
         return false;

      if ( ! dbase->next() )
      {
         failLookup( tr( "centerpiece %1" ).arg( ids[ ii ] ) );
         return false;
      }

      US_AbstractCenterpiece cp;
      cp.serial_number = ids[ ii ];
      cp.guid          = dbase->value( 0 ).toString();
      cp.name          = dbase->value( 1 ).toString();
      cp.channels      = qMax( dbase->value( 2 ).toString().toInt(), 1 );
      QString bottoms  = dbase->value( 3 ).toString();
      cp.shape         = dbase->value( 4 ).toString();
      cp.shape         = cp.shape.isEmpty() ? "sector" : cp.shape;
      cp.maxRPM        = dbase->value( 5 ).toString().toDouble();
      double pathlen   = dbase->value( 6 ).toString().toDouble();
      cp.angle         = dbase->value( 7 ).toString().toDouble();
      cp.width         = dbase->value( 8 ).toString().toDouble();
      cp.material      = cp.name.section( " ", 0, 0 );

      for ( int jj = 0; jj < cp.channels; jj++ )
      {
         cp.bottom_position << bottoms.section( ":", jj, jj ).toDouble();
         cp.path_length     << pathlen;
      }

      centerpieces << cp;
   }

   return true;
}

bool US_DataPubImporter::findByGuid( US_DataPub::EntityType type,
                                     const QString& guid, QString& id,
                                     QString& name )
{
   id  .clear();
   name.clear();

   if ( guid.isEmpty() )  return false;

   // ---- reference hardware, in either target -------------------------------
   if ( type == US_DataPub::Centerpiece )
   {
      QList< US_AbstractCenterpiece > centerpieces;

      if ( ! readCenterpieces( centerpieces ) )  return false;

      for ( int ii = 0; ii < centerpieces.size(); ii++ )
      {
         if ( centerpieces[ ii ].guid != guid )  continue;

         id   = QString::number( centerpieces[ ii ].serial_number );
         name = centerpieces[ ii ].name;
         return true;
      }

      return false;
   }

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
         {
            // No procedure looks an experiment up by its GUID; the bundle's
            // runs were looked up before the transaction opened
            if ( ! exp_resolved )
            {
               QString rerror;

               if ( ! resolveExperiments( rerror ) )
               {
                  if ( lookup_error.isEmpty() )  lookup_error = rerror;
                  return false;
               }
            }

            if ( ! exp_by_guid.contains( guid ) )  return false;

            id   = exp_by_guid.value( guid ).first;
            name = exp_by_guid.value( guid ).second;
            return true;
         }

         case US_DataPub::RotorCalibration:
         {
            QVector< US_Rotor::RotorCalibration > cals;

            if ( ! readCalibrationsDb( cals ) )  return false;

            for ( int ii = 0; ii < cals.size(); ii++ )
            {
               if ( cals[ ii ].GUID != guid )  continue;

               id   = QString::number( cals[ ii ].ID );
               name = cals[ ii ].label;
               return true;
            }

            return false;
         }

         default:
            return false;
      }

      dbase->query( query );

      if ( lookupFailed( tr( "%1 %2" ).arg( US_DataPub::typeText( type ) )
                         .arg( guid ) ) )
         return false;

      if ( ! dbase->next() )  return false;

      id = dbase->value( 0 ).toString();

      return ! id.isEmpty();
   }

   // ---- disk target -------------------------------------------------------
   QString results = opts.outputDir.trimmed().isEmpty()
                     ? US_Settings::resultDir()
                     : opts.outputDir.trimmed() + "/results";

   if ( type == US_DataPub::Experiment )
   {
      QStringList runs = QDir( results ).entryList(
                         QDir::AllDirs | QDir::NoDotAndDotDot, QDir::Name );

      for ( int ii = 0; ii < runs.size(); ii++ )
      {
         QString xml = experiment_xml( QDir( results + "/" + runs[ ii ] ),
                                       runs[ ii ] );

         if ( xml.isEmpty() )  continue;

         QMap< QString, QString > attrs = first_attributes( xml, "experiment" );

         if ( attrs.value( "guid" ) != guid )  continue;

         id   = attrs.value( "id", QString( "-1" ) );
         name = runs[ ii ];
         return true;
      }

      return false;
   }

   if ( type == US_DataPub::EditedData )
   {
      // An edit is a file of its run directory that names itself: looked
      // for only when a model names an edit the bundle does not carry
      QStringList runs = QDir( results ).entryList(
                         QDir::AllDirs | QDir::NoDotAndDotDot, QDir::Name );

      for ( int ii = 0; ii < runs.size(); ii++ )
      {
         if ( ! runIdIsUsable( runs[ ii ] ) )  continue;

         QDir        dir( results + "/" + runs[ ii ] );
         QStringList xmls = dir.entryList( QStringList( runs[ ii ] + ".*.xml" ),
                                           QDir::Files, QDir::Name );

         for ( int jj = 0; jj < xmls.size(); jj++ )
         {
            // runID.editID.runType.cell.channel.wavelength.xml
            if ( xmls[ jj ].split( '.' ).size() < 7 )  continue;

            QMap< QString, QString > attrs =
                  first_attributes( dir.absoluteFilePath( xmls[ jj ] ),
                                    "editGUID" );

            if ( attrs.value( "value" ) != guid )  continue;

            id   = QString( "-1" );
            name = xmls[ jj ];
            return true;
         }
      }

      return false;
   }

   if ( type == US_DataPub::RawData  ||  type == US_DataPub::TimeState )
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

/* Look a name up among the records the target already holds.

   The listing this needs is read once per record type and kept in an index,
   because the lookup is repeated: a rename walks one candidate name after
   another until it finds a free one, and reading the whole buffer, analyte
   or solution list again for each attempt is what made an import crawl.
*/
bool US_DataPubImporter::findByName( US_DataPub::EntityType type,
                                     const QString& name, QString& guid,
                                     QString& id )
{
   guid.clear();
   id  .clear();

   if ( name.isEmpty() )  return false;

   if ( ! indexedByName( type ) )
      return findOneByName( type, name, guid, id );

   QHash< QString, Named >&          held = namesHeld( type );
   QHash< QString, Named >::iterator it   = held.find( name );

   if ( it == held.end() )  return false;

   if ( ! it->resolved )
   {  // The listing gave the name and the ID; the GUID costs one more query
      it->guid     = guidOfRecord( type, it->id );
      it->resolved = true;
   }

   guid = it->guid;
   id   = it->id;

   return true;
}

bool US_DataPubImporter::indexedByName( US_DataPub::EntityType type )
{
   switch ( type )
   {
      // One run identifier can be asked about directly, which beats
      // listing every experiment of the target
      case US_DataPub::Experiment:  return false;

      // These are named by their file inside the run directory
      case US_DataPub::RawData:
      case US_DataPub::EditedData:
      case US_DataPub::TimeState:   return false;

      default:                      return true;
   }
}

QHash< QString, US_DataPubImporter::Named >& US_DataPubImporter::namesHeld(
      US_DataPub::EntityType type )
{
   int key = int( type );

   if ( ! held_names.contains( key ) )
   {
      QHash< QString, Named > index;

      readNames( type, index );

      held_names.insert( key, index );
   }

   return held_names[ key ];
}

void US_DataPubImporter::nameTaken( US_DataPub::EntityType type,
                                    const QString& name, const QString& guid,
                                    const QString& id )
{
   if ( type == US_DataPub::Experiment )
   {
      exp_by_guid.insert( guid, qMakePair( id, name ) );
      run_lookup .insert( name, qMakePair( id, guid ) );
   }

   if ( name.isEmpty()  ||  ! indexedByName( type ) )  return;

   int key = int( type );

   if ( ! held_names.contains( key ) )  return;   // Nothing read yet to add to

   Named entry;
   entry.id       = id;
   entry.guid     = guid;
   entry.resolved = true;

   held_names[ key ].insert( name, entry );
}

// Read every record the target holds of one type, by the name it goes under.
// The first record of a given name is the one a lookup finds, which is the
// order the listings themselves come back in.
void US_DataPubImporter::readNames( US_DataPub::EntityType type,
                                    QHash< QString, Named >& index )
{
   if ( opts.target == US_DataPub::TargetDb )
   {
      if ( dbase == nullptr )  return;

      QString     invID = QString::number( US_Settings::us_inv_ID() );
      QStringList query;

      // How the listing of each type is read: the procedure, the columns
      // holding the name and the ID, and the column holding the GUID when
      // the listing carries one (-1 when it has to be looked up later).
      int nameCol = -1;
      int idCol   =  0;
      int guidCol = -1;

      switch ( type )
      {
         case US_DataPub::Project:
            query << "get_project_desc" << invID;
            nameCol = 1;                        break;

         case US_DataPub::Solution:
            query << "all_solutionIDs"  << invID;
            nameCol = 1;                        break;

         case US_DataPub::Buffer:
            query << "get_buffer_desc"  << invID;
            nameCol = 1;                        break;

         case US_DataPub::Analyte:
            query << "get_analyte_desc" << invID;
            nameCol = 1;                        break;

         case US_DataPub::Model:
            query << "get_model_desc"   << invID;
            nameCol = 2;  guidCol = 1;          break;

         case US_DataPub::Noise:
            query << "get_noise_desc"   << invID;
            nameCol = 9;  guidCol = 1;          break;

         case US_DataPub::RotorCalibration:
         {
            // A listing that fails is a failed lookup, never "no names"
            QVector< US_Rotor::RotorCalibration > cals;

            if ( ! readCalibrationsDb( cals ) )  return;

            for ( int kk = 0; kk < cals.size(); kk++ )
            {
               if ( index.contains( cals[ kk ].label ) )  continue;

               Named entry;
               entry.id       = QString::number( cals[ kk ].ID );
               entry.guid     = cals[ kk ].GUID;
               entry.resolved = true;

               index.insert( cals[ kk ].label, entry );
            }

            return;
         }

         default:
            return;
      }

      dbase->query( query );

      if ( lookupFailed( tr( "its %1 records" )
                         .arg( US_DataPub::typeText( type ) ) ) )
         return;

      while ( dbase->next() )
      {
         QString rname = dbase->value( nameCol ).toString();

         if ( rname.isEmpty()  ||  index.contains( rname ) )  continue;

         Named entry;
         entry.id       = dbase->value( idCol ).toString();

         if ( guidCol >= 0 )
         {
            entry.guid     = dbase->value( guidCol ).toString();
            entry.resolved = true;
         }

         index.insert( rname, entry );
      }

      return;
   }

   // ---- disk target -------------------------------------------------------
   DiskLayout layout = disk_layout( type );

   if ( layout.prefix.isEmpty() )  return;

   QString     dir   = diskDir( type );
   QStringList files = QDir( dir ).entryList(
                       QStringList( layout.prefix + "???????.xml" ),
                       QDir::Files, QDir::Name );

   for ( int ii = 0; ii < files.size(); ii++ )
   {
      QString path  = dir + "/" + files[ ii ];
      QString rname = US_DataPubRecords::recordName( path, type );

      if ( rname.isEmpty()  ||  index.contains( rname ) )  continue;

      QMap< QString, QString > attrs = first_attributes( path,
                                                         layout.element );
      Named entry;
      entry.id       = attrs.value( "id", QString( "-1" ) );
      entry.guid     = attrs.value( layout.guidAttr );
      entry.resolved = true;

      index.insert( rname, entry );
   }
}

// The GUID of a record the listing named only by its ID
QString US_DataPubImporter::guidOfRecord( US_DataPub::EntityType type,
                                          const QString& id )
{
   if ( opts.target != US_DataPub::TargetDb  ||  dbase == nullptr )
      return QString();

   QStringList query;
   int         guidCol = 0;

   switch ( type )
   {
      case US_DataPub::Project:
         query << "get_project_info" << id;
         guidCol = 1;                     break;

      case US_DataPub::Buffer:
         query << "get_buffer_info"  << id;
         guidCol = 0;                     break;

      case US_DataPub::Analyte:
         query << "get_analyte_info" << id;
         guidCol = 0;                     break;

      case US_DataPub::Solution:
      {
         US_Solution solution;

         if ( solution.readFromDB( id.toInt(), dbase ) != IUS_DB2::OK )
         {
            lookupFailed( tr( "solution %1" ).arg( id ) );
            return QString();
         }

         return solution.solutionGUID;
      }

      default:
         return QString();
   }

   dbase->query( query );

   if ( lookupFailed( tr( "%1 %2" ).arg( US_DataPub::typeText( type ) )
                      .arg( id ) ) )
      return QString();

   if ( ! dbase->next() )  return QString();

   return dbase->value( guidCol ).toString();
}

// The types the index leaves out are looked up one name at a time
bool US_DataPubImporter::findOneByName( US_DataPub::EntityType type,
                                        const QString& name, QString& guid,
                                        QString& id )
{
   if ( type != US_DataPub::Experiment )
      return false;              // Named by their file inside the run directory

   if ( opts.target == US_DataPub::TargetDb )
   {
      if ( dbase == nullptr )  return false;

      // The bundle's own run IDs were asked about before the transaction
      if ( run_lookup.contains( name ) )
      {
         id   = run_lookup.value( name ).first;
         guid = run_lookup.value( name ).second;

         return ! id.isEmpty();
      }

      QStringList query;
      query << "get_experiment_info_by_runID" << name
            << QString::number( US_Settings::us_inv_ID() );
      dbase->query( query );

      if ( lookupFailed( tr( "run %1" ).arg( name ) ) )  return false;

      if ( ! dbase->next() )  return false;

      id   = dbase->value( 1 ).toString();
      guid = dbase->value( 2 ).toString();

      return ! id.isEmpty();
   }

   QString base = opts.outputDir.trimmed().isEmpty()
                  ? US_Settings::resultDir()
                  : opts.outputDir.trimmed() + "/results";
   QDir    dir( base + "/" + name );

   if ( ! dir.exists() )  return false;

   QString xml = experiment_xml( dir, name );

   if ( ! xml.isEmpty() )
   {
      QMap< QString, QString > attrs = first_attributes( xml, "experiment" );
      guid = attrs.value( "guid" );
      id   = attrs.value( "id", QString( "-1" ) );

      return true;
   }

   // A run directory without an experiment XML still occupies the name
   guid = QString();
   id   = QString( "-1" );

   return true;
}

QString US_DataPubImporter::targetFingerprint( US_DataPub::EntityType type,
                                               const QString& guid,
                                               const QString& id )
{
   // Never matched on an empty GUID: that would compare with whichever
   // record happened to come first
   if ( guid.isEmpty() )  return QString();

   QString ownElem;
   QString ownAttr;
   own_identity( type, ownElem, ownAttr );

   QString workDir = bundle.workPath() + "/target";
   QDir().mkpath( workDir );

   QString path = workDir + "/" + US_DataPub::typeKey( type ) + ".xml";

   if ( QFile::exists( path ) )  QFile::remove( path );

   if ( opts.target == US_DataPub::TargetDisk )
   {
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

         if ( attrs.value( layout.guidAttr ) != guid )  continue;

         return US_DataPubHash::fingerprint( fpath, ownElem, ownAttr );
      }

      return QString();
   }

   if ( dbase == nullptr )  return QString();

   switch ( type )
   {
      case US_DataPub::Project:
      {
         US_Project project;

         if ( project.readFromDB( id.toInt(), dbase ) != IUS_DB2::OK )
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

         if ( analyte.load( true, guid, dbase ) != IUS_DB2::OK )
            return QString();

         analyte.write( false, path );
         break;
      }

      case US_DataPub::Solution:
      {
         US_Solution solution;

         if ( solution.readFromDB( id.toInt(), dbase ) != IUS_DB2::OK )
            return QString();

         solution.saveToFile( path );
         break;
      }

      case US_DataPub::Model:
      {
         US_Model model;

         if ( model.load( id, dbase ) != IUS_DB2::OK )  return QString();

         model.write( path );
         break;
      }

      case US_DataPub::Noise:
      {
         US_Noise noise;

         if ( noise.load( id, dbase ) != IUS_DB2::OK )  return QString();

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

      default:
         // An experiment of the same run ID and another GUID is another run,
         // whatever it holds, so it is never "the same"
         return QString();
   }

   return US_DataPubHash::fingerprint( path, ownElem, ownAttr );
}

QString US_DataPubImporter::uniqueName( US_DataPub::EntityType type,
                                        const QString& name )
{
   QString suffix = opts.renameSuffix.trimmed();

   if ( suffix.isEmpty() )  suffix = tr( "imported" );

   QString base = name.isEmpty() ? US_DataPub::typeText( type ) : name;
   QString guid;
   QString id;

   if ( type == US_DataPub::Experiment )
   {  // A run ID goes into directory and file names: keep it usable
      suffix.replace( QRegularExpression( "[^A-Za-z0-9_-]" ), "_" );
      base  .replace( QRegularExpression( "[^A-Za-z0-9_-]" ), "_" );

      if ( suffix.isEmpty() )  suffix = "imported";
   }

   for ( int ii = 1; ii < 1000; ii++ )
   {
      QString candidate = ( ii == 1 )
                          ? QString( "%1 (%2)" ).arg( base ).arg( suffix )
                          : QString( "%1 (%2 %3)" ).arg( base ).arg( suffix )
                            .arg( ii );

      if ( type == US_DataPub::Experiment )
      {
         QString tail = ( ii == 1 ) ? QString( "-%1" ).arg( suffix )
                        : QString( "-%1-%2" ).arg( suffix ).arg( ii );
         candidate = base.left( qMax( 1, 80 - tail.size() ) ) + tail;

         if ( ! runIdIsUsable( candidate ) )  continue;
      }

      if ( ! findByName( type, candidate, guid, id ) )  return candidate;

      if ( ! lookup_error.isEmpty() )  return QString();
   }

   QString last = US_Util::new_guid().left( 8 );

   if ( type == US_DataPub::Experiment )
      return base.left( 70 ) + "-" + last;

   return base + "-" + last;
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
         QString projectDesc;      // The project description in the target
         QMap< QString, QString > solutionGUIDs;  // old GUID -> new GUID
         QMap< QString, QString > solutionIDs;    // new GUID -> new ID
         QMap< QString, QString > centerpieceIDs; // old serial -> new serial
         QString calibrationID;    // The rotor calibration ID in the target
         QString rotorID;          // The rotor ID in the target
   };

   bool remap_experiment_xml( const QString& path, const ExpRemap& remap,
                              QString& error )
   {
      AttrEdit edit = [ &remap ]( const QString& ename,
                                  QMap< QString, QString >& values )
      {
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
            if ( ! remap.projectDesc.isEmpty() )
               values.insert( "desc", remap.projectDesc );
         }

         else if ( ename == "calibration" )
         {
            if ( ! remap.calibrationID.isEmpty() )
               values.insert( "id", remap.calibrationID );
         }

         else if ( ename == "rotor" )
         {
            if ( ! remap.rotorID.isEmpty()  &&  values.contains( "id" ) )
               values.insert( "id", remap.rotorID );
            if ( ! remap.calibrationID.isEmpty()  &&
                 values.contains( "calibrationID" ) )
               values.insert( "calibrationID", remap.calibrationID );
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
      };

      return rewrite_xml( path, edit, error );
   }
}

/* The order the record types are replayed in, from their dependencies.

   The edges are those of the target plus those the manifest's records name.
   A database creates a solution with the ID of its run, so the run goes
   first there; a disk store keeps a solution as a file that names no run,
   while the run's experiment XML names its solutions (and is rewritten to
   name them as the target has them), so there the solutions go first and
   the manifest's solution-to-run dependency is the one edge left out.  The
   types are then sorted so that each comes after everything it needs,
   keeping a fixed order (hardware, chemistry, run, data, fits) where nothing
   decides; a cycle refuses the bundle.
*/
QList< US_DataPub::EntityType > US_DataPubImporter::importOrder(
      const US_DataPubManifest& manifest, US_DataPub::Target target,
      QString& error )
{
   typedef US_DataPub::EntityType Type;
   bool                    db    = ( target == US_DataPub::TargetDb );
   QList< Type >           types;

   // The order where no dependency decides: reference hardware, the
   // chemistry, the run, its data, then what was fitted to it
   types << US_DataPub::Project    << US_DataPub::RotorCalibration
         << US_DataPub::Centerpiece << US_DataPub::Buffer
         << US_DataPub::Analyte    << US_DataPub::Solution
         << US_DataPub::Experiment << US_DataPub::RawData
         << US_DataPub::TimeState  << US_DataPub::EditedData
         << US_DataPub::Model      << US_DataPub::Noise;
   QMap< int, QSet< int > > needs;          // type -> the types it needs first

   auto need = [ &needs ]( Type type, Type parent )
   {
      if ( type != parent )  needs[ int( type ) ].insert( int( parent ) );
   };

   // ---- what the target itself needs -------------------------------------
   need( US_DataPub::Experiment, US_DataPub::Project );
   need( US_DataPub::Experiment, US_DataPub::RotorCalibration );
   need( US_DataPub::Experiment, US_DataPub::Centerpiece );
   need( US_DataPub::Solution,   US_DataPub::Buffer );
   need( US_DataPub::Solution,   US_DataPub::Analyte );

   if ( db )
      need( US_DataPub::Solution,   US_DataPub::Experiment );
   else
      need( US_DataPub::Experiment, US_DataPub::Solution );

   need( US_DataPub::RawData,    US_DataPub::Experiment );
   need( US_DataPub::RawData,    US_DataPub::Solution );
   need( US_DataPub::TimeState,  US_DataPub::Experiment );
   need( US_DataPub::TimeState,  US_DataPub::RawData );
   need( US_DataPub::EditedData, US_DataPub::RawData );
   need( US_DataPub::Model,      US_DataPub::EditedData );
   need( US_DataPub::Model,      US_DataPub::Analyte );
   need( US_DataPub::Noise,      US_DataPub::Model );
   need( US_DataPub::Noise,      US_DataPub::EditedData );

   // ---- what the manifest's records name ---------------------------------
   QList< US_DataPubEntity > entities = manifest.all();

   for ( int ii = 0; ii < entities.size(); ii++ )
   {
      const US_DataPubEntity& entity = entities[ ii ];
      QList< QString >        keys   = entity.depends.keys();

      if ( ! types.contains( entity.type ) )  types << entity.type;

      for ( int jj = 0; jj < keys.size(); jj++ )
      {
         Type parent = US_DataPubEntity::dependType( keys[ jj ] );

         if ( parent == US_DataPub::UnknownType )  continue;

         if ( ! db  &&  entity.type == US_DataPub::Solution  &&
              parent == US_DataPub::Experiment )
            continue;

         need( entity.type, parent );
      }
   }

   // Every type an edge names is in the list, so a type can wait for it
   QList< int > named = needs.keys();

   for ( int ii = 0; ii < named.size(); ii++ )
   {
      if ( ! types.contains( Type( named[ ii ] ) ) )
         types << Type( named[ ii ] );

      for ( int parent : needs.value( named[ ii ] ) )
         if ( ! types.contains( Type( parent ) ) )  types << Type( parent );
   }

   // ---- dependencies first, the fixed order otherwise ------------------
   QList< Type > order;
   QSet< int >   placed;

   while ( order.size() < types.size() )
   {
      bool progress = false;

      for ( int ii = 0; ii < types.size(); ii++ )
      {
         int key = int( types[ ii ] );

         if ( placed.contains( key ) )  continue;

         // Every type it needs is in the list, so it waits for them all
         if ( ! ( needs.value( key ) - placed ).isEmpty() )  continue;

         order  << types[ ii ];
         placed << key;
         progress = true;
         break;                   // Start again from the manifest's order
      }

      if ( ! progress )
      {
         QStringList stuck;

         for ( int ii = 0; ii < types.size(); ii++ )
            if ( ! placed.contains( int( types[ ii ] ) ) )
               stuck << US_DataPub::typeText( types[ ii ] );

         error = tr( "The records of the bundle depend on each other in a"
                     " circle (%1), so there is no order to import them in" )
                 .arg( stuck.join( ", " ) );
         return QList< Type >();
      }
   }

   error.clear();

   return order;
}


QString US_DataPubImporter::runIdFor( const US_DataPubEntity& entity ) const
{
   QString runID   = entity.attrs.value( "runID" );
   QString expGUID = entity.depend( US_DataPub::Experiment );

   // An edit names its raw data rather than its run; the run is the raw
   // data's, or failing that the bundle's run of that ID.  Without this an
   // edit of a renamed run would land in the run it was renamed away from.
   if ( expGUID.isEmpty() )
   {
      US_DataPubEntity raw;

      if ( mani.entity( US_DataPub::RawData,
                        entity.depend( US_DataPub::RawData ), raw ) )
         expGUID = raw.depend( US_DataPub::Experiment );
   }

   if ( expGUID.isEmpty() )
   {
      QList< US_DataPubEntity > exps = mani.section( US_DataPub::Experiment );

      for ( int ii = 0; ii < exps.size()  &&  expGUID.isEmpty(); ii++ )
         if ( exps[ ii ].name == runID )  expGUID = exps[ ii ].guid;
   }

   QString renamed = nameMap.value( expGUID, QString() );

   if ( ! renamed.isEmpty() )  return renamed;

   return runID;
}

QString US_DataPubImporter::renamedFile( const US_DataPubEntity& entity ) const
{
   QString oldRun = entity.attrs.value( "runID" );
   QString newRun = runIdFor( entity );
   QString name   = entity.filename;

   if ( oldRun.isEmpty()  ||  newRun.isEmpty()  ||  oldRun == newRun )
      return name;

   if ( ! name.startsWith( oldRun + "." ) )  return name;

   return newRun + name.mid( oldRun.size() );
}

bool US_DataPubImporter::createDisk( const US_DataPubEntity& entity,
                                     const QString& workPath,
                                     const QString& name, QString& newID,
                                     QString& error )
{
   newID = QString( "-1" );

   US_DataPub::EntityType type = entity.type;

   if ( type == US_DataPub::Experiment  ||  type == US_DataPub::RawData  ||
        type == US_DataPub::EditedData  ||  type == US_DataPub::TimeState )
   {
      QString runID = ( type == US_DataPub::Experiment )
                      ? name : runIdFor( entity );
      QString dir   = diskResultDir( runID, true );

      if ( dir.isEmpty() )
      {
         error = tr( "%1 names the unusable run ID \"%2\"" )
                 .arg( entity.label() ).arg( runID );
         return false;
      }

      QString base = ( type == US_DataPub::Experiment )
                     ? runID + "." + entity.attrs.value( "runType", "RA" )
                             + ".xml"
                     : renamedFile( entity );
      QString path = dir + "/" + base;
      QString source = workPath;

      if ( ! plain_name( base ) )
      {
         error = tr( "%1 has no usable file name" ).arg( entity.label() );
         return false;
      }

      if ( QFile::exists( path ) )
      {
         error = tr( "The file %1 already exists; the import will not"
                     " overwrite it" ).arg( path );
         return false;
      }

      if ( type == US_DataPub::Experiment )
      {
         source = workCopy( entity, error );

         if ( source.isEmpty() )  return false;

         ExpRemap remap;
         remap.runID = runID;
         remap.expID = QString( "-1" );

         QString projGUID = entity.depend( US_DataPub::Project );

         if ( ! tproj_guid.isEmpty() )
         {  // The import was told which project to attach the runs to
            remap.projectID   = tproj_id;
            remap.projectGUID = tproj_guid;
            remap.projectDesc = tproj_desc;
         }

         else if ( ! projGUID.isEmpty()  &&  guidMap.contains( projGUID ) )
         {
            remap.projectID   = mappedId  ( projGUID );
            remap.projectGUID = mappedGuid( projGUID );
            remap.projectDesc = mappedName( projGUID );
         }

         else
         {  // The run's own reference: the target's ID for it, or none
            QByteArray data;
            bool       wellFormed = false;
            read_capped( source, data, error );
            QString named = element_value( data, "project", "guid",
                                           wellFormed );
            QString pid;
            QString pname;

            if ( ! named.isEmpty()  &&
                 findByGuid( US_DataPub::Project, named, pid, pname ) )
            {
               remap.projectID   = pid;
               remap.projectDesc = pname;
            }

            else
               remap.projectID   = QString( "-1" );
         }

         QString calGUID = entity.depend( US_DataPub::RotorCalibration );

         remap.calibrationID = ( ! calGUID.isEmpty()  &&
                                 guidMap.contains( calGUID ) )
                               ? mappedId( calGUID ) : QString( "-1" );
         remap.rotorID       = QString( "-1" );

         QList< US_DataPubEntity > sols = mani.section( US_DataPub::Solution );

         for ( int ii = 0; ii < sols.size(); ii++ )
         {
            QString newGUID = mappedGuid( sols[ ii ].guid );
            remap.solutionGUIDs.insert( sols[ ii ].guid, newGUID );
            remap.solutionIDs  .insert( newGUID, mappedId( sols[ ii ].guid ) );
         }

         QList< US_DataPubEntity > cps = mani.section( US_DataPub::Centerpiece );

         for ( int ii = 0; ii < cps.size(); ii++ )
            remap.centerpieceIDs.insert( cps[ ii ].id,
                                         idMap.value( cps[ ii ].guid, "0" ) );

         if ( ! remap_experiment_xml( source, remap, error ) )
         {
            error = tr( "The experiment XML of run %1 could not be rewritten"
                        " for the target: %2" ).arg( runID ).arg( error );
            return false;
         }
      }

      QString defsSource;
      QString defsTarget;
      QString ripSource;
      QString ripTarget;

      if ( type == US_DataPub::Experiment  &&
           ! entity.attrs.value( "riProfilePayload" ).isEmpty() )
      {
         // The RI intensity profile is a file of the run directory too
         if ( ! insideBundle( entity.attrs.value( "riProfilePayload" ),
                              ripSource, error )  ||
              ! QFile::exists( ripSource ) )
         {
            error = tr( "The RI intensity profile of run %1 is missing from"
                        " the bundle" ).arg( runID );
            return false;
         }

         ripTarget = dir + "/" + runID + ".RIProfile.xml";

         if ( QFile::exists( ripTarget ) )
         {
            error = tr( "The file %1 already exists; the import will not"
                        " overwrite it" ).arg( ripTarget );
            return false;
         }
      }

      if ( type == US_DataPub::TimeState )
      {
         // A time state carries a sibling file with its field definitions,
         // and is no use without it
         QString defs = entity.attrs.value( "definitionsPayload" );

         if ( defs.isEmpty()  ||  ! insideBundle( defs, defsSource, error )  ||
              ! QFile::exists( defsSource ) )
         {
            error = tr( "The field definitions of the time state of run %1"
                        " are missing from the bundle" ).arg( runID );
            return false;
         }

         defsTarget = US_DataPub::timeStateDefs( path );

         if ( QFile::exists( defsTarget ) )
         {
            error = tr( "The file %1 already exists; the import will not"
                        " overwrite it" ).arg( defsTarget );
            return false;
         }
      }

      if ( ! QFile::copy( source, path ) )
      {
         error = tr( "Cannot write %1" ).arg( path );
         return false;
      }

      disk_written << path;
      QFile::setPermissions( path, QFileDevice::ReadOwner
                                   | QFileDevice::WriteOwner
                                   | QFileDevice::ReadGroup
                                   | QFileDevice::ReadOther );

      if ( ! ripTarget.isEmpty() )
      {
         if ( ! QFile::copy( ripSource, ripTarget ) )
         {
            error = tr( "Cannot write %1" ).arg( ripTarget );
            return false;
         }

         disk_written << ripTarget;
      }

      if ( ! defsTarget.isEmpty() )
      {
         if ( ! QFile::copy( defsSource, defsTarget ) )
         {
            error = tr( "Cannot write %1" ).arg( defsTarget );
            return false;
         }

         disk_written << defsTarget;
      }

      return true;
   }

   // ---- the file-backed records of the data directory ---------------------
   if ( type == US_DataPub::Buffer  &&
        ! mapComponents( entity, workPath, true, error ) )
      return false;

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

   disk_written << path;
   QFile::setPermissions( path, QFileDevice::ReadOwner
                                | QFileDevice::WriteOwner
                                | QFileDevice::ReadGroup
                                | QFileDevice::ReadOther );

   return true;
}

/* Find the target's hardware for a run: by GUID or serial where the run
   names one, otherwise a stand-in with a warning (or, when the import is
   strict about hardware, a failure).  Nothing keeps a source ID just
   because the target happens to have a row of that number.
*/
bool US_DataPubImporter::resolveHardware( US_Experiment& exper,
                                          const US_DataPubEntity& entity,
                                          QString& error )
{
   // ---- the rotor calibration, and through it the rotor -------------------
   QString calGUID = entity.depend( US_DataPub::RotorCalibration );
   int     calID   = guidMap.contains( calGUID ) ? mappedId( calGUID ).toInt()
                                                 : 0;
   int     rotorID = 0;

   if ( calID > 0 )
   {
      US_Rotor::RotorCalibration calibration;
      US_Rotor::Status           cstat = calibration.readDB( calID, dbase );

      if ( cstat == US_Rotor::ROTOR_OK )
         rotorID = calibration.rotorID;

      else if ( cstat != US_Rotor::NOT_FOUND )
      {
         failLookup( tr( "rotor calibration %1" ).arg( calID ) );
         error = lookup_error;
         return false;
      }
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

         if ( lookupFailed( tr( "rotor %1" ).arg( rotorGUID ) ) )
         {
            error = lookup_error;
            return false;
         }

         if ( dbase->next() )
            rotorID = dbase->value( 0 ).toString().toInt();
      }
   }

   QVector< US_Rotor::Lab > labs;

   if ( read_labs_db( labs, dbase ) == US_Rotor::MISC_ERROR )
   {
      lookupFailed( tr( "its labs" ) );
      error = lookup_error.isEmpty() ? tr( "the labs could not be read" )
                                     : lookup_error;
      return false;
   }

   // The rotors of every lab, read once; a listing that fails is a failed
   // lookup, not a lab without rotors
   QList< QVector< US_Rotor::Rotor > > labRotors;

   for ( int ii = 0; ii < labs.size(); ii++ )
   {
      QVector< US_Rotor::Rotor > rotors;

      if ( US_Rotor::readRotorsFromDB( rotors, labs[ ii ].ID, dbase )
           == US_Rotor::MISC_ERROR )
      {
         failLookup( tr( "the rotors of lab %1" ).arg( labs[ ii ].ID ) );
         error = lookup_error;
         return false;
      }

      labRotors << rotors;
   }

   if ( rotorID < 1 )
   {  // Fall back to the first rotor the target knows about
      for ( int ii = 0; ii < labs.size()  &&  rotorID < 1; ii++ )
      {
         const QVector< US_Rotor::Rotor >& rotors = labRotors[ ii ];

         if ( rotors.isEmpty() )  continue;

         rotorID = rotors[ 0 ].ID;

         if ( ! warn( tr( "The rotor of run %1 is not in the target"
                          " database; rotor \"%2\" is recorded instead" )
                      .arg( exper.runID ).arg( rotors[ 0 ].name ), error ) )
            return false;
      }
   }

   if ( rotorID < 1 )
   {
      error = tr( "Run %1 cannot be imported: the target database has no"
                  " rotor" ).arg( exper.runID );
      return false;
   }

   exper.rotorID = rotorID;

   if ( calID < 1 )
   {  // Take the rotor's first calibration profile
      QVector< US_Rotor::RotorCalibration > cals;

      if ( US_Rotor::readCalibrationProfilesDB( cals, rotorID, dbase )
           == US_Rotor::MISC_ERROR )
      {
         failLookup( tr( "the calibrations of rotor %1" ).arg( rotorID ) );
         error = lookup_error;
         return false;
      }

      if ( cals.isEmpty() )
      {
         error = tr( "Run %1 cannot be imported: the target's rotor has no"
                     " calibration profile" ).arg( exper.runID );
         return false;
      }

      calID = cals[ 0 ].ID;

      if ( ! warn( tr( "The rotor calibration of run %1 is not in the target"
                       " database; calibration \"%2\" is recorded instead" )
                   .arg( exper.runID ).arg( cals[ 0 ].label ), error ) )
         return false;
   }

   exper.calibrationID = calID;

   // ---- the lab that owns the rotor --------------------------------------
   int labID = 0;

   for ( int ii = 0; ii < labs.size()  &&  labID < 1; ii++ )
   {
      const QVector< US_Rotor::Rotor >& rotors = labRotors[ ii ];

      for ( int jj = 0; jj < rotors.size(); jj++ )
      {
         if ( rotors[ jj ].ID != rotorID )  continue;

         labID = labs[ ii ].ID;
         break;
      }
   }

   if ( labID < 1 )
   {
      error = tr( "Run %1 cannot be imported: no lab of the target owns its"
                  " rotor" ).arg( exper.runID );
      return false;
   }

   exper.labID = labID;

   // ---- the instrument, by its serial number in that lab ------------------
   US_Rotor::Lab lab;

   if ( lab.readDB( labID, dbase ) != US_Rotor::ROTOR_OK )
   {
      failLookup( tr( "lab %1 and its instruments" ).arg( labID ) );
      error = lookup_error;
      return false;
   }

   QString wantSerial = exper.instrumentSerial;
   int     instID     = 0;

   for ( int ii = 0; ii < lab.instruments.size(); ii++ )
   {
      if ( wantSerial.isEmpty()  ||
           lab.instruments[ ii ].serial != wantSerial )  continue;

      instID = lab.instruments[ ii ].ID;
      break;
   }

   if ( instID < 1 )
   {
      if ( lab.instruments.isEmpty() )
      {
         error = tr( "Run %1 cannot be imported: the lab of its rotor has no"
                     " instrument" ).arg( exper.runID );
         return false;
      }

      if ( ! warn( tr( "The instrument of run %1 (serial \"%2\") is not in"
                       " the target database; instrument \"%3\" is recorded"
                       " instead" ).arg( exper.runID ).arg( wantSerial )
                   .arg( lab.instruments[ 0 ].serial ), error ) )
         return false;

      instID     = lab.instruments[ 0 ].ID;
      wantSerial = lab.instruments[ 0 ].serial;
   }

   exper.instrumentID     = instID;
   exper.instrumentSerial = wantSerial;

   // ---- the operator, by GUID ---------------------------------------------
   int operID = 0;

   if ( ! exper.operatorGUID.isEmpty() )
   {
      QStringList query;
      query << "get_personID_from_GUID" << exper.operatorGUID;
      dbase->query( query );

      if ( lookupFailed( tr( "operator %1" ).arg( exper.operatorGUID ) ) )
      {
         error = lookup_error;
         return false;
      }

      if ( dbase->next() )
         operID = dbase->value( 0 ).toString().toInt();
   }

   if ( operID < 1 )
   {
      if ( ! warn( tr( "The operator of run %1 is not in the target"
                       " database; the current investigator is recorded"
                       " instead" ).arg( exper.runID ), error ) )
         return false;

      operID = US_Settings::us_inv_ID();
   }

   exper.operatorID = operID;
   exper.invID      = US_Settings::us_inv_ID();

   return true;
}

/* Put a time state's field definitions beside its work copy.

   A time state is a pair of files and US_TimeState works on the pair: given
   the path of the binary it reads the definitions from the file of the same
   name with an .xml extension.  The work copy holds the payload of one
   record, so the definitions the bundle carries alongside have to be copied
   in next to it.
*/
bool US_DataPubImporter::stageTimeStateDefs( const US_DataPubEntity& entity,
                                             const QString& workPath,
                                             QString& error )
{
   QString defs = entity.attrs.value( "definitionsPayload" );
   QString source;

   if ( defs.isEmpty() )
   {
      error = tr( "The bundle carries no field definitions for the time"
                  " state of run %1, so it cannot be rebuilt" )
              .arg( runIdFor( entity ) );
      return false;
   }

   if ( ! insideBundle( defs, source, error ) )  return false;

   if ( ! QFile::exists( source ) )
   {
      error = tr( "The field definitions of the time state of run %1 are"
                  " missing from the bundle" ).arg( runIdFor( entity ) );
      return false;
   }

   QString target = US_DataPub::timeStateDefs( workPath );

   if ( target.isEmpty() )
   {
      error = tr( "The time state payload %1 has no name to go by" )
              .arg( entity.payload );
      return false;
   }

   if ( QFile::exists( target )  &&  ! QFile::remove( target ) )
   {
      error = tr( "Cannot replace %1" ).arg( target );
      return false;
   }

   if ( ! QFile::copy( source, target ) )
   {
      error = tr( "Cannot copy the field definitions of the time state of"
                  " run %1" ).arg( runIdFor( entity ) );
      return false;
   }

   QFile::setPermissions( target, QFileDevice::ReadOwner
                                  | QFileDevice::WriteOwner );
   error.clear();

   return true;
}

// A project record, written with the procedure US_Project::saveToDB() uses
// but without the copy that call writes into the local disk store
bool US_DataPubImporter::createProjectDb( US_Project& project, QString& error )
{
   QStringList query;
   query << "new_project2" << project.projectGUID << project.goals
         << project.molecules << project.purity << project.expense
         << project.bufferComponents << project.saltInformation
         << project.AUC_questions << project.expDesign << project.notes
         << project.projectDesc << project.status
         << QString::number( US_Settings::us_inv_ID() );

   int status = dbase->statusQuery( query );

   if ( status != IUS_DB2::OK )
   {
      error = tr( "The project \"%1\" could not be written to the database"
                  " (status %2): %3" ).arg( project.projectDesc ).arg( status )
              .arg( dbase->lastError() );
      return false;
   }

   project.projectID = dbase->lastInsertID();

   if ( project.projectID < 1 )
   {
      error = tr( "The database gave the project \"%1\" no ID" )
              .arg( project.projectDesc );
      return false;
   }

   return true;
}

/* A run, written as an experiment row with its speed steps.

   The run is staged the way US_ConvertIO reads one -- the experiment XML
   with the run's .auc files beside it, under the run ID it gets in the
   target -- so the triples and the speed profile come from the same reader
   the rest of UltraScan uses.  The staged run is kept for the raw data,
   which US_ConvertIO::writeRawDataToDB() writes once the solutions are in.
*/
bool US_DataPubImporter::createExperimentDb( const US_DataPubEntity& entity,
                                             const QString& /*workPath*/,
                                             const QString& name,
                                             QString& newID, QString& error )
{
   QString runType = entity.attrs.value( "runType", "RA" );
   QString runDir  = bundle.workPath() + "/runs/" + name;

   QDir( runDir ).removeRecursively();

   if ( ! QDir().mkpath( runDir ) )
   {
      error = tr( "Cannot create the staging directory %1" ).arg( runDir );
      return false;
   }

   // ---- the experiment XML, under the target's run ID ---------------------
   QString source = payloadPath( entity );
   QString xmlPath = runDir + "/" + name + "." + runType + ".xml";

   if ( ! QFile::copy( source, xmlPath ) )
   {
      error = tr( "Cannot stage the experiment XML of run %1" ).arg( name );
      return false;
   }

   QFile::setPermissions( xmlPath, QFileDevice::ReadOwner
                                   | QFileDevice::WriteOwner );
   ExpRemap remap;
   remap.runID = name;

   if ( ! remap_experiment_xml( xmlPath, remap, error ) )
   {
      error = tr( "The experiment XML of run %1 could not be rewritten: %2" )
              .arg( name ).arg( error );
      return false;
   }

   // ---- the raw data the bundle carries for it ----------------------------
   QList< US_DataPubEntity > raws = mani.section( US_DataPub::RawData );
   int                       nraw = 0;

   for ( int ii = 0; ii < raws.size(); ii++ )
   {
      if ( raws[ ii ].depend( US_DataPub::Experiment ) != entity.guid )
         continue;

      // The run has no target name on record yet, so the file is renamed
      // here: checkBundle() made sure it starts with its run ID
      QString file = name + raws[ ii ].filename.mid( entity.name.size() );

      if ( ! QFile::copy( payloadPath( raws[ ii ] ), runDir + "/" + file ) )
      {
         error = tr( "Cannot stage %1" ).arg( raws[ ii ].filename );
         return false;
      }

      nraw++;
   }

   // Read in place: a run is not copied, its triples hold whole solutions
   staged_runs.remove( entity.guid );
   QSharedPointer< US_ConvertIO::DiskRun > staged( new US_ConvertIO::DiskRun );
   staged_runs.insert( entity.guid, staged );
   US_ConvertIO::DiskRun& run = *staged;
   QString               detail;
   int                   status;

   if ( nraw > 0 )
   {
      status = US_ConvertIO::readDiskRun( runDir, run, detail );
   }

   else
   {  // No data to go with it: the XML alone is read, for the speed steps
      run.directory = runDir + "/";
      run.runID     = name;
      run.runType   = runType;
      run.experiment.clear();
      status = run.experiment.readFromDisk( run.triples, runType, name,
                                            run.directory, run.speedSteps,
                                            detail );
   }

   // A bundle may carry a part of a run's triples, which is what PARTIAL_XML
   // reports; anything else the reader says is wrong with the run is final
   if ( status != US_Convert::OK  &&  status != US_Convert::PARTIAL_XML )
   {
      error = tr( "The experiment of run %1 could not be read (status %2):"
                  " %3" ).arg( name ).arg( status ).arg( detail );
      return false;
   }

   if ( status == US_Convert::PARTIAL_XML )
      note( tr( "Run %1: %2" ).arg( name ).arg( detail ) );

   // Every triple has to be one of the bundle's raw data, under its own GUID
   for ( int ii = 0; ii < run.triples.size(); ii++ )
   {
      QString tripleGUID = US_Util::uuid_unparse(
                           (unsigned char*)run.triples[ ii ].tripleGUID );
      QString rawGUID    = ( ii < run.rawData.size() )
                           ? US_Util::uuid_unparse(
                             (unsigned char*)run.rawData[ ii ].rawGUID )
                           : QString();
      US_DataPubEntity raw;

      if ( tripleGUID != rawGUID  ||
           ! mani.entity( US_DataPub::RawData, rawGUID, raw ) )
      {
         error = tr( "Triple %1 of run %2 is not described by the run's"
                     " experiment XML" ).arg( run.triples[ ii ].tripleDesc )
                 .arg( name );
         return false;
      }
   }

   US_Experiment& exper = run.experiment;

   // An RI run's intensity profile: read the bundle's file the way the
   // experiment reads its own, and store what it writes back, which is the
   // XML US_Experiment::saveToDB() would have stored
   QByteArray RIxml;
   QString    rip = entity.attrs.value( "riProfilePayload" );

   if ( ! rip.isEmpty() )
   {
      QString rsource;
      QString rname = name + ".RIProfile.xml";

      if ( ! insideBundle( rip, rsource, error )  ||
           ! QFile::copy( rsource, runDir + "/" + rname ) )
      {
         error = tr( "Cannot stage the RI intensity profile of run %1" )
                 .arg( name );
         return false;
      }

      QFile::setPermissions( runDir + "/" + rname, QFileDevice::ReadOwner
                                                   | QFileDevice::WriteOwner );

      if ( exper.readRIDisk( name, runDir + "/" ) != US_Convert::OK  ||
           exper.RIProfile.isEmpty()  ||
           exper.saveRIDisk( name, runDir ) != US_Convert::OK  ||
           ! read_capped( runDir + "/" + rname, RIxml, error ) )
      {
         error = tr( "The RI intensity profile of run %1 could not be read" )
                 .arg( name );
         return false;
      }

      if ( RIxml.isEmpty() )
         note( tr( "Run %1 carries an RI profile but is not RI data; the"
                   " profile is not stored" ).arg( name ) );
   }
   exper.runID = name;

   // ---- the project -------------------------------------------------------
   // Every experiment in the database belongs to a project.  When the bundle
   // carries one, the run is attached to the project that was just imported;
   // when it does not, the project the run's own XML names is looked up in
   // the target, and created from that reference only when the target does
   // not have it.
   QString projGUID = entity.depend( US_DataPub::Project );
   int     projID   = 0;

   if ( ! tproj_guid.isEmpty() )
      projID = tproj_id.toInt();

   else if ( ! projGUID.isEmpty()  &&  guidMap.contains( projGUID ) )
      projID = mappedId( projGUID ).toInt();

   else
   {
      QString named = exper.project.projectGUID;

      if ( named.isEmpty() )
      {
         error = tr( "Run %1 names no project, and a database experiment"
                     " must belong to one" ).arg( name );
         return false;
      }

      QString pid;
      QString pname;

      if ( findByGuid( US_DataPub::Project, named, pid, pname ) )
      {
         projID = pid.toInt();
         note( tr( "Run %1 is attached to the project \"%2\" the target"
                   " already has" ).arg( name ).arg( pname ) );
      }

      else if ( ! lookup_error.isEmpty() )
      {
         error = lookup_error;
         return false;
      }

      else
      {
         US_Project project;
         project.projectGUID = named;
         project.projectDesc = exper.project.projectDesc;

         if ( ! createProjectDb( project, error ) )  return false;

         projID = project.projectID;
         note( tr( "The bundle carries no project; a project record \"%1\""
                   " is created from what run %2 names" )
               .arg( project.projectDesc ).arg( name ) );
      }
   }

   if ( projID < 1 )
   {
      error = tr( "The project of run %1 is not in the target database" )
              .arg( name );
      return false;
   }

   // ---- the hardware ------------------------------------------------------
   if ( ! resolveHardware( exper, entity, error ) )  return false;

   // ---- the experiment row, as US_Experiment::saveToDB() writes it, minus
   //      the project update and the local disk copy that call makes -------
   // An RI run carries its intensity profile, read above from the bundle's
   // file the way US_Experiment reads its own; any other run stores none.
   QStringList query;
   query << "new_experiment"
         << entity.guid
         << QString::number( projID )
         << name
         << QString::number( exper.labID )
         << QString::number( exper.instrumentID )
         << QString::number( exper.operatorID )
         << QString::number( exper.rotorID )
         << QString::number( exper.calibrationID )
         << exper.expType
         << QString( exper.opticalSystem )
         << QString::fromUtf8( RIxml )
         << exper.runTemp
         << exper.label
         << exper.comments
         << exper.protocolGUID
         << QString::number( US_Settings::us_inv_ID() );

   int dbstat = dbase->statusQuery( query );
   int expID  = ( dbstat == IUS_DB2::OK ) ? dbase->lastInsertID() : 0;

   if ( dbstat != IUS_DB2::OK  ||  expID < 1 )
   {
      error = tr( "Run %1 could not be written to the database (status %2):"
                  " %3" ).arg( name ).arg( dbstat ).arg( dbase->lastError() );
      return false;
   }

   exper.expID = expID;

   for ( int ii = 0; ii < run.speedSteps.size(); ii++ )
   {
      int sstat = US_SimulationParameters::speedstepToDB( dbase, expID,
                                                          &run.speedSteps[ ii ] );

      if ( sstat != IUS_DB2::OK )
      {
         error = tr( "Speed step %1 of run %2 could not be written (status"
                     " %3): %4" ).arg( ii + 1 ).arg( name ).arg( sstat )
                 .arg( dbase->lastError() );
         return false;
      }
   }

   if ( ! run.speedSteps.isEmpty() )
      note( tr( "Run %1 carries %2 speed steps" ).arg( name )
            .arg( run.speedSteps.size() ) );

   created_exps << entity.guid;
   newID = QString::number( expID );

   return true;
}

/* A solution, written with the procedures US_Solution::saveToDB() uses but
   without the solution and analyte files that call writes into the local
   disk store.  Its buffer and analytes are in the target by now: the
   preflight made sure of that, and the import order put them in first.
*/
bool US_DataPubImporter::createSolutionDb( const US_DataPubEntity& entity,
                                           const QString& workPath,
                                           const QString& name,
                                           QString& newID, QString& error )
{
   US_Solution solution;

   if ( solution.readFromFile( workPath, false ) != IUS_DB2::OK )
   {
      error = tr( "The solution payload could not be read" );
      return false;
   }

   // A new solution row is associated with an experiment; it has to be the
   // run the bundle says it belongs to, never some other run -- and a run
   // this import created: a run the target already had is not added to
   QString expGUID = entity.depend( US_DataPub::Experiment );
   int     expID   = guidMap.contains( expGUID ) ? mappedId( expGUID ).toInt()
                                                 : 0;

   if ( expID < 1 )
   {
      error = tr( "The solution \"%1\" belongs to no run of this import" )
              .arg( name );
      return false;
   }

   if ( ! created_exps.contains( expGUID ) )
   {
      error = tr( "run %1 is already in the target database without the"
                  " solution \"%2\"; the import does not add a solution to a"
                  " run it did not create" )
              .arg( mappedName( expGUID ) ).arg( name );
      return false;
   }

   QStringList query;
   query << "new_solution"
         << entity.guid
         << name
         << QString::number( solution.commonVbar20 )
         << QString::number( solution.storageTemp )
         << solution.notes
         << QString::number( expID )
         << QString( "1" )
         << QString::number( US_Settings::us_inv_ID() );

   int status = dbase->statusQuery( query );
   int solID  = ( status == IUS_DB2::OK ) ? dbase->lastInsertID() : 0;

   if ( status != IUS_DB2::OK  ||  solID < 1 )
   {
      error = tr( "The solution \"%1\" could not be written to the database"
                  " (status %2): %3" ).arg( name ).arg( status )
              .arg( dbase->lastError() );
      return false;
   }

   QString sSolID = QString::number( solID );

   if ( ! solution.buffer.GUID.isEmpty() )
   {
      query.clear();
      query << "new_solutionBuffer" << sSolID << QString( "0" )
            << solution.buffer.GUID;
      status = dbase->statusQuery( query );

      if ( status != IUS_DB2::OK )
      {
         error = tr( "The buffer of solution \"%1\" could not be recorded"
                     " (status %2): %3" ).arg( name ).arg( status )
                 .arg( dbase->lastError() );
         return false;
      }
   }

   for ( int ii = 0; ii < solution.analyteInfo.size(); ii++ )
   {
      query.clear();
      query << "new_solutionAnalyte" << sSolID << QString( "0" )
            << solution.analyteInfo[ ii ].analyte.analyteGUID
            << QString::number( solution.analyteInfo[ ii ].amount );
      status = dbase->statusQuery( query );

      if ( status != IUS_DB2::OK )
      {
         error = tr( "An analyte of solution \"%1\" could not be recorded"
                     " (status %2): %3" ).arg( name ).arg( status )
                 .arg( dbase->lastError() );
         return false;
      }
   }

   newID = sSolID;

   return true;
}

/* The raw data of one run, through US_ConvertIO::writeRawDataToDB(): the
   rawData rows and blobs, the cell rows and the experiment-solution links,
   each written once per run the way the rest of UltraScan writes them.
*/
bool US_DataPubImporter::writeRunRawData( const QString& expGUID,
                                          QString& error )
{
   if ( ! staged_runs.contains( expGUID ) )
   {
      error = tr( "The run of this raw data was not staged" );
      return false;
   }

   US_ConvertIO::DiskRun& run = *staged_runs.value( expGUID );

   // Centerpiece serials of the source -> the target's
   QMap< int, int > serials;
   QList< US_DataPubEntity > cps = mani.section( US_DataPub::Centerpiece );

   for ( int ii = 0; ii < cps.size(); ii++ )
      if ( idMap.contains( cps[ ii ].guid ) )
         serials.insert( cps[ ii ].id.toInt(),
                         idMap.value( cps[ ii ].guid ).toInt() );

   for ( int ii = 0; ii < run.triples.size(); ii++ )
   {
      US_Convert::TripleInfo& triple = run.triples[ ii ];
      QString                 oldSol = triple.solution.solutionGUID;
      QString                 newSol = mappedGuid( oldSol );
      QString                 sid;
      QString                 sname;

      // writeRawDataToDB() would create a solution it cannot find, through
      // US_Solution::saveToDB() and its local disk copy; it has to find it
      if ( newSol.isEmpty()  ||
           ! findByGuid( US_DataPub::Solution, newSol, sid, sname ) )
      {
         error = lookup_error.isEmpty()
                 ? tr( "The solution %1 of triple %2 is not in the target" )
                   .arg( oldSol ).arg( triple.tripleDesc )
                 : lookup_error;
         return false;
      }

      triple.solution.solutionGUID = newSol;

      if ( serials.contains( triple.centerpiece ) )
      {
         triple.centerpiece = serials.value( triple.centerpiece );
      }

      else
      {
         QString ignored;

         if ( ! warn( tr( "Centerpiece %1 of triple %2 is not in the bundle;"
                          " the triple names no centerpiece" )
                      .arg( triple.centerpiece ).arg( triple.tripleDesc ),
                      error ) )
            return false;

         triple.centerpiece = 0;
      }
   }

   run.experiment.expID = mappedId( expGUID ).toInt();
   run.experiment.invID = US_Settings::us_inv_ID();

   QString werror = US_ConvertIO::writeRawDataToDB( run.experiment,
                                                    run.triples,
                                                    run.directory, dbase );

   if ( ! werror.isEmpty() )
   {
      error = tr( "The raw data of run %1 could not be written: %2" )
              .arg( run.runID ).arg( werror.trimmed() );
      return false;
   }

   for ( int ii = 0; ii < run.triples.size(); ii++ )
   {
      QString rawGUID = US_Util::uuid_unparse(
                        (unsigned char*)run.triples[ ii ].tripleGUID );
      int     rawID   = 0;

      if ( US_ConvertIO::findRawDataId( dbase, rawGUID, rawID, error )
           != IUS_DB2::OK )
         return false;

      raw_written.insert( rawGUID, QString::number( rawID ) );
   }

   return true;
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

         if ( project.readFromFile( workPath ) != IUS_DB2::OK )
         {
            error = tr( "The project payload could not be read" );
            return false;
         }

         project.projectGUID = entity.guid;
         project.projectDesc = name;

         if ( ! createProjectDb( project, error ) )  return false;

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

         if ( lookupFailed( tr( "rotor %1" ).arg( calibration.rotorGUID ) ) )
         {
            error = lookup_error;
            return false;
         }

         int rotorID = 0;

         if ( dbase->next() )
            rotorID = dbase->value( 0 ).toString().toInt();

         if ( rotorID < 1 )
         {
            if ( ! warn( tr( "Rotor %1 is not in the target database, so the"
                             " rotor calibration \"%2\" is not imported" )
                         .arg( calibration.rotorGUID ).arg( name ), error ) )
               return false;

            record( entity, US_DataPub::ResolvedOmitted, QString(), name,
                    QString( "-1" ), tr( "the target does not have its rotor" ) );
            newID = "omitted";
            return true;
         }

         calibration.ID                      = 0;
         calibration.rotorID                 = rotorID;
         calibration.calibrationExperimentID = 0;
         calibration.label                   = name;

         if ( calibration.saveDB( rotorID, dbase ) != IUS_DB2::OK  ||
              calibration.ID < 1 )
         {
            error = tr( "The rotor calibration \"%1\" could not be written"
                        " to the database: %2" ).arg( name )
                    .arg( dbase->lastError() );
            return false;
         }

         newID = QString::number( calibration.ID );
         return true;
      }

      case US_DataPub::Buffer:
      {
         US_Buffer buffer;

         if ( ! mapComponents( entity, workPath, true, error ) )  return false;

         if ( ! buffer.readFromDisk( workPath ) )
         {
            error = tr( "The buffer payload could not be read" );
            return false;
         }

         for ( int ii = 0; ii < buffer.componentIDs.size(); ii++ )
         {
            if ( tcomps.contains( buffer.componentIDs[ ii ] ) )  continue;

            error = tr( "Component %1 of buffer \"%2\" is not in the"
                        " target" ).arg( buffer.componentIDs[ ii ] )
                    .arg( name );
            return false;
         }

         // The rows US_Buffer::saveToDB() writes for a new buffer, in its
         // order and with its formats -- the buffer, its components and its
         // extinction profile -- but not the copy that call always writes
         // into the local disk store.  The library writes the profile only
         // for a spectrum it was told is new; every imported one is.
         QString descrip = buffer.description;
         int     manx    = descrip.indexOf( "  [M]" );

         if ( manx > 0 )  descrip = descrip.left( manx ).simplified();

         QStringList query;
         query << "new_buffer"
               << entity.guid
               << descrip
               << QString::number( buffer.compressibility, 'e', 4 )
               << QString::number( buffer.pH             , 'f', 4 )
               << QString::number( buffer.density        , 'f', 6 )
               << QString::number( buffer.viscosity      , 'f', 5 )
               << US_Util::bool_string( buffer.manual )
               << QString( "1" )                           // Private
               << QString::number( US_Settings::us_inv_ID() );

         int status = dbase->statusQuery( query );
         int bufID  = ( status == IUS_DB2::OK ) ? dbase->lastInsertID() : 0;

         if ( status != IUS_DB2::OK  ||  bufID < 1 )
         {
            error = tr( "The buffer \"%1\" could not be written to the"
                        " database (status %2): %3" ).arg( name ).arg( status )
                    .arg( dbase->lastError() );
            return false;
         }

         QString sBufID = QString::number( bufID );

         for ( int ii = 0; ii < buffer.componentIDs.size(); ii++ )
         {
            double conc = ( ii < buffer.concentration.size() )
                          ? buffer.concentration[ ii ] : 0.0;
            query.clear();
            query << "add_buffer_component" << sBufID
                  << buffer.componentIDs[ ii ]
                  << QString::number( conc, 'f', 5 );
            status = dbase->statusQuery( query );

            if ( status != IUS_DB2::OK )
            {
               error = tr( "Component %1 of buffer \"%2\" could not be"
                           " recorded (status %3): %4" )
                       .arg( buffer.componentIDs[ ii ] ).arg( name )
                       .arg( status ).arg( dbase->lastError() );
               return false;
            }
         }

         if ( ! buffer.extinction.isEmpty() )
         {
            QString compType( "Buffer" );
            QString valType ( "molarExtinction" );

            if ( US_ExtProfile::create_eprofile( dbase, bufID, compType,
                                                 valType,
                                                 buffer.extinction ) < 1 )
            {
               error = tr( "The extinction profile of buffer \"%1\" could"
                           " not be written: %2" ).arg( name )
                       .arg( dbase->lastError() );
               return false;
            }
         }

         newID = sBufID;
         return true;
      }

      case US_DataPub::Analyte:
      {
         US_Analyte analyte;

         if ( analyte.readFromFile( workPath ) != IUS_DB2::OK )
         {
            error = tr( "The analyte payload could not be read" );
            return false;
         }

         // The rows US_Analyte::write_db() writes for a new analyte, in its
         // order and with its formats -- the analyte, a nucleotide's
         // details and the extinction profile -- but not the copy that call
         // always writes into the local disk store.  The library writes the
         // profile only for a spectrum it was told is new; every imported
         // one is.
         QString atype = ( analyte.type == US_Analyte::PROTEIN ) ? "Protein"
                       : ( analyte.type == US_Analyte::DNA     ) ? "DNA"
                       : ( analyte.type == US_Analyte::RNA     ) ? "RNA"
                                                                 : "Other";
         QString spectrum;
         double  lambda1 = 0.0;
         double  coeff1  = 0.0;

         if ( ! analyte.extinction.isEmpty() )
         {
            lambda1 = analyte.extinction.firstKey();
            coeff1  = analyte.extinction.first();
         }

         else if ( ! analyte.refraction.isEmpty() )
         {
            lambda1 = analyte.refraction.firstKey();
            coeff1  = analyte.refraction.first();
         }

         else if ( ! analyte.fluorescence.isEmpty() )
         {
            lambda1 = analyte.fluorescence.firstKey();
            coeff1  = analyte.fluorescence.first();
         }

         if ( coeff1 != 0.0 )
            spectrum = QString::number( lambda1 ) + "/"
                       + QString::number( coeff1 );

         QStringList query;
         query << "new_analyte" << entity.guid << atype
               << analyte.sequence
               << QString::number( analyte.vbar20 )
               << analyte.description
               << spectrum
               << QString::number( analyte.mw )
               << US_Util::bool_string( analyte.grad_form )
               << QString::number( US_Settings::us_inv_ID() );

         int status = dbase->statusQuery( query );
         int anaID  = ( status == IUS_DB2::OK ) ? dbase->lastInsertID() : 0;

         if ( status != IUS_DB2::OK  ||  anaID < 1 )
         {
            error = tr( "The analyte \"%1\" could not be written to the"
                        " database (status %2): %3" ).arg( name )
                    .arg( status ).arg( dbase->lastError() );
            return false;
         }

         QString sAnaID = QString::number( anaID );

         if ( analyte.type == US_Analyte::DNA  ||
              analyte.type == US_Analyte::RNA )
         {
            query.clear();
            query << "set_nucleotide_info" << sAnaID
                  << QString::number( analyte.doubleStranded )
                  << QString::number( analyte.complement )
                  << QString::number( analyte._3prime )
                  << QString::number( analyte._5prime )
                  << QString::number( analyte.sodium )
                  << QString::number( analyte.potassium )
                  << QString::number( analyte.lithium )
                  << QString::number( analyte.magnesium )
                  << QString::number( analyte.calcium );
            status = dbase->statusQuery( query );

            if ( status != IUS_DB2::OK )
            {
               error = tr( "The nucleotide details of analyte \"%1\" could"
                           " not be written (status %2): %3" ).arg( name )
                       .arg( status ).arg( dbase->lastError() );
               return false;
            }
         }

         if ( ! analyte.extinction.isEmpty() )
         {
            QString compType( "Analyte" );
            QString valType ( "molarExtinction" );

            if ( US_ExtProfile::create_eprofile( dbase, anaID, compType,
                                                 valType,
                                                 analyte.extinction ) < 1 )
            {
               error = tr( "The extinction profile of analyte \"%1\" could"
                           " not be written: %2" ).arg( name )
                       .arg( dbase->lastError() );
               return false;
            }
         }

         newID = sAnaID;
         return true;
      }

      case US_DataPub::Solution:
         return createSolutionDb( entity, workPath, name, newID, error );

      case US_DataPub::Experiment:
         return createExperimentDb( entity, workPath, name, newID, error );

      case US_DataPub::RawData:
      {
         QString expGUID = entity.depend( US_DataPub::Experiment );

         if ( ! created_exps.contains( expGUID ) )
         {
            error = tr( "The experiment of %1 was not imported" )
                    .arg( entity.filename );
            return false;
         }

         if ( ! writeRunRawData( expGUID, error ) )  return false;

         if ( ! raw_written.contains( entity.guid ) )
         {
            error = tr( "%1 was not written with its run" )
                    .arg( entity.filename );
            return false;
         }

         newID = raw_written.value( entity.guid );
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

         // US_TimeState reads the field definitions from beside the binary,
         // and the work copy holds the binary alone, so the definitions
         // have to be put there first
         if ( ! stageTimeStateDefs( entity, workPath, error ) )  return false;

         int tmstID = US_TimeState::dbCreate( dbase, expID.toInt(), workPath );

         if ( tmstID < 1 )
         {
            error = tr( "The time state of run %1 could not be written to"
                        " the database (status %2)" )
                    .arg( runIdFor( entity ) ).arg( tmstID );
            return false;
         }

         int status = US_TimeState::dbUpload( dbase, tmstID, workPath );

         if ( status != IUS_DB2::OK )
         {
            error = tr( "The time state data of run %1 could not be uploaded"
                        " (status %2): %3" ).arg( runIdFor( entity ) )
                    .arg( status ).arg( dbase->lastError() );
            return false;
         }

         newID = QString::number( tmstID );
         return true;
      }

      case US_DataPub::EditedData:
      {
         QString rawGUID = mappedGuid( entity.depend( US_DataPub::RawData ) );
         int     rawID   = 0;

         if ( US_ConvertIO::findRawDataId( dbase, rawGUID, rawID, error )
              != IUS_DB2::OK )
            return false;

         US_ConvertIO::EditedDataRecord rec;
         rec.rawDataID = rawID;
         rec.editGUID  = entity.guid;
         rec.label     = runIdFor( entity );
         rec.filename  = renamedFile( entity );

         // No comment, as us_edit and us_import_run store an edit: the
         // triple is in the file name already
         rec.comment.clear();

         int editID = 0;

         if ( US_ConvertIO::createEditedData( dbase, rec, editID, error )
              != IUS_DB2::OK )
            return false;

         if ( editID < 1 )
         {
            error = tr( "The database gave the edit %1 no ID" )
                    .arg( rec.filename );
            return false;
         }

         if ( US_ConvertIO::uploadEditedDataBlob( dbase, editID, workPath,
                                                  error ) != IUS_DB2::OK )
            return false;

         newID = QString::number( editID );
         return true;
      }

      case US_DataPub::Model:
      {
         // Written with the procedure US_Model::write() uses, but without
         // the copy that call always writes into the local models directory.
         // The model is stored as the payload's own text, which the import
         // has already pointed at the target's edit.
         US_Model   model;
         QByteArray contents;

         if ( model.load( workPath ) != IUS_DB2::OK  ||
              ! read_capped( workPath, contents, error ) )
         {
            error = tr( "The model payload could not be read" );
            return false;
         }

         QStringList query;
         query << "new_model" << entity.guid << name << QString( contents )
               << QString::number( model.variance )
               << QString::number( model.meniscus )
               << model.editGUID
               << QString::number( US_Settings::us_inv_ID() );

         int status = dbase->statusQuery( query );
         int modelID = ( status == IUS_DB2::OK ) ? dbase->lastInsertID() : 0;

         if ( status != IUS_DB2::OK  ||  modelID < 1 )
         {
            error = tr( "The model \"%1\" could not be written to the"
                        " database (status %2): %3" ).arg( name ).arg( status )
                    .arg( dbase->lastError() );
            return false;
         }

         newID = QString::number( modelID );
         return true;
      }

      case US_DataPub::Noise:
      {
         US_Noise noise;

         if ( noise.load( workPath ) != IUS_DB2::OK )
         {
            error = tr( "The noise payload could not be read" );
            return false;
         }

         if ( noise.write( dbase ) != IUS_DB2::OK )
         {
            error = tr( "The noise record \"%1\" could not be written to"
                        " the database: %2" ).arg( name ).arg( noise.message );
            return false;
         }

         QStringList query;
         query << "get_noiseID" << noise.noiseGUID;
         dbase->query( query );

         if ( lookupFailed( tr( "noise %1" ).arg( noise.noiseGUID ) )  ||
              ! dbase->next() )
         {
            error = tr( "The noise record \"%1\" was written but cannot be"
                        " found" ).arg( name );
            return false;
         }

         newID = dbase->value( 0 ).toString();
         return true;
      }

      default:
         break;
   }

   error = tr( "%1 cannot be imported into a database" ).arg( entity.label() );

   return false;
}
