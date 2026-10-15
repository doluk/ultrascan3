#include "mpi.h"
#include "us_math2.h"
#include "us_mpi_analysis.h"
#include "us_mpi_jobconfig.h"
#include "us_settings.h"

//  Parse XML routines
void US_MPI_Analysis::parse( const QString& xmlfile )
{
   QFile file ( xmlfile );

   if ( ! file.open( QIODevice::ReadOnly | QIODevice::Text) )
   {  // The udp server is not known yet, so the message goes to stdout only
      abort( "Cannot open file " + xmlfile );
   }

   QString tmst_fn;
   QXmlStreamReader xml( &file );

   while ( ! xml.atEnd() )
   {
      xml.readNext();

      if ( xml.isStartElement() )
      {
         if ( xml.name() == "US_JobSubmit" )
         {
            QXmlStreamAttributes a = xml.attributes();
            analysis_type = a.value( "method" ).toString();
         }

         if ( xml.name() == "job" )
            parse_job( xml );

         if ( xml.name() == "dataset" )
         {
            US_SolveSim::DataSet* d = new US_SolveSim::DataSet;
            d->tmst_file   = tmst_fn;       // Initialize to blank or previous
            parse_dataset( xml, d );
            tmst_fn        = d->tmst_file;  // Init for subsequent dataset

            if ( parameters.contains( "CG_model" ) )
            {  // Flag Custom Grid model input
               d->model_file  = parameters.value( "CG_model" );
               d->solute_type = 2;
               attr_x         = ATTR_S;
               attr_y         = ATTR_V;
               attr_z         = ATTR_K;
            }

            else if ( parameters.contains( "DC_model" ) )
            {  // Flag DMGA_Constraints model input
               d->model_file  = parameters.value( "DC_model" );
            }

            else if ( ! parameters.value( "ztype" ).isEmpty() )
            {  // Flag generic x,y model with fixed "z"
               QStringList s_attrs;
               s_attrs << "s" << "ff0" << "MW" << "vbar" << "D" << "f";
               attr_x         = s_attrs.indexOf( parameters.value( "xtype" ) );
               attr_y         = s_attrs.indexOf( parameters.value( "ytype" ) );
               attr_z         = s_attrs.indexOf( parameters.value( "ztype" ) );
               attr_x         = ( attr_x < 0 ) ? ATTR_S : attr_x;
               attr_y         = ( attr_y < 0 ) ? ATTR_K : attr_y;
               attr_z         = ( attr_z < 0 ) ? ATTR_V : attr_z;
               // The solute type flag is a number which when viewed as
               //  octal is in the form "01xyz".
               d->solute_type = ( attr_x << 6 ) + ( attr_y << 3 ) + attr_z;
            }

            else if ( parameters.value( "ytype" ) == "vbar" )
            {  // Flag s,vbar model with fixed f_f0
               d->solute_type = 1;
               attr_x         = ATTR_S;
               attr_y         = ATTR_V;
               attr_z         = ATTR_K;
            }

            else if ( ! parameters.value( "solute_type" ).isEmpty() )
            {  // Flag explicit solute type
               QString s_styp = parameters.value( "solute_type" );
               d->solute_type = US_ModelRecord::stype_flag( s_styp );
               attr_x         = ( d->solute_type >> 6 ) & 7;
               attr_y         = ( d->solute_type >> 3 ) & 7;
               attr_z         =   d->solute_type        & 7;
            }

            else
            {  // Flag standard s,k model with fixed vbar
               d->solute_type = 0;
               attr_x         = ATTR_S;
               attr_y         = ATTR_K;
               attr_z         = ATTR_V;
//d->solute_type = (0 << 6) + (1 << 3) + 3;
            }

if (my_rank==0) DbgLv(0) << "PF: solute_type" << d->solute_type
 << "attr_x _y _z" << attr_x << attr_y << attr_z;
            data_sets << d;
         }
      }
   }

   if ( xml.hasError() )
   {  // A malformed or truncated file: do not run with what was read
      abort( QString( "XML error in %1, line %2 column %3: %4" )
             .arg( xmlfile ).arg( xml.lineNumber() )
             .arg( xml.columnNumber() ).arg( xml.errorString() ) );
   }

   file.close();

   if ( parameters.contains( "debug_text" ) )
   {
      QString dbgtxt_str     = parameters.value( "debug_text" );
      QStringList dbgtxt_sls = dbgtxt_str.split( ',' );
      int ndtln              = dbgtxt_sls.count();

      for ( int ii = 0; ii < ndtln; ii++ )
      {
         dbgtxt_sls[ ii ] = dbgtxt_sls[ ii ].simplified();
      }

      US_Settings::set_debug_text( dbgtxt_sls );
//*DEBUG*
if (my_rank==0) {
DbgLv(0) << "DBGTXT: dt string: " << dbgtxt_str;
DbgLv(0) << "DBGTXT: dt sl_count" << ndtln;
DbgLv(0) << "DBGTXT: dt strlst: " << dbgtxt_sls;
dbgtxt_sls = US_Settings::debug_text();
ndtln = dbgtxt_sls.count();
DbgLv(0) << "DBGTXT: dt sl_count(2)" << ndtln;
DbgLv(0) << "DBGTXT: dt strlst(2): " << dbgtxt_sls;
}
//*DEBUG*
   }
}

