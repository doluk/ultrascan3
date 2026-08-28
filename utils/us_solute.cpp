//! \file us_solute.cpp
#include "us_solute.h"
#include "us_math2.h"
#include "us_model.h"
#include "us_zsolute.h"

// The attribute numbering is shared across the solute classes; keep it so.
static_assert( (int)US_Solute::ATTR_S == (int)US_ZSolute::ATTR_S &&
               (int)US_Solute::ATTR_K == (int)US_ZSolute::ATTR_K &&
               (int)US_Solute::ATTR_W == (int)US_ZSolute::ATTR_W &&
               (int)US_Solute::ATTR_V == (int)US_ZSolute::ATTR_V &&
               (int)US_Solute::ATTR_D == (int)US_ZSolute::ATTR_D,
               "US_Solute and US_ZSolute attribute numbering must agree" );

US_Solute::US_Solute( double s0, double k0, double c0,
                      double v0, double d0 )
{
   s = s0;
   k = k0;
   c = c0;
   v = v0;
   d = d0;
}

void US_Solute::init_solutes( double s_min,   double s_max,   int s_res,
                              double ff0_min, double ff0_max, int ff0_res,
                              int    grid_reps, double cnstff0,
                              QList< QVector< US_Solute > >& solute_list )
{
   grid_reps       = qMax( grid_reps, 1 );
   int nprs        = qMax( 1, ( s_res   - 1 ) );
   int nprk        = qMax( 1, ( ff0_res - 1 ) );
   double s_grid   = qAbs( s_max   - s_min   ) / (double)nprs;
   double ff0_grid = qAbs( ff0_max - ff0_min ) / (double)nprk;
   double s_step   = s_grid   * grid_reps;
   double ff0_step = ff0_grid * grid_reps;
qDebug() << "InSo: nprs nprk" << nprs << nprk
 << "s_step k_step" << s_step*1.e+13 << ff0_step
 << "s_grid k_grid" << s_grid*1.e+13 << ff0_grid;

   // Allow a 1% overscan
   s_max          += 0.01 * s_step;
   ff0_max        += 0.01 * ff0_step;

   solute_list.reserve( sq( grid_reps ) );

   // Generate solutes for each grid repetition
   for ( int js = 0; js < grid_reps; js++ )
   {
      double s_min_g  = s_min + s_grid * js;
      for ( int jk = 0; jk < grid_reps; jk++ )
      {
         double k_min_g  = ff0_min + ff0_grid * jk;
         solute_list << create_solutes( s_min_g,   s_max,   s_step,
                                        k_min_g, ff0_max, ff0_step,
                                        cnstff0 );
      }
   }
}

QVector< US_Solute > US_Solute::create_solutes(
        double s_min,   double s_max,   double s_step,
        double ff0_min, double ff0_max, double ff0_step,
        double cnstff0 )
{
   QVector< US_Solute > solute_vector;
   double off0  = cnstff0;
   double ovbar = 0.0;

   for ( double ff0 = ff0_min; ff0 <= ff0_max; ff0 += ff0_step )
   {
      if ( cnstff0 > 0.0 )
         ovbar = ff0;
      else
         off0  = ff0;

      for ( double svl = s_min; svl <= s_max; svl += s_step )
      {
         // Omit s values close to zero.
         if ( svl >= -5.0e-15  &&  svl <= 5.0e-15 ) continue;

         solute_vector << US_Solute( svl, off0, 0.0, ovbar );
      }
   }

   return solute_vector;
}



//----------------------------------------------------------------------------
// Three-dimensional grids
//----------------------------------------------------------------------------

const double US_Solute::FF0_SPHERE_TOLER = 1.0e-9;

