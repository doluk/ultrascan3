//! \file test_us_math_bf.cpp
// test_us_math_bf.cpp - Unit tests for the band-forming gradient (US_Math_BF)
#include "qt_test_base.h"
#include "us_buffer.h"
#include "us_constants.h"
#include "us_dataIO.h"
#include "us_math2.h"
#include "us_math_bf.h"
#include "us_simparms.h"
#include <cmath>
#include <random>
#include <thread>
#include <vector>

namespace
{
// H2O overlaid on a D2O buffer, as in Dobler et al. (2026): the overlay
//  component lowers density and viscosity by 1.93e-3 g/mL and 4.5e-3 cP per M
const double BASE_DENS = 1.00599 + 1.93e-3 * 55.51;
const double BASE_VISC = 1.01960 + 4.50e-3 * 55.51;

QList<US_CosedComponent> components()
{
   US_CosedComponent h2o;
   h2o.name          = "H2O";
   h2o.conc          = 55.51;
   h2o.overlaying    = true;
   h2o.s_coeff       = 0.0;
   h2o.d_coeff       = 1.8e-5;
   h2o.vbar          = 1.0;
   h2o.dens_coeff[0] = BASE_DENS;
   h2o.visc_coeff[0] = BASE_VISC;
   h2o.dens_coeff[2] = -1.93e-3;
   h2o.visc_coeff[2] = -4.50e-3;

   US_CosedComponent base;
   base.name          = "D2O buffer";
   base.conc          = 1.0;
   base.overlaying    = false;
   base.s_coeff       = 0.0;
   base.d_coeff       = 0.0;
   base.vbar          = 1.0;
   base.dens_coeff[0] = BASE_DENS;
   base.visc_coeff[0] = BASE_VISC;

   return QList<US_CosedComponent>() << h2o << base;
}

// Geometry of the simulated band-forming experiment of Dobler et al. (2026)
const double MENISCUS = 6.1;
const double BOTTOM   = 7.2;
const double VOLUME   = 0.015;  // mL
const double PATHLEN  = 1.2;
const double ANGLE    = 2.5;

US_Math_BF::Band_Forming_Gradient makeGradient( double meniscus = MENISCUS )
{
   QList<US_CosedComponent> comps = components();
   US_Math_BF::Band_Forming_Gradient bfg( meniscus, BOTTOM, VOLUME, comps, PATHLEN, ANGLE );
   bfg.get_eigenvalues();
   return bfg;
}

// Three scans at 20 C and 50 krpm up to last_time
void makeRun( double meniscus, double last_time, US_SimulationParameters& sp, US_DataIO::RawData& raw )
{
   sp.meniscus          = meniscus;
   sp.bottom            = BOTTOM;
   sp.band_forming      = true;
   sp.band_volume       = VOLUME;
   sp.cp_pathlen        = PATHLEN;
   sp.cp_angle          = ANGLE;
   sp.meshType          = US_SimulationParameters::ASTFVM;
   sp.radial_resolution = 0.001;
   sp.temperature       = 20.0;
   for ( int ii = 0; ii < 11; ii++ )
   {
      raw.xvalues << meniscus + ii * ( BOTTOM - meniscus ) / 10.0;
   }
   for ( int ss = 0; ss < 3; ss++ )
   {
      US_DataIO::Scan scan;
      scan.seconds     = last_time * ( ss + 1 ) / 3.0;
      scan.temperature = 20.0;
      scan.rpm         = 50000.0;
      scan.omega2t     = scan.seconds * pow( 50000.0 * M_PI / 30.0, 2 );
      scan.rvalues.fill( 0.0, raw.xvalues.size() );
      raw.scanData << scan;
   }
}
} // namespace

class TestUSMathBF : public QtTestBase
{
};

// Two components with equal values are equal (the coefficient arrays were
//  compared by address, so no two distinct lists were ever equal)
TEST_F( TestUSMathBF, CosedComponentEqualityComparesCoefficients )
{
   QList<US_CosedComponent> aa = components();
   QList<US_CosedComponent> bb = components();
   EXPECT_TRUE( aa == bb );

   bb[0].dens_coeff[2] += 1.0e-6;
   EXPECT_FALSE( aa == bb );
}

