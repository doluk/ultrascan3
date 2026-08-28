// test_us_solute3d.cpp - Unit tests for three-dimensional solute grids
//
// Covers US_Solute::validate_mask(), US_Solute::physical_sdv() and
// US_Solute::init_solutes_3d(), the grid generation a 3DSA fit is built on.
// See doc/develop/3dsa_design.md sections 5.1 and 6.1.
//
// The property that matters most is the partition: the subgrids must between
// them contain every grid point exactly once.  A subgrid that silently
// duplicates or drops points would show up as a distorted distribution rather
// than as an obvious failure, so it is asserted directly here.

#include "qt_test_base.h"

#include "us_solute.h"
#include "us_zsolute.h"
#include "us_model.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <vector>

namespace {

// Attribute masks, written as they read in octal: 0<x><y><z>.
int mask_of( int ax, int ay, int az )
{
   return ( ax << 6 ) | ( ay << 3 ) | az;
}

// s, f/f0, vbar -- the primary 3DSA parameterization.
const int MASK_S_K_V = mask_of( US_Solute::ATTR_S,
                                US_Solute::ATTR_K,
                                US_Solute::ATTR_V );

// s, D, vbar -- the alternate axis mapping.
const int MASK_S_D_V = mask_of( US_Solute::ATTR_S,
                                US_Solute::ATTR_D,
                                US_Solute::ATTR_V );

int total_solutes( const QList< QVector< US_Solute > >& list )
{
   int total = 0;
   for ( const QVector< US_Solute >& sv : list )
      total += sv.size();
   return total;
}

// A comparable key for a solute, at a tolerance far below any grid spacing
// used here, so that duplicates can be counted.
std::vector< long long > key_of( const US_Solute& s )
{
   return { (long long)llround( s.s * 1.0e+18 ),
            (long long)llround( s.k * 1.0e+9  ),
            (long long)llround( s.v * 1.0e+9  ),
            (long long)llround( s.d * 1.0e+15 ) };
}

} // namespace

class TestUSSolute3D : public QtTestBase {};

// ---------------------------------------------------------------------------
// Mask validation
// ---------------------------------------------------------------------------
TEST_F( TestUSSolute3D, ValidMasksAreAccepted )
{
   QString err;

   EXPECT_TRUE( US_Solute::validate_mask( MASK_S_K_V, err ) ) << err.toStdString();
   EXPECT_TRUE( err.isEmpty() );

   EXPECT_TRUE( US_Solute::validate_mask( MASK_S_D_V, err ) ) << err.toStdString();

   // Axis order is free: the mask says which attribute, not which slot.
   EXPECT_TRUE( US_Solute::validate_mask(
                   mask_of( US_Solute::ATTR_S, US_Solute::ATTR_V,
                            US_Solute::ATTR_K ), err ) ) << err.toStdString();
}

TEST_F( TestUSSolute3D, SharedFieldMasksAreRejected )
{
   QString err;

   // MW, D and f all live in the single "d" field, so no two of them can be
   // grid axes at once.
   EXPECT_FALSE( US_Solute::validate_mask(
                    mask_of( US_Solute::ATTR_S, US_Solute::ATTR_D,
                             US_Solute::ATTR_W ), err ) );
   EXPECT_FALSE( err.isEmpty() );

   EXPECT_FALSE( US_Solute::validate_mask(
                    mask_of( US_Solute::ATTR_D, US_Solute::ATTR_F,
                             US_Solute::ATTR_V ), err ) );
   EXPECT_FALSE( US_Solute::validate_mask(
                    mask_of( US_Solute::ATTR_W, US_Solute::ATTR_S,
                             US_Solute::ATTR_F ), err ) );
}

TEST_F( TestUSSolute3D, DuplicateAndUnknownAttributesAreRejected )
{
   QString err;

   EXPECT_FALSE( US_Solute::validate_mask(
                    mask_of( US_Solute::ATTR_S, US_Solute::ATTR_S,
                             US_Solute::ATTR_V ), err ) );
   EXPECT_FALSE( err.isEmpty() );

   // 6 and 7 fit in the three bits but name no attribute.
   EXPECT_FALSE( US_Solute::validate_mask(
                    mask_of( US_Solute::ATTR_S, 6, US_Solute::ATTR_V ), err ) );
   EXPECT_FALSE( US_Solute::validate_mask(
                    mask_of( 7, US_Solute::ATTR_K, US_Solute::ATTR_V ), err ) );
}

