// test_us_solve_sim_vbar.cpp - Identifiability tests for vbar as a fitted
//                              dimension (3DSA).
//
// See doc/develop/3dsa_design.md.  These tests pin down the single fact that
// decides the shape of any 3DSA implementation:
//
//   The Lamm-equation forward model consumes only the experimental-space pair
//   (s*, D*).  vbar enters solely through the standard-space corrections, and
//   only through s* -- never through D*.  A grid over (s, f/f0, vbar) is
//   therefore a map from three coordinates onto two observables, whose fibres
//   are one-dimensional curves along which the simulated data are *identical*.
//
// Consequences encoded below:
//
//   * On a single dataset, vbar is not merely poorly determined, it is
//     completely unconstrained.  Molar masses differing by more than a factor
//     of two fit the same data exactly.  A 3DSA restricted to one dataset
//     would report a grid artefact, not a measurement.
//
//   * Across a buoyancy-contrast (density) series, the fibre separates and
//     vbar becomes identifiable, with a resolution set by the density
//     contrast.  This is why 3DSA is designed as a global, multi-dataset fit.
//
// If SingleDatasetFibreIsExact or LammSolverIgnoresVbar ever fails, the
// premise of the 3DSA design has changed and doc/develop/3dsa_design.md needs
// to be revisited before the gate on single-dataset fits is relaxed.
//
// The later tests cover the Phase 1 additions built on that premise:
// US_SolveSim::buoyancy_contrast(), vbar_resolution(), the gate thresholds,
// and the DataSet::fit_vbar flag that tells calc_residuals() to recompute the
// buffer corrections per solute.

#include "qt_test_base.h"

#include "us_model.h"
#include "us_math2.h"
#include "us_constants.h"
#include "us_simparms.h"
#include "us_astfem_rsa.h"
#include "us_dataIO.h"
#include "us_solve_sim.h"

#include <cmath>
#include <memory>
#include <vector>

