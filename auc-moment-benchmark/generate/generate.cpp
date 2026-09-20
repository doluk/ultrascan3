// Layer 1 -- UltraScan-linked synthetic data generator.
//
// This is the ONLY part of the prototype that depends on UltraScan/Qt.  It is
// deliberately thin: it produces files and nothing else.  All analysis lives
// in the Python layers so that the handoff package can be run by someone who
// has never heard of UltraScan.
//
// For each model in the frozen test set it writes:
//   <id>.auc        UltraScan raw data (consumable by us_2dsa, us_dcdt, us_vhw)
//   <id>.model.xml  the ground-truth US_Model
//   <id>.ti.noise / <id>.ri.noise   the injected noise vectors
//   <id>.scans.csv  a plain ASCII dump: one row per scan, "time, r1, r2, ..."
//
// The CSV is what analysis/ actually reads.  Keeping a parallel ASCII dump
// means the Python side never needs a Qt XML parser.
//
// STATUS: written against the class/method signatures in utils/ on this
// branch (us_astfem_rsa.h, us_model.h, us_simparms.h, us_dataIO.h,
// us_noise.h).  It has NOT been compiled: this container has no Qt
// installation.  Treat it as a reviewed draft, not as tested code.  See
// FINDINGS.md, "Open items".

#include <QtCore>

#include "us_astfem_math.h"
#include "us_astfem_rsa.h"
#include "us_dataIO.h"
#include "us_model.h"
#include "us_noise.h"
#include "us_simparms.h"
#include "us_util.h"

