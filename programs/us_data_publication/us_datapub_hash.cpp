//! \file us_datapub_hash.cpp
#include "us_datapub_hash.h"

#include <QCryptographicHash>
#include <QXmlStreamReader>

QString US_DataPubHash::fileHash( const QString& filename )
{
   QFile file( filename );

   if ( ! file.open( QIODevice::ReadOnly ) )  return QString();

   QCryptographicHash hash( QCryptographicHash::Sha256 );

   while ( ! file.atEnd() )
   {
      QByteArray block = file.read( 1024 * 256 );

      if ( block.isEmpty() )  break;

      hash.addData( block );
   }

   file.close();

   return QString( hash.result().toHex() );
}

QString US_DataPubHash::dataHash( const QByteArray& data )
{
   return QString( QCryptographicHash::hash( data,
                   QCryptographicHash::Sha256 ).toHex() );
}

bool US_DataPubHash::isXmlFile( const QString& filename )
{
   return filename.endsWith( ".xml", Qt::CaseInsensitive );
}

bool US_DataPubHash::isVolatileKey( const QString& name )
{
   QString key = name.toLower();

   if ( key == "id"          )  return true;
   if ( key.endsWith( "id" ) )  return true;
   if ( key.contains( "guid"    ) )  return true;
   if ( key.contains( "date"    ) )  return true;
   if ( key.contains( "updated" ) )  return true;
   if ( key.contains( "created" ) )  return true;

   return false;
}

QByteArray US_DataPubHash::canonicalXml( const QByteArray& xml )
{
   QXmlStreamReader reader( xml );
   QByteArray       out;

   while ( ! reader.atEnd() )
   {
      reader.readNext();

      if ( reader.isStartElement() )
      {
         out += "<" + reader.name().toString().toUtf8();

         QXmlStreamAttributes attrs = reader.attributes();
         QStringList          names;

         for ( int ii = 0; ii < attrs.size(); ii++ )
            names << attrs.at( ii ).name().toString();

         names.sort();

         for ( int ii = 0; ii < names.size(); ii++ )
         {
            QString aname = names[ ii ];

            if ( isVolatileKey( aname ) )  continue;

            out += " " + aname.toUtf8() + "=\""
                   + attrs.value( aname ).toString().trimmed().toUtf8() + "\"";
         }

         out += ">";
      }

      else if ( reader.isEndElement() )
      {
         out += "</" + reader.name().toString().toUtf8() + ">";
      }

      else if ( reader.isCharacters()  &&  ! reader.isWhitespace() )
      {
         QString text = reader.text().toString().simplified();

         if ( ! text.isEmpty() )
            out += text.toUtf8();
      }
   }

   if ( reader.hasError() )  return QByteArray();

   return out;
}

QString US_DataPubHash::xmlFingerprint( const QByteArray& xml )
{
   QByteArray canonical = canonicalXml( xml );

   if ( canonical.isEmpty() )              // Not parseable as XML
      return dataHash( xml );

   return dataHash( canonical );
}

QString US_DataPubHash::fingerprint( const QString& filename )
{
   if ( ! isXmlFile( filename ) )  return fileHash( filename );

   QFile file( filename );

   if ( ! file.open( QIODevice::ReadOnly ) )  return QString();

   QByteArray xml = file.readAll();
   file.close();

   return xmlFingerprint( xml );
}
