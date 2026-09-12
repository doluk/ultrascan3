//! \file us_datapub_manifest.cpp
#include "us_datapub_manifest.h"

US_DataPubEntity::US_DataPubEntity()
{
   type = US_DataPub::UnknownType;
   id   = QString( "-1" );
}

US_DataPubEntity::US_DataPubEntity( US_DataPub::EntityType a_type )
{
   type = a_type;
   id   = QString( "-1" );
}

void US_DataPubEntity::setDepend( US_DataPub::EntityType parentType,
                                  const QString& parentGUID )
{
   if ( parentGUID.isEmpty() )  return;

   depends.insert( US_DataPub::guidKey( parentType ), parentGUID );
}

QString US_DataPubEntity::depend( US_DataPub::EntityType parentType ) const
{
   return depends.value( US_DataPub::guidKey( parentType ), QString() );
}

void US_DataPubEntity::setField( const QString& key, const QString& value )
{
   if      ( key == US_DataPub::idKey  ( type ) )  id       = value;
   else if ( key == US_DataPub::guidKey( type ) )  guid     = value;
   else if ( key == "payload"                   )  payload  = value;
   else if ( key == "sha256"                    )  sha256   = value;
   else                                            attrs.insert( key, value );

   // The name key may coincide with "filename"; keep both in step
   if ( key == US_DataPub::nameKey( type ) )       name     = value;
   if ( key == "filename"                   )      filename = value;

   if ( key == US_DataPub::nameKey( type )  ||  key == "filename" )
      attrs.remove( key );
}

QString US_DataPubEntity::field( const QString& key ) const
{
   if      ( key == US_DataPub::idKey  ( type ) )  return id;
   else if ( key == US_DataPub::guidKey( type ) )  return guid;
   else if ( key == US_DataPub::nameKey( type ) )  return name;
   else if ( key == "filename"                  )  return filename;
   else if ( key == "payload"                   )  return payload;
   else if ( key == "sha256"                    )  return sha256;

   return attrs.value( key, QString() );
}

QStringList US_DataPubEntity::fieldKeys( void ) const
{
   QStringList keys;
   QString     nkey = US_DataPub::nameKey( type );

   keys << US_DataPub::idKey  ( type );
   keys << US_DataPub::guidKey( type );

   if ( ! name.isEmpty() )
      keys << nkey;

   if ( ! filename.isEmpty()  &&  nkey != "filename" )
      keys << "filename";

   QList< QString > akeys = attrs.keys();

   for ( int ii = 0; ii < akeys.size(); ii++ )
      if ( ! keys.contains( akeys[ ii ] ) )
         keys << akeys[ ii ];

   if ( ! payload.isEmpty() )  keys << "payload";
   if ( ! sha256 .isEmpty() )  keys << "sha256";

   return keys;
}

bool US_DataPubEntity::isValid( void ) const
{
   return ( type != US_DataPub::UnknownType  &&  ! guid.isEmpty() );
}

QString US_DataPubEntity::label( void ) const
{
   QString text = US_DataPub::typeText( type );

   if ( ! name.isEmpty() )
      text += ": " + name;

   else if ( ! filename.isEmpty() )
      text += ": " + filename;

   else
      text += ": " + guid;

   return text;
}

US_DataPubManifest::US_DataPubManifest()
{
   clear();
}

void US_DataPubManifest::clear( void )
{
   version    = US_DataPubManifest::currentVersion;
   created    = QString();
   generator  = QString();
   source     = QString();
   scope      = QString();
   bundleGUID = QString();
   comment    = QString();

   sections.clear();
}

QList< US_DataPub::EntityType > US_DataPubManifest::orderedTypes( void )
{
   QList< US_DataPub::EntityType > types;

   types << US_DataPub::Project
         << US_DataPub::Experiment
         << US_DataPub::RawData
         << US_DataPub::TimeState
         << US_DataPub::RotorCalibration
         << US_DataPub::Centerpiece
         << US_DataPub::Buffer
         << US_DataPub::Analyte
         << US_DataPub::Solution
         << US_DataPub::EditedData
         << US_DataPub::Model
         << US_DataPub::Noise;

   return types;
}