TEST_F( TestUSSolute3D, AttributeNumberingMatchesZSolute )
{
   // us_solute.cpp static-asserts this; check it from the outside too, since
   // a divergence would silently mis-map every grid axis.
   EXPECT_EQ( (int)US_Solute::ATTR_S, (int)US_ZSolute::ATTR_S );
   EXPECT_EQ( (int)US_Solute::ATTR_K, (int)US_ZSolute::ATTR_K );
   EXPECT_EQ( (int)US_Solute::ATTR_W, (int)US_ZSolute::ATTR_W );
   EXPECT_EQ( (int)US_Solute::ATTR_V, (int)US_ZSolute::ATTR_V );
   EXPECT_EQ( (int)US_Solute::ATTR_D, (int)US_ZSolute::ATTR_D );
}

// ---------------------------------------------------------------------------
// Grid generation
// ---------------------------------------------------------------------------
TEST_F( TestUSSolute3D, InvalidMaskProducesNoGrid )
{
   QList< QVector< US_Solute > > grid;

   const int reps = US_Solute::init_solutes_3d(
                       1.0e-13, 10.0e-13, 8,
                       1.0,      4.0,     6,
                       0.60,     0.85,    5,
                       2, mask_of( US_Solute::ATTR_S, US_Solute::ATTR_D,
                                   US_Solute::ATTR_W ), grid );

   EXPECT_EQ( 0, reps );
   EXPECT_TRUE( grid.isEmpty() );
}

TEST_F( TestUSSolute3D, SubgridsPartitionTheGridExactly )
{
   const int nx = 12, ny = 9, nz = 6, reps = 3;

   QList< QVector< US_Solute > > grid;
   const int used = US_Solute::init_solutes_3d(
                       1.0e-13, 10.0e-13, nx,
                       1.0,      4.0,     ny,
                       0.60,     0.85,    nz,
                       reps, MASK_S_K_V, grid );

   ASSERT_EQ( reps, used );
   ASSERT_EQ( reps * reps * reps, grid.size() );

   // No subgrid is empty ...
   for ( int ii = 0; ii < grid.size(); ii++ )
      EXPECT_GT( grid[ ii ].size(), 0 ) << "subgrid " << ii;

   // ... every point appears ...
   EXPECT_EQ( nx * ny * nz, total_solutes( grid ) );

   // ... and none appears twice.
   std::set< std::vector< long long > > seen;
   for ( const QVector< US_Solute >& sv : grid )
      for ( const US_Solute& s : sv )
         EXPECT_TRUE( seen.insert( key_of( s ) ).second )
            << "duplicated grid point";

   EXPECT_EQ( (size_t)( nx * ny * nz ), seen.size() );
}

TEST_F( TestUSSolute3D, GridPointsSpanTheRequestedRanges )
{
   const double slo = 1.0e-13, sup = 10.0e-13;
   const double klo = 1.0,     kup = 4.0;
   const double vlo = 0.60,    vup = 0.85;
   const int    nx  = 10,      ny  = 7,   nz = 5;

   QList< QVector< US_Solute > > grid;
   ASSERT_EQ( 2, US_Solute::init_solutes_3d( slo, sup, nx, klo, kup, ny,
                                             vlo, vup, nz, 2,
                                             MASK_S_K_V, grid ) );

   double s_lo = 1.0e+30, s_hi = -1.0e+30;
   double k_lo = 1.0e+30, k_hi = -1.0e+30;
   double v_lo = 1.0e+30, v_hi = -1.0e+30;

   for ( const QVector< US_Solute >& sv : grid )
      for ( const US_Solute& s : sv )
      {
         s_lo = std::min( s_lo, s.s );  s_hi = std::max( s_hi, s.s );
         k_lo = std::min( k_lo, s.k );  k_hi = std::max( k_hi, s.k );
         v_lo = std::min( v_lo, s.v );  v_hi = std::max( v_hi, s.v );
      }

   // Endpoints are hit exactly: points are indexed, not accumulated.
   EXPECT_DOUBLE_EQ( slo, s_lo );  EXPECT_DOUBLE_EQ( sup, s_hi );
   EXPECT_DOUBLE_EQ( klo, k_lo );  EXPECT_DOUBLE_EQ( kup, k_hi );
   EXPECT_DOUBLE_EQ( vlo, v_lo );  EXPECT_DOUBLE_EQ( vup, v_hi );
}

