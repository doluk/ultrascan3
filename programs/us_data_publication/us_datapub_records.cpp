//! \file us_datapub_records.cpp
#include "us_datapub_records.h"

namespace
{
   // A number written so that reading it back gives the same double
   QString exact( double value )
   {
      return QString::number( value, 'g', 17 );
   }

   // Finish an XML payload: the writer and the file both have to be clean
   // before the file replaces whatever was there
   bool finish_xml( QXmlStreamWriter& xml, QSaveFile& file )
   {
      if ( xml.hasError()  ||  file.error() != QFileDevice::NoError )
      {
         file.cancelWriting();
         return false;
      }

      return file.commit();
   }
}

bool US_DataPubRecords::writeCalibration(
      const US_Rotor::RotorCalibration& calibration, const QString& filename )
{
   QSaveFile file( filename );

   if ( ! file.open( QIODevice::WriteOnly | QIODevice::Text ) )  return false;

   QXmlStreamWriter xml;
   xml.setDevice( &file );
   xml.setAutoFormatting( true );

   xml.writeStartDocument();
   xml.writeDTD         ( "<!DOCTYPE US_RotorCalibration>" );
   xml.writeStartElement( "RotorCalibrationData" );
   xml.writeAttribute   ( "version", "1.0" );

   xml.writeStartElement( "Calibration" );
   xml.writeAttribute   ( "id",         QString::number( calibration.ID ) );
   xml.writeAttribute   ( "guid",       calibration.GUID );
   xml.writeAttribute   ( "rotorID",    QString::number( calibration.rotorID ) );
   xml.writeAttribute   ( "rotorGUID",  calibration.rotorGUID );
   xml.writeAttribute   ( "calExpID",
                          QString::number( calibration.calibrationExperimentID ) );
   xml.writeAttribute   ( "calExpGUID", calibration.calibrationExperimentGUID );
   xml.writeAttribute   ( "coeff1",     exact( calibration.coeff1 ) );
   xml.writeAttribute   ( "coeff2",     exact( calibration.coeff2 ) );
   xml.writeAttribute   ( "lastUpdated",
                          calibration.lastUpdated.toString( "yyyy-MM-dd" ) );
   xml.writeAttribute   ( "omega2t",    exact( calibration.omega2t ) );
   xml.writeAttribute   ( "label",      calibration.label );
   xml.writeTextElement ( "report",     calibration.report );
   xml.writeEndElement  ();          // Calibration

   xml.writeEndElement  ();          // RotorCalibrationData
   xml.writeEndDocument ();

   return finish_xml( xml, file );
}

QString US_DataPubRecords::calibrationGuid(
      const US_Rotor::RotorCalibration& calibration )
{
   if ( ! calibration.GUID.isEmpty() )  return calibration.GUID;

   // The ID and the rotor GUID alone are not unique across installations:
   // calibration 3 of a rotor is a different record in every database that
   // has one.  What was measured is part of the name, so two installations
   // only agree on the identity when they hold the same calibration.
   return US_DataPub::derivedGuid(
            "rotorCalibration:" + calibration.rotorGUID
          + ":" + QString::number( calibration.ID )
          + ":" + exact( calibration.coeff1 )
          + ":" + exact( calibration.coeff2 )
          + ":" + calibration.calibrationExperimentGUID
          + ":" + calibration.lastUpdated.toString( "yyyy-MM-dd" ) );
}

QString US_DataPubRecords::centerpieceGuid(
      const US_AbstractCenterpiece& centerpiece )
{
   if ( ! centerpiece.guid.isEmpty() )  return centerpiece.guid;

   // A serial number and a name are local choices; the geometry is what
   // makes two table rows the same centerpiece
   QStringList parts;
   parts << "centerpiece"
         << QString::number( centerpiece.serial_number )
         << centerpiece.name
         << centerpiece.material
         << QString::number( centerpiece.channels )
         << centerpiece.shape
         << exact( centerpiece.maxRPM )
         << exact( centerpiece.angle )
         << exact( centerpiece.width );

   int rows = qMin( centerpiece.path_length.size(),
                    centerpiece.bottom_position.size() );

   for ( int ii = 0; ii < rows; ii++ )
      parts << exact( centerpiece.path_length[ ii ] )
            << exact( centerpiece.bottom_position[ ii ] );

   return US_DataPub::derivedGuid( parts.join( ":" ) );
}

