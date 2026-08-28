// test_us_math2_nnls.cpp - Tests for US_Math2::nnls
//
// The non-negative least squares solver underlies every UltraScan spectrum
// method -- 2DSA, 3DSA, PCSA, GA -- and had no direct coverage.
//
// Background: the secondary loop of the Lawson & Hanson algorithm removes
// coefficients from the active set one at a time, re-checking feasibility
// after each removal.  The feasibility flag has to be reset before each
// re-check; set once before the loop instead, it can only ever go from 1 to
// 0, so a single round-off induced removal makes the loop run until the
// set-P counter and the index cursor fall below zero and the routine writes
// outside its index array.
//
// The direct reproducer for that is not here.  Twenty thousand randomly
// generated near-rank-deficient systems fail to reach the path; it needs the
// particular structure of real Lamm-equation columns -- smooth, positive and
// strongly correlated.  TestUS3dsaProcess.ProducesAStandardSpaceModel in
// test_us_3dsa_process.cpp segfaults without the fix and is the regression
// test that matters.  What follows is the general coverage the routine
// previously lacked entirely.

#include "qt_test_base.h"

#include "us_math2.h"

#include <cmath>
#include <random>
#include <vector>

namespace {

// nnls() consumes its inputs, so every call gets fresh copies.  The matrix is
// column-major with leading dimension nrows.
struct Problem
{
   int                   nrows;
   int                   ncols;
   std::vector< double > a;
   std::vector< double > b;
};

// Solve, returning the coefficients.  a and b are copied first.
std::vector< double > solve( const Problem& p, int* status = nullptr )
{
   std::vector< double > a = p.a;
   std::vector< double > b = p.b;
   std::vector< double > x( p.ncols, -1.0 );

   const int rc = US_Math2::nnls( a.data(), p.nrows, p.nrows, p.ncols,
                                  b.data(), x.data() );

   if ( status != nullptr )  *status = rc;

   return x;
}

double residual_norm( const Problem& p, const std::vector< double >& x )
{
   double sum = 0.0;

   for ( int rr = 0; rr < p.nrows; rr++ )
   {
      double fit = 0.0;

      for ( int cc = 0; cc < p.ncols; cc++ )
         fit += p.a[ rr + cc * p.nrows ] * x[ cc ];

      sum += ( p.b[ rr ] - fit ) * ( p.b[ rr ] - fit );
   }

   return std::sqrt( sum );
}

double vector_norm( const std::vector< double >& v )
{
   double sum = 0.0;
   for ( double e : v )  sum += e * e;
   return std::sqrt( sum );
}

} // namespace

class TestUSMath2Nnls : public QtTestBase {};

// ---------------------------------------------------------------------------
TEST_F( TestUSMath2Nnls, SolvesAWellConditionedProblem )
{
   // Two orthogonal columns and an exactly representable right-hand side.
   Problem p;
   p.nrows = 4;
   p.ncols = 2;
   p.a = { 1.0, 1.0, 0.0, 0.0,      // column 0
           0.0, 0.0, 1.0, 1.0 };    // column 1
   p.b = { 3.0, 3.0, 5.0, 5.0 };    // 3 * col0 + 5 * col1

   const std::vector< double > x = solve( p );

   ASSERT_EQ( 2u, x.size() );
   EXPECT_NEAR( 3.0, x[ 0 ], 1.0e-10 );
   EXPECT_NEAR( 5.0, x[ 1 ], 1.0e-10 );
   EXPECT_LT( residual_norm( p, x ), 1.0e-10 );
}

TEST_F( TestUSMath2Nnls, EnforcesNonNegativity )
{
   // The unconstrained least-squares answer needs a negative coefficient on
   // the second column, so the constrained optimum must zero it.
   Problem p;
   p.nrows = 4;
   p.ncols = 2;
   p.a = { 1.0, 1.0, 0.0, 0.0,
           0.0, 0.0, 1.0, 1.0 };
   p.b = { 2.0, 2.0, -4.0, -4.0 };

   const std::vector< double > x = solve( p );

   ASSERT_EQ( 2u, x.size() );
   EXPECT_NEAR( 2.0, x[ 0 ], 1.0e-10 );
   EXPECT_DOUBLE_EQ( 0.0, x[ 1 ] );

   for ( double e : x )
      EXPECT_GE( e, 0.0 );
}

