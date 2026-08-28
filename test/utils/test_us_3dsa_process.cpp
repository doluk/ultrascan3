// test_us_3dsa_process.cpp - End-to-end tests for the 3DSA fit engine
//
// Builds a synthetic buoyancy-contrast series from a known model, runs
// US_3dsaProcess over it, and checks that the fit recovers the partial
// specific volume that produced the data.  See doc/develop/3dsa_design.md
// section 8.2.
//
// The datasets are deliberately given *different loading concentrations*,
// because a real density series loads each cell separately.  Without the
// per-dataset amplitude factors that difference biases the fitted vbar, so
// recovering both the vbar and the loadings is the point of the test.

#include "qt_test_base.h"

#include "us_3dsa_process.h"
#include "us_astfem_rsa.h"
#include "us_dataIO.h"
#include "us_math2.h"
#include "us_model.h"
#include "us_simparms.h"
#include "us_solve_sim.h"
#include "us_solute.h"

#include <cmath>
#include <memory>
#include <vector>

namespace {

struct Buffer
{
   double density;
   double viscosity;
   double temperature;
};

const Buffer BUF_WATER { 0.998234, 1.001940, 20.0 };
const Buffer BUF_HALF  { 1.050000, 1.120000, 20.0 };
const Buffer BUF_D2O   { 1.105000, 1.251000, 20.0 };

// The species the synthetic data is generated from.
const double TRUE_S    = 4.0e-13;
const double TRUE_FF0  = 1.5;
const double TRUE_VBAR = 0.73;
const double TRUE_CONC = 0.50;

// Cheap but realistic simulation geometry: enough scans and radial points to
// resolve a boundary, few enough for a unit test.
const int    NSCANS    = 4;
const int    RPM       = 45000;
const double MENISCUS  = 5.85;
const double BOTTOM    = 7.15;
const double DELTA_R   = 0.005;
const int    SIMPOINTS  = 100;

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

US_SimulationParameters make_simparams()
{
   US_SimulationParameters sp;
   sp.simpoints         = SIMPOINTS;
   sp.radial_resolution = DELTA_R;
   sp.meniscus          = MENISCUS;
   sp.bottom            = BOTTOM;
   sp.bottom_position   = BOTTOM;
   sp.temperature       = 20.0;
   sp.sim               = true;
   sp.speed_step[ 0 ].duration_hours    = 5;
   sp.speed_step[ 0 ].duration_minutes  = 0.0;
   sp.speed_step[ 0 ].delay_hours       = 0;
   sp.speed_step[ 0 ].delay_minutes     = 0.0;
   sp.speed_step[ 0 ].scans             = NSCANS;
   sp.speed_step[ 0 ].rotorspeed        = RPM;
   sp.speed_step[ 0 ].acceleration      = 400;
   sp.speed_step[ 0 ].acceleration_flag = false;
   return sp;
}

// An empty data grid on the geometry above, ready for the solver to fill.
void make_grid( US_DataIO::RawData& data )
{
   const double w2 = std::pow( RPM * M_PI / 30.0, 2.0 );

   data.xvalues .clear();
   data.scanData.clear();

   for ( double rr = MENISCUS; rr <= BOTTOM + 1.0e-9; rr += DELTA_R )
      data.xvalues << rr;

   const int npoints = data.xvalues.size();

   for ( int ii = 0; ii < NSCANS; ii++ )
   {
      US_DataIO::Scan sc;
      sc.temperature = 20.0;
      sc.rpm         = (double)RPM;
      sc.seconds     = 2400.0 * ( ii + 1 );
      sc.omega2t     = w2 * sc.seconds;
      sc.wavelength  = 280.0;
      sc.plateau     = 0.0;
      sc.delta_r     = DELTA_R;
      sc.rvalues     = QVector< double >( npoints, 0.0 );
      data.scanData << sc;
   }
}

// Simulate a standard-space (20W) model as it would be observed in one
// buffer, scaled by that cell's loading.
void simulate_in( const US_Model& truth_20w, const Buffer& b,
                  double loading, US_DataIO::RawData& out )
{
   US_Model expt = truth_20w;

   for ( int cc = 0; cc < expt.components.size(); cc++ )
   {
      US_Math2::SolutionData sd =
         corrections( truth_20w.components[ cc ].vbar20, b );
      expt.components[ cc ].s /= sd.s20w_correction;
      expt.components[ cc ].D /= sd.D20w_correction;
      expt.components[ cc ].signal_concentration *= loading;
   }

   make_grid( out );

   US_SimulationParameters sp = make_simparams();
   US_SimulationParameters::computeSpeedSteps( &out.scanData, sp.speed_step );

   US_Astfem_RSA astfem( expt, sp );
   astfem.set_debug_flag( 0 );
   astfem.calculate( out );
}

// Wrap simulated data as an edited dataset the solver will accept.
US_SolveSim::DataSet* make_dataset( const US_Model& truth_20w, const Buffer& b,
                                    double loading, double nominal_vbar )
{
   US_DataIO::RawData sim;
   simulate_in( truth_20w, b, loading, sim );

   US_SolveSim::DataSet* d = new US_SolveSim::DataSet;

   d->run_data.runID       = "synthetic";
   d->run_data.dataType    = "RA";
   d->run_data.cell        = "1";
   d->run_data.channel     = "A";
   d->run_data.wavelength  = "280";
   d->run_data.expType     = "velocity";
   d->run_data.meniscus    = MENISCUS;
   d->run_data.bottom      = BOTTOM;
   d->run_data.baseline    = 0.0;
   d->run_data.plateau     = 0.0;
   d->run_data.ODlimit     = 1.0e+30;   // never substitute zeros
   d->run_data.xvalues     = sim.xvalues;
   d->run_data.scanData    = sim.scanData;

   d->simparams   = make_simparams();
   US_SimulationParameters::computeSpeedSteps( &d->run_data.scanData,
                                               d->simparams.speed_step );

   d->density     = b.density;
   d->viscosity   = b.viscosity;
   d->temperature = b.temperature;
   d->manual      = false;
   d->compress    = 0.0;
   d->vbar20      = nominal_vbar;

   US_Math2::SolutionData sd = corrections( nominal_vbar, b );
   d->vbartb          = sd.vbar;
   d->s20w_correction = sd.s20w_correction;
   d->D20w_correction = sd.D20w_correction;
   d->solute_type     = US_3dsaProcess::mask_s_k_v();
   d->centerpiece_bottom = BOTTOM;
   d->rotor_stretch[ 0 ] = 0.0;
   d->rotor_stretch[ 1 ] = 0.0;

   return d;
}

US_Model truth_model()
{
   US_Model m;
   m.components.resize( 1 );
   m.components[ 0 ].s      = TRUE_S;
   m.components[ 0 ].f_f0   = TRUE_FF0;
   m.components[ 0 ].vbar20 = TRUE_VBAR;
   m.components[ 0 ].D      = 0.0;
   m.components[ 0 ].mw     = 0.0;
   m.components[ 0 ].f      = 0.0;
   m.components[ 0 ].signal_concentration = TRUE_CONC;
   m.update_coefficients();
   return m;
}

// Concentration-weighted mean of an attribute over a fitted model.
double weighted_vbar( const US_Model& m )
{
   double num = 0.0;
   double den = 0.0;

   for ( int cc = 0; cc < m.components.size(); cc++ )
   {
      const double conc = m.components[ cc ].signal_concentration;
      num += m.components[ cc ].vbar20 * conc;
      den += conc;
   }

   return ( den > 0.0 ) ? ( num / den ) : 0.0;
}

double weighted_s( const US_Model& m )
{
   double num = 0.0;
   double den = 0.0;

   for ( int cc = 0; cc < m.components.size(); cc++ )
   {
      const double conc = m.components[ cc ].signal_concentration;
      num += m.components[ cc ].s * conc;
      den += conc;
   }

   return ( den > 0.0 ) ? ( num / den ) : 0.0;
}

// Owns a series of datasets and hands out the QList the engine wants.
class Series
{
public:
   void add( const Buffer& b, double loading )
   {
      owned_.emplace_back( make_dataset( truth_model(), b, loading,
                                         TRUE_VBAR ) );
      list_ << owned_.back().get();
   }
   QList< US_SolveSim::DataSet* >& list() { return list_; }

private:
   std::vector< std::unique_ptr< US_SolveSim::DataSet > > owned_;
   QList< US_SolveSim::DataSet* >                         list_;
};

// A grid that straddles the truth, with the true values landing exactly on
// grid points so the test measures the fit rather than the discretisation.
US_3dsaProcess::Parameters base_parameters()
{
   US_3dsaProcess::Parameters p;
   p.x_min      = 2.0e-13;   p.x_max = 6.0e-13;  p.x_res = 5;   // step 1.0
   p.y_min      = 1.00;      p.y_max = 2.00;     p.y_res = 5;   // step 0.25
   p.z_min      = 0.65;      p.z_max = 0.81;     p.z_res = 5;   // step 0.04
   p.grid_reps  = 1;
   p.s_mask     = US_3dsaProcess::mask_s_k_v();
   p.nthreads   = 4;
   p.noisflag   = 0;
   p.max_tsols  = 200;
   p.fit_scales = true;
   return p;
}

} // namespace