// Internal: the US_Solute field an attribute maps to, matching
// US_SolveSim::set_comp_attr().  Returns 's', 'k', 'v' or 'd'.
static char solute_field( int a_type )
{
   switch ( a_type )
   {
      case US_Solute::ATTR_S:  return 's';
      case US_Solute::ATTR_K:  return 'k';
      case US_Solute::ATTR_V:  return 'v';
      case US_Solute::ATTR_W:                  // MW, D and f all share the
      case US_Solute::ATTR_D:                  //  single "d" field
      case US_Solute::ATTR_F:  return 'd';
      default:                 return '?';
   }
}

// Internal: store one axis value into the field its attribute maps to.
static void put_solute_attr( US_Solute& solute, double value, int a_type )
{
   switch ( solute_field( a_type ) )
   {
      case 's':  solute.s = value;  break;
      case 'k':  solute.k = value;  break;
      case 'v':  solute.v = value;  break;
      default:   solute.d = value;  break;
   }
}

// Internal: name of an attribute, for error messages.
static QString attr_name( int a_type )
{
   switch ( a_type )
   {
      case US_Solute::ATTR_S:  return QString( "s"    );
      case US_Solute::ATTR_K:  return QString( "f/f0" );
      case US_Solute::ATTR_W:  return QString( "MW"   );
      case US_Solute::ATTR_V:  return QString( "vbar" );
      case US_Solute::ATTR_D:  return QString( "D"    );
      case US_Solute::ATTR_F:  return QString( "f"    );
      default:                 return QString::number( a_type );
   }
}

bool US_Solute::validate_mask( int s_mask, QString& errmsg )
{
   errmsg           = QString( "" );

   const int attrs[ 3 ] = { ( s_mask >> 6 ) & 7,
                            ( s_mask >> 3 ) & 7,
                              s_mask        & 7 };
   const QString axis[ 3 ] = { QString( "X" ), QString( "Y" ),
                               QString( "Z" ) };

   for ( int ii = 0; ii < 3; ii++ )
   {
      if ( solute_field( attrs[ ii ] ) == '?' )
      {
         errmsg = QObject::tr( "The %1 axis names attribute %2, which is not"
                               " a solute attribute." )
                  .arg( axis[ ii ] ).arg( attrs[ ii ] );
         return false;
      }
   }

   for ( int ii = 0; ii < 3; ii++ )
   {
      for ( int jj = ii + 1; jj < 3; jj++ )
      {
         if ( attrs[ ii ] == attrs[ jj ] )
         {
            errmsg = QObject::tr( "The %1 and %2 axes both name \"%3\"." )
                     .arg( axis[ ii ] ).arg( axis[ jj ] )
                     .arg( attr_name( attrs[ ii ] ) );
            return false;
         }

         if ( solute_field( attrs[ ii ] ) == solute_field( attrs[ jj ] ) )
         {
            errmsg = QObject::tr( "The %1 axis (\"%2\") and the %3 axis"
                                  " (\"%4\") cannot both be grid axes:"
                                  " a solute holds only one of MW, D and f." )
                     .arg( axis[ ii ] ).arg( attr_name( attrs[ ii ] ) )
                     .arg( axis[ jj ] ).arg( attr_name( attrs[ jj ] ) );
            return false;
         }
      }
   }

   return true;
}

