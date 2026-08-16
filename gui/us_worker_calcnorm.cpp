//! \file us_worker_calcnorm.cpp
#include "us_worker_calcnorm.h"
#include "us_util.h"
#include "us_settings.h"
#include "us_astfem_math.h"
#include "us_model.h"
#include "us_sleep.h"
#include "us_constants.h"
#include "us_memory.h"


// construct worker thread to calculate norms
WorkerThreadCalcNorm::WorkerThreadCalcNorm( QObject* parent )
   : QThread( parent )
{
   dset       = NULL;
   thrn       = -1;
   attr_x     = 0   ;
   attr_y     = 1;
   attr_z     = 3;
   amask      = 0;
   cff0       = 0.0;
   varyvbar   = false;
   bfgrad     = NULL;
   cosedd     = NULL;
   nss        = 0;
   row0       = 0;
   nrows      = 0;
   nsamp      = 0;
   sig_nrad   = 0;
   sig_nscn   = 0;
   sig_rstr   = 1;
   sig_sstr   = 1;
   sigbuf     = NULL;
   dbg_level  = US_Settings::us_debug();
DbgLv(1) << "CN(WT): Thread created";
}

// worker thread destructor
WorkerThreadCalcNorm::~WorkerThreadCalcNorm()
{
DbgLv(1) << "CN(WT):   Thread destroy - (1)finished?" << isFinished() << thrn;
   if ( ! wait( 2000 ) )
   {
      qDebug() << "Thread destroy wait timeout(2secs) : Thread" << thrn;
   }
DbgLv(1) << "CN(WT):   Thread destroy - (2)finished?" << isFinished() << thrn;
DbgLv(1) << "CN(WT):    Thread destroyed" << thrn;
}

// define work for a worker thread
void WorkerThreadCalcNorm::define_work( WorkPacketCN& workin )
{
   thrn        = workin.thrn;           // thread number
   nthrd       = workin.nthrd;          // total threads count
   dset        = workin.dset;           // dataset pointer
   amask       = workin.amask;          // xyz attribute mask
   cff0        = workin.cff0;           // constant f/f0 value
   varyvbar    = workin.varyvbar;       // vbar comes from each solute
   bfgrad      = workin.bfgrad;         // band-forming gradient (or null)
   cosedd      = workin.cosedd;         // co-sedimenting data (or null)
   nss         = workin.nss;            // grid row length (0 if no grid)
   row0        = workin.row0;           // first grid row for this worker
   nrows       = workin.nrows;          // grid rows for this worker
   nsamp       = workin.nsamp;          // signature geometry, set by caller
   sig_nrad    = workin.sig_nrad;
   sig_nscn    = workin.sig_nscn;
   sig_rstr    = workin.sig_rstr;
   sig_sstr    = workin.sig_sstr;
   sigbuf      = workin.sigbuf;         // caller's buffer slice
   attr_x      = ( amask >> 6 );        // attribute indecies
   attr_y      = ( amask >> 3 ) & 7;
   attr_z      = amask & 7;
DbgLv(1) << "define_work_threadno." << thrn << " of" << nthrd<< attr_x << attr_y << attr_z ;
   solutes_i   = workin.isolutes;       // full solute points vector
   nsolutes    = solutes_i.count();     // total solute points
}

// get results of a completed worker thread
void WorkerThreadCalcNorm::get_result( WorkPacketCN& workout )
{
   workout.thrn     = thrn;
   workout.nthrd    = nthrd;
   workout.dset     = dset;
   workout.amask    = amask;
   workout.csolutes = solutes_c;
   workout.solxs    = solxs;
   workout.nsolutes = nsolutes;
   workout.nwsols   = nwsols;
   workout.nss      = nss;
   workout.row0     = row0;
   workout.nrows    = nrows;
   workout.nsamp    = nsamp;
   workout.coher_x  = coher_x;
   workout.coher_y  = coher_y;
   workout.signorm  = signorm;
DbgLv(1) << "get_result" << workout.csolutes.size() << workout.nthrd ;
}

// run the worker thread
void WorkerThreadCalcNorm::run()
{
   DbgLv(1) << "CN(WT):  run: calc_norms:";
   calc_norms();                  // do all the work here
   quit();
   exec();
}

