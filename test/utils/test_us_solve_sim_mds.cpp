// test_us_solve_sim_mds.cpp - Per-data-set TI and RI noise fitting
//
// US_SolveSim sizes its noise vectors for the whole series -- ntinois sums
// pointCount() over every data set, nrinois sums scanCount(), and the
// residual loop indexes them per data set -- but every routine that computes
// the noise reads data_sets[ d_offs ] and loops over that one set's point and
// scan counts.  Only the first block is ever written.  On one data set the
// two agree exactly; on a series the second set onward get no noise at all.
//
// Systematic noise belongs to the optics and the cell, not to the analyte, so
// two runs have unrelated TI and RI vectors and a single shared vector is the
// wrong model.  3DSA is global by construction, so it cannot use the
// single-data-set path.
//
// The tests below pin three things:
//
//   * On one data set US_SolveSimMDS reproduces US_SolveSim to the last bit,
//     so nothing about the established path has been changed by restating it.
//   * On a series it recovers each data set's own injected noise, and leaves
//     every data set with comparable residuals.
//   * US_SolveSim on the same series does not, which is the defect this class
//     exists to avoid.  That test is a live record of the difference: if it
//     ever starts failing because US_SolveSim was fixed too, this class can
//     be retired.
//
// See doc/develop/3dsa_design.md section 3C.

#include "qt_test_base.h"

#include "us_astfem_rsa.h"
#include "us_dataIO.h"
#include "us_math2.h"
#include "us_model.h"
#include "us_simparms.h"
#include "us_solve_sim.h"
#include "us_solve_sim_mds.h"
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
const Buffer BUF_D2O   { 1.105000, 1.251000, 20.0 };

const double TRUE_S    = 4.0e-13;
const double TRUE_FF0  = 1.5;
const double TRUE_VBAR = 0.73;
const double TRUE_CONC = 0.50;

const int    NSCANS    = 5;
const int    RPM       = 45000;
const double MENISCUS  = 5.85;
const double BOTTOM    = 7.15;
const double DELTA_R   = 0.005;
const int    SIMPOINTS = 100;

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

void simulate_in( const Buffer& b, US_DataIO::RawData& out )
{
   US_Model expt = truth_model();

   for ( int cc = 0; cc < expt.components.size(); cc++ )
   {
      US_Math2::SolutionData sd =
         corrections( expt.components[ cc ].vbar20, b );
      expt.components[ cc ].s /= sd.s20w_correction;
      expt.components[ cc ].D /= sd.D20w_correction;
   }

   make_grid( out );

   US_SimulationParameters sp = make_simparams();
   US_SimulationParameters::computeSpeedSteps( &out.scanData, sp.speed_step );

   US_Astfem_RSA astfem( expt, sp );
   astfem.set_debug_flag( 0 );
   astfem.calculate( out );
}

// The systematic noise of one cell.  Deliberately different shapes, so that
// a fit that recovers one and copies it to the other cannot pass.
QVector< double > ti_profile( int npoints, int which )
{
   QVector< double > ti( npoints, 0.0 );

   for ( int rr = 0; rr < npoints; rr++ )
   {
      const double u = (double)rr / (double)( npoints - 1 );

      ti[ rr ] = ( which == 0 )
                 ? (  0.0100 * std::sin( 6.0 * M_PI * u ) - 0.0040 )
                 : ( -0.0060 * std::cos( 3.0 * M_PI * u ) + 0.0090 * u );
   }

   return ti;
}

QVector< double > ri_profile( int nscans, int which )
{
   QVector< double > ri( nscans, 0.0 );

   for ( int ss = 0; ss < nscans; ss++ )
   {
      const double u = (double)ss / (double)qMax( 1, nscans - 1 );

      ri[ ss ] = ( which == 0 ) ? (  0.0080 - 0.0050 * u )
                                : ( -0.0030 + 0.0110 * u * u );
   }

   return ri;
}