TEST_F( TestUSMath2Nnls, NeverReturnsANegativeCoefficient )
{
   std::mt19937 rng( 20240828u );
   std::uniform_real_distribution< double > uni( -1.0, 1.0 );

   for ( int trial = 0; trial < 40; trial++ )
   {
      Problem p;
      p.nrows = 30;
      p.ncols = 8;
      p.a.resize( p.nrows * p.ncols );
      p.b.resize( p.nrows );

      for ( double& e : p.a )  e = uni( rng );
      for ( double& e : p.b )  e = uni( rng );

      const std::vector< double > x = solve( p );

      for ( int cc = 0; cc < p.ncols; cc++ )
      {
         EXPECT_GE( x[ cc ], 0.0 ) << "trial " << trial << " column " << cc;
         EXPECT_TRUE( std::isfinite( x[ cc ] ) );
      }
   }
}

// ---------------------------------------------------------------------------
// Near-duplicate columns are what a 3DSA grid produces: the forward model
// reads only (s*, D*), so many grid points map to nearly the same column.
// These do not by themselves reach the removal path that was broken (see the
// note at the top of this file), but they are the regime the solver has to
// stay sane in.
// ---------------------------------------------------------------------------
TEST_F( TestUSMath2Nnls, StaysSaneOnNearRankDeficientSystems )
{
   std::mt19937 rng( 12345u );
   std::uniform_real_distribution< double > uni( 0.0, 1.0 );

   for ( int trial = 0; trial < 60; trial++ )
   {
      const int nrows = 40;
      const int ncols = 12;

      Problem p;
      p.nrows = nrows;
      p.ncols = ncols;
      p.a.assign( nrows * ncols, 0.0 );
      p.b.assign( nrows, 0.0 );

      // Three genuine directions, each cloned into four columns that differ
      // only by a perturbation far below the scale of the data.
      std::vector< std::vector< double > > basis( 3,
                                                  std::vector< double >( nrows ) );
      for ( int kk = 0; kk < 3; kk++ )
         for ( int rr = 0; rr < nrows; rr++ )
            basis[ kk ][ rr ] = uni( rng ) + 0.25;

      for ( int cc = 0; cc < ncols; cc++ )
      {
         const std::vector< double >& src = basis[ cc % 3 ];

         for ( int rr = 0; rr < nrows; rr++ )
            p.a[ rr + cc * nrows ] = src[ rr ] * ( 1.0 + 1.0e-9 * uni( rng ) )
                                     + 1.0e-10 * uni( rng );
      }

      // A right-hand side inside the cone, plus a little noise so the exact
      // solution is not attainable and the active set has to move.
      for ( int rr = 0; rr < nrows; rr++ )
         p.b[ rr ] = 0.7 * basis[ 0 ][ rr ] + 1.3 * basis[ 2 ][ rr ]
                     + 1.0e-3 * uni( rng );

      int status = -99;
      const std::vector< double > x = solve( p, &status );

      // It has to come back at all, and come back with a usable answer.
      ASSERT_EQ( (size_t)ncols, x.size() ) << "trial " << trial;

      for ( int cc = 0; cc < ncols; cc++ )
      {
         ASSERT_TRUE( std::isfinite( x[ cc ] ) )
            << "trial " << trial << " column " << cc;
         EXPECT_GE( x[ cc ], 0.0 ) << "trial " << trial << " column " << cc;
      }

      // And it has to be a fit, not just any feasible point: the zero vector
      // is always feasible, so beating it is the weakest real requirement.
      EXPECT_LT( residual_norm( p, x ), vector_norm( p.b ) )
         << "trial " << trial << " status " << status;
   }
}