// Build a unit-length subsampled signature of a simulation.
//
// The signature is used only for the neighbour-coherence diagnostic, never
// for the reported norm.  Subsampling keeps a full grid row of signatures
// small enough to hold in memory; the simulated boundaries are smooth, so
// the sampled inner product tracks the full one closely.
double WorkerThreadCalcNorm::signature( US_DataIO::RawData& simdat, float* sig )
{
   double sumsq   = 0.0;
   int    kk      = 0;

   for ( int ii = 0; ii < sig_nscn; ii++ )
   {
      int jscn    = ii * sig_sstr;

      for ( int jj = 0; jj < sig_nrad; jj++ )
      {
         double dval = simdat.value( jscn, jj * sig_rstr );
         sig[ kk++ ] = (float)dval;
         sumsq      += ( dval * dval );
      }
   }

   // Normalize to unit length so that a dot product of two signatures is
   //  directly the cosine of the angle between the two A columns.  The norm
   //  is returned so the caller can undo this and recover readings in OD.
   double signrm  = ( sumsq > 0.0 ) ? sqrt( sumsq ) : 0.0;
   double scale   = ( signrm > 0.0 ) ? ( 1.0 / signrm ) : 0.0;

   for ( int ii = 0; ii < nsamp; ii++ )
      sig[ ii ]   = (float)( sig[ ii ] * scale );

   return signrm;
}

// Dot product of two unit-length signatures
double WorkerThreadCalcNorm::coherence( const float* siga, const float* sigb )
{
   double dotp    = 0.0;

   for ( int ii = 0; ii < nsamp; ii++ )
      dotp       += ( (double)siga[ ii ] * (double)sigb[ ii ] );

   return qBound( 0.0, dotp, 1.0 );
}