// A data set carrying one cell's data plus, optionally, that cell's own
// systematic noise added on top.
US_SolveSim::DataSet* make_dataset( const Buffer& b, int which,
                                    bool add_ti, bool add_ri )
{
   US_DataIO::RawData sim;
   simulate_in( b, sim );

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
   d->run_data.ODlimit     = 1.0e+30;
   d->run_data.xvalues     = sim.xvalues;
   d->run_data.scanData    = sim.scanData;

   const int npoints = d->run_data.pointCount();
   const int nscans  = d->run_data.scanCount();

   const QVector< double > tin = ti_profile( npoints, which );
   const QVector< double > rin = ri_profile( nscans,  which );

   for ( int ss = 0; ss < nscans; ss++ )
   {
      for ( int rr = 0; rr < npoints; rr++ )
      {
         double value = d->run_data.value( ss, rr );
         if ( add_ti )  value += tin[ rr ];
         if ( add_ri )  value += rin[ ss ];
         d->run_data.setValue( ss, rr, value );
      }
   }

   d->simparams   = make_simparams();
   US_SimulationParameters::computeSpeedSteps( &d->run_data.scanData,
                                               d->simparams.speed_step );

   d->density     = b.density;
   d->viscosity   = b.viscosity;
   d->temperature = b.temperature;
   d->manual      = false;
   d->compress    = 0.0;
   d->vbar20      = TRUE_VBAR;

   US_Math2::SolutionData sd = corrections( TRUE_VBAR, b );
   d->vbartb             = sd.vbar;
   d->s20w_correction    = sd.s20w_correction;
   d->D20w_correction    = sd.D20w_correction;
   d->solute_type        = 0;            // s, f/f0, constant vbar
   d->centerpiece_bottom = BOTTOM;
   d->rotor_stretch[ 0 ] = 0.0;
   d->rotor_stretch[ 1 ] = 0.0;

   return d;
}

// A small (s, f/f0) grid that contains the truth exactly, so what is measured
// is the noise algebra and not the discretisation.
//
// s_lo moves the low corner.  At 2 S with f/f0 = 1 the species is small and
// strongly diffusing, so its profile barely changes between scans and NNLS
// can spend concentration there instead of on time-invariant noise.  That
// degeneracy is a property of the experiment, not of the solver, but it puts
// a floor on how exactly a TI vector can be recovered, so the tests that ask
// for an exact TI vector start the grid above it.
QVector< US_Solute > make_solutes( double s_lo = 2.0 )
{
   QVector< US_Solute > solutes;

   for ( int ii = 0; ii < 5; ii++ )
   {
      for ( int jj = 0; jj < 3; jj++ )
      {
         US_Solute so;
         so.s = ( s_lo + 1.0 * ii ) * 1.0e-13;  // five points, 1 S apart
         so.k = 1.00 + 0.25 * jj;               // 1.00 .. 1.50, truth at jj=2
         so.v = TRUE_VBAR;
         so.c = 0.0;
         solutes << so;
      }
   }

   return solutes;
}

class Series
{
public:
   void add( const Buffer& b, int which, bool add_ti, bool add_ri )
   {
      owned_.emplace_back( make_dataset( b, which, add_ti, add_ri ) );
      list_ << owned_.back().get();
   }
   QList< US_SolveSim::DataSet* >& list() { return list_; }
   int size() const { return list_.size(); }

private:
   std::vector< std::unique_ptr< US_SolveSim::DataSet > > owned_;
   QList< US_SolveSim::DataSet* >                         list_;
};

US_SolveSim::Simulation make_sim_vals( int noisflag, double s_lo = 2.0 )
{
   US_SolveSim::Simulation sv;
   sv.solutes    = make_solutes( s_lo );
   sv.noisflag   = noisflag;
   sv.alpha      = 0.0;
   sv.dbg_level  = 0;
   sv.dbg_timing = false;
   return sv;
}

// Largest deviation between two vectors after removing a common offset.  When
// both TI and RI noise are fitted the split between them is determined only
// up to an additive constant, so the shape is what can be compared.
double max_shape_error( const QVector< double >& got,
                        const QVector< double >& want, int offset, int count )
{
   double mean_got  = 0.0;
   double mean_want = 0.0;

   for ( int ii = 0; ii < count; ii++ )
   {
      mean_got  += got [ offset + ii ];
      mean_want += want[ ii ];
   }

   mean_got  /= (double)count;
   mean_want /= (double)count;

   double worst = 0.0;

   for ( int ii = 0; ii < count; ii++ )
      worst = qMax( worst, std::fabs( ( got[ offset + ii ] - mean_got )
                                    - ( want[ ii ] - mean_want ) ) );

   return worst;
}

