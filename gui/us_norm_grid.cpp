//! \file us_norm_grid.cpp

#include "us_norm_grid.h"
#include "us_settings.h"
#include "us_astfem_math.h"
#include "us_math2.h"
#include "us_constants.h"
#include "us_buffer.h"

#ifndef DbgLv
#define DbgLv(a) if(dbg_level>=a)qDebug()
#endif

US_NormGrid::US_NormGrid( QObject* parent ) : QObject( parent )
{
   dset       = NULL;
   kthrdr     = 0;
   nsolutes   = 0;
   nrm_nss    = 0;
   nrm_nks    = 0;
   nrm_nsamp  = 0;
   nrm_nrad   = 0;
   nrm_nscn   = 0;
   nrm_rstr   = 1;
   nrm_sstr   = 1;
   busy       = false;
   bfgrad     = NULL;
   cosedd     = NULL;
   dbg_level  = US_Settings::us_debug();
}

US_NormGrid::~US_NormGrid()
{
}

void US_NormGrid::set_cosed( US_Math_BF::Band_Forming_Gradient* bfg,
      US_LammAstfvm::CosedData* csd )
{
   bfgrad     = bfg;
   cosedd     = csd;
}

bool US_NormGrid::is_busy( void ) const
{
   return busy;
}

int US_NormGrid::total_steps( void ) const
{
   return nsolutes;
}

US_show_norm* US_NormGrid::viewer( void ) const
{
   return analcd.isNull() ? NULL : analcd.data();
}

void US_NormGrid::close_viewer( void )
{
   if ( ! analcd.isNull() )
      analcd->close();
}

// Resolve the rotor speed profile into the data set's own simulation
//  parameters, as US_SolveSim::calc_residuals() does before a fit.  Done
//  once here rather than in each worker, since the workers share the data
//  set and the profile governs every column's simulation.
void US_NormGrid::resolve_timestate( US_SolveSim::DataSet* a_dset )
{
   if ( a_dset->simparams.tsobj != NULL  &&
        a_dset->simparams.sim_speed_prof.count() > 0 )
      return;                             // Already loaded

   a_dset->simparams.tsobj = NULL;
   a_dset->simparams.sim_speed_prof.clear();

   QString runID      = a_dset->run_data.runID;
   QString tmst_fpath = a_dset->tmst_file.isEmpty()
                      ? ( US_Settings::resultDir() + "/" + runID + "/"
                          + runID + ".time_state.tmst" )
                      : a_dset->tmst_file;
   QFileInfo check_file( tmst_fpath );

   if ( ! check_file.exists()  ||  ! check_file.isFile() )
   {
DbgLv(1) << "NG: no timestate file" << tmst_fpath;
      return;
   }

   // Only a one-second-interval time state can be loaded directly
   US_DataIO::RawData tsimdat;
   US_AstfemMath::initSimData( tsimdat, a_dset->run_data, 0.0 );

   if ( US_AstfemMath::timestate_onesec( tmst_fpath, tsimdat ) )
      a_dset->simparams.simSpeedsFromTimeState( tmst_fpath );

DbgLv(1) << "NG: timestate" << tmst_fpath << "tsobj"
 << a_dset->simparams.tsobj << "sspknt"
 << a_dset->simparams.sim_speed_prof.count();
}