void US_DataPubManifest::add( const US_DataPubEntity& entity )
{
   sections[ int( entity.type ) ] << entity;
}

void US_DataPubManifest::setSection( US_DataPub::EntityType type,
                                     const QList< US_DataPubEntity >& entities )
{
   sections.insert( int( type ), entities );
}

QList< US_DataPubEntity > US_DataPubManifest::section(
      US_DataPub::EntityType type ) const
{
   return sections.value( int( type ), QList< US_DataPubEntity >() );
}

QList< US_DataPubEntity > US_DataPubManifest::all( void ) const
{
   QList< US_DataPubEntity >       entities;
   QList< US_DataPub::EntityType > types = orderedTypes();

   for ( int ii = 0; ii < types.size(); ii++ )
      entities << section( types[ ii ] );

   return entities;
}

int US_DataPubManifest::count( US_DataPub::EntityType type ) const
{
   return section( type ).size();
}

int US_DataPubManifest::total( void ) const
{
   int knt = 0;
   QList< US_DataPub::EntityType > types = orderedTypes();

   for ( int ii = 0; ii < types.size(); ii++ )
      knt += count( types[ ii ] );

   return knt;
}

bool US_DataPubManifest::contains( US_DataPub::EntityType type,
                                   const QString& guid ) const
{
   US_DataPubEntity found;

   return entity( type, guid, found );
}

bool US_DataPubManifest::entity( US_DataPub::EntityType type,
                                 const QString& guid,
                                 US_DataPubEntity& found ) const
{
   QList< US_DataPubEntity > entities = section( type );

   for ( int ii = 0; ii < entities.size(); ii++ )
   {
      if ( entities[ ii ].guid == guid )
      {
         found = entities[ ii ];
         return true;
      }
   }

   return false;
}

QString US_DataPubManifest::escape( const QString& value )
{
   QString text = value;

   text.replace( "\\", "\\\\" );
   text.replace( "\"", "\\\"" );
   text.replace( "\n", "\\n"  );
   text.replace( "\r", "\\r"  );
   text.replace( "\t", "\\t"  );

   return "\"" + text + "\"";
}

QString US_DataPubManifest::unescape( const QString& value )
{
   QString text = value.trimmed();

   if ( text.size() < 2  ||  ! text.startsWith( "\"" )  ||
        ! text.endsWith( "\"" ) )
      return text;

   text    = text.mid( 1, text.size() - 2 );
   QString out;
   int     ii = 0;

   while ( ii < text.size() )
   {
      QChar chr = text.at( ii++ );

      if ( chr != QChar( '\\' )  ||  ii >= text.size() )
      {
         out += chr;
         continue;
      }

      QChar nxt = text.at( ii++ );

      if      ( nxt == QChar( 'n' ) )  out += QChar( '\n' );
      else if ( nxt == QChar( 'r' ) )  out += QChar( '\r' );
      else if ( nxt == QChar( 't' ) )  out += QChar( '\t' );
      else                             out += nxt;
   }

   return out;
}

QString US_DataPubManifest::toYaml( void ) const
{
   QString text;
   QTextStream ts( &text, QIODevice::WriteOnly );

   ts << "# UltraScan3 data publication bundle manifest\n";
   ts << "# Records are listed in dependency order; an import replays them"
         " top to bottom.\n";
   ts << "version: "    << QString::number( version ) << "\n";
   ts << "created: "    << escape( created    )       << "\n";
   ts << "generator: "  << escape( generator  )       << "\n";
   ts << "source: "     << escape( source     )       << "\n";
   ts << "scope: "      << escape( scope      )       << "\n";
   ts << "bundleGUID: " << escape( bundleGUID )       << "\n";
   ts << "comment: "    << escape( comment    )       << "\n";

   QList< US_DataPub::EntityType > types = orderedTypes();

   for ( int ii = 0; ii < types.size(); ii++ )
   {
      US_DataPub::EntityType     type     = types[ ii ];
      QList< US_DataPubEntity >  entities = section( type );
      QString                    key      = US_DataPub::typeKey( type );

      if ( entities.isEmpty() )
      {
         ts << key << ": []\n";
         continue;
      }

      ts << key << ":\n";

      for ( int jj = 0; jj < entities.size(); jj++ )
      {
         const US_DataPubEntity& entity = entities[ jj ];
         QStringList             fkeys  = entity.fieldKeys();
         bool                    first  = true;

         for ( int kk = 0; kk < fkeys.size(); kk++ )
         {
            QString fkey = fkeys[ kk ];

            ts << ( first ? "  - " : "    " ) << fkey << ": "
               << escape( entity.field( fkey ) ) << "\n";
            first = false;
         }

         if ( first )            // An entity with no fields at all
            ts << "  - {}\n";

         if ( entity.depends.isEmpty() )  continue;

         ts << "    depends:\n";

         QList< QString > dkeys = entity.depends.keys();

         for ( int kk = 0; kk < dkeys.size(); kk++ )
            ts << "      " << dkeys[ kk ] << ": "
               << escape( entity.depends.value( dkeys[ kk ] ) ) << "\n";
      }
   }

   ts.flush();

   return text;
}