class TestUS3dsaProcess : public QtTestBase {};

// ---------------------------------------------------------------------------
// The gate: a series without contrast must be refused, not fitted.
// ---------------------------------------------------------------------------
TEST_F( TestUS3dsaProcess, RefusesASeriesWithoutContrast )
{
   Series series;
   series.add( BUF_WATER, 1.0 );
   series.add( BUF_WATER, 1.0 );

   US_3dsaProcess proc( series.list() );
   US_3dsaProcess::Result result;

   EXPECT_FALSE( proc.fit( base_parameters(), result ) );
   EXPECT_FALSE( proc.lastError().isEmpty() );
   EXPECT_DOUBLE_EQ( 0.0, result.contrast );
}

TEST_F( TestUS3dsaProcess, RefusesASingleDataset )
{
   Series series;
   series.add( BUF_WATER, 1.0 );

   US_3dsaProcess proc( series.list() );
   US_3dsaProcess::Result result;

   EXPECT_FALSE( proc.fit( base_parameters(), result ) );
}

TEST_F( TestUS3dsaProcess, RejectsAnUnusableMask )
{
   Series series;
   series.add( BUF_WATER, 1.0 );
   series.add( BUF_D2O,   1.0 );

   US_3dsaProcess::Parameters p = base_parameters();
   p.s_mask = ( US_Solute::ATTR_S << 6 ) | ( US_Solute::ATTR_D << 3 )
              | US_Solute::ATTR_W;          // D and MW share one field

   US_3dsaProcess proc( series.list() );
   US_3dsaProcess::Result result;

   EXPECT_FALSE( proc.fit( p, result ) );
   EXPECT_TRUE( proc.lastError().contains( "MW" ) )
      << proc.lastError().toStdString();
}

