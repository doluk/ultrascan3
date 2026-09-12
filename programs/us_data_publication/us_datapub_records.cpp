//! \file us_datapub_records.cpp
#include "us_datapub_records.h"

bool US_DataPubRecords::writeCalibration(
      const US_Rotor::RotorCalibration& calibration, const QString& filename )
{
   QFile file( filename );

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
   xml.writeAttribute   ( "coeff1",     QString::number( calibration.coeff1 ) );
   xml.writeAttribute   ( "coeff2",     QString::number( calibration.coeff2 ) );
   xml.writeAttribute   ( "lastUpdated",
                          calibration.lastUpdated.toString( "yyyy-MM-dd" ) );
   xml.writeAttribute   ( "omega2t",    QString::number( calibration.omega2t ) );
   xml.writeAttribute   ( "label",      calibration.label );
   xml.writeTextElement ( "report",     calibration.report );
   xml.writeEndElement  ();          // Calibration

   xml.writeEndElement  ();          // RotorCalibrationData
   xml.writeEndDocument ();

   file.close();

   return true;
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
   QFile file( filename );

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
   xml.writeAttribute   ( "maxRPM", QString::number( centerpiece.maxRPM ) );
   xml.writeAttribute   ( "angle",  QString::number( centerpiece.angle  ) );
   xml.writeAttribute   ( "width",  QString::number( centerpiece.width  ) );

   int rows = qMin( centerpiece.path_length.size(),
                    centerpiece.bottom_position.size() );

   for ( int ii = 0; ii < rows; ii++ )
   {
      xml.writeStartElement( "row" );
      xml.writeAttribute   ( "pathlen",
                             QString::number( centerpiece.path_length[ ii ] ) );
      xml.writeAttribute   ( "bottom",
                             QString::number( centerpiece.bottom_position[ ii ] ) );
      xml.writeEndElement  ();
   }

   xml.writeEndElement  ();          // abstractCenterpiece
   xml.writeEndElement  ();          // abstractCenterpieces
   xml.writeEndDocument ();

   file.close();

   return true;
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