double rms( const QVector< double >& v, int offset, int count )
{
   double sum = 0.0;

   for ( int ii = 0; ii < count; ii++ )
      sum += v[ offset + ii ] * v[ offset + ii ];

   return std::sqrt( sum / (double)count );
}

class TestSolveSimMDS : public QtTestBase {};

} // namespace

// ---------------------------------------------------------------------------
// On a single data set the copy must be indistinguishable from the original.
// ---------------------------------------------------------------------------
TEST_F( TestSolveSimMDS, MatchesUS_SolveSimOnASingleDataSet )
{
   for ( int noisflag = 0; noisflag <= 3; noisflag++ )
   {
      Series series;
      series.add( BUF_WATER, 0, true, true );

      US_SolveSim::Simulation ref = make_sim_vals( noisflag );
      US_SolveSim::Simulation mds = make_sim_vals( noisflag );

      US_SolveSim    old_solver( series.list(), 1, false );
      US_SolveSimMDS new_solver( series.list(), 1, false );

      old_solver.calc_residuals( 0, 1, ref );
      new_solver.calc_residuals( 0, 1, mds );

      SCOPED_TRACE( "noisflag " + std::to_string( noisflag ) );

      EXPECT_DOUBLE_EQ( ref.variance, mds.variance );
      ASSERT_EQ( ref.ti_noise.size(), mds.ti_noise.size() );
      ASSERT_EQ( ref.ri_noise.size(), mds.ri_noise.size() );
      ASSERT_EQ( ref.solutes .size(), mds.solutes .size() );

      for ( int ii = 0; ii < ref.ti_noise.size(); ii++ )
         EXPECT_NEAR( ref.ti_noise[ ii ], mds.ti_noise[ ii ], 1.0e-12 );

      for ( int ii = 0; ii < ref.ri_noise.size(); ii++ )
         EXPECT_NEAR( ref.ri_noise[ ii ], mds.ri_noise[ ii ], 1.0e-12 );

      for ( int ii = 0; ii < ref.solutes.size(); ii++ )
         EXPECT_NEAR( ref.solutes[ ii ].c, mds.solutes[ ii ].c, 1.0e-12 );
   }
}

// ---------------------------------------------------------------------------
// Each data set gets its own TI vector.
// ---------------------------------------------------------------------------
TEST_F( TestSolveSimMDS, RecoversPerDataSetTINoise )
{
   Series series;
   series.add( BUF_WATER, 0, true, false );
   series.add( BUF_D2O,   1, true, false );

   const int npoints = series.list()[ 0 ]->run_data.pointCount();

   US_SolveSim::Simulation sv = make_sim_vals( 1 );   // TI only
   US_SolveSimMDS solver( series.list(), 1, false );
   solver.calc_residuals( 0, series.size(), sv );

   ASSERT_EQ( 2 * npoints, sv.ti_noise.size() )
      << "the TI vector must carry one block per data set";

   for ( int ii = 0; ii < 2; ii++ )
   {
      SCOPED_TRACE( "data set " + std::to_string( ii ) );

      const QVector< double > want = ti_profile( npoints, ii );

      // TI alone is identifiable outright, offset included.
      EXPECT_LT( max_shape_error( sv.ti_noise, want, ii * npoints, npoints ),
                 1.0e-3 );
      EXPECT_LT( std::fabs( rms( sv.ti_noise, ii * npoints, npoints )
                          - rms( want, 0, npoints ) ), 1.0e-3 );

      // And it is this cell's noise, not the other cell's.
      EXPECT_GT( max_shape_error( sv.ti_noise, ti_profile( npoints, 1 - ii ),
                                  ii * npoints, npoints ), 1.0e-3 );
   }

   // Both data sets must end up equally well fitted.  A fit that cleans up
   // one and abandons the other passes every aggregate check but this one.
   ASSERT_EQ( 2, sv.variances.size() );
   const double rmsd0 = std::sqrt( sv.variances[ 0 ] );
   const double rmsd1 = std::sqrt( sv.variances[ 1 ] );

   EXPECT_LT( rmsd0, 1.0e-3 );
   EXPECT_LT( rmsd1, 1.0e-3 );
   EXPECT_LT( qMax( rmsd0, rmsd1 ) / qMax( qMin( rmsd0, rmsd1 ), 1.0e-30 ),
              5.0 );
}

