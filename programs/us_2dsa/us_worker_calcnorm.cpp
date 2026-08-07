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
   nss        = 0;
   row0       = 0;
   nrows      = 0;
   nsamp      = 0;
   sig_nrad   = 0;
   sig_nscn   = 0;
   sig_rstr   = 1;
   sig_sstr   = 1;
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
   nss         = workin.nss;            // grid row length (0 if no grid)
   row0        = workin.row0;           // first grid row for this worker
   nrows       = workin.nrows;          // grid rows for this worker
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
   workout.sig_first = sig_first;
   workout.sig_last  = sig_last;
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
void WorkerThreadCalcNorm::signature( US_DataIO::RawData& simdat, float* sig )
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
   //  directly the cosine of the angle between the two A columns.
   double scale   = ( sumsq > 0.0 ) ? ( 1.0 / sqrt( sumsq ) ) : 0.0;

   for ( int ii = 0; ii < nsamp; ii++ )
      sig[ ii ]   = (float)( sig[ ii ] * scale );
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

   // A grid description means this worker owns a contiguous band of grid
   //  rows and can report neighbour coherences.  Without one, fall back to
   //  the interleaved share of solute points and report norms only.
   bool ongrid    = ( nss > 0  &&  nrows > 0 );

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

   US_DataIO::RawData simdat;              // simulation data set
   US_Model           model1;              // 1-component work model
   model1.components.resize( 1 );

   // Initialize the simulation data set
   US_AstfemMath::initSimData( simdat, dset->run_data, 0.0 );

   int nscan         = simdat.scanCount();
   int npoint        = simdat.pointCount();
DbgLv(1) << "CN(WT):  CN:  nscan" << nscan << "npoint" << npoint;

   // Signature geometry:  subsample to a bounded number of values
   sig_rstr          = qMax( 1, ( npoint + MAX_SIG_RAD - 1 ) / MAX_SIG_RAD );
   sig_sstr          = qMax( 1, ( nscan  + MAX_SIG_SCN - 1 ) / MAX_SIG_SCN );
   sig_nrad          = ( npoint + sig_rstr - 1 ) / sig_rstr;
   sig_nscn          = ( nscan  + sig_sstr - 1 ) / sig_sstr;
   nsamp             = sig_nrad * sig_nscn;

   // Rolling row buffers used for the coherence calculation.  Only three
   //  grid rows of subsampled signatures are ever resident.
   QVector< float > sig_prev;      // signatures of the previous grid row
   QVector< float > sig_curr;      // signatures of the row being filled

   if ( ongrid )
   {
      sig_prev .resize( nss * nsamp );
      sig_curr .resize( nss * nsamp );
      sig_first.resize( nss * nsamp );
      coher_x  .fill( -1.0, nwsols );
      coher_y  .fill( -1.0, nwsols );
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
      // Initialize component, then set X,Y,Z from solute point
      model1.components[ 0 ]    = zcomponent;
      set_comp_attr( model1.components[ 0 ], solutes_c[ ii ], attr_x );
      set_comp_attr( model1.components[ 0 ], solutes_c[ ii ], attr_y );
      set_comp_attr( model1.components[ 0 ], solutes_c[ ii ], attr_z );

      // Compute the other coefficients
      model1.update_coefficients();

      // Convert to 20w space
      model1.components[ 0 ].s /= dset->s20w_correction;
      model1.components[ 0 ].D /= dset->D20w_correction;

      // Reinitialize the simulation data set initial concentrations
      for ( int jj = 0; jj < nscan; jj++ )
         for ( int kk = 0; kk < npoint; kk++ )
            simdat.setValue( jj, kk, 0.0 );

DbgLv(1) << "CN(WT):  CN:   ii" << ii << "astfem_rsa:";
      // Perform finite element modeling to compute the simulation
      US_Astfem_RSA astfem_rsa( model1, simparms );

      astfem_rsa.calculate( simdat );

      // Store the norm value for this simulation (A matrix column)
      double znorm      = US_Math2::norm_value( &simdat );
      solutes_c[ ii ].c = znorm;

      if ( ongrid )
      {  // Accumulate coherences against the already-computed neighbours
         int    isx        = ii % nss;         // X index within the row
         int    irx        = ii / nss;         // row index within the band
         float* sigc       = sig_curr.data() + ( isx * nsamp );

         signature( simdat, sigc );

         if ( isx > 0 )
         {  // Coherence with the previous point in the same row is
            //  reported on that previous point (its +X neighbour)
            coher_x[ ii - 1 ] = coherence( sigc - nsamp, sigc );
         }

         if ( irx > 0 )
         {  // Coherence with the point below in the previous row is
            //  reported on that lower point (its +Y neighbour)
            coher_y[ ii - nss ] = coherence( sig_prev.data() + ( isx * nsamp ),
                                             sigc );
         }

         if ( isx == ( nss - 1 ) )
         {  // Row complete:  keep a copy of the first row for the caller,
            //  then roll the current row into the previous-row slot.  The
            //  swapped-in buffer is stale but every entry of it is rewritten
            //  before it is read again.
            if ( irx == 0 )
               sig_first    = sig_curr;

            sig_prev.swap( sig_curr );
         }
      }

      // Signal a completed step (solute point)
      emit work_progress( 1 );
   }

   if ( ongrid )
   {  // The last fully-filled row closes the Y coherence at the far edge
      //  of this worker's band, once the caller pairs it with the first
      //  row of the next band.
      sig_last          = sig_prev;
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