// ---------------------------------------------------------------------------
// The exit criterion: recover vbar from a contrast series.
// ---------------------------------------------------------------------------
TEST_F( TestUS3dsaProcess, RecoversVbarFromAContrastSeries )
{
   // Three buffers, three different cell loadings.
   Series series;
   series.add( BUF_WATER, 1.00 );
   series.add( BUF_HALF,  0.70 );
   series.add( BUF_D2O,   1.40 );

   US_3dsaProcess proc( series.list() );
   US_3dsaProcess::Result result;

   QObject::connect( &proc, &US_3dsaProcess::message_update,
                     [ ]( QString m )
                     { std::printf( "  | %s\n", m.toStdString().c_str() ); } );

   ASSERT_TRUE( proc.fit( base_parameters(), result ) )
      << proc.lastError().toStdString();

   std::printf( "\n  %s\n", result.report.toStdString().c_str() );
   std::printf( "  scales:" );
   for ( double sc : result.scales )  std::printf( " %.4f", sc );
   std::printf( "\n  components: %d   weighted vbar %.4f   weighted s %.4f S\n",
                (int)result.model.components.size(),
                weighted_vbar( result.model ),
                weighted_s( result.model ) * 1.0e13 );

   // The series is well above the refusal threshold.
   EXPECT_GT( result.contrast, US_SolveSim::VBAR_CONTRAST_WARN );

   ASSERT_GT( result.model.components.size(), 0 );

   // vbar comes back well inside the resolution the contrast implies
   // (0.0049 mL/g at 1% s precision), and far inside one grid step of 0.04.
   EXPECT_NEAR( weighted_vbar( result.model ), TRUE_VBAR, 0.005 );

   // s likewise, against a grid step of 1.0 S.
   EXPECT_NEAR( weighted_s( result.model ) * 1.0e13, TRUE_S * 1.0e13, 0.05 );

   // The loadings come back as ratios against the first data set.
   ASSERT_EQ( 3, result.scales.size() );
   EXPECT_NEAR( result.scales[ 0 ], 1.00, 1.0e-9 );   // the gauge
   EXPECT_NEAR( result.scales[ 1 ], 0.70, 0.01 );
   EXPECT_NEAR( result.scales[ 2 ], 1.40, 0.01 );

   // Total concentration matches the loading of the gauge data set.
   double total_conc = 0.0;
   for ( int cc = 0; cc < result.model.components.size(); cc++ )
      total_conc += result.model.components[ cc ].signal_concentration;
   EXPECT_NEAR( total_conc, TRUE_CONC, TRUE_CONC * 0.02 );

   // The amplitude loop converged rather than running out of iterations.
   // Without the geometric extrapolation this sequence contracts by only
   // ~0.79 per pass and needs roughly 26.
   EXPECT_LT( result.nscaliter, 12 );
   EXPECT_LT( result.rmsd, 1.0e-3 );
}