void US_MPI_Analysis::parse_job( QXmlStreamReader& xml )
{
   QXmlStreamAttributes attr;
   QString xname;
   QString ytype;

   while ( ! xml.atEnd() )
   {
      xml.readNext();
      xname       = xml.name().toString();

      if ( xml.isEndElement()  &&  xname == "job" ) break;

      if ( xml.isStartElement() )
      {
         attr        = xml.attributes();

         if ( xname == "cluster" )
         {
            cluster     = attr.value( "shortname" ).toString();
         }

         else if ( xname == "name" )
         {
            db_name     = attr.value( "value" ).toString();
         }

         else if ( xname == "udp" )
         {
            server     = QHostAddress( attr.value( "server" ).toString() );
            port       = (quint16)attr.value( "port" ).toString().toInt();
         }

         else if ( xname == "global_fit" )
         {
            parameters[ xname ] = attr.value( "value" ).toString();
         }

         else if ( xname == "request" )
         {
            requestID   = QString::asprintf( "%06d",
                                             attr.value( "id" ).toString().toInt() );
            requestGUID = attr.value( "guid" ).toString();
         }

         else if ( xname == "jobParameters" )
         {
            while ( ! xml.atEnd() )
            {
               xml.readNext();
               QString name = xml.name().toString();

               if ( xml.isEndElement()  &&  name == "jobParameters" ) break;

               if ( xml.isStartElement() )
               {
                  attr    = xml.attributes();

                  if ( name == "bucket" )
                  { // Get bucket coords; try to forestall round-off problems
                     QString ytyp0 = QString( "ff0" );
                     Bucket b;
                     QString stxmn = attr.value( "x_min"    ).toString();
                     QString stxmx = attr.value( "x_max"    ).toString();
                     QString stymn = attr.value( "y_min"    ).toString();
                     QString stymx = attr.value( "y_max"    ).toString();
                     QString stfmn = attr.value( "ff0_min"  ).toString();
                     QString stfmx = attr.value( "ff0_max"  ).toString();
                     QString stvmn = attr.value( "vbar_min" ).toString();
                     QString stvmx = attr.value( "vbar_max" ).toString();
                     double xmin   = stxmn.toDouble();
                     double xmax   = stxmx.toDouble();
                     double ymin   = stymn.toDouble();
                     double ymax   = stymx.toDouble();

                     if ( stxmx.isEmpty() )
                     {
                        xmin       = attr.value( "s_min").toString().toDouble();
                        xmax       = attr.value( "s_max").toString().toDouble();
                        if ( !stfmx.isEmpty() )
                        {
                           ymin    = stfmn.toDouble();
                           ymax    = stfmx.toDouble();
                           ytyp0   = QString( "ff0" );
                        }
                        else if ( !stvmx.isEmpty() )
                        {
                           ymin    = stvmn.toDouble();
                           ymax    = stvmx.toDouble();
                           ytyp0   = QString( "vbar" );
                        }
                     }

                     if ( ytype.isEmpty() )
                     {
                        ytype      = ytyp0;
                        parameters[ "ytype" ]     = ytype;
                     }

                     // Try to forestall round-off problems: round to 7
                     //  digits of the larger magnitude.  Limits may be zero
                     //  or negative (flotation s), so use the magnitude.
if (my_rank==0) DbgLv(0) << "PF: xymnmx" << xmin << xmax << ymin << ymax;
                     auto round_inc = []( double vmin, double vmax )
                     {
                        double vmag = qMax( qAbs( vmin ), qAbs( vmax ) );
                        return ( vmag > 0.0 )
                               ? pow( 10.0, qFloor( log10( vmag ) ) - 6 )
                               : 1.0;
                     };
                     double xinc   = round_inc( xmin, xmax );
                     double yinc   = round_inc( ymin, ymax );
if (my_rank==0) DbgLv(0) << "PF:  xi yi" << xinc << yinc;
                     b.x_min       = qRound64( xmin / xinc ) * xinc;
                     b.x_max       = qRound64( xmax / xinc ) * xinc;
                     b.y_min       = qRound64( ymin / yinc ) * yinc;
                     b.y_max       = qRound64( ymax / yinc ) * yinc;
if (my_rank==0) DbgLv(0) << "PF:   rnd xymnmx" << b.x_min << b.x_max << b.y_min << b.y_max;

                     buckets << b;
                  }
                  else if ( name == "CG_model"  ||  name == "DC_model" )
                  {
                     parameters[ name ]    = attr.value( "filename").toString();
if (my_rank==0) DbgLv(0) << "PF:   DC_model" << parameters.value( name ) << name;
                  }
                  else if ( name == "bucket_fixed" )
                  {
                     parameters[ name ]    = attr.value( "value" ).toString();
                     parameters[ "xtype" ] = attr.value( "xtype" ).toString();
                     parameters[ "ytype" ] = attr.value( "ytype" ).toString();
                     parameters[ "ztype" ] = attr.value("fixedtype").toString();
                     ytype                 = parameters.value( "ytype" );
                  }

                  else if ( name.startsWith( "s_m" )  ||
                            name.startsWith( "ff0_m" ) )
                  {
                     QString svalu = attr.value( "value" ).toString();
                     parameters[ name ]    = svalu;
                     name          = name.replace( "s_m",   "x_m" );
                     name          = name.replace( "ff0_m", "y_m" );
                     parameters[ name ]    = svalu;
                  }

                  else
                  {
                     parameters[ name ]    = attr.value( "value" ).toString();
                  }
               }
            }
         }
      }

      if ( ! parameters.contains( "z_value" ) )
         parameters[ "z_value" ]     = "0.0";

      if ( parameters.contains( "debug_level" ) )
         dbg_level  = parameters.value( "debug_level" ).toInt();

      else
         dbg_level  = 0;

//      US_Settings::set_us_debug( dbg_level );
      int dbglv = ( my_rank < 2 || my_rank == 15 ) ? dbg_level : 0;
      US_Settings::set_us_debug( dbglv );
      dbg_timing = ( parameters.contains( "debug_timings" )
                 &&  parameters.value( "debug_timings" ).toInt() != 0 );
   }
}