// Build a data set from a simulation (class method)
bool US_NormGrid::dataset_from_sim( US_SolveSim::DataSet& dset,
      const US_Model& model, const US_Buffer& buffer,
      const US_SimulationParameters& simparams,
      US_DataIO::RawData& simdata, QString& errmsg )
{
   errmsg         = QString();

   int nscan      = simdata.scanCount();
   int npoint     = simdata.pointCount();

   if ( nscan < 1  ||  npoint < 2 )
   {
      errmsg         = tr( "The simulation has no data to build a grid on.\n"
                           "Run a simulation first." );
      return false;
   }

   // The norm path only needs the radial and scan geometry of the data, not
   //  its readings:  every column is simulated onto this grid from scratch.
   US_DataIO::EditedData& edat = dset.run_data;
   edat.runID        = simdata.description.isEmpty() ? QString( "simulation" )
                                                     : simdata.description;
   edat.editID       = "sim";
   edat.dataType     = "RI";
   edat.cell         = QString::number( simdata.cell );
   edat.channel      = QString( QChar( simdata.channel ) );
   edat.description  = simdata.description;
   edat.meniscus     = simparams.meniscus;
   edat.bottom       = simparams.bottom;
   edat.baseline     = 0.0;
   edat.plateau      = 0.0;
   edat.ODlimit      = 1.0e+30;     // A simulation has no OD limit clipping
   edat.floatingData = false;
   edat.xvalues      = simdata.xvalues;
   edat.scanData.resize( nscan );

   for ( int ii = 0; ii < nscan; ii++ )
   {
      US_DataIO::Scan* sscan = &simdata.scanData[ ii ];
      US_DataIO::Scan& escan = edat.scanData[ ii ];

      escan.temperature = sscan->temperature;
      escan.rpm         = sscan->rpm;
      escan.seconds     = sscan->seconds;
      escan.omega2t     = sscan->omega2t;
      escan.wavelength  = sscan->wavelength;
      escan.plateau     = sscan->plateau;
      escan.delta_r     = sscan->delta_r;
      escan.rvalues.fill( 0.0, npoint );
   }

   dset.simparams        = simparams;
   dset.solution_rec.buffer = buffer;
   dset.viscosity        = buffer.viscosity;
   dset.density          = buffer.density;
   dset.compress         = buffer.compressibility;
   dset.manual           = buffer.manual;
   dset.temperature      = simparams.temperature;
   dset.solute_type      = 0;
   dset.tmst_file        = QString();

   // Buffer corrections, from the model's own vbar where it has one
   double vbar20         = ( model.components.size() > 0  &&
                             model.components[ 0 ].vbar20 > 0.0 )
                         ? model.components[ 0 ].vbar20 : TYPICAL_VBAR;

   US_Math2::SolutionData sd;
   sd.density            = buffer.density;
   sd.viscosity          = buffer.viscosity;
   sd.manual             = buffer.manual;
   sd.vbar20             = vbar20;
   sd.vbar               = US_Math2::adjust_vbar20( vbar20,
                                                    simparams.temperature );
   US_Math2::data_correction( simparams.temperature, sd );

   dset.vbar20           = vbar20;
   dset.vbartb           = sd.vbar;
   dset.s20w_correction  = sd.s20w_correction;
   dset.D20w_correction  = sd.D20w_correction;

   return true;
}

// Grid limits spanning a model's own components (class method)
void US_NormGrid::grid_from_model( US_Model& model, GridDef& grid )
{
   int nc         = model.components.size();

   if ( nc < 1 )
      return;

   double slo     = 1.0e+30;
   double sup     = -1.0e+30;
   double klo     = 1.0e+30;
   double kup     = -1.0e+30;

   for ( int ii = 0; ii < nc; ii++ )
   {
      slo            = qMin( slo, model.components[ ii ].s    );
      sup            = qMax( sup, model.components[ ii ].s    );
      klo            = qMin( klo, model.components[ ii ].f_f0 );
      kup            = qMax( kup, model.components[ ii ].f_f0 );
   }

   // Pad out so that the model's own species sit inside the grid rather
   //  than on its edge, where they would have no neighbour to compare with
   double spad    = ( sup > slo ) ? ( ( sup - slo ) * 0.5 )
                                  : qMax( qAbs( sup ) * 0.5, 1.0e-13 );
   double kpad    = ( kup > klo ) ? ( ( kup - klo ) * 0.5 ) : 0.5;

   grid.slo       = qMax( slo - spad, 1.0e-15 );
   grid.sup       = sup + spad;
   grid.klo       = qMax( klo - kpad, 1.0 );
   grid.kup       = kup + kpad;
}

