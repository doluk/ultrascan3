// test_us_grid_control.cpp
#include "qt_test_base.h"
#include "us_constants.h"
#include "us_grid_control.h"

#include <cmath>
#include <vector>

using ::testing::DoubleNear;

class US_GridControlTest : public QtTestBase {
protected:
    void SetUp() override {
        QtTestBase::SetUp();

        meniscus = 5.8;
        bottom   = 6.9;
        span     = bottom - meniscus;
        base_h   = span / 199.0;                        // 200 simulation points
        omega2   = std::pow( 45000.0 * M_PI / 30.0, 2.0 );
        sedcoef  = 5.0e-13;
        diffcoef = 5.0e-07;
        velocity = US_GridControl::front_velocity( sedcoef, omega2, bottom );
        dt_char  = std::log( bottom / meniscus ) / ( omega2 * sedcoef * 200.0 );
    }

    US_GridControl::Config cfg;
    double meniscus, bottom, span, base_h;
    double omega2, sedcoef, diffcoef, velocity, dt_char;
};

// ---------------------------------------------------------------- geometry --

TEST_F(US_GridControlTest, LamellaWidthMatchesSectorVolume) {
    // A 15 ul band in a 2.5 degree, 1.2 cm centerpiece
    const double width = US_GridControl::lamella_width( 5.8, 0.015, 1.2, 2.5 );

    // Invert: the sector volume between meniscus and meniscus+width
    const double vol = ( 2.5 / 360.0 ) * M_PI * 1.2
                     * ( std::pow( 5.8 + width, 2.0 ) - std::pow( 5.8, 2.0 ) );

    EXPECT_THAT( vol, DoubleNear( 0.015, 1.0e-12 ) );
    EXPECT_GT( width, 0.0 );
    EXPECT_LT( width, 0.1 );
}

TEST_F(US_GridControlTest, LamellaWidthFallsBackOnCenterpieceDefaults) {
    EXPECT_THAT( US_GridControl::lamella_width( 5.8, 0.015, 0.0, 0.0 ),
                 DoubleNear( US_GridControl::lamella_width( 5.8, 0.015, 1.2, 2.5 ),
                             1.0e-12 ) );
}

TEST_F(US_GridControlTest, LamellaWidthIsZeroWithoutABand) {
    EXPECT_THAT( US_GridControl::lamella_width( 5.8, 0.0, 1.2, 2.5 ),
                 DoubleNear( 0.0, 1.0e-15 ) );
    EXPECT_THAT( US_GridControl::lamella_width( 0.0, 0.015, 1.2, 2.5 ),
                 DoubleNear( 0.0, 1.0e-15 ) );
}

// ------------------------------------------------------------ front_scale --

TEST_F(US_GridControlTest, FrontScaleOfAConstantProfileIsInfinite) {
    std::vector< double > x( 11 );
    std::vector< double > u( 11, 3.0 );

    for ( int jj = 0; jj < 11; jj++ ) {
        x[ jj ] = 5.8 + 0.1 * jj;
    }

    EXPECT_FALSE( std::isfinite( US_GridControl::front_scale( x.data(), 11, u.data() ) ) );
}

TEST_F(US_GridControlTest, FrontScaleOfALinearProfileIsTheColumnLength) {
    std::vector< double > x( 11 );
    std::vector< double > u( 11 );

    for ( int jj = 0; jj < 11; jj++ ) {
        x[ jj ] = 5.8 + 0.1 * jj;
        u[ jj ] = 2.0 * x[ jj ];
    }

    // range / max slope = 2*(x_n-x_0) / 2 = column length
    EXPECT_THAT( US_GridControl::front_scale( x.data(), 11, u.data() ),
                 DoubleNear( 1.0, 1.0e-12 ) );
}

TEST_F(US_GridControlTest, FrontScaleOfAStepIsOneCell) {
    std::vector< double > x( 11 );
    std::vector< double > u( 11, 0.0 );

    for ( int jj = 0; jj < 11; jj++ ) {
        x[ jj ] = 5.8 + 0.1 * jj;
    }

    for ( int jj = 0; jj < 5; jj++ ) {
        u[ jj ] = 1.0;
    }

    EXPECT_THAT( US_GridControl::front_scale( x.data(), 11, u.data() ),
                 DoubleNear( 0.1, 1.0e-12 ) );
}

TEST_F(US_GridControlTest, FrontScaleHonorsTheNodeStride) {
    // Piecewise quadratic storage: node values at even indices, midpoints odd
    std::vector< double > x( 5 );
    std::vector< double > u( 9, 0.0 );

    for ( int jj = 0; jj < 5; jj++ ) {
        x[ jj ]      = 5.8 + 0.1 * jj;
        u[ jj * 2 ]  = ( jj < 2 ) ? 1.0 : 0.0;
    }

    EXPECT_THAT( US_GridControl::front_scale( x.data(), 5, u.data(), 2 ),
                 DoubleNear( 0.1, 1.0e-12 ) );
}

// ------------------------------------------------------------ min_spacing --

