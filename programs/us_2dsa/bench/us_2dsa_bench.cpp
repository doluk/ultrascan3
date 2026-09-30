//! \file us_2dsa_bench.cpp
//! \brief Headless benchmark of 2DSA subgrid partitions for custom grids.
//!
//! Data are simulated by us_astfem_sim (its US_Astfem_Sim class, driven
//! through init_from_args() exactly as its command line does), which saves
//! the AUC data, edit file, run definition, TimeState and TI/RI noise into
//! a run directory.  Fits load a run directory as us_2dsa does and use the
//! us_2dsa processing classes (US_2dsaProcess and its worker threads) with a
//! CUSTOMGRID model whose component order encodes a given subgrid partition
//! (component k belongs to subgrid k mod n, as in us_2dsa and
//! us_mpi_analysis).
//!
//! Modes:
//!   us_2dsa_bench prepare <dir> <rnoise%> <tinoise%> <rinoise%>
//!      Write mixture models (mix_<name>.xml), a water buffer (water.xml)
//!      and simulation parameters (sp_noisy.xml, sp_clean.xml).
//!   us_2dsa_bench simulate <model.xml> <buffer.xml> <simparams.xml>
//!                          <rundir> <seed> [<left radius>]
//!      Run the us_astfem_sim simulation and save it into <rundir>;
//!      optionally move the left data limit of the edit to <left radius>.
//!   us_2dsa_bench columns <rundir> <solutes.csv> <out.f32> [threads]
//!      Simulate each (s [S], f/f0) row at unit concentration on the run's
//!      edited data grid; write float32 arrays of nscans*npoints values.
//!   us_2dsa_bench fit <rundir> <truth rundir> <grid.csv|ugrid:N:R>
//!                     <nsubgrids> <maxiters> <threads> <out.json>
//!      Fit with a custom grid (CSV rows: s [S], f/f0, subgrid label) or
//!      with the regular-grid path (ugrid:N:R, N x N points, R repetitions);
//!      the truth run holds the same experiment simulated without noise.
//!   us_2dsa_bench dump <rundir> <out.f32>
//!      Write the edited data readings (float32, scan by scan).
//!
//! Merging of task results uses the "2DSA-OrderedMerge" debug setting, so
//! that fits do not depend on the order in which threads finish; set the
//! environment variable BENCH_ARRIVAL=1 to merge in arrival order instead.
//! BENCH_DEBUG adds further debug settings (comma-separated, for example
//! "2DSA-MergePool=512" or "SolveSim-ExactNoise").  BENCH_NOISE selects the
//! fitted noise (0 none, 1 TI, 2 RI, 3 TI+RI; default 3), and BENCH_TI_FILE
//! names the output of an earlier fit whose TI noise is subtracted from the
//! data before fitting.  BENCH_BAND_VOLUME (mL) fits as a band-forming
//! experiment with that lamella volume.

#include <QApplication>
#include <QtCore>
#include <QMessageBox>
#include <atomic>
#include <thread>
#include <sys/resource.h>
#include <unistd.h>

#include "us_2dsa_process.h"
#include "us_astfem_sim.h"
#include "us_astfem_rsa.h"
#include "us_astfem_math.h"
#include "us_settings.h"
#include "us_model.h"
#include "us_buffer.h"
#include "us_math2.h"
#include "us_constants.h"
#include "us_util.h"