bool US_DataPubRecords::riProfileIsUsable( const QByteArray& xml,
                                           QString* why )
{
   QString          reason;
   QXmlStreamReader reader( xml );
   bool             root   = false;
   int              values = 0;

   while ( ! reader.atEnd() )
   {
      reader.readNext();

      if ( ! reader.isStartElement() )  continue;

      if ( ! root )
      {
         if ( reader.name().toString() != "US_RIProfile" )
         {
            reason = QObject::tr( "it is not a US_RIProfile document" );
            break;
         }

         root = true;
         continue;
      }

      if ( reader.name().toString() == "RI"  &&
           reader.attributes().hasAttribute( "value" ) )
         values++;
   }

   if ( reason.isEmpty() )
   {
      if ( xml.trimmed().isEmpty() )
         reason = QObject::tr( "it is empty" );
      else if ( reader.hasError() )
         reason = QObject::tr( "it is not well-formed XML: %1" )
                  .arg( reader.errorString() );
      else if ( ! root )
         reason = QObject::tr( "it is not a US_RIProfile document" );
      else if ( values < 1 )
         reason = QObject::tr( "it holds no RI values" );
   }

   if ( why != nullptr )  *why = reason;

   return reason.isEmpty();
}

bool US_DataPubRecords::readCalibration(
      const QString& filename, US_Rotor::RotorCalibration& calibration )
{
   QFile file( filename );

   if ( ! file.open( QIODevice::ReadOnly | QIODevice::Text ) )  return false;

   QXmlStreamReader xml( &file );

   calibration.reset();

   while ( ! xml.atEnd() )
   {
      xml.readNext();

      if ( ! xml.isStartElement() )                       continue;
      if ( xml.name().toString() != "Calibration" )       continue;

      QXmlStreamAttributes a = xml.attributes();

      calibration.ID        = a.value( "id"        ).toString().toInt();
      calibration.GUID      = a.value( "guid"      ).toString();
      calibration.rotorID   = a.value( "rotorID"   ).toString().toInt();
      calibration.rotorGUID = a.value( "rotorGUID" ).toString();
      calibration.calibrationExperimentID
                            = a.value( "calExpID"  ).toString().toInt();
      calibration.calibrationExperimentGUID
                            = a.value( "calExpGUID" ).toString();
      calibration.coeff1    = a.value( "coeff1"    ).toString().toDouble();
      calibration.coeff2    = a.value( "coeff2"    ).toString().toDouble();
      calibration.lastUpdated = QDate::fromString(
                                a.value( "lastUpdated" ).toString(),
                                "yyyy-MM-dd" );
      calibration.omega2t   = a.value( "omega2t"   ).toString().toDouble();
      calibration.label     = a.value( "label"     ).toString();
      calibration.report    = QString();

      while ( ! xml.atEnd() )
      {
         xml.readNext();

         if ( xml.isEndElement()  &&  xml.name().toString() == "Calibration" )
            break;

         if ( xml.isStartElement()  &&  xml.name().toString() == "report" )
            calibration.report = xml.readElementText();
      }

      break;
   }

   bool failed = xml.hasError();
   file.close();

   return ! failed;
}

