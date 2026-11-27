//! \file us_datapub_hash.cpp
#include "us_datapub_hash.h"
#include "us_datapub_defs.h"

#include <QCryptographicHash>
#include <QRegularExpression>
#include <QXmlStreamReader>

QString US_DataPubHash::fileHash( const QString& filename )
{
   // The digest the export writes into the manifest: one that fails on any
   // read error rather than digest the part read before it
   QString digest;

   if ( ! US_DataPub::fileSha256( filename, digest ) )  return QString();

   return digest;
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

   /* A number in one form.  The library's writers have changed how they
      write numbers -- "7.00000" (a fixed number of decimals) then, "7" (the
      shortest form that reads back as the same double) now -- so the same
      record written before and after that change differs in its text only.

      Only an attribute that holds a number (isNumericKey()) is read as
      one; a name, a description, a label or a serial number keeps its
      text whatever it looks like, so "007" stays apart from "7".  And only
      a value a double holds exactly enough to compare is folded: one with
      more significant digits than a double keeps (a huge integer), or one
      that is not zero but reads as zero (an underflow), keeps its text, so
      two different values never become one.  Anything that is not a
      decimal number (a GUID, "inf") is kept as it is.
   */
   QString canonical_value( const QString& value )
   {
      static const QRegularExpression number( QRegularExpression::anchoredPattern(
         "[+-]?([0-9]+\\.?[0-9]*|\\.[0-9]+)([eE][+-]?[0-9]+)?" ) );

      if ( ! number.match( value ).hasMatch() )  return value;

      // The significant digits of the mantissa
      QString mantissa = value.section( QRegularExpression( "[eE]" ), 0, 0 );
      bool    point    = mantissa.contains( '.' );
      QString digits   = mantissa;
      digits.remove( '+' ).remove( '-' ).remove( '.' );

      while ( digits.startsWith( '0' ) )  digits.remove( 0, 1 );

      if ( point )
         while ( digits.endsWith( '0' ) )  digits.chop( 1 );

      if ( digits.size() > 17 )  return value;     // More than a double holds

      bool   ok = false;
      double dv = QLocale::c().toDouble( value, &ok );

      if ( ! ok  ||  ! qIsFinite( dv ) )  return value;

      if ( dv == 0.0  &&  ! digits.isEmpty() )  return value;   // Underflow

      if ( dv == 0.0 )  dv = 0.0;         // -0 is 0

      return QLocale::c().toString( dv, 'g', QLocale::FloatingPointShortest );
   }
}

/* The attributes the records' writers put numbers in, by name.

   An explicit list, like the volatile keys: a pattern would sooner or
   later take in a text attribute, and a text that merely looks like a
   number ("007", a serial, a label) would then compare equal to another.
   An attribute missing from the list keeps its text, which at worst makes
   two copies of one record compare as different -- the import then makes
   a renamed copy rather than reuse the wrong record.
*/
bool US_DataPubHash::isNumericKey( const QString& name )
{
   static const QSet< QString > keys = QSet< QString >()
      // buffers and their components
      << "compressibility" << "ph"         << "density"    << "viscosity"
      << "concentration"   << "value"      << "wavelength"
      << "c0" << "c1" << "c2" << "c3" << "c4" << "c5"
      // analytes
      << "vbar20"     << "mw"           << "frequency"  << "sodium"
      << "potassium"  << "lithium"      << "magnesium"  << "calcium"
      // solutions
      << "amount"     << "commonVbar20" << "storageTemp"
      // models and their components
      << "D"          << "K_d"          << "alphaRP"    << "bottom"
      << "delta"      << "extinction"   << "f"          << "f_f0"
      << "k_off"      << "meniscus"     << "molar"      << "radius"
      << "s"          << "sigma"        << "signal"     << "stoich"
      << "variance"   << "xMax"         << "xMin"       << "xRes"
      << "yMax"       << "yMin"         << "yRes"       << "zVal"
      << "temperature"
      // noise
      << "minradius"  << "maxradius"    << "v"
      // rotor calibrations and centerpieces
      << "coeff1"     << "coeff2"       << "omega2t"    << "angle"
      << "pathlen"    << "width"        << "maxRPM";

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

            if ( isVolatileOn( ename, aname ) )  continue;
            if ( isOwn  &&  aname == ownAttr )   continue;

            QString value = attrs.value( aname ).toString().trimmed();

            if ( isNumericKey( aname ) )  value = canonical_value( value );

            out += " " + aname.toUtf8() + "=\"" + escaped( value, true )
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