// The expansion against values from an independent evaluation with SciPy's
//  Bessel functions and Brent roots of the eigenvalue condition
TEST_F( TestUSMathBF, ConcentrationMatchesReferenceExpansion )
{
   US_Math_BF::Band_Forming_Gradient bfg = makeGradient();
   EXPECT_EQ( bfg.eigenvalues.size(), 1750 );

   const double ref[][3] = { { 6.12, 60.0, 35.588366118956 },
                             { 6.15, 600.0, 12.951930274109 },
                             { 6.4, 3000.0, 3.998079677886 },
                             { 7.0, 20000.0, 1.981818901746 } };
   for ( const auto& rr : ref )
   {
      double dens, visc, conc;
      ASSERT_TRUE( bfg.calc_dens_visc( rr[0], rr[1], 293.15, dens, visc, conc ) );
      EXPECT_NEAR( conc, rr[2], 1.0e-8 ) << "x " << rr[0] << " t " << rr[1];
      EXPECT_NEAR( dens, BASE_DENS - 1.93e-3 * conc, 1.0e-12 );
      EXPECT_NEAR( visc, BASE_VISC - 4.50e-3 * conc, 1.0e-12 );
   }
}

// The scalar and the array interface share one cache: whichever runs first,
//  both give the same density and viscosity
TEST_F( TestUSMathBF, ScalarAndArrayInterfacesAgreeInAnyOrder )
{
   US_Math_BF::Band_Forming_Gradient bfg = makeGradient();
   const double tt = 600.0;
   const double TT = 293.15;

   const double x1 = 6.1234;
   double d1, v1, c1;
   bfg.calc_dens_visc( x1, tt, TT, d1, v1, c1 );
   double dd1[1] = { BASE_DENS };
   double vv1[1] = { BASE_VISC };
   bfg.calc_dens_visc( 1, &x1, tt, TT, dd1, vv1 );
   EXPECT_DOUBLE_EQ( dd1[0], d1 );
   EXPECT_DOUBLE_EQ( vv1[0], v1 );

   const double x2 = 6.1500;
   double dd2[1] = { BASE_DENS };
   double vv2[1] = { BASE_VISC };
   bfg.calc_dens_visc( 1, &x2, tt, TT, dd2, vv2 );
   double d2, v2, c2;
   bfg.calc_dens_visc( x2, tt, TT, d2, v2, c2 );
   EXPECT_DOUBLE_EQ( dd2[0], d2 );
   EXPECT_DOUBLE_EQ( vv2[0], v2 );
   // The overlay has reached x2 by now
   EXPECT_LT( d2, BASE_DENS - 1.0e-3 );
}

// Radii closer than the old cache keys' bins get their own values
TEST_F( TestUSMathBF, NearbyRadiiAreNotMerged )
{
   US_Math_BF::Band_Forming_Gradient bfg = makeGradient();
   double da, va, ca;
   double db, vb, cb;
   bfg.calc_dens_visc( 6.15000, 30.0, 293.15, da, va, ca );
   bfg.calc_dens_visc( 6.15005, 30.0, 293.15, db, vb, cb );
   EXPECT_NE( ca, cb );
}

// is_suitable() accepts the geometry the gradient was made for, as long as
//  its table reaches the time asked for, and nothing else
TEST_F( TestUSMathBF, IsSuitableChecksGeometryAndTime )
{
   US_Math_BF::Band_Forming_Gradient bfg = makeGradient();
   QList<US_CosedComponent> comps = components();
   EXPECT_FALSE( bfg.is_suitable( MENISCUS, BOTTOM, VOLUME, PATHLEN, ANGLE, comps, 1000 ) )
      << "no table yet";

   US_SimulationParameters sp;
   US_DataIO::RawData raw;
   makeRun( MENISCUS, 1500.0, sp, raw );
   bfg.calculate_gradient( sp, &raw );
   ASSERT_GT( bfg.dens_bfg_data.scanCount(), 0 );

   EXPECT_TRUE( bfg.is_suitable( MENISCUS, BOTTOM, VOLUME, PATHLEN, ANGLE, comps, 1500 ) );
   EXPECT_FALSE( bfg.is_suitable( MENISCUS, BOTTOM, VOLUME, PATHLEN, ANGLE, comps, 100000 ) )
      << "table too short";
   EXPECT_FALSE( bfg.is_suitable( MENISCUS + 0.003, BOTTOM, VOLUME, PATHLEN, ANGLE, comps, 1500 ) )
      << "meniscus of another fit point";
   EXPECT_FALSE( bfg.is_suitable( MENISCUS, BOTTOM - 0.003, VOLUME, PATHLEN, ANGLE, comps, 1500 ) );
   EXPECT_FALSE( bfg.is_suitable( MENISCUS, BOTTOM, VOLUME + 0.001, PATHLEN, ANGLE, comps, 1500 ) );
   EXPECT_FALSE( bfg.is_suitable( MENISCUS, BOTTOM, VOLUME, PATHLEN, ANGLE + 0.05, comps, 1500 ) );

   QList<US_CosedComponent> other = components();
   other[0].d_coeff = 2.0e-5;
   EXPECT_FALSE( bfg.is_suitable( MENISCUS, BOTTOM, VOLUME, PATHLEN, ANGLE, other, 1500 ) );
}