void US_MPI_Analysis::parse_dataset( QXmlStreamReader& xml, DATASET* dataset )
{
   dataset->simparams.speed_step.clear();

   QXmlStreamAttributes a;

   while ( ! xml.atEnd() )
   {
      xml.readNext();
      QString              name = xml.name().toString();

      if ( xml.isEndElement()  &&  xml.name() == "dataset" ) return;

      if ( ! xml.isStartElement() )  continue;

      if ( xml.name() == "files" )
         parse_files( xml, dataset ); 

      if ( xml.name() == "solution" )
         parse_solution( xml, dataset ); 

      a                  = xml.attributes();
      if ( xml.name() == "simpoints" )
      {
         dataset->simparams.simpoints
                            = a.value( "value" ).toString().toInt();
      }

      if ( xml.name() == "band_volume" )
      {
         dataset->simparams.band_volume
                            = a.value( "value" ).toString().toDouble();
      }

      if ( xml.name() == "radial_grid" )
      {
         dataset->simparams.meshType = (US_SimulationParameters::MeshType)
                              a.value( "value" ).toString().toInt();
      }

      if ( xml.name() == "time_grid" )
      {
         dataset->simparams.gridType = (US_SimulationParameters::GridType)
                              a.value( "value" ).toString().toInt();
      }

      if ( xml.name() == "density" )
      {
         dataset->density     = a.value( "value" ).toString().toDouble();
      }

      if ( xml.name() == "viscosity" )
      {
         dataset->viscosity   = a.value( "value" ).toString().toDouble();
      }

      if ( xml.name() == "compress" )
      {
         dataset->compress    = a.value( "value" ).toString().toDouble();
      }

      if ( xml.name() == "rotor_stretch" )
      {
         QString     svalue   = a.value( "value" ).toString();
         QStringList stretch  = svalue.split( " ", Qt::SkipEmptyParts );

         if ( stretch.size() < 2 )
            abort( "Invalid rotor_stretch value \"" + svalue
                   + "\": two coefficients expected" );

         dataset->rotor_stretch[ 0 ] = stretch[ 0 ].toDouble();
         dataset->rotor_stretch[ 1 ] = stretch[ 1 ].toDouble();
      }

      if ( xml.name() == "centerpiece_bottom" )
      {
         dataset->centerpiece_bottom
                                = a.value( "value" ).toString().toDouble();
      }

      if ( xml.name() == "centerpiece_shape" )
      {
         QString shape          = a.value( "value" ).toString();
         QStringList shapes;
         shapes << "sector"
                << "standard"
                << "rectangular"
                << "band-forming"
                << "meniscus-matching"
                << "circular"
                << "synthetic";
         dataset->simparams.cp_sector
                                = qMax( 0, shapes.indexOf( shape ) );
      }

      if ( xml.name() == "centerpiece_angle" )
      {
         dataset->simparams.cp_angle
                                = a.value( "value" ).toString().toDouble();
      }

      if ( xml.name() == "centerpiece_pathlength" )
      {
         dataset->simparams.cp_pathlen
                                = a.value( "value" ).toString().toDouble();
      }

      if ( xml.name() == "centerpiece_width" )
      {
         dataset->simparams.cp_width
                                = a.value( "value" ).toString().toDouble();
      }

      if ( xml.name() == "speedstep" )
      {
         US_SimulationParameters::SpeedProfile sp;

         US_SimulationParameters::speedstepFromXml( xml, sp );

         dataset->simparams.speed_step << sp;
      }

      if ( xml.name() == "total_concentration" )
      {
         concentrations << a.value( "value" ).toString().toDouble();
      }
   }
}

