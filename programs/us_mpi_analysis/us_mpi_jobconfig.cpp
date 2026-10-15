//! \file us_mpi_jobconfig.cpp
#include "us_mpi_jobconfig.h"
#include "us_math2.h"

#include <cmath>

namespace
{
double value_attr( const QXmlStreamAttributes& attr, const char* name )
{
   return attr.value( name ).toString().toDouble();
}

// Round to 7 digits of the larger magnitude, to forestall round-off
//  problems.  Limits may be zero or negative (flotation s).
double rounded( double value, double vmin, double vmax )
{
   double vmag = qMax( qAbs( vmin ), qAbs( vmax ) );
   double vinc = ( vmag > 0.0 ) ? pow( 10.0, qFloor( log10( vmag ) ) - 6 ) : 1.0;
   return qRound64( value / vinc ) * vinc;
}
} // namespace

US_MPI_JobConfig US_MPI_JobConfig::fromFile( const QString& path )
{
   QFile file( path );

   if ( !file.open( QIODevice::ReadOnly | QIODevice::Text ) )
   {
      US_MPI_JobConfig config;
      config.errors << "Cannot open file " + path;
      return config;
   }

   return fromXml( file, path );
}

US_MPI_JobConfig US_MPI_JobConfig::fromXml( QIODevice& device, const QString& source )
{
   US_MPI_JobConfig config;
   QXmlStreamReader xml( &device );

   while ( !xml.atEnd() )
   {
      xml.readNext();

      if ( !xml.isStartElement() )
      {
         continue;
      }

      if ( xml.name() == QLatin1String( "US_JobSubmit" ) )
      {
         config.method = xml.attributes().value( "method" ).toString();
      }

      else if ( xml.name() == QLatin1String( "job" ) )
      {
         config.read_job( xml );
      }

      else if ( xml.name() == QLatin1String( "dataset" ) )
      {
         DatasetSpec spec;
         config.read_dataset( xml, spec );
         config.datasets << spec;
      }
   }

   if ( xml.hasError() )
   {
      config.errors << QString( "XML error in %1, line %2 column %3: %4" )
                          .arg( source )
                          .arg( xml.lineNumber() )
                          .arg( xml.columnNumber() )
                          .arg( xml.errorString() );
   }

   return config;
}

void US_MPI_JobConfig::read_job( QXmlStreamReader& xml )
{
   bool read_any = false;

   while ( !xml.atEnd() )
   {
      xml.readNext();

      if ( xml.isEndElement() && xml.name() == QLatin1String( "job" ) )
      {
         break;
      }

      read_any = true;

      if ( !xml.isStartElement() )
      {
         continue;
      }

      QXmlStreamAttributes attr  = xml.attributes();
      QString              xname = xml.name().toString();

      if ( xname == "cluster" )
      {
         cluster = attr.value( "shortname" ).toString();
      }

      else if ( xname == "name" )
      {
         db_name = attr.value( "value" ).toString();
      }

      else if ( xname == "udp" )
      {
         udp_server = attr.value( "server" ).toString();
         udp_port   = (quint16) attr.value( "port" ).toString().toInt();
      }

      else if ( xname == "global_fit" )
      {
         parameters[xname] = attr.value( "value" ).toString();
      }

      else if ( xname == "request" )
      {
         request_id   = QString::asprintf( "%06d", attr.value( "id" ).toString().toInt() );
         request_guid = attr.value( "guid" ).toString();
      }

      else if ( xname == "jobParameters" )
      {
         read_job_parameters( xml );
      }
   }

   // An empty job element leaves z_value unset, as before
   if ( read_any && !parameters.contains( "z_value" ) )
   {
      parameters["z_value"] = "0.0";
   }
}