// The overlay's amount of H2O stays in the sector at all times
TEST_F( TestUSMathBF, OverlayMassIsConserved )
{
   US_Math_BF::Band_Forming_Gradient bfg = makeGradient();
   const int    nn    = 4001;
   const double dr    = ( BOTTOM - MENISCUS ) / ( nn - 1 );
   const double theta = ANGLE * M_PI / 180.0;
   for ( double tt : { 2.0, 100.0, 1000.0, 10000.0 } )
   {
      double mass = 0.0;
      for ( int ii = 0; ii < nn; ii++ )
      {
         const double rr = MENISCUS + ii * dr;
         double dens, visc, conc;
         bfg.calc_dens_visc( rr, tt, 293.15, dens, visc, conc );
         mass += ( ii == 0 || ii == nn - 1 ? 0.5 : 1.0 ) * conc * rr * dr;
      }
      mass *= theta * PATHLEN;                     // mmol, with mL and mol/L
      EXPECT_NEAR( mass / ( 55.51 * VOLUME ), 1.0, 2.0e-4 ) << "t " << tt;
   }
}

// Every point of the gradient table equals the direct evaluation, also ahead
//  of the H2O front (an early stop there froze density and viscosity)
TEST_F( TestUSMathBF, TableEqualsDirectEvaluationEverywhere )
{
   US_Math_BF::Band_Forming_Gradient bfg = makeGradient();
   US_SimulationParameters sp;
   US_DataIO::RawData raw;
   makeRun( MENISCUS, 1500.0, sp, raw );
   bfg.calculate_gradient( sp, &raw );
   ASSERT_GT( bfg.dens_bfg_data.scanCount(), 10 );

   US_Math_BF::Band_Forming_Gradient direct = makeGradient();
   double max_ddens = 0.0;
   double max_dvisc = 0.0;
   for ( int ss = 1; ss < bfg.dens_bfg_data.scanCount(); ss += 7 )
   {
      const US_DataIO::Scan& dscan = bfg.dens_bfg_data.scanData[ ss ];
      for ( int ii = 0; ii < bfg.dens_bfg_data.pointCount(); ii += 3 )
      {
         double dens, visc, conc;
         direct.calc_dens_visc( bfg.dens_bfg_data.xvalues[ ii ], dscan.seconds, dscan.temperature, dens, visc, conc );
         max_ddens = qMax( max_ddens, fabs( dscan.rvalues[ ii ] - dens ) );
         max_dvisc = qMax( max_dvisc, fabs( bfg.visc_bfg_data.scanData[ ss ].rvalues[ ii ] - visc ) );
      }
   }
   EXPECT_LT( max_ddens, 1.0e-12 );
   EXPECT_LT( max_dvisc, 1.0e-12 );
}