namespace {

// A buffer, reduced to the three properties the corrections depend on.
struct Buffer
{
   double density;       // g/mL at 20 C
   double viscosity;     // cP at 20 C
   double temperature;   // run temperature, degrees C
};

// Water, and three contrast buffers spanning the useful range.
const Buffer BUF_WATER { 0.998234, 1.001940, 20.0 };
const Buffer BUF_D2O   { 1.105000, 1.251000, 20.0 };  // 100% D2O
const Buffer BUF_HALF  { 1.050000, 1.120000, 20.0 };  // ~50% D2O
const Buffer BUF_WEAK  { 1.010000, 1.020000, 20.0 };  // insufficient contrast

// Reference species: 4 S, f/f0 = 1.5, vbar = 0.73 mL/g.
const double REF_S    = 4.0e-13;
const double REF_FF0  = 1.5;
const double REF_VBAR = 0.73;

// The vbar interval a 3DSA grid would plausibly span for a protein.
const double VBAR_LO  = 0.60;
const double VBAR_HI  = 0.85;

// Gate thresholds, in (mL/g)^-1.  Phase 1 moved these into US_SolveSim, so
// take them from there rather than restating the design document.
const double GATE_REFUSE = US_SolveSim::VBAR_CONTRAST_REFUSE;
const double GATE_WARN   = US_SolveSim::VBAR_CONTRAST_WARN;

// Experimental-space coefficients plus the standard-space values they came
// from, i.e. everything calc_residuals() derives for one grid point.
struct Obs
{
   double s_exp;
   double D_exp;
   double ff0;
   double mw;
   double D20w;
};

US_Math2::SolutionData corrections( double vbar20, const Buffer& b )
{
   US_Math2::SolutionData sd;
   sd.density   = b.density;
   sd.viscosity = b.viscosity;
   sd.manual    = false;
   sd.vbar20    = vbar20;
   sd.vbar      = US_Math2::adjust_vbar20( vbar20, b.temperature );
   US_Math2::data_correction( b.temperature, sd );
   return sd;
}

// (s20w, f/f0, vbar) -> experimental space, exactly as calc_residuals does.
Obs forward_from_sk( double s20w, double ff0, double vbar20, const Buffer& b )
{
   US_Model::SimulationComponent c;
   c.s      = s20w;
   c.f_f0   = ff0;
   c.vbar20 = vbar20;
   c.D      = 0.0;
   c.mw     = 0.0;
   c.f      = 0.0;
   EXPECT_TRUE( US_Model::calc_coefficients( c ) );

   US_Math2::SolutionData sd = corrections( vbar20, b );
   return { c.s / sd.s20w_correction, c.D / sd.D20w_correction,
            c.f_f0, c.mw, c.D };
}

// (s20w, D20w, vbar) -> experimental space.  The alternate axis mapping.
Obs forward_from_sd( double s20w, double D20w, double vbar20, const Buffer& b )
{
   US_Model::SimulationComponent c;
   c.s      = s20w;
   c.D      = D20w;
   c.vbar20 = vbar20;
   c.f_f0   = 0.0;
   c.mw     = 0.0;
   c.f      = 0.0;
   EXPECT_TRUE( US_Model::calc_coefficients( c ) );

   US_Math2::SolutionData sd = corrections( vbar20, b );
   return { c.s / sd.s20w_correction, c.D / sd.D20w_correction,
            c.f_f0, c.mw, c.D };
}

// Invert the corrections: for a chosen vbar, the standard-space pair that
// reproduces the given observables exactly.  This constructs a point on the
// fibre rather than searching for one.
void fibre_point( double s_exp, double D_exp, double vbar20, const Buffer& b,
                  double& s20w, double& D20w )
{
   US_Math2::SolutionData sd = corrections( vbar20, b );
   s20w = s_exp * sd.s20w_correction;
   D20w = D_exp * sd.D20w_correction;
}

// Closed form of d(ln R)/d(vbar), the gain from a relative s-ratio
// measurement to a vbar estimate.  Section 3.4 of the design document.
//
// The densities passed in must be the *temperature-corrected* buffer
// densities (US_Math2::SolutionData::density_tb), not the nominal 20 C
// values: data_correction() forms the buoyancy term from density_tb, and
// density_tb differs from the nominal density by the factor
// density_wt(T)/DENS_20W.  That factor is only ~4e-7 at 20 C but grows to
// ~0.1% at 25 C and ~2% at 40 C, so the distinction matters for real runs.
double buoyancy_gain( double vbar, double rho1_tb, double rho2_tb )
{
   return rho1_tb / ( 1.0 - vbar * rho1_tb )
        - rho2_tb / ( 1.0 - vbar * rho2_tb );
}

// The temperature-corrected density data_correction() actually uses.
double density_tb( double vbar20, const Buffer& b )
{
   return corrections( vbar20, b ).density_tb;
}

double rel_diff( double a, double b )
{
   return std::fabs( a - b ) / std::fabs( b );
}

} // namespace

class TestSolveSimVbar : public QtTestBase
{
protected:
   // The vbar values sampled along the fibre.
   static std::vector< double > vbar_samples()
   {
      std::vector< double > v;
      for ( int i = 0; i <= 10; i++ )
         v.push_back( VBAR_LO + ( VBAR_HI - VBAR_LO ) * i / 10.0 );
      return v;
   }