void US_MPI_JobConfig::read_job_parameters( QXmlStreamReader& xml )
{
   QString ytype; // Set by bucket_fixed or by the first bucket

   while ( !xml.atEnd() )
   {
      xml.readNext();
      QString name = xml.name().toString();

      if ( xml.isEndElement() && name == "jobParameters" )
      {
         break;
      }

      if ( !xml.isStartElement() )
      {
         continue;
      }

      QXmlStreamAttributes attr = xml.attributes();

      if ( name == "bucket" )
      {
         read_bucket( attr, ytype );
      }

      else if ( name == "CG_model" || name == "DC_model" )
      {
         parameters[name] = attr.value( "filename" ).toString();
      }

      else if ( name == "bucket_fixed" )
      {
         parameters[name]    = attr.value( "value" ).toString();
         parameters["xtype"] = attr.value( "xtype" ).toString();
         parameters["ytype"] = attr.value( "ytype" ).toString();
         parameters["ztype"] = attr.value( "fixedtype" ).toString();
         ytype               = parameters.value( "ytype" );
      }

      else if ( name.startsWith( "s_m" ) || name.startsWith( "ff0_m" ) )
      { // Also stored as the generic x/y limit
         QString svalu    = attr.value( "value" ).toString();
         parameters[name] = svalu;
         name.replace( "s_m", "x_m" );
         name.replace( "ff0_m", "y_m" );
         parameters[name] = svalu;
      }

      else
      {
         parameters[name] = attr.value( "value" ).toString();
      }
   }
}

void US_MPI_JobConfig::read_bucket( const QXmlStreamAttributes& attr, QString& ytype )
{
   QString ytyp0 = "ff0";
   QString stxmx = attr.value( "x_max" ).toString();
   QString stfmx = attr.value( "ff0_max" ).toString();
   QString stvmx = attr.value( "vbar_max" ).toString();
   double  xmin  = value_attr( attr, "x_min" );
   double  xmax  = stxmx.toDouble();
   double  ymin  = value_attr( attr, "y_min" );
   double  ymax  = value_attr( attr, "y_max" );

   if ( stxmx.isEmpty() )
   { // s and f/f0 or vbar limits
      xmin = value_attr( attr, "s_min" );
      xmax = value_attr( attr, "s_max" );

      if ( !stfmx.isEmpty() )
      {
         ymin = value_attr( attr, "ff0_min" );
         ymax = stfmx.toDouble();
      }

      else if ( !stvmx.isEmpty() )
      {
         ymin  = value_attr( attr, "vbar_min" );
         ymax  = stvmx.toDouble();
         ytyp0 = "vbar";
      }
   }

   if ( ytype.isEmpty() )
   {
      ytype               = ytyp0;
      parameters["ytype"] = ytype;
   }

   Bucket bucket;
   bucket.x_min = rounded( xmin, xmin, xmax );
   bucket.x_max = rounded( xmax, xmin, xmax );
   bucket.y_min = rounded( ymin, ymin, ymax );
   bucket.y_max = rounded( ymax, ymin, ymax );
   buckets << bucket;
}