int US_Solute::init_solutes_3d(
        double x_min, double x_max, int x_res,
        double y_min, double y_max, int y_res,
        double z_min, double z_max, int z_res,
        int grid_reps, int s_mask,
        QList< QVector< US_Solute > >& solute_list )
{
   solute_list.clear();

   QString errmsg;
   if ( ! validate_mask( s_mask, errmsg ) )
   {
      qDebug() << "US_Solute::init_solutes_3d: invalid mask" << s_mask
               << ":" << errmsg;
      return 0;
   }

   const int attr_x = ( s_mask >> 6 ) & 7;
   const int attr_y = ( s_mask >> 3 ) & 7;
   const int attr_z =   s_mask        & 7;

   x_res            = qMax( 1, x_res );
   y_res            = qMax( 1, y_res );
   z_res            = qMax( 1, z_res );

   // Never produce an empty subgrid: with more repetitions than points on an
   // axis, some residue classes would contain nothing at all.
   const int reps   = qBound( 1, grid_reps,
                              qMin( x_res, qMin( y_res, z_res ) ) );

   // Point spacing.  A single-point axis sits at its minimum.
   const double x_grid = ( x_res > 1 ) ? ( x_max - x_min ) / ( x_res - 1 ) : 0.0;
   const double y_grid = ( y_res > 1 ) ? ( y_max - y_min ) / ( y_res - 1 ) : 0.0;
   const double z_grid = ( z_res > 1 ) ? ( z_max - z_min ) / ( z_res - 1 ) : 0.0;

   // Which axis, if any, carries s -- needed for the zero-s guard.
   const int sx = ( attr_x == ATTR_S ) ? 0
                : ( attr_y == ATTR_S ) ? 1
                : ( attr_z == ATTR_S ) ? 2 : -1;

   // The rectangular s-by-D parameterization needs the f/f0 >= 1 filter.
   const bool has_s   = ( sx >= 0 );
   const bool has_d   = ( attr_x == ATTR_D  ||  attr_y == ATTR_D  ||
                          attr_z == ATTR_D );
   const bool has_v   = ( attr_x == ATTR_V  ||  attr_y == ATTR_V  ||
                          attr_z == ATTR_V );
   const bool drop_unphysical = ( has_s  &&  has_d  &&  has_v );

   solute_list.reserve( reps * reps * reps );

   for ( int ri = 0; ri < reps; ri++ )
   {
      for ( int rj = 0; rj < reps; rj++ )
      {
         for ( int rk = 0; rk < reps; rk++ )
         {
            QVector< US_Solute > solutes;

            for ( int kk = rk; kk < z_res; kk += reps )
            {
               const double zval = z_min + z_grid * kk;

               for ( int jj = rj; jj < y_res; jj += reps )
               {
                  const double yval = y_min + y_grid * jj;

                  for ( int ii = ri; ii < x_res; ii += reps )
                  {
                     const double xval = x_min + x_grid * ii;

                     // Omit s values close to zero, as the 2-D grid does.
                     if ( sx == 0  &&  qAbs( xval ) <= 5.0e-15 ) continue;
                     if ( sx == 1  &&  qAbs( yval ) <= 5.0e-15 ) continue;
                     if ( sx == 2  &&  qAbs( zval ) <= 5.0e-15 ) continue;

                     US_Solute solute;
                     put_solute_attr( solute, xval, attr_x );
                     put_solute_attr( solute, yval, attr_y );
                     put_solute_attr( solute, zval, attr_z );

                     if ( drop_unphysical  &&  ! physical_sdv( solute ) )
                        continue;

                     solutes << solute;
                  }
               }
            }

            solute_list << solutes;
         }
      }
   }

   return reps;
}

// Internal: is an (s, D, vbar) solute a shape a particle could actually have?
// Completes the coefficients and rejects f/f0 below 1.0, which would mean a
// particle more compact than a sphere of the same mass.
bool US_Solute::physical_sdv( const US_Solute& solute )
{
   US_Model::SimulationComponent comp;
   comp.s      = solute.s;
   comp.D      = solute.d;
   comp.vbar20 = solute.v;
   comp.f_f0   = 0.0;
   comp.mw     = 0.0;
   comp.f      = 0.0;

   if ( ! US_Model::calc_coefficients( comp ) )
      return false;                       // e.g. buoyancy/s sign mismatch

   // Deriving D from f/f0 = 1 and then f/f0 back from that D lands a few ulp
   // below 1.0, so a strict test would discard genuine spheres.  The
   // tolerance sits far above that round-off and far below any physically
   // meaningful departure from sphericity.
   return ( comp.f_f0 >= ( 1.0 - FF0_SPHERE_TOLER ) );
}