// ---------------------------------------------------------------------------
// Each data set gets its own RI vector.
// ---------------------------------------------------------------------------
TEST_F( TestSolveSimMDS, RecoversPerDataSetRINoise )
{
   Series series;
   series.add( BUF_WATER, 0, false, true );
   series.add( BUF_D2O,   1, false, true );

   const int nscans = series.list()[ 0 ]->run_data.scanCount();

   US_SolveSim::Simulation sv = make_sim_vals( 2 );   // RI only
   US_SolveSimMDS solver( series.list(), 1, false );
   solver.calc_residuals( 0, series.size(), sv );

   ASSERT_EQ( 2 * nscans, sv.ri_noise.size() );

   for ( int ii = 0; ii < 2; ii++ )
   {
      SCOPED_TRACE( "data set " + std::to_string( ii ) );

      const QVector< double > want = ri_profile( nscans, ii );
      EXPECT_LT( max_shape_error( sv.ri_noise, want, ii * nscans, nscans ),
                 1.0e-3 );
      EXPECT_GT( max_shape_error( sv.ri_noise, ri_profile( nscans, 1 - ii ),
                                  ii * nscans, nscans ), 1.0e-3 );
   }

   ASSERT_EQ( 2, sv.variances.size() );
   const double rmsd0 = std::sqrt( sv.variances[ 0 ] );
   const double rmsd1 = std::sqrt( sv.variances[ 1 ] );

   EXPECT_LT( rmsd0, 1.0e-3 );
   EXPECT_LT( rmsd1, 1.0e-3 );
   EXPECT_LT( qMax( rmsd0, rmsd1 ) / qMax( qMin( rmsd0, rmsd1 ), 1.0e-30 ),
              5.0 );
}

// ---------------------------------------------------------------------------
// Both together, which is the case a real series presents.
//
// Fitting TI and RI at once does not recover either vector to the last digit,
// and that limit is not a multi-data-set effect: the reduced problem that
// eliminates the noise is handed to NNLS as its normal equations, A'PA x =
// A'Pb, which squares the conditioning of an already ill-conditioned Lamm
// basis.  A little concentration leaks onto neighbouring grid points and the
// difference surfaces in the TI vector.  US_SolveSim has always done it this
// way, the same floor shows up on one data set, and this test measures it
// there first so a failure on the series cannot be blamed on the wrong thing.
//
// What is exact, and is what this class changed, is which data set each block
// belongs to.
// ---------------------------------------------------------------------------
TEST_F( TestSolveSimMDS, RecoversPerDataSetTIAndRINoiseTogether )
{
   // First, one data set -- where US_SolveSimMDS is bit-identical to
   // US_SolveSim -- to establish the floor the series result sits on.
   double solo_ti_error = 0.0;
   {
      Series solo;
      solo.add( BUF_WATER, 0, true, true );

      const int np = solo.list()[ 0 ]->run_data.pointCount();

      US_SolveSim::Simulation sv = make_sim_vals( 3 );
      US_SolveSimMDS solver( solo.list(), 1, false );
      solver.calc_residuals( 0, 1, sv );

      solo_ti_error = max_shape_error( sv.ti_noise, ti_profile( np, 0 ),
                                       0, np );

      EXPECT_GT( solo_ti_error, 1.0e-3 )
         << "if the single-data-set TI vector has become exact, the normal-"
            "equations floor is gone and the series bound below is stale";
      EXPECT_LT( solo_ti_error, 1.0e-2 );
   }

   Series series;
   series.add( BUF_WATER, 0, true, true );
   series.add( BUF_D2O,   1, true, true );

   const int npoints = series.list()[ 0 ]->run_data.pointCount();
   const int nscans  = series.list()[ 0 ]->run_data.scanCount();

   US_SolveSim::Simulation sv = make_sim_vals( 3 );   // TI and RI
   US_SolveSimMDS solver( series.list(), 1, false );
   solver.calc_residuals( 0, series.size(), sv );

   ASSERT_EQ( 2 * npoints, sv.ti_noise.size() );
   ASSERT_EQ( 2 * nscans,  sv.ri_noise.size() );

   for ( int ii = 0; ii < 2; ii++ )
   {
      SCOPED_TRACE( "data set " + std::to_string( ii ) );

      // With both fitted the split between them is determined only up to an
      // additive constant, so the shapes are what can be compared.
      const double ti_own   = max_shape_error( sv.ti_noise,
                                 ti_profile( npoints, ii ),
                                 ii * npoints, npoints );
      const double ti_other = max_shape_error( sv.ti_noise,
                                 ti_profile( npoints, 1 - ii ),
                                 ii * npoints, npoints );

      // Within a small multiple of the single-data-set floor measured above,
      // and much closer to this cell's own profile than to the other's.
      EXPECT_LT( ti_own, 3.0 * solo_ti_error );
      EXPECT_LT( ti_own * 2.0, ti_other )
         << "this block must look like its own cell's noise, not the other's";

      // RI is short and well determined, so it has no such floor.
      const double ri_own   = max_shape_error( sv.ri_noise,
                                 ri_profile( nscans, ii ),
                                 ii * nscans, nscans );
      const double ri_other = max_shape_error( sv.ri_noise,
                                 ri_profile( nscans, 1 - ii ),
                                 ii * nscans, nscans );

      EXPECT_LT( ri_own, 1.0e-3 );
      EXPECT_LT( ri_own * 3.0, ri_other );
   }

   ASSERT_EQ( 2, sv.variances.size() );
   const double rmsd0 = std::sqrt( sv.variances[ 0 ] );
   const double rmsd1 = std::sqrt( sv.variances[ 1 ] );

   EXPECT_LT( rmsd0, 2.0e-3 );
   EXPECT_LT( rmsd1, 2.0e-3 );
   EXPECT_LT( qMax( rmsd0, rmsd1 ) / qMax( qMin( rmsd0, rmsd1 ), 1.0e-30 ),
              5.0 );
}