void US_MPI_JobConfig::read_dataset( QXmlStreamReader& xml, DatasetSpec& spec )
{
   const QStringList shapes = { "sector", "standard", "rectangular", "band-forming", "meniscus-matching", "circular", "synthetic" };

   while ( !xml.atEnd() )
   {
      xml.readNext();

      if ( xml.isEndElement() && xml.name() == QLatin1String( "dataset" ) )
      {
         return;
      }

      if ( !xml.isStartElement() )
      {
         continue;
      }

      QString              name = xml.name().toString();
      QXmlStreamAttributes attr = xml.attributes();
      QString              sval = attr.value( "value" ).toString();

      if ( name == "files" )
      {
         read_files( xml, spec );
      }
      else if ( name == "solution" )
      {
         read_solution( xml, spec );
      }
      else if ( name == "simpoints" )
      {
         spec.simpoints = sval.toInt();
      }
      else if ( name == "band_volume" )
      {
         spec.band_volume = sval.toDouble();
      }
      else if ( name == "radial_grid" )
      {
         spec.mesh_type = sval.toInt();
      }
      else if ( name == "time_grid" )
      {
         spec.grid_type = sval.toInt();
      }
      else if ( name == "density" )
      {
         spec.density = sval.toDouble();
      }
      else if ( name == "viscosity" )
      {
         spec.viscosity = sval.toDouble();
      }
      else if ( name == "compress" )
      {
         spec.compress = sval.toDouble();
      }
      else if ( name == "rotor_stretch" )
      {
         QStringList stretch = sval.split( " ", Qt::SkipEmptyParts );

         if ( stretch.size() < 2 )
         {
            errors << "Invalid rotor_stretch value \"" + sval + "\": two coefficients expected";
         }
         else
         {
            spec.rotor_stretch[0] = stretch[0].toDouble();
            spec.rotor_stretch[1] = stretch[1].toDouble();
         }
      }
      else if ( name == "centerpiece_bottom" )
      {
         spec.centerpiece_bottom = sval.toDouble();
      }
      else if ( name == "centerpiece_shape" )
      {
         spec.cp_sector = qMax( 0, shapes.indexOf( sval ) );
      }
      else if ( name == "centerpiece_angle" )
      {
         spec.cp_angle = sval.toDouble();
      }
      else if ( name == "centerpiece_pathlength" )
      {
         spec.cp_pathlen = sval.toDouble();
      }
      else if ( name == "centerpiece_width" )
      {
         spec.cp_width = sval.toDouble();
      }
      else if ( name == "speedstep" )
      {
         US_SimulationParameters::SpeedProfile sp;
         US_SimulationParameters::speedstepFromXml( xml, sp );
         spec.speed_steps << sp;
      }
      else if ( name == "total_concentration" )
      {
         spec.total_concentrations << sval.toDouble();
      }
   }
}

void US_MPI_JobConfig::read_files( QXmlStreamReader& xml, DatasetSpec& spec )
{
   while ( !xml.atEnd() )
   {
      xml.readNext();

      if ( xml.isEndElement() && xml.name() == QLatin1String( "files" ) )
      {
         break;
      }

      if ( !xml.isStartElement() )
      {
         continue;
      }

      QString type     = xml.name().toString();
      QString filename = xml.attributes().value( "filename" ).toString();

      if ( type == "auc" )
      {
         spec.auc_file = filename;
      }
      else if ( type == "edit" )
      {
         spec.edit_file = filename;
      }
      else if ( type == "noise" )
      {
         spec.noise_files << filename;
      }
      else if ( type == "timestate" )
      {
         spec.tmst_file = filename;
      }
   }
}

void US_MPI_JobConfig::read_solution( QXmlStreamReader& xml, DatasetSpec& spec )
{
   spec.has_solution = true;

   while ( !xml.atEnd() )
   {
      xml.readNext();

      if ( xml.isEndElement() && xml.name() == QLatin1String( "solution" ) )
      {
         break;
      }

      if ( !xml.isStartElement() )
      {
         continue;
      }

      QXmlStreamAttributes attr = xml.attributes();

      if ( xml.name() == QLatin1String( "buffer" ) )
      {
         spec.density   = value_attr( attr, "density" );
         spec.viscosity = value_attr( attr, "viscosity" );
         spec.compress  = value_attr( attr, "compress" );
         spec.manual    = attr.value( "manual" ).toString().toInt() != 0;
      }

      else if ( xml.name() == QLatin1String( "analyte" ) )
      {
         US_Solution::AnalyteInfo aninfo;
         aninfo.analyte.mw     = value_attr( attr, "mw" );
         aninfo.analyte.vbar20 = value_attr( attr, "vbar20" );
         aninfo.amount         = value_attr( attr, "amount" );
         QString atype         = attr.value( "type" ).toString();
         aninfo.analyte.type   = US_Analyte::PROTEIN;

         if ( atype == "DNA" )
         {
            aninfo.analyte.type = US_Analyte::DNA;
         }
         else if ( atype == "RNA" )
         {
            aninfo.analyte.type = US_Analyte::RNA;
         }
         else if ( atype == "Other" )
         {
            aninfo.analyte.type = US_Analyte::CARBOHYDRATE;
         }

         spec.analytes << aninfo;
      }
   }
}

