//! \file us_cell_misalign.cpp
#include "us_cell_misalign.h"

#include <cmath>
#include <algorithm>

namespace
{
   const double DEG2RAD = M_PI / 180.0;
}

US_CellMisalignment::US_CellMisalignment( double delta, double sector,
      double pivot_r, double kap, double beta )
   : delta_deg( delta ), sector_deg( sector ), pivot( pivot_r ),
     kappa( kap ), beta_deg( beta )
{
}

bool US_CellMisalignment::active() const
{
   return ( delta_deg != 0.0  &&  kappa > 0.0  &&  sector_deg > 0.0
            &&  pivot > 0.0 );
}

double US_CellMisalignment::wall_offset( int side ) const
{
   const double phi0 = ( beta_deg + ( side < 0 ? -0.5 : 0.5 ) * sector_deg )
                       * DEG2RAD;
   const double dlt  = delta_deg * DEG2RAD;
   return pivot * ( std::sin( phi0 + dlt ) - std::sin( phi0 ) );
}

double US_CellMisalignment::mean_offset() const
{
   return std::fabs( 0.5 * ( wall_offset( 1 ) + wall_offset( -1 ) ) );
}

double US_CellMisalignment::wall_tilt( double r, int side ) const
{
   return std::asin( wall_offset( side ) / r ) / DEG2RAD;
}

double US_CellMisalignment::wall_phi( double r, int side ) const
{
   const double phi0 = ( beta_deg + ( side < 0 ? -0.5 : 0.5 ) * sector_deg )
                       * DEG2RAD;
   return phi0 + delta_deg * DEG2RAD - std::asin( wall_offset( side ) / r );
}

double US_CellMisalignment::angular_width( double r ) const
{
   return wall_phi( r, 1 ) - wall_phi( r, -1 );
}

double US_CellMisalignment::radial_shift() const
{
   return -delta_deg * DEG2RAD * pivot * std::sin( beta_deg * DEG2RAD );
}

double US_CellMisalignment::beam_margin( double r ) const
{
   const double phib = beta_deg * DEG2RAD;
   return std::min( wall_phi( r, 1 ) - phib, phib - wall_phi( r, -1 ) ) * r;
}

double US_CellMisalignment::bottom_smear( double bottom ) const
{
   return sector_deg * DEG2RAD * bottom
          * std::fabs( std::sin( delta_deg * DEG2RAD ) );
}

double US_CellMisalignment::velocity_eps( double r, double bottom ) const
{
   if ( ! active()  ||  r >= bottom )
      return 0.0;

   return kappa * mean_offset() * ( bottom - r )
          / ( angular_width( r ) * r * r );
}

double US_CellMisalignment::sink_rate( double r, double s_w2 ) const
{
   if ( ! active() )
      return 0.0;

   return kappa * s_w2 * mean_offset() / ( angular_width( r ) * r );
}