// Start computing a norm grid
bool US_NormGrid::start( US_SolveSim::DataSet* a_dset, const GridDef& a_gdef,
      QWidget* a_parentw, QString& errmsg )
{
   errmsg          = QString();

   if ( busy )
   {
      errmsg          = tr( "A norm grid calculation is already running." );
      return false;
   }

   if ( a_dset == NULL  ||  a_dset->run_data.scanCount() < 1 )
   {
      errmsg          = tr( "There is no experiment data to simulate on." );
      return false;
   }

   dset            = a_dset;
   gdef            = a_gdef;
   specmode        = false;

   int    nss      = qMax( 1, gdef.nss );
   int    nks      = qMax( 1, gdef.nks );
   double slo      = gdef.slo;
   double sup      = gdef.sup;
   double klo      = gdef.klo;
   double kup      = gdef.kup;
   double cff0     = gdef.cff0;
   int    nthrd    = qMax( 1, gdef.nthrd );

   int    nprs     = qMax( 1, ( nss - 1 ) );
   int    nprk     = qMax( 1, ( nks - 1 ) );
   double s_step   = qAbs( sup - slo ) / (double)nprs;
   double ff0_step = qAbs( kup - klo ) / (double)nprk;
   s_step          = ( s_step   > 0.0 ) ? s_step   : ( slo * 1.001 );
   ff0_step        = ( ff0_step > 0.0 ) ? ff0_step : ( klo * 1.001 );
   sup            += (   s_step * 0.001 );
   kup            += ( ff0_step * 0.001 );

   QVector< US_Solute > solutes = US_Solute::create_solutes(
         slo, sup, s_step, klo, kup, ff0_step, cff0 );

   nsolutes        = solutes.size();

   if ( nsolutes < 1 )
   {
      errmsg          = tr( "The given limits produce no grid points.\n"
                            "Adjust the limits and step counts, then retry." );
      return false;
   }

   // Work out the grid shape actually produced.  create_solutes() steps the
   //  Y attribute in the outer loop and X (s) in the inner one, so a run of
   //  entries sharing the first Y value is exactly one grid row.  Deriving
   //  the shape rather than trusting the requested step counts keeps this
   //  correct when floating-point stepping yields a different count.
   bool   varyvbar = ( cff0 > 0.0 );
   double yfirst   = varyvbar ? solutes[ 0 ].v : solutes[ 0 ].k;
   int    gnss     = 0;

   while ( gnss < nsolutes )
   {
      double yval     = varyvbar ? solutes[ gnss ].v : solutes[ gnss ].k;

      if ( yval != yfirst )
         break;

      gnss++;
   }

   int    gnks     = ( gnss > 0 ) ? ( nsolutes / gnss ) : 0;

   if ( gnss < 1  ||  ( gnss * gnks ) != nsolutes )
   {  // Not a clean rectangular grid:  compute norms without the
      //  neighbour-coherence diagnostic rather than mis-pairing points
DbgLv(1) << "NG: irregular grid - coherence disabled" << gnss << gnks
 << nsolutes;
      gnss            = 0;
      gnks            = 0;
   }

DbgLv(1) << "NG: grid gnss gnks" << gnss << gnks << "of" << nsolutes;

   return launch( solutes, gnss, gnks, ( cff0 > 0.0 ), cff0, nthrd,
                  a_parentw, errmsg );
}

// Start computing norms for a model's own species
bool US_NormGrid::start_species( US_SolveSim::DataSet* a_dset,
      US_Model& model, int nthrd, QWidget* a_parentw, QString& errmsg )
{
   errmsg          = QString();

   if ( busy )
   {
      errmsg          = tr( "A norm calculation is already running." );
      return false;
   }

   if ( a_dset == NULL  ||  a_dset->run_data.scanCount() < 1 )
   {
      errmsg          = tr( "There is no data to simulate on." );
      return false;
   }

   int ncomp       = model.components.size();

   if ( ncomp < 1 )
   {
      errmsg          = tr( "The model has no components to simulate." );
      return false;
   }

   model.update_coefficients();

   // One column per model species, each carrying its own vbar.  Laid out as
   //  a single-column grid so that consecutive species are neighbours; the
   //  viewer plots them at their real s and f/f0 positions regardless.
   QVector< US_Solute > solutes;
   solutes.reserve( ncomp );

   for ( int ii = 0; ii < ncomp; ii++ )
   {
      US_Model::SimulationComponent& mc = model.components[ ii ];
      double vbar     = ( mc.vbar20 > 0.0 ) ? mc.vbar20 : a_dset->vbar20;
      solutes << US_Solute( mc.s, mc.f_f0, 0.0, vbar );
   }

   dset            = a_dset;
   gdef            = GridDef();
   gdef.nthrd      = nthrd;
   specmode        = true;
   nsolutes        = ncomp;

   return launch( solutes, 1, ncomp, true, 0.0, nthrd, a_parentw, errmsg );
}

