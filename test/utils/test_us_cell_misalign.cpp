//! \file test_us_cell_misalign.cpp
// Tests for the rotational cell-misalignment model (US_CellMisalignment) and
// its use in the ASTFVM solver (US_LammAstfvm).
#include "qt_test_base.h"
#include "us_cell_misalign.h"
#include "us_lamm_astfvm.h"
#include "us_model.h"
#include "us_simparms.h"
#include "us_dataIO.h"
#include <QTemporaryDir>
#include <cmath>

namespace
{
   const double R_M   = 6.0;
   const double R_B   = 7.2;
   const double S_VAL = 4.4e-13;
   const double D_VAL = 6.0e-7;
   const int    RPM   = 50000;

   // Constant-speed synthetic run: radii 6.0..7.2 cm, scans every 600 s
   US_DataIO::RawData makeGrid( int nscans = 10 )
   {
      US_DataIO::RawData rd;
      rd.type[ 0 ] = 'R';
      rd.type[ 1 ] = 'A';
      memset( rd.rawGUID, 0, sizeof( rd.rawGUID ) );
      rd.cell        = 1;
      rd.channel     = 'A';
      rd.description = "misalign";
      for ( int rr = 0; rr <= 1200; rr++ )
         rd.xvalues << R_M + rr * 0.001;

      const double w2 = std::pow( RPM * M_PI / 30.0, 2 );
      for ( int ss = 0; ss < nscans; ss++ )
      {
         US_DataIO::Scan scan;
         scan.temperature = 20.0;
         scan.rpm         = RPM;
         scan.seconds     = 600.0 * ( ss + 1 );
         scan.omega2t     = w2 * scan.seconds;
         scan.wavelength  = 280.0;
         scan.plateau     = 0.0;
         scan.delta_r     = 0.001;
         scan.nz_stddev   = false;
         scan.rvalues.fill( 0.0, rd.xvalues.size() );
         rd.scanData << scan;
      }
      return rd;
   }

   US_SimulationParameters makeParams( double angle, double kappa )
   {
      US_SimulationParameters sp;
      sp.speed_step[ 0 ].rotorspeed = RPM;
      sp.speed_step[ 0 ].avg_speed  = RPM;
      sp.meniscus       = R_M;
      sp.bottom         = R_B;
      sp.cp_angle       = 2.5;
      sp.meshType       = US_SimulationParameters::ASTFVM;
      sp.misalign_angle = angle;
      sp.misalign_pivot = 6.5;
      sp.misalign_kappa = kappa;
      return sp;
   }

   US_DataIO::RawData simulate( double angle, double kappa )
   {
      US_Model model;
      US_Model::SimulationComponent sc;
      sc.s                    = S_VAL;
      sc.D                    = D_VAL;
      sc.vbar20               = 0.73;
      sc.signal_concentration = 1.0;
      model.components << sc;

      US_SimulationParameters sp = makeParams( angle, kappa );
      US_DataIO::RawData      rd = makeGrid();
      US_LammAstfvm           fvm( model, sp );
      EXPECT_EQ( fvm.calculate( rd ), 0 );
      return rd;
   }

   // Radius at which the scan first reaches half of its value at r = 6.9
   double midpoint( const US_DataIO::RawData& rd, int ss )
   {
      const int    jp   = (int)qRound( ( 6.9 - R_M ) / 0.001 );
      const double half = 0.5 * rd.value( ss, jp );
      for ( int jj = 1; jj < rd.xvalues.size(); jj++ )
      {
         if ( rd.value( ss, jj ) >= half )
         {
            double c0 = rd.value( ss, jj - 1 );
            double c1 = rd.value( ss, jj );
            return rd.xvalues[ jj - 1 ] + 0.001 * ( half - c0 ) / ( c1 - c0 );
         }
      }
      return -1.0;
   }
}

class TestUSCellMisalign : public QtTestBase {};

TEST_F( TestUSCellMisalign, AlignedCellIsInactive )
{
   US_CellMisalignment ma( 0.0 );
   EXPECT_FALSE( ma.active() );
   EXPECT_DOUBLE_EQ( ma.wall_offset( 1 ), 0.0 );
   EXPECT_DOUBLE_EQ( ma.velocity_eps( 6.2, R_B ), 0.0 );
   EXPECT_DOUBLE_EQ( ma.sink_rate( 6.2, 1.0 ), 0.0 );
   EXPECT_NEAR( ma.angular_width( 6.5 ) * 180.0 / M_PI, 2.5, 1e-12 );
   EXPECT_FALSE( US_CellMisalignment( 1.0, 2.5, 6.5, 0.0 ).active() );
}