bool US_DataPubManifest::fromYaml( const QString& text, QString& error )
{
   clear();

   QStringList lines = text.split( "\n" );

   US_DataPub::EntityType    curType  = US_DataPub::UnknownType;
   QList< US_DataPubEntity > entities;
   bool                      haveItem = false;
   US_DataPubEntity          item;
   QString                   nestKey;
   int                       nestInd  = -1;
   bool                      haveSect = false;

   for ( int ii = 0; ii < lines.size(); ii++ )
   {
      QString line = lines[ ii ];

      if ( line.endsWith( "\r" ) )  line.chop( 1 );

      QString trimmed = line.trimmed();

      if ( trimmed.isEmpty()  ||  trimmed.startsWith( "#" ) )  continue;

      int indent = 0;

      while ( indent < line.size()  &&  line.at( indent ) == QChar( ' ' ) )
         indent++;

      if ( indent < line.size()  &&  line.at( indent ) == QChar( '\t' ) )
      {
         error = QObject::tr( "manifest line %1: tabs are not allowed" )
                 .arg( ii + 1 );
         return false;
      }

      // ---- a new top-level key closes whatever came before ----
      if ( indent == 0 )
      {
         if ( haveSect )
         {
            if ( haveItem )  entities << item;
            setSection( curType, entities );
         }

         haveItem = false;
         haveSect = false;
         nestKey.clear();
         nestInd  = -1;
         entities.clear();
         item     = US_DataPubEntity();

         int colon = trimmed.indexOf( ':' );

         if ( colon < 0 )
         {
            error = QObject::tr( "manifest line %1: expected \"key: value\"" )
                    .arg( ii + 1 );
            return false;
         }

         QString key   = trimmed.left( colon ).trimmed();
         QString value = trimmed.mid( colon + 1 ).trimmed();

         if ( key == "version"    )  { version    = value.toInt();      continue; }
         if ( key == "created"    )  { created    = unescape( value );  continue; }
         if ( key == "generator"  )  { generator  = unescape( value );  continue; }
         if ( key == "source"     )  { source     = unescape( value );  continue; }
         if ( key == "scope"      )  { scope      = unescape( value );  continue; }
         if ( key == "bundleGUID" )  { bundleGUID = unescape( value );  continue; }
         if ( key == "comment"    )  { comment    = unescape( value );  continue; }

         US_DataPub::EntityType type = US_DataPub::typeOfKey( key );

         if ( type == US_DataPub::UnknownType )
         {
            error = QObject::tr( "manifest line %1: unknown section \"%2\"" )
                    .arg( ii + 1 ).arg( key );
            return false;
         }

         curType  = type;
         haveSect = true;

         if ( value == "[]" )         // An explicitly empty section
         {
            setSection( curType, QList< US_DataPubEntity >() );
            haveSect = false;
         }

         continue;
      }

      if ( ! haveSect )
      {
         error = QObject::tr( "manifest line %1: indented line outside of"
                              " a section" ).arg( ii + 1 );
         return false;
      }

      // ---- a "- " starts the next item of the current section ----
      if ( trimmed.startsWith( "- " )  ||  trimmed == "-" )
      {
         if ( haveItem )  entities << item;

         item     = US_DataPubEntity( curType );
         haveItem = true;
         nestKey.clear();
         nestInd  = -1;
         trimmed  = trimmed.mid( 1 ).trimmed();

         if ( trimmed.isEmpty()  ||  trimmed == "{}" )  continue;
      }

      if ( ! haveItem )
      {
         error = QObject::tr( "manifest line %1: value outside of a record" )
                 .arg( ii + 1 );
         return false;
      }

      int colon = trimmed.indexOf( ':' );

      if ( colon < 0 )
      {
         error = QObject::tr( "manifest line %1: expected \"key: value\"" )
                 .arg( ii + 1 );
         return false;
      }

      QString key   = trimmed.left( colon ).trimmed();
      QString value = trimmed.mid( colon + 1 ).trimmed();

      if ( ! nestKey.isEmpty()  &&  indent > nestInd )
      {
         if ( nestKey == "depends" )
            item.depends.insert( key, unescape( value ) );

         continue;
      }

      nestKey.clear();
      nestInd = -1;

      if ( value.isEmpty() )
      {
         nestKey = key;
         nestInd = indent;
         continue;
      }

      item.setField( key, unescape( value ) );
   }

   if ( haveSect )
   {
      if ( haveItem )  entities << item;
      setSection( curType, entities );
   }

   error.clear();

   return true;
}