bool US_DataPubRecords::writeCenterpiece(
      const US_AbstractCenterpiece& centerpiece, const QString& filename )
{
   QSaveFile file( filename );

   if ( ! file.open( QIODevice::WriteOnly | QIODevice::Text ) )  return false;

   QXmlStreamWriter xml;
   xml.setDevice( &file );
   xml.setAutoFormatting( true );

   xml.writeStartDocument();
   xml.writeStartElement( "abstractCenterpieces" );
   xml.writeAttribute   ( "version", "2.0" );

   xml.writeStartElement( "abstractCenterpiece" );
   xml.writeAttribute   ( "id",   QString::number( centerpiece.serial_number ) );
   xml.writeAttribute   ( "guid", centerpiece.guid );
   xml.writeAttribute   ( "name", centerpiece.name );
   xml.writeAttribute   ( "materialName", centerpiece.material );
   xml.writeAttribute   ( "channels", QString::number( centerpiece.channels ) );
   xml.writeAttribute   ( "shape",  centerpiece.shape );
   xml.writeAttribute   ( "maxRPM", exact( centerpiece.maxRPM ) );
   xml.writeAttribute   ( "angle",  exact( centerpiece.angle  ) );
   xml.writeAttribute   ( "width",  exact( centerpiece.width  ) );

   int rows = qMin( centerpiece.path_length.size(),
                    centerpiece.bottom_position.size() );

   for ( int ii = 0; ii < rows; ii++ )
   {
      xml.writeStartElement( "row" );
      xml.writeAttribute   ( "pathlen", exact( centerpiece.path_length[ ii ] ) );
      xml.writeAttribute   ( "bottom",
                             exact( centerpiece.bottom_position[ ii ] ) );
      xml.writeEndElement  ();
   }

   xml.writeEndElement  ();          // abstractCenterpiece
   xml.writeEndElement  ();          // abstractCenterpieces
   xml.writeEndDocument ();

   return finish_xml( xml, file );
}

bool US_DataPubRecords::readCenterpiece( const QString& filename,
                                         US_AbstractCenterpiece& centerpiece )
{
   QFile file( filename );

   if ( ! file.open( QIODevice::ReadOnly | QIODevice::Text ) )  return false;

   QXmlStreamReader xml( &file );

   centerpiece = US_AbstractCenterpiece();
   centerpiece.path_length    .clear();
   centerpiece.bottom_position.clear();

   while ( ! xml.atEnd() )
   {
      xml.readNext();

      if ( ! xml.isStartElement() )  continue;

      QString              ename = xml.name().toString();
      QXmlStreamAttributes a     = xml.attributes();

      if ( ename == "abstractCenterpiece" )
      {
         centerpiece.serial_number = a.value( "id"   ).toString().toInt();
         centerpiece.guid          = a.value( "guid" ).toString();
         centerpiece.name          = a.value( "name" ).toString();
         centerpiece.material      = a.value( "materialName" ).toString();
         centerpiece.channels      = a.value( "channels" ).toString().toInt();
         centerpiece.shape         = a.value( "shape"  ).toString();
         centerpiece.maxRPM        = a.value( "maxRPM" ).toString().toDouble();
         centerpiece.angle         = a.value( "angle"  ).toString().toDouble();
         centerpiece.width         = a.value( "width"  ).toString().toDouble();
      }

      else if ( ename == "row" )
      {
         centerpiece.path_length     << a.value( "pathlen" ).toString().toDouble();
         centerpiece.bottom_position << a.value( "bottom"  ).toString().toDouble();
      }
   }

   bool failed = xml.hasError();
   file.close();

   return ! failed;
}

namespace
{
   // Where the user-visible name of a record type lives inside its payload
   class NameLocation
   {
      public:
         NameLocation() : attribute( true ) {}
         QString element;      // The element carrying the name
         QString key;          // The attribute or child element name
         bool    attribute;    // True when the name is an attribute
   };