TEST_F( TestUSSolute3D, SingleRepetitionGivesOneFullGrid )
{
   const int nx = 5, ny = 4, nz = 3;

   QList< QVector< US_Solute > > grid;
   ASSERT_EQ( 1, US_Solute::init_solutes_3d( 1.0e-13, 5.0e-13, nx,
                                             1.0,     3.0,     ny,
                                             0.70,    0.80,    nz,
                                             1, MASK_S_K_V, grid ) );

   ASSERT_EQ( 1, grid.size() );
   EXPECT_EQ( nx * ny * nz, grid[ 0 ].size() );
}

TEST_F( TestUSSolute3D, RepetitionsAreClampedSoNoSubgridIsEmpty )
{
   // Asking for more repetitions than there are points on the shortest axis
   // would leave whole residue classes empty; the count is clamped instead.
   QList< QVector< US_Solute > > grid;
   const int used = US_Solute::init_solutes_3d( 1.0e-13, 9.0e-13, 9,
                                                1.0,     4.0,     7,
                                                0.70,    0.80,    3,
                                                8, MASK_S_K_V, grid );

   EXPECT_EQ( 3, used );
   ASSERT_EQ( 27, grid.size() );
   for ( int ii = 0; ii < grid.size(); ii++ )
      EXPECT_GT( grid[ ii ].size(), 0 ) << "subgrid " << ii;

   EXPECT_EQ( 9 * 7 * 3, total_solutes( grid ) );
}

TEST_F( TestUSSolute3D, AxisValuesLandInTheFieldTheMaskNames )
{
   QList< QVector< US_Solute > > grid;

   // s in X, vbar in Y, f/f0 in Z -- deliberately not the natural order.
   ASSERT_EQ( 1, US_Solute::init_solutes_3d(
                    2.0e-13, 2.0e-13, 1,
                    0.75,    0.75,    1,
                    1.6,     1.6,     1,
                    1, mask_of( US_Solute::ATTR_S, US_Solute::ATTR_V,
                                US_Solute::ATTR_K ), grid ) );

   ASSERT_EQ( 1, grid[ 0 ].size() );
   const US_Solute& s = grid[ 0 ][ 0 ];
   EXPECT_DOUBLE_EQ( 2.0e-13, s.s );
   EXPECT_DOUBLE_EQ( 0.75,    s.v );
   EXPECT_DOUBLE_EQ( 1.6,     s.k );
   EXPECT_DOUBLE_EQ( 0.0,     s.d );

   // With D as an axis the value lands in the shared "d" field instead.
   grid.clear();
   ASSERT_EQ( 1, US_Solute::init_solutes_3d(
                    2.0e-13, 2.0e-13, 1,
                    5.0e-7,  5.0e-7,  1,
                    0.75,    0.75,    1,
                    1, MASK_S_D_V, grid ) );
   ASSERT_EQ( 1, grid[ 0 ].size() );
   EXPECT_DOUBLE_EQ( 5.0e-7, grid[ 0 ][ 0 ].d );
   EXPECT_DOUBLE_EQ( 0.0,    grid[ 0 ][ 0 ].k );
}

TEST_F( TestUSSolute3D, ZeroSedimentationPointsAreOmitted )
{
   // An s range straddling zero, with a point exactly on it.
   QList< QVector< US_Solute > > grid;
   ASSERT_EQ( 1, US_Solute::init_solutes_3d( -4.0e-13, 4.0e-13, 5,
                                              1.0,     3.0,     3,
                                              0.70,    0.80,    2,
                                              1, MASK_S_K_V, grid ) );

   // 5 x 3 x 2 = 30 points, less the 3 x 2 = 6 sitting at s = 0.
   EXPECT_EQ( 30 - 6, grid[ 0 ].size() );

   for ( const US_Solute& s : grid[ 0 ] )
      EXPECT_GT( std::fabs( s.s ), 5.0e-15 );
}

