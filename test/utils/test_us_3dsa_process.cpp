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
#include "us_astfem_math.h"
#include "us_astfem_rsa.h"
#include "us_buffer.h"
#include "us_dataIO.h"
#include "us_fvm_gradients.h"
#include "us_lamm_astfvm.h"
#include "us_math2.h"
#include "us_model.h"
#include "us_sim_cache.h"
#include "us_simparms.h"
#include "us_solute.h"
#include "us_solve_sim.h"

#include <cmath>
#include <memory>
#include <vector>

namespace
{

struct Buffer
{
      double density;
      double viscosity;
      double temperature;
};

const Buffer           BUF_WATER{ 0.998234, 1.001940, 20.0 };
const Buffer           BUF_HALF{ 1.050000, 1.120000, 20.0 };
const Buffer           BUF_D2O{ 1.105000, 1.251000, 20.0 };

// The species the synthetic data is generated from.
const double           TRUE_S    = 4.0e-13;
const double           TRUE_FF0  = 1.5;
const double           TRUE_VBAR = 0.73;
const double           TRUE_CONC = 0.50;

// Cheap but realistic simulation geometry: enough scans and radial points to
// resolve a boundary, few enough for a unit test.
const int              NSCANS    = 4;
const int              RPM       = 45000;
const double           MENISCUS  = 5.85;
const double           BOTTOM    = 7.15;
const double           DELTA_R   = 0.005;
const int              SIMPOINTS = 100;

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

US_SimulationParameters make_simparams( int rpm = RPM )
{
   US_SimulationParameters sp;
   sp.simpoints                       = SIMPOINTS;
   sp.radial_resolution               = DELTA_R;
   sp.meniscus                        = MENISCUS;
   sp.bottom                          = BOTTOM;
   sp.bottom_position                 = BOTTOM;
   sp.temperature                     = 20.0;
   sp.sim                             = true;
   sp.speed_step[0].duration_hours    = 5;
   sp.speed_step[0].duration_minutes  = 0.0;
   sp.speed_step[0].delay_hours       = 0;
   sp.speed_step[0].delay_minutes     = 0.0;
   sp.speed_step[0].scans             = NSCANS;
   sp.speed_step[0].rotorspeed        = rpm;
   sp.speed_step[0].acceleration      = 400;
   sp.speed_step[0].acceleration_flag = false;
   return sp;
}

// An empty data grid on the geometry above, ready for the solver to fill.
void make_grid( US_DataIO::RawData& data, int rpm = RPM )
{
   const double w2 = std::pow( rpm * M_PI / 30.0, 2.0 );

   data.xvalues.clear();
   data.scanData.clear();

   for ( double rr = MENISCUS; rr <= BOTTOM + 1.0e-9; rr += DELTA_R )
   {
      data.xvalues << rr;
   }

   const int npoints = data.xvalues.size();

   for ( int ii = 0; ii < NSCANS; ii++ )
   {
      US_DataIO::Scan sc;
      sc.temperature = 20.0;
      sc.rpm         = (double) rpm;
      sc.seconds     = 2400.0 * ( ii + 1 );
      sc.omega2t     = w2 * sc.seconds;
      sc.wavelength  = 280.0;
      sc.plateau     = 0.0;
      sc.delta_r     = DELTA_R;
      sc.rvalues     = QVector<double>( npoints, 0.0 );
      data.scanData << sc;
   }
}

// Simulate a standard-space (20W) model as it would be observed in one
// buffer, scaled by that cell's loading.
void simulate_in( const US_Model& truth_20w, const Buffer& b, double loading, US_DataIO::RawData& out, int rpm = RPM )
{
   US_Model expt = truth_20w;

   for ( int cc = 0; cc < expt.components.size(); cc++ )
   {
      US_Math2::SolutionData sd = corrections( truth_20w.components[cc].vbar20, b );
      expt.components[cc].s /= sd.s20w_correction;
      expt.components[cc].D /= sd.D20w_correction;
      expt.components[cc].signal_concentration *= loading;
   }

   make_grid( out, rpm );

   US_SimulationParameters sp = make_simparams( rpm );
   US_SimulationParameters::computeSpeedSteps( &out.scanData, sp.speed_step );

   // A timestate of this run's own.  Left to US_Astfem_RSA, a NO_DB build
   // would name it by process and second only, and a second run simulated
   // within the same second -- at another speed -- would be generated with
   // this one's speed profile.  (Design document section 3D.)
   // The sim flag first, from the channel, exactly as calculate() sets it
   // before writing a timestate itself.
   QTemporaryDir tmst_dir;
   const QString tmst = tmst_dir.filePath( "simulated.time_state.tmst" );
   sp.sim             = ( out.channel == 'S' );
   US_AstfemMath::writetimestate( tmst, sp, out );
   sp.simSpeedsFromTimeState( tmst );

   US_Astfem_RSA astfem( expt, sp );
   astfem.set_debug_flag( 0 );
   astfem.calculate( out );
}

// Wrap simulated data as an edited dataset the solver will accept.
US_SolveSim::DataSet* make_dataset( const US_Model& truth_20w, const Buffer& b, double loading, double nominal_vbar, int rpm = RPM )
{
   US_DataIO::RawData sim;
   simulate_in( truth_20w, b, loading, sim, rpm );

   US_SolveSim::DataSet* d = new US_SolveSim::DataSet;

   d->run_data.runID      = "synthetic";
   d->run_data.dataType   = "RA";
   d->run_data.cell       = "1";
   d->run_data.channel    = "A";
   d->run_data.wavelength = "280";
   d->run_data.expType    = "velocity";
   d->run_data.meniscus   = MENISCUS;
   d->run_data.bottom     = BOTTOM;
   d->run_data.baseline   = 0.0;
   d->run_data.plateau    = 0.0;
   d->run_data.ODlimit    = 1.0e+30; // never substitute zeros
   d->run_data.xvalues    = sim.xvalues;
   d->run_data.scanData   = sim.scanData;

   d->simparams = make_simparams( rpm );
   US_SimulationParameters::computeSpeedSteps( &d->run_data.scanData, d->simparams.speed_step );

   d->density     = b.density;
   d->viscosity   = b.viscosity;
   d->temperature = b.temperature;
   d->manual      = false;
   d->compress    = 0.0;
   d->vbar20      = nominal_vbar;

   US_Math2::SolutionData sd = corrections( nominal_vbar, b );
   d->vbartb                 = sd.vbar;
   d->s20w_correction        = sd.s20w_correction;
   d->D20w_correction        = sd.D20w_correction;
   d->solute_type            = US_3dsaProcess::mask_s_k_v();
   d->centerpiece_bottom     = BOTTOM;
   d->rotor_stretch[0]       = 0.0;
   d->rotor_stretch[1]       = 0.0;

   return d;
}

US_Model truth_model()
{
   US_Model m;
   m.components.resize( 1 );
   m.components[0].s                    = TRUE_S;
   m.components[0].f_f0                 = TRUE_FF0;
   m.components[0].vbar20               = TRUE_VBAR;
   m.components[0].D                    = 0.0;
   m.components[0].mw                   = 0.0;
   m.components[0].f                    = 0.0;
   m.components[0].signal_concentration = TRUE_CONC;
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
      const double conc = m.components[cc].signal_concentration;
      num += m.components[cc].vbar20 * conc;
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
      const double conc = m.components[cc].signal_concentration;
      num += m.components[cc].s * conc;
      den += conc;
   }

