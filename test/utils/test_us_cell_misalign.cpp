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

   US_SimulationParameters makeParams( double angle, double gD = 0.033,
                                       double gs = 0.033 / 13.5 )
   {
      US_SimulationParameters sp;
      sp.speed_step[ 0 ].rotorspeed = RPM;
      sp.speed_step[ 0 ].avg_speed  = RPM;
      sp.meniscus         = R_M;
      sp.bottom           = R_B;
      sp.cp_angle         = 2.5;
      sp.meshType         = US_SimulationParameters::ASTFVM;
      sp.misalign_angle   = angle;
      sp.misalign_pivot   = 6.5;
      sp.misalign_gamma_D = gD;
      sp.misalign_gamma_s = gs;
      sp.misalign_conc    = 1.0;
      return sp;
   }

   US_DataIO::RawData simulate( const US_SimulationParameters& sp,
                                double s = S_VAL, double D = D_VAL )
   {
      US_Model model;
      US_Model::SimulationComponent sc;
      sc.s                    = s;
      sc.D                    = D;
      sc.vbar20               = 0.73;
      sc.signal_concentration = 1.0;
      model.components << sc;

      US_DataIO::RawData      rd = makeGrid();
      US_LammAstfvm           fvm( model, const_cast< US_SimulationParameters& >( sp ) );
      EXPECT_EQ( fvm.calculate( rd ), 0 );
      return rd;
   }

   // Mean position and variance of the boundary gradient (r < 7.0 cm)
   void boundary_moments( const US_DataIO::RawData& rd, int ss,
                          double& mean, double& var )
   {
      double m0 = 0.0, m1 = 0.0, m2 = 0.0;
      for ( int jj = 1; jj < rd.xvalues.size() && rd.xvalues[ jj ] < 7.0; jj++ )
      {
         double g = rd.value( ss, jj ) - rd.value( ss, jj - 1 );
         double r = 0.5 * ( rd.xvalues[ jj ] + rd.xvalues[ jj - 1 ] );
         m0 += g;  m1 += g * r;  m2 += g * r * r;
      }
      mean = m1 / m0;
      var  = m2 / m0 - mean * mean;
   }
}

class TestUSCellMisalign : public QtTestBase {};

TEST_F( TestUSCellMisalign, AlignedCellIsInactive )
{
   US_CellMisalignment ma( 0.0 );
   EXPECT_FALSE( ma.active() );
   EXPECT_DOUBLE_EQ( ma.wall_offset( 1 ), 0.0 );
   EXPECT_DOUBLE_EQ( ma.diffusion_eps(), 0.0 );
   EXPECT_DOUBLE_EQ( ma.sedimentation_eps(), 0.0 );
   EXPECT_NEAR( ma.angular_width( 6.5 ) * 180.0 / M_PI, 2.5, 1e-12 );
   US_CellMisalignment g0( 1.0, 2.5, 6.5 );
   g0.gamma_D = g0.gamma_s = 0.0;
   EXPECT_FALSE( g0.active() );
}

// Reference values from scripts/misalignment/misalign1d.py
TEST_F( TestUSCellMisalign, GeometryMatchesReference )
{
   US_CellMisalignment ma( 1.0, 2.5, 6.5 );
   EXPECT_TRUE( ma.active() );
   EXPECT_NEAR( ma.wall_tilt( 6.0 ), 1.0829, 1e-4 );
   EXPECT_NEAR( ma.wall_tilt( 6.5 ), 0.9996, 1e-4 );
   EXPECT_NEAR( ma.wall_tilt( 7.2 ), 0.9024, 1e-4 );
   EXPECT_NEAR( ma.tilt(), 0.9998, 1e-4 );
   // the angular width changes only at O(alpha delta^2)
   EXPECT_NEAR( ma.angular_width( 6.5 ) * 180.0 / M_PI, 2.5, 1e-3 );
   EXPECT_NEAR( ma.bottom_smear( R_B ) * 1e4, 54.83, 0.01 );
   EXPECT_NEAR( ma.beam_margin( 6.0 ) * 1e4, 1222.0, 1.0 );
   EXPECT_NEAR( US_CellMisalignment( 1.0, 2.5, 6.5, 1.5 ).radial_shift() * 1e4, -29.70, 0.01 );
}