   // Simulate one species through the real ASTFEM solver.  Deliberately
   // takes experimental-space s and D plus a vbar, so that vbar can be
   // varied while the pair the solver actually reads is held fixed.
   static void simulate( double s_exp, double D_exp, double vbar20,
                         US_DataIO::RawData& out )
   {
      const int    nscans   = 5;
      const int    rpm      = 45000;
      const double meniscus = 5.8;
      const double bottom   = 7.2;
      const double dr       = 0.002;
      const double w2       = std::pow( rpm * M_PI / 30.0, 2.0 );

      US_SimulationParameters sp;
      sp.simpoints         = 200;
      sp.radial_resolution = dr;
      sp.meniscus          = meniscus;
      sp.bottom            = bottom;
      sp.bottom_position   = bottom;
      sp.temperature       = 20.0;
      sp.sim               = true;
      sp.speed_step[ 0 ].duration_hours    = 5;
      sp.speed_step[ 0 ].duration_minutes  = 0.0;
      sp.speed_step[ 0 ].delay_hours       = 0;
      sp.speed_step[ 0 ].delay_minutes     = 0.0;
      sp.speed_step[ 0 ].scans             = nscans;
      sp.speed_step[ 0 ].rotorspeed        = rpm;
      sp.speed_step[ 0 ].acceleration      = 400;
      sp.speed_step[ 0 ].acceleration_flag = false;

      US_Model m;
      m.components.resize( 1 );
      m.components[ 0 ].s       = s_exp;
      m.components[ 0 ].D       = D_exp;
      m.components[ 0 ].vbar20  = vbar20;
      m.components[ 0 ].f_f0    = 0.0;
      m.components[ 0 ].mw      = 0.0;
      m.components[ 0 ].f       = 0.0;
      m.components[ 0 ].signal_concentration = 1.0;

      out.xvalues .clear();
      out.scanData.clear();
      for ( double r = meniscus; r <= bottom + 1.0e-9; r += dr )
         out.xvalues << r;

      const int npoints = out.xvalues.size();
      for ( int ii = 0; ii < nscans; ii++ )
      {
         US_DataIO::Scan sc;
         sc.temperature = 20.0;
         sc.rpm         = (double)rpm;
         sc.seconds     = 3600.0 * ( ii + 1 );
         sc.omega2t     = w2 * sc.seconds;
         sc.wavelength  = 280.0;
         sc.plateau     = 0.0;
         sc.delta_r     = dr;
         sc.rvalues     = QVector< double >( npoints, 0.0 );
         out.scanData << sc;
      }

      US_SimulationParameters::computeSpeedSteps( &out.scanData,
                                                  sp.speed_step );
      US_Astfem_RSA astfem( m, sp );
      astfem.set_debug_flag( 0 );
      ASSERT_EQ( 0, astfem.calculate( out ) );
   }
};

// ---------------------------------------------------------------------------
// (s, f/f0, vbar) and (s, D, vbar) are two coordinate systems on the same
// grid.  Either may be offered as the axis mapping; they must agree.
// ---------------------------------------------------------------------------
TEST_F( TestSolveSimVbar, ParameterisationsAreEquivalent )
{
   Obs from_k = forward_from_sk( REF_S, REF_FF0, REF_VBAR, BUF_WATER );
   Obs from_d = forward_from_sd( REF_S, from_k.D20w, REF_VBAR, BUF_WATER );

   EXPECT_LT( rel_diff( from_d.s_exp, from_k.s_exp ), 1.0e-12 );
   EXPECT_LT( rel_diff( from_d.D_exp, from_k.D_exp ), 1.0e-12 );
   EXPECT_LT( rel_diff( from_d.ff0,   from_k.ff0   ), 1.0e-12 );
   EXPECT_LT( rel_diff( from_d.mw,    from_k.mw    ), 1.0e-12 );
}

// ---------------------------------------------------------------------------
// The degeneracy, constructively.  For every vbar in the grid range there is
// an (s20w, f/f0) that reproduces the observables exactly -- so a single
// dataset cannot distinguish them, however good the data.
//
// The molar-mass spread is the part worth reading: the same data are fitted
// exactly by species differing in mass by more than a factor of two.
// ---------------------------------------------------------------------------
TEST_F( TestSolveSimVbar, SingleDatasetFibreIsExact )
{
   const Obs ref = forward_from_sk( REF_S, REF_FF0, REF_VBAR, BUF_WATER );

   double mw_lo = 1.0e300;
   double mw_hi = 0.0;

   for ( double vbar : vbar_samples() )
   {
      double s20w, D20w;
      fibre_point( ref.s_exp, ref.D_exp, vbar, BUF_WATER, s20w, D20w );
      Obs got = forward_from_sd( s20w, D20w, vbar, BUF_WATER );

      // Same observables, to round-off.
      EXPECT_LT( rel_diff( got.s_exp, ref.s_exp ), 1.0e-12 )
         << "vbar = " << vbar;
      EXPECT_LT( rel_diff( got.D_exp, ref.D_exp ), 1.0e-12 )
         << "vbar = " << vbar;

      mw_lo = std::min( mw_lo, got.mw );
      mw_hi = std::max( mw_hi, got.mw );
   }

   // Radically different species, identical observables.
   EXPECT_GT( mw_hi / mw_lo, 2.0 )
      << "molar mass range along the fibre: " << mw_lo << " .. " << mw_hi;
}