   return ( den > 0.0 ) ? ( num / den ) : 0.0;
}

// ---- a density gradient --------------------------------------------------
//
// A cell of CsCl: the salt sediments during the run and forms a density
// gradient across the cell, so a solute meets a different density at every
// radius and its own vbar shapes its simulation.  Short and coarse, for a
// unit test.
const int    GRAD_RPM       = 48000;
const double GRAD_MENISCUS  = 6.0;
const double GRAD_BOTTOM    = 7.2;
const double GRAD_DELTA_R   = 0.004;
const int    GRAD_SIMPOINTS = 50;
const int    GRAD_SCANS     = 6;
const double GRAD_CSCL      = 1.5; // M

// Water as the buffer's base, plus CsCl as a co-sedimenting component.  The
// density and viscosity coefficients are etc/bufferComponents.xml's,
// scaled the way US_Buffer::compositeCoeffs scales them so a polynomial in
// molar concentration gives g/mL and cP; s, D and vbar are literature-scale
// values for CsCl.
US_Buffer    gradient_buffer( double molar )
{
   US_Buffer b;
   b.description = "CsCl gradient";
   b.density     = BUF_WATER.density;
   b.viscosity   = BUF_WATER.viscosity;
   b.manual      = false;

   US_CosedComponent water;
   water.id            = 1;
   water.componentID   = "1";
   water.GUID          = "3dsa-test-water";
   water.name          = "Water";
   water.dens_coeff[0] = BUF_WATER.density;
   water.visc_coeff[0] = BUF_WATER.viscosity;
   water.s_coeff       = 0.0;
   water.d_coeff       = 0.0;
   water.vbar          = 1.0;
   water.conc          = 1.0;
   water.overlaying    = false;
   b.cosed_component << water;

   if ( molar > 0.0 )
   {
      const double      dtab[6]  = { 0.0, 0.0, 12.68641, 1.27445, -11.95400, 258.86600 };
      const double      vtab[6]  = { 0.0, -19.41040, -4.07863, 11.54890, -21.77400, 0.0 };
      const double      scale[6] = { 1.0, 1.0e-3, 1.0e-2, 1.0e-3, 1.0e-4, 1.0e-6 };

      US_CosedComponent cs;
      cs.id          = 2;
      cs.componentID = "13";
      cs.GUID        = "3dsa-test-cscl";
      cs.name        = "Cesium chloride";
      for ( int jj = 0; jj < 6; jj++ )
      {
         cs.dens_coeff[jj] = dtab[jj] * scale[jj];
         cs.visc_coeff[jj] = vtab[jj] * scale[jj];
      }
      cs.s_coeff    = 1.06e-13;
      cs.d_coeff    = 2.0e-5;
      cs.vbar       = 0.23;
      cs.conc       = molar;
      cs.overlaying = false;
      b.cosed_component << cs;
   }

   return b;
}

// The buffer of a band-forming cell: the cell holds the CsCl buffer, and the
// sample is layered on from a lamella of buffer with sucrose in it.  The
// lamella is still the lighter (1.04 g/mL against 1.19), so the band is
// stable; its sucrose diffuses down while the CsCl sediments.
US_Buffer band_forming_buffer( double cscl_molar, double sucrose_molar )
{
   US_Buffer         b        = gradient_buffer( cscl_molar );
   const double      dtab[6]  = { 0.0, -0.21963, 13.23726, -1.83647, 0.0, -25.13871 };
   const double      vtab[6]  = { 0.0, -52.13491, 107.94530, 0.0, 11916.33984, 0.0 };
   const double      scale[6] = { 1.0, 1.0e-3, 1.0e-2, 1.0e-3, 1.0e-4, 1.0e-6 };

   US_CosedComponent su;
   su.id          = 3;
   su.componentID = "86";
   su.GUID        = "3dsa-test-sucrose";
   su.name        = "Sucrose";
   for ( int jj = 0; jj < 6; jj++ )
   {
      su.dens_coeff[jj] = dtab[jj] * scale[jj];
      su.visc_coeff[jj] = vtab[jj] * scale[jj];
   }
   su.s_coeff    = 0.0;
   su.d_coeff    = 5.2e-6;
   su.vbar       = 0.63;
   su.conc       = sucrose_molar;
   su.overlaying = true;
   b.description = "Band forming on CsCl";
   b.cosed_component << su;
   return b;
}

