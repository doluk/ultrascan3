//! \file us_grid_control.cpp

#include "us_grid_control.h"
#include "us_constants.h"
#include "us_settings.h"

#include <cmath>

US_GridControl::Config::Config()
{
   enabled        = true;
   courant        = 0.5;
   diff_number    = 0.5;
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
      // Half the resolution targets: useful to tell a physically narrow
      // feature apart from one that is merely under-resolved.
      cfg.courant        *= 2.0;
      cfg.diff_number    *= 2.0;
      cfg.feature_points  = qMax( 2, cfg.feature_points / 2 );
      cfg.max_reduction  *= 0.5;
      cfg.max_refinement *= 0.5;
   }

   return cfg;
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

double US_GridControl::front_scale( const double* x, const int nv,
                                    const double* u, const int stride )
{
   if ( x == nullptr  ||  u == nullptr  ||  nv < 2  ||  stride < 1 )
   {
      return qInf();
   }

   double umin     = u[ 0 ];
   double umax     = u[ 0 ];
   double maxslope = 0.0;

   for ( int jj = 1; jj < nv; jj++ )
   {
      const double uval = u[ jj * stride ];
      umin              = qMin( umin, uval );
      umax              = qMax( umax, uval );

      const double dx   = x[ jj ] - x[ jj - 1 ];

      if ( dx > 0.0 )
      {
         maxslope = qMax( maxslope,
                          qAbs( uval - u[ ( jj - 1 ) * stride ] ) / dx );
      }
   }

   const double range = umax - umin;

   if ( range <= 0.0  ||  maxslope <= 0.0 )
   {
      return qInf();                     // constant profile: nothing to resolve
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

double US_GridControl::target_cell( const double feature, const double span,
                                    const double base_h, const Config& cfg )
{
   if ( !cfg.enabled  ||  base_h <= 0.0 )
   {
      return base_h;
   }

   const double npts = qMax( 1, cfg.feature_points );

   // Never finer than the hard refinement limit and never coarser than the
   // unrefined grid: the controller may add resolution, it may not take away
   // what the user asked for through simpoints.
   const double hlim   = base_h / qMax( 1.0, cfg.max_refinement );
   const double hfloor = ( span > 0.0 )
                         ? qMin( qMax( hlim, span / 1.0e6 ), base_h )
                         : hlim;

   double htgt;

   if ( !std::isfinite( feature ) )
   {  // No measurable feature at all - the existing grid is what is wanted
      htgt = base_h;
   }
   else if ( feature <= 0.0 )
   {  // A discontinuity: as sharp as it gets, so ask for the finest cell the
      // controller is ever allowed to create
      htgt = hfloor;
   }
   else
   {
      htgt = feature / npts;
   }

   return qBound( hfloor, htgt, base_h );
}

double US_GridControl::target_step( const double dt_char, const double h,
                                    const double velocity,
                                    const double diffusion, const Config& cfg )
{
   if ( !cfg.enabled  ||  dt_char <= 0.0  ||  h <= 0.0 )
   {
      return dt_char;
   }

   double dt = dt_char;

   if ( velocity > 0.0 )
   {  // Courant condition of the sedimentation transport
      dt = qMin( dt, cfg.courant * h / velocity );
   }

   if ( diffusion > 0.0 )
   {  // Diffusion number of the Crank-Nicolson step: beyond this the high
      // frequency modes of a steep front are inverted instead of damped, which
      // is exactly the ringing seen in band forming simulations.
      dt = qMin( dt, cfg.diff_number * h * h / diffusion );
   }

   // Keep the price bounded. A perfectly sharp initial condition has no
   // resolvable width at all, so without this floor the controller would chase
   // the discontinuity down to machine precision.
   return qMax( dt, dt_char / qMax( 1.0, cfg.max_reduction ) );
}

double US_GridControl::step_for_feature( const double dt_char,
                                         const double feature,
                                         const double span,
                                         const double base_h,
                                         const double velocity,
                                         const double diffusion,
                                         const Config& cfg )
{
   if ( !cfg.enabled  ||  dt_char <= 0.0  ||  base_h <= 0.0 )
   {
      return dt_char;
   }

   const double dt_low   = dt_char / qMax( 1.0, cfg.max_reduction );
   const double h_afford = min_cell( dt_low, velocity, diffusion, cfg );

   double h = target_cell( feature, span, base_h, cfg );

   // Refining below what the cheapest admissible step can carry would only
   // reintroduce the ringing the controller is there to avoid.
   h = qBound( qMin( h_afford, base_h ), h, base_h );

   if ( h >= base_h )
   {  // The feature is already resolved by the requested grid - leave the
      // classic characteristic step alone.
      return dt_char;
   }

   return target_step( dt_char, h, velocity, diffusion, cfg );
}

double US_GridControl::min_cell( const double dt, const double velocity,
                                 const double diffusion, const Config& cfg )
{
   if ( !cfg.enabled  ||  dt <= 0.0 )
   {
      return 0.0;
   }

   double hmin = 0.0;

   if ( velocity > 0.0  &&  cfg.courant > 0.0 )
   {
      hmin = qMax( hmin, velocity * dt / cfg.courant );
   }

   if ( diffusion > 0.0  &&  cfg.diff_number > 0.0 )
   {
      hmin = qMax( hmin, sqrt( diffusion * dt / cfg.diff_number ) );
   }

   return hmin;
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

   const double grow = qMax( 1.0, cfg.growth );

   return qMin( dt_target, dt_prev * grow );
}

double US_GridControl::front_velocity( const double s, const double omega2,
                                       const double radius )
{
   return qAbs( s ) * qAbs( omega2 ) * qAbs( radius );
}

int US_GridControl::points_for_span( const double span, const double h )
{
   if ( span <= 0.0  ||  h <= 0.0 )
   {
      return 2;
   }

   return qMax( 2, static_cast< int >( ceil( span / h ) ) + 1 );
}
