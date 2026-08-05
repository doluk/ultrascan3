//! \file us_grid_control.cpp

#include "us_grid_control.h"
#include "us_constants.h"
#include "us_settings.h"

#include <cmath>

US_GridControl::Config::Config()
{
   enabled        = true;
   courant        = 0.5;
   ring_factor    = 1.0 / ( 4.0 * M_PI * M_PI );
   feature_points = 8;
   max_reduction  = 32.0;
   max_refinement = 64.0;
   growth         = 1.05;
}

US_GridControl::Config US_GridControl::config( void )
{
   Config cfg;

   if ( US_Settings::debug_match( "NO_ADAPTIVE_GRID" ) )
   {
      cfg.enabled = false;
   }

   if ( US_Settings::debug_match( "COARSE_ADAPTIVE_GRID" ) )
   {
      // Relax the resolution targets: useful to tell a physically narrow
      // feature apart from one that is merely under-resolved.
      cfg.courant        *= 2.0;
      cfg.ring_factor    *= 4.0;
      cfg.feature_points  = qMax( 2, cfg.feature_points / 2 );
      cfg.max_reduction  *= 0.5;
      cfg.max_refinement *= 0.5;
   }

   return cfg;
}

double US_GridControl::magnitude( const double* u, const int nv, const int stride )
{
   if ( u == nullptr  ||  nv < 1  ||  stride < 1 )
   {
      return 0.0;
   }

   double umin = u[ 0 ];
   double umax = u[ 0 ];

   for ( int jj = 1; jj < nv; jj++ )
   {
      const double uval = u[ jj * stride ];
      umin              = qMin( umin, uval );
      umax              = qMax( umax, uval );
   }

   return umax - umin;
}

double US_GridControl::front_scale( const double* x, const int nv,
                                    const double* u, const int stride )
{
   if ( x == nullptr  ||  u == nullptr  ||  nv < 2  ||  stride < 1 )
   {
      return qInf();
   }

   const double range = magnitude( u, nv, stride );

   if ( range <= 0.0 )
   {
      return qInf();                     // constant profile: nothing to resolve
   }

   double maxslope = 0.0;

   for ( int jj = 1; jj < nv; jj++ )
   {
      const double dx = x[ jj ] - x[ jj - 1 ];

      if ( dx > 0.0 )
      {
         const double du = u[ jj * stride ] - u[ ( jj - 1 ) * stride ];
         maxslope        = qMax( maxslope, qAbs( du ) / dx );
      }
   }

   if ( maxslope <= 0.0 )
   {
      return qInf();
   }

   return range / maxslope;
}

double US_GridControl::min_spacing( const double* x, const int nv )
{
   if ( x == nullptr  ||  nv < 2 )
   {
      return 0.0;
   }

   double hmin = x[ 1 ] - x[ 0 ];

   for ( int jj = 2; jj < nv; jj++ )
   {
      hmin = qMin( hmin, x[ jj ] - x[ jj - 1 ] );
   }

   return qMax( hmin, 0.0 );
}

double US_GridControl::step_for_feature( const double dt_char, const double feature,
                                         const double velocity,
                                         const double diffusion,
                                         const Config& cfg )
{
   if ( !cfg.enabled  ||  dt_char <= 0.0 )
   {
      return dt_char;
   }

   const double dt_low = dt_char / qMax( 1.0, cfg.max_reduction );

   if ( !std::isfinite( feature ) )
   {  // Nothing steep anywhere - the characteristic step is what was asked for
      return dt_char;
   }

   if ( feature <= 0.0 )
   {  // A genuine discontinuity has no width to resolve, so no step is small
      // enough. Spend the whole budget and no more.
      return dt_low;
   }

   double dt = dt_char;

   if ( velocity > 0.0 )
   {  // Do not carry a feature across a large part of its own width per step.
      // Only the magnitude enters, so floating species behave like sedimenting
      // ones and mixtures of both are covered by the faster of them.
      dt = qMin( dt, cfg.courant * feature / velocity );
   }

   if ( diffusion > 0.0 )
   {  // Keep the Crank-Nicolson amplification factor positive at the
      // wavenumber the feature occupies: beyond this bound that scale is
      // inverted from step to step instead of damped, which is the ringing.
      dt = qMin( dt, cfg.ring_factor * feature * feature / diffusion );
   }

   return qMax( dt, dt_low );
}