   NameLocation name_location( US_DataPub::EntityType type )
   {
      NameLocation loc;

      switch ( type )
      {
         case US_DataPub::Project:
            loc.element = "project";      loc.key = "description";
            loc.attribute = false;        break;

         case US_DataPub::Solution:
            loc.element = "solution";     loc.key = "description";
            loc.attribute = false;        break;

         case US_DataPub::Buffer:
            loc.element = "buffer";       loc.key = "description";  break;

         case US_DataPub::Analyte:
            loc.element = "analyte";      loc.key = "description";  break;

         case US_DataPub::Model:
            loc.element = "model";        loc.key = "description";  break;

         case US_DataPub::Noise:
            loc.element = "noise";        loc.key = "description";  break;

         case US_DataPub::RotorCalibration:
            loc.element = "Calibration";  loc.key = "label";        break;

         case US_DataPub::Experiment:
            loc.element = "experiment";   loc.key = "runID";        break;

         default:
            break;
      }

      return loc;
   }
}

QString US_DataPubRecords::recordName( const QString& filename,
                                       US_DataPub::EntityType type )
{
   NameLocation loc = name_location( type );

   if ( loc.element.isEmpty() )  return QString();

   QFile file( filename );

   if ( ! file.open( QIODevice::ReadOnly | QIODevice::Text ) )  return QString();

   QXmlStreamReader xml( &file );
   QString          name;
   bool             inside = false;

   while ( ! xml.atEnd() )
   {
      xml.readNext();

      if ( xml.isEndElement()  &&  xml.name().toString() == loc.element )
         inside = false;

      if ( ! xml.isStartElement() )  continue;

      QString ename = xml.name().toString();

      if ( ename == loc.element )
      {
         inside = true;

         if ( loc.attribute )
         {
            name = xml.attributes().value( loc.key ).toString();
            break;
         }
      }

      else if ( inside  &&  ! loc.attribute  &&  ename == loc.key )
      {
         name = xml.readElementText();
         break;
      }
   }

   file.close();

   return name;
}

bool US_DataPubRecords::setRecordName( const QString& filename,
                                       US_DataPub::EntityType type,
                                       const QString& newName )
{
   NameLocation loc = name_location( type );

   if ( loc.element.isEmpty() )  return false;

   QFile in( filename );

   if ( ! in.open( QIODevice::ReadOnly ) )  return false;

   QByteArray data = in.readAll();
   in.close();

   QXmlStreamReader reader( data );
   QByteArray       out;
   QXmlStreamWriter writer( &out );
   writer.setAutoFormatting( false );

   bool inside  = false;
   bool changed = false;

   while ( ! reader.atEnd() )
   {
      reader.readNext();

      if ( reader.hasError() )  return false;
      if ( reader.tokenType() == QXmlStreamReader::NoToken )   continue;
      if ( reader.tokenType() == QXmlStreamReader::Invalid )   break;

      if ( reader.isEndElement()  &&
           reader.name().toString() == loc.element )
         inside = false;

      if ( reader.isStartElement() )
      {
         QString ename = reader.name().toString();

         if ( ename == loc.element )
         {
            inside = true;

            if ( loc.attribute )
            {
               QXmlStreamAttributes attrs = reader.attributes();
               writer.writeStartElement( ename );

               bool written = false;

               for ( int ii = 0; ii < attrs.size(); ii++ )
               {
                  QString aname = attrs.at( ii ).name().toString();

                  if ( aname == loc.key )
                  {
                     writer.writeAttribute( aname, newName );
                     written = true;
                     changed = true;
                  }

                  else
                     writer.writeAttribute( aname,
                                            attrs.at( ii ).value().toString() );
               }

               if ( ! written )
               {
                  writer.writeAttribute( loc.key, newName );
                  changed = true;
               }

               continue;
            }
         }

         else if ( inside  &&  ! loc.attribute  &&  ename == loc.key )
         {
            reader.readElementText();        // Consume up to the end element
            writer.writeTextElement( ename, newName );
            changed = true;
            continue;
         }
      }

      writer.writeCurrentToken( reader );
   }

   if ( ! changed )  return false;

   QFile outFile( filename );

   if ( ! outFile.open( QIODevice::WriteOnly | QIODevice::Truncate ) )
      return false;

   outFile.write( out );
   outFile.close();

   return true;
}