// ---------------------------------------------------------------------------
// The same statement at the level of the solver rather than the coefficients:
// two fibre points produce identical simulated data.  This is what makes the
// NNLS columns duplicates and vbar unrecoverable from one dataset.
// ---------------------------------------------------------------------------
TEST_F( TestSolveSimVbar, LammSolverIgnoresVbar )
{
   const Obs ref = forward_from_sk( REF_S, REF_FF0, REF_VBAR, BUF_WATER );

   double s_lo, d_lo, s_hi, d_hi;
   fibre_point( ref.s_exp, ref.D_exp, VBAR_LO, BUF_WATER, s_lo, d_lo );
   fibre_point( ref.s_exp, ref.D_exp, VBAR_HI, BUF_WATER, s_hi, d_hi );

   Obs a = forward_from_sd( s_lo, d_lo, VBAR_LO, BUF_WATER );
   Obs b = forward_from_sd( s_hi, d_hi, VBAR_HI, BUF_WATER );

   // Two genuinely different species by any structural measure ...
   ASSERT_GT( b.mw / a.mw, 2.0 );

   US_DataIO::RawData sim_a, sim_b;
   ASSERT_NO_FATAL_FAILURE( simulate( a.s_exp, a.D_exp, VBAR_LO, sim_a ) );
   ASSERT_NO_FATAL_FAILURE( simulate( b.s_exp, b.D_exp, VBAR_HI, sim_b ) );

   ASSERT_EQ( sim_a.scanCount(),  sim_b.scanCount()  );
   ASSERT_EQ( sim_a.pointCount(), sim_b.pointCount() );

   double max_diff = 0.0;
   double max_val  = 0.0;
   for ( int ss = 0; ss < sim_a.scanCount(); ss++ )
      for ( int rr = 0; rr < sim_a.pointCount(); rr++ )
      {
         max_diff = std::max( max_diff,
                       std::fabs( sim_a.value( ss, rr )
                                - sim_b.value( ss, rr ) ) );
         max_val  = std::max( max_val, std::fabs( sim_a.value( ss, rr ) ) );
      }

   // ... producing indistinguishable data.
   ASSERT_GT( max_val, 1.0e-6 ) << "simulation produced no signal";
   EXPECT_LT( max_diff / max_val, 1.0e-10 )
      << "max |A-B| = " << max_diff << ", max |A| = " << max_val;
}

// ---------------------------------------------------------------------------
// Adding a second buffer density spreads the fibre out.  The spread is the
// signal a global 3DSA fit works from.
// ---------------------------------------------------------------------------
TEST_F( TestSolveSimVbar, DensityContrastSeparatesTheFibre )
{
   const Obs ref = forward_from_sk( REF_S, REF_FF0, REF_VBAR, BUF_WATER );

   auto spread_in = [ & ]( const Buffer& b )
   {
      double lo = 1.0e300;
      double hi = 0.0;
      for ( double vbar : vbar_samples() )
      {
         double s20w, D20w;
         fibre_point( ref.s_exp, ref.D_exp, vbar, BUF_WATER, s20w, D20w );
         Obs got = forward_from_sd( s20w, D20w, vbar, b );
         lo = std::min( lo, got.s_exp );
         hi = std::max( hi, got.s_exp );
      }
      return ( hi - lo ) / lo;
   };

   // In the buffer the fibre was built for, there is by construction none.
   EXPECT_LT( spread_in( BUF_WATER ), 1.0e-12 );

   // Full contrast separates the fibre by more than its own width.
   EXPECT_GT( spread_in( BUF_D2O  ), 1.00 );
   // Half contrast still separates it well.
   EXPECT_GT( spread_in( BUF_HALF ), 0.25 );
   // Weak contrast does not, and monotonicity in density must hold.
   EXPECT_LT( spread_in( BUF_WEAK ), spread_in( BUF_HALF ) );
   EXPECT_LT( spread_in( BUF_HALF ), spread_in( BUF_D2O  ) );
}

