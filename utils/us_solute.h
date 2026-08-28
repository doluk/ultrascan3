//! \file us_solute.h
#ifndef US_SOLUTE_H
#define US_SOLUTE_H

#include "us_extern.h"

//! \brief Solutes for UltraScan
//!
//! This class provides a solute structure and utilities

class US_UTIL_EXTERN US_Solute
{
   public:
      //! \param s0 The initial sedimentation
      //! \param k0 The initial frictional ratio
      //! \param c0 The initial concentration
      //! \param v0 The initial vbar
      //! \param d0 The initial diffusion coefficient (or MW or f)
      US_Solute( double = 0.0, double = 0.0, double = 0.0,
                 double = 0.0, double = 0.0 );

      double s;  //!< Sedimentation value
      double k;  //!< Frictional ratio
      double c;  //!< Concentration
      double v;  //!< Vbar
      double d;  //!< Diffusion coefficient (or Molec.Weight or Fric.Coeff.)

      //! A test for solute equality
      //! \param solute A value for comparison 
      bool operator== ( const US_Solute& solute ) const
      {
         return ( s == solute.s  &&  k == solute.k  &&
                  v == solute.v  &&  d == solute.d );
      }

      //! A test for inequality.
      //! \param solute A value for comparison 
      bool operator!= ( const US_Solute& solute ) const
      {
         return ( s != solute.s  ||  k != solute.k  ||
                  v != solute.v  ||  d != solute.d );
      }

      //! A test for ordering solutes.  Tests sedimentation before frictional
      //! ratio.
      //! \param solute A value for comparison 
      bool operator< ( const US_Solute& solute ) const
      {
         if ( s < solute.s )
            return true;

         else if (  s == solute.s  &&  k < solute.k )
            return true;

         else if (  s == solute.s  &&  k == solute.k  &&
                     d < solute.d )
            return true;

         else if (  s == solute.s  &&  k == solute.k   &&
                    d == solute.d  &&  v < solute.v )
            return true;

         else
            return false;
      }

      //! A static function to initialize solutes
      //! \param s_min The minimum sedimentation value
      //! \param s_max The maximum sedimentation value
      //! \param s_res The number of ponts to evaluate between s_min and s_max
      //! \param ff0_min The minimum frictional ratio
      //! \param ff0_max The maximum frictional ratio
      //! \param ff0_res The number of ponts to evaluate between ff0_min and 
      //!                ff0_max
      //! \param grid_reps The number of grids used to partition the data
      //! \param cnstff0   Constant f/f0 (or 0.0 if vbar is constant)
      //! \param solute_list A reference to the output values.  This is a 
      //!                    list of vectors.
      static void init_solutes( double, double, int,
                                double, double, int, int, double,
                                QList< QVector< US_Solute > >& );

      static QVector< US_Solute > create_solutes(
                    double s_min,   double s_max,   double s_step,
                    double ff0_min, double ff0_max, double ff0_step,
                    double cnstff0 );

      //! \brief The attribute an axis of a solute grid carries.
      //!
      //! These values are the canonical UltraScan attribute numbering, shared
      //! with US_ZSolute::attr_type and US_SolveSim::attr_type; us_solute.cpp
      //! static-asserts that they agree.  A 9-bit "solute type" mask packs
      //! three of them as ( x << 6 ) | ( y << 3 ) | z.
      enum attr_type { ATTR_S = 0,   //!< Sedimentation coefficient -> s
                       ATTR_K,       //!< Frictional ratio          -> k
                       ATTR_W,       //!< Molecular weight          -> d
                       ATTR_V,       //!< Partial specific volume   -> v
                       ATTR_D,       //!< Diffusion coefficient     -> d
                       ATTR_F };     //!< Frictional coefficient    -> d

      //! \brief Check that an attribute mask can be represented by a solute
      //!
      //! US_Solute has one field each for s, k and v but a single shared
      //! field for MW, D and f, so a mask may name at most one of those
      //! three.  Duplicate and out-of-range attributes are rejected too.
      //!
      //! \param s_mask  Attribute mask, ( x << 6 ) | ( y << 3 ) | z
      //! \param errmsg  Returned description of the problem, empty if valid
      //! \returns       True if the mask is usable
      static bool validate_mask( int s_mask, QString& errmsg );

      //! \brief Is an (s, D, vbar) solute a shape a particle could have?
      //!
      //! Completes the coefficients and rejects an implied f/f0 below 1.0,
      //! which would describe a particle more compact than a sphere of equal
      //! mass.  Used to trim the rectangular s-by-D grid, most of which is
      //! otherwise spent on impossible shapes.
      //!
      //! \param solute  Solute carrying s in .s, D in .d and vbar in .v
      //! \returns       True if the implied f/f0 is at least 1.0, within
      //!                FF0_SPHERE_TOLER
      static bool physical_sdv( const US_Solute& solute );

      //! Tolerance below f/f0 = 1.0 that physical_sdv() still accepts.
      //! Round-tripping an exact sphere through US_Model::calc_coefficients()
      //! lands a few ulp low, so a strict test would discard real spheres.
      static const double FF0_SPHERE_TOLER;

      //! \brief Initialize a three-dimensional grid of solutes
      //!
      //! Builds an x_res by y_res by z_res grid over the given ranges and
      //! partitions it into grid_reps cubed interleaved subgrids, each a
      //! coarse covering of the whole box.  Subgrid ( i, j, k ) takes the
      //! points whose indices are congruent to ( i, j, k ) modulo grid_reps,
      //! so the subgrids partition the grid exactly: every point appears in
      //! one of them, and none appears twice.
      //!
      //! Grid points are computed by integer index rather than by repeated
      //! addition, so the point count is exact and does not depend on
      //! floating-point accumulation.
      //!
      //! Two filters are applied.  Points whose s value is within rounding of
      //! zero are dropped, as they are for the 2-D grid.  When the mask names
      //! both s and D -- the rectangular s-by-D parameterization -- points
      //! whose implied f/f0 is below 1.0 are also dropped, since no particle
      //! can be more compact than a sphere of equal mass.
      //!
      //! \param x_min       Minimum of the X axis
      //! \param x_max       Maximum of the X axis
      //! \param x_res       Number of points on the X axis
      //! \param y_min       Minimum of the Y axis
      //! \param y_max       Maximum of the Y axis
      //! \param y_res       Number of points on the Y axis
      //! \param z_min       Minimum of the Z axis
      //! \param z_max       Maximum of the Z axis
      //! \param z_res       Number of points on the Z axis
      //! \param grid_reps   Requested number of subgrid repetitions per axis
      //! \param s_mask      Attribute mask, ( x << 6 ) | ( y << 3 ) | z
      //! \param solute_list Returned list of grid_reps cubed solute vectors
      //! \returns           The grid_reps actually used, clamped so that no
      //!                    subgrid is empty; zero if the mask is invalid
      static int init_solutes_3d(
                    double x_min, double x_max, int x_res,
                    double y_min, double y_max, int y_res,
                    double z_min, double z_max, int z_res,
                    int grid_reps, int s_mask,
                    QList< QVector< US_Solute > >& solute_list );
};
#endif