// Reference values from scripts/misalignment/misalign1d.py
TEST_F( TestUSCellMisalign, GeometryMatchesReference )
{
   US_CellMisalignment ma( 1.0, 2.5, 6.5, 1.0 );
   EXPECT_TRUE( ma.active() );
   EXPECT_NEAR( ma.wall_tilt( 6.0 ), 1.0829, 1e-4 );
   EXPECT_NEAR( ma.wall_tilt( 6.5 ), 0.9996, 1e-4 );
   EXPECT_NEAR( ma.wall_tilt( 7.2 ), 0.9024, 1e-4 );
   // the angular width changes only at O(alpha delta^2)
   EXPECT_NEAR( ma.angular_width( 6.5 ) * 180.0 / M_PI, 2.5, 1e-3 );
   EXPECT_NEAR( ma.velocity_eps( 6.0, R_B ), 0.0866, 1e-4 );
   EXPECT_NEAR( ma.velocity_eps( 6.5, R_B ), 0.0431, 1e-4 );
   EXPECT_DOUBLE_EQ( ma.velocity_eps( R_B, R_B ), 0.0 );
   EXPECT_NEAR( ma.bottom_smear( R_B ) * 1e4, 54.83, 0.01 );
   EXPECT_NEAR( ma.beam_margin( 6.0 ) * 1e4, 1222.0, 1.0 );
   EXPECT_NEAR( US_CellMisalignment( 1.0, 2.5, 6.5, 1.0, 1.5 ).radial_shift() * 1e4, -29.70, 0.01 );

   // sink rate is -(1/r) d(r u)/dr for u = s w2 r eps:  check numerically
   const double sw2 = 1.0e-5;
   const double r   = 6.4;
   const double h   = 1.0e-5;
   auto ru = [&]( double x ) { return x * sw2 * x * ma.velocity_eps( x, R_B ); };
   double kfd = -( ru( r + h ) - ru( r - h ) ) / ( 2.0 * h ) / r;
   EXPECT_NEAR( ma.sink_rate( r, sw2 ) / kfd, 1.0, 1e-3 );
}

TEST_F( TestUSCellMisalign, SimparmsXmlRoundTrip )
{
   QTemporaryDir tmp;
   ASSERT_TRUE( tmp.isValid() );
   QString fn = tmp.filePath( "sp.xml" );

   US_SimulationParameters sp = makeParams( 0.75, 0.3 );
   sp.misalign_pivot          = 6.45;
   ASSERT_EQ( sp.save_simparms( fn ), 0 );

   US_SimulationParameters rd;
   ASSERT_EQ( rd.load_simparms( fn ), 0 );
   EXPECT_DOUBLE_EQ( rd.misalign_angle, 0.75 );
   EXPECT_DOUBLE_EQ( rd.misalign_pivot, 6.45 );
   EXPECT_DOUBLE_EQ( rd.misalign_kappa, 0.3 );

   // aligned:  attributes are not written, defaults are kept on reading
   US_SimulationParameters al = makeParams( 0.0, 1.0 );
   ASSERT_EQ( al.save_simparms( fn ), 0 );
   QFile xf( fn );
   ASSERT_TRUE( xf.open( QIODevice::ReadOnly ) );
   EXPECT_FALSE( QString( xf.readAll() ).contains( "misalign" ) );
}

TEST_F( TestUSCellMisalign, ZeroKappaEqualsAlignedCell )
{
   US_DataIO::RawData al = simulate( 0.0, 1.0 );
   US_DataIO::RawData k0 = simulate( 2.0, 0.0 );
   for ( int ss = 0; ss < al.scanData.size(); ss++ )
      for ( int jj = 0; jj < al.xvalues.size(); jj += 50 )
         ASSERT_DOUBLE_EQ( al.value( ss, jj ), k0.value( ss, jj ) );
}

// Misalignment moves the boundary faster as predicted by the sharp-boundary
// limit of the closure, leaves the plateau unchanged and conserves mass
TEST_F( TestUSCellMisalign, BoundaryAcceleratesPlateauUnchanged )
{
   US_DataIO::RawData al = simulate( 0.0, 1.0 );
   US_DataIO::RawData ma = simulate( 1.0, 1.0 );
   US_CellMisalignment geo( 1.0, 2.5, 6.5, 1.0 );
   const double        w2 = std::pow( RPM * M_PI / 30.0, 2 );
   const int           jp = (int)qRound( ( 6.95 - R_M ) / 0.001 );

   for ( int ss = 2; ss < al.scanData.size(); ss++ )
   {
      SCOPED_TRACE( ss );
      double rb_al = midpoint( al, ss );
      double rb_ma = midpoint( ma, ss );
      EXPECT_GT( rb_ma, rb_al );

      // integrate the sharp-boundary ODE  dr/dt = s w2 r (1 + eps(r))
      double rb = R_M;
      double t  = 0.0;
      const double tt = al.scanData[ ss ].seconds;
      const double dt = 1.0;
      while ( t < tt - 1e-9 )
      {
         auto f = [&]( double x ) { return S_VAL * w2 * x * ( 1.0 + geo.velocity_eps( x, R_B ) ); };
         double k1 = f( rb );
         double k2 = f( rb + 0.5 * dt * k1 );
         double k3 = f( rb + 0.5 * dt * k2 );
         double k4 = f( rb + dt * k3 );
         rb += dt * ( k1 + 2 * k2 + 2 * k3 + k4 ) / 6.0;
         t  += dt;
      }
      double rb_ideal = R_M * std::exp( S_VAL * w2 * tt );
      // shift of the boundary relative to the aligned cell:  within 3 %
      EXPECT_NEAR( ( rb_ma - rb_al ) / ( rb - rb_ideal ), 1.0, 0.03 );

      // plateau concentration unchanged (radial dilution only)
      EXPECT_NEAR( ma.value( ss, jp ) / al.value( ss, jp ), 1.0, 2e-3 );
   }
}
