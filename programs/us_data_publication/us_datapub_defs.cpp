//! \file us_datapub_defs.cpp
#include "us_datapub_defs.h"

#include <QtCore>

#include "us_convertio.h"

namespace
{
   struct TypeInfo
   {
      US_DataPub::EntityType type;
      const char*            key;
      const char*            text;
      const char*            idKey;
      const char*            guidKey;
      const char*            nameKey;
      const char*            dir;
   };

   // The single place where the manifest vocabulary is defined.  The order
   // is the dependency order of a bundle and must not be rearranged.
   const TypeInfo type_table[] =
   {
      { US_DataPub::Project,          "project",          "Project",
        "projectID",          "projectGUID",          "description", "project"          },
      { US_DataPub::Experiment,       "experiment",       "Experiment",
        "experimentID",       "experimentGUID",       "runID",       "experiment"       },
      { US_DataPub::RawData,          "rawData",          "Raw Data",
        "rawDataID",          "rawDataGUID",          "filename",    "rawData"          },
      { US_DataPub::RotorCalibration, "rotorCalibration", "Rotor Calibration",
        "rotorCalibrationID", "rotorCalibrationGUID", "label",       "rotorCalibration" },
      { US_DataPub::Centerpiece,      "centerpiece",      "Centerpiece",
        "centerpieceID",      "centerpieceGUID",      "name",        "centerpiece"      },
      { US_DataPub::Buffer,           "buffers",          "Buffer",
        "bufferID",           "bufferGUID",           "description", "buffers"          },
      { US_DataPub::Analyte,          "analytes",         "Analyte",
        "analyteID",          "analyteGUID",          "description", "analytes"         },
      { US_DataPub::Solution,         "solutions",        "Solution",
        "solutionID",         "solutionGUID",         "description", "solutions"        },
      { US_DataPub::EditedData,       "edits",            "Edited Data",
        "editedDataID",       "editedDataGUID",       "filename",    "edits"            },
      { US_DataPub::Model,            "models",           "Model",
        "modelID",            "modelGUID",            "description", "models"           },
      { US_DataPub::Noise,            "noise",            "Noise",
        "noiseID",            "noiseGUID",            "description", "noise"            },
      { US_DataPub::TimeState,        "timeState",        "Time State",
        "timeStateID",        "timeStateGUID",        "filename",    "timeState"        }
   };

   const int type_count = int( sizeof( type_table ) / sizeof( type_table[ 0 ] ) );

   const TypeInfo* info_of( US_DataPub::EntityType type )
   {
      for ( int ii = 0; ii < type_count; ii++ )
         if ( type_table[ ii ].type == type )  return &type_table[ ii ];

      return nullptr;
   }

   // Scope levels in dependency order.  TimeState is not a scope of its
   // own:  it travels with the raw data of its run.
   const US_DataPub::EntityType scope_types[] =
   {
      US_DataPub::Project,
      US_DataPub::Experiment,
      US_DataPub::RawData,
      US_DataPub::RotorCalibration,
      US_DataPub::Centerpiece,
      US_DataPub::Buffer,
      US_DataPub::Analyte,
      US_DataPub::Solution,
      US_DataPub::EditedData,
      US_DataPub::Model,
      US_DataPub::Noise
   };

   const int scope_count = int( sizeof( scope_types ) / sizeof( scope_types[ 0 ] ) );
}

QString US_DataPub::typeKey( US_DataPub::EntityType type )
{
   const TypeInfo* ti = info_of( type );
   return ti ? QString( ti->key ) : QString( "unknown" );
}

US_DataPub::EntityType US_DataPub::typeOfKey( const QString& key )
{
   for ( int ii = 0; ii < type_count; ii++ )
      if ( key == QString( type_table[ ii ].key ) )  return type_table[ ii ].type;

   return US_DataPub::UnknownType;
}

QString US_DataPub::typeText( US_DataPub::EntityType type )
{
   const TypeInfo* ti = info_of( type );
   return ti ? QString( ti->text ) : QString( "Unknown" );
}

QString US_DataPub::idKey( US_DataPub::EntityType type )
{
   const TypeInfo* ti = info_of( type );
   return ti ? QString( ti->idKey ) : QString( "id" );
}

QString US_DataPub::guidKey( US_DataPub::EntityType type )
{
   const TypeInfo* ti = info_of( type );
   return ti ? QString( ti->guidKey ) : QString( "guid" );
}