void US_MPI_Analysis::parse_files( QXmlStreamReader& xml, DATASET* dataset )
{
   while ( ! xml.atEnd() )
   {
      xml.readNext();

      if ( xml.isEndElement()  &&  xml.name() == "files" )  break;

      if ( xml.isStartElement() )
      {
         QXmlStreamAttributes a        = xml.attributes();
         QString              type     = xml.name().toString();
         QString              filename = a.value( "filename" ).toString();

         if ( type == "auc"   )      dataset->auc_file   = filename;
         if ( type == "edit"  )      dataset->edit_file  = filename;
         if ( type == "noise" )      dataset->noise_files << filename;
         if ( type == "timestate" )  dataset->tmst_file  = filename;
      }
   }

   QString clambda = dataset->edit_file.section( ".", -2, -2 );
   if ( clambda.contains( "-" ) )
   {  // Modify edit file name for MWL case
      clambda         = clambda + "@" + dataset->auc_file.section( ".", -2, -2 );
      QString fpart1  = dataset->edit_file.section( ".",  0, -3 );
      QString fpart2  = dataset->edit_file.section( ".", -1, -1 );
      dataset->edit_file = fpart1 + "." + clambda + "." + fpart2;
if (my_rank==0) DbgLv(0) << "PF: MWL edit_file" << dataset->edit_file;
   }
}