// ---------------------------------------------------------------------------
// Without the amplitude factors the same data biases vbar.  This is why
// design decision D5 exists.
// ---------------------------------------------------------------------------
TEST_F( TestUS3dsaProcess, ScaleFittingRemovesTheLoadingBias )
{
   auto run = [ & ]( bool fit_scales, US_3dsaProcess::Result& result )
   {
      Series series;
      series.add( BUF_WATER, 1.00 );
      series.add( BUF_HALF,  0.70 );
      series.add( BUF_D2O,   1.40 );

      US_3dsaProcess proc( series.list() );
      US_3dsaProcess::Parameters p = base_parameters();
      p.fit_scales = fit_scales;

      return proc.fit( p, result );
   };

   US_3dsaProcess::Result with_scales;
   US_3dsaProcess::Result without;

   ASSERT_TRUE( run( true,  with_scales ) );
   ASSERT_TRUE( run( false, without     ) );

   const double err_with    = std::fabs( weighted_vbar( with_scales.model )
                                         - TRUE_VBAR );
   const double err_without = std::fabs( weighted_vbar( without.model )
                                         - TRUE_VBAR );

   std::printf( "\n  vbar error with scales %.4f, without %.4f\n",
                err_with, err_without );
   std::printf( "  rmsd with scales %.3e, without %.3e\n",
                with_scales.rmsd, without.rmsd );

   // Mismatched loadings must cost the unscaled fit something real.
   EXPECT_GT( without.rmsd, with_scales.rmsd * 2.0 );
   EXPECT_LE( err_with, err_without );
}