US_SimulationParameters gradient_simparams()
{
   US_SimulationParameters sp;
   sp.meshType                        = US_SimulationParameters::ASTFVM;
   sp.simpoints                       = GRAD_SIMPOINTS;
   sp.radial_resolution               = GRAD_DELTA_R;
   sp.meniscus                        = GRAD_MENISCUS;
   sp.bottom                          = GRAD_BOTTOM;
   sp.bottom_position                 = GRAD_BOTTOM;
   sp.temperature                     = 20.0;
   sp.sim                             = false;
   sp.speed_step[0].duration_hours    = 2;
   sp.speed_step[0].duration_minutes  = 0.0;
   sp.speed_step[0].delay_hours       = 0;
   sp.speed_step[0].delay_minutes     = 0.0;
   sp.speed_step[0].scans             = GRAD_SCANS;
   sp.speed_step[0].rotorspeed        = GRAD_RPM;
   sp.speed_step[0].acceleration      = 400;
   sp.speed_step[0].acceleration_flag = true;
   return sp;
}

// The same run, with the sample layered on from a 15 uL lamella through a
// band-forming centerpiece
US_SimulationParameters band_forming_simparams()
{
   US_SimulationParameters sp = gradient_simparams();
   sp.band_forming            = true;
   sp.band_volume             = 0.015;
   sp.cp_pathlen              = 1.2;
   sp.cp_angle                = 2.5;
   return sp;
}

// Scans over two hours, with the rotor reaching speed at 400 rpm/s so that
// omega-squared-t is what a real run records.
void make_gradient_grid( US_DataIO::RawData& data )
{
   const double w2    = std::pow( GRAD_RPM * M_PI / 30.0, 2.0 );
   const double t_acc = GRAD_RPM / 400.0;

   data.xvalues.clear();
   data.scanData.clear();
   data.channel = 'A';

   for ( double rr = GRAD_MENISCUS; rr <= GRAD_BOTTOM + 1.0e-9; rr += GRAD_DELTA_R )
   {
      data.xvalues << rr;
   }

   for ( int ii = 0; ii < GRAD_SCANS; ii++ )
   {
      US_DataIO::Scan sc;
      sc.temperature = 20.0;
      sc.rpm         = (double) GRAD_RPM;
      sc.seconds     = 1200.0 * ( ii + 1 );
      sc.omega2t     = w2 * ( sc.seconds - t_acc ) + w2 * t_acc / 3.0;
      sc.wavelength  = 280.0;
      sc.plateau     = 0.0;
      sc.delta_r     = GRAD_DELTA_R;
      sc.rvalues     = QVector<double>( data.xvalues.size(), 0.0 );
      data.scanData << sc;
   }
}

// One run in a gradient buffer, simulated the way the fit will simulate it:
// the ASTFVM, from the standard-space model, with the gradient prepared on
// the same grid the engine will prepare it on.
US_SolveSim::DataSet* make_gradient_dataset( const US_Model&                truth_20w,
                                             const US_Buffer&               buffer,
                                             double                         nominal_vbar,
                                             const US_SimulationParameters& run_sp = gradient_simparams() )
{
   US_DataIO::RawData      sim;
   US_SimulationParameters sp = run_sp;
   make_gradient_grid( sim );
   US_SimulationParameters::computeSpeedSteps( &sim.scanData, sp.speed_step );

   QTemporaryDir tmst_dir;
   const QString tmst = tmst_dir.filePath( "gradient.time_state.tmst" );
   US_AstfemMath::writetimestate( tmst, sp, sim );
   sp.simSpeedsFromTimeState( tmst );

   US_FvmGradients grads;
   QString         msg;
   const double    t_end = sim.scanData.last().seconds;
   if ( !grads.prepare( buffer, sp, US_FvmGradients::gradient_grid( sp, t_end, 100, qMin( 0.002, GRAD_DELTA_R ) ), msg ) )
   {
      ADD_FAILURE() << msg.toStdString();
   }

   US_Model      model = truth_20w;
   US_LammAstfvm astfvm( model, sp );
   astfvm.set_buffer( buffer, grads.band_forming(), grads.cosed() );
   astfvm.calculate( sim );

   US_SolveSim::DataSet* d = new US_SolveSim::DataSet;
   d->run_data.runID       = "gradient";
   d->run_data.dataType    = "RA";
   d->run_data.cell        = "1";
   d->run_data.channel     = "A";
   d->run_data.wavelength  = "280";
   d->run_data.expType     = "velocity";
   d->run_data.meniscus    = GRAD_MENISCUS;
   d->run_data.bottom      = GRAD_BOTTOM;
   d->run_data.baseline    = 0.0;
   d->run_data.plateau     = 0.0;
   d->run_data.ODlimit     = 1.0e+30;
   d->run_data.xvalues     = sim.xvalues;
   d->run_data.scanData    = sim.scanData;

   d->simparams = run_sp;
   US_SimulationParameters::computeSpeedSteps( &d->run_data.scanData, d->simparams.speed_step );
   d->solution_rec.buffer = buffer;

   const Buffer base{ buffer.density, buffer.viscosity, 20.0 };
   d->density                = base.density;
   d->viscosity              = base.viscosity;
   d->temperature            = 20.0;
   d->manual                 = false;
   d->vbar20                 = nominal_vbar;
   US_Math2::SolutionData sd = corrections( nominal_vbar, base );
   d->vbartb                 = sd.vbar;
   d->s20w_correction        = sd.s20w_correction;
   d->D20w_correction        = sd.D20w_correction;
   d->solute_type            = US_3dsaProcess::mask_s_k_v();
   d->centerpiece_bottom     = GRAD_BOTTOM;
   return d;
}

