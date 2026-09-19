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

#include "us_astfem_rsa.h"
#include "us_dataIO.h"
#include "us_model.h"
#include "us_noise.h"
#include "us_simparms.h"

namespace {

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
   model.compressibility = 0.0;
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
   sp.meniscus          = 5.90;
   sp.bottom            = 7.20;
   sp.bottom_position   = 7.20;
   sp.rnoise            = 0.0;   // noise is injected afterwards, not here
   sp.tinoise           = 0.0;
   sp.rinoise           = 0.0;
   sp.band_forming      = false;

   // Phase A.2 showed the GUI defaults are too coarse for high moments.
   sp.simpoints         = 6000;
   sp.radial_resolution = 1.0e-3;
   sp.meshType          = US_SimulationParameters::ASTFEM;
   sp.gridType          = US_SimulationParameters::MOVING;

   US_SimulationParameters::SpeedProfile prof;
   prof.rotorspeed      = qRound( tm.rpm );
   prof.acceleration    = 400;
   prof.scans           = n_scans;
   prof.time_first      = qRound( t_start );
   prof.time_last       = qRound( t_end );
   prof.duration_hours  = int( t_end / 3600.0 );
   prof.duration_minutes= ( t_end - prof.duration_hours * 3600.0 ) / 60.0;
   prof.delay_hours     = 0;
   prof.delay_minutes   = t_start / 60.0;
   sp.speed_step << prof;

   return sp;
}

// Allocate the RawData container the solver fills in.
US_DataIO::RawData make_container( const US_SimulationParameters& sp,
                                   const TestModel& tm,
                                   int n_scans, double t_start, double t_end )
{
   US_DataIO::RawData data;
   data.type[ 0 ] = 'R';  data.type[ 1 ] = 'I';
   data.cell      = 1;
   data.channel   = 'A';
   data.description = tm.id;

   const int n_r = int( ( sp.bottom - sp.meniscus ) / sp.radial_resolution ) + 1;
   for ( int j = 0; j < n_r; j++ )
      data.xvalues << sp.meniscus + j * sp.radial_resolution;

   const double omega = tm.rpm * M_PI / 30.0;
   for ( int i = 0; i < n_scans; i++ )
   {
      US_DataIO::Scan scan;
      scan.seconds = t_start + ( t_end - t_start ) * i / double( n_scans - 1 );
      scan.rpm     = tm.rpm;
      scan.omega2t = omega * omega * scan.seconds;
      scan.wavelength = 280.0;
      scan.rvalues.fill( 0.0, n_r );
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

   const QString outdir = ( argc > 1 ) ? QString( argv[ 1 ] ) : QString( "out" );
   QDir().mkpath( outdir );

   const int    n_scans = 100;
   const double t_start = 600.0;
   const double t_end   = 22000.0;

   for ( const TestModel& tm : frozen_test_set() )
   {
      US_Model                model  = build_model( tm );
      US_SimulationParameters params = build_params( tm, n_scans, t_start, t_end );
      US_DataIO::RawData      data   = make_container( params, tm, n_scans,
                                                       t_start, t_end );

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
