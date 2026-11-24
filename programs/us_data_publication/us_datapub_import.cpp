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
   hw_read     = false;
   cp_read     = false;
   step_count  = 0;
   user_level  = -1;
   catalog_version = -1;
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
   bundle.cleanup();               // The tree, then its lock
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
   // The rule the export checks its records against too
   return US_DataPub::guidIsUsable( guid );
}

QString US_DataPubImporter::resolutionText( US_DataPub::Resolution resolution )
{
   return US_DataPub::resolutionText( resolution );
}

// ------------------------------------------------------------ small helpers

namespace
{
   // A rotor calibration as a message names it: its label in quotes, or,
   // for a calibration without one, its GUID
   QString calibration_text( const QString& label, const QString& guid )
   {
      return label.trimmed().isEmpty() ? guid
                                       : QString( "\"%1\"" ).arg( label );
   }

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

// ------------------------------------------------------- staging directories

namespace
{
   // The name of the lock an importer holds in its staging directory
   const char* const stage_lock_name = "import.lock";

   // A staging directory without a lock that has not changed for this long
   // belongs to no running importer or exporter
   const qint64 stale_unlocked_secs = 2 * 24 * 3600;
}

int US_DataPubImporter::sweepStaleStaging( void )
{
   QString     parent  = US_Settings::tmpDir() + "/us_datapub";
   QStringList dirs    = QDir( parent ).entryList( QStringList( "bundle_*" ),
                                                   QDir::Dirs
                                                   | QDir::NoDotAndDotDot );
   int         removed = 0;
   QDateTime   now     = QDateTime::currentDateTimeUtc();

   for ( int ii = 0; ii < dirs.size(); ii++ )
   {
      QString path = parent + "/" + dirs[ ii ];
      QString lock = path + "/" + stage_lock_name;
      bool    gone = false;

      if ( QFile::exists( lock ) )
      {
         // Stale only when the process that took it is not running: the
         // lock of a live importer, however old, is never taken over
         QLockFile probe( lock );
         probe.setStaleLockTime( 0 );

         if ( ! probe.tryLock( 0 ) )  continue;

         probe.unlock();
         gone = true;
      }

      else
      {
         QDateTime changed = QFileInfo( path ).lastModified().toUTC();

         gone = changed.isValid()  &&
                changed.secsTo( now ) > stale_unlocked_secs;
      }

      if ( gone  &&  QDir( path ).removeRecursively() )  removed++;
   }

   return removed;
}

/* The old writer framed a sequence S as "\n", then S in pieces of 80
   characters each followed by "\n", then what is left of S (fewer than 80
   characters) and a last "\n".  The pieces are counted in characters,
   whatever they hold -- a line break of S's own included -- so the framing
   is taken off by counting too, and only when the text is exactly such a
   framing; any other text is S itself.
*/
QString US_DataPubImporter::unframedSequence( const QString& fileText )
{
   QString text = fileText;

   for ( ;; )
   {
      if ( text.size() < 2  ||
           ! text.startsWith( '\n' )  ||  ! text.endsWith( '\n' ) )
         return text;

      QString inner;
      int     pos  = 1;
      int     size = text.size();

      // The full pieces: 80 characters and a line break, with at least the
      // last line break still to come
      while ( size - pos >= 82  &&  text[ pos + 80 ] == '\n' )
      {
         inner += text.mid( pos, 80 );
         pos   += 81;
      }

      // What is left: fewer than 80 characters, then the last line break
      int left = size - pos - 1;

      if ( left < 0  ||  left >= 80 )  return text;

      inner += text.mid( pos, left );
      text   = inner;
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

   // What importers that were killed left behind
   int swept = sweepStaleStaging();

   if ( swept > 0 )
      note( tr( "%1 staging directories left behind by importers that are"
                " no longer running were removed" ).arg( swept ) );

   // The bundle takes the lock in its staging directory as soon as it is
   // made, before anything is unpacked into it, and lets go of it only
   // when everything else has been removed
   bundle.setStagingLock( stage_lock_name );

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

   // Every run ID has to be one a run directory can be called; said first,
   // and in these words, since it is the most likely thing to be wrong
   for ( int ii = 0; ii < entities.size(); ii++ )
   {
      const US_DataPubEntity& entity = entities[ ii ];
      US_DataPub::EntityType  type   = entity.type;

      if ( type == US_DataPub::Experiment  &&  ! runIdIsUsable( entity.name ) )
      {
         error = tr( "The run ID \"%1\" in the bundle is not usable: a run"
                     " ID may hold only letters, digits, the underscore"
                     " and the hyphen, and at most 80 of them" )
                 .arg( entity.name );
         return false;
      }

      if ( ( type == US_DataPub::RawData  ||  type == US_DataPub::EditedData  ||
             type == US_DataPub::TimeState )  &&
           ! runIdIsUsable( entity.attrs.value( "runID" ) ) )
      {
         error = tr( "%1 names the unusable run ID \"%2\"" )
                 .arg( entity.label() ).arg( entity.attrs.value( "runID" ) );
         return false;
      }
   }

   // The rules an export applies before it packs a bundle (GUIDs, run
   // types, the file names of a run's files, a time state's definitions):
   // one implementation, so that what an export writes an import accepts
   if ( ! mani.checkAcceptance( error ) )  return false;

   // ... and, on top, where every path really leads: a path that reads
   // well can still be a symbolic link out of the bundle
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

      // Every extra payload -- a time state's definitions, an RI profile,
      // a buffer's components, a record's spectra -- is a path into the
      // bundle as well
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

   // What the server lets the person logged in do decides some of what
   // follows: which analytes they may use, which hardware they may add
   if ( ! readUserLevel( error ) )  return false;

   // The character sets the target's text goes into, which the payloads'
   // text is checked against
   type_charsets.clear();

   if ( opts.target == US_DataPub::TargetDb )  readTargetCharsets();

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

   // The runs are looked up now, before any transaction is open, and so is
   // everything else that can be
   if ( ! resolveExperiments( error ) )  return false;

   if ( ! lookUpInAdvance( error ) )     return false;

   // ... and what the server would refuse only when it is written
   if ( ! checkServerRules( error ) )    return false;

   return true;
}

namespace
{

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
   holds, belongs to someone else: the import refuses it, rather than create
   a second experiment of that GUID or attach the data to another person's
   records.  The raw data lookup (get_rawDataID_from_GUID) checks the
   permission of the person logged in: it finds anyone's raw data for an
   admin, and answers "not permitted" for someone else's to anyone else --
   which says just as much.  A run of someone else's without raw data in the
   database cannot be seen at all; experiment.experimentGUID is UNIQUE
   (us3.sql), so its second copy is refused by new_experiment and the import
   rolled back.
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

         if ( dbase->lastErrno() != IUS_DB2::NOTPERMITTED  &&
              lookupFailed( tr( "raw data %1" ).arg( raws[ jj ].guid ) ) )
         {
            error = lookup_error;
            return false;
         }

         // The lookup checks the permission of the person logged in: for
         // anyone but an admin, someone else's raw data is refused
         bool foreign = ( dbase->lastErrno() == IUS_DB2::NOTPERMITTED );

         if ( ! foreign  &&  ! dbase->next() )  continue;

         // Found, and permitted: whose it is is what the investigator's
         // listing says.  Permitted is not the investigator's own: an
         // admin may read anyone's, and the raw data of experiment 1 --
         // the "magic" ID verify_experiment_permission lets anyone use --
         // is found for everyone, although it is the run of whoever's
         // first run it became
         if ( ! foreign )
         {
            QString rawID = dbase->value( 0 ).toString();

            if ( ! readOwned( US_DataPub::RawData ) )
            {
               error = lookup_error;
               return false;
            }

            const Owned& list = owned[ int( US_DataPub::RawData ) ];
            bool         mine = list.ids.contains( rawID )  ||
                                list.guids.contains( raws[ jj ].guid.toLower() );

            if ( mine )
            {
               error = tr( "Run %1 (experiment %2) is not among the runs of"
                           " investigator %3 in the target database, but its"
                           " raw data %4 is: it belongs to another run of"
                           " the same investigator, under another experiment"
                           " GUID.  The import does not put the same raw"
                           " data into a second run" )
                       .arg( exp.name ).arg( exp.guid ).arg( invID )
                       .arg( raws[ jj ].filename );
               return false;
            }
         }

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

/* True when a record of a type is in the bundle or already in the target --
   and there, when it is a record of a type others own (a project, an edit,
   a model, a noise record, a solution), one of the investigator's own.  The
   database's GUID lookups are not limited to one investigator, so without
   that check an import would attach its records to whatever record of that
   GUID the database holds.
*/
bool US_DataPubImporter::availableAs( US_DataPub::EntityType type,
                                      const QString& guid, QString& error )
{
   US_DataPubEntity entity;

   if ( mani.entity( type, guid, entity ) )  return true;

   QString id;
   QString name;

   if ( findByGuid( type, guid, id, name ) )
   {
      if ( ! checkOwner( type, guid, id, error ) )  return false;

      // A buffer or analyte the bundle does not carry cannot be copied:
      // it has to be one the investigator may use as it is
      if ( sharedUsable( type, guid, id, error ) )  return true;

      if ( error.isEmpty() )
         error = tr( "%1 %2 is in the target database, but investigator %3"
                     " may not use it (%4), and the bundle does not carry it,"
                     " so the import cannot make a copy of its own; export"
                     " it with the bundle" )
                 .arg( US_DataPub::typeText( type ) ).arg( guid )
                 .arg( US_Settings::us_inv_ID() )
                 .arg( type == US_DataPub::Analyte
                       ? tr( "it is another investigator's analyte" )
                       : tr( "it is another investigator's private buffer" ) );

      return false;
   }

   if ( guid_foreign.contains( US_DataPub::typeKey( type ) + ":" + guid ) )
      error = foreignText( type, guid );

   else if ( ! lookup_error.isEmpty() )
      error = lookup_error;
   else
      error = tr( "%1 %2 is neither in the bundle nor in the target" )
              .arg( US_DataPub::typeText( type ) ).arg( guid );

   return false;
}

// ------------------------------------------------- whose records the target holds

bool US_DataPubImporter::ownedType( US_DataPub::EntityType type )
{
   switch ( type )
   {
      case US_DataPub::Project:
      case US_DataPub::Solution:
      case US_DataPub::RawData:
      case US_DataPub::EditedData:
      case US_DataPub::Model:
      case US_DataPub::Noise:      return true;

      // Buffers and analytes are chemistry the investigators of a database
      // share; hardware and runs have rules of their own
      default:                     return false;
   }
}

/* The investigator's own records of a type, from the listing the rest of
   UltraScan lists them with.  Each listing is limited to the investigator
   it is given (and refused for another one, unless the person logged in is
   an admin), which is what the GUID lookups are not.
*/
bool US_DataPubImporter::readOwned( US_DataPub::EntityType type )
{
   if ( owned.contains( int( type ) ) )  return true;

   if ( dbase == nullptr )  return false;

   QString     invID = QString::number( US_Settings::us_inv_ID() );
   QStringList query;
   int         guidCol = -1;           // -1: the listing carries no GUID

   switch ( type )
   {
      case US_DataPub::Project:
         query << "get_project_desc"  << invID;                break;
      case US_DataPub::Analyte:
         query << "get_analyte_desc"  << invID;                break;
      case US_DataPub::Solution:
         query << "all_solutionIDs"   << invID;                break;
      case US_DataPub::RawData:
         query << "all_rawDataIDs"    << invID;  guidCol = 9;  break;
      case US_DataPub::EditedData:
         query << "all_editedDataIDs" << invID;  guidCol = 9;  break;
      case US_DataPub::Model:
         query << "get_model_desc"    << invID;  guidCol = 1;  break;
      case US_DataPub::Noise:
         query << "get_noise_desc"    << invID;  guidCol = 1;  break;
      default:
         owned.insert( int( type ), Owned() );
         return true;
   }

   dbase->query( query );

   if ( lookupFailed( tr( "the investigator's %1 records" )
                      .arg( US_DataPub::typeText( type ) ) ) )
      return false;

   Owned list;

   while ( dbase->next() )
   {
      QString id = dbase->value( 0 ).toString();
      list.ids << id;

      if ( guidCol >= 0 )
         list.guids << dbase->value( guidCol ).toString().toLower();

      // The edit a model was fitted to, which its noise records name
      if ( type == US_DataPub::Model )
         list.edits.insert( dbase->value( 1 ).toString().toLower(),
                            dbase->value( 6 ).toString() );
   }

   owned.insert( int( type ), list );

   return true;
}

bool US_DataPubImporter::checkOwner( US_DataPub::EntityType type,
                                     const QString& guid, const QString& id,
                                     QString& error )
{
   if ( opts.target != US_DataPub::TargetDb  ||  ! ownedType( type ) )
      return true;

   if ( ! readOwned( type ) )
   {
      error = lookup_error;
      return false;
   }

   const Owned& list = owned[ int( type ) ];

   if ( list.guids.contains( guid.toLower() )  ||  list.ids.contains( id ) )
      return true;

   error = foreignText( type, guid );

   return false;
}

QString US_DataPubImporter::foreignText( US_DataPub::EntityType type,
                                         const QString& guid ) const
{
   return tr( "%1 %2 is in the target database, but it is not a record of"
              " investigator %3: it belongs to another investigator.  The"
              " import neither reuses someone else's records nor attaches"
              " anything to them; import as the investigator who owns it,"
              " or export it with the bundle" )
          .arg( US_DataPub::typeText( type ) ).arg( guid )
          .arg( US_Settings::us_inv_ID() );
}

/* A record of the bundle that the target database already holds under its
   GUID is reused, and whatever the bundle hangs from it is attached to it.
   That is right for the investigator's own record and wrong for anyone
   else's, so a record of someone else -- one the lookup refused, or one
   that is not in the investigator's listing -- refuses the bundle before
   anything is written.
*/
bool US_DataPubImporter::checkReuse( QString& error )
{
   if ( opts.target != US_DataPub::TargetDb )  return true;

   QList< US_DataPubEntity > entities = mani.all();

   for ( int ii = 0; ii < entities.size(); ii++ )
   {
      const US_DataPubEntity& entity = entities[ ii ];

      if ( ! ownedType( entity.type ) )  continue;

      // A project the import was told to use replaces the bundle's
      if ( entity.type == US_DataPub::Project  &&  ! tproj_guid.isEmpty() )
         continue;

      QString id;
      QString name;

      if ( findByGuid( entity.type, entity.guid, id, name ) )
      {
         if ( checkOwner( entity.type, entity.guid, id, error ) )  continue;

         error = tr( "%1: %2" ).arg( entity.label() ).arg( error );
         return false;
      }

      if ( ! lookup_error.isEmpty() )
      {
         error = lookup_error;
         return false;
      }

      if ( guid_foreign.contains( US_DataPub::typeKey( entity.type ) + ":"
                                  + entity.guid ) )
      {
         error = tr( "%1: %2" ).arg( entity.label() )
                 .arg( foreignText( entity.type, entity.guid ) );
         return false;
      }
   }

   return true;
}

// ------------------------------------------- what the server allows the person

/* The user level of the person logged in, which decides what the server
   lets the import do: an administrator (3) may create rotor calibrations and
   use any analyte, level 2 may add buffer components.  Read once, before
   anything else is asked, from get_user_info -- the level of the person the
   connection is logged in as, which is what the procedures check, not that
   of the investigator the import writes for.
*/
bool US_DataPubImporter::readUserLevel( QString& error )
{
   if ( user_level >= 0 )  return true;

   if ( opts.target != US_DataPub::TargetDb  ||  dbase == nullptr )
   {
      user_level = 0;
      return true;
   }

   QStringList query( "get_user_info" );
   dbase->query( query );

   if ( lookupFailed( tr( "the person logged in" ) ) )
   {
      error = lookup_error;
      return false;
   }

   if ( ! dbase->next() )
   {
      failLookup( tr( "the person logged in" ) );
      error = lookup_error;
      return false;
   }

   user_level = qMax( 0, dbase->value( 5 ).toString().toInt() );

   return true;
}

bool US_DataPubImporter::isAdmin( void ) const
{
   return user_level >= 3;
}

/* Whether a buffer or analyte the target already holds may be named by the
   records the import writes.

   An analyte has to be one the person may use: new_solutionAnalyte checks
   verify_analyte_permission, which takes an administrator, or an
   analytePerson row of the person -- the investigator's own analytes,
   which get_analyte_desc lists.  A buffer is checked by no procedure at
   all, but another investigator's private buffer is not the import's to
   hang a solution from: get_buffer_info refuses it to anyone but an
   administrator, and tells an administrator whose it is and whether it is
   private.  Anything else may be used as it is.
*/
bool US_DataPubImporter::sharedUsable( US_DataPub::EntityType type,
                                       const QString& guid, const QString& id,
                                       QString& error )
{
   error.clear();

   if ( opts.target != US_DataPub::TargetDb  ||  dbase == nullptr )
      return true;

   if ( type != US_DataPub::Analyte  &&  type != US_DataPub::Buffer )
      return true;

   QString key = US_DataPub::typeKey( type ) + ":" + guid.toLower();

   if ( shared_ok.contains( key ) )  return shared_ok.value( key );

   bool usable = false;

   if ( type == US_DataPub::Analyte )
   {
      if ( isAdmin() )
         usable = true;

      else
      {
         if ( ! readOwned( US_DataPub::Analyte ) )
         {
            error = lookup_error;
            return false;
         }

         usable = owned[ int( US_DataPub::Analyte ) ].ids.contains( id );
      }
   }

   else
   {
      QStringList query;
      query << "get_buffer_info" << id;
      dbase->query( query );

      int status = dbase->lastErrno();

      if ( status != IUS_DB2::NOTPERMITTED  &&
           lookupFailed( tr( "buffer %1" ).arg( guid ) ) )
      {
         error = lookup_error;
         return false;
      }

      // Refused: someone else's private buffer.  No row: a buffer without
      // an owner, which no one can say is the investigator's to use.
      if ( status == IUS_DB2::OK  &&  dbase->next() )
      {
         QString owner = dbase->value( 7 ).toString();
         QString priv  = dbase->value( 8 ).toString();

         usable = ( owner == QString::number( US_Settings::us_inv_ID() ) )
                  ||  priv == "0"  ||  priv.toLower() == "false";
      }
   }

   shared_ok.insert( key, usable );

   return usable;
}

QString US_DataPubImporter::targetGuidOf( const US_DataPubEntity& entity ) const
{
   return fresh_guids.value( entity.guid, entity.guid );
}

/* The edit a noise record is filed under (new_noise takes its ID).

   A noise record names its model; the model was fitted to an edit.  When
   the bundle carries the model, the edit is the one the model's payload
   names: one of the bundle (written by this import, so still pending in
   the preflight) or one the target holds.  When it does not, the model is
   the investigator's, and the edit the one their model listing gives.  A
   noise record that comes to no edit is refused: new_noise would be given
   edit 0, which the server does not have.
*/
bool US_DataPubImporter::noiseEditOf( const US_DataPubEntity& entity,
                                      const QString& workPath,
                                      QString& editID, bool& pending,
                                      QString& error )
{
   editID .clear();
   pending = false;

   US_Noise noise;

   if ( noise.load( workPath ) != IUS_DB2::OK )
   {
      error = tr( "The payload of %1 could not be read" ).arg( entity.label() );
      return false;
   }

   // The bundle's model of that GUID, under its own GUID or the target's
   US_DataPubEntity          model;
   bool                      inBundle = false;
   QList< US_DataPubEntity > models   = mani.section( US_DataPub::Model );

   for ( int ii = 0; ii < models.size()  &&  ! inBundle; ii++ )
   {
      if ( models[ ii ].guid != noise.modelGUID  &&
           mappedGuid( models[ ii ].guid ) != noise.modelGUID )
         continue;

      inBundle = true;
      model    = models[ ii ];
   }

   if ( inBundle )
   {
      US_Model fitted;

      if ( fitted.load( payloadPath( model ) ) != IUS_DB2::OK )
      {
         error = tr( "The model of %1 could not be read" ).arg( entity.label() );
         return false;
      }

      US_DataPubEntity edit;

      if ( mani.entity( US_DataPub::EditedData, fitted.editGUID, edit ) )
      {
         // The edit this import writes: known once it is written
         editID  = guidMap.contains( fitted.editGUID )
                   ? mappedId( fitted.editGUID ) : QString();
         pending = ( editID.toInt() < 1 );

         if ( pending )  editID.clear();

         return true;
      }

      QString ename;

      if ( ! null_guid( fitted.editGUID )  &&
           findByGuid( US_DataPub::EditedData, mappedGuid( fitted.editGUID ),
                       editID, ename ) )
         return true;
   }

   else
   {
      if ( ! readOwned( US_DataPub::Model ) )
      {
         error = lookup_error;
         return false;
      }

      editID = owned[ int( US_DataPub::Model ) ].edits
               .value( noise.modelGUID.toLower() );
   }

   if ( ! lookup_error.isEmpty() )
   {
      error = lookup_error;
      return false;
   }

   if ( editID.toInt() > 0 )  return true;

   editID.clear();
   error = tr( "%1 cannot be filed under an edit: its model %2 names none"
               " that the bundle carries or the target holds, and a noise"
               " record of the database belongs to an edit" )
           .arg( entity.label() ).arg( noise.modelGUID );

   return false;
}

/* The rules of the server that only its writes would enforce, checked
   before anything is written -- so that a dry run that succeeds is an
   import that would, and an import that cannot succeed fails before its
   transaction opens:
   - add_rotor_calibration takes an administrator: a calibration of a rotor
     the target has, which the target does not hold already, is left out
     for anyone else (with a warning, so --strict-hardware refuses it);
   - create_buffer_component takes level 2: a buffer component the target
     lacks cannot be added below it;
   - the raw data and solution writers always name channel 1 (US_ConvertIO
     and new_solution), and the server refuses a row that names a channel
     it does not have;
   - a noise record has to come to an edit (noiseEditOf()).
   The analytes and buffers the records name are checked as they are found
   (sharedUsable()).
*/
bool US_DataPubImporter::checkServerRules( QString& error )
{
   if ( opts.target != US_DataPub::TargetDb  ||  dbase == nullptr )
      return true;

   // ---- rotor calibrations: administrators only --------------------------
   QList< US_DataPubEntity > cals = mani.section( US_DataPub::RotorCalibration );

   for ( int ii = 0; ii < cals.size(); ii++ )
   {
      const US_DataPubEntity& entity = cals[ ii ];
      QString                 id;
      QString                 name;

      if ( findByGuid( US_DataPub::RotorCalibration, entity.guid, id, name ) )
         continue;                                  // Reused, not created

      US_Rotor::RotorCalibration calibration;
      int                        rotorID = 0;

      US_DataPubRecords::readCalibration( payloadPath( entity ), calibration );

      if ( ! rotorIdOf( calibration.rotorGUID, rotorID ) )
      {
         error = lookup_error;
         return false;
      }

      if ( rotorID < 1  ||  isAdmin() )  continue;  // Left out, or allowed

      // The same calibration under its name is reused, not created
      QString other;
      QString otherID;

      if ( findByName( US_DataPub::RotorCalibration, targetName( entity ),
                       other, otherID )  &&  ! other.isEmpty() )
      {
         QString ours = US_DataPubHash::fingerprint( payloadPath( entity ),
                                                     "Calibration", "guid" );

         if ( ! ours.isEmpty()  &&
              ours == targetFingerprint( US_DataPub::RotorCalibration, other,
                                         otherID ) )
            continue;
      }

      if ( ! lookup_error.isEmpty() )
      {
         error = lookup_error;
         return false;
      }

      if ( ! warn( tr( "Rotor calibration %1 is not imported: only an"
                       " administrator may add a rotor calibration to the"
                       " target database (user level 3; the person logged in"
                       " has level %2)" )
                   .arg( calibration_text( entity.name, entity.guid ) )
                   .arg( user_level ),
                   error ) )
         return false;

      cal_refused << entity.guid;
   }

   // ---- buffer components: level 2 ---------------------------------------
   QList< US_DataPubEntity > buffers = mani.section( US_DataPub::Buffer );
   QStringList               adding;

   for ( int ii = 0; ii < buffers.size(); ii++ )
   {
      const US_DataPubEntity& entity = buffers[ ii ];
      QString                 id;
      QString                 name;

      if ( ! fresh_guids.contains( entity.guid )  &&
           findByGuid( US_DataPub::Buffer, entity.guid, id, name ) )
         continue;                                  // Reused, not created

      const QMap< QString, US_BufferComponent > defs =
            comp_defs.value( entity.guid );
      QList< QString > keys = defs.keys();

      for ( int jj = 0; jj < keys.size(); jj++ )
      {
         bool             have = false;
         QList< QString > ids  = tcomps.keys();

         for ( int kk = 0; kk < ids.size()  &&  ! have; kk++ )
            have = tcomps[ ids[ kk ] ].sameDefinition( defs[ keys[ jj ] ] );

         QString label = QString( "\"%1\"" ).arg( defs[ keys[ jj ] ].name );

         if ( ! have  &&  ! adding.contains( label ) )  adding << label;
      }
   }

   if ( ! adding.isEmpty() )
   {
      if ( user_level < 2 )
      {
         error = tr( "The buffer components %1 are not in the target database"
                     " and would have to be added to its component table,"
                     " which takes user level 2; the person logged in has"
                     " level %2" ).arg( adding.join( ", " ) ).arg( user_level );
         return false;
      }

      // A server whose create_buffer_component gives a new row the ID of
      // the row count plus one fails when that ID is taken
      int         count = tcomp_ids.size();
      QStringList taken;

      for ( int jj = 1; jj <= adding.size(); jj++ )
         if ( tcomp_ids.contains( QString::number( count + jj ) ) )
            taken << QString::number( count + jj );

      if ( ! taken.isEmpty()  &&  componentIdsMayBeCounted()  &&
           ! warn( tr( "The target's component table has %1 rows but"
                       " already uses the IDs %2: unless its"
                       " create_buffer_component has been updated, the"
                       " server gives a new component the ID of the row"
                       " count plus one, and fails there, which fails the"
                       " import (it is then rolled back)" )
                   .arg( count ).arg( taken.join( ", " ) ), error ) )
         return false;
   }

   // ---- the channel the raw data and solution rows name ------------------
   if ( ! mani.section( US_DataPub::RawData ).isEmpty()  ||
        ! mani.section( US_DataPub::Solution ).isEmpty() )
   {
      int status = dbase->statusQuery(
            QString( "SELECT IF( COUNT(*) > 0, 0, %1 ) FROM channel"
                     " WHERE channelID = 1" ).arg( int( IUS_DB2::NOROWS ) ) );

      if ( status == IUS_DB2::NOROWS )
      {
         error = tr( "The target database has no channel 1, which every raw"
                     " data and solution row the import writes names"
                     " (US_ConvertIO and new_solution record channel 1"
                     " only); the server would refuse the first of them.  Add"
                     " the row to the channel table first (as us3.sql's"
                     " initial data does)" );
         return false;
      }

      if ( status != IUS_DB2::OK )
         note( tr( "Whether the target database has channel 1, which the raw"
                   " data and solution rows name, could not be checked (the"
                   " table cannot be read: %1)" ).arg( dbase->lastError() ) );
   }

   // ---- an edit for every noise record -----------------------------------
   QList< US_DataPubEntity > noises = mani.section( US_DataPub::Noise );

   for ( int ii = 0; ii < noises.size(); ii++ )
   {
      QString editID;
      bool    pending = false;

      if ( noiseEditOf( noises[ ii ], payloadPath( noises[ ii ] ), editID,
                        pending, error ) )
         continue;

      if ( error.isEmpty() )  error = lookup_error;

      return false;
   }

   return true;
}

/* Whether create_buffer_component may count rows to pick a new ID.

   A server says which procedures it runs in two ways a client can read.
   The updated procedures come with a version marker: "SELECT
   us3_catalog_version()" answers 1 or more there, and fails elsewhere (no
   such function, 1305, or no right to execute it, 1370), which is version
   0.  And information_schema shows a routine's definition to its definer
   and an administrator (anyone else reads NULL).  The procedure servers
   have run for years inserts with COUNT(*) + 1; the updated one counts
   nothing.  Asked once per import.
*/
bool US_DataPubImporter::componentIdsMayBeCounted( void )
{
   if ( dbase == nullptr )  return true;

   if ( catalog_version < 0 )
   {
      dbase->rawQuery( "SELECT us3_catalog_version()" );

      catalog_version = ( dbase->lastErrno() == IUS_DB2::OK  &&
                          dbase->next() )
                        ? qMax( 0, dbase->value( 0 ).toInt() ) : 0;
   }

   if ( catalog_version >= 1 )  return false;

   dbase->rawQuery(
      "SELECT ROUTINE_DEFINITION FROM information_schema.ROUTINES"
      " WHERE ROUTINE_SCHEMA = DATABASE()"
      " AND ROUTINE_NAME = 'create_buffer_component'" );

   QString definition = dbase->next() ? dbase->value( 0 ).toString()
                                      : QString();

   if ( definition.trimmed().isEmpty() )  return true;    // Cannot tell

   return definition.contains( "COUNT(", Qt::CaseInsensitive );
}

/* A buffer component the target lacks, added to its table, through
   US_BufferComponent::saveToDB(): the library's one way of creating a
   component, which finds the new row by its definition rather than by the
   ID create_buffer_component answers with (on the procedure servers have
   run for years, the connection's previous insert ID).  The row joins the
   target's table as the import knows it, so that a second buffer naming
   the same component is pointed at it.
*/
bool US_DataPubImporter::createComponentDb( const US_BufferComponent& want,
                                            QString& newID, QString& error )
{
   newID.clear();

   US_BufferComponent comp = want;
   QString            why;
   int                id   = comp.saveToDB( dbase, &why );

   if ( id < 1 )
   {
      error = tr( "Buffer component \"%1\" could not be added to the"
                  " database: %2" ).arg( want.name ).arg( why );
      return false;
   }

   newID = QString::number( id );
   tcomp_ids << newID;
   tcomps.insert( newID, comp );

   return true;
}

/* A project no record of the bundle stands for: a run of a bundle without a
   project goes into the project its XML names, which the import creates
   when the target does not have it.  It is reported with the records. */
void US_DataPubImporter::recordImplicitProject( const QString& guid,
                                                const QString& desc,
                                                const QString& id,
                                                const QString& run )
{
   Result result;
   result.type       = US_DataPub::Project;
   result.guid       = guid;
   result.targetGUID = guid;
   result.name       = desc;
   result.targetName = desc;
   result.targetID   = id;
   result.resolution = US_DataPub::ResolvedCreated;
   result.detail     = tr( "not in the bundle; made from what run %1 names" )
                       .arg( run );

   reslist << result;
}

QStringList US_DataPubImporter::solutionsNamed( const QByteArray& xml )
{
   QStringList      guids;
   QXmlStreamReader reader( xml );

   while ( ! reader.atEnd() )
   {
      reader.readNext();

      if ( ! reader.isStartElement()  ||
           reader.name().toString() != "solution" )
         continue;

      QString guid = reader.attributes().value( "guid" ).toString();

      if ( ! guid.isEmpty()  &&  ! guids.contains( guid ) )  guids << guid;
   }

   return guids;
}

/* The labs, rotors and calibrations of the target database, read once.

   They are read as US_Rotor reads them, but without the copy
   US_Rotor::readLabsDB() writes into the local disk store, and every
   listing that fails is a failed lookup rather than an empty one.  The
   hardware decisions of every run -- and the name and GUID lookups of
   rotor calibrations -- are made from them, so none of these listings is
   read inside the import's transaction.
*/
bool US_DataPubImporter::readHardwareDb( void )
{
   if ( hw_read )  return true;

   if ( dbase == nullptr )  return false;

   QVector< US_Rotor::Lab > labs;

   if ( read_labs_db( labs, dbase ) == US_Rotor::MISC_ERROR )
   {
      failLookup( tr( "its labs" ) );
      return false;
   }

   QList< QVector< US_Rotor::Rotor > >                rotors;
   QMap< int, QVector< US_Rotor::RotorCalibration > > cals;

   for ( int ii = 0; ii < labs.size(); ii++ )
   {
      QVector< US_Rotor::Rotor > labRotors;

      if ( US_Rotor::readRotorsFromDB( labRotors, labs[ ii ].ID, dbase )
           == US_Rotor::MISC_ERROR )
      {
         failLookup( tr( "the rotors of lab %1" ).arg( labs[ ii ].ID ) );
         return false;
      }

      for ( int jj = 0; jj < labRotors.size(); jj++ )
      {
         QVector< US_Rotor::RotorCalibration > rotorCals;

         if ( US_Rotor::readCalibrationProfilesDB( rotorCals,
                                                   labRotors[ jj ].ID, dbase )
              == US_Rotor::MISC_ERROR )
         {
            failLookup( tr( "the calibrations of rotor %1" )
                        .arg( labRotors[ jj ].ID ) );
            return false;
         }

         cals.insert( labRotors[ jj ].ID, rotorCals );
      }

      rotors << labRotors;
   }

   hw_labs   = labs;
   hw_rotors = rotors;
   hw_cals   = cals;
   hw_read   = true;

   return true;
}

bool US_DataPubImporter::rotorIdOf( const QString& rotorGUID, int& rotorID )
{
   rotorID = 0;

   if ( rotorGUID.isEmpty() )  return true;

   if ( rotor_ids.contains( rotorGUID ) )
   {
      rotorID = rotor_ids.value( rotorGUID );
      return true;
   }

   QStringList query;
   query << "get_rotorID_from_GUID" << rotorGUID;
   dbase->query( query );

   if ( lookupFailed( tr( "rotor %1" ).arg( rotorGUID ) ) )  return false;

   if ( dbase->next() )  rotorID = dbase->value( 0 ).toString().toInt();

   rotor_ids.insert( rotorGUID, rotorID );

   return true;
}

bool US_DataPubImporter::personIdOf( const QString& personGUID, int& personID )
{
   personID = 0;

   if ( personGUID.isEmpty() )  return true;

   if ( person_ids.contains( personGUID ) )
   {
      personID = person_ids.value( personGUID );
      return true;
   }

   QStringList query;
   query << "get_personID_from_GUID" << personGUID;
   dbase->query( query );

   if ( lookupFailed( tr( "operator %1" ).arg( personGUID ) ) )  return false;

   if ( dbase->next() )  personID = dbase->value( 0 ).toString().toInt();

   person_ids.insert( personGUID, personID );

   return true;
}

/* Every target lookup a database import can make before it writes.

   What is asked here is kept, so that the transaction finds the answers
   waiting and runs no lookup it could have run before:
   - every record of the bundle by its GUID, and whether a record the
     target holds of a type others own is the investigator's;
   - the investigator's listings of every record type of the bundle (the
     name indexes), and the GUIDs and fingerprints of the records whose name
     a bundle record has;
   - the buffer component table, when a buffer names components;
   - the labs, rotors, calibrations and centerpieces, and the rotors and
     operators the runs and calibrations name;
   - the project a run names when the bundle carries none, and the
     solutions its triples name that the bundle does not carry.
   Only what depends on the import's own writes is left to the
   transaction: the records it has just written, the names of model and
   noise records of a run it renamed, and a last check that each new run ID
   is still free.
*/
bool US_DataPubImporter::lookUpInAdvance( QString& error )
{
   if ( opts.target != US_DataPub::TargetDb  ||  dbase == nullptr )
      return true;

   QList< US_DataPubEntity > entities = mani.all();
   bool                      hardware = false;
   bool                      comps    = false;

   for ( int ii = 0; ii < entities.size(); ii++ )
   {
      const US_DataPubEntity& entity = entities[ ii ];
      US_DataPub::EntityType  type   = entity.type;
      QString                 id;
      QString                 name;

      if ( type == US_DataPub::Experiment  ||  type == US_DataPub::Centerpiece
           ||  type == US_DataPub::RotorCalibration )
         hardware = true;

      if ( type == US_DataPub::Buffer  &&
           ! comp_defs.value( entity.guid ).isEmpty() )
         comps = true;

      // ---- the record by its GUID ---------------------------------------
      bool found = false;

      if ( type != US_DataPub::TimeState  &&  type != US_DataPub::Experiment )
         found = findByGuid( type, entity.guid, id, name );

      if ( ! lookup_error.isEmpty() )
      {
         error = lookup_error;
         return false;
      }

      // A buffer or analyte the target holds for someone else, where the
      // investigator may not use it, is not reused: the import makes a copy
      // of the bundle's record under a GUID of its own
      if ( found  &&  ( type == US_DataPub::Buffer  ||
                        type == US_DataPub::Analyte ) )
      {
         if ( ! sharedUsable( type, entity.guid, id, error ) )
         {
            if ( ! error.isEmpty() )  return false;

            fresh_guids.insert( entity.guid, US_Util::new_guid() );
            note( tr( "%1 is in the target database as %2, which"
                      " investigator %3 may not use; the import makes a copy"
                      " of it under the GUID %4" )
                  .arg( entity.label() )
                  .arg( type == US_DataPub::Analyte
                        ? tr( "another investigator's analyte" )
                        : tr( "another investigator's private buffer" ) )
                  .arg( US_Settings::us_inv_ID() )
                  .arg( fresh_guids.value( entity.guid ) ) );
         }
      }

      // ---- the record of its name, and what that record holds -----------
      if ( indexedByName( type ) )
      {
         QString other;
         QString otherID;
         QString tname = targetName( entity );

         if ( ! tname.isEmpty()  &&  findByName( type, tname, other, otherID )
              &&  ! other.isEmpty()  &&  other != entity.guid )
            targetFingerprint( type, other, otherID );

         if ( ! lookup_error.isEmpty() )
         {
            error = lookup_error;
            return false;
         }
      }

      // ---- what a run names ---------------------------------------------
      if ( type == US_DataPub::Experiment )
      {
         QByteArray       data;
         bool             wellFormed = false;
         int              knownID    = 0;
         US_DataPubEntity named;

         if ( ! read_capped( payloadPath( entity ), data, error ) )
            return false;

         if ( ! rotorIdOf ( element_value( data, "rotor", "guid",
                                           wellFormed ), knownID )  ||
              ! personIdOf( element_value( data, "operator", "guid",
                                           wellFormed ), knownID ) )
         {
            error = lookup_error;
            return false;
         }

         // A run of a bundle without a project goes into the project it
         // names, which has to be the investigator's
         QString projGUID = element_value( data, "project", "guid",
                                           wellFormed );

         if ( tproj_guid.isEmpty()  &&
              entity.depend( US_DataPub::Project ).isEmpty()  &&
              ! projGUID.isEmpty()  &&
              ! mani.entity( US_DataPub::Project, projGUID, named ) )
         {
            if ( findByGuid( US_DataPub::Project, projGUID, id, name )  &&
                 ! checkOwner( US_DataPub::Project, projGUID, id, error ) )
            {
               error = tr( "Run %1: %2" ).arg( entity.name ).arg( error );
               return false;
            }

            if ( guid_foreign.contains( US_DataPub::typeKey(
                                        US_DataPub::Project ) + ":"
                                        + projGUID ) )
            {
               error = tr( "Run %1: %2" ).arg( entity.name )
                       .arg( foreignText( US_DataPub::Project, projGUID ) );
               return false;
            }
         }

         // A run whose ID the target uses for another run is renamed: the
         // names a rename tries are asked about now, and kept
         QPair< QString, QString > taken = run_lookup.value( entity.name );

         if ( ! exp_by_guid.contains( entity.guid )  &&
              ! taken.first.isEmpty()  &&
              taken.second.toLower() != entity.guid.toLower() )
            uniqueName( US_DataPub::Experiment, entity.name );

         if ( ! lookup_error.isEmpty() )
         {
            error = lookup_error;
            return false;
         }

         // Its triples name their solutions, which have to be the bundle's
         // or the investigator's
         QStringList sols = solutionsNamed( data );

         for ( int jj = 0; jj < sols.size(); jj++ )
         {
            if ( availableAs( US_DataPub::Solution, sols[ jj ], error ) )
               continue;

            error = tr( "Run %1: %2" ).arg( entity.name ).arg( error );
            return false;
         }
      }

      if ( type == US_DataPub::RotorCalibration )
      {
         US_Rotor::RotorCalibration calibration;
         int                        knownID = 0;

         US_DataPubRecords::readCalibration( payloadPath( entity ),
                                             calibration );

         if ( ! rotorIdOf( calibration.rotorGUID, knownID )  ||
              ! rotorIdOf( entity.attrs.value( "rotorGUID" ), knownID ) )
         {
            error = lookup_error;
            return false;
         }
      }
   }

   if ( ! checkReuse( error ) )  return false;

   // The listings a noise record's edit and a run's hardware come from
   if ( ! mani.section( US_DataPub::Noise ).isEmpty()  &&
        ! readOwned( US_DataPub::Model ) )
   {
      error = lookup_error;
      return false;
   }

   QList< US_AbstractCenterpiece > centerpieces;

   if ( hardware  &&  ( ! readHardwareDb()  ||
                        ! readCenterpieces( centerpieces ) ) )
   {
      error = lookup_error.isEmpty()
              ? tr( "The target's hardware could not be read" )
              : lookup_error;
      return false;
   }

   if ( comps  &&  ! targetComponents( error ) )  return false;

   if ( ! lookup_error.isEmpty() )
   {
      error = lookup_error;
      return false;
   }

   return true;
}

/* The extinction spectrum of a buffer, analyte or solution of the bundle.

   A bundle of this version carries the spectra of a record in a payload of
   their own (spectraPayload), at full precision: the library writers round
   a buffer's wavelengths to 0.1 nm and its values to five digits, an
   analyte's values to six.  That payload is read when it is there; a record
   without spectra has none.  An older bundle has only the record's own XML,
   whose <extinction> elements are read instead -- never US_Analyte's
   reader, which adds the extinction at 280 nm it computes from a protein's
   sequence: a protein without a spectrum would gain a profile of that one
   point, and a measured spectrum a point it never had.  A solution's own
   profile travels only in the spectra payload.
*/
bool US_DataPubImporter::payloadSpectrum( const US_DataPubEntity& entity,
                                          const QString& workPath,
                                          QMap< double, double >& spectrum,
                                          QString& error ) const
{
   spectrum.clear();

   QString rel = entity.attrs.value( "spectraPayload" );

   if ( ! rel.isEmpty() )
   {
      QString                     path;
      QString                     why;
      US_DataPubRecords::Spectra  spectra;

      if ( ! insideBundle( rel, path, error ) )  return false;

      if ( ! US_DataPubRecords::readSpectra( path, spectra, &why ) )
      {
         error = tr( "The spectra of %1 cannot be read: %2" )
                 .arg( entity.label() ).arg( why );
         return false;
      }

      spectrum = spectra.value( "extinction" );
      return true;
   }

   QString owner;                  // The record's own element
   QString lambda;                 // The attribute holding the wavelength

   if ( entity.type == US_DataPub::Analyte )
   {
      owner  = "analyte";
      lambda = "frequency";
   }

   else if ( entity.type == US_DataPub::Buffer )
   {
      owner  = "buffer";
      lambda = "wavelength";
   }

   else
      return true;

   QByteArray data;

   if ( ! read_capped( workPath, data, error ) )  return false;

   QXmlStreamReader reader( data );
   int              depth  = 0;     // Inside the record's own element
   bool             inside = false;
   bool             done   = false;
   bool             protein = false;

   while ( ! reader.atEnd()  &&  ! done )
   {
      reader.readNext();

      if ( reader.isStartElement() )
      {
         QString ename = reader.name().toString();

         if ( ! inside  &&  ename == owner )
         {
            inside  = true;
            depth   = 0;
            protein = ( reader.attributes().value( "type" ).toString()
                        .toUpper() == "PROTEIN" );
            continue;
         }

         if ( inside )  depth++;

         if ( inside  &&  ename == "extinction" )
         {
            QXmlStreamAttributes attrs = reader.attributes();
            bool                 okl   = false;
            bool                 okv   = false;
            double               wl    = attrs.value( lambda ).toString()
                                         .toDouble( &okl );
            double               value = attrs.value( "value" ).toString()
                                         .toDouble( &okv );

            // A value that is not finite is no measurement, and "nan" as a
            // key would not even be found again in the map
            if ( ! okl  ||  ! okv  ||  ! qIsFinite( wl )  ||
                 ! qIsFinite( value ) )
            {
               error = tr( "%1 holds an extinction value that is not a"
                           " finite number" ).arg( entity.label() );
               return false;
            }

            spectrum.insert( wl, value );
         }
      }

      else if ( reader.isEndElement()  &&  inside )
      {
         if ( depth == 0 )  done = true;
         else               depth--;
      }
   }

   if ( reader.hasError() )
   {
      error = tr( "The payload of %1 is not well-formed XML: %2" )
              .arg( entity.label() ).arg( reader.errorString() );
      return false;
   }

   // A bundle written before the spectra had a payload of their own stored
   // a protein's analyte as US_Analyte wrote it: with the extinction at 280
   // nm that the reader computes from the sequence, whether or not anything
   // was measured.  A lone point at 280 nm is that computed value (a
   // measured spectrum has more than one point) and is not imported as if
   // it were measured.  A measured 280 nm value of a spectrum of more
   // points cannot be told apart from the computed one, and is kept.
   if ( protein  &&  spectrum.size() == 1  &&  spectrum.contains( 280.0 ) )
      spectrum.clear();

   return true;
}

// ------------------------------------- values the target can store at all

namespace
{
   // How much of Unicode a MySQL character set holds: 4 all of it, 3 the
   // basic multilingual plane, 2 latin1 (cp1252, as MySQL has it), 1 ASCII,
   // 0 a set this does not know (only ASCII is taken as safe); -1 binary,
   // which holds bytes and has no say
   int charset_rank( const QString& charset )
   {
      QString cs = charset.trimmed().toLower();

      if ( cs.isEmpty()  ||  cs == "binary" )                   return -1;
      if ( cs.startsWith( "utf8mb4" )  ||  cs.startsWith( "utf16" )  ||
           cs.startsWith( "utf32" ) )                            return 4;
      if ( cs == "utf8"  ||  cs == "utf8mb3"  ||  cs == "ucs2" )  return 3;
      if ( cs == "latin1" )                                      return 2;
      if ( cs == "ascii" )                                       return 1;

      return 0;
   }

   // The characters MySQL's latin1 (Windows-1252, with the five codes it
   // leaves undefined passed through) holds beyond U+00A0..U+00FF
   bool cp1252_holds( uint code )
   {
      static const uint extra[] =
      {
         0x20AC, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6,
         0x2030, 0x0160, 0x2039, 0x0152, 0x017D, 0x2018, 0x2019, 0x201C,
         0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A,
         0x0153, 0x017E, 0x0178, 0x0081, 0x008D, 0x008F, 0x0090, 0x009D
      };

      if ( code < 0x80  ||  ( code >= 0xA0  &&  code <= 0xFF ) )  return true;

      for ( uint ii = 0; ii < sizeof( extra ) / sizeof( uint ); ii++ )
         if ( extra[ ii ] == code )  return true;

      return false;
   }

   // A character as a message shows it: itself and its code point
   QString char_text( const QString& ch )
   {
      QVector< uint > codes = ch.toUcs4();
      uint            code  = codes.isEmpty() ? 0 : codes[ 0 ];

      return QString( "\"%1\" (U+%2)" ).arg( ch )
             .arg( QString::number( code, 16 ).toUpper()
                   .rightJustified( 4, QChar( '0' ) ) );
   }
}

QString US_DataPubImporter::unstorableCharacter( const QString& charset,
                                                 const QString& text )
{
   int rank = charset_rank( charset );

   if ( rank < 0  ||  rank >= 4 )  return QString();

   QVector< uint > codes = text.toUcs4();

   for ( int ii = 0; ii < codes.size(); ii++ )
   {
      uint code = codes[ ii ];
      bool fits = ( rank == 3 ) ? ( code <= 0xFFFF  &&
                                    ! ( code >= 0xD800  &&  code <= 0xDFFF ) )
                : ( rank == 2 ) ? cp1252_holds( code )
                                : ( code < 0x80 );

      if ( ! fits )
      {
         char32_t one = char32_t( code );
         return QString::fromUcs4( &one, 1 );
      }
   }

   return QString();
}

/* The character sets the target stores a record type's text in.

   A procedure's text parameters have the character set the schema had
   when the procedure was made (latin1 for us3.sql's), whatever the
   connection's is, and the columns have their own; text that either of
   them cannot hold is refused in strict mode ("Incorrect string value",
   1366) -- in the middle of the import, unless the preflight finds it.
   So the narrowest set among the parameters of the procedures and the
   columns of the tables a record type is written with is what its text
   has to fit.  When information_schema says nothing (a server that does
   not show it), the schema's default stands in, which is what parameters
   take; when that cannot be read either, latin1, the default of us3.sql.
*/
namespace
{
   struct TypeStore
   {
      US_DataPub::EntityType type;
      const char*            procs;
      const char*            tables;
   };

   const TypeStore type_stores[] =
   {
      { US_DataPub::Project,          "new_project new_project2 update_project"
                                      " update_project2",
                                      "project" },
      { US_DataPub::Experiment,       "new_experiment new_speedstep new_project",
                                      "experiment speedstep project" },
      { US_DataPub::RawData,          "new_rawData",
                                      "rawData" },
      { US_DataPub::RotorCalibration, "add_rotor_calibration",
                                      "rotorCalibration" },
      { US_DataPub::Centerpiece,      "",
                                      "abstractCenterpiece" },
      { US_DataPub::Buffer,           "new_buffer update_buffer"
                                      " add_buffer_component"
                                      " create_buffer_component new_spectrum",
                                      "buffer bufferComponent spectrum"
                                      " extinctionProfile" },
      { US_DataPub::Analyte,          "new_analyte update_analyte"
                                      " set_nucleotide_info new_spectrum",
                                      "analyte spectrum extinctionProfile" },
      { US_DataPub::Solution,         "new_solution update_solution"
                                      " new_solutionBuffer new_solutionAnalyte"
                                      " new_experiment_solution",
                                      "solution" },
      { US_DataPub::EditedData,       "new_editedData update_editedData",
                                      "editedData" },
      { US_DataPub::Model,            "new_model update_model",
                                      "model" },
      { US_DataPub::Noise,            "new_noise update_noise",
                                      "noise" },
      { US_DataPub::TimeState,        "new_timestate update_timestate",
                                      "timestate" }
   };

   const int ntype_stores = int( sizeof( type_stores ) / sizeof( TypeStore ) );
}

bool US_DataPubImporter::readTargetCharsets( void )
{
   target_charsets.clear();
   type_charsets  .clear();

   if ( dbase == nullptr )  return false;

   QStringList procs;
   QStringList tables;

   for ( int ii = 0; ii < ntype_stores; ii++ )
   {
      procs  << QString( type_stores[ ii ].procs  ).split( ' ',
                                                    Qt::SkipEmptyParts );
      tables << QString( type_stores[ ii ].tables ).split( ' ',
                                                    Qt::SkipEmptyParts );
   }

   procs .removeDuplicates();
   tables.removeDuplicates();

   // Only names of the lists above: nothing of the bundle goes in here
   QString inProcs  = "'" + procs .join( "','" ) + "'";
   QString inTables = "'" + tables.join( "','" ) + "'";

   dbase->rawQuery(
      "SELECT 'p', LOWER( SPECIFIC_NAME ), CHARACTER_SET_NAME"
      " FROM information_schema.PARAMETERS"
      " WHERE SPECIFIC_SCHEMA = DATABASE()"
      " AND CHARACTER_SET_NAME IS NOT NULL"
      " AND SPECIFIC_NAME IN ( " + inProcs + " )"
      " GROUP BY SPECIFIC_NAME, CHARACTER_SET_NAME"
      " UNION ALL"
      " SELECT 't', LOWER( TABLE_NAME ), CHARACTER_SET_NAME"
      " FROM information_schema.COLUMNS"
      " WHERE TABLE_SCHEMA = DATABASE()"
      " AND CHARACTER_SET_NAME IS NOT NULL"
      " AND TABLE_NAME IN ( " + inTables + " )"
      " GROUP BY TABLE_NAME, CHARACTER_SET_NAME" );

   while ( dbase->next() )
   {
      QString key = dbase->value( 0 ).toString() + ":"
                    + dbase->value( 1 ).toString();
      QString cs  = dbase->value( 2 ).toString();
      QString had = target_charsets.value( key );

      // The narrowest of a procedure's parameters, of a table's columns
      if ( had.isEmpty()  ||  charset_rank( cs ) < charset_rank( had ) )
         target_charsets.insert( key, cs );
   }

   // For a type information_schema says nothing of: the schema's default
   dbase->rawQuery( "SELECT @@character_set_database" );

   QString dbcs    = dbase->next() ? dbase->value( 0 ).toString() : QString();
   default_charset = dbcs.isEmpty() ? QString( "latin1" ) : dbcs;

   for ( int ii = 0; ii < ntype_stores; ii++ )
   {
      QStringList names;
      QString     narrowest;

      for ( const QString& name : QString( type_stores[ ii ].procs ).split( ' ',
                                                    Qt::SkipEmptyParts ) )
         names << "p:" + name.toLower();

      for ( const QString& name : QString( type_stores[ ii ].tables ).split(
                                                    ' ', Qt::SkipEmptyParts ) )
         names << "t:" + name.toLower();

      for ( int jj = 0; jj < names.size(); jj++ )
      {
         QString cs = target_charsets.value( names[ jj ] );

         if ( cs.isEmpty()  ||  charset_rank( cs ) < 0 )  continue;

         if ( narrowest.isEmpty()  ||
              charset_rank( cs ) < charset_rank( narrowest ) )
            narrowest = cs;
      }

      if ( narrowest.isEmpty() )  narrowest = default_charset;

      type_charsets.insert( int( type_stores[ ii ].type ), narrowest );
   }

   return true;
}

/* The values of a record the target has to be able to take: no number of
   it is NaN or infinite (the server would store what such text becomes,
   or refuse it, and a disk store would hold "nan"), and, in a database,
   no text of it holds a character the columns and procedure parameters it
   goes through cannot store.  The record's XML is read whole: attributes
   and text are what the library writers pass on, apart from an edit's and
   a time state's XML, which go into a BLOB as they are.
*/
bool US_DataPubImporter::checkValues( const US_DataPubEntity& entity,
                                      const QString& file,
                                      const QByteArray& data,
                                      QString& error )
{
   bool    toDb    = ( opts.target == US_DataPub::TargetDb );
   QString charset = toDb ? type_charsets.value( int( entity.type ) )
                          : QString();
   bool    blob    = ( entity.type == US_DataPub::EditedData  ||
                       entity.type == US_DataPub::TimeState   ||
                       entity.type == US_DataPub::RawData );
   QString fname   = QFileInfo( file ).fileName();

   auto unstorable = [ & ]( const QString& text, const QString& field )
   {
      if ( charset.isEmpty()  ||  blob )  return false;

      QString ch = unstorableCharacter( charset, text );

      if ( ch.isEmpty() )  return false;

      error = tr( "%1: %2 holds the character %3, which the target database"
                  " cannot store -- the text of this record goes into"
                  " columns and procedure parameters of character set %4."
                  "  The import is refused before anything is written" )
              .arg( entity.label() ).arg( field ).arg( char_text( ch ) )
              .arg( charset );
      return true;
   };

   if ( data.isNull() )
   {  // The manifest's own fields of the record
      QStringList keys = entity.attrs.keys();

      if ( unstorable( entity.name, tr( "its name in the manifest" ) ) )
         return false;

      for ( int ii = 0; ii < keys.size(); ii++ )
      {
         if ( US_DataPub::isPayloadKey( keys[ ii ] )  ||
              keys[ ii ].toLower().contains( "sha256" ) )
            continue;

         QString value = entity.attrs.value( keys[ ii ] );
         QString field = tr( "the manifest field \"%1\"" ).arg( keys[ ii ] );

         if ( ! US_DataPub::isTextAttribute( QString(), keys[ ii ] )  &&
              US_DataPub::isNonFiniteNumber( value ) )
         {
            error = tr( "%1: %2 is \"%3\", which is not a finite number;"
                        " the bundle is refused" )
                    .arg( entity.label() ).arg( field ).arg( value );
            return false;
         }

         if ( unstorable( value, field ) )  return false;
      }

      return true;
   }

   // The numbers: the check the export makes of what it writes
   QString nonFinite = US_DataPub::nonFiniteNumberInXml( data );

   if ( ! nonFinite.isEmpty() )
   {
      error = tr( "%1: %2 in %3 is not a finite number; the bundle is"
                  " refused" ).arg( entity.label() ).arg( nonFinite )
              .arg( fname );
      return false;
   }

   QXmlStreamReader xml( data );

   while ( ! xml.atEnd() )
   {
      xml.readNext();

      if ( xml.isStartElement() )
      {
         QString                elem  = xml.name().toString();
         QXmlStreamAttributes   attrs = xml.attributes();

         for ( int ii = 0; ii < attrs.size(); ii++ )
         {
            QString name  = attrs[ ii ].name().toString();
            QString value = attrs[ ii ].value().toString();
            QString field = tr( "the attribute \"%1\" of <%2> in %3" )
                            .arg( name ).arg( elem ).arg( fname );

            if ( unstorable( value, field ) )  return false;
         }

         elements << elem;
      }

      else if ( xml.isEndElement() )
      {
         if ( ! elements.isEmpty() )  elements.removeLast();
      }

      else if ( xml.isCharacters()  &&  ! xml.isWhitespace() )
      {
         QString field = tr( "the text of <%1> in %2" )
                         .arg( elements.isEmpty() ? QString()
                                                  : elements.last() )
                         .arg( fname );

         if ( unstorable( xml.text().toString(), field ) )  return false;
      }
   }

   elements.clear();

   return true;
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

   // A GUID ends up in procedure arguments, file contents and lookups;
   // every one a payload names -- the main one and every extra XML one --
   // has to be a real GUID
   QStringList pkeys = entity.payloadKeys();

   for ( int jj = 0; jj < pkeys.size(); jj++ )
   {
      QString xpath = path;

      if ( pkeys[ jj ] != "payload"  &&
           ! insideBundle( entity.attrs.value( pkeys[ jj ] ), xpath, error ) )
         return false;

      if ( ! US_DataPubHash::isXmlFile( xpath )  ||  ! QFile::exists( xpath ) )
         continue;

      QByteArray data;

      if ( ! read_capped( xpath, data, error ) )  return false;

      QString bad = US_DataPub::unusableGuidInXml( data );

      if ( ! bad.isEmpty() )
      {
         error = tr( "The payload %1 of %2 names \"%3\", which is not a"
                     " GUID; the bundle is refused" )
                 .arg( QFileInfo( xpath ).fileName() ).arg( label ).arg( bad );
         return false;
      }

      // Values the target cannot take, found before anything is written
      if ( ! checkValues( entity, xpath, data, error ) )  return false;
   }

   // ... and the manifest's own fields of the record
   if ( ! checkValues( entity, path, QByteArray(), error ) )  return false;

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

   // The spectra a record carries beside it have to be readable too -- and
   // those an older bundle carries inside a buffer's or analyte's XML
   if ( ! entity.attrs.value( "spectraPayload" ).isEmpty()  ||
        entity.type == US_DataPub::Buffer  ||
        entity.type == US_DataPub::Analyte )
   {
      QMap< double, double > spectrum;

      if ( ! payloadSpectrum( entity, path, spectrum, error ) )  return false;
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

   if ( ! QDir().mkpath( workDir )  ||  ! QFileInfo( workDir ).isDir() )
   {
      error = tr( "Cannot create the work directory %1 for %2" )
              .arg( workDir ).arg( entity.label() );
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

   if ( QFile::exists( target )  &&  ! QFile::remove( target ) )
   {
      error = tr( "Cannot replace the work copy %1 of %2" )
              .arg( target ).arg( entity.label() );
      return QString();
   }

   QFile payload( source );

   if ( ! payload.copy( target ) )
   {
      error = tr( "Cannot copy the payload of %1 to %2: %3" )
              .arg( entity.label() ).arg( target ).arg( payload.errorString() );
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
               note( tr( "The rotor of calibration %1 is not in the target"
                         " disk store; the calibration names no rotor ID" )
                     .arg( calibration_text( entity.name, entity.guid ) ) );
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

   QStringList ids;
   QString     why;

   if ( ! US_BufferComponent::listIDs( dbase, ids, &why ) )
   {
      if ( lookup_error.isEmpty() )
         lookup_error = tr( "The target database could not be asked about its"
                            " buffer components: %1" ).arg( why );

      error = lookup_error;
      return false;
   }

   // Every row, parsed or not: the row a creation adds is the one that is
   // not among these
   for ( int ii = 0; ii < ids.size(); ii++ )
      tcomp_ids << ids[ ii ];

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
         have = tcomps[ ids[ jj ] ].sameDefinition( want );

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
         if ( ! tcomps[ ids[ ii ] ].sameDefinition( want ) )  continue;

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
      // transaction, and found again by what it is
      QString newID;

      if ( ! createComponentDb( want, newID, failure ) )  return;

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

   // A record a real import wrote is in the target now, under its GUID
   if ( ! opts.dryRun  &&  opts.target == US_DataPub::TargetDb  &&
        ( resolution == US_DataPub::ResolvedCreated  ||
          resolution == US_DataPub::ResolvedRenamed ) )
   {
      QString key = US_DataPub::typeKey( entity.type ) + ":" + targetGUID;
      guid_missing.remove( key );
      guid_found.insert( key, qMakePair( targetID, targetName ) );

      // ... and it is the investigator's own (a listing not read yet will
      // hold it when it is read)
      if ( owned.contains( int( entity.type ) ) )
      {
         owned[ int( entity.type ) ].ids   << targetID;
         owned[ int( entity.type ) ].guids << targetGUID.toLower();
      }
   }

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
                                          QString& path, bool& identical,
                                          QString& error )
{
   path     .clear();
   error    .clear();
   identical = false;

   QString runID = runIdFor( entity );
   QString dir   = diskResultDir( runID );

   if ( dir.isEmpty() )  return false;

   path = dir + "/" + renamedFile( entity );

   if ( ! QFile::exists( path ) )  return false;

   // Compared with what the import would write, which is the bundle's
   // payload rewritten for the target.  A payload that cannot be put in
   // those terms is a failure of its own, not a difference.
   QString work = workCopy( entity, error );

   if ( work.isEmpty()  ||  ! rewriteWork( entity, work, error ) )
   {
      if ( error.isEmpty() )
         error = tr( "%1 could not be compared with %2" )
                 .arg( entity.label() ).arg( path );
      return true;
   }

   QString ours   = US_DataPubHash::fileHash( work );
   QString theirs = US_DataPubHash::fileHash( path );

   if ( ours.isEmpty()  ||  theirs.isEmpty() )
   {
      error = tr( "%1 could not be read to be compared" )
              .arg( ours.isEmpty() ? work : path );
      return true;
   }

   identical = ( ours == theirs );

   // A time state is the pair of files: the one already there is the same
   // only when its field definitions are there too and are the bundle's
   if ( identical  &&  entity.type == US_DataPub::TimeState )
   {
      QString defs = entity.attrs.value( "definitionsPayload" );
      QString source;
      QString target = US_DataPub::timeStateDefs( path );

      if ( defs.isEmpty()  ||  ! insideBundle( defs, source, error ) )
      {
         if ( error.isEmpty() )
            error = tr( "%1 carries no field definitions" )
                    .arg( entity.label() );
         return true;
      }

      if ( target.isEmpty()  ||  ! QFile::exists( target ) )
         identical = false;

      else
      {
         QString sdefs = US_DataPubHash::fileHash( source );
         QString tdefs = US_DataPubHash::fileHash( target );

         if ( sdefs.isEmpty()  ||  tdefs.isEmpty() )
         {
            error = tr( "The field definitions %1 could not be read to be"
                        " compared" ).arg( sdefs.isEmpty() ? source : target );
            return true;
         }

         identical = ( sdefs == tdefs );
      }
   }

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
   owned.clear();
   guid_found.clear();
   guid_missing.clear();
   guid_foreign.clear();
   hw_read = false;
   hw_labs.clear();
   hw_rotors.clear();
   hw_cals.clear();
   cp_read = false;
   cp_list.clear();
   rotor_ids.clear();
   person_ids.clear();
   created_cals.clear();
   print_cache.clear();
   raw_checked.clear();
   tcomp_ids.clear();
   user_level = -1;
   catalog_version = -1;
   fresh_guids.clear();
   cal_refused.clear();
   shared_ok.clear();
   implicit_projs.clear();
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
   // A file of a run directory goes by its file name, which follows a
   // renamed run
   if ( entity.type == US_DataPub::RawData     ||
        entity.type == US_DataPub::EditedData  ||
        entity.type == US_DataPub::TimeState )
      return renamedFile( entity );

   // A database keeps a buffer's description without the "  [M]" marker of
   // a manually entered buffer (US_Buffer::saveToDB() strips it, and
   // readFromDB() takes it off again), so the name is compared, renamed and
   // written without it
   if ( entity.type == US_DataPub::Buffer  &&
        opts.target == US_DataPub::TargetDb )
   {
      int manx = entity.name.indexOf( "  [M]" );

      return ( manx > 0 ) ? entity.name.left( manx ).simplified()
                          : entity.name;
   }

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

      if ( diskFilePresent( entity, present, identical, error ) )
      {
         if ( ! error.isEmpty() )  return false;

         if ( ! identical )
         {
            error = ( type == US_DataPub::TimeState )
                    ? tr( "%1 already exists with different content, or"
                          " without the field definitions of this bundle;"
                          " the import will not overwrite it" ).arg( present )
                    : tr( "%1 already exists with different content;"
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
   // (A buffer or analyte the investigator may not use is copied instead,
   // under a GUID of its own: the preflight decided which.)
   QString existingID;
   QString existingName;
   QString tguid = targetGuidOf( entity );

   if ( ! fresh_guids.contains( entity.guid )  &&
        findByGuid( type, entity.guid, existingID, existingName ) )
   {
      record( entity, US_DataPub::ResolvedReused, entity.guid,
              existingName.isEmpty() ? targetName( entity ) : existingName,
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
                            : ourFingerprint( entity, workPath, error );

      if ( ! workPath.isEmpty()  &&  ourPrint.isEmpty()  &&  ! error.isEmpty() )
         return false;

      QString theirPrint  = targetFingerprint( type, otherGUID, otherID );

      if ( ! lookup_error.isEmpty() )
      {
         error = lookup_error;
         return false;
      }

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

      if ( name == origName  ||  nameInUse( type, name ) )
         name = uniqueName( type, origName );

      if ( ! lookup_error.isEmpty() )
      {
         error = lookup_error;
         return false;
      }

      if ( name.isEmpty() )
      {
         error = tr( "No free name was found for %1 \"%2\"" )
                 .arg( US_DataPub::typeText( type ) ).arg( origName );
         return false;
      }

      if ( opts.dryRun )
      {
         bool omitted = false;

         if ( ! checkDryDb( entity, workPath, name, omitted, error ) )
            return false;

         if ( omitted )  return true;

         nameTaken( type, name, tguid, QString( "-1" ) );
         record( entity, US_DataPub::ResolvedRenamed, tguid, name,
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

      nameTaken( type, name, tguid, newID );
      record( entity, US_DataPub::ResolvedRenamed, tguid, name, newID,
              details );
      return true;
   }

   // ---- a plain new record ------------------------------------------------
   if ( opts.dryRun )
   {
      bool omitted = false;

      if ( ! checkDryDb( entity, workPath, name, omitted, error ) )
         return false;

      if ( omitted )  return true;

      nameTaken( type, name, tguid, QString( "-1" ) );
      record( entity, US_DataPub::ResolvedCreated, tguid, name,
              QString( "-1" ), copyNote( entity ) );
      return true;
   }

   // A file of a run directory carries its name in no payload
   bool runFile = ( type == US_DataPub::RawData     ||
                    type == US_DataPub::EditedData  ||
                    type == US_DataPub::TimeState );

   if ( name != entity.name  &&  ! workPath.isEmpty()  &&  ! runFile  &&
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

   nameTaken( type, name, tguid, newID );
   record( entity, US_DataPub::ResolvedCreated, tguid, name, newID,
           copyNote( entity ) );

   return true;
}

QString US_DataPubImporter::copyNote( const US_DataPubEntity& entity ) const
{
   if ( ! fresh_guids.contains( entity.guid ) )  return QString();

   return tr( "a copy under a GUID of its own: the target's record of the"
              " bundle's GUID is another investigator's" );
}

/* The fingerprint of the record the import would write, in the terms the
   target's copy is written out in (targetFingerprint()).  An analyte is
   compared as the values it is stored with: its sequence without the
   framing US_Analyte's old writer put around it (unframedSequence()), and
   the extinction values the import stores (payloadSpectrum()) -- the
   target's copy holds its stored profile,
   where a payload written from a disk store also holds the 280 nm value
   US_Analyte computes from a protein's sequence.
*/
QString US_DataPubImporter::ourFingerprint( const US_DataPubEntity& entity,
                                            const QString& workPath,
                                            QString& error )
{
   error.clear();

   QString ownElem;
   QString ownAttr;
   own_identity( entity.type, ownElem, ownAttr );

   if ( entity.type != US_DataPub::Analyte )
      return US_DataPubHash::fingerprint( workPath, ownElem, ownAttr );

   US_Analyte             analyte;
   QMap< double, double > spectrum;

   if ( analyte.readFromFile( workPath ) != IUS_DB2::OK  ||
        ! payloadSpectrum( entity, workPath, spectrum, error ) )
   {
      if ( error.isEmpty() )
         error = tr( "The payload of %1 could not be read" )
                 .arg( entity.label() );
      return QString();
   }

   analyte.extinction = spectrum;

   QString print = analyteFingerprint( analyte, "ours_analyte.xml" );

   if ( print.isEmpty() )
      error = tr( "%1 could not be written out to be compared" )
              .arg( entity.label() );

   return print;
}

/* The fingerprint of an analyte as it is stored, whichever way it was read.

   The sequence loses the framing US_Analyte's old writer added (only that:
   it is stored as the bundle holds it, and an analyte stored by an older
   UltraScan holds the framing its file gave it), and a
   protein's extinction loses the value at 280 nm that US_Analyte's file
   reader computes from the sequence, when that is the value there: a copy
   of the analyte read from a file holds it, one read from a database or
   from a bundle's spectra does not, and it is not a property of the
   record.  A measured 280 nm value that differs from the computed one is
   kept.  Both sides of a comparison go through here.
*/
QString US_DataPubImporter::analyteFingerprint( US_Analyte& analyte,
                                                const QString& fileName )
{
   analyte.sequence = unframedSequence( analyte.sequence );

   if ( analyte.type == US_Analyte::PROTEIN  &&
        analyte.extinction.contains( 280.0 ) )
   {
      // The value the file reader computes: the analyte without a spectrum,
      // through a file and back
      US_Analyte probe = analyte;
      probe.extinction.clear();
      QString    probePath = bundle.workPath() + "/target/probe_analyte.xml";
      QDir().mkpath( bundle.workPath() + "/target" );
      QFile::remove( probePath );

      US_Analyte computedFrom;
      double     stored   = analyte.extinction.value( 280.0 );
      double     computed = qQNaN();

      if ( probe.write( false, probePath ) == IUS_DB2::OK  &&
           computedFrom.readFromFile( probePath ) == IUS_DB2::OK  &&
           computedFrom.extinction.contains( 280.0 ) )
         computed = computedFrom.extinction.value( 280.0 );

      if ( qAbs( stored - computed ) <=
           1.0e-9 * qMax( 1.0, qMax( qAbs( stored ), qAbs( computed ) ) ) )
         analyte.extinction.remove( 280.0 );
   }

   QString workDir = bundle.workPath() + "/target";
   QDir().mkpath( workDir );
   QString path = workDir + "/" + fileName;
   QFile::remove( path );

   if ( analyte.write( false, path ) != IUS_DB2::OK )  return QString();

   return US_DataPubHash::fingerprint( path, "analyte", "analyteGUID" );
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
   // neither part has a dot.  An edit is "<runID>.<editID>.<runType>...xml",
   // so the count of parts tells it apart -- for a usable run ID, which has
   // no dots.  So does the run type, which is what the bundle's run types
   // are held to: one to eight letters and digits.  That leaves out the
   // other three-part files of a run directory, a time state's definitions
   // "<runID>.time_state.xml" and an RI run's "<runID>.RIProfile.xml" --
   // which sorts before the XML of a run of type "WA", say.
   QString experiment_xml( const QDir& dir, const QString& runID )
   {
      if ( ! US_DataPubImporter::runIdIsUsable( runID ) )  return QString();

      QStringList xmls = dir.entryList( QStringList( runID + ".*.xml" ),
                                        QDir::Files, QDir::Name );

      for ( int ii = 0; ii < xmls.size(); ii++ )
      {
         QStringList parts = xmls[ ii ].split( '.' );

         if ( parts.size() != 3  ||  parts[ 0 ] != runID )  continue;
         if ( ! US_DataPub::runTypeIsUsable( parts[ 1 ] ) )  continue;

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

   if ( ! readHardwareDb() )  return false;

   for ( int ii = 0; ii < hw_rotors.size(); ii++ )
      for ( int jj = 0; jj < hw_rotors[ ii ].size(); jj++ )
         all << hw_cals.value( hw_rotors[ ii ][ jj ].ID );

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

   // Read once, before the transaction
   if ( cp_read )
   {
      centerpieces = cp_list;
      return true;
   }

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

   cp_list = centerpieces;
   cp_read = true;

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

      // Asked once per import: the answer is kept, and kept up to date as
      // the import creates records (record())
      QString key = US_DataPub::typeKey( type ) + ":" + guid;

      if ( guid_found.contains( key ) )
      {
         id   = guid_found.value( key ).first;
         name = guid_found.value( key ).second;
         return true;
      }

      if ( guid_missing.contains( key )  ||  guid_foreign.contains( key ) )
         return false;

      dbase->query( query );

      // A lookup that checks the permission of the person logged in
      // (project, solution, raw data) refuses someone else's record: it is
      // there, but not one this import may use
      if ( dbase->lastErrno() == IUS_DB2::NOTPERMITTED )
      {
         guid_foreign << key;
         return false;
      }

      if ( lookupFailed( tr( "%1 %2" ).arg( US_DataPub::typeText( type ) )
                         .arg( guid ) ) )
         return false;

      if ( dbase->next() )  id = dbase->value( 0 ).toString();

      if ( id.isEmpty() )
      {
         guid_missing << key;
         return false;
      }

      guid_found.insert( key, qMakePair( id, name ) );

      return true;
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

bool US_DataPubImporter::nameInUse( US_DataPub::EntityType type,
                                    const QString& name )
{
   QString guid;
   QString id;

   if ( name.isEmpty() )  return false;

   if ( ! indexedByName( type ) )
      return findOneByName( type, name, guid, id );

   return namesHeld( type ).contains( name );
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

      if ( dbase->next() )
      {
         id   = dbase->value( 1 ).toString();
         guid = dbase->value( 2 ).toString();
      }

      // Kept, so that the candidates a rename tried before the transaction
      // are not asked about again inside it
      run_lookup.insert( name, qMakePair( id, guid ) );

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

   // Read once: the preflight reads the records a conflict compares with
   QString key = US_DataPub::typeKey( type ) + ":" + guid;

   if ( print_cache.contains( key ) )  return print_cache.value( key );

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

         // An analyte in the terms ourFingerprint() puts the bundle's in
         if ( type == US_DataPub::Analyte )
         {
            US_Analyte analyte;

            if ( analyte.readFromFile( fpath ) != IUS_DB2::OK )
            {
               if ( lookup_error.isEmpty() )
                  lookup_error = tr( "The target's analyte %1 could not be"
                                     " read to be compared" ).arg( fpath );
               return QString();
            }

            return analyteFingerprint( analyte, "their_analyte.xml" );
         }

         return US_DataPubHash::fingerprint( fpath, ownElem, ownAttr );
      }

      return QString();
   }

   if ( dbase == nullptr )  return QString();

   // The target's record, written out the way the bundle's payloads are, to
   // be compared in the same terms.  A record that cannot be read, or was
   // read but cannot be written out, cannot be compared at all, which is a
   // failure, not a difference: taken for one, it would have the import
   // make a renamed duplicate of a record the target may well hold already
   // (US_Buffer::writeToDisk(), for one, refuses a buffer whose component
   // has no concentration).
   bool written = false;
   bool read    = false;

   switch ( type )
   {
      case US_DataPub::Project:
      {
         US_Project project;

         read    = ( project.readFromDB( id.toInt(), dbase ) == IUS_DB2::OK );
         written = read  &&  project.saveToFile( path );
         break;
      }

      case US_DataPub::Buffer:
      {
         US_Buffer buffer;

         read    = buffer.readFromDB( dbase, id );
         written = read  &&  buffer.writeToDisk( path );
         break;
      }

      case US_DataPub::Analyte:
      {
         US_Analyte analyte;

         read = ( analyte.load( true, guid, dbase ) == IUS_DB2::OK );

         if ( ! read )  break;

         // In the terms ourFingerprint() puts the bundle's analyte in
         QString print = analyteFingerprint( analyte, "their_analyte.xml" );

         if ( print.isEmpty() )  break;

         print_cache.insert( key, print );
         return print;
      }

      case US_DataPub::Solution:
      {
         US_Solution solution;

         read    = ( solution.readFromDB( id.toInt(), dbase ) == IUS_DB2::OK );
         written = read  &&  solution.saveToFile( path );
         break;
      }

      case US_DataPub::Model:
      {
         US_Model model;

         read    = ( model.load( id, dbase ) == IUS_DB2::OK );
         written = read  &&  ( model.write( path ) == IUS_DB2::OK );
         break;
      }

      case US_DataPub::Noise:
      {
         US_Noise noise;

         read    = ( noise.load( id, dbase ) == IUS_DB2::OK );
         written = read  &&  ( noise.write( path ) == IUS_DB2::OK );
         break;
      }

      case US_DataPub::RotorCalibration:
      {
         US_Rotor::RotorCalibration calibration;

         read    = ( calibration.readDB( id.toInt(), dbase )
                     == US_Rotor::ROTOR_OK );
         written = read  &&
                   US_DataPubRecords::writeCalibration( calibration, path );
         break;
      }

      default:
         // An experiment of the same run ID and another GUID is another run,
         // whatever it holds, so it is never "the same"
         return QString();
   }

   if ( ! read )
   {
      if ( lookup_error.isEmpty() )
         lookup_error = tr( "The target's %1 \"%2\" (ID %3), whose name a"
                            " record of the bundle has, could not be read to"
                            " be compared with it (status %4): %5" )
                        .arg( US_DataPub::typeText( type ) ).arg( guid )
                        .arg( id ).arg( dbase->lastErrno() )
                        .arg( dbase->lastError() );
      return QString();
   }

   if ( ! written )
   {
      if ( lookup_error.isEmpty() )
         lookup_error = tr( "The target's %1 \"%2\" (ID %3) could not be"
                            " written out to be compared with the bundle's" )
                        .arg( US_DataPub::typeText( type ) ).arg( guid )
                        .arg( id );
      return QString();
   }

   QString print = US_DataPubHash::fingerprint( path, ownElem, ownAttr );
   print_cache.insert( key, print );

   return print;
}

QString US_DataPubImporter::uniqueName( US_DataPub::EntityType type,
                                        const QString& name )
{
   QString suffix = opts.renameSuffix.trimmed();

   if ( suffix.isEmpty() )  suffix = tr( "imported" );

   QString base = name.isEmpty() ? US_DataPub::typeText( type ) : name;

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

      // Only whether the name is taken matters, not by which record
      if ( ! nameInUse( type, candidate ) )
         return lookup_error.isEmpty() ? candidate : QString();

      if ( ! lookup_error.isEmpty() )  return QString();
   }

   // A thousand names taken: a random tail, which is checked like the rest
   for ( int ii = 0; ii < 100; ii++ )
   {
      QString last      = US_Util::new_guid().left( 8 );
      QString candidate = ( type == US_DataPub::Experiment )
                          ? base.left( 70 ) + "-" + last
                          : base + "-" + last;

      if ( type == US_DataPub::Experiment  &&  ! runIdIsUsable( candidate ) )
         continue;

      if ( ! nameInUse( type, candidate ) )
         return lookup_error.isEmpty() ? candidate : QString();

      if ( ! lookup_error.isEmpty() )  return QString();
   }

   return QString();
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

   // A disk store keeps a solution's extinction profile nowhere: the
   // solution file has no place for it, and the database is where
   // US_Solution keeps one
   if ( type == US_DataPub::Solution  &&
        ! entity.attrs.value( "spectraPayload" ).isEmpty() )
   {
      QMap< double, double > spectrum;

      if ( payloadSpectrum( entity, workPath, spectrum, error )  &&
           ! spectrum.isEmpty() )
         note( tr( "The extinction profile of solution \"%1\" is not kept:"
                   " a disk store has no place for it" ).arg( name ) );

      error.clear();
   }

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

   if ( type == US_DataPub::RotorCalibration )
   {
      // A disk store finds a calibration by its ID (US_Rotor reads the
      // file whose "id" is the one a run's XML names), so the calibration
      // gets one that no calibration of the store has, and the runs of
      // the bundle name it: without one the run would name -1 and lose its
      // calibration
      US_Rotor::RotorCalibration calibration;

      if ( ! US_DataPubRecords::readCalibration( workPath, calibration ) )
      {
         error = tr( "The rotor calibration payload could not be read" );
         return false;
      }

      int         top  = 0;
      QStringList held = QDir( dir ).entryList(
                         QStringList( "C???????.xml" ), QDir::Files );

      for ( int ii = 0; ii < held.size(); ii++ )
         top = qMax( top, first_attributes( dir + "/" + held[ ii ],
                                            "Calibration" )
                          .value( "id" ).toInt() );

      calibration.ID = top + 1;

      if ( ! US_DataPubRecords::writeCalibration( calibration, path ) )
      {
         error = tr( "Cannot write %1" ).arg( path );
         return false;
      }

      newID = QString::number( calibration.ID );
   }

   else if ( ! QFile::copy( workPath, path ) )
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
   // Everything below is decided from what the preflight read
   if ( ! readHardwareDb() )
   {
      error = lookup_error.isEmpty() ? tr( "the labs could not be read" )
                                     : lookup_error;
      return false;
   }

   // ---- the rotor calibration, and through it the rotor -------------------
   QString calGUID = entity.depend( US_DataPub::RotorCalibration );
   int     calID   = guidMap.contains( calGUID ) ? mappedId( calGUID ).toInt()
                                                 : 0;
   int     rotorID = 0;

   // A dry run has not written the calibration it imports: the run is
   // going to name it, under the rotor its GUID names
   bool    calPending = opts.dryRun  &&  guidMap.contains( calGUID )  &&
                        mappedId( calGUID ) == QString( "-1" );

   if ( calID > 0 )
   {
      if ( created_cals.contains( calID ) )
         rotorID = created_cals.value( calID );

      for ( int ii = 0; ii < hw_rotors.size()  &&  rotorID < 1; ii++ )
      {
         for ( int jj = 0; jj < hw_rotors[ ii ].size(); jj++ )
         {
            const QVector< US_Rotor::RotorCalibration > cals =
                  hw_cals.value( hw_rotors[ ii ][ jj ].ID );

            for ( int kk = 0; kk < cals.size(); kk++ )
               if ( cals[ kk ].ID == calID )  rotorID = hw_rotors[ ii ][ jj ].ID;
         }
      }
   }

   if ( rotorID < 1 )
   {
      US_DataPubEntity calEntity;
      QString          rotorGUID;

      if ( mani.entity( US_DataPub::RotorCalibration, calGUID, calEntity ) )
         rotorGUID = calEntity.attrs.value( "rotorGUID" );

      if ( rotorGUID.isEmpty() )  rotorGUID = exper.rotorGUID;

      if ( ! rotorIdOf( rotorGUID, rotorID ) )
      {
         error = lookup_error;
         return false;
      }
   }

   if ( rotorID < 1 )
   {  // Fall back to the first rotor the target knows about
      for ( int ii = 0; ii < hw_labs.size()  &&  rotorID < 1; ii++ )
      {
         const QVector< US_Rotor::Rotor >& rotors = hw_rotors[ ii ];

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

   if ( calID < 1  &&  ! calPending )
   {  // Take the rotor's first calibration profile
      QVector< US_Rotor::RotorCalibration > cals = hw_cals.value( rotorID );

      if ( cals.isEmpty() )
      {
         error = tr( "Run %1 cannot be imported: the target's rotor has no"
                     " calibration profile" ).arg( exper.runID );
         return false;
      }

      calID = cals[ 0 ].ID;

      if ( ! warn( tr( "The rotor calibration of run %1 is not in the target"
                       " database; calibration %2 is recorded instead" )
                   .arg( exper.runID )
                   .arg( calibration_text( cals[ 0 ].label, cals[ 0 ].GUID ) ),
                   error ) )
         return false;
   }

   exper.calibrationID = calID;

   // ---- the lab that owns the rotor --------------------------------------
   int labIndex = -1;

   for ( int ii = 0; ii < hw_labs.size()  &&  labIndex < 0; ii++ )
   {
      const QVector< US_Rotor::Rotor >& rotors = hw_rotors[ ii ];

      for ( int jj = 0; jj < rotors.size(); jj++ )
      {
         if ( rotors[ jj ].ID != rotorID )  continue;

         labIndex = ii;
         break;
      }
   }

   if ( labIndex < 0 )
   {
      error = tr( "Run %1 cannot be imported: no lab of the target owns its"
                  " rotor" ).arg( exper.runID );
      return false;
   }

   // The lab as US_Rotor::Lab::readDB() read it, with its instruments
   const US_Rotor::Lab& lab = hw_labs[ labIndex ];
   exper.labID = lab.ID;

   // ---- the instrument, by its serial number in that lab ------------------
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

   if ( ! personIdOf( exper.operatorGUID, operID ) )
   {
      error = lookup_error;
      return false;
   }

   // The operator has to hold a permit for the instrument: new_experiment
   // refuses anyone else (verify_operator_permission).  One the target does
   // not have, or one without a permit, is stood in for by the current
   // investigator when they hold one, or else by an operator who does.
   const US_Rotor::Instrument* instrument = nullptr;

   for ( int ii = 0; ii < lab.instruments.size(); ii++ )
      if ( lab.instruments[ ii ].ID == instID )
         instrument = &lab.instruments[ ii ];

   auto permitted = [ instrument ]( int personID )
   {
      if ( instrument == nullptr  ||  personID < 1 )  return false;

      for ( int ii = 0; ii < instrument->operators.size(); ii++ )
         if ( instrument->operators[ ii ].ID == personID )  return true;

      return false;
   };

   if ( ! permitted( operID ) )
   {
      int     invID = US_Settings::us_inv_ID();
      int     stand = 0;
      QString standName;

      if ( permitted( invID ) )
      {
         stand     = invID;
         standName = tr( "the current investigator" );
      }

      else if ( instrument != nullptr  &&  ! instrument->operators.isEmpty() )
      {
         stand     = instrument->operators[ 0 ].ID;
         standName = tr( "operator %1, %2" )
                     .arg( instrument->operators[ 0 ].lname )
                     .arg( instrument->operators[ 0 ].fname );
      }

      if ( stand < 1 )
      {
         error = tr( "Run %1 cannot be imported: no one holds a permit to"
                     " operate instrument \"%2\" of the target database,"
                     " and an experiment's operator has to" )
                 .arg( exper.runID ).arg( wantSerial );
         return false;
      }

      QString why = ( operID < 1 )
                    ? tr( "is not in the target database" )
                    : tr( "holds no permit for instrument \"%1\" in the target"
                          " database" ).arg( wantSerial );

      if ( ! warn( tr( "The operator of run %1 %2; %3 is recorded instead" )
                   .arg( exper.runID ).arg( why ).arg( standName ), error ) )
         return false;

      operID = stand;
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

/* A run, staged and decided, but not written.

   The run is staged the way US_ConvertIO reads one -- the experiment XML
   with the run's .auc files beside it, under the run ID it gets in the
   target -- so the triples and the speed profile come from the same reader
   the rest of UltraScan uses.  The staged run is kept for the raw data,
   which US_ConvertIO::writeRawDataToDB() writes once the solutions are in.
   Every triple is checked against the bundle, the RI profile is read, and
   the project and the hardware are resolved -- all of it in the staging
   directory and from what the preflight read, so that a dry run decides
   what a real import would.
*/
bool US_DataPubImporter::prepareRunDb( const US_DataPubEntity& entity,
                                       const QString& name,
                                       QByteArray& RIxml, int& projID,
                                       QString& error )
{
   projID = 0;
   RIxml.clear();

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
   QString projGUID    = entity.depend( US_DataPub::Project );
   bool    pending     = false;     // A dry run's project, not written
   bool    createLater = false;     // Created from what the run names

   if ( ! tproj_guid.isEmpty() )
      projID = tproj_id.toInt();

   else if ( ! projGUID.isEmpty()  &&  guidMap.contains( projGUID ) )
   {
      projID = mappedId( projGUID ).toInt();

      // A dry run has not written the project it imports
      pending = opts.dryRun  &&  mappedId( projGUID ) == QString( "-1" );
   }

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
         // Someone else's project is not attached to (the preflight said so
         // already; this is the second line)
         if ( ! checkOwner( US_DataPub::Project, named, pid, error ) )
            return false;

         projID = pid.toInt();
         note( tr( "Run %1 is attached to the project \"%2\" the target"
                   " already has" ).arg( name ).arg( pname ) );
      }

      else if ( ! lookup_error.isEmpty() )
      {
         error = lookup_error;
         return false;
      }

      else if ( guid_foreign.contains( US_DataPub::typeKey(
                                       US_DataPub::Project ) + ":" + named ) )
      {
         error = foreignText( US_DataPub::Project, named );
         return false;
      }

      else if ( opts.dryRun  &&  implicit_projs.contains( named ) )
      {  // A dry run's earlier run would have created it
         projID  = 0;
         pending = true;
      }

      else
      {  // Created by the writer, from what the run names
         projID      = 0;
         createLater = true;
         note( tr( "The bundle carries no project; a project record \"%1\""
                   " %2 from what run %3 names" )
               .arg( exper.project.projectDesc )
               .arg( opts.dryRun ? tr( "would be created" )
                                 : tr( "is created" ) )
               .arg( name ) );

         if ( opts.dryRun )
         {
            implicit_projs << named;
            recordImplicitProject( named, exper.project.projectDesc,
                                   QString( "-1" ), name );
         }
      }
   }

   if ( projID < 1  &&  ! pending  &&  ! createLater )
   {
      error = tr( "The project of run %1 is not in the target database" )
              .arg( name );
      return false;
   }

   // ---- the hardware ------------------------------------------------------
   if ( ! resolveHardware( exper, entity, error ) )  return false;

   return true;
}

/* A run, written as an experiment row with its speed steps.

   prepareRunDb() staged it and decided everything; what is left is the
   project a bundle without one has to have, a last check that the run ID
   is still free -- another session may have taken it since the preflight
   -- and the rows.
*/
bool US_DataPubImporter::createExperimentDb( const US_DataPubEntity& entity,
                                             const QString& /*workPath*/,
                                             const QString& name,
                                             QString& newID, QString& error )
{
   QByteArray RIxml;
   int        projID = 0;

   if ( ! prepareRunDb( entity, name, RIxml, projID, error ) )  return false;

   US_ConvertIO::DiskRun& run   = *staged_runs.value( entity.guid );
   US_Experiment&         exper = run.experiment;

   if ( projID == 0 )
   {
      US_Project project;
      project.projectGUID = exper.project.projectGUID;
      project.projectDesc = exper.project.projectDesc;

      if ( ! createProjectDb( project, error ) )  return false;

      projID = project.projectID;
      recordImplicitProject( project.projectGUID, project.projectDesc,
                             QString::number( projID ), name );

      // The next run that names it finds it
      QString key = US_DataPub::typeKey( US_DataPub::Project ) + ":"
                    + project.projectGUID;
      guid_missing.remove( key );
      guid_found.insert( key, qMakePair( QString::number( projID ),
                                         project.projectDesc ) );

      // ... and it is the investigator's (a listing not read yet will hold
      // it when it is read)
      if ( owned.contains( int( US_DataPub::Project ) ) )
         owned[ int( US_DataPub::Project ) ].ids << QString::number( projID );
   }

   // ---- the run ID, once more, inside the transaction ---------------------
   QStringList check;
   check << "get_experiment_info_by_runID" << name
         << QString::number( US_Settings::us_inv_ID() );
   dbase->query( check );

   if ( lookupFailed( tr( "run %1" ).arg( name ) ) )
   {
      error = lookup_error;
      return false;
   }

   if ( dbase->next() )
   {
      error = tr( "The run ID %1 was taken in the target database while the"
                  " import ran; import the bundle again to have the run"
                  " renamed" ).arg( name );
      return false;
   }

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

   // The solution's own extinction profile, under the component type
   // US_Solution::saveToDB() stores it with
   QMap< double, double > spectrum;

   if ( ! payloadSpectrum( entity, workPath, spectrum, error ) )  return false;

   if ( ! spectrum.isEmpty() )
   {
      QString compType( "Sample" );
      QString valType ( "molarExtinction" );

      if ( US_ExtProfile::create_eprofile( dbase, solID, compType, valType,
                                           spectrum ) < 1 )
      {
         error = tr( "The extinction profile of solution \"%1\" could not be"
                     " written: %2" ).arg( name ).arg( dbase->lastError() );
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
bool US_DataPubImporter::prepareRunRawData( const QString& expGUID,
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
      // US_Solution::saveToDB() and its local disk copy; it has to find it.
      // A dry run's solution of the bundle is not written, but would be.
      bool pending = opts.dryRun  &&  guidMap.contains( oldSol );

      if ( ! pending  &&
           ( newSol.isEmpty()  ||
             ! findByGuid( US_DataPub::Solution, newSol, sid, sname ) ) )
      {
         error = lookup_error.isEmpty()
                 ? tr( "The solution %1 of triple %2 is not in the target" )
                   .arg( oldSol ).arg( triple.tripleDesc )
                 : lookup_error;

         if ( guid_foreign.contains( US_DataPub::typeKey(
                                     US_DataPub::Solution ) + ":" + newSol ) )
            error = foreignText( US_DataPub::Solution, newSol );

         return false;
      }

      // ... and one of the investigator's own when the bundle has not got it
      if ( ! pending  &&  ! guidMap.contains( oldSol )  &&
           ! checkOwner( US_DataPub::Solution, newSol, sid, error ) )
         return false;

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

   return true;
}

bool US_DataPubImporter::rawFieldOfRun( const QString& expGUID,
                                        const QString& key, QString& value )
{
   QList< US_DataPubEntity > raws = mani.section( US_DataPub::RawData );
   QSet< QString >           values;
   bool                      every = true;
   bool                      any   = false;

   for ( int ii = 0; ii < raws.size(); ii++ )
   {
      if ( raws[ ii ].depend( US_DataPub::Experiment ) != expGUID )  continue;

      if ( raws[ ii ].attrs.contains( key ) )
      {
         any = true;
         values << raws[ ii ].attrs.value( key );
      }
      else
         every = false;
   }

   if ( ! any )  return false;

   if ( every  &&  values.size() == 1 )
   {
      value = *values.constBegin();
      return true;
   }

   note( tr( "The raw data records of run %1 do not share one %2; they are"
             " written with the run's" ).arg( mappedName( expGUID ) )
         .arg( key ) );

   return false;
}

bool US_DataPubImporter::writeRunRawData( const QString& expGUID,
                                          QString& error )
{
   if ( ! prepareRunRawData( expGUID, error ) )  return false;

   US_ConvertIO::DiskRun& run = *staged_runs.value( expGUID );

   run.experiment.expID = mappedId( expGUID ).toInt();
   run.experiment.invID = US_Settings::us_inv_ID();

   // writeRawDataToDB() gives every rawData row the experiment's label and
   // comment, as us_convert writes a run.  The source's rows may hold
   // others (the manifest's "label" and "comment" of a raw data record):
   // the one label and comment all of the run's records agree on is
   // written instead.  Rows that differ among themselves cannot each keep
   // theirs through that call, and are written as it writes them.
   QString label    = run.experiment.label;
   QString comments = run.experiment.comments;
   QString rawLabel;
   QString rawComment;

   if ( rawFieldOfRun( expGUID, "label", rawLabel ) )
      run.experiment.label = rawLabel;

   if ( rawFieldOfRun( expGUID, "comment", rawComment ) )
      run.experiment.comments = rawComment;

   QString werror = US_ConvertIO::writeRawDataToDB( run.experiment,
                                                    run.triples,
                                                    run.directory, dbase );

   run.experiment.label    = label;
   run.experiment.comments = comments;

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

/* What a dry run into a database decides about one record.

   The writers decide a few things as they go: whether a calibration's rotor
   is there (or the calibration is left out), the hardware and project of a
   run, whether a solution and a time state have a run of this import,
   whether each triple's solution is there.  A dry run makes the same
   decisions here, in the staging directory and from what the preflight
   read, and stops short of the writes -- so a dry run that succeeds is an
   import that would, short of what only the writes can find (a constraint
   the database enforces, a lost connection, a record someone else wrote in
   the meantime).
*/
bool US_DataPubImporter::checkDryDb( const US_DataPubEntity& entity,
                                     const QString& workPath,
                                     const QString& name, bool& omitted,
                                     QString& error )
{
   omitted = false;

   if ( opts.target != US_DataPub::TargetDb )  return true;

   QString expGUID = entity.depend( US_DataPub::Experiment );

   switch ( entity.type )
   {
      case US_DataPub::RotorCalibration:
      {
         US_Rotor::RotorCalibration calibration;
         int                        rotorID = 0;

         if ( ! US_DataPubRecords::readCalibration( workPath, calibration ) )
         {
            error = tr( "The rotor calibration payload could not be read" );
            return false;
         }

         if ( ! rotorIdOf( calibration.rotorGUID, rotorID ) )
         {
            error = lookup_error;
            return false;
         }

         if ( rotorID > 0  &&  ( isAdmin()  ||
                                 cal_refused.contains( entity.guid ) ) )
         {
            // Refused in the preflight, which said so
            if ( cal_refused.contains( entity.guid ) )
            {
               record( entity, US_DataPub::ResolvedOmitted, QString(), name,
                       QString( "-1" ), tr( "only an administrator may add"
                                            " a rotor calibration" ) );
               omitted = true;
            }

            return true;
         }

         if ( rotorID > 0 )
         {  // Not foreseen in the preflight: the writer refuses it all the same
            if ( ! warn( tr( "Rotor calibration %1 would not be imported:"
                             " only an administrator may add one" )
                         .arg( calibration_text( name, entity.guid ) ),
                         error ) )
               return false;

            record( entity, US_DataPub::ResolvedOmitted, QString(), name,
                    QString( "-1" ), tr( "only an administrator may add a"
                                         " rotor calibration" ) );
            omitted = true;
            return true;
         }

         if ( ! warn( tr( "Rotor %1 is not in the target database, so the"
                          " rotor calibration %2 would not be imported" )
                      .arg( calibration.rotorGUID )
                      .arg( calibration_text( name, entity.guid ) ), error ) )
            return false;

         record( entity, US_DataPub::ResolvedOmitted, QString(), name,
                 QString( "-1" ), tr( "the target does not have its rotor" ) );
         omitted = true;
         return true;
      }

      case US_DataPub::Buffer:
      {
         US_Buffer buffer;
         QMap< double, double > spectrum;

         if ( ! buffer.readFromDisk( workPath )  ||
              ! payloadSpectrum( entity, workPath, spectrum, error ) )
         {
            if ( error.isEmpty() )
               error = tr( "The buffer payload could not be read" );
            return false;
         }

         // A component the target lacks would be added with the buffer
         for ( int ii = 0; ii < buffer.componentIDs.size(); ii++ )
            if ( buffer.componentIDs[ ii ].startsWith( "new:" ) )
               note( tr( "Buffer component %1 of \"%2\" would be added to the"
                         " target's component table" )
                     .arg( buffer.componentIDs[ ii ].mid( 4 ) ).arg( name ) );

         return true;
      }

      case US_DataPub::Analyte:
      {
         US_Analyte analyte;
         QMap< double, double > spectrum;

         if ( analyte.readFromFile( workPath ) != IUS_DB2::OK  ||
              ! payloadSpectrum( entity, workPath, spectrum, error ) )
         {
            if ( error.isEmpty() )
               error = tr( "The analyte payload could not be read" );
            return false;
         }

         return true;
      }

      case US_DataPub::Solution:
      {
         US_Solution solution;

         if ( solution.readFromFile( workPath, false ) != IUS_DB2::OK )
         {
            error = tr( "The solution payload could not be read" );
            return false;
         }

         if ( ! guidMap.contains( expGUID ) )
         {
            error = tr( "The solution \"%1\" belongs to no run of this import" )
                    .arg( name );
            return false;
         }

         if ( ! created_exps.contains( expGUID ) )
         {
            error = tr( "run %1 is already in the target database without the"
                        " solution \"%2\"; the import does not add a solution"
                        " to a run it did not create" )
                    .arg( mappedName( expGUID ) ).arg( name );
            return false;
         }

         return true;
      }

      case US_DataPub::Experiment:
      {
         QByteArray RIxml;
         int        projID = 0;

         return prepareRunDb( entity, name, RIxml, projID, error );
      }

      case US_DataPub::RawData:
      {
         if ( ! created_exps.contains( expGUID ) )
         {
            error = tr( "The experiment of %1 was not imported" )
                    .arg( entity.filename );
            return false;
         }

         // The triples of a run are checked once, as they are written once
         if ( raw_checked.contains( expGUID ) )  return true;

         raw_checked << expGUID;

         return prepareRunRawData( expGUID, error );
      }

      case US_DataPub::TimeState:
      {
         if ( ! created_exps.contains( expGUID ) )
         {
            error = tr( "The experiment of the time state was not imported" );
            return false;
         }

         return stageTimeStateDefs( entity, workPath, error );
      }

      case US_DataPub::EditedData:
      {
         // Its raw data comes with the bundle, or is the investigator's
         QString          rawGUID = entity.depend( US_DataPub::RawData );
         US_DataPubEntity raw;

         if ( mani.entity( US_DataPub::RawData, rawGUID, raw ) )  return true;

         if ( availableAs( US_DataPub::RawData, rawGUID, error ) )  return true;

         error = tr( "%1: %2" ).arg( entity.label() ).arg( error );
         return false;
      }

      case US_DataPub::Model:
      {
         US_Model model;

         if ( model.load( workPath ) != IUS_DB2::OK )
         {
            error = tr( "The model payload could not be read" );
            return false;
         }

         return true;
      }

      case US_DataPub::Noise:
      {
         QString editID;
         bool    pending = false;

         return noiseEditOf( entity, workPath, editID, pending, error );
      }

      default:
         return true;
   }
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

         // Looked up in the preflight
         int rotorID = 0;

         if ( ! rotorIdOf( calibration.rotorGUID, rotorID ) )
         {
            error = lookup_error;
            return false;
         }

         if ( rotorID < 1 )
         {
            if ( ! warn( tr( "Rotor %1 is not in the target database, so the"
                             " rotor calibration %2 is not imported" )
                         .arg( calibration.rotorGUID )
                         .arg( calibration_text( name, entity.guid ) ),
                         error ) )
               return false;

            record( entity, US_DataPub::ResolvedOmitted, QString(), name,
                    QString( "-1" ), tr( "the target does not have its rotor" ) );
            newID = "omitted";
            return true;
         }

         // add_rotor_calibration takes an administrator: the preflight
         // left out what the person may not add (and warned of it)
         if ( ! isAdmin() )
         {
            if ( ! cal_refused.contains( entity.guid )  &&
                 ! warn( tr( "Rotor calibration %1 is not imported: only"
                             " an administrator may add one" )
                         .arg( calibration_text( name, entity.guid ) ),
                         error ) )
               return false;

            record( entity, US_DataPub::ResolvedOmitted, QString(), name,
                    QString( "-1" ), tr( "only an administrator may add a"
                                         " rotor calibration" ) );
            newID = "omitted";
            return true;
         }

         // The calibration experiment is the source's, which the import does
         // not carry: -1 is the procedure's "none" (0 is an experiment ID it
         // looks for, and refuses)
         calibration.ID                      = 0;
         calibration.rotorID                 = rotorID;
         calibration.calibrationExperimentID = -1;
         calibration.label                   = name;

         if ( calibration.saveDB( rotorID, dbase ) != IUS_DB2::OK  ||
              calibration.ID < 1 )
         {
            error = tr( "The rotor calibration %1 could not be written"
                        " to the database: %2" )
                    .arg( calibration_text( name, entity.guid ) )
                    .arg( dbase->lastError() );
            return false;
         }

         newID = QString::number( calibration.ID );
         created_cals.insert( calibration.ID, rotorID );
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
         // order and with its formats: the buffer, its components and its
         // extinction profile.  Since it learnt to join a caller's
         // transaction (and then to leave out its local disk copy) it could
         // almost be used, but not quite: it updates whatever buffer of
         // that GUID the database holds, whoever owns it, where the import
         // only ever creates; and it writes the profile only for a spectrum
         // it was told is new, from the spectrum it read, where the import
         // stores the one the payload holds.
         QString descrip = buffer.description;
         int     manx    = descrip.indexOf( "  [M]" );

         if ( manx > 0 )  descrip = descrip.left( manx ).simplified();

         // Private unless the manifest says the source's buffer was public
         // (its "private" attribute, "0"): a public buffer stays one, so
         // that other investigators can use it as they could at the
         // source.  A buffer of a disk store has no such flag and is
         // private, as US_Buffer::saveToDB() makes a buffer by default.
         QString priv = entity.attrs.value( "private" ).trimmed().toLower();
         bool    pub  = ( priv == "0"  ||  priv == "false" );

         QStringList query;
         query << "new_buffer"
               << targetGuidOf( entity )
               << descrip
               << QString::number( buffer.compressibility, 'e', 4 )
               << QString::number( buffer.pH             , 'f', 4 )
               << QString::number( buffer.density        , 'f', 6 )
               << QString::number( buffer.viscosity      , 'f', 5 )
               << US_Util::bool_string( buffer.manual )
               << QString( pub ? "0" : "1" )               // Private
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

         // The concentrations at full precision, from the component
         // definitions a bundle of this version carries (the buffer file
         // rounds them to five decimals, which turns 5 uM into 0.00001 mM);
         // an older bundle has only the buffer file's.  They are matched by
         // the source's component ID, which the bundle's buffer payload
         // names in the order of the work copy's components -- never by
         // position in the definitions file.
         QList< double > concs = buffer.concentration;
         QString         crel  = entity.attrs.value( "componentsPayload" );

         if ( ! crel.isEmpty()  &&  ! buffer.componentIDs.isEmpty() )
         {
            QString         cpath;
            QString         why;
            QStringList     ids;
            QList< double > full;
            US_Buffer       source;

            if ( ! insideBundle( crel, cpath, error ) )  return false;

            if ( ! source.readFromDisk( payloadPath( entity ) )  ||
                 source.componentIDs.size() != buffer.componentIDs.size() )
            {
               error = tr( "The components of buffer \"%1\" could not be read"
                           " from its payload" ).arg( name );
               return false;
            }

            if ( US_DataPubRecords::readBufferConcentrations( cpath, ids, full,
                                                              &why ) )
            {
               QMap< QString, double > byID;

               for ( int ii = 0; ii < ids.size()  &&  ii < full.size(); ii++ )
                  byID.insert( ids[ ii ], full[ ii ] );

               concs.clear();

               for ( int ii = 0; ii < source.componentIDs.size(); ii++ )
               {
                  if ( ! byID.contains( source.componentIDs[ ii ] ) )
                  {
                     error = tr( "The component definitions of buffer \"%1\""
                                 " give no concentration for its component"
                                 " %2" ).arg( name )
                             .arg( source.componentIDs[ ii ] );
                     return false;
                  }

                  concs << byID.value( source.componentIDs[ ii ] );
               }
            }

            else if ( ! why.isEmpty() )
            {
               error = tr( "The component concentrations of buffer \"%1\""
                           " cannot be read: %2" ).arg( name ).arg( why );
               return false;
            }
         }

         for ( int ii = 0; ii < buffer.componentIDs.size(); ii++ )
         {
            if ( ii >= concs.size() )
            {
               error = tr( "Component %1 of buffer \"%2\" has no"
                           " concentration" ).arg( buffer.componentIDs[ ii ] )
                       .arg( name );
               return false;
            }

            query.clear();
            query << "add_buffer_component" << sBufID
                  << buffer.componentIDs[ ii ]
                  << QString::number( concs[ ii ], 'g', 17 );
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

         // The spectrum the payload holds, and nothing else
         QMap< double, double > spectrum;

         if ( ! payloadSpectrum( entity, workPath, spectrum, error ) )
            return false;

         if ( ! spectrum.isEmpty() )
         {
            QString compType( "Buffer" );
            QString valType ( "molarExtinction" );

            if ( US_ExtProfile::create_eprofile( dbase, bufID, compType,
                                                 valType, spectrum ) < 1 )
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
         // order and with its formats: the analyte, a nucleotide's details
         // and the extinction profile.  Like US_Buffer::saveToDB() it joins
         // a caller's transaction now, and leaves out its disk copy there,
         // but it updates an analyte of that GUID whoever owns it, and
         // stores the spectrum the reader holds -- with the 280 nm value it
         // computed from a protein's sequence.
         QString atype = ( analyte.type == US_Analyte::PROTEIN ) ? "Protein"
                       : ( analyte.type == US_Analyte::DNA     ) ? "DNA"
                       : ( analyte.type == US_Analyte::RNA     ) ? "RNA"
                                                                 : "Other";
         // The extinction values the payload holds: the reader adds the 280
         // nm value it computes from a protein's sequence, which is not part
         // of the record and is never stored as if it were measured
         QMap< double, double > extinction;

         if ( ! payloadSpectrum( entity, workPath, extinction, error ) )
            return false;

         QString spectrum;
         double  lambda1 = 0.0;
         double  coeff1  = 0.0;

         if ( ! extinction.isEmpty() )
         {
            lambda1 = extinction.firstKey();
            coeff1  = extinction.first();
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

         // The source row's own spectrum column, when the manifest carries
         // it (its "spectrum" attribute, empty for an empty column): what
         // the column held is kept, rather than made up from the profile
         if ( entity.attrs.contains( "spectrum" ) )
            spectrum = entity.attrs.value( "spectrum" );

         // The sequence exactly as the payload holds it -- a FASTA header,
         // residue numbers and line breaks are the analyte's own -- which
         // is also what a disk import keeps
         QStringList query;
         query << "new_analyte" << targetGuidOf( entity ) << atype
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

         if ( ! extinction.isEmpty() )
         {
            QString compType( "Analyte" );
            QString valType ( "molarExtinction" );

            if ( US_ExtProfile::create_eprofile( dbase, anaID, compType,
                                                 valType, extinction ) < 1 )
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

         // The source row's comment when the manifest carries it (its
         // "comment" attribute); otherwise none, as us_edit and
         // us_import_run store an edit: the triple is in the file name
         rec.comment = entity.attrs.value( "comment" );

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

         // The variance and meniscus are columns of the model row as well
         // as attributes of its XML, which a model the database stored
         // need not have, and which US_Model writes with six digits.  The
         // manifest carries the source row's values at full precision:
         // they win, and the XML's stand in for a bundle without them.
         // Either is written with all its digits.
         double variance = model.variance;
         double meniscus = model.meniscus;
         bool   ok       = false;
         double value    = entity.attrs.value( "variance" ).toDouble( &ok );

         if ( ok  &&  qIsFinite( value ) )  variance = value;

         value = entity.attrs.value( "meniscus" ).toDouble( &ok );

         if ( ok  &&  qIsFinite( value ) )  meniscus = value;

         QStringList query;
         query << "new_model" << entity.guid << name << QString( contents )
               << QString::number( variance, 'g', 17 )
               << QString::number( meniscus, 'g', 17 )
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
         // Written with the procedure US_Noise::write() uses, but neither
         // with the lookups that call makes inside the transaction (the
         // model's ID, the investigator's whole model listing for its edit,
         // the noise's own ID) nor with its update of a noise record that
         // happens to have this GUID: the import only ever creates.  The
         // noise is stored as the payload's own text, which the import has
         // already pointed at the target's model.
         US_Noise   noise;
         QByteArray contents;

         if ( noise.load( workPath ) != IUS_DB2::OK  ||
              ! read_capped( workPath, contents, error ) )
         {
            error = tr( "The noise payload could not be read" );
            return false;
         }

         // The model, and the edit it was fitted to: the import's own, or
         // the investigator's model the preflight listed
         QString modelID;
         QString editID;
         QString modelName;
         bool    pending = false;

         if ( ! findByGuid( US_DataPub::Model, noise.modelGUID, modelID,
                            modelName ) )
         {
            error = lookup_error.isEmpty()
                    ? tr( "The model %1 of the noise record \"%2\" is not in"
                          " the target" ).arg( noise.modelGUID ).arg( name )
                    : lookup_error;
            return false;
         }

         // The preflight made sure there is one; never edit 0
         if ( ! noiseEditOf( entity, workPath, editID, pending, error ) )
            return false;

         if ( editID.toInt() < 1 )
         {
            error = tr( "The edit of the noise record \"%1\" was not written"
                        " to the target" ).arg( name );
            return false;
         }

         QStringList query;
         query << "new_noise" << noise.noiseGUID << editID << modelID
               << noise.modelGUID
               << QString( ( noise.type == US_Noise::TI ) ? "ti_noise"
                                                          : "ri_noise" )
               << name << QString::fromUtf8( contents );

         int status  = dbase->statusQuery( query );
         int noiseID = ( status == IUS_DB2::OK ) ? dbase->lastInsertID() : 0;

         if ( status != IUS_DB2::OK  ||  noiseID < 1 )
         {
            error = tr( "The noise record \"%1\" could not be written to"
                        " the database (status %2): %3" ).arg( name )
                    .arg( status ).arg( dbase->lastError() );
            return false;
         }

         newID = QString::number( noiseID );
         return true;
      }

      default:
         break;
   }

   error = tr( "%1 cannot be imported into a database" ).arg( entity.label() );

   return false;
}