TEST_F(US_GridControlTest, MinSpacingFindsTheNarrowestCell) {
    const double x[ 5 ] = { 5.80, 5.90, 5.91, 6.20, 6.90 };

    EXPECT_THAT( US_GridControl::min_spacing( x, 5 ), DoubleNear( 0.01, 1.0e-12 ) );
}

// ------------------------------------------------------------- resolution --

TEST_F(US_GridControlTest, SmoothProfileKeepsTheCharacteristicStep) {
    // A feature much wider than the grid can resolve is nothing to chase
    const double dt = US_GridControl::step_for_feature( dt_char, span, span,
                                                        base_h, velocity,
                                                        diffcoef, cfg );

    EXPECT_THAT( dt, DoubleNear( dt_char, 1.0e-12 ) );
}

TEST_F(US_GridControlTest, NoMeasurableFeatureKeepsTheCharacteristicStep) {
    const double dt = US_GridControl::step_for_feature( dt_char, qInf(), span,
                                                        base_h, velocity,
                                                        diffcoef, cfg );

    EXPECT_THAT( dt, DoubleNear( dt_char, 1.0e-12 ) );
}

TEST_F(US_GridControlTest, DiscontinuityShrinksTheStepToTheAllowedFloor) {
    const double dt = US_GridControl::step_for_feature( dt_char, 0.0, span,
                                                        base_h, velocity,
                                                        diffcoef, cfg );

    EXPECT_LT( dt, dt_char );
    EXPECT_THAT( dt, DoubleNear( dt_char / cfg.max_reduction, 1.0e-9 ) );
}

TEST_F(US_GridControlTest, StepAndCellSizeAreMutuallyConsistent) {
    const double dt = US_GridControl::step_for_feature( dt_char, 0.0, span,
                                                        base_h, velocity,
                                                        diffcoef, cfg );
    const double h  = US_GridControl::min_cell( dt, velocity, diffcoef, cfg );

    // Both the Courant number and the diffusion number must be within budget
    EXPECT_LE( velocity * dt / h, cfg.courant + 1.0e-9 );
    EXPECT_LE( diffcoef * dt / ( h * h ), cfg.diff_number + 1.0e-9 );

    // ... and the cell must still be finer than the unrefined grid, otherwise
    // the controller would have had no reason to shrink the step
    EXPECT_LT( h, base_h );
}

TEST_F(US_GridControlTest, ResolutionRelaxesAsAFrontSpreads) {
    double previous = 0.0;

    for ( double feature = 2.0 * base_h; feature < span; feature *= 2.0 ) {
        const double dt = US_GridControl::step_for_feature( dt_char, feature, span,
                                                            base_h, velocity,
                                                            diffcoef, cfg );
        EXPECT_GE( dt, previous );
        EXPECT_LE( dt, dt_char );
        previous = dt;
    }

    EXPECT_THAT( previous, DoubleNear( dt_char, 1.0e-12 ) );
}

TEST_F(US_GridControlTest, DisabledControllerReproducesTheLegacyResolution) {
    cfg.enabled = false;

    EXPECT_THAT( US_GridControl::step_for_feature( dt_char, 0.0, span, base_h,
                                                   velocity, diffcoef, cfg ),
                 DoubleNear( dt_char, 1.0e-12 ) );
    EXPECT_THAT( US_GridControl::min_cell( 1.0, velocity, diffcoef, cfg ),
                 DoubleNear( 0.0, 1.0e-15 ) );
    EXPECT_THAT( US_GridControl::target_cell( 0.0, span, base_h, cfg ),
                 DoubleNear( base_h, 1.0e-15 ) );
}

// ------------------------------------------------------------------ relax --

TEST_F(US_GridControlTest, RelaxShrinksAtOnceAndGrowsGradually) {
    EXPECT_THAT( US_GridControl::relax( 10.0, 1.0, cfg ), DoubleNear( 1.0, 1.0e-12 ) );
    EXPECT_THAT( US_GridControl::relax( 1.0, 10.0, cfg ),
                 DoubleNear( cfg.growth, 1.0e-12 ) );
    EXPECT_THAT( US_GridControl::relax( 1.0, 1.01, cfg ), DoubleNear( 1.01, 1.0e-12 ) );
}

TEST_F(US_GridControlTest, RelaxationReachesTheTargetInABoundedNumberOfSteps) {
    double dt    = 1.0;
    int    steps = 0;

    while ( dt < ( dt_char - 1.0e-9 )  &&  steps < 10000 ) {
        dt = US_GridControl::relax( dt, dt_char, cfg );
        steps++;
    }

    EXPECT_THAT( dt, DoubleNear( dt_char, 1.0e-9 ) );
    EXPECT_LT( steps, 200 );
}

// --------------------------------------------------------- points_for_span --

TEST_F(US_GridControlTest, PointsForSpanCoversTheRegion) {
    EXPECT_EQ( US_GridControl::points_for_span( 1.0, 0.1 ), 11 );
    EXPECT_EQ( US_GridControl::points_for_span( 0.0, 0.1 ), 2 );
    EXPECT_EQ( US_GridControl::points_for_span( 1.0, 0.0 ), 2 );
}