bool US_DataPubManifest::write( const QString& filename, QString& error ) const
{
   QFile file( filename );

   if ( ! file.open( QIODevice::WriteOnly | QIODevice::Text ) )
   {
      error = QObject::tr( "Cannot open manifest for writing: %1" )
              .arg( filename );
      return false;
   }

   QTextStream ts( &file );
   ts << toYaml();
   ts.flush();
   file.close();

   error.clear();

   return true;
}

bool US_DataPubManifest::read( const QString& filename, QString& error )
{
   QFile file( filename );

   if ( ! file.open( QIODevice::ReadOnly | QIODevice::Text ) )
   {
      error = QObject::tr( "Cannot open manifest for reading: %1" )
              .arg( filename );
      return false;
   }

   QTextStream ts( &file );
   QString     text = ts.readAll();
   file.close();

   return fromYaml( text, error );
}

bool US_DataPubManifest::validate( QString& error ) const
{
   QList< US_DataPubEntity > entities = all();

   for ( int ii = 0; ii < entities.size(); ii++ )
   {
      const US_DataPubEntity& entity = entities[ ii ];

      if ( ! entity.isValid() )
      {
         error = QObject::tr( "%1 record %2 has no GUID" )
                 .arg( US_DataPub::typeText( entity.type ) )
                 .arg( ii + 1 );
         return false;
      }

      QList< QString > dkeys = entity.depends.keys();

      for ( int jj = 0; jj < dkeys.size(); jj++ )
      {
         QString                dkey  = dkeys[ jj ];
         QString                dguid = entity.depends.value( dkey );
         US_DataPub::EntityType dtype = US_DataPub::typeOfGuidKey( dkey );

         if ( dtype == US_DataPub::UnknownType )
         {
            error = QObject::tr( "%1 \"%2\" depends on unknown key \"%3\"" )
                    .arg( US_DataPub::typeText( entity.type ) )
                    .arg( entity.label() ).arg( dkey );
            return false;
         }

         if ( dguid.isEmpty() )  continue;

         if ( ! contains( dtype, dguid ) )
         {
            error = QObject::tr( "%1 \"%2\" depends on %3 %4,"
                                 " which the bundle does not contain" )
                    .arg( US_DataPub::typeText( entity.type ) )
                    .arg( entity.label() )
                    .arg( US_DataPub::typeText( dtype ) ).arg( dguid );
            return false;
         }
      }
   }

   error.clear();

   return true;
}

QString US_DataPubManifest::summary( void ) const
{
   QString                         text;
   QList< US_DataPub::EntityType > types = orderedTypes();

   for ( int ii = 0; ii < types.size(); ii++ )
   {
      int knt = count( types[ ii ] );

      if ( knt < 1 )  continue;

      text += QString( "%1: %2\n" )
              .arg( US_DataPub::typeText( types[ ii ] ) ).arg( knt );
   }

   if ( text.isEmpty() )
      text = QObject::tr( "(empty bundle)\n" );

   return text;
}