void US_MPI_Analysis::parse_solution( QXmlStreamReader& xml, DATASET* dataset )
{
   while ( ! xml.atEnd() )
   {
      xml.readNext();

      if ( xml.isEndElement()  &&  xml.name() == "solution" ) break;

      if ( xml.isStartElement() && xml.name() == "buffer" )
      {
         QXmlStreamAttributes a        = xml.attributes();
         dataset->density   = a.value( "density"   ).toString().toDouble();
         dataset->viscosity = a.value( "viscosity" ).toString().toDouble();
         dataset->compress  = a.value( "compress"  ).toString().toDouble();
         dataset->manual    = a.value( "manual"    ).toString().toInt();
      }

      if ( xml.isStartElement() && xml.name() == "analyte" )
      {
         US_Solution::AnalyteInfo aninfo;
         QXmlStreamAttributes a        = xml.attributes();

         aninfo.analyte.mw     = a.value( "mw"     ).toString().toDouble();
         aninfo.analyte.vbar20 = a.value( "vbar20" ).toString().toDouble();
         aninfo.amount         = a.value( "amount" ).toString().toDouble();
         QString atype         = a.value( "type"   ).toString();
         aninfo.analyte.type   = US_Analyte::PROTEIN;
         if ( atype == "DNA" )
            aninfo.analyte.type   = US_Analyte::DNA;
         if ( atype == "RNA" )
            aninfo.analyte.type   = US_Analyte::RNA;
         if ( atype == "Other" )
            aninfo.analyte.type   = US_Analyte::CARBOHYDRATE;

         dataset->solution_rec.analyteInfo << aninfo;
      }
   }

   dataset->vbar20  = US_Math2::calcCommonVbar( dataset->solution_rec, 20.0 );
}