US_DataPub::EntityType US_DataPub::typeOfGuidKey( const QString& key )
{
   for ( int ii = 0; ii < type_count; ii++ )
      if ( key == QString( type_table[ ii ].guidKey ) )  return type_table[ ii ].type;

   return US_DataPub::UnknownType;
}

QString US_DataPub::nameKey( US_DataPub::EntityType type )
{
   const TypeInfo* ti = info_of( type );
   return ti ? QString( ti->nameKey ) : QString( "name" );
}

QString US_DataPub::payloadDir( US_DataPub::EntityType type )
{
   const TypeInfo* ti = info_of( type );
   return ti ? QString( ti->dir ) : QString( "other" );
}

QString US_DataPub::timeStateDefs( const QString& tmstPath )
{
   if ( tmstPath.isEmpty() )  return QString();

   QFileInfo info( tmstPath );
   QString   base = info.completeBaseName();

   if ( base.isEmpty() )  return QString();

   QString dir = info.path();

   return dir.isEmpty() ? base + ".xml" : dir + "/" + base + ".xml";
}

US_DataPub::EntityType US_DataPub::scopeType( US_DataPub::Scope scope )
{
   int index = int( scope );

   if ( index < 0  ||  index >= scope_count )  return US_DataPub::UnknownType;

   return scope_types[ index ];
}

US_DataPub::Scope US_DataPub::typeScope( US_DataPub::EntityType type )
{
   // The time state of a run is exported together with its raw data
   if ( type == US_DataPub::TimeState )  return US_DataPub::ScopeRawData;

   for ( int ii = 0; ii < scope_count; ii++ )
      if ( scope_types[ ii ] == type )  return US_DataPub::Scope( ii );

   return US_DataPub::ScopeNone;
}

QString US_DataPub::scopeKey( US_DataPub::Scope scope )
{
   int index = int( scope );

   if ( index < 0  ||  index >= scope_count )  return QString( "none" );

   return US_DataPub::typeKey( scope_types[ index ] );
}

US_DataPub::Scope US_DataPub::scopeOfKey( const QString& key )
{
   for ( int ii = 0; ii < scope_count; ii++ )
      if ( US_DataPub::typeKey( scope_types[ ii ] ) == key )
         return US_DataPub::Scope( ii );

   return US_DataPub::ScopeNone;
}

QStringList US_DataPub::scopeKeys( void )
{
   QStringList keys;

   for ( int ii = 0; ii < scope_count; ii++ )
      keys << US_DataPub::typeKey( scope_types[ ii ] );

   return keys;
}

QString US_DataPub::policyKey( US_DataPub::ConflictPolicy policy )
{
   switch ( policy )
   {
      case US_DataPub::PolicyRename:  return QString( "rename" );
      case US_DataPub::PolicyFail:    return QString( "fail"   );
      default:                        return QString( "reuse"  );
   }
}

US_DataPub::ConflictPolicy US_DataPub::policyOfKey( const QString& name, bool* ok )
{
   if ( ok != nullptr )  *ok = true;

   if ( name == "reuse"  )  return US_DataPub::PolicyReuse;
   if ( name == "rename" )  return US_DataPub::PolicyRename;
   if ( name == "fail"   )  return US_DataPub::PolicyFail;

   if ( ok != nullptr )  *ok = false;

   return US_DataPub::PolicyReuse;
}

QString US_DataPub::resolutionText( US_DataPub::Resolution resolution )
{
   switch ( resolution )
   {
      case US_DataPub::ResolvedCreated:  return QObject::tr( "created" );
      case US_DataPub::ResolvedReused:   return QObject::tr( "reused"  );
      case US_DataPub::ResolvedRenamed:  return QObject::tr( "renamed" );
      case US_DataPub::ResolvedSkipped:  return QObject::tr( "skipped" );
      case US_DataPub::ResolvedSubstituted:
                                         return QObject::tr( "substituted" );
      case US_DataPub::ResolvedOmitted:  return QObject::tr( "not imported" );
      case US_DataPub::ResolvedFailed:   return QObject::tr( "failed"  );
   }

   return QObject::tr( "failed" );
}

// ------------------------------------------------------------ path safety

bool US_DataPub::isReservedDeviceName( const QString& name )
{
   // Windows opens a device for these names, whatever extension follows
   static const QRegularExpression rx(
         "\\A(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])\\z",
         QRegularExpression::CaseInsensitiveOption );

   return rx.match( name.section( '.', 0, 0 ).trimmed() ).hasMatch();
}