// One gradient, and a copy sharing its tables, used by several threads at
//  once: every thread gets exactly the single-thread values (the caches are
//  per thread, and the shared tables are only read)
TEST_F( TestUSMathBF, SharedGradientGivesTheSameValuesInThreads )
{
   US_Math_BF::Band_Forming_Gradient bfg = makeGradient();
   US_SimulationParameters sp;
   US_DataIO::RawData raw;
   makeRun( MENISCUS, 1500.0, sp, raw );
   bfg.calculate_gradient( sp, &raw );
   US_Math_BF::Band_Forming_Gradient copy( bfg );   // shares the table arrays

   // Points and times off the table grid, and the table interpolation
   const int npts = 400;
   std::vector<double> xs( npts );
   std::vector<double> ts( npts );
   std::mt19937 gen( 7 );
   std::uniform_real_distribution<double> ux( MENISCUS, BOTTOM );
   std::uniform_real_distribution<double> ut( 1.0, 1500.0 );
   for ( int ii = 0; ii < npts; ii++ )
   {
      xs[ ii ] = ux( gen );
      ts[ ii ] = ut( gen );
   }
   std::sort( xs.begin(), xs.end() );

   // Everything one thread computes, in one vector
   auto work = [ & ]( US_Math_BF::Band_Forming_Gradient* gg, int rounds ) {
      std::vector<double> out;
      for ( int rr = 0; rr < rounds; rr++ )
      {
         for ( int ii = 0; ii < npts; ii += 4 )
         {
            double dens, visc, conc;
            gg->calc_dens_visc( xs[ ii ], ts[ ii ], 293.15, dens, visc, conc );
            out.push_back( dens );
            out.push_back( visc );
            double dd[1] = { BASE_DENS };
            double vv[1] = { BASE_VISC };
            gg->calc_dens_visc( 1, &xs[ ii ], ts[ ii ], 293.15, dd, vv );
            out.push_back( dd[0] );
         }
         std::vector<double> dens( npts, BASE_DENS );
         std::vector<double> visc( npts, BASE_VISC );
         std::vector<double> conc( npts, 0.0 );
         gg->interpolateCCodiff( npts, xs.data(), ts[ rr % npts ], 293.15, dens.data(), visc.data(), conc.data() );
         out.insert( out.end(), dens.begin(), dens.end() );
         out.insert( out.end(), visc.begin(), visc.end() );
      }
      return out;
   };

   const int rounds = 3;
   const std::vector<double> expected = work( &bfg, rounds );

   const int nthreads = 8;
   std::vector<std::vector<double>> results( nthreads );
   std::vector<std::thread> threads;
   for ( int tt = 0; tt < nthreads; tt++ )
   {
      US_Math_BF::Band_Forming_Gradient* gg = ( tt % 2 == 0 ) ? &bfg : &copy;
      threads.emplace_back( [ &, tt, gg ]() { results[ tt ] = work( gg, rounds ); } );
   }
   for ( std::thread& thread : threads )
   {
      thread.join();
   }
   for ( int tt = 0; tt < nthreads; tt++ )
   {
      ASSERT_EQ( results[ tt ].size(), expected.size() ) << "thread " << tt;
      EXPECT_TRUE( results[ tt ] == expected ) << "thread " << tt;
   }
}

// Threads that each need the eigenvalues first find them computed once
TEST_F( TestUSMathBF, EigenvaluesAreSetUpOnceForConcurrentInterpolation )
{
   QList<US_CosedComponent> comps = components();
   US_Math_BF::Band_Forming_Gradient bfg( MENISCUS, BOTTOM, VOLUME, comps, PATHLEN, ANGLE );
   // No eigenvalues yet, and no table: the ASTFEM mesh type evaluates directly
   bfg.simparms.meshType = US_SimulationParameters::ASTFEM;

   const double xx[3] = { 6.12, 6.2, 6.5 };
   std::vector<std::vector<double>> results( 6 );
   std::vector<std::thread> threads;
   for ( int tt = 0; tt < 6; tt++ )
   {
      threads.emplace_back( [ &, tt ]() {
         std::vector<double> dens( 3, BASE_DENS );
         std::vector<double> visc( 3, BASE_VISC );
         std::vector<double> conc( 3, 0.0 );
         bfg.interpolateCCodiff( 3, xx, 300.0, 293.15, dens.data(), visc.data(), conc.data() );
         results[ tt ] = dens;
      } );
   }
   for ( std::thread& thread : threads )
   {
      thread.join();
   }
   EXPECT_EQ( bfg.eigenvalues.size(), 1750 );
   for ( int tt = 1; tt < 6; tt++ )
   {
      EXPECT_TRUE( results[ tt ] == results[ 0 ] ) << "thread " << tt;
   }
   EXPECT_LT( results[ 0 ][ 0 ], BASE_DENS - 1.0e-3 );
}