// Common tail of start() and start_species()
bool US_NormGrid::launch( QVector< US_Solute >& solutes, int gnss, int gnks,
      bool varyvbar, double cff0, int nthrd, QWidget* a_parentw,
      QString& errmsg )
{
   errmsg          = QString();
   nsolutes        = solutes.size();
   parentw         = a_parentw;

   // Any existing map is superseded by this pass.  Close it now, and let its
   //  deferred delete run, so that its copy of the column signatures is
   //  released before a new set is allocated below.
   if ( ! analcd.isNull() )
   {
      analcd->close();
      qApp->processEvents();
   }


   // Resolve the speed profile before any worker simulates
   resolve_timestate( dset );

   // Never start more threads than there is work to hand out.  With a grid,
   //  work is handed out as whole rows so that each worker can pair each of
   //  its points with its grid neighbours.
   nthrd           = ( gnks > 0 ) ? qMin( nthrd, gnks )
                                  : qMin( nthrd, nsolutes );
   kthrdr          = nthrd;
   nrm_nss         = gnss;
   nrm_nks         = gnks;
   nrm_coher_x.fill( -1.0, nsolutes );
   nrm_coher_y.fill( -1.0, nsolutes );
   nrm_signorm.fill(  0.0, nsolutes );

   // Size the column signatures.  Retaining one for every grid point is what
   //  lets the viewer measure the coherence between any pair of columns, not
   //  just neighbours, so that it can answer "how far do I have to move
   //  before the fit can tell the difference".  Cap the total at a memory
   //  budget, reducing the sampling only when a grid is large enough to
   //  need it.
   const qint64 sig_budget = 512LL * 1024LL * 1024LL;   // bytes

   nrm_nrad        = 0;
   nrm_nscn        = 0;
   nrm_rstr        = 1;
   nrm_sstr        = 1;
   nrm_nsamp       = 0;
   nrm_sigs.clear();

   if ( gnks > 0 )
   {
      int    nscn     = dset->run_data.scanCount();
      int    npnt     = dset->run_data.pointCount();
      qint64 fcap     = sig_budget / ( 4LL * (qint64)nsolutes );
      int    scap     = (int)qBound( (qint64)128, fcap,
            (qint64)( WorkerThreadCalcNorm::MAX_SIG_RAD *
                      WorkerThreadCalcNorm::MAX_SIG_SCN ) );
      double shrink   = qMin( 1.0, sqrt( (double)scap /
            (double)( WorkerThreadCalcNorm::MAX_SIG_RAD *
                      WorkerThreadCalcNorm::MAX_SIG_SCN ) ) );
      int    maxrad   = qMax( 16,
            (int)( WorkerThreadCalcNorm::MAX_SIG_RAD * shrink ) );
      int    maxscn   = qMax(  8,
            (int)( WorkerThreadCalcNorm::MAX_SIG_SCN * shrink ) );

      nrm_rstr        = qMax( 1, ( npnt + maxrad - 1 ) / maxrad );
      nrm_sstr        = qMax( 1, ( nscn + maxscn - 1 ) / maxscn );
      nrm_nrad        = ( npnt + nrm_rstr - 1 ) / nrm_rstr;
      nrm_nscn        = ( nscn + nrm_sstr - 1 ) / nrm_sstr;
      nrm_nsamp       = nrm_nrad * nrm_nscn;

      nrm_sigs.resize( nsolutes * nrm_nsamp );

DbgLv(1) << "NG: signatures" << nrm_nrad << "x" << nrm_nscn << "=" << nrm_nsamp
 << "for" << nsolutes << "columns," << ( nrm_sigs.size() * 4 / 1048576 ) << "MB";
   }

   model2.components.resize( nsolutes );
   busy            = true;

   int attr_x      = WorkerThreadCalcNorm::ATTR_S;
   int attr_y      = WorkerThreadCalcNorm::ATTR_K;
   int attr_z      = WorkerThreadCalcNorm::ATTR_V;
   int smask       = ( attr_x << 6 ) | ( attr_y << 3 ) | attr_z;

   // Create and start calc-norm worker threads
   for ( int ii = 0; ii < nthrd; ii++ )
   {
      WorkerThreadCalcNorm* wthr = new WorkerThreadCalcNorm( this );
      WorkPacketCN workin;
      workin.thrn     = ii + 1;
      workin.nthrd    = nthrd;
      workin.amask    = smask;
      workin.nsolutes = nsolutes;
      workin.nwsols   = 0;
      workin.cff0     = cff0;
      workin.varyvbar = varyvbar;
      workin.bfgrad   = bfgrad;
      workin.cosedd   = cosedd;
      workin.isolutes = solutes;
      workin.dset     = dset;
      // Contiguous band of grid rows for this worker (zero-length when the
      //  solute set is not a clean grid, which selects the interleaved
      //  fallback partition inside the worker)
      workin.nss      = gnss;
      workin.row0     = ( gnks > 0 ) ? ( ( ii * gnks ) / nthrd ) : 0;
      workin.nrows    = ( gnks > 0 )
                      ? ( ( ( ( ii + 1 ) * gnks ) / nthrd ) - workin.row0 )
                      : 0;
      workin.nsamp    = nrm_nsamp;
      workin.sig_nrad = nrm_nrad;
      workin.sig_nscn = nrm_nscn;
      workin.sig_rstr = nrm_rstr;
      workin.sig_sstr = nrm_sstr;
      // Disjoint slice of the shared signature buffer.  Workers only ever
      //  touch their own rows, so no locking is needed, and nothing may
      //  resize nrm_sigs while they run.
      workin.sigbuf   = ( nrm_nsamp > 0 )
                      ? ( nrm_sigs.data()
                          + ( (size_t)workin.row0 * gnss * nrm_nsamp ) )
                      : NULL;

      wthr->define_work( workin );

      connect( wthr, SIGNAL( work_progress  ( int ) ),
               this, SLOT  ( worker_progress( int ) ) );
      connect( wthr, SIGNAL( work_complete  ( WorkerThreadCalcNorm* ) ),
               this, SLOT  ( worker_complete( WorkerThreadCalcNorm* ) ) );

      wthr->start();
   }

   return true;
}