bool US_DataPub::isSafeFileName( const QString& name, QString* why )
{
   QString reason;

   if ( name.isEmpty() )
      reason = QObject::tr( "the name is empty" );
   else if ( name.contains( '/' )  ||  name.contains( '\\' ) )
      reason = QObject::tr( "the name has a directory separator in it" );
   else if ( name == "."  ||  name == ".." )
      reason = QObject::tr( "the name is a directory reference" );
   else if ( name.startsWith( '.' ) )
      reason = QObject::tr( "the name is hidden" );
   else if ( name.contains( ':' ) )
      reason = QObject::tr( "the name has a colon in it" );
   else if ( name.toUtf8().size() > 255 )
      reason = QObject::tr( "the name is too long" );
   else if ( name.endsWith( '.' )  ||  name.endsWith( ' ' ) )
      reason = QObject::tr( "the name ends in a dot or a space" );
   else if ( isReservedDeviceName( name ) )
      reason = QObject::tr( "the name is reserved for a device on Windows" );
   else
   {
      static const QString forbidden( "<>\"|?*" );

      for ( int ii = 0; ii < name.size(); ii++ )
      {
         QChar chr  = name.at( ii );
         uint  code = chr.unicode();

         if ( chr.isHighSurrogate()  &&  ii + 1 < name.size()  &&
              name.at( ii + 1 ).isLowSurrogate() )
            code = QChar::surrogateToUcs4( chr, name.at( ++ii ) );

         if ( code < 0x20  ||  ( code >= 0x7f  &&  code <= 0x9f ) )
            reason = QObject::tr( "the name has a control character in it" );

         else if ( forbidden.contains( chr ) )
            reason = QObject::tr( "the name has one of the characters"
                                  " < > \" | ? * in it" );

         else if ( QChar::category( code ) == QChar::Other_Format )
            reason = QObject::tr( "the name has an invisible formatting"
                                  " character in it" );

         if ( ! reason.isEmpty() )  break;
      }
   }

   if ( why != nullptr )  *why = reason;

   return reason.isEmpty();
}

bool US_DataPub::isSafeRelativePath( const QString& path, QString* why )
{
   QString reason;

   if ( path.isEmpty() )
      reason = QObject::tr( "the path is empty" );
   else if ( path.contains( '\\' ) )
      reason = QObject::tr( "the path has a backslash in it" );
   else if ( path.startsWith( '/' ) )
      reason = QObject::tr( "the path is absolute" );
   else if ( path.size() >= 2  &&  path.at( 1 ) == QChar( ':' ) )
      reason = QObject::tr( "the path has a drive prefix" );
   else if ( path.size() > 1024 )
      reason = QObject::tr( "the path is too long" );
   else
   {
      const QStringList parts = path.split( '/' );

      for ( int ii = 0; ii < parts.size(); ii++ )
      {
         QString partWhy;

         if ( isSafeFileName( parts[ ii ], &partWhy ) )  continue;

         reason = QObject::tr( "component %1: %2" ).arg( ii + 1 ).arg( partWhy );
         break;
      }
   }

   if ( why != nullptr )  *why = reason;

   return reason.isEmpty();
}

bool US_DataPub::runIdIsUsable( const QString& runID )
{
   // The rule of the reader the run is handed to, and an anchored check of
   // its own on top: "$" also matches before a final newline, and a device
   // name cannot be a directory on Windows
   static const QRegularExpression rx( "\\A[A-Za-z0-9_-]{1,80}\\z" );

   return US_ConvertIO::runIdIsUsable( runID )  &&
          rx.match( runID ).hasMatch()  &&
          ! isReservedDeviceName( runID );
}

QString US_DataPub::derivedGuid( const QString& name )
{
   // A fixed namespace for the identities this program makes up
   static const QUuid ns( "6c1f6f0e-3b2a-5d4e-9c1f-7a2b3c4d5e6f" );

   return QUuid::createUuidV5( ns, name.toUtf8() )
          .toString( QUuid::WithoutBraces );
}

bool US_DataPub::isPayloadKey( const QString& key )
{
   return ( key == "payload" )  ||
          ( key.size() > 7  &&  key.endsWith( "Payload" ) );
}

QString US_DataPub::digestKeyOf( const QString& payloadKey )
{
   if ( payloadKey == "payload" )  return QString( "sha256" );

   if ( ! isPayloadKey( payloadKey ) )  return QString();

   return payloadKey.left( payloadKey.size() - 7 ) + "Sha256";
}