// D of the co-diffusing component scales with T / eta_water(T): the profile at
//  T over t is the one at 20 C over t * (T / 293.15) * eta_w(20) / eta_w(T)
TEST_F( TestUSMathBF, CodiffusionScalesWithWaterViscosity )
{
   US_Math_BF::Band_Forming_Gradient bfg = makeGradient();
   double dens_w20, visc_w20;
   US_Math2::water_properties( 20.0, dens_w20, visc_w20 );
   for ( double tc : { 4.0, 25.0, 37.0 } )
   {
      double dens_w, visc_w;
      US_Math2::water_properties( tc, dens_w, visc_w );
      const double scale = ( tc + K0 ) / K20 * visc_w20 / visc_w;
      for ( double xx : { 6.13, 6.25 } )
      {
         double dT, vT, cT, d20, v20, c20;
         bfg.calc_dens_visc( xx, 400.0, tc + K0, dT, vT, cT );
         bfg.calc_dens_visc( xx, 400.0 * scale, 20.0 + K0, d20, v20, c20 );
         EXPECT_NEAR( cT, c20, 1.0e-9 ) << "T " << tc << " x " << xx;
      }
   }
   // At 4 C water is 1.56 times as viscous as at 20 C: the H2O spreads slower
   double c4, c20, dd, vv;
   bfg.calc_dens_visc( 6.25, 400.0, 4.0 + K0, dd, vv, c4 );
   bfg.calc_dens_visc( 6.25, 400.0, 20.0 + K0, dd, vv, c20 );
   EXPECT_LT( c4, 0.9 * c20 );
}

// The table follows the run's recorded temperature
TEST_F( TestUSMathBF, TableFollowsTheRunTemperature )
{
   US_Math_BF::Band_Forming_Gradient bfg = makeGradient();
   US_SimulationParameters sp;
   US_DataIO::RawData raw;
   makeRun( MENISCUS, 1500.0, sp, raw );
   for ( US_DataIO::Scan& scan : raw.scanData )
   {
      scan.temperature = 25.0;
   }
   bfg.calculate_gradient( sp, &raw );

   US_Math_BF::Band_Forming_Gradient direct = makeGradient();
   double max_dc = 0.0;
   for ( int ss = 1; ss < bfg.conc_bfg_data.scanCount(); ss += 5 )
   {
      const US_DataIO::Scan& cscan = bfg.conc_bfg_data.scanData[ ss ];
      for ( int ii = 0; ii < bfg.conc_bfg_data.pointCount(); ii += 7 )
      {
         double dens, visc, conc;
         direct.calc_dens_visc( bfg.conc_bfg_data.xvalues[ ii ], cscan.seconds, 25.0 + K0, dens, visc, conc );
         max_dc = qMax( max_dc, fabs( cscan.rvalues[ ii ] - conc ) );
      }
   }
   EXPECT_LT( max_dc, 1.0e-9 );
}

// s and D in the gradient at T: the standard correction of a buffer with the
//  local 20 C density and viscosity (US_Math2::data_correction, not manual)
TEST_F( TestUSMathBF, AdjustSdIsTheStandardCorrectionOfTheLocalBuffer )
{
   US_Math_BF::Band_Forming_Gradient bfg = makeGradient();
   const double vbar = 0.7468;
   const double s20w = 2.0e-13;
   const double D20w = 1.1e-6;
   for ( double tc : { 4.0, 20.0, 25.0 } )
   {
      const double xx = 6.16;
      const double tt = 900.0;
      double dens, visc, conc;
      bfg.calc_dens_visc( xx, tt, tc + K0, dens, visc, conc );

      US_Math2::SolutionData sd{};
      sd.density   = dens;
      sd.viscosity = visc;
      sd.vbar20    = vbar;
      sd.vbar      = vbar;
      sd.manual    = false;
      US_Math2::data_correction( tc, sd );

      double ss = s20w;
      double dd = D20w;
      ASSERT_TRUE( bfg.adjust_sd( xx, tt, ss, dd, tc + K0, vbar ) );
      EXPECT_NEAR( ss / ( s20w / sd.s20w_correction ), 1.0, 1.0e-12 ) << "T " << tc;
      EXPECT_NEAR( dd / ( D20w / sd.D20w_correction ), 1.0, 1.0e-12 ) << "T " << tc;
   }
}

