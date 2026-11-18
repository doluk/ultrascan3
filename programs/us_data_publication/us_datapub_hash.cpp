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

/* The keys a copied record legitimately differs in.

   This is an explicit list on purpose.  Matching on a pattern -- "anything
   ending in id", "anything containing guid" -- also dropped real properties
   (a buffer component that is "solid", an analyte that is "liquid") and the
   GUIDs records use to name their parents, so two records that differed in
   exactly those compared equal and one was silently reused for the other.
   The record's own GUID is not on the list: which attribute holds it
   depends on the record type, so the caller names it.
*/
bool US_DataPubHash::isVolatileKey( const QString& name )
{
   static const QStringList keys = QStringList()
      // numeric record IDs, which each installation allocates itself
      << "id"         << "ID"           << "rotorID"      << "calExpID"
      << "bufferID"   << "analyteID"    << "solutionID"   << "projectID"
      << "expID"      << "experimentID" << "rawDataID"    << "editedDataID"
      << "editID"     << "modelID"      << "noiseID"      << "calibrationID"
      << "labID"      << "instrumentID" << "operatorID"   << "invID"
      << "personID"
      // time stamps
      << "date"       << "lastUpdated"  << "dateUpdated"  << "updated"
      << "created"    << "timeCreated"  << "dateCreated"
      // the calibration experiment of a rotor calibration: an import does
      // not carry it, so the target's copy never has the source's
      << "calExpGUID";

   return keys.contains( name );
}

QByteArray US_DataPubHash::canonicalXml( const QByteArray& xml,
                                         const QString& ownElement,
                                         const QString& ownAttr )
{
   QXmlStreamReader reader( xml );
   QByteArray       out;
   bool             ownSeen = ownElement.isEmpty()  ||  ownAttr.isEmpty();

   while ( ! reader.atEnd() )
   {
      reader.readNext();

      if ( reader.isStartElement() )
      {
         QString ename = reader.name().toString();
         bool    isOwn = false;

         if ( ! ownSeen  &&  ename == ownElement )
         {
            isOwn   = true;
            ownSeen = true;
         }

         out += "<" + ename.toUtf8();

         QXmlStreamAttributes attrs = reader.attributes();
         QStringList          names;

         for ( int ii = 0; ii < attrs.size(); ii++ )
            names << attrs.at( ii ).name().toString();

         names.sort();

         for ( int ii = 0; ii < names.size(); ii++ )
         {
            QString aname = names[ ii ];

            if ( isVolatileKey( aname ) )        continue;
            if ( isOwn  &&  aname == ownAttr )   continue;

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

QString US_DataPubHash::xmlFingerprint( const QByteArray& xml,
                                        const QString& ownElement,
                                        const QString& ownAttr )
{
   QByteArray canonical = canonicalXml( xml, ownElement, ownAttr );

   if ( canonical.isEmpty() )              // Not parseable as XML
      return dataHash( xml );

   return dataHash( canonical );
}

QString US_DataPubHash::fingerprint( const QString& filename,
                                     const QString& ownElement,
                                     const QString& ownAttr )
{
   if ( ! isXmlFile( filename ) )  return fileHash( filename );

   QFile file( filename );

   if ( ! file.open( QIODevice::ReadOnly ) )  return QString();

   // Read no more than the cap, whatever the file claims its size is
   QByteArray xml = file.read( maxXmlSize + 1 );
   file.close();

   if ( xml.size() > maxXmlSize )  return QString();

   return xmlFingerprint( xml, ownElement, ownAttr );
}