// ---------------------------------------------------------------------------
// The defect this class exists to avoid.  US_SolveSim writes only the first
// data set's block; every later block stays zero and no noise is subtracted
// from that data.
//
// This test asserts the *old* behaviour.  If US_SolveSim is ever fixed as
// well, it will fail -- at which point US_SolveSimMDS has no reason to exist
// and both it and this test should go.
// ---------------------------------------------------------------------------
TEST_F( TestSolveSimMDS, US_SolveSimLeavesLaterDataSetsWithoutNoise )
{
   Series series;
   series.add( BUF_WATER, 0, true, false );
   series.add( BUF_D2O,   1, true, false );

   const int npoints = series.list()[ 0 ]->run_data.pointCount();

   US_SolveSim::Simulation sv = make_sim_vals( 1 );
   US_SolveSim solver( series.list(), 1, false );
   solver.calc_residuals( 0, series.size(), sv );

   ASSERT_EQ( 2 * npoints, sv.ti_noise.size() );

   // The second block was never written.
   EXPECT_DOUBLE_EQ( 0.0, rms( sv.ti_noise, npoints, npoints ) );

   // And so the second data set keeps its noise in the residual.  The
   // injected profile has an RMS near 0.007 OD; the first data set is fitted
   // far below that and the second is not.
   ASSERT_EQ( 2, sv.variances.size() );
   const double rmsd0 = std::sqrt( sv.variances[ 0 ] );
   const double rmsd1 = std::sqrt( sv.variances[ 1 ] );

   EXPECT_GT( rmsd1 / qMax( rmsd0, 1.0e-30 ), 5.0 )
      << "US_SolveSim is expected to fit only the first data set's noise";
}

// ---------------------------------------------------------------------------
// A series with no noise requested must be untouched by any of this.
// ---------------------------------------------------------------------------
TEST_F( TestSolveSimMDS, NoNoiseRequestedMatchesUS_SolveSimOnASeries )
{
   Series series;
   series.add( BUF_WATER, 0, false, false );
   series.add( BUF_D2O,   1, false, false );

   US_SolveSim::Simulation ref = make_sim_vals( 0 );
   US_SolveSim::Simulation mds = make_sim_vals( 0 );

   US_SolveSim    old_solver( series.list(), 1, false );
   US_SolveSimMDS new_solver( series.list(), 1, false );

   old_solver.calc_residuals( 0, series.size(), ref );
   new_solver.calc_residuals( 0, series.size(), mds );

   EXPECT_DOUBLE_EQ( ref.variance, mds.variance );
   ASSERT_EQ( ref.solutes.size(), mds.solutes.size() );

   for ( int ii = 0; ii < ref.solutes.size(); ii++ )
      EXPECT_NEAR( ref.solutes[ ii ].c, mds.solutes[ ii ].c, 1.0e-12 );
}