void US_NormGrid::worker_progress( int kstep )
{
   emit norm_progress( kstep );
}

// Handle the completion of a calc-norm worker thread
void US_NormGrid::worker_complete( WorkerThreadCalcNorm* wthr )
{
   WorkPacketCN  workout;

   wthr->get_result( workout );

   US_Model::SimulationComponent zcomponent;  // Zeroed component to init
   zcomponent.s      = 0.0;
   zcomponent.D      = 0.0;
   zcomponent.mw     = 0.0;
   zcomponent.f      = 0.0;
   zcomponent.f_f0   = 0.0;
   zcomponent.vbar20 = 0.0;
   zcomponent.signal_concentration = 0.0;

   for ( int ii = 0; ii < workout.nwsols; ii++ )
   {
      int kk     = workout.solxs[ ii ];
      model2.components[ kk ]        = zcomponent;
      model2.components[ kk ].s      = workout.csolutes[ ii ].s;
      model2.components[ kk ].f_f0   = workout.csolutes[ ii ].k;
      model2.components[ kk ].vbar20 = workout.csolutes[ ii ].v;
      model2.components[ kk ].signal_concentration = workout.csolutes[ ii ].c;

      if ( ii < workout.coher_x.size() )
      {  // Neighbour coherences, in the same global solute indexing
         nrm_coher_x[ kk ] = workout.coher_x[ ii ];
         nrm_coher_y[ kk ] = workout.coher_y[ ii ];
         nrm_signorm[ kk ] = workout.signorm[ ii ];
      }
   }

   wthr->deleteLater();          // Worker has delivered its results
   kthrdr--;

   if ( kthrdr > 0 )
      return;

   // Close the Y coherence across the worker band boundaries.  Each worker
   //  computed the coherences inside its own band; the one row where a band
   //  meets the next has its +Y neighbour in the other band, and is filled
   //  in here now that every signature is written.
   if ( nrm_nsamp > 0 )
   {
      for ( int iy = 0; ( iy + 1 ) < nrm_nks; iy++ )
      {
         for ( int ix = 0; ix < nrm_nss; ix++ )
         {
            int kk            = iy * nrm_nss + ix;

            if ( nrm_coher_y[ kk ] >= 0.0 )
               continue;                  // Already done by a worker

            const float* siga = nrm_sigs.constData()
                              + ( (size_t)kk * nrm_nsamp );
            const float* sigb = siga + ( (size_t)nrm_nss * nrm_nsamp );
            double dotp       = 0.0;

            for ( int jj = 0; jj < nrm_nsamp; jj++ )
               dotp             += ( (double)siga[ jj ] * (double)sigb[ jj ] );

            nrm_coher_y[ kk ] = qBound( 0.0, dotp, 1.0 );
         }
      }
   }

   // Complete the remaining coefficients only once, when every component
   //  has been filled in
   model2.update_coefficients();

   bool cnst_vbr     = ( gdef.cff0 == 0.0 );

   for ( int ii = 0; ii < model2.components.size(); ii++ )
   {  // For plotting purposes, scale sedimentation coefficients
      model2.components[ ii ].s *= 1.0e+13;
   }

   // Describe the grid for the viewer:  shape, the NNLS column-norm cutoff
   //  in effect, the data point count behind each norm, and the coherences
   US_NormGridInfo ginfo;
   ginfo.nss         = nrm_nss;
   ginfo.nks         = nrm_nks;
   ginfo.norm_cut    = US_SolveSim::norm_cutoff();
   ginfo.ndpts       = dset->run_data.scanCount()
                     * dset->run_data.pointCount();
   ginfo.coher_x     = nrm_coher_x;
   ginfo.coher_y     = nrm_coher_y;
   ginfo.nsamp       = nrm_nsamp;
   ginfo.sig_nrad    = nrm_nrad;
   ginfo.sig_nscn    = nrm_nscn;
   ginfo.signorm     = nrm_signorm;

   ginfo.species     = specmode;

   if ( specmode )
      ginfo.descr       = tr( "%1  (%2 model species)" )
                             .arg( dset->run_data.runID ).arg( nrm_nks );
   else
      ginfo.descr       = tr( "%1  (%2 s x %3 %4 grid points)" )
                             .arg( dset->run_data.runID )
                             .arg( nrm_nss ).arg( nrm_nks )
                             .arg( cnst_vbr ? tr( "f/f0" ) : tr( "vbar" ) );

   // Say which cell configuration and solver the columns were simulated
   //  under, so that the norms are not read as belonging to a different fit
   US_SimulationParameters& sparm = dset->simparams;

   if ( sparm.band_forming )
   {
      US_SolveSim::BandThresholds bthr;
      bool bdthr        = US_SolveSim::bandform_thresholds( sparm, bthr );
      ginfo.descr      += tr( "\nBand-forming, %1 uL load%2" )
                             .arg( sparm.band_volume * 1000.0, 0, 'g', 4 )
                             .arg( bdthr ? tr( ", data thresholds applied" )
                                         : tr( ", no threshold config" ) );
   }

   else
      ginfo.descr      += tr( "\nStandard sector cell" );

   bool usefvm       = ( sparm.meshType == US_SimulationParameters::ASTFVM );

   ginfo.descr      += tr( ",  %1 sim points,  %2" )
                          .arg( sparm.simpoints )
                          .arg( usefvm ? tr( "ASTFVM solver" )
                                       : tr( "ASTFEM solver" ) );

   // A fit over a buffer with co-sedimenting or co-diffusing components
   //  builds a band-forming gradient and a co-sedimenting simulation and
   //  hands them to the solver.  Say plainly when the columns here were
   //  built without them, so the difference is not read as a property of
   //  the grid.
   if ( ! dset->solution_rec.buffer.cosed_component.isEmpty() )
   {
      if ( ! usefvm )
         ginfo.descr      += tr( "\nBuffer has co-sedimenting components,"
                                 " which only the ASTFVM solver applies" );

      else if ( bfgrad == NULL  &&  cosedd == NULL )
         ginfo.descr      += tr( "\nBuffer has co-sedimenting components, but"
                                 " no gradient was supplied:\n"
                                 "  columns simulated in a plain buffer" );

      else
         ginfo.descr      += tr( "\nCo-sedimenting buffer gradient applied" );
   }

   // Radius and elapsed time of the sampled points, so that the viewer can
   //  plot a signature back as a set of simulated scans
   if ( nrm_nsamp > 0 )
   {
      ginfo.sig_radius.resize( nrm_nrad );
      ginfo.sig_time  .resize( nrm_nscn );

      for ( int jj = 0; jj < nrm_nrad; jj++ )
         ginfo.sig_radius[ jj ] = dset->run_data.radius( jj * nrm_rstr );

      for ( int jj = 0; jj < nrm_nscn; jj++ )
         ginfo.sig_time  [ jj ] = dset->run_data.scanData[ jj * nrm_sstr ]
                                  .seconds;
   }

   // Hand the signatures over rather than sharing them, so that no second
   //  reference is held once the viewer owns them
   ginfo.sigs.swap( nrm_sigs );

   analcd  = new US_show_norm( &model2, cnst_vbr, ginfo,
                               parentw.isNull() ? NULL : parentw.data() );
   analcd->setAttribute( Qt::WA_DeleteOnClose );
   analcd->show();

   busy    = false;

   emit norm_finished();
}