// ---------------------------------------------------------------------------
// The gain that converts s-ratio precision into vbar precision, checked
// against the corrections rather than assumed.  Phase 1 lifts this into
// US_SolveSim::buoyancy_contrast(); the expected values come from
// doc/develop/3dsa_design.md section 3.4.
// ---------------------------------------------------------------------------
TEST_F( TestSolveSimVbar, BuoyancyGainMatchesCorrections )
{
   // Central difference of what data_correction() actually computes.
   auto gain_from_corrections = [ & ]( double vbar, const Buffer& b2 )
   {
      const double h = 1.0e-5;
      auto ln_ratio = [ & ]( double v )
      {
         US_Math2::SolutionData c1 = corrections( v, BUF_WATER );
         US_Math2::SolutionData c2 = corrections( v, b2 );
         return std::log( c1.s20w_correction / c2.s20w_correction );
      };
      return ( ln_ratio( vbar + h ) - ln_ratio( vbar - h ) ) / ( 2.0 * h );
   };

   struct Case { const Buffer& buf; double expected; };
   const Case cases[] = {
      { BUF_D2O,  -2.04 },
      { BUF_HALF, -0.82 },
      { BUF_WEAK, -0.17 },
   };

   for ( const Case& c : cases )
   {
      const double numeric = gain_from_corrections( REF_VBAR, c.buf );

      const double exact   = buoyancy_gain( REF_VBAR,
                                            density_tb( REF_VBAR, BUF_WATER ),
                                            density_tb( REF_VBAR, c.buf ) );

      const double nominal = buoyancy_gain( REF_VBAR, BUF_WATER.density,
                                            c.buf.density );

      // Evaluated at density_tb, the closed form *is* the derivative of the
      // corrections: agreement is limited only by the finite difference.
      EXPECT_LT( std::fabs( exact - numeric ) / std::fabs( exact ), 1.0e-6 )
         << "density " << c.buf.density;

      // Using the nominal density instead is systematically off.  Small at
      // 20 C, but it is a bias, not noise -- Phase 1's buoyancy_contrast()
      // must take density_tb from data_correction().
      EXPECT_GT( std::fabs( nominal - numeric ) / std::fabs( nominal ),
                 10.0 * std::fabs( exact - numeric ) / std::fabs( exact ) )
         << "density " << c.buf.density;

      // Both agree with the documented table to its stated precision.
      EXPECT_NEAR( exact, c.expected, 0.01 ) << "density " << c.buf.density;
   }
}

// ---------------------------------------------------------------------------
// The gate.  A series must clear the contrast threshold before a fit is
// allowed; these are the cases the Phase 1 metric has to classify.
// ---------------------------------------------------------------------------
TEST_F( TestSolveSimVbar, ContrastGateClassifiesSeries )
{
   const double rho_w  = density_tb( REF_VBAR, BUF_WATER );
   const double g_full = std::fabs(
      buoyancy_gain( REF_VBAR, rho_w, density_tb( REF_VBAR, BUF_D2O  ) ) );
   const double g_half = std::fabs(
      buoyancy_gain( REF_VBAR, rho_w, density_tb( REF_VBAR, BUF_HALF ) ) );
   const double g_weak = std::fabs(
      buoyancy_gain( REF_VBAR, rho_w, density_tb( REF_VBAR, BUF_WEAK ) ) );

   EXPECT_GT( g_full, GATE_WARN   );   // accepted outright
   EXPECT_GT( g_half, GATE_REFUSE );   // accepted with a warning
   EXPECT_LT( g_half, GATE_WARN   );
   EXPECT_LT( g_weak, GATE_REFUSE );   // refused

   // A single dataset has no contrast at all: the gain against itself is
   // identically zero, which is what makes single-dataset 3DSA meaningless.
   EXPECT_DOUBLE_EQ( 0.0, buoyancy_gain( REF_VBAR, rho_w, rho_w ) );

   // Resolution implied at 1% precision on the s-ratio, for the record.
   EXPECT_NEAR( 0.01 / g_full, 0.005, 0.001 );
   EXPECT_NEAR( 0.01 / g_half, 0.012, 0.001 );
   EXPECT_NEAR( 0.01 / g_weak, 0.061, 0.005 );
}