namespace {

// Selected by the --band command-line flag; see main().
static bool g_band = false;

struct Species { double s_svedberg; double conc; double f_f0; };

struct TestModel
{
   QString            id;
   QVector< Species > species;
   double             rpm;
};

// Keep these numbers identical to forward/models.py.  If the two ever drift
// apart the cross-validation in Phase A is meaningless.
QVector< TestModel > frozen_test_set()
{
   const double C  = 0.5;
   const double s0 = 15.0;   // see models.py: the 4 S set has no usable window

   QVector< TestModel > out;
   out << TestModel{ "F-S1",     { { s0, C, 1.25 } }, 40000.0 };

   const double deltas[] = { 0.15, 0.10, 0.05, 0.03 };
   const char*  tags  [] = { "15", "10", "05", "03" };
   for ( int i = 0; i < 4; i++ )
      out << TestModel{ QString( "F-M2-%1" ).arg( tags[ i ] ),
                        { { s0, C / 2, 1.25 },
                          { s0 * ( 1.0 + deltas[ i ] ), C / 2, 1.25 } },
                        40000.0 };

   out << TestModel{ "F-M3",
                     { { s0, C / 3, 1.25 }, { s0 * 1.06, C / 3, 1.25 },
                       { s0 * 1.12, C / 3, 1.25 } }, 40000.0 };
   out << TestModel{ "F-M2+AGG",
                     { { s0, C * 0.495, 1.25 }, { s0 * 1.10, C * 0.495, 1.25 },
                       { s0 * 2.50, C * 0.010, 1.25 } }, 40000.0 };
   return out;
}

// Diffusion coefficient consistent with (s, f/f0); mirrors models.d_from_s().
double d_from_s( double s, double f_f0, double vbar20 )
{
   const double eta = 0.0100194;      // g/(cm s), water at 20 C
   const double rho = 0.998234;       // g/mL
   const double N_A = 6.02214076e23;
   const double k_B = 1.380649e-16;   // erg/K
   const double T   = 293.15;

   const double buoy = 1.0 - vbar20 * rho;
   const double c    = f_f0 * 6.0 * M_PI * eta
                     * std::pow( 3.0 * vbar20 / ( 4.0 * M_PI * N_A ), 1.0 / 3.0 );
   const double m23  = c * N_A * s / buoy;
   const double M    = std::pow( m23, 1.5 );
   const double f    = M * buoy / ( N_A * s );
   return k_B * T / f;
}

US_Model build_model( const TestModel& tm )
{
   US_Model model;
   model.description  = tm.id;
   model.wavelength   = 280.0;

   for ( const Species& sp : tm.species )
   {
      US_Model::SimulationComponent c;
      c.name                 = QString( "%1_%2S" ).arg( tm.id ).arg( sp.s_svedberg );
      c.vbar20               = 0.73;
      c.f_f0                 = sp.f_f0;
      c.s                    = sp.s_svedberg * 1.0e-13;
      c.D                    = d_from_s( c.s, sp.f_f0, c.vbar20 );
      c.signal_concentration = sp.conc;
      c.molar_concentration  = 0.0;
      c.extinction           = 1.0;
      model.components << c;
   }
   return model;
}

US_SimulationParameters build_params( const TestModel& tm, int n_scans,
                                      double t_start, double t_end )
{
   US_SimulationParameters sp;

   // Populates bottom_position and the rotor stretch coefficients.  Without
   // it calc_bottom() has nothing to work from.
   sp.setHardware( NULL, "0", 0, 0 );

   US_SimulationParameters::SpeedProfile prof;
   prof.rotorspeed        = qRound( tm.rpm );
   prof.set_speed         = qRound( tm.rpm );
   prof.avg_speed         = tm.rpm;
   prof.scans             = n_scans;
   // A high acceleration keeps the run at constant speed for all but the
   // first second of 22000, so the comparison against forward/lamm.py --
   // which assumes the speed is reached instantly -- is not confounded by
   // the acceleration zone.
   prof.acceleration      = 40000;
   prof.acceleration_flag = true;
   prof.delay_hours       = 0;
   prof.delay_minutes     = t_start / 60.0;
   prof.duration_hours    = int( t_end / 3600.0 );
   prof.duration_minutes  = ( t_end - prof.duration_hours * 3600.0 ) / 60.0;
   sp.speed_step.clear();
   sp.speed_step << prof;

   // Phase A.2 showed the GUI default (200) is far too coarse for high
   // moments; see FINDINGS.md.
   sp.simpoints         = 6000;
   sp.radial_resolution = 1.0e-3;
   sp.meshType          = US_SimulationParameters::ASTFEM;
   sp.gridType          = US_SimulationParameters::MOVING;

   // Fixed geometry, matching forward/models.py RunGeometry.  simparams
   // meniscus/bottom take precedence over the hardware-derived values, which
   // is what we want: both solvers must see the same cell.
   sp.meniscus          = 5.90;
   sp.bottom            = 7.20;
   sp.temperature       = 20.0;

   sp.rnoise            = 0.0;   // noise is injected in Layer 2, not here
   sp.lrnoise           = 0.0;
   sp.tinoise           = 0.0;
   sp.rinoise           = 0.0;
   sp.baseline          = 0.0;
   sp.rotorCalID        = "0";

   // Band forming (zonal) vs sedimentation velocity.  US_Astfem_RSA builds
   // the lamella from band_volume together with the centrepiece angle and
   // pathlength (us_astfem_rsa.cpp:1483), so those must be set too --
   // leaving them zero makes it fall back to defaults silently.
   sp.band_forming      = g_band;
   sp.band_volume       = g_band ? 0.015 : 0.0;
   sp.cp_angle          = 2.5;
   sp.cp_pathlen        = 1.2;

   return sp;
}

// Allocate and time-stamp the RawData container the solver fills in.
//
// US_Astfem_RSA::calculate() writes into an existing grid: the caller owns
// the radial grid, the scan times and the omega^2t values.  Getting the
// omega^2t values from US_AstfemMath::calc_omega2t rather than computing
// w^2*t by hand is what makes the acceleration zone consistent with what the
// solver expects.
US_DataIO::RawData make_container( US_SimulationParameters& sp,
                                   const TestModel& tm, const US_Model& model,
                                   int n_scans, double t_start, double t_end )
{
   US_DataIO::RawData data;
   data.type[ 0 ] = 'R';  data.type[ 1 ] = 'A';
   US_Util::uuid_parse( US_Util::new_guid(), (uchar*)data.rawGUID );
   data.cell        = 1;
   data.channel     = 'S';
   data.description = tm.id;

   const int points = int( ( sp.bottom - sp.meniscus ) / sp.radial_resolution ) + 1;
   data.xvalues.resize( points );
   for ( int jp = 0; jp < points; jp++ )
      data.xvalues[ jp ] = sp.meniscus + jp * sp.radial_resolution;

   const int terpsize = ( points + 7 ) / 8;

   US_SimulationParameters::SpeedProfile* prof = &sp.speed_step[ 0 ];
   const double target_speed = prof->set_speed;
   const double delay    = qRound( prof->delay_hours    * 3600.0
                                 + prof->delay_minutes  * 60.0 );
   const double duration = qRound( prof->duration_hours * 3600.0
                                 + prof->duration_minutes * 60.0 );
   const double dt       = ( duration - delay ) / double( prof->scans - 1 );

   for ( int js = 0; js < prof->scans; js++ )
   {
      US_DataIO::Scan scan;
      scan.temperature = sp.temperature;
      scan.rpm         = target_speed;
      scan.wavelength  = model.wavelength;
      scan.plateau     = 0.0;
      scan.delta_r     = sp.radial_resolution;
      scan.seconds     = double( qRound( delay + dt * js ) );
      scan.omega2t     = US_AstfemMath::calc_omega2t( 0.0, 0.0, 0.0,
                            target_speed, prof->acceleration, scan.seconds );
      scan.rvalues     .fill( 0.0, points );
      scan.interpolated.fill( 0,   terpsize );

      if ( js == 0 )
      {
         prof->time_first = scan.seconds;
         prof->w2t_first  = scan.omega2t;
      }
      else if ( js == prof->scans - 1 )
      {
         prof->time_last  = scan.seconds;
         prof->w2t_last   = scan.omega2t;
      }
      data.scanData << scan;
   }
   return data;
}

bool dump_csv( const QString& path, const US_DataIO::RawData& data )
{
   QFile f( path );
   if ( ! f.open( QIODevice::WriteOnly | QIODevice::Text ) )
      return false;
   QTextStream ts( &f );

   ts << "# radii";
   for ( double r : data.xvalues ) ts << "," << QString::number( r, 'g', 10 );
   ts << "\n";
   for ( const US_DataIO::Scan& sc : data.scanData )
   {
      ts << QString::number( sc.seconds, 'g', 10 );
      for ( double v : sc.rvalues ) ts << "," << QString::number( v, 'g', 10 );
      ts << "\n";
   }
   return true;
}

}  // namespace