QString US_MPI_JobConfig::DatasetSpec::edit_file_name() const
{
   QString clambda = edit_file.section( ".", -2, -2 );

   if ( !clambda.contains( "-" ) )
   {
      return edit_file;
   }

   // MWL: "<run>.<edit>.<triple>.<lambda1-lambda2>.xml" becomes
   //  "<run>.<edit>.<triple>.<lambda1-lambda2>@<lambda>.xml"
   clambda        = clambda + "@" + auc_file.section( ".", -2, -2 );
   QString fpart1 = edit_file.section( ".", 0, -3 );
   QString fpart2 = edit_file.section( ".", -1, -1 );
   return fpart1 + "." + clambda + "." + fpart2;
}

int US_MPI_JobConfig::debug_level() const
{
   return parameters.value( "debug_level", "0" ).toInt();
}

bool US_MPI_JobConfig::debug_timings() const
{
   return parameters.value( "debug_timings", "0" ).toInt() != 0;
}

QStringList US_MPI_JobConfig::debug_text() const
{
   QStringList entries;

   if ( parameters.contains( "debug_text" ) )
   {
      for ( const QString& entry : parameters.value( "debug_text" ).split( ',' ) )
      {
         entries << entry.simplified();
      }
   }

   return entries;
}

QList<US_SolveSim::DataSet*> US_MPI_JobConfig::make_datasets() const
{
   QList<US_SolveSim::DataSet*> dsets;
   QString                      tmst_prev; // Timestate of the previous dataset

   for ( const DatasetSpec& spec : datasets )
   {
      US_SolveSim::DataSet*    ds = new US_SolveSim::DataSet();
      US_SimulationParameters& sp = ds->simparams;

      ds->auc_file    = spec.auc_file;
      ds->edit_file   = spec.edit_file_name();
      ds->tmst_file   = spec.tmst_file.value_or( tmst_prev );
      tmst_prev       = ds->tmst_file;
      ds->noise_files = spec.noise_files;
      sp.speed_step   = spec.speed_steps;

      // Values the file does not give keep their defaults
      auto set = []( auto& target, const auto& value )
      {
         if ( value )
         {
            target = *value;
         }
      };
      set( ds->density, spec.density );
      set( ds->viscosity, spec.viscosity );
      set( ds->compress, spec.compress );
      set( ds->manual, spec.manual );
      set( ds->rotor_stretch[0], spec.rotor_stretch[0] );
      set( ds->rotor_stretch[1], spec.rotor_stretch[1] );
      set( ds->centerpiece_bottom, spec.centerpiece_bottom );
      set( sp.simpoints, spec.simpoints );
      set( sp.band_volume, spec.band_volume );
      set( sp.cp_sector, spec.cp_sector );
      set( sp.cp_angle, spec.cp_angle );
      set( sp.cp_pathlen, spec.cp_pathlen );
      set( sp.cp_width, spec.cp_width );

      if ( spec.mesh_type )
      {
         sp.meshType = (US_SimulationParameters::MeshType) *spec.mesh_type;
      }
      if ( spec.grid_type )
      {
         sp.gridType = (US_SimulationParameters::GridType) *spec.grid_type;
      }

      ds->solution_rec.analyteInfo = spec.analytes;
      if ( spec.has_solution )
      {
         ds->vbar20 = US_Math2::calcCommonVbar( ds->solution_rec, 20.0 );
      }

      dsets << ds;
   }

   return dsets;
}
