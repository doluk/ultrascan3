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

void US_DataPubEntity::addDepend( US_DataPub::EntityType parentType,
                                  const QString& parentGUID )
{
   if ( parentGUID.isEmpty() )  return;

   QStringList have = dependsOn( parentType );

   if ( have.contains( parentGUID ) )  return;

   QString key = US_DataPub::guidKey( parentType );

   if ( ! have.isEmpty() )
   {
      int index = have.size() + 1;

      while ( depends.contains( key + "_" + QString::number( index ) ) )
         index++;

      key += "_" + QString::number( index );
   }

   depends.insert( key, parentGUID );
}

QString US_DataPubEntity::depend( US_DataPub::EntityType parentType ) const
{
   return depends.value( US_DataPub::guidKey( parentType ), QString() );
}

QStringList US_DataPubEntity::dependsOn(
      US_DataPub::EntityType parentType ) const
{
   QStringList      guids;
   QList< QString > keys = depends.keys();

   for ( int ii = 0; ii < keys.size(); ii++ )
      if ( dependType( keys[ ii ] ) == parentType )
         guids << depends.value( keys[ ii ] );

   return guids;
}

US_DataPub::EntityType US_DataPubEntity::dependType( const QString& key )
{
   US_DataPub::EntityType type = US_DataPub::typeOfGuidKey( key );

   if ( type != US_DataPub::UnknownType )  return type;

   static const QRegularExpression rx( "^([A-Za-z]+)_([2-9]|[1-9][0-9]+)$" );
   QRegularExpressionMatch         match = rx.match( key );

   if ( ! match.hasMatch() )  return US_DataPub::UnknownType;

   return US_DataPub::typeOfGuidKey( match.captured( 1 ) );
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

QStringList US_DataPubEntity::payloadKeys( void ) const
{
   QStringList keys;

   if ( ! payload.isEmpty() )  keys << QString( "payload" );

   QList< QString > akeys = attrs.keys();

   for ( int ii = 0; ii < akeys.size(); ii++ )
      if ( US_DataPub::isPayloadKey( akeys[ ii ] )  &&
           ! attrs.value( akeys[ ii ] ).isEmpty() )
         keys << akeys[ ii ];

   return keys;
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
   index   .clear();
}

/* The section order of a manifest.  Every record depends only on records
   of sections listed before its own: an experiment names its project and
   rotor calibration, a solution its buffer, analytes and experiment, and a
   raw-data triple its experiment and solution.  This is not the order of
   the export scopes -- a scope adds what a user asks for, the section order
   is what a reader needs to resolve the dependencies top to bottom.
*/
QList< US_DataPub::EntityType > US_DataPubManifest::orderedTypes( void )
{
   QList< US_DataPub::EntityType > types;

   types << US_DataPub::Project
         << US_DataPub::RotorCalibration
         << US_DataPub::Centerpiece
         << US_DataPub::Experiment
         << US_DataPub::TimeState
         << US_DataPub::Buffer
         << US_DataPub::Analyte
         << US_DataPub::Solution
         << US_DataPub::RawData
         << US_DataPub::EditedData
         << US_DataPub::Model
         << US_DataPub::Noise;

   return types;
}

void US_DataPubManifest::add( const US_DataPubEntity& entity )
{
   QList< US_DataPubEntity >& list  = sections[ int( entity.type ) ];
   QHash< QString, int >&     guids = index   [ int( entity.type ) ];

   if ( ! guids.contains( entity.guid ) )
      guids.insert( entity.guid, list.size() );

   list << entity;
}

void US_DataPubManifest::declareSection( US_DataPub::EntityType type )
{
   if ( ! sections.contains( int( type ) ) )
      sections.insert( int( type ), QList< US_DataPubEntity >() );
}

bool US_DataPubManifest::isDeclared( US_DataPub::EntityType type ) const
{
   return sections.contains( int( type ) );
}

void US_DataPubManifest::setSection( US_DataPub::EntityType type,
                                     const QList< US_DataPubEntity >& entities )
{
   sections.insert( int( type ), entities );

   QHash< QString, int >& guids = index[ int( type ) ];
   guids.clear();

   for ( int ii = 0; ii < entities.size(); ii++ )
      if ( ! guids.contains( entities[ ii ].guid ) )
         guids.insert( entities[ ii ].guid, ii );
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

// A GUID lookup is a hash lookup: the exporter asks before every record
// it adds and the validation for every dependency, so a scan of the section
// would make both quadratic in the size of the bundle
bool US_DataPubManifest::contains( US_DataPub::EntityType type,
                                   const QString& guid ) const
{
   return index.value( int( type ) ).contains( guid );
}

bool US_DataPubManifest::entity( US_DataPub::EntityType type,
                                 const QString& guid,
                                 US_DataPubEntity& found ) const
{
   int at = index.value( int( type ) ).value( guid, -1 );

   if ( at < 0 )  return false;

   found = sections[ int( type ) ].at( at );

   return true;
}

bool US_DataPubManifest::addDependency( US_DataPub::EntityType type,
                                        const QString& guid,
                                        US_DataPub::EntityType parentType,
                                        const QString& parentGUID )
{
   int at = index.value( int( type ) ).value( guid, -1 );

   if ( at < 0 )  return false;

   sections[ int( type ) ][ at ].addDepend( parentType, parentGUID );

   return true;
}

QString US_DataPubManifest::escape( const QString& value )
{
   QString text = value;

   text.replace( "\\", "\\\\" );
   text.replace( "\"", "\\\"" );
   text.replace( "\n", "\\n"  );
   text.replace( "\r", "\\r"  );
   text.replace( "\t", "\\t"  );

   // Everything outside YAML's printable set goes as YAML's "\uXXXX" (or
   // "\UXXXXXXXX" beyond the first plane): the other control characters --
   // C0, DEL, C1 -- U+FFFE and U+FFFF, and a surrogate without its pair,
   // which UTF-8 cannot even hold.  So do the Unicode line and paragraph
   // separators and the noncharacters (U+FDD0..U+FDEF and the last two
   // code points of every plane): a YAML reader may take the former as line
   // breaks, and other tools refuse the latter.  A NUL would end the text
   // for a reader that stops at one.
   QString out;
   out.reserve( text.size() );

   for ( int ii = 0; ii < text.size(); ii++ )
   {
      QChar chr  = text.at( ii );
      uint  code = chr.unicode();
      int   len  = 1;

      if ( chr.isHighSurrogate()  &&  ii + 1 < text.size()  &&
           text.at( ii + 1 ).isLowSurrogate() )
      {
         code = QChar::surrogateToUcs4( chr, text.at( ii + 1 ) );
         len  = 2;
      }

      bool lone    = ( len == 1  &&  chr.isSurrogate() );
      bool nonchar = ( code >= 0xfdd0  &&  code <= 0xfdef )  ||
                     ( ( code & 0xfffe ) == 0xfffe );

      if ( code < 0x20  ||  ( code >= 0x7f  &&  code <= 0x9f )  ||
           code == 0x2028  ||  code == 0x2029  ||  lone  ||  nonchar )
      {
         if ( code > 0xffff )
            out += QString( "\\U%1" ).arg( code, 8, 16, QChar( '0' ) );
         else
            out += QString( "\\u%1" ).arg( code, 4, 16, QChar( '0' ) );
      }

      else
         out += text.mid( ii, len );

      ii += len - 1;
   }

   return "\"" + out + "\"";
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
      bool  hex = false;

      static const QRegularExpression hex4(
         QRegularExpression::anchoredPattern( "[0-9A-Fa-f]{4}" ) );
      static const QRegularExpression hex8(
         QRegularExpression::anchoredPattern( "[0-9A-Fa-f]{8}" ) );

      if ( nxt == QChar( 'u' )  &&  ii + 4 <= text.size()  &&
           hex4.match( text.mid( ii, 4 ) ).hasMatch() )
      {
         ushort code = text.mid( ii, 4 ).toUShort( &hex, 16 );

         if ( hex )
         {
            out += QChar( code );
            ii  += 4;
            continue;
         }
      }

      // YAML's escape beyond the first plane
      if ( nxt == QChar( 'U' )  &&  ii + 8 <= text.size()  &&
           hex8.match( text.mid( ii, 8 ) ).hasMatch() )
      {
         uint code = text.mid( ii, 8 ).toUInt( &hex, 16 );

         if ( hex  &&  code <= 0x10ffff )
         {
            if ( QChar::requiresSurrogates( code ) )
            {
               out += QChar( QChar::highSurrogate( code ) );
               out += QChar( QChar::lowSurrogate ( code ) );
            }

            else
               out += QChar( ushort( code ) );

            ii += 8;
            continue;
         }
      }

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
   ts << "# Records are listed in dependency order: each depends only on"
         " records listed before it.\n";
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

      if ( ! isDeclared( type ) )  continue;      // Not exported at all

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
   QStringList               seenKeys;      // Top-level keys read so far
   QStringList               itemKeys;      // Keys of the current item
   bool                      haveVers = false;

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

         if ( seenKeys.contains( key ) )
         {
            error = QObject::tr( "manifest line %1: \"%2\" appears more than"
                                 " once" ).arg( ii + 1 ).arg( key );
            return false;
         }

         seenKeys << key;

         if ( key == "version" )
         {
            bool ok   = false;
            int  vers = unescape( value ).toInt( &ok );

            if ( ! ok  ||  vers < 1 )
            {
               error = QObject::tr( "manifest line %1: \"%2\" is not a"
                                    " manifest format version" )
                       .arg( ii + 1 ).arg( value );
               return false;
            }

            version  = vers;
            haveVers = true;
            continue;
         }

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

         else if ( ! value.isEmpty() )
         {
            error = QObject::tr( "manifest line %1: section \"%2\" must be a"
                                 " list" ).arg( ii + 1 ).arg( key );
            return false;
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
         itemKeys.clear();
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
         if ( value.isEmpty() )
         {
            error = QObject::tr( "manifest line %1: dependency \"%2\" has"
                                 " no value" ).arg( ii + 1 ).arg( key );
            return false;
         }

         if ( item.depends.contains( key ) )
         {
            error = QObject::tr( "manifest line %1: dependency \"%2\""
                                 " appears more than once" )
                    .arg( ii + 1 ).arg( key );
            return false;
         }

         item.depends.insert( key, unescape( value ) );
         continue;
      }

      nestKey.clear();
      nestInd = -1;

      if ( itemKeys.contains( key ) )
      {
         error = QObject::tr( "manifest line %1: \"%2\" appears more than"
                              " once in one record" ).arg( ii + 1 ).arg( key );
         return false;
      }

      itemKeys << key;

      if ( value.isEmpty() )
      {  // Only "depends" is a nested map; anything else would have its
         // lines dropped without a word
         if ( key != "depends" )
         {
            error = QObject::tr( "manifest line %1: \"%2\" has no value;"
                                 " only \"depends\" may hold a nested map" )
                    .arg( ii + 1 ).arg( key );
            return false;
         }

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

   if ( ! haveVers )
   {
      error = QObject::tr( "the manifest names no format version" );
      return false;
   }

   if ( ! checkPaths( error ) )  return false;

   error.clear();

   return true;
}

// The manifest is UTF-8 whatever the locale: the bytes are written and
// read directly rather than through a QTextStream, whose codec on Qt5 is
// the locale's.
bool US_DataPubManifest::write( const QString& filename, QString& error ) const
{
   QSaveFile  file( filename );
   QByteArray bytes = toYaml().toUtf8();

   if ( ! file.open( QIODevice::WriteOnly ) )
   {
      error = QObject::tr( "Cannot open manifest for writing: %1" )
              .arg( filename );
      return false;
   }

   if ( file.write( bytes ) != bytes.size()  ||  ! file.commit() )
   {
      error = QObject::tr( "Cannot write the manifest %1: %2" )
              .arg( filename ).arg( file.errorString() );
      return false;
   }

   error.clear();

   return true;
}

bool US_DataPubManifest::read( const QString& filename, QString& error )
{
   QFile file( filename );

   if ( ! file.open( QIODevice::ReadOnly ) )
   {
      error = QObject::tr( "Cannot open manifest for reading: %1" )
              .arg( filename );
      return false;
   }

   // A manifest lists records, it does not carry them; one larger than
   // this is not read into memory at all
   QByteArray bytes  = file.read( maxManifestBytes + 1 );
   bool       failed = ( file.error() != QFileDevice::NoError );
   file.close();

   if ( failed )
   {
      error = QObject::tr( "Cannot read the manifest %1" ).arg( filename );
      return false;
   }

   if ( bytes.size() > maxManifestBytes )
   {
      error = QObject::tr( "The manifest %1 is larger than %2 MiB" )
              .arg( filename ).arg( maxManifestBytes >> 20 );
      return false;
   }

   // With its size: Qt 5's fromUtf8( QByteArray ) stops at a NUL
   return fromYaml( QString::fromUtf8( bytes.constData(), bytes.size() ),
                    error );
}

bool US_DataPubManifest::checkPaths( QString& error ) const
{
   QList< US_DataPubEntity > entities = all();

   // Every payload path, folded to one case: two paths that differ only in
   // case are one file on Windows and macOS, so one record's payload would
   // be read as another's
   QHash< QString, QString > folded;

   for ( int ii = 0; ii < entities.size(); ii++ )
   {
      const US_DataPubEntity& entity = entities[ ii ];
      QStringList             pkeys  = entity.payloadKeys();
      QString                 why;

      for ( int jj = 0; jj < pkeys.size(); jj++ )
      {
         QString path = entity.field( pkeys[ jj ] );

         if ( ! US_DataPub::isSafeRelativePath( path, &why ) )
         {
            error = QObject::tr( "%1: the %2 \"%3\" cannot be used (%4)" )
                    .arg( entity.label() ).arg( pkeys[ jj ] ).arg( path )
                    .arg( why );
            return false;
         }

         QString key = path.toCaseFolded();

         if ( folded.contains( key ) )
         {
            error = QObject::tr( "%1: the %2 \"%3\" is the same file as"
                                 " \"%4\" on a system that ignores case" )
                    .arg( entity.label() ).arg( pkeys[ jj ] ).arg( path )
                    .arg( folded.value( key ) );
            return false;
         }

         folded.insert( key, path );
      }

      if ( ! entity.filename.isEmpty()  &&
           ! US_DataPub::isSafeFileName( entity.filename, &why ) )
      {
         error = QObject::tr( "%1: the filename \"%2\" cannot be used (%3)" )
                 .arg( entity.label() ).arg( entity.filename ).arg( why );
         return false;
      }

      QStringList runIDs;

      if ( entity.type == US_DataPub::Experiment )
         runIDs << entity.name;

      if ( entity.attrs.contains( "runID" ) )
         runIDs << entity.attrs.value( "runID" );

      for ( int jj = 0; jj < runIDs.size(); jj++ )
      {
         if ( US_DataPub::runIdIsUsable( runIDs[ jj ] ) )  continue;

         error = QObject::tr( "%1: \"%2\" cannot be a run ID: a run ID may"
                              " hold only letters, digits, '_' and '-', at"
                              " most 80 of them" )
                 .arg( entity.label() ).arg( runIDs[ jj ] );
         return false;
      }
   }

   error.clear();

   return true;
}

bool US_DataPubManifest::checkAcceptance( QString& error ) const
{
   if ( ! checkPaths( error ) )  return false;

   QList< US_DataPubEntity > entities = all();
   QMap< QString, QString >  expRuns;          // experiment GUID -> run ID
   QList< US_DataPubEntity > exps     = section( US_DataPub::Experiment );

   for ( int ii = 0; ii < exps.size(); ii++ )
      expRuns.insert( exps[ ii ].guid, exps[ ii ].name );

   for ( int ii = 0; ii < entities.size(); ii++ )
   {
      const US_DataPubEntity& entity = entities[ ii ];
      US_DataPub::EntityType  type   = entity.type;

      if ( ! US_DataPub::guidIsUsable( entity.guid ) )
      {
         error = QObject::tr( "%1: \"%2\" is not a GUID" )
                 .arg( entity.label() ).arg( entity.guid );
         return false;
      }

      QList< QString > dkeys = entity.depends.keys();

      for ( int jj = 0; jj < dkeys.size(); jj++ )
      {
         QString dguid = entity.depends.value( dkeys[ jj ] );

         if ( US_DataPub::guidIsUsable( dguid ) )  continue;

         error = QObject::tr( "%1: the %2 it depends on, \"%3\", is not a"
                              " GUID" ).arg( entity.label() )
                 .arg( dkeys[ jj ] ).arg( dguid );
         return false;
      }

      if ( type == US_DataPub::Experiment )
      {
         QString runType = entity.attrs.value( "runType", "RA" );

         if ( ! US_DataPub::runTypeIsUsable( runType ) )
         {
            error = QObject::tr( "Run %1 has the unusable run type \"%2\":"
                                 " a run type is one to eight letters or"
                                 " digits" ).arg( entity.name ).arg( runType );
            return false;
         }
      }

      if ( type != US_DataPub::RawData  &&  type != US_DataPub::EditedData  &&
           type != US_DataPub::TimeState )
         continue;

      QString runID   = entity.attrs.value( "runID" );
      QString expGUID = entity.depend( US_DataPub::Experiment );

      if ( expRuns.contains( expGUID )  &&  expRuns.value( expGUID ) != runID )
      {
         error = QObject::tr( "%1 names run %2, but its experiment is run %3" )
                 .arg( entity.label() ).arg( runID )
                 .arg( expRuns.value( expGUID ) );
         return false;
      }

      if ( ! US_DataPub::runFileNameIsUsable( entity.filename, runID ) )
      {
         error = QObject::tr( "%1: the file name \"%2\" does not start with"
                              " its run ID %3" ).arg( entity.label() )
                 .arg( entity.filename ).arg( runID );
         return false;
      }

      if ( type == US_DataPub::TimeState  &&
           entity.filename != US_DataPub::timeStateFileName( runID ) )
      {
         error = QObject::tr( "%1: the file name \"%2\" is not %3" )
                 .arg( entity.label() ).arg( entity.filename )
                 .arg( US_DataPub::timeStateFileName( runID ) );
         return false;
      }

      if ( type == US_DataPub::TimeState  &&
           entity.attrs.value( "definitionsPayload" ).isEmpty() )
      {
         error = QObject::tr( "%1 carries no field definitions" )
                 .arg( entity.label() );
         return false;
      }
   }

   error.clear();

   return true;
}

bool US_DataPubManifest::validate( QString& error ) const
{
   QList< US_DataPubEntity >       entities = all();
   QList< US_DataPub::EntityType > order    = orderedTypes();

   if ( ! checkPaths( error ) )  return false;

   QHash< int, QSet< QString > > seenGuids;

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

      QSet< QString >& seen = seenGuids[ int( entity.type ) ];

      if ( seen.contains( entity.guid ) )
      {
         error = QObject::tr( "%1 %2 is listed more than once" )
                 .arg( US_DataPub::typeText( entity.type ) ).arg( entity.guid );
         return false;
      }

      seen.insert( entity.guid );

      QList< QString > dkeys = entity.depends.keys();

      for ( int jj = 0; jj < dkeys.size(); jj++ )
      {
         QString                dkey  = dkeys[ jj ];
         QString                dguid = entity.depends.value( dkey );
         US_DataPub::EntityType dtype = US_DataPubEntity::dependType( dkey );

         if ( dtype == US_DataPub::UnknownType )
         {
            error = QObject::tr( "%1 \"%2\" depends on unknown key \"%3\"" )
                    .arg( US_DataPub::typeText( entity.type ) )
                    .arg( entity.label() ).arg( dkey );
            return false;
         }

         if ( dguid.isEmpty() )  continue;

         if ( order.indexOf( dtype ) >= order.indexOf( entity.type ) )
         {
            error = QObject::tr( "%1 \"%2\" depends on a %3, which is not"
                                 " listed before it" )
                    .arg( US_DataPub::typeText( entity.type ) )
                    .arg( entity.label() ).arg( US_DataPub::typeText( dtype ) );
            return false;
         }

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