int main( int argc, char* argv[] )
{
   QCoreApplication app( argc, argv );

   QString outdir = "out";
   for ( int i = 1; i < argc; i++ )
   {
      const QString arg = QString( argv[ i ] );
      if ( arg == "--band" ) g_band = true;
      else                   outdir = arg;
   }
   QDir().mkpath( outdir );

   const int    n_scans = 100;
   // A band run is scheduled to the band's transit time: every scan after it
   // reaches the bottom is useless, for moments and for the band-gated noise
   // estimate alike.  See models.band_run_geometry().
   const double t_start = g_band ?  300.0 :   600.0;
   const double t_end   = g_band ? 6000.0 : 22000.0;

   qInfo() << ( g_band ? "band-forming run" : "sedimentation velocity run" )
           << "->" << outdir;

   for ( const TestModel& tm : frozen_test_set() )
   {
      US_Model                model  = build_model( tm );
      US_SimulationParameters params = build_params( tm, n_scans, t_start, t_end );
      US_DataIO::RawData      data   = make_container( params, tm, model,
                                                       n_scans, t_start, t_end );

      US_Astfem_RSA solver( model, params );
      solver.setTimeCorrection( false );
      solver.setTimeInterpolation( true );
      solver.set_debug_flag( 0 );

      const int rc = solver.calculate( data );
      if ( rc != 0 )
      {
         qWarning() << tm.id << ": solver returned" << rc;
         return 1;
      }

      const QString stem = outdir + "/" + tm.id;
      US_DataIO::writeRawData( stem + ".auc", data );
      model.write( stem + ".model.xml" );
      dump_csv( stem + ".scans.csv", data );

      qInfo() << "wrote" << stem << "scans:" << data.scanData.size()
              << "points:" << data.xvalues.size();
   }
   return 0;
}