// Read the job description into job_config and validate it: errors abort,
//  warnings are printed.  Transitional while the readers move to
//  US_MPI_JobConfig: abort if anything differs from the result of parse().
void US_MPI_Analysis::read_job_config( const QString& xmlfile )
{
   job_config                     = US_MPI_JobConfig::fromFile( xmlfile );
   const US_MPI_JobConfig& config = job_config;
   QStringList      diffs;

   auto             differ = [&diffs]( bool same, const QString& what )
   {
      if ( !same )
      {
         diffs << what;
      }
   };
   auto speedsteps = []( const QVector<US_SimulationParameters::SpeedProfile>& sps )
   {
      QString          text;
      QXmlStreamWriter xml( &text );
      for ( auto sp : sps )
      {
         US_SimulationParameters::speedstepToXml( xml, &sp );
      }
      return text;
   };

   differ( config.errors.isEmpty(), "errors: " + config.errors.join( "; " ) );
   differ( config.method == analysis_type, "method" );
   differ( config.cluster == cluster, "cluster" );
   differ( config.db_name == db_name, "db_name" );
   differ( QHostAddress( config.udp_server ) == server, "udp server" );
   differ( config.udp_port == port, "udp port" );
   differ( config.request_id == requestID, "request id" );
   differ( config.request_guid == requestGUID, "request guid" );
   differ( config.parameters == parameters, "parameters" );
   differ( config.debug_level() == dbg_level, "debug_level" );
   differ( config.debug_timings() == dbg_timing, "debug_timings" );
   if ( parameters.contains( "debug_text" ) )
   {
      differ( config.debug_text() == US_Settings::debug_text(), "debug_text" );
   }

   differ( config.buckets.size() == buckets.size(), "bucket count" );
   for ( int ii = 0; ii < qMin( config.buckets.size(), buckets.size() ); ii++ )
   {
      const US_MPI_JobConfig::Bucket& cb = config.buckets[ii];
      const Bucket&                   lb = buckets[ii];
      differ( cb.x_min == lb.x_min && cb.x_max == lb.x_max && cb.y_min == lb.y_min && cb.y_max == lb.y_max,
              QString( "bucket %1" ).arg( ii ) );
   }

   QVector<double>              concs;
   QList<US_SolveSim::DataSet*> dsets = config.make_datasets();
   differ( dsets.size() == data_sets.size(), "dataset count" );

   for ( int ii = 0; ii < qMin( dsets.size(), data_sets.size() ); ii++ )
   { // Compare what the file gives; other values are not initialized
      const US_MPI_JobConfig::DatasetSpec& spec = config.datasets[ii];
      const US_SolveSim::DataSet*          cd   = dsets[ii];
      const US_SolveSim::DataSet*          ld   = data_sets[ii];
      const US_SimulationParameters&       cp   = cd->simparams;
      const US_SimulationParameters&       lp   = ld->simparams;
      QString                              dsx  = QString( "dataset %1: " ).arg( ii );
      concs += spec.total_concentrations;

      differ( cd->auc_file == ld->auc_file, dsx + "auc_file" );
      differ( cd->edit_file == ld->edit_file, dsx + "edit_file" );
      differ( cd->tmst_file == ld->tmst_file, dsx + "tmst_file" );
      differ( cd->noise_files == ld->noise_files, dsx + "noise_files" );
      differ( speedsteps( cp.speed_step ) == speedsteps( lp.speed_step ), dsx + "speed_step" );
      differ( cd->solution_rec.analyteInfo == ld->solution_rec.analyteInfo, dsx + "analytes" );
      if ( spec.has_solution )
      {
         differ( cd->vbar20 == ld->vbar20, dsx + "vbar20" );
      }
      if ( spec.density )
      {
         differ( cd->density == ld->density, dsx + "density" );
      }
      if ( spec.viscosity )
      {
         differ( cd->viscosity == ld->viscosity, dsx + "viscosity" );
      }
      if ( spec.compress )
      {
         differ( cd->compress == ld->compress, dsx + "compress" );
      }
      if ( spec.manual )
      {
         differ( cd->manual == ld->manual, dsx + "manual" );
      }
      if ( spec.rotor_stretch[0] )
      {
         differ( cd->rotor_stretch[0] == ld->rotor_stretch[0] && cd->rotor_stretch[1] == ld->rotor_stretch[1],
                 dsx + "rotor_stretch" );
      }
      if ( spec.centerpiece_bottom )
      {
         differ( cd->centerpiece_bottom == ld->centerpiece_bottom, dsx + "centerpiece_bottom" );
      }
      differ( cp.simpoints == lp.simpoints, dsx + "simpoints" );
      differ( cp.band_volume == lp.band_volume, dsx + "band_volume" );
      differ( cp.meshType == lp.meshType, dsx + "radial_grid" );
      differ( cp.gridType == lp.gridType, dsx + "time_grid" );
      differ( cp.cp_sector == lp.cp_sector, dsx + "centerpiece_shape" );
      differ( cp.cp_angle == lp.cp_angle, dsx + "centerpiece_angle" );
      differ( cp.cp_pathlen == lp.cp_pathlen, dsx + "centerpiece_pathlength" );
      differ( cp.cp_width == lp.cp_width, dsx + "centerpiece_width" );
   }

   differ( concs == concentrations, "total_concentration" );
   qDeleteAll( dsets );

   if ( !diffs.isEmpty() )
   {
      abort( "US_MPI_JobConfig differs from parse() in " + xmlfile + ": " + diffs.join( ", " ) );
   }

   job_config.validate();

   if ( my_rank == 0 )
   {
      for ( const QString& warning : job_config.warnings )
      {
         printf( "  Warning: %s\n", qPrintable( warning ) );
      }
   }

   if ( !job_config.errors.isEmpty() )
   {
      abort( "Invalid job " + xmlfile + ": " + job_config.errors.join( "; " ) );
   }
}