// ---------------------------------------------------------------------------
// US_SolveSim::buoyancy_contrast() -- the metric the D6 gate is built on.
// ---------------------------------------------------------------------------
namespace {

// A DataSet carrying just the fields buoyancy_contrast() reads.
US_SolveSim::DataSet* make_dataset( const Buffer& b )
{
   US_SolveSim::DataSet* d = new US_SolveSim::DataSet;
   d->density     = b.density;
   d->viscosity   = b.viscosity;
   d->temperature = b.temperature;
   d->manual      = false;
   d->vbar20      = REF_VBAR;
   return d;
}

// Owns the DataSets for one test and hands out the QList the API wants.
class Series
{
public:
   explicit Series( std::initializer_list< Buffer > buffers )
   {
      for ( const Buffer& b : buffers )
      {
         owned_.emplace_back( make_dataset( b ) );
         list_ << owned_.back().get();
      }
   }
   QList< US_SolveSim::DataSet* >& list() { return list_; }

private:
   std::vector< std::unique_ptr< US_SolveSim::DataSet > > owned_;
   QList< US_SolveSim::DataSet* >                         list_;
};

} // namespace

TEST_F( TestSolveSimVbar, BuoyancyContrastAgreesWithTheClosedForm )
{
   struct Case { Buffer second; double expected; };
   const Case cases[] = {
      { BUF_D2O,  2.04 },
      { BUF_HALF, 0.82 },
      { BUF_WEAK, 0.17 },
   };

   for ( const Case& c : cases )
   {
      Series series { BUF_WATER, c.second };
      QString msg;
      const double gain = US_SolveSim::buoyancy_contrast( series.list(),
                                                          REF_VBAR, msg );

      EXPECT_NEAR( gain, c.expected, 0.01 ) << "density " << c.second.density;
      EXPECT_FALSE( msg.isEmpty() );

      // Same number the local closed form gives, at density_tb.
      const double closed = std::fabs(
         buoyancy_gain( REF_VBAR, density_tb( REF_VBAR, BUF_WATER ),
                                  density_tb( REF_VBAR, c.second ) ) );
      EXPECT_LT( std::fabs( gain - closed ) / closed, 1.0e-12 );
   }
}

TEST_F( TestSolveSimVbar, BuoyancyContrastTakesTheWidestPair )
{
   // A third, intermediate buffer must not lower the reported contrast: the
   // metric reports the best pair in the series, not the average.
   Series pair   { BUF_WATER, BUF_D2O };
   Series triple { BUF_WATER, BUF_HALF, BUF_D2O };

   QString m1, m2;
   const double g_pair   = US_SolveSim::buoyancy_contrast( pair  .list(),
                                                           REF_VBAR, m1 );
   const double g_triple = US_SolveSim::buoyancy_contrast( triple.list(),
                                                           REF_VBAR, m2 );

   EXPECT_NEAR( g_pair, g_triple, 1.0e-12 );
}

TEST_F( TestSolveSimVbar, BuoyancyContrastIsZeroWithoutContrast )
{
   // One data set: no contrast at all, and the message must say why.
   Series single { BUF_WATER };
   QString msg;
   EXPECT_DOUBLE_EQ( 0.0, US_SolveSim::buoyancy_contrast( single.list(),
                                                          REF_VBAR, msg ) );
   EXPECT_FALSE( msg.isEmpty() );

   // Several runs in the same buffer are no better.
   Series same { BUF_WATER, BUF_WATER, BUF_WATER };
   QString msg2;
   EXPECT_DOUBLE_EQ( 0.0, US_SolveSim::buoyancy_contrast( same.list(),
                                                          REF_VBAR, msg2 ) );

   // Both fall below the refusal threshold, which is what the gate acts on.
   EXPECT_LT( 0.0, US_SolveSim::VBAR_CONTRAST_REFUSE );
}

