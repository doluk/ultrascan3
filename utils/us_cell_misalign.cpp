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
     beta_deg( beta ), gamma_D( 0.033 ), gamma_s( 0.033 / 13.5 ),
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
   // log-log interpolation in (x, y), extrapolation with the end slopes
   double loglog( const double* x, const double* y, int n, double xx )
   {
      if ( xx <= 0.0 )
         return 1.0;
      int k = 0;
      while ( k < n - 2  &&  xx > x[ k + 1 ] )
         k++;
      const double sl = std::log( y[ k + 1 ] / y[ k ] )
                        / std::log( x[ k + 1 ] / x[ k ] );
      return y[ k ] * std::pow( xx / x[ k ], sl );
   }

   // 2D sector results, BSA 0.1 mg/mL, 1 deg, equal omega^2 t
   // (scripts/misalignment/README.md, section 10)
   const double RPM_X[]  = { 20000.0, 35000.0, 50000.0, 60000.0 };
   const double RPM_D[]  = { 0.237,   0.604,   1.0,     1.144   };
   const double LEN_X[]  = { 0.45,    0.9,     1.2   };
   const double LEN_D[]  = { 0.367,   1.0,     1.280 };
   const double LEN_S[]  = { 0.481,   1.0,     1.169 };

   // common factor (psi / 1 deg) (c / 1 mg/mL)^conc_exp
   double closure_scale( const US_CellMisalignment& m )
   {
      const double cc = ( m.conc > 0.0 ) ? m.conc : 1.0;
      return m.tilt() * std::pow( cc, m.conc_exp );
   }
}

double US_CellMisalignment::speed_factor_D( double rpm )
{
   return loglog( RPM_X, RPM_D, 4, rpm );
}

double US_CellMisalignment::length_factor_D( double length )
{
   return loglog( LEN_X, LEN_D, 3, length );
}

double US_CellMisalignment::length_factor_s( double length )
{
   return loglog( LEN_X, LEN_S, 3, length );
}

double US_CellMisalignment::diffusion_eps( double rpm, double length ) const
{
   return active() ? gamma_D * closure_scale( *this )
                     * speed_factor_D( rpm ) * length_factor_D( length )
                   : 0.0;
}

double US_CellMisalignment::sedimentation_eps( double rpm, double length ) const
{
   return active() ? gamma_s * closure_scale( *this )
                     * length_factor_s( length )
                   : 0.0;
}