double US_GridControl::resolvable_feature( const double dt, const double velocity,
                                           const double diffusion, const Config& cfg )
{
   if ( !cfg.enabled  ||  dt <= 0.0 )
   {
      return 0.0;
   }

   double width = 0.0;

   if ( velocity > 0.0  &&  cfg.courant > 0.0 )
   {  // Inverse of the transport bound
      width = qMax( width, velocity * dt / cfg.courant );
   }

   if ( diffusion > 0.0  &&  cfg.ring_factor > 0.0 )
   {  // Inverse of the diffusive bound
      width = qMax( width, sqrt( diffusion * dt / cfg.ring_factor ) );
   }

   return width;
}

double US_GridControl::min_cell( const double dt, const double velocity,
                                 const double diffusion, const Config& cfg )
{
   const double width = resolvable_feature( dt, velocity, diffusion, cfg );

   if ( width <= 0.0 )
   {
      return 0.0;
   }

   return width / qMax( 1, cfg.feature_points );
}

double US_GridControl::relax( const double dt_prev, const double dt_target,
                              const Config& cfg )
{
   if ( !cfg.enabled  ||  dt_prev <= 0.0 )
   {
      return dt_target;
   }

   if ( dt_target <= dt_prev )
   {  // A feature just sharpened - follow it immediately
      return dt_target;
   }

   return qMin( dt_target, dt_prev * qMax( 1.0, cfg.growth ) );
}

double US_GridControl::front_velocity( const double s, const double omega2,
                                       const double radius )
{
   return qAbs( s ) * qAbs( omega2 ) * qAbs( radius );
}

int US_GridControl::steep_regions( const double* x, const int nv, const double* u,
                                   const int stride, const double h, const double pad,
                                   const Config& cfg,
                                   QVector< double >& lo, QVector< double >& hi )
{
   lo.clear();
   hi.clear();

   if ( !cfg.enabled  ||  x == nullptr  ||  u == nullptr  ||
        nv < 2  ||  stride < 1  ||  h <= 0.0 )
   {
      return 0;
   }

   const double range = magnitude( u, nv, stride );

   if ( range <= 0.0 )
   {
      return 0;
   }

   const double npts = qMax( 1, cfg.feature_points );

   for ( int jj = 1; jj < nv; jj++ )
   {
      const double dx = x[ jj ] - x[ jj - 1 ];

      if ( dx <= 0.0 )
      {
         continue;
      }

      const double du = qAbs( u[ jj * stride ] - u[ ( jj - 1 ) * stride ] );

      if ( du <= 0.0 )
      {
         continue;
      }

      // Length scale of this interval, and the spacing it would need
      const double scale = range / ( du / dx );

      if ( ( scale / npts ) >= dx )
      {
         continue;                       // already fine enough here
      }

      const double rlo = qMax( x[ jj - 1 ] - pad, x[ 0 ] );
      const double rhi = qMin( x[ jj ]     + pad, x[ nv - 1 ] );

      if ( !lo.isEmpty()  &&  rlo <= hi.last() )
      {  // Overlaps the region built so far - extend it
         hi.last() = qMax( hi.last(), rhi );
      }
      else
      {
         lo .append( rlo );
         hi .append( rhi );
      }
   }

   return lo.size();
}

int US_GridControl::points_for_span( const double span, const double h )
{
   if ( span <= 0.0  ||  h <= 0.0 )
   {
      return 2;
   }

   return qMax( 2, static_cast< int >( ceil( span / h ) ) + 1 );
}

double US_GridControl::lamella_width( const double meniscus,
                                      const double band_volume,
                                      const double path_length,
                                      const double angle )
{
   if ( meniscus <= 0.0  ||  band_volume <= 0.0 )
   {
      return 0.0;
   }

   const double plen = ( path_length != 0.0 ) ? path_length : 1.2;
   const double angl = ( angle       != 0.0 ) ? angle       : 2.5;

   if ( plen <= 0.0  ||  angl <= 0.0 )
   {
      return 0.0;
   }

   // The lamella fills the sector between the meniscus and its outer edge:
   //    V = ( angle / 360 ) * pi * ( r^2 - m^2 ) * path_length
   const double base = meniscus * meniscus
                     + band_volume * 360.0 / ( angl * plen * M_PI );

   return sqrt( base ) - meniscus;
}