// Do the real work of a thread:  norm values for each of its solutes
void WorkerThreadCalcNorm::calc_norms()
{
DbgLv(1) << "calc_norms is called" << nsolutes << nthrd ;

   // A grid description and a signature buffer mean this worker owns a
   //  contiguous band of grid rows and can record signatures and neighbour
   //  coherences.  Without them, fall back to the interleaved share of
   //  solute points and report norms only.
   bool ongrid    = ( nss > 0  &&  nrows > 0  &&  nsamp > 0  &&
                      sigbuf != NULL );

   if ( ongrid )
   {
      int ilo     = row0 * nss;
      int ihi     = qMin( nsolutes, ( row0 + nrows ) * nss );

      for ( int ii = ilo; ii < ihi; ii++ )
         solxs << ii;
   }

   else
   {
      for ( int ii = ( thrn - 1 ); ii < nsolutes; ii += nthrd )
         solxs << ii;
   }

   nwsols         = solxs.count();         // count of solutes for worker

   if ( nwsols < 1 )
   {  // No solute points fell to this worker:  nothing to simulate
DbgLv(1) << "CN(WT):  CN:  no solutes for thread" << thrn;
      solutes_c.clear();
      emit work_complete( this );
      return;
   }

DbgLv(1) << "nwsols_" << nwsols << "solx0" << solxs[0]
 << "solxn" << solxs[nwsols-1] << "ongrid" << ongrid << "row0" << row0
 << "nrows" << nrows;

   solutes_c.resize( nwsols );             // computed solute points

   for ( int ii = 0; ii < nwsols; ii++ )
   {  // computed solutes initialized from full input list
      solutes_c[ ii ]    = solutes_i[ solxs[ ii ] ];
      solutes_c[ ii ].c  = 0.0;
   }
DbgLv(1) << "CN(WT):  CN:  sol_c0.s" << solutes_c[0].s;

   simparms       = dset->simparams;       // local simulation parameters

   // The run selects its Lamm-equation solver through the mesh type.  The
   //  finite volume solver takes the model in 20W space and handles the
   //  buffer corrections internally, so the two paths differ in more than
   //  which solver object is constructed.
   bool usefvm    = ( simparms.meshType == US_SimulationParameters::ASTFVM );

   US_DataIO::RawData simdat;              // simulation data set
   US_Model           model1;              // 1-component work model
   model1.components.resize( 1 );

   // A band-forming run has its data held within thresholds before the A
   //  matrix is built, and columns whose thresholded simulation is entirely
   //  zero are dropped from the fit.  Reproduce both here, so that a norm
   //  grid describes the fit that would actually be run.
   US_SolveSim::BandThresholds bthr;
   bool banddthr  = US_SolveSim::bandform_thresholds( simparms, bthr );
   US_DataIO::EditedData wdata;

   if ( banddthr )
   {  // Threshold the experiment data, as the fit does, so that the
      //  simulation is laid out on the same radial and scan grid
      wdata          = dset->run_data;
      US_SolveSim::data_threshold( &wdata, bthr.zerothr, bthr.linethr,
                                   bthr.maxod, bthr.mfactex );
   }

DbgLv(1) << "CN(WT):  CN:  banddthr" << banddthr << "bandvol"
 << simparms.band_volume << "mfactor" << bthr.mfactor;

   // Initialize the simulation data set on the experiment's grid
   US_DataIO::EditedData* bdata = banddthr ? &wdata : &dset->run_data;

   US_AstfemMath::initSimData( simdat, *bdata, 0.0 );

   int nscan         = simdat.scanCount();
   int npoint        = simdat.pointCount();
DbgLv(1) << "CN(WT):  CN:  nscan" << nscan << "npoint" << npoint;

   // Where the experiment reaches its OD limit, the fit substitutes zero in
   //  the B vector and zeroes the same position in every A column, so those
   //  readings contribute to no column norm.  As in calc_residuals, this
   //  engages only when the limit is actually reached somewhere, and the
   //  positions excluded are those the B vector leaves at zero.
   QVector< char > odzero;
   int    kodl       = 0;
   double odlim      = bdata->ODlimit;

   for ( int jj = 0; jj < nscan; jj++ )
      for ( int kk = 0; kk < npoint; kk++ )
         if ( bdata->value( jj, kk ) >= odlim )
            kodl++;

   if ( kodl > 0 )
   {
      odzero.fill( 0, nscan * npoint );

      for ( int jj = 0; jj < nscan; jj++ )
      {
         for ( int kk = 0; kk < npoint; kk++ )
         {
            double evalue     = bdata->value( jj, kk );
            evalue            = ( evalue >= odlim ) ? 0.0 : evalue;
            odzero[ jj * npoint + kk ] = ( evalue == 0.0 ) ? 1 : 0;
         }
      }
   }

DbgLv(1) << "CN(WT):  CN:  ODlimit" << odlim << "exceeded at" << kodl;

   if ( ongrid )
   {  // The signature geometry is fixed by the caller, which sized the
      //  shared buffer from it
      coher_x  .fill( -1.0, nwsols );
      coher_y  .fill( -1.0, nwsols );
      signorm  .fill(  0.0, nwsols );
DbgLv(1) << "CN(WT):  CN:  sig nrad nscn" << sig_nrad << sig_nscn
 << "rstr sstr" << sig_rstr << sig_sstr << "nsamp" << nsamp;
   }

   // Zeroed model component for initialization
   US_Model::SimulationComponent zcomponent;
   zcomponent.s      = 0.0;
   zcomponent.D      = 0.0;
   zcomponent.mw     = 0.0;
   zcomponent.f      = 0.0;
   zcomponent.f_f0   = cff0;
   zcomponent.vbar20 = dset->vbar20;

   // Do the work of finite element modeling, norm value from simulation
   for ( int ii = 0; ii < nwsols; ii++ )
   {
      // Initialize component, then set X,Y from the solute point.
      //
      // The Z attribute is only taken from the solute when it is what the
      // grid varies, i.e. the constant-f/f0 case.  With a constant vbar the
      // solutes carry no vbar of their own, so applying Z would set vbar to
      // zero and US_Model::calc_coefficients would silently substitute
      // TYPICAL_VBAR for it, giving every column a different buoyancy from
      // the one the fit uses.  calc_residuals sets only X and Y for the
      // same reason, taking vbar from the data set.
      model1.components[ 0 ]    = zcomponent;
      set_comp_attr( model1.components[ 0 ], solutes_c[ ii ], attr_x );
      set_comp_attr( model1.components[ 0 ], solutes_c[ ii ], attr_y );

      if ( varyvbar )
         set_comp_attr( model1.components[ 0 ], solutes_c[ ii ], attr_z );

      // Compute the other coefficients
      model1.update_coefficients();

      // Convert to experiment space.  The finite volume solver takes the
      //  model in 20W space and applies the buffer corrections itself, so
      //  correcting beforehand would apply them twice.  When vbar varies
      //  over the grid the corrections depend on each column's own vbar, so
      //  they must be recomputed per column as calc_residuals does; with
      //  vbar fixed the data set's own corrections apply to every column.
      double scorr      = usefvm ? 1.0 : dset->s20w_correction;
      double dcorr      = usefvm ? 1.0 : dset->D20w_correction;

      if ( varyvbar  &&  ! usefvm )
      {
         US_Math2::SolutionData sd;
         sd.viscosity      = dset->viscosity;
         sd.density        = dset->density;
         sd.manual         = dset->manual;
         sd.vbar20         = model1.components[ 0 ].vbar20;
         sd.vbar           = US_Math2::adjust_vbar20( sd.vbar20,
                                                      dset->temperature );
         US_Math2::data_correction( dset->temperature, sd );
         scorr             = sd.s20w_correction;
         dcorr             = sd.D20w_correction;
      }

      model1.components[ 0 ].s /= scorr;
      model1.components[ 0 ].D /= dcorr;

      // Reinitialize the simulation data set initial concentrations
      for ( int jj = 0; jj < nscan; jj++ )
         for ( int kk = 0; kk < npoint; kk++ )
            simdat.setValue( jj, kk, 0.0 );

DbgLv(1) << "CN(WT):  CN:   ii" << ii << "usefvm" << usefvm;
      // Solve the Lamm equation with whichever solver the run selects
      if ( usefvm )
      {
         US_LammAstfvm astfvm( model1, simparms );
         astfvm.set_buffer( dset->solution_rec.buffer, bfgrad, cosedd );
         astfvm.calculate( simdat );
      }

      else
      {
         US_Astfem_RSA astfem_rsa( model1, simparms );
         astfem_rsa.calculate( simdat );
      }

      if ( banddthr )
      {  // Hold the simulation within the band-forming thresholds.  A
         //  column left entirely zero is one the fit would not include at
         //  all, which a zero norm reports faithfully:  it falls below any
         //  cutoff and is marked as dropped.
         US_SolveSim::data_threshold( &simdat, bthr.zerothr, bthr.linethr,
                                      bthr.maxod, bthr.mfactor,
                                      bthr.minnzsc );
      }

      if ( kodl > 0 )
      {  // Zero the positions the fit excludes from every A column
         for ( int jj = 0; jj < nscan; jj++ )
            for ( int kk = 0; kk < npoint; kk++ )
               if ( odzero[ jj * npoint + kk ] )
                  simdat.setValue( jj, kk, 0.0 );
      }

      // Store the norm value for this simulation (A matrix column)
      double znorm      = US_Math2::norm_value( &simdat );
      solutes_c[ ii ].c = znorm;

      if ( ongrid )
      {  // Record the signature, then take the coherences with the two
         //  neighbours that have already been computed.  Both live in this
         //  worker's own slice, since it walks a contiguous band of rows
         //  with X varying fastest.
         int    isx        = ii % nss;         // X index within the row
         float* sigc       = sigbuf + ( (size_t)ii * nsamp );

         signorm[ ii ]     = signature( simdat, sigc );

         if ( isx > 0 )
         {  // Coherence with the previous point in the same row is
            //  reported on that previous point (its +X neighbour)
            coher_x[ ii - 1 ]   = coherence( sigc - nsamp, sigc );
         }

         if ( ii >= nss )
         {  // Coherence with the point below in the previous row is
            //  reported on that lower point (its +Y neighbour)
            coher_y[ ii - nss ] = coherence( sigc - ( (size_t)nss * nsamp ),
                                             sigc );
         }
      }

      // Signal a completed step (solute point)
      emit work_progress( 1 );
   }

   // Signal that a thread's work is done
   emit work_complete( this);
}

// Set a model component coefficient from a solute attribute
void WorkerThreadCalcNorm::set_comp_attr( US_Model::SimulationComponent& component,
      US_Solute& solute, int attr_type )
{
   switch ( attr_type )
   {
      default:
      case ATTR_S:          // Sedimentation Coefficient
         component.s      = solute.s;
         break;
      case ATTR_K:          // Frictional Ratio
         component.f_f0   = solute.k;
         break;
      case ATTR_W:          // Molecular Weight
         component.mw     = solute.d;
         break;
      case ATTR_V:          // Partial Specific Volume (vbar)
         component.vbar20 = solute.v;
         break;
      case ATTR_D:          // Diffusion Coefficient
         component.D      = solute.d;
         break;
      case ATTR_F:          // Frictional Coefficient
         component.f      = solute.d;
         break;
   }
}

