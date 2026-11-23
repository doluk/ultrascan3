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

      // Nothing read before the end is a read error: the digest of the
      // part read so far would pass for the file's
      if ( block.isEmpty() )
      {
         file.close();
         return QString();
      }

      hash.addData( block );
   }

   bool failed = ( file.error() != QFileDevice::NoError );
   file.close();

   if ( failed )  return QString();

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

/* Whether an attribute is left out on one element.

   The volatile keys are left out everywhere but in one place: the "id" of a
   buffer's <component>.  A component element carries nothing but that ID
   and a concentration, so dropping the ID made a buffer of 150 mM NaCl the
   same as one of 150 mM KCl.  The ID is installation specific, so a caller
   comparing two buffers has to put both in terms of the same component
   table first -- which the importer does, as it does for parent GUIDs.
*/
bool US_DataPubHash::isVolatileOn( const QString& element, const QString& name )
{
   if ( element == "component"  &&  name == "id" )  return false;

   return isVolatileKey( name );
}

namespace
{
   // Markup characters escaped, so that no attribute value or text can
   // close its own quote or element and fold two records onto one form
   QByteArray escaped( const QString& text, bool attribute )
   {
      QString out;
      out.reserve( text.size() );

      for ( int ii = 0; ii < text.size(); ii++ )
      {
         QChar ch = text[ ii ];

         if      ( ch == '&' )                 out += "&amp;";
         else if ( ch == '<' )                 out += "&lt;";
         else if ( ch == '>' )                 out += "&gt;";
         else if ( ch == '"'  &&  attribute )  out += "&quot;";
         else                                  out += ch;
      }

      return out.toUtf8();
   }
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

            if ( isVolatileOn( ename, aname ) )  continue;
            if ( isOwn  &&  aname == ownAttr )   continue;

            out += " " + aname.toUtf8() + "=\""
                   + escaped( attrs.value( aname ).toString().trimmed(), true )
                   + "\"";
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
            out += escaped( text, false );
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
