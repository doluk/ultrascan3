//! \file us_sim_cache_2d.cpp
#include "us_sim_cache_2d.h"

// Create a cache with a memory cap in megabytes
US_SimCache2D::US_SimCache2D( qint64 max_mb )
   : max_bytes( qMax( (qint64)0, max_mb ) * 1024 * 1024 ), nbytes( 0 ),
     nhits( 0 ), nmiss( 0 ), nstored( 0 ), nfull( 0 )
{
}

// Key:  the component values and parameters that determine a simulation
QByteArray US_SimCache2D::make_key( const US_Model::SimulationComponent& comp,
                                    const US_SimulationParameters& sparms,
                                    const US_DataIO::RawData& simdat )
{
   // The simulator uses the given bottom only when it exceeds the bottom
   // position; otherwise it computes it from bottom position and rotor
   // stretch (so, e.g., 0 and the bottom position give the same result).
   double bottom  = ( sparms.bottom > sparms.bottom_position )
                    ? sparms.bottom : -1.0;
   double vals[ 15 ];
   vals[  0 ] = comp.s;
   vals[  1 ] = comp.D;
   vals[  2 ] = comp.vbar20;
   vals[  3 ] = comp.f_f0;
   vals[  4 ] = comp.mw;
   vals[  5 ] = comp.signal_concentration;
   vals[  6 ] = comp.sigma;
   vals[  7 ] = comp.delta;
   vals[  8 ] = sparms.meniscus;
   vals[  9 ] = bottom;
   vals[ 10 ] = sparms.bottom_position;
   vals[ 11 ] = sparms.rotorcoeffs[ 0 ];
   vals[ 12 ] = sparms.rotorcoeffs[ 1 ];
   vals[ 13 ] = (double)simdat.scanData.size();
   vals[ 14 ] = (double)simdat.xvalues.size();
   return QByteArray( (const char*)vals, sizeof( vals ) );
}

// Fill a simulation from the cache if present
bool US_SimCache2D::fetch( const US_Model::SimulationComponent& comp,
                           const US_SimulationParameters& sparms,
                           US_DataIO::RawData& simdat )
{
   QByteArray key = make_key( comp, sparms, simdat );
   QReadLocker rlock( &lock );
   auto it        = sims.constFind( key );

   if ( it == sims.constEnd() )
   {
      nmiss++;
      return false;
   }

   const float* vv = it.value().constData();
   int nscans     = simdat.scanCount();
   int npoints    = simdat.pointCount();

   for ( int ss = 0; ss < nscans; ss++ )
   {
      double* rv     = simdat.scanData[ ss ].rvalues.data();

      for ( int rr = 0; rr < npoints; rr++ )
         rv[ rr ]      = (double)*vv++;
   }

   nhits++;
   return true;
}

// Store a simulation if not yet present and there is room
void US_SimCache2D::store( const US_Model::SimulationComponent& comp,
                           const US_SimulationParameters& sparms,
                           const US_DataIO::RawData& simdat )
{
   int nscans     = simdat.scanData.size();
   int npoints    = simdat.xvalues.size();
   qint64 size    = (qint64)nscans * npoints * (qint64)sizeof( float )
                    + 128;          // Approximate hash entry overhead
   QByteArray key = make_key( comp, sparms, simdat );
   QWriteLocker wlock( &lock );

   if ( sims.contains( key ) )
      return;

   if ( nbytes + size > max_bytes )
   {  // Cache full:  keep what is there
      nfull++;
      return;
   }

   QVector< float > vals( nscans * npoints );
   float* vv      = vals.data();

   for ( int ss = 0; ss < nscans; ss++ )
   {
      const double* rv = simdat.scanData[ ss ].rvalues.constData();

      for ( int rr = 0; rr < npoints; rr++ )
         *vv++         = (float)rv[ rr ];
   }

   sims.insert( key, vals );
   nbytes        += size;
   nstored++;
}

// Text of the cache statistics
QString US_SimCache2D::stats()
{
   QReadLocker rlock( &lock );
   qint64 nh      = nhits;
   qint64 nm      = nmiss;
   double hitpc   = ( nh + nm ) > 0 ? ( 100.0 * nh / (double)( nh + nm ) ) : 0.0;

   return QObject::tr( "Simulation cache:  %1 reused, %2 computed (%3% reused);"
                       " %4 stored in %5 of %6 MB, %7 not stored (cache full)" )
      .arg( nh ).arg( nm ).arg( hitpc, 0, 'f', 1 ).arg( nstored )
      .arg( nbytes / ( 1024.0 * 1024.0 ), 0, 'f', 1 )
      .arg( max_bytes / ( 1024 * 1024 ) ).arg( nfull );
}