// ---------------------------------------------------------------------------
// Subgridding divides the work.  It is an approximation -- each subgrid sees
// only a coarse covering of the box, and only the survivors are re-fitted
// together -- so it is not required to reproduce the single-subgrid fit, only
// to stay inside the grid discretisation.
// ---------------------------------------------------------------------------
TEST_F( TestUS3dsaProcess, SubgriddingStaysWithinTheDiscretisation )
{
   auto run = [ & ]( int grid_reps, int nthreads,
                     US_3dsaProcess::Result& result )
   {
      Series series;
      series.add( BUF_WATER, 1.00 );
      series.add( BUF_D2O,   1.00 );

      US_3dsaProcess proc( series.list() );
      US_3dsaProcess::Parameters p = base_parameters();
      p.grid_reps  = grid_reps;
      p.nthreads   = nthreads;
      p.fit_scales = false;      // isolate the partitioning

      return proc.fit( p, result );
   };

   US_3dsaProcess::Result one;
   US_3dsaProcess::Result many;

   ASSERT_TRUE( run( 1, 1, one  ) );
   ASSERT_TRUE( run( 2, 4, many ) );

   // Both cover the same grid; only the partitioning differs.
   EXPECT_EQ( one.ngrid, many.ngrid );
   EXPECT_EQ( 1,         one.nsubgrids  );
   EXPECT_EQ( 8,         many.nsubgrids );
   EXPECT_GT( many.ntasks, one.ntasks );

   const double vbar_step = ( 0.81 - 0.65 ) / 4.0;   // 0.04
   const double s_step    = ( 6.0  - 2.0  ) / 4.0;   // 1.0 S

   // One subgrid fits every column together and lands on the truth.
   EXPECT_NEAR( weighted_vbar( one.model ), TRUE_VBAR, 0.002 );
   EXPECT_NEAR( weighted_s( one.model ) * 1.0e13, TRUE_S * 1.0e13, 0.05 );

   // Eight subgrids give up some accuracy for the smaller NNLS problems, but
   // must still land within half a grid step.  Note this is a deliberately
   // punishing case: two repetitions over a five-point axis leaves each
   // subgrid a very coarse covering, where a production run has eight
   // repetitions over sixty-four points.
   EXPECT_LT( std::fabs( weighted_vbar( many.model ) - TRUE_VBAR ),
              vbar_step * 0.5 )
      << "subgridded vbar " << weighted_vbar( many.model );
   EXPECT_LT( std::fabs( weighted_s( many.model ) - TRUE_S ) * 1.0e13,
              s_step * 0.5 );
}

// ---------------------------------------------------------------------------
// The model comes back in standard space with the analysis type set, which
// is what a model file holds.
// ---------------------------------------------------------------------------
TEST_F( TestUS3dsaProcess, ProducesAStandardSpaceModel )
{
   Series series;
   series.add( BUF_WATER, 1.0 );
   series.add( BUF_D2O,   1.0 );

   US_3dsaProcess proc( series.list() );
   US_3dsaProcess::Result result;
   ASSERT_TRUE( proc.fit( base_parameters(), result ) );

   EXPECT_EQ( US_Model::THREEDSA, result.model.analysis );

   // A fit over more than one data set is a global fit, and typeText()
   // labels it so.
   EXPECT_EQ( US_Model::GLOBAL,      result.model.global );
   EXPECT_EQ( QString( "3DSA-GL" ),  result.model.typeText() );

   for ( int cc = 0; cc < result.model.components.size(); cc++ )
   {
      const US_Model::SimulationComponent& c = result.model.components[ cc ];

      // Coefficients are complete and self-consistent.
      EXPECT_GT( c.s,      0.0 );
      EXPECT_GT( c.D,      0.0 );
      EXPECT_GT( c.mw,     0.0 );
      EXPECT_GE( c.f_f0,   1.0 );
      EXPECT_GT( c.vbar20, 0.0 );
      EXPECT_GT( c.signal_concentration, 0.0 );
      EXPECT_FALSE( c.name.isEmpty() );

      // Values sit inside the grid that generated them, i.e. they are
      // standard-space and were not converted to experimental space.
      EXPECT_GE( c.s * 1.0e13, 2.0 - 1.0e-9 );
      EXPECT_LE( c.s * 1.0e13, 6.0 + 1.0e-9 );
      EXPECT_GE( c.vbar20, 0.65 - 1.0e-9 );
      EXPECT_LE( c.vbar20, 0.81 + 1.0e-9 );
   }
}