// A component in both layers: the order of the list does not matter, and
//  the overlay excess starts at the overlay's and lower solution's values
TEST_F( TestUSMathBF, ComponentOrderDoesNotMatter )
{
   US_CosedComponent base = components()[ 1 ];
   US_CosedComponent lo   = components()[ 0 ];
   lo.name = "NaCl"; lo.conc = 0.1; lo.overlaying = false; lo.d_coeff = 1.5e-5;
   lo.dens_coeff[2] = 0.04; lo.visc_coeff[2] = 0.0;
   US_CosedComponent up = lo;
   up.conc = 0.5; up.overlaying = true;

   const double h  = sqrt( MENISCUS * MENISCUS + VOLUME * 360.0 / ( ANGLE * PATHLEN * M_PI ) ) - MENISCUS;
   const double vt = M_PI * PATHLEN * ANGLE / 360.0 * ( BOTTOM * BOTTOM - MENISCUS * MENISCUS );
   for ( int order = 0; order < 2; order++ )
   {
      QList<US_CosedComponent> comps;
      comps << base;
      if ( order == 0 ) comps << up << lo; else comps << lo << up;
      US_Math_BF::Band_Forming_Gradient bfg( MENISCUS, BOTTOM, VOLUME, comps, PATHLEN, ANGLE );
      bfg.get_eigenvalues();
      double dens, visc, cin, cbelow, ceq;
      bfg.calc_dens_visc( MENISCUS + 0.5 * h, 0.5, 293.15, dens, visc, cin );
      bfg.calc_dens_visc( MENISCUS + 3.0 * h, 0.5, 293.15, dens, visc, cbelow );
      bfg.calc_dens_visc( 6.8, 1.0e7, 293.15, dens, visc, ceq );
      EXPECT_NEAR( cin, 0.5, 1.0e-12 ) << "order " << order;
      EXPECT_NEAR( cbelow, 0.1, 1.0e-12 ) << "order " << order;
      EXPECT_NEAR( ceq, 0.1 + 0.4 * VOLUME / vt, 1.0e-6 ) << "order " << order;
   }
}

// The table's last interval interpolates up to the last scan
TEST_F( TestUSMathBF, TableInterpolatesTheLastInterval )
{
   US_Math_BF::Band_Forming_Gradient bfg = makeGradient();
   US_SimulationParameters sp;
   US_DataIO::RawData raw;
   makeRun( MENISCUS, 6000.0, sp, raw );
   bfg.calculate_gradient( sp, &raw );
   const int ns = bfg.conc_bfg_data.scanCount();
   ASSERT_GT( ns, 2 );
   const double tlast = bfg.conc_bfg_data.scanData[ ns - 1 ].seconds;
   const double xx    = 6.6;
   double dens[1] = { BASE_DENS }, visc[1] = { BASE_VISC }, conc[1] = { 0.0 };
   bfg.interpolateCCodiff( 1, &xx, tlast, 293.15, dens, visc, conc );
   double dd, vv, cc;
   bfg.calc_dens_visc( xx, tlast, 293.15, dd, vv, cc );
   EXPECT_NEAR( conc[0], cc, 1.0e-6 );
}

// An overlay component that does not diffuse still gives a table to use
TEST_F( TestUSMathBF, NonDiffusingOverlayComponentGivesAUsableTable )
{
   QList<US_CosedComponent> comps = components();
   comps[ 0 ].d_coeff = 0.0;
   US_Math_BF::Band_Forming_Gradient bfg( MENISCUS, BOTTOM, VOLUME, comps, PATHLEN, ANGLE );
   bfg.get_eigenvalues();
   US_SimulationParameters sp;
   US_DataIO::RawData raw;
   makeRun( MENISCUS, 600.0, sp, raw );
   bfg.calculate_gradient( sp, &raw );
   EXPECT_TRUE( std::isfinite( bfg.dt ) );
   EXPECT_GT( bfg.dens_bfg_data.scanCount(), 1 );
   const double xx = 6.3;
   double dens[1] = { BASE_DENS }, visc[1] = { BASE_VISC }, conc[1] = { 0.0 };
   bfg.interpolateCCodiff( 1, &xx, 300.0, 293.15, dens, visc, conc );
   // The H2O stays in the overlay; without diffusion nothing damps the
   //  series, so its 1750 terms show Gibbs ripple of ~0.05 % of the step
   EXPECT_NEAR( conc[0], 0.0, 0.002 * 55.51 );
}