TEST_F( TestSolveSimVbar, ContrastGateThresholdsClassifyTheExampleSeries )
{
   auto gain_of = [ & ]( const Buffer& second )
   {
      Series series { BUF_WATER, second };
      QString msg;
      return US_SolveSim::buoyancy_contrast( series.list(), REF_VBAR, msg );
   };

   EXPECT_GT( gain_of( BUF_D2O  ), US_SolveSim::VBAR_CONTRAST_WARN   );
   EXPECT_GT( gain_of( BUF_HALF ), US_SolveSim::VBAR_CONTRAST_REFUSE );
   EXPECT_LT( gain_of( BUF_HALF ), US_SolveSim::VBAR_CONTRAST_WARN   );
   EXPECT_LT( gain_of( BUF_WEAK ), US_SolveSim::VBAR_CONTRAST_REFUSE );

   // The thresholds themselves must stay in the order the gate assumes.
   EXPECT_LT( US_SolveSim::VBAR_CONTRAST_REFUSE,
              US_SolveSim::VBAR_CONTRAST_WARN );
}

TEST_F( TestSolveSimVbar, VbarResolutionInvertsTheGain )
{
   Series series { BUF_WATER, BUF_D2O };
   QString msg;
   const double gain = US_SolveSim::buoyancy_contrast( series.list(),
                                                       REF_VBAR, msg );

   EXPECT_NEAR( US_SolveSim::vbar_resolution( gain, 0.01 ), 0.005, 0.001 );

   // Halving the s precision halves the resolution.
   EXPECT_NEAR( US_SolveSim::vbar_resolution( gain, 0.005 ),
                US_SolveSim::vbar_resolution( gain, 0.010 ) * 0.5, 1.0e-12 );

   // No contrast means no resolution, reported as a large sentinel rather
   // than a division by zero.
   EXPECT_GT( US_SolveSim::vbar_resolution( 0.0, 0.01 ), 1.0e+12 );
}

// ---------------------------------------------------------------------------
// The corrections path in calc_residuals().  fit_vbar tells it that vbar
// varies per solute even when vbar sits in the mask's Z slot; without the
// flag the historical positional rule is preserved exactly.
// ---------------------------------------------------------------------------
TEST_F( TestSolveSimVbar, FitVbarDefaultsOffSoBehaviourIsUnchanged )
{
   US_SolveSim::DataSet dset;
   EXPECT_FALSE( dset.fit_vbar )
      << "fit_vbar must default off, or every existing caller changes"
         " behaviour";
}

TEST_F( TestSolveSimVbar, CorrectionsAreVbarSensitiveOnlyUnderContrast )
{
   // What fit_vbar controls is whether each solute gets its own corrections
   // or the data set's cached ones.  That only matters where the corrections
   // actually depend on vbar -- and the size of the dependence is the same
   // quantity buoyancy_contrast() measures.
   const double nominal_vbar = 0.73;   // the data set's vbar
   const double solute_vbar  = 0.62;   // a grid point elsewhere on the axis

   auto s_corr_shift = [ & ]( const Buffer& b )
   {
      const US_Math2::SolutionData cached = corrections( nominal_vbar, b );
      const US_Math2::SolutionData fresh  = corrections( solute_vbar,  b );

      // D never sees vbar, in any buffer.
      EXPECT_DOUBLE_EQ( fresh.D20w_correction, cached.D20w_correction );

      return std::fabs( fresh.s20w_correction - cached.s20w_correction )
             / cached.s20w_correction;
   };

   // In water the buoyancy ratio is (1 - v*rho_20w)/(1 - v*rho_tb) with the
   // two densities nearly equal, so vbar very nearly cancels.  Using the
   // cached corrections there costs almost nothing -- which is exactly why a
   // single aqueous run cannot determine vbar.
   EXPECT_LT( s_corr_shift( BUF_WATER ), 1.0e-3 );

   // Under real contrast the same vbar shift moves the correction by more
   // than 10%, so the cached-versus-recomputed distinction is decisive.
   EXPECT_GT( s_corr_shift( BUF_D2O  ), 0.05 );
   EXPECT_GT( s_corr_shift( BUF_HALF ), 0.01 );

   // The ordering follows the contrast, as it must.
   EXPECT_LT( s_corr_shift( BUF_WATER ), s_corr_shift( BUF_WEAK ) );
   EXPECT_LT( s_corr_shift( BUF_WEAK  ), s_corr_shift( BUF_HALF ) );
   EXPECT_LT( s_corr_shift( BUF_HALF  ), s_corr_shift( BUF_D2O  ) );
}