US_Model gradient_truth()
{
   US_Model m;
   m.components.resize( 1 );
   m.components[0].s                    = 10.0e-13;
   m.components[0].f_f0                 = 1.5;
   m.components[0].vbar20               = 0.80;
   m.components[0].D                    = 0.0;
   m.components[0].mw                   = 0.0;
   m.components[0].f                    = 0.0;
   m.components[0].signal_concentration = 0.5;
   m.update_coefficients();
   return m;
}

// A grid around the gradient truth, with the truth on a grid point.
US_3dsaProcess::Parameters gradient_parameters()
{
   US_3dsaProcess::Parameters p;
   p.x_min      = 6.0e-13;
   p.x_max      = 14.0e-13;
   p.x_res      = 5; // step 2 S
   p.y_min      = 1.25;
   p.y_max      = 1.75;
   p.y_res      = 3; // step 0.25
   p.z_min      = 0.76;
   p.z_max      = 0.84;
   p.z_res      = 5; // step 0.02
   p.grid_reps  = 1;
   p.s_mask     = US_3dsaProcess::mask_s_k_v();
   p.nthreads   = 1;
   p.noisflag   = 0;
   p.max_tsols  = 200;
   p.fit_scales = false;
   return p;
}

// Owns a series of datasets and hands out the QList the engine wants.
class Series
{
   public:
      void add( const Buffer& b, double loading, int rpm = RPM )
      {
         owned_.emplace_back( make_dataset( truth_model(), b, loading, TRUE_VBAR, rpm ) );
         list_ << owned_.back().get();
      }
      QList<US_SolveSim::DataSet*>& list()
      {
         return list_;
      }

   private:
      std::vector<std::unique_ptr<US_SolveSim::DataSet>> owned_;
      QList<US_SolveSim::DataSet*>                       list_;
};

// A grid that straddles the truth, with the true values landing exactly on
// grid points so the test measures the fit rather than the discretisation.
US_3dsaProcess::Parameters base_parameters()
{
   US_3dsaProcess::Parameters p;
   p.x_min      = 2.0e-13;
   p.x_max      = 6.0e-13;
   p.x_res      = 5; // step 1.0
   p.y_min      = 1.00;
   p.y_max      = 2.00;
   p.y_res      = 5; // step 0.25
   p.z_min      = 0.65;
   p.z_max      = 0.81;
   p.z_res      = 5; // step 0.04
   p.grid_reps  = 1;
   p.s_mask     = US_3dsaProcess::mask_s_k_v();
   p.nthreads   = 4;
   p.noisflag   = 0;
   p.max_tsols  = 200;
   p.fit_scales = true;
   return p;
}

} // namespace

class TestUS3dsaProcess : public QtTestBase
{
};

// ---------------------------------------------------------------------------
// The gate: a series without contrast must be refused, not fitted.
// ---------------------------------------------------------------------------
TEST_F( TestUS3dsaProcess, RefusesASeriesWithoutContrast )
{
   Series series;
   series.add( BUF_WATER, 1.0 );
   series.add( BUF_WATER, 1.0 );

   US_3dsaProcess         proc( series.list() );
   US_3dsaProcess::Result result;

   EXPECT_FALSE( proc.fit( base_parameters(), result ) );
   EXPECT_FALSE( proc.lastError().isEmpty() );
   EXPECT_DOUBLE_EQ( 0.0, result.contrast );
}

TEST_F( TestUS3dsaProcess, RefusesASingleDataset )
{
   Series series;
   series.add( BUF_WATER, 1.0 );

   US_3dsaProcess         proc( series.list() );
   US_3dsaProcess::Result result;

   EXPECT_FALSE( proc.fit( base_parameters(), result ) );
}

TEST_F( TestUS3dsaProcess, RejectsAnUnusableMask )
{
   Series series;
   series.add( BUF_WATER, 1.0 );
   series.add( BUF_D2O, 1.0 );

   US_3dsaProcess::Parameters p = base_parameters();
   p.s_mask = ( US_Solute::ATTR_S << 6 ) | ( US_Solute::ATTR_D << 3 ) | US_Solute::ATTR_W; // D and MW share one field

   US_3dsaProcess         proc( series.list() );
   US_3dsaProcess::Result result;

   EXPECT_FALSE( proc.fit( p, result ) );
   EXPECT_TRUE( proc.lastError().contains( "MW" ) ) << proc.lastError().toStdString();
}

