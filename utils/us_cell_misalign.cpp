//! \file us_cell_misalign.cpp
#include "us_cell_misalign.h"

#include <cmath>
#include <algorithm>

namespace
{
   const double DEG2RAD = M_PI / 180.0;
}

US_CellMisalignment::US_CellMisalignment( double delta, double sector,
      double pivot_r, double beta )
   : delta_deg( delta ), sector_deg( sector ), pivot( pivot_r ),
     beta_deg( beta ), gamma_D( 0.043 ), gamma_s( 0.043 / 12.5 ),
     conc( 1.0 ), conc_exp( -0.12 )
{
}

bool US_CellMisalignment::active() const
{
   return ( delta_deg != 0.0  &&  sector_deg > 0.0  &&  pivot > 0.0
            &&  ( gamma_D != 0.0  ||  gamma_s != 0.0 ) );
}

double US_CellMisalignment::tilt() const
{
   return std::asin( std::min( 1.0, mean_offset() / pivot ) ) / DEG2RAD;
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

namespace
{
   // common factor (psi / 1 deg) (c / 1 mg/mL)^conc_exp
   double closure_scale( const US_CellMisalignment& m )
   {
      const double cc = ( m.conc > 0.0 ) ? m.conc : 1.0;
      return m.tilt() * std::pow( cc, m.conc_exp );
   }
}

double US_CellMisalignment::diffusion_eps() const
{
   return active() ? gamma_D * closure_scale( *this ) : 0.0;
}

double US_CellMisalignment::sedimentation_eps() const
{
   return active() ? gamma_s * closure_scale( *this ) : 0.0;
}