// ------------------------------------------------------- buffer components

bool US_DataPubRecords::writeBufferComponents(
      const QList< US_BufferComponent >& components, const QString& filename )
{
   QSaveFile file( filename );

   if ( ! file.open( QIODevice::WriteOnly ) )  return false;

   QXmlStreamWriter xml( &file );
   xml.setAutoFormatting( true );

   xml.writeStartDocument();
   xml.writeDTD         ( "<!DOCTYPE US_BufferComponents>" );
   xml.writeStartElement( "BufferComponents" );
   xml.writeAttribute   ( "version", "1.0" );

   for ( int ii = 0; ii < components.size(); ii++ )
   {
      const US_BufferComponent& comp = components[ ii ];

      xml.writeStartElement( "component" );
      xml.writeAttribute( "id",    comp.componentID );
      xml.writeAttribute( "name",  comp.name );
      xml.writeAttribute( "unit",  comp.unit );
      xml.writeAttribute( "range", comp.range );
      xml.writeAttribute( "gradient_forming",
                          comp.grad_form ? QString( "1" ) : QString( "0" ) );

      xml.writeStartElement( "densityCoefficients" );

      for ( int jj = 0; jj < 6; jj++ )
         xml.writeAttribute( QString( "c%1" ).arg( jj ),
                             QString::number( comp.dens_coeff[ jj ], 'g', 17 ) );

      xml.writeEndElement();      // densityCoefficients

      xml.writeStartElement( "viscosityCoefficients" );

      for ( int jj = 0; jj < 6; jj++ )
         xml.writeAttribute( QString( "c%1" ).arg( jj ),
                             QString::number( comp.visc_coeff[ jj ], 'g', 17 ) );

      xml.writeEndElement();      // viscosityCoefficients
      xml.writeEndElement();      // component
   }

   xml.writeEndElement();         // BufferComponents
   xml.writeEndDocument();

   if ( xml.hasError() )
   {
      file.cancelWriting();
      return false;
   }

   return file.commit();
}

bool US_DataPubRecords::readBufferComponents(
      const QString& filename, QMap< QString, US_BufferComponent >& components )
{
   components.clear();

   QFile file( filename );

   if ( ! file.open( QIODevice::ReadOnly | QIODevice::Text ) )  return false;

   QXmlStreamReader   xml( &file );
   US_BufferComponent comp;
   bool               inComp = false;

   while ( ! xml.atEnd() )
   {
      xml.readNext();

      QString ename = xml.name().toString();

      if ( xml.isEndElement()  &&  ename == "component"  &&  inComp )
      {
         components.insert( comp.componentID, comp );
         inComp = false;
         continue;
      }

      if ( ! xml.isStartElement() )  continue;

      QXmlStreamAttributes attrs = xml.attributes();

      if ( ename == "component" )
      {
         comp             = US_BufferComponent();
         comp.componentID = attrs.value( "id"    ).toString();
         comp.name        = attrs.value( "name"  ).toString();
         comp.unit        = attrs.value( "unit"  ).toString();
         comp.range       = attrs.value( "range" ).toString();
         QString grad     = attrs.value( "gradient_forming" ).toString();
         comp.grad_form   = ( grad == "1"  ||  grad.toLower() == "true" );
         inComp           = true;
      }

      else if ( inComp  &&  ( ename == "densityCoefficients"  ||
                              ename == "viscosityCoefficients" ) )
      {
         double* coeffs = ( ename == "densityCoefficients" )
                          ? comp.dens_coeff : comp.visc_coeff;

         for ( int jj = 0; jj < 6; jj++ )
            coeffs[ jj ] = attrs.value( QString( "c%1" ).arg( jj ) )
                           .toString().toDouble();
      }
   }

   bool failed = xml.hasError();
   file.close();

   if ( failed )  components.clear();

   return ! failed;
}