// ---------------------------------------------------------------------------
// The exit criterion: recover vbar from a contrast series.
// ---------------------------------------------------------------------------
TEST_F( TestUS3dsaProcess, RecoversVbarFromAContrastSeries )
{
   // Three buffers, three different cell loadings.
   Series series;
   series.add( BUF_WATER, 1.00 );
   series.add( BUF_HALF, 0.70 );
   series.add( BUF_D2O, 1.40 );

   US_3dsaProcess         proc( series.list() );
   US_3dsaProcess::Result result;

   QObject::connect(
      &proc, &US_3dsaProcess::message_update, []( QString m ) { std::printf( "  | %s\n", m.toStdString().c_str() ); } );

   ASSERT_TRUE( proc.fit( base_parameters(), result ) ) << proc.lastError().toStdString();

   std::printf( "\n  %s\n", result.report.toStdString().c_str() );
   std::printf( "  scales:" );
   for ( double sc : result.scales )
   {
      std::printf( " %.4f", sc );
   }
   std::printf( "\n  components: %d   weighted vbar %.4f   weighted s %.4f S\n",
                (int) result.model.components.size(),
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
   EXPECT_NEAR( result.scales[0], 1.00, 1.0e-9 ); // the gauge
   EXPECT_NEAR( result.scales[1], 0.70, 0.01 );
   EXPECT_NEAR( result.scales[2], 1.40, 0.01 );

   // Total concentration matches the loading of the gauge data set.
   double total_conc = 0.0;
   for ( int cc = 0; cc < result.model.components.size(); cc++ )
   {
      total_conc += result.model.components[cc].signal_concentration;
   }
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
   auto run = [&]( bool fit_scales, US_3dsaProcess::Result& result )
   {
      Series series;
      series.add( BUF_WATER, 1.00 );
      series.add( BUF_HALF, 0.70 );
      series.add( BUF_D2O, 1.40 );

      US_3dsaProcess             proc( series.list() );
      US_3dsaProcess::Parameters p = base_parameters();
      p.fit_scales                 = fit_scales;

      return proc.fit( p, result );
   };

   US_3dsaProcess::Result with_scales;
   US_3dsaProcess::Result without;

   ASSERT_TRUE( run( true, with_scales ) );
   ASSERT_TRUE( run( false, without ) );

   const double err_with    = std::fabs( weighted_vbar( with_scales.model ) - TRUE_VBAR );
   const double err_without = std::fabs( weighted_vbar( without.model ) - TRUE_VBAR );

   std::printf( "\n  vbar error with scales %.4f, without %.4f\n", err_with, err_without );
   std::printf( "  rmsd with scales %.3e, without %.3e\n", with_scales.rmsd, without.rmsd );

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
   auto run = [&]( int grid_reps, int nthreads, US_3dsaProcess::Result& result )
   {
      Series series;
      series.add( BUF_WATER, 1.00 );
      series.add( BUF_D2O, 1.00 );

      US_3dsaProcess             proc( series.list() );
      US_3dsaProcess::Parameters p = base_parameters();
      p.grid_reps                  = grid_reps;
      p.nthreads                   = nthreads;
      p.fit_scales                 = false; // isolate the partitioning

      return proc.fit( p, result );
   };

   US_3dsaProcess::Result one;
   US_3dsaProcess::Result many;

   ASSERT_TRUE( run( 1, 1, one ) );
   ASSERT_TRUE( run( 2, 4, many ) );

   // Both cover the same grid; only the partitioning differs.
   EXPECT_EQ( one.ngrid, many.ngrid );
   EXPECT_EQ( 1, one.nsubgrids );
   EXPECT_EQ( 8, many.nsubgrids );
   EXPECT_GT( many.ntasks, one.ntasks );

   const double vbar_step = ( 0.81 - 0.65 ) / 4.0; // 0.04
   const double s_step    = ( 6.0 - 2.0 ) / 4.0;   // 1.0 S

   // One subgrid fits every column together and lands on the truth.
   EXPECT_NEAR( weighted_vbar( one.model ), TRUE_VBAR, 0.002 );
   EXPECT_NEAR( weighted_s( one.model ) * 1.0e13, TRUE_S * 1.0e13, 0.05 );

   // Eight subgrids give up some accuracy for the smaller NNLS problems, but
   // must still land within half a grid step.  Note this is a deliberately
   // punishing case: two repetitions over a five-point axis leaves each
   // subgrid a very coarse covering, where a production run has eight
   // repetitions over sixty-four points.
   EXPECT_LT( std::fabs( weighted_vbar( many.model ) - TRUE_VBAR ), vbar_step * 0.5 )
      << "subgridded vbar " << weighted_vbar( many.model );
   EXPECT_LT( std::fabs( weighted_s( many.model ) - TRUE_S ) * 1.0e13, s_step * 0.5 );
}

// ---------------------------------------------------------------------------
// The model comes back in standard space with the analysis type set, which
// is what a model file holds.
// ---------------------------------------------------------------------------
TEST_F( TestUS3dsaProcess, ProducesAStandardSpaceModel )
{
   Series series;
   series.add( BUF_WATER, 1.0 );
   series.add( BUF_D2O, 1.0 );

   US_3dsaProcess         proc( series.list() );
   US_3dsaProcess::Result result;
   ASSERT_TRUE( proc.fit( base_parameters(), result ) );

   EXPECT_EQ( US_Model::THREEDSA, result.model.analysis );

   // A fit over more than one data set is a global fit, and typeText()
   // labels it so.
   EXPECT_EQ( US_Model::GLOBAL, result.model.global );
   EXPECT_EQ( QString( "3DSA-GL" ), result.model.typeText() );

   for ( int cc = 0; cc < result.model.components.size(); cc++ )
   {
      const US_Model::SimulationComponent& c = result.model.components[cc];

      // Coefficients are complete and self-consistent.
      EXPECT_GT( c.s, 0.0 );
      EXPECT_GT( c.D, 0.0 );
      EXPECT_GT( c.mw, 0.0 );
      EXPECT_GE( c.f_f0, 1.0 );
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

// ---------------------------------------------------------------------------
// The headless build's Lamm-equation solver keeps process-wide state (design
// document section 3D):  a temporary timestate named by process and second,
// and function-static work arrays.  The engine must not depend on either.
// ---------------------------------------------------------------------------
TEST_F( TestUS3dsaProcess, PreparesATimestateOfItsOwnForEachDataSet )
{
   Series series;
   series.add( BUF_WATER, 1.0, 45000 );
   series.add( BUF_D2O, 1.0, 30000 );

   QTemporaryDir dir;
   ASSERT_TRUE( dir.isValid() );
   QString msg;
   ASSERT_TRUE( US_3dsaProcess::prepare_timestates( series.list(), dir.path(), msg ) ) << msg.toStdString();

   const int expected_rpm[] = { 45000, 30000 };
   for ( int dd = 0; dd < 2; dd++ )
   {
      const US_SimulationParameters& sp = series.list()[dd]->simparams;
      EXPECT_NE( sp.tsobj, nullptr );
      ASSERT_FALSE( sp.sim_speed_prof.isEmpty() );
      EXPECT_EQ( sp.sim_speed_prof[0].rotorspeed, expected_rpm[dd] ) << "data set " << dd;
   }

   // One file each, not one per process and second
   EXPECT_EQ( QDir( dir.path() ).entryList( QStringList( "*.tmst" ), QDir::Files ).size(), 2 );

   // A data set that already has a timestate keeps it
   US_TimeState* before = series.list()[0]->simparams.tsobj;
   ASSERT_TRUE( US_3dsaProcess::prepare_timestates( series.list(), dir.path(), msg ) );
   EXPECT_EQ( series.list()[0]->simparams.tsobj, before );
}

TEST_F( TestUS3dsaProcess, RunTimingMayDifferAcrossTheSeries )
{
   // A real series is several runs, rarely at one speed.  Before each data set
   // got its own timestate, a NO_DB build simulated every cell with the speed
   // profile of whichever was simulated first in that second, and the fitted
   // vbar was wrong with nothing to show for it.
   Series series;
   series.add( BUF_WATER, 1.00, 45000 );
   series.add( BUF_HALF, 0.70, 50000 );
   series.add( BUF_D2O, 1.40, 38000 );

   US_3dsaProcess         proc( series.list() );
   US_3dsaProcess::Result result;
   ASSERT_TRUE( proc.fit( base_parameters(), result ) ) << proc.lastError().toStdString();

   EXPECT_NEAR( weighted_vbar( result.model ), TRUE_VBAR, 0.005 );
   EXPECT_NEAR( weighted_s( result.model ) * 1.0e13, TRUE_S * 1.0e13, 0.05 );
   EXPECT_LT( result.rmsd, 1.0e-3 );
}

TEST_F( TestUS3dsaProcess, ThreadsDoNotChangeTheFit )
{
#ifdef NO_DB
   EXPECT_FALSE( US_3dsaProcess::concurrent_solves_safe() );
#else
   EXPECT_TRUE( US_3dsaProcess::concurrent_solves_safe() );
#endif

   Series series;
   series.add( BUF_WATER, 1.00 );
   series.add( BUF_HALF, 0.70 );
   series.add( BUF_D2O, 1.40 );

   // Eight subgrids, so the first level really has tasks to share out
   US_3dsaProcess::Parameters parms = base_parameters();
   parms.grid_reps                  = 2;

   parms.nthreads = 4;
   US_3dsaProcess         threaded( series.list() );
   US_3dsaProcess::Result rthr;
   ASSERT_TRUE( threaded.fit( parms, rthr ) ) << threaded.lastError().toStdString();
   EXPECT_EQ( rthr.nthreads, US_3dsaProcess::concurrent_solves_safe() ? 4 : 1 );

   parms.nthreads = 1;
   US_3dsaProcess         serial( series.list() );
   US_3dsaProcess::Result rser;
   ASSERT_TRUE( serial.fit( parms, rser ) ) << serial.lastError().toStdString();
   EXPECT_EQ( rser.nthreads, 1 );

   // Tasks are independent and merged in task order, so the answer must not
   // depend on how many threads ran them
   EXPECT_DOUBLE_EQ( rthr.rmsd, rser.rmsd );
   EXPECT_DOUBLE_EQ( weighted_vbar( rthr.model ), weighted_vbar( rser.model ) );
   EXPECT_EQ( rthr.model.components.size(), rser.model.components.size() );

   // The engine works on copies: the callers' data sets stay as they were
   for ( US_SolveSim::DataSet* dset : series.list() )
   {
      EXPECT_EQ( dset->simparams.tsobj, nullptr );
   }
}

// ---------------------------------------------------------------------------
// The amplitudes change from one iteration to the next, the simulations do
// not: the cache must cut the solves to one per grid point and data set
// without changing the answer at all.
// ---------------------------------------------------------------------------
TEST_F( TestUS3dsaProcess, CacheDoesNotChangeTheFit )
{
   Series series;
   series.add( BUF_WATER, 1.00 );
   series.add( BUF_HALF, 0.70 );
   series.add( BUF_D2O, 1.40 );

   US_3dsaProcess::Parameters parms = base_parameters();

   parms.use_cache = false;
   US_3dsaProcess         plain( series.list() );
   US_3dsaProcess::Result rpl;
   ASSERT_TRUE( plain.fit( parms, rpl ) ) << plain.lastError().toStdString();

   parms.use_cache = true;
   US_3dsaProcess         cached( series.list() );
   US_3dsaProcess::Result rca;
   ASSERT_TRUE( cached.fit( parms, rca ) ) << cached.lastError().toStdString();

   // The unequal loadings take several amplitude iterations, and without the
   // cache each one simulates the whole grid again
   const int nsolves = rpl.ngrid * series.list().size();
   ASSERT_GT( rpl.nscaliter, 1 );
   EXPECT_GE( rpl.nsimul, rpl.nscaliter * nsolves );

   // "SimCache=off" in the debug text turns the cache off everywhere
   if ( US_SimCache::default_max_bytes() > 0 )
   {
      EXPECT_EQ( rca.nsimul, nsolves );
   }

   // Simulations are reused exactly as computed, so the fit is identical
   EXPECT_EQ( rca.nscaliter, rpl.nscaliter );
   EXPECT_EQ( rca.rmsd, rpl.rmsd );
   EXPECT_EQ( rca.scales, rpl.scales );
   ASSERT_EQ( rca.model.components.size(), rpl.model.components.size() );

   for ( int cc = 0; cc < rca.model.components.size(); cc++ )
   {
      const US_Model::SimulationComponent& a = rca.model.components[cc];
      const US_Model::SimulationComponent& b = rpl.model.components[cc];
      EXPECT_EQ( a.s, b.s ) << "component " << cc;
      EXPECT_EQ( a.D, b.D ) << "component " << cc;
      EXPECT_EQ( a.vbar20, b.vbar20 ) << "component " << cc;
      EXPECT_EQ( a.signal_concentration, b.signal_concentration ) << "component " << cc;
   }
}


// ---------------------------------------------------------------------------
// A buffer that forms a density gradient: prepared once per data set, over
// the whole cell, and only for the ASTFVM.
// ---------------------------------------------------------------------------
TEST_F( TestUS3dsaProcess, PreparesTheGradientOfACosedimentingBuffer )
{
   std::unique_ptr<US_SolveSim::DataSet> grad( make_gradient_dataset( gradient_truth(), gradient_buffer( GRAD_CSCL ), 0.80 ) );
   std::unique_ptr<US_SolveSim::DataSet> plain( make_gradient_dataset( gradient_truth(), gradient_buffer( 0.0 ), 0.80 ) );

   QList<US_SolveSim::DataSet*>          list{ grad.get(), plain.get() };
   QTemporaryDir                         tdir;
   QString                               msg;
   ASSERT_TRUE( US_3dsaProcess::prepare_timestates( list, tdir.path(), msg ) ) << msg.toStdString();
   ASSERT_TRUE( US_3dsaProcess::prepare_gradients( list, msg ) ) << msg.toStdString();

   // The CsCl cell spans a real range of densities; the water cell, though
   // simulated by the ASTFVM too, prepares nothing
   ASSERT_FALSE( grad->fvm.isNull() );
   EXPECT_TRUE( grad->fvm->is_gradient() );
   EXPECT_TRUE( std::isfinite( grad->fvm->density_lo() ) );
   EXPECT_GT( grad->fvm->density_lo(), 1.05 );
   EXPECT_GT( grad->fvm->density_hi() - grad->fvm->density_lo(), 0.05 );
   EXPECT_TRUE( plain->fvm.isNull() );

   // The ASTFEM would ignore the gradient: refuse rather than fit it wrong
   grad->simparams.meshType = US_SimulationParameters::ASTFEM;
   grad->fvm.reset();
   EXPECT_FALSE( US_3dsaProcess::prepare_gradients( list, msg ) );
   EXPECT_TRUE( msg.contains( "ASTFVM" ) ) << msg.toStdString();
}

// ---------------------------------------------------------------------------
// In a gradient, one run determines vbar: the solute's buoyancy changes along
// its path, by an amount that depends on its own vbar.  The same species in
// a plain buffer is still refused.
// ---------------------------------------------------------------------------
TEST_F( TestUS3dsaProcess, RecoversVbarFromOneGradientRun )
{
   std::unique_ptr<US_SolveSim::DataSet> grad( make_gradient_dataset( gradient_truth(), gradient_buffer( GRAD_CSCL ), 0.80 ) );
   QList<US_SolveSim::DataSet*>          one{ grad.get() };

   US_3dsaProcess                        proc( one );
   US_3dsaProcess::Result                result;
   QObject::connect(
      &proc, &US_3dsaProcess::message_update, []( QString m ) { std::printf( "  | %s\n", m.toStdString().c_str() ); } );

   ASSERT_TRUE( proc.fit( gradient_parameters(), result ) ) << proc.lastError().toStdString();
   std::printf( "\n  %s\n  weighted vbar %.4f   weighted s %.3f S\n",
                result.report.toStdString().c_str(),
                weighted_vbar( result.model ),
                weighted_s( result.model ) * 1.0e13 );

   EXPECT_GT( result.contrast, US_SolveSim::VBAR_CONTRAST_WARN );
   EXPECT_NEAR( weighted_vbar( result.model ), 0.80, 0.005 );
   EXPECT_NEAR( weighted_s( result.model ) * 1.0e13, 10.0, 0.5 );
   EXPECT_LT( result.rmsd, 1.0e-3 );

   std::unique_ptr<US_SolveSim::DataSet> plain( make_gradient_dataset( gradient_truth(), gradient_buffer( 0.0 ), 0.80 ) );
   QList<US_SolveSim::DataSet*>          lone{ plain.get() };
   US_3dsaProcess                        refused( lone );
   US_3dsaProcess::Result                rplain;
   EXPECT_FALSE( refused.fit( gradient_parameters(), rplain ) );
   EXPECT_TRUE( refused.lastError().contains( "vbar" ) ) << refused.lastError().toStdString();
}

// A band-forming run onto CsCl: the sample starts as a thin lamella with
// sucrose in it.  The engine prepares both gradients -- the sucrose that
// diffuses out of the lamella and the CsCl that sediments -- and the band's
// passage through them determines vbar from the one run.
TEST_F( TestUS3dsaProcess, RecoversVbarFromABandFormingRun )
{
   const US_SimulationParameters         bfe = band_forming_simparams();
   std::unique_ptr<US_SolveSim::DataSet> band(
      make_gradient_dataset( gradient_truth(), band_forming_buffer( GRAD_CSCL, 0.3 ), 0.80, bfe ) );

   // The band is a band: most of the signal of the last scan sits within
   // half a centimetre
   const US_DataIO::Scan& last  = band->run_data.scanData.last();
   double                 total = 0.0;
   double                 peak  = 0.0;
   int                    kpeak = 0;
   for ( int jj = 0; jj < last.rvalues.size(); jj++ )
   {
      total += last.rvalues[jj];
      if ( last.rvalues[jj] > peak )
      {
         peak  = last.rvalues[jj];
         kpeak = jj;
      }
   }
   double in_band = 0.0;
   for ( int jj = 0; jj < last.rvalues.size(); jj++ )
   {
      if ( std::fabs( band->run_data.xvalues[jj] - band->run_data.xvalues[kpeak] ) < 0.25 )
      {
         in_band += last.rvalues[jj];
      }
   }
   ASSERT_GT( total, 0.0 );
   EXPECT_GT( in_band / total, 0.8 );

   QList<US_SolveSim::DataSet*> one{ band.get() };
   QTemporaryDir                tdir;
   QString                      msg;
   ASSERT_TRUE( US_3dsaProcess::prepare_timestates( one, tdir.path(), msg ) ) << msg.toStdString();
   ASSERT_TRUE( US_3dsaProcess::prepare_gradients( one, msg ) ) << msg.toStdString();
   ASSERT_FALSE( band->fvm.isNull() );
   EXPECT_NE( band->fvm->cosed(), nullptr );
   EXPECT_NE( band->fvm->band_forming(), nullptr );
   std::printf( "\n  band-forming cell: density %.4f to %.4f g/mL\n", band->fvm->density_lo(), band->fvm->density_hi() );
   EXPECT_GT( band->fvm->density_hi() - band->fvm->density_lo(), 0.05 );

   US_3dsaProcess         proc( one );
   US_3dsaProcess::Result result;
   QObject::connect(
      &proc, &US_3dsaProcess::message_update, []( QString m ) { std::printf( "  | %s\n", m.toStdString().c_str() ); } );

   // Each band-forming solve steps at the sucrose gradient's few seconds, so
   // the grid is cut to the vbar axis and a few s values around the truth
   US_3dsaProcess::Parameters parms = gradient_parameters();
   parms.x_min                      = 8.0e-13;
   parms.x_max                      = 12.0e-13;
   parms.x_res                      = 3;
   parms.y_min                      = 1.5;
   parms.y_max                      = 1.5;
   parms.y_res                      = 1;

   ASSERT_TRUE( proc.fit( parms, result ) ) << proc.lastError().toStdString();
   std::printf( "\n  %s\n  weighted vbar %.4f   weighted s %.3f S\n",
                result.report.toStdString().c_str(),
                weighted_vbar( result.model ),
                weighted_s( result.model ) * 1.0e13 );

   EXPECT_NEAR( weighted_vbar( result.model ), 0.80, 0.005 );
   EXPECT_NEAR( weighted_s( result.model ) * 1.0e13, 10.0, 0.5 );
   EXPECT_LT( result.rmsd, 1.0e-3 );
}

// A single-scan series -- an ABDE edit keeps the equilibrium scan -- cannot
// resolve both s and f/f0, and the engine says so; it says nothing once one
// of them is held, or when the data carry the approach to equilibrium.
TEST_F( TestUS3dsaProcess, NotesAnUnresolvableEquilibriumGrid )
{
   US_SolveSim::DataSet one_scan;
   US_DataIO::Scan      sc;
   one_scan.run_data.scanData << sc;
   US_SolveSim::DataSet two_scans;
   two_scans.run_data.scanData << sc << sc;

   QList<US_SolveSim::DataSet*> eq{ &one_scan };
   QList<US_SolveSim::DataSet*> vel{ &two_scans };
   QList<US_SolveSim::DataSet*> mixed{ &one_scan, &two_scans };

   US_3dsaProcess::Parameters   parms = gradient_parameters(); // s, f/f0, vbar
   EXPECT_TRUE( US_3dsaProcess::equilibrium_note( eq, parms ).contains( "molar mass" ) );
   EXPECT_TRUE( US_3dsaProcess::equilibrium_note( vel, parms ).isEmpty() );
   EXPECT_TRUE( US_3dsaProcess::equilibrium_note( mixed, parms ).isEmpty() );

   parms.y_min = parms.y_max = 1.5;
   parms.y_res               = 1;
   EXPECT_TRUE( US_3dsaProcess::equilibrium_note( eq, parms ).isEmpty() );

   parms.s_mask = US_3dsaProcess::mask_s_d_v();
   parms.y_res  = 3;
   EXPECT_FALSE( US_3dsaProcess::equilibrium_note( eq, parms ).isEmpty() );
}