// Calibrated closure (scripts/misalignment/README.md, section 9, sector 2D):
// eps_D = 3.3 % (psi / 1 deg) (c / 1 mg/mL)^-0.12, eps_s = eps_D / 13.5
TEST_F( TestUSCellMisalign, ClosureCalibration )
{
   US_CellMisalignment ma( 1.0, 2.5, 6.5 );
   EXPECT_NEAR( ma.diffusion_eps(), 0.033, 1e-4 );
   EXPECT_NEAR( ma.diffusion_eps() / ma.sedimentation_eps(), 13.5, 1e-9 );
   ma.conc = 0.01;
   EXPECT_NEAR( ma.diffusion_eps(), 0.0588, 2e-3 );      // 2D: 5.9 %
   ma.conc = 0.1;
   EXPECT_NEAR( ma.diffusion_eps(), 0.0439, 2e-3 );      // 2D: 4.4 %
   ma.conc = 1.0;
   ma.delta_deg = 2.0;
   EXPECT_NEAR( ma.diffusion_eps(), 0.066, 2e-4 );       // linear in psi
   ma.delta_deg = -2.0;
   EXPECT_NEAR( ma.diffusion_eps(), 0.066, 2e-4 );       // symmetric in +-psi
}

TEST_F( TestUSCellMisalign, SimparmsXmlRoundTrip )
{
   QTemporaryDir tmp;
   ASSERT_TRUE( tmp.isValid() );
   QString fn = tmp.filePath( "sp.xml" );

   US_SimulationParameters sp = makeParams( 0.75, 0.05, 0.004 );
   sp.misalign_pivot          = 6.45;
   sp.misalign_conc           = 0.7;
   ASSERT_EQ( sp.save_simparms( fn ), 0 );

   US_SimulationParameters rd;
   ASSERT_EQ( rd.load_simparms( fn ), 0 );
   EXPECT_DOUBLE_EQ( rd.misalign_angle,   0.75 );
   EXPECT_DOUBLE_EQ( rd.misalign_pivot,   6.45 );
   EXPECT_DOUBLE_EQ( rd.misalign_gamma_D, 0.05 );
   EXPECT_DOUBLE_EQ( rd.misalign_gamma_s, 0.004 );
   EXPECT_DOUBLE_EQ( rd.misalign_conc,    0.7 );

   // aligned:  attributes are not written, defaults are kept on reading
   US_SimulationParameters al = makeParams( 0.0 );
   ASSERT_EQ( al.save_simparms( fn ), 0 );
   QFile xf( fn );
   ASSERT_TRUE( xf.open( QIODevice::ReadOnly ) );
   EXPECT_FALSE( QString( xf.readAll() ).contains( "misalign" ) );
}

TEST_F( TestUSCellMisalign, ZeroGammaEqualsAlignedCell )
{
   US_DataIO::RawData al = simulate( makeParams( 0.0 ) );
   US_DataIO::RawData g0 = simulate( makeParams( 2.0, 0.0, 0.0 ) );
   for ( int ss = 0; ss < al.scanData.size(); ss++ )
      for ( int jj = 0; jj < al.xvalues.size(); jj += 50 )
         ASSERT_DOUBLE_EQ( al.value( ss, jj ), g0.value( ss, jj ) );
}

// The misaligned cell is equivalent to an aligned cell with s and D scaled
// by (1 + eps_s) and (1 + eps_D): boundary position and spreading agree to
// a small fraction of the misalignment effect; the plateau is unchanged
TEST_F( TestUSCellMisalign, EquivalentToScaledSD )
{
   const double psi = 2.0;
   US_SimulationParameters spm = makeParams( psi );
   US_CellMisalignment geo( psi, 2.5, 6.5 );
   US_DataIO::RawData al = simulate( makeParams( 0.0 ) );
   US_DataIO::RawData ma = simulate( spm );
   US_DataIO::RawData eq = simulate( makeParams( 0.0 ),
                                     S_VAL * ( 1.0 + geo.sedimentation_eps() ),
                                     D_VAL * ( 1.0 + geo.diffusion_eps() ) );
   const int jp = (int)qRound( ( 7.0 - R_M ) / 0.001 );

   for ( int ss = 2; ss < al.scanData.size(); ss++ )
   {
      SCOPED_TRACE( ss );
      double ra, va, rm, vm, re, ve;
      boundary_moments( al, ss, ra, va );
      boundary_moments( ma, ss, rm, vm );
      boundary_moments( eq, ss, re, ve );
      EXPECT_GT( vm / va - 1.0, 0.5 * geo.diffusion_eps() );   // dispersion
      EXPECT_GT( rm, ra );                                      // drift
      EXPECT_LT( qAbs( vm - ve ), 0.1 * ( vm - va ) );
      EXPECT_LT( qAbs( rm - re ), 0.1 * ( rm - ra ) );
      EXPECT_NEAR( ma.value( ss, jp ) / al.value( ss, jp ), 1.0, 2e-3 );
   }
}