namespace
{
const double VBAR   = 0.73;

struct Species { double s; double k; double c; };

QVector< Species > mixture( const QString& name )
{
   QVector< Species > mix;
   if ( name == "M1" )
      mix << Species{ 3.3, 1.25, 0.4 } << Species{ 6.1, 1.55, 0.6 };
   else if ( name == "M2" )
      mix << Species{ 2.2, 1.4, 0.3 } << Species{ 4.7, 1.3, 0.4 }
          << Species{ 8.4, 2.6, 0.3 };
   else if ( name == "M3" )
      mix << Species{ 2.5, 1.2, 0.5 } << Species{ 5.5, 3.5, 0.5 };
   else if ( name == "H1" )
      mix << Species{ 4.3, 1.3, 0.95 } << Species{ 6.4, 1.45, 0.05 };
   else if ( name == "H2" )
   {
      for ( int ii = 0; ii < 12; ii++ )
         mix << Species{ 2.0 + 6.0 * ii / 11.0, 1.2 + 1.6 * ii / 11.0,
                         1.0 / 12.0 };
   }
   else if ( name == "H3" )
      mix << Species{ 4.0, 1.2, 0.5 } << Species{ 4.0, 2.5, 0.5 };
   else if ( name == "A1" )
      mix << Species{ 6.5, 1.5, 0.90 } << Species{ 9.5, 1.6, 0.07 }
          << Species{ 3.5, 1.4, 0.03 };
   return mix;
}

const QStringList MIXTURES = { "M1", "M2", "M3", "H1", "H2", "H3", "A1" };

// A standard-space (20,W) component for s [S] and f/f0
US_Model::SimulationComponent component( double s, double k, double c )
{
   US_Model::SimulationComponent sc;
   sc.s      = s * 1.0e-13;
   sc.f_f0   = k;
   sc.vbar20 = qgetenv( "BENCH_VBAR" ).isEmpty() ? VBAR     // as the data set
             : qgetenv( "BENCH_VBAR" ).toDouble();
   sc.D      = 0.0;
   sc.mw     = 0.0;
   sc.f      = 0.0;
   sc.signal_concentration = c;
   US_Model::calc_coefficients( sc );
   return sc;
}

long rss_kb( const char* key )
{
   QFile fi( "/proc/self/status" );
   if ( ! fi.open( QIODevice::ReadOnly ) ) return 0;
   for ( const QByteArray& line : fi.readAll().split( '\n' ) )
      if ( line.startsWith( key ) )
         return line.mid( strlen( key ) ).trimmed().split( ' ' )[ 0 ].toLong();
   return 0;
}

// ---------------------------------------------------------------- prepare
int mode_prepare( const QStringList& args )
{
   QDir dir( args[ 0 ] );
   dir.mkpath( "." );
   for ( const QString& name : MIXTURES )
   {
      US_Model model;
      model.description = "bench mixture " + name;
      model.modelGUID   = US_Util::new_guid();
      model.analysis    = US_Model::MANUAL;
      model.wavelength  = 280.0;
      for ( const Species& sp : mixture( name ) )
         model.components << component( sp.s, sp.k, sp.c );
      for ( int ii = 0; ii < model.components.size(); ii++ )
         model.components[ ii ].name = QString( "C%1" ).arg( ii + 1 );
      model.write( dir.filePath( "mix_" + name + ".xml" ) );
   }

   US_Buffer buffer;
   buffer.GUID        = US_Util::new_guid();
   buffer.description = "water 20C";
   buffer.density     = DENS_20W;
   buffer.viscosity   = VISC_20W;
   buffer.manual      = true;
   buffer.writeToDisk( dir.filePath( "water.xml" ) );

   US_SimulationParameters sp;
   sp.speed_step.clear();
   US_SimulationParameters::SpeedProfile ss;
   ss.rotorspeed       = 50000;
   ss.set_speed        = 50000;
   ss.avg_speed        = 50000;
   ss.acceleration     = 400;
   ss.acceleration_flag = true;
   ss.duration_hours   = 5;
   ss.duration_minutes = 0.0;
   ss.delay_hours      = 0;
   ss.delay_minutes    = 5.0;
   ss.scans            = 60;
   sp.speed_step << ss;
   sp.simpoints         = 200;
   sp.radial_resolution = 0.003;
   sp.meshType          = US_SimulationParameters::ASTFEM;
   sp.gridType          = US_SimulationParameters::MOVING;
   sp.meniscus          = 5.9;
   sp.bottom            = 7.2;
   sp.bottom_position   = 7.2;
   sp.temperature       = 20.0;
   sp.band_forming      = false;
   sp.rnoise            = 0.0;
   sp.tinoise           = 0.0;
   sp.rinoise           = 0.0;
   sp.save_simparms( dir.filePath( "sp_clean.xml" ) );
   sp.rnoise            = args[ 1 ].toDouble();
   sp.tinoise           = args[ 2 ].toDouble();
   sp.rinoise           = args[ 3 ].toDouble();
   sp.save_simparms( dir.filePath( "sp_noisy.xml" ) );
   return 0;
}

// --------------------------------------------------------------- simulate
QString edit_file( const QString& rundir )
{  // runID.<editID>.<type>.<cell>.<channel>.<wl>.xml
   QDir dir( rundir );
   for ( const QString& fn : dir.entryList( QStringList( "*.xml" ) ) )
      if ( fn.split( '.' ).size() == 7  &&  ! fn.contains( "time_state" ) )
         return fn;
   return QString();
}

int mode_simulate( const QStringList& args )
{
   QString rundir = QDir( args[ 3 ] ).absolutePath();
   QDir().mkpath( rundir );
   US_Settings::set_default_data_location( 2 );          // --no-db
   US_Math2::randomize( args[ 4 ].toUInt() );            // noise realization

   // Dismiss any modal message box (the CLI path can open one when readings
   // exceed twice the total concentration near the bottom)
   QTimer dismiss;
   QObject::connect( &dismiss, &QTimer::timeout, []()
   {
      QWidget* w = QApplication::activeModalWidget();
      if ( w != 0 )
      {
         QMessageBox* mb = qobject_cast< QMessageBox* >( w );
         fprintf( stderr, "dismissed dialog: %s\n",
                  mb ? qPrintable( mb->text().replace( '\n', ' ' ) ) : "?" );
         w->close();
      }
   } );
   dismiss.start( 200 );

   QMap< QString, QString > flags;
   flags[ "model" ]     = QFileInfo( args[ 0 ] ).absoluteFilePath();
   flags[ "buffer" ]    = QFileInfo( args[ 1 ] ).absoluteFilePath();
   flags[ "simparams" ] = QFileInfo( args[ 2 ] ).absoluteFilePath();
   flags[ "rotor" ]     = "0:0";                          // no rotor stretch
   flags[ "start" ]     = "true";
   flags[ "save" ]      = rundir;
   flags[ "close" ]     = "true";
   flags[ "errors-cl" ] = "true";
   US_Astfem_Sim sim;
   int status = sim.init_from_args( flags );
   if ( status != 0 )
   {
      fprintf( stderr, "us_astfem_sim init_from_args status %d\n", status );
      return status;
   }

   if ( args.size() > 5 )
   {  // Move the left data limit of the edit (e.g. to exclude the meniscus)
      QString efn = QDir( rundir ).filePath( edit_file( rundir ) );
      QFile fi( efn );
      if ( ! fi.open( QIODevice::ReadOnly | QIODevice::Text ) ) return 3;
      QString xml = QString::fromUtf8( fi.readAll() );
      fi.close();
      xml.replace( QRegularExpression( "(<data_range left=\")[^\"]*\"" ),
                   "\\1" + args[ 5 ] + "\"" );
      if ( ! fi.open( QIODevice::WriteOnly | QIODevice::Text ) ) return 3;
      fi.write( xml.toUtf8() );
   }
   return 0;
}

// ---------------------------------------------------- load run as us_2dsa
bool load_experiment( const QString& rundir, US_SolveSim::DataSet& dset,
                      QString& err )
{
   QString efn = edit_file( rundir );
   if ( efn.isEmpty() ) { err = "no edit file in " + rundir;  return false; }
   QVector< US_DataIO::EditedData > edats;
   QVector< US_DataIO::RawData >    rdats;
   int stat = US_DataIO::loadData( rundir, efn, edats, rdats );
   if ( stat != US_DataIO::OK  ||  edats.isEmpty() )
   {  err = "loadData: " + US_DataIO::errorString( stat );  return false; }

   dset.run_data  = edats[ 0 ];
   US_DataIO::EditedData* edata = &dset.run_data;
   QString runID  = edata->runID;

   // As in US_2dsa::load (no database)
   dset.simparams.initFromData( NULL, *edata, true );
   dset.simparams.sim = ( edata->channel == "S" );

   // Band-forming centerpiece (as set from the centerpiece shape or in the
   // us_2dsa advanced dialog): BENCH_BAND_VOLUME gives the lamella volume in
   // mL; path length and sector angle are those of the simulation
   if ( ! qgetenv( "BENCH_BAND_VOLUME" ).isEmpty() )
   {
      dset.simparams.band_forming = true;
      dset.simparams.band_volume  = qgetenv( "BENCH_BAND_VOLUME" ).toDouble();
      dset.simparams.cp_pathlen   = 1.2;
      dset.simparams.cp_angle     = 2.5;
   }
   QString tmst  = QDir( rundir ).filePath( runID + ".time_state.tmst" );
   US_DataIO::RawData sdata;
   US_AstfemMath::initSimData( sdata, *edata, 0.0 );
   if ( ! ( QFile( tmst ).exists()  &&
            US_AstfemMath::timestate_onesec( tmst, sdata ) ) )
   {
      tmst = QDir::temp().filePath( QString( "bench2dsa_%1.tmst" )
                                    .arg( getpid() ) );
      US_AstfemMath::writetimestate( tmst, dset.simparams, sdata );
   }
   dset.simparams.simSpeedsFromTimeState( tmst );
   dset.simparams.speedstepsFromSSprof();
   dset.tmst_file          = tmst;

   // Buffer and vbar:  water and VBAR unless BENCH_DENSITY, BENCH_VISCOSITY
   // and BENCH_VBAR are given (for real data)
   double vbar             = qgetenv( "BENCH_VBAR" ).isEmpty() ? VBAR
                           : qgetenv( "BENCH_VBAR" ).toDouble();
   dset.viscosity          = qgetenv( "BENCH_VISCOSITY" ).isEmpty() ? VISC_20W
                           : qgetenv( "BENCH_VISCOSITY" ).toDouble();
   dset.density            = qgetenv( "BENCH_DENSITY" ).isEmpty() ? DENS_20W
                           : qgetenv( "BENCH_DENSITY" ).toDouble();
   dset.compress           = 0.0;
   dset.temperature        = edata->average_temperature();
   dset.vbar20             = vbar;
   dset.vbartb             = US_Math2::adjust_vbar20( vbar, dset.temperature );
   dset.manual             = false;
   US_Math2::SolutionData sd;
   sd.density              = dset.density;
   sd.viscosity            = dset.viscosity;
   sd.vbar20               = dset.vbar20;
   sd.vbar                 = dset.vbartb;
   sd.manual               = false;
   US_Math2::data_correction( dset.temperature, sd );
   dset.s20w_correction    = sd.s20w_correction;
   dset.D20w_correction    = sd.D20w_correction;
   dset.solute_type        = 0;
   dset.rotor_stretch[ 0 ] = dset.simparams.rotorcoeffs[ 0 ];
   dset.rotor_stretch[ 1 ] = dset.simparams.rotorcoeffs[ 1 ];
   dset.centerpiece_bottom = dset.simparams.bottom_position;
   return true;
}

// Simulate components (standard space) on the experiment grid
void simulate( US_SolveSim::DataSet& dset,
               QVector< US_Model::SimulationComponent > comps,
               US_DataIO::RawData& sim )
{
   US_Model model;
   for ( int ii = 0; ii < comps.size(); ii++ )
   {
      comps[ ii ].s /= dset.s20w_correction;
      comps[ ii ].D /= dset.D20w_correction;
   }
   model.components = comps;
   US_SimulationParameters sp = dset.simparams;
   US_AstfemMath::initSimData( sim, dset.run_data, 0.0 );
   US_Astfem_RSA astfem( model, sp );
   astfem.calculate( sim );
}

// ---------------------------------------------------------------- columns
int mode_columns( const QStringList& args )
{
   US_SolveSim::DataSet dset;
   QString err;
   if ( ! load_experiment( args[ 0 ], dset, err ) )
   {  fprintf( stderr, "%s\n", qPrintable( err ) );  return 2; }
   int nscan  = dset.run_data.scanCount();
   int npts   = dset.run_data.pointCount();
   QVector< QPair< double, double > > sols;
   QFile fi( args[ 1 ] );
   if ( ! fi.open( QIODevice::ReadOnly | QIODevice::Text ) ) return 1;
   QTextStream ts( &fi );
   while ( ! ts.atEnd() )
   {
      QStringList f = ts.readLine().split( ',' );
      if ( f.size() >= 2 )
         sols << qMakePair( f[ 0 ].toDouble(), f[ 1 ].toDouble() );
   }
   int nthr   = args.size() > 3 ? args[ 3 ].toInt() : 4;
   int nsol   = sols.size();
   int ncell  = nscan * npts;
   QVector< float > out( (qint64)nsol * ncell );
   std::atomic< int > next( 0 );
   auto work = [ & ]()
   {
      US_SolveSim::DataSet dloc = dset;
      int jj;
      while ( ( jj = next++ ) < nsol )
      {
         US_DataIO::RawData sim;
         QVector< US_Model::SimulationComponent > comps;
         comps << component( sols[ jj ].first, sols[ jj ].second, 1.0 );
         simulate( dloc, comps, sim );
         float* col = out.data() + (qint64)jj * ncell;
         for ( int ss = 0; ss < nscan; ss++ )
            for ( int rr = 0; rr < npts; rr++ )
               col[ ss * npts + rr ] = (float)sim.value( ss, rr );
      }
   };
   QElapsedTimer tm; tm.start();
   std::vector< std::thread > thr;
   for ( int ii = 0; ii < nthr; ii++ ) thr.emplace_back( work );
   for ( auto& t : thr ) t.join();
   QFile fo( args[ 2 ] );
   if ( ! fo.open( QIODevice::WriteOnly ) ) return 1;
   fo.write( (const char*)out.data(), out.size() * sizeof( float ) );
   fprintf( stderr, "columns: %d solutes (%d scans x %d points) in %.1f s\n",
            nsol, nscan, npts, tm.elapsed() / 1000.0 );
   return 0;
}

// ------------------------------------------------------------------- dump
int mode_dump( const QStringList& args )
{  // Edited data readings as float32, scan by scan
   US_SolveSim::DataSet dset;
   QString err;
   if ( ! load_experiment( args[ 0 ], dset, err ) )
   {  fprintf( stderr, "%s\n", qPrintable( err ) );  return 2; }
   QVector< float > buf;
   for ( int ss = 0; ss < dset.run_data.scanCount(); ss++ )
      for ( int rr = 0; rr < dset.run_data.pointCount(); rr++ )
         buf << (float)dset.run_data.value( ss, rr );
   QFile fo( args[ 1 ] );
   if ( ! fo.open( QIODevice::WriteOnly ) ) return 1;
   fo.write( (const char*)buf.data(), buf.size() * sizeof( float ) );
   return 0;
}

// Build a CUSTOMGRID model: members of subgrid i at positions m*n + i
bool grid_model( const QString& path, int nsub, US_Model& model, QString& err )
{
   QFile fi( path );
   if ( ! fi.open( QIODevice::ReadOnly | QIODevice::Text ) )
   {  err = "cannot open " + path;  return false; }
   QVector< QVector< US_Model::SimulationComponent > > members( nsub );
   QTextStream ts( &fi );
   int npts = 0;
   while ( ! ts.atEnd() )
   {
      QStringList f = ts.readLine().split( ',' );
      if ( f.size() < 3 ) continue;
      int lab = f[ 2 ].toInt();
      if ( lab < 0  ||  lab >= nsub ) { err = "bad label";  return false; }
      members[ lab ] << component( f[ 0 ].toDouble(), f[ 1 ].toDouble(), 0.0 );
      npts++;
   }
   // Sizes forced by the k mod n decoding: first (N mod n) subgrids hold one more
   for ( int ii = 0; ii < nsub; ii++ )
   {
      int need = ( npts - ii + nsub - 1 ) / nsub;
      if ( members[ ii ].size() != need )
      {
         err = QString( "subgrid %1 has %2 points; k mod n decoding needs %3" )
               .arg( ii ).arg( members[ ii ].size() ).arg( need );
         return false;
      }
   }
   model = US_Model();
   model.analysis    = US_Model::CUSTOMGRID;
   model.subGrids    = nsub;
   model.description = "bench-CustomGrid";
   model.components.resize( npts );
   for ( int kk = 0; kk < npts; kk++ )
   {
      US_Model::SimulationComponent sc = members[ kk % nsub ][ kk / nsub ];
      sc.name = QString( "sg%1_p%2" ).arg( kk % nsub + 1, 3, 10, QChar( '0' ) )
                                     .arg( kk / nsub + 1, 3, 10, QChar( '0' ) );
      model.components[ kk ] = sc;
   }
   return true;
}

class FitRunner : public QObject
{
   public:
   int stage  = -1;
   int niters = 0;
   void done( int st )
   {
      if ( st == 0 ) { niters++; return; }     // refinement iteration done
      stage = st;
      QCoreApplication::quit();
   }
};

void project( QVector< double >& v, int nscan, int npts )
{  // double centering (removes TI and RI components)
   QVector< double > cm( npts, 0.0 ), rm( nscan, 0.0 );
   double gm = 0.0;
   for ( int ss = 0; ss < nscan; ss++ )
      for ( int rr = 0; rr < npts; rr++ )
      {
         double x = v[ ss * npts + rr ];
         cm[ rr ] += x / nscan;  rm[ ss ] += x / npts;  gm += x;
      }
   gm /= (double)( nscan * npts );
   for ( int ss = 0; ss < nscan; ss++ )
      for ( int rr = 0; rr < npts; rr++ )
         v[ ss * npts + rr ] -= cm[ rr ] + rm[ ss ] - gm;
}

// -------------------------------------------------------------------- fit
int mode_fit( const QStringList& args )
{
   QString rundir  = args[ 0 ];
   QString truedir = args[ 1 ];
   QString gridarg = args[ 2 ];
   int     nsub    = args[ 3 ].toInt();
   int     mxiter  = args[ 4 ].toInt();
   int     nthr    = args[ 5 ].toInt();
   QString outpath = args[ 6 ];

   // Task-ordered merging unless BENCH_ARRIVAL=1 (arrival-ordered, as on main)
   // plus any debug settings in BENCH_DEBUG (comma-separated, e.g.
   // "2DSA-MergePool=512")
   bool ordered  = ( qgetenv( "BENCH_ARRIVAL" ) != "1" );
   QStringList dbgtext;
   if ( ordered )
      dbgtext << "2DSA-OrderedMerge";
   QString dbgextra = QString::fromLocal8Bit( qgetenv( "BENCH_DEBUG" ) );
   if ( ! dbgextra.isEmpty() )
      dbgtext << dbgextra.split( ',' );
   US_Settings::set_debug_text( dbgtext );

   US_SolveSim::DataSet dset, tset;
   QString err;
   if ( ! load_experiment( rundir, dset, err )  ||
        ! load_experiment( truedir, tset, err ) )
   {  fprintf( stderr, "%s\n", qPrintable( err ) );  return 2; }
   int nscan  = dset.run_data.scanCount();
   int npts   = dset.run_data.pointCount();

   // Noise flag (BENCH_NOISE: 0 none, 1 TI, 2 RI, 3 TI+RI; default 3)
   int noif   = qgetenv( "BENCH_NOISE" ).isEmpty() ? 3
              : qgetenv( "BENCH_NOISE" ).toInt();

   // BENCH_TI_FILE:  output of an earlier fit whose TI noise is subtracted
   // from the data first (as when loading data with a TI noise correction)
   QString tifile = QString::fromLocal8Bit( qgetenv( "BENCH_TI_FILE" ) );
   if ( ! tifile.isEmpty() )
   {
      QFile fi( tifile );
      if ( ! fi.open( QIODevice::ReadOnly ) )
      {  fprintf( stderr, "cannot open %s\n", qPrintable( tifile ) );  return 2; }
      QJsonArray ti = QJsonDocument::fromJson( fi.readAll() ).object()
                      .value( "ti_noise" ).toArray();
      if ( ti.size() != npts )
      {  fprintf( stderr, "TI noise has %d values, data %d points\n",
                  (int)ti.size(), npts );  return 2; }
      for ( int ss = 0; ss < nscan; ss++ )
         for ( int rr = 0; rr < npts; rr++ )
            dset.run_data.setValue( ss, rr, dset.run_data.value( ss, rr )
                                            - ti[ rr ].toDouble() );
   }

   int  jgref = -1;
   int  nss = 64, nks = 64;
   double slo = 1.0, sup = 10.0, klo = 1.0, kup = 4.0;
   if ( gridarg.startsWith( "ugrid:" ) )
   {  // ugrid:N:R  (N x N points over s 1-10, f/f0 1-4; R repetitions) or
      // ugrid:slo:sup:ns:klo:kup:nk  (repetitions and point counts as
      // chosen by us_2dsa, US_Math2::best_grid_reps)
      QStringList f = gridarg.split( ':' );
      if ( f.size() >= 7 )
      {
         slo   = f[ 1 ].toDouble();  sup = f[ 2 ].toDouble();  nss = f[ 3 ].toInt();
         klo   = f[ 4 ].toDouble();  kup = f[ 5 ].toDouble();  nks = f[ 6 ].toInt();
         jgref = US_Math2::best_grid_reps( nss, nks );
         nss   = ( ( nss + jgref / 2 ) / jgref ) * jgref;
         nks   = ( ( nks + jgref / 2 ) / jgref ) * jgref;
         fprintf( stderr, "grid: s %g-%g x%d, f/f0 %g-%g x%d, %d repetitions\n",
                  slo, sup, nss, klo, kup, nks, jgref );
      }
      else
      {
         nss = nks = f[ 1 ].toInt();
         jgref = f[ 2 ].toInt();
      }
   }
   else if ( ! grid_model( gridarg, nsub, dset.model, err ) )
   {
      fprintf( stderr, "grid error: %s\n", qPrintable( err ) );
      return 2;
   }

   QList< US_SolveSim::DataSet* > dsets;
   dsets << &dset;
   long rss_base = rss_kb( "VmRSS:" );

   FitRunner runner;
   US_2dsaProcess* proc = new US_2dsaProcess( dsets, &runner );
   QObject::connect( proc, &US_2dsaProcess::process_complete,
                     [ & ]( int st ) { runner.done( st ); } );

   struct rusage ru0, ru1;
   getrusage( RUSAGE_SELF, &ru0 );
   QElapsedTimer timer;
   timer.start();
   proc->set_iters( mxiter, 0, 0, 1.0e-12, 0.0, 0.0, jgref, 0 );
   proc->start_fit( slo, sup, nss, klo, kup, nks, jgref, nthr, noif );
   QCoreApplication::exec();
   qint64 wall_ms = timer.elapsed();
   getrusage( RUSAGE_SELF, &ru1 );

   US_DataIO::RawData sdata, rdata;
   US_Model fmodel;
   US_Noise tin, rin;
   proc->get_results( &sdata, &rdata, &fmodel, &tin, &rin );

   // Residual RMSD; error of the fitted signal against the noise-free data
   double vari = 0.0;
   QVector< double > dsig( nscan * npts );
   for ( int ss = 0; ss < nscan; ss++ )
      for ( int rr = 0; rr < npts; rr++ )
      {
         vari += sq( rdata.value( ss, rr ) );
         dsig[ ss * npts + rr ] = sdata.value( ss, rr )
                                - tset.run_data.value( ss, rr );
      }
   project( dsig, nscan, npts );
   double err_proj = 0.0;
   for ( double x : dsig ) err_proj += x * x;
   double ncell = (double)( nscan * npts );

   // Residual structure:  lag-1 autocorrelation along radius and time, and
   // the RMS of residual scan means and radius means
   double acr = 0.0, act = 0.0, rsum = 0.0;
   QVector< double > rmean_s( nscan, 0.0 ), rmean_r( npts, 0.0 );
   for ( int ss = 0; ss < nscan; ss++ )
      for ( int rr = 0; rr < npts; rr++ )
      {
         double rv    = rdata.value( ss, rr );
         rsum        += rv * rv;
         rmean_s[ ss ] += rv / npts;
         rmean_r[ rr ] += rv / nscan;
         if ( rr + 1 < npts )  acr += rv * rdata.value( ss, rr + 1 );
         if ( ss + 1 < nscan ) act += rv * rdata.value( ss + 1, rr );
      }
   double rms_smean = 0.0, rms_rmean = 0.0;
   for ( double x : rmean_s ) rms_smean += x * x;
   for ( double x : rmean_r ) rms_rmean += x * x;

   QJsonObject jo;
   jo[ "rundir" ]    = rundir;
   jo[ "grid" ]      = gridarg;
   jo[ "nsubgrids" ] = nsub;
   jo[ "maxiters" ]  = mxiter;
   jo[ "threads" ]   = nthr;
   jo[ "ordered_merge" ] = ordered;
   jo[ "debug_text" ] = dbgtext.join( "," );
   jo[ "stage" ]     = runner.stage;
   jo[ "iterations" ] = runner.niters + 1;
   jo[ "nscans" ]    = nscan;
   jo[ "npoints" ]   = npts;
   jo[ "rmsd" ]      = sqrt( vari / ncell );
   jo[ "signal_err_rmsd" ] = sqrt( err_proj / ncell );
   jo[ "noise_flag" ]  = noif;
   jo[ "ti_file" ]     = tifile;
   jo[ "resid_ac_radius" ] = rsum > 0.0 ? acr / rsum : 0.0;
   jo[ "resid_ac_time" ]   = rsum > 0.0 ? act / rsum : 0.0;
   jo[ "resid_rms_scan_means" ]   = sqrt( rms_smean / nscan );
   jo[ "resid_rms_radius_means" ] = sqrt( rms_rmean / npts );
   QJsonArray jti, jri;
   for ( double x : tin.values ) jti << x;
   for ( double x : rin.values ) jri << x;
   jo[ "ti_noise" ] = jti;
   jo[ "ri_noise" ] = jri;
   jo[ "wall_s" ]    = wall_ms / 1000.0;
   jo[ "cpu_user_s" ] = ( ru1.ru_utime.tv_sec - ru0.ru_utime.tv_sec )
                      + ( ru1.ru_utime.tv_usec - ru0.ru_utime.tv_usec ) * 1e-6;
   jo[ "cpu_sys_s" ] = ( ru1.ru_stime.tv_sec - ru0.ru_stime.tv_sec )
                      + ( ru1.ru_stime.tv_usec - ru0.ru_stime.tv_usec ) * 1e-6;
   jo[ "rss_base_kb" ] = (double)rss_base;
   jo[ "rss_peak_kb" ] = (double)rss_kb( "VmHWM:" );

   QJsonArray jt;
   QJsonArray jfin;
   qint64 nsim = 0;
   for ( const US_2dsaTaskRecord& tr : proc->task_records() )
   {
      QJsonArray t;
      t << tr.iter << tr.depth << tr.taskx << ( tr.final ? 1 : 0 )
        << tr.isolutes.size() << tr.csolutes.size();
      jt << t;
      nsim += tr.isolutes.size();
      if ( tr.final )
      {  // Input solutes of the (last) final fit
         jfin = QJsonArray();
         for ( const US_Solute& so : tr.isolutes )
            jfin << QJsonArray( { so.s * 1.0e+13, so.k } );
      }
   }
   jo[ "tasks" ]        = jt;  // [iter, depth, taskx, final, n_in, n_out]
   jo[ "simulations" ]  = (double)nsim;
   jo[ "final_inputs" ] = jfin;   // [s (S), f/f0] of the last final fit

   QJsonArray js;
   for ( const US_Model::SimulationComponent& sc : fmodel.components )
   {
      QJsonArray c;
      c << sc.s * 1.0e+13 << sc.f_f0 << sc.signal_concentration << sc.D;
      js << c;
   }
   jo[ "solutes" ] = js;      // [s (S), f/f0, conc, D]

   QFile fo( outpath );
   if ( ! fo.open( QIODevice::WriteOnly ) ) return 1;
   fo.write( QJsonDocument( jo ).toJson( QJsonDocument::Compact ) );
   fprintf( stderr, "fit %s %s n=%d: rmsd %.6f  err %.6f  wall %.1f s"
            "  peak %ld MB  iters %d  nsol %d\n", qPrintable( rundir ),
            qPrintable( gridarg ), nsub, sqrt( vari / ncell ),
            sqrt( err_proj / ncell ), wall_ms / 1000.0,
            rss_kb( "VmHWM:" ) / 1024, runner.niters + 1,
            fmodel.components.size() );
   return 0;
}
}  // namespace

int main( int argc, char* argv[] )
{
   qputenv( "QT_QPA_PLATFORM", "offscreen" );
   QApplication app( argc, argv );
   QStringList args = app.arguments().mid( 1 );
   if ( args.isEmpty() )
   {
      fprintf( stderr, "usage: us_2dsa_bench prepare|simulate|columns|fit ...\n" );
      return 1;
   }
   QString mode = args.takeFirst();
   if ( mode == "prepare"  && args.size() >= 4 ) return mode_prepare( args );
   if ( mode == "simulate" && args.size() >= 5 ) return mode_simulate( args );
   if ( mode == "columns"  && args.size() >= 3 ) return mode_columns( args );
   if ( mode == "fit"      && args.size() >= 7 ) return mode_fit( args );
   if ( mode == "dump"     && args.size() >= 2 ) return mode_dump( args );
   fprintf( stderr, "bad arguments\n" );
   return 1;
}