// ---------------------------------------------------------------------------
// The f/f0 >= 1 filter on the rectangular s-by-D grid
// ---------------------------------------------------------------------------
TEST_F( TestUSSolute3D, PhysicalSdvRejectsSubSphericalShapes )
{
   // Build a species with a known f/f0, then perturb D to move f/f0 across 1.
   US_Model::SimulationComponent comp;
   comp.s      = 4.0e-13;
   comp.f_f0   = 1.0;
   comp.vbar20 = 0.73;
   comp.D      = 0.0;
   comp.mw     = 0.0;
   comp.f      = 0.0;
   ASSERT_TRUE( US_Model::calc_coefficients( comp ) );

   const double d_at_unity = comp.D;

   // An exact sphere must survive.  Deriving D from f/f0 = 1 and reading
   // f/f0 back off that D lands a few ulp low, which is what
   // FF0_SPHERE_TOLER exists to absorb.
   US_Solute on_sphere;
   on_sphere.s = 4.0e-13;
   on_sphere.v = 0.73;
   on_sphere.d = d_at_unity;
   EXPECT_TRUE( US_Solute::physical_sdv( on_sphere ) );

   // Larger D means faster diffusion than a sphere of that mass: impossible.
   US_Solute too_fast = on_sphere;
   too_fast.d = d_at_unity * 1.10;
   EXPECT_FALSE( US_Solute::physical_sdv( too_fast ) );

   // The tolerance must not be a loophole.  A 0.15% rise in D puts f/f0 near
   // 0.999, which is unphysical by far more than round-off, and is rejected.
   US_Solute just_inside = on_sphere;
   just_inside.d = d_at_unity * 1.0015;
   EXPECT_FALSE( US_Solute::physical_sdv( just_inside ) );

   // Smaller D is an elongated or hydrated particle: fine.
   US_Solute elongated = on_sphere;
   elongated.d = d_at_unity * 0.50;
   EXPECT_TRUE( US_Solute::physical_sdv( elongated ) );

   // The tolerance is a round-off allowance, not a physical margin.
   EXPECT_LT( US_Solute::FF0_SPHERE_TOLER, 1.0e-6 );
   EXPECT_GT( US_Solute::FF0_SPHERE_TOLER, 1.0e-13 );
}

TEST_F( TestUSSolute3D, SdvGridDropsUnphysicalPointsOnly )
{
   const int nx = 8, ny = 8, nz = 4;

   QList< QVector< US_Solute > > grid;
   const int used = US_Solute::init_solutes_3d( 1.0e-13, 8.0e-13,  nx,
                                                1.0e-7,  9.0e-7,   ny,
                                                0.65,    0.80,     nz,
                                                2, MASK_S_D_V, grid );
   ASSERT_EQ( 2, used );

   const int kept = total_solutes( grid );

   // The rectangular s-by-D box is mostly impossible shapes, so the filter
   // must bite -- that is the point of it -- but must not empty the grid.
   EXPECT_LT( kept, nx * ny * nz );
   EXPECT_GT( kept, 0 );

   // Everything that survived is physical, and nothing physical was dropped.
   int physical_in_box = 0;
   for ( int ii = 0; ii < nx; ii++ )
      for ( int jj = 0; jj < ny; jj++ )
         for ( int kk = 0; kk < nz; kk++ )
         {
            US_Solute s;
            s.s = 1.0e-13 + ( 8.0e-13 - 1.0e-13 ) * ii / ( nx - 1 );
            s.d = 1.0e-7  + ( 9.0e-7  - 1.0e-7  ) * jj / ( ny - 1 );
            s.v = 0.65    + ( 0.80    - 0.65    ) * kk / ( nz - 1 );
            if ( US_Solute::physical_sdv( s ) )  physical_in_box++;
         }

   EXPECT_EQ( physical_in_box, kept );

   for ( const QVector< US_Solute >& sv : grid )
      for ( const US_Solute& s : sv )
         EXPECT_TRUE( US_Solute::physical_sdv( s ) );
}

TEST_F( TestUSSolute3D, SkvGridKeepsEveryPoint )
{
   // The filter applies only to the s-by-D parameterization: an f/f0 axis is
   // already bounded below by 1, so nothing should be dropped there.
   const int nx = 6, ny = 6, nz = 3;

   QList< QVector< US_Solute > > grid;
   ASSERT_EQ( 2, US_Solute::init_solutes_3d( 1.0e-13, 8.0e-13, nx,
                                             1.0,     4.0,     ny,
                                             0.65,    0.80,    nz,
                                             2, MASK_S_K_V, grid ) );

   EXPECT_EQ( nx * ny * nz, total_solutes( grid ) );
}
