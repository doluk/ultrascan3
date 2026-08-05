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

    // Build a grid of nv points spanning the cell
    std::vector< double > grid( int nv ) const {
        std::vector< double > x( nv );
        for ( int jj = 0; jj < nv; jj++ ) {
            x[ jj ] = meniscus + span * jj / ( nv - 1 );
        }
        return x;
    }

    US_GridControl::Config cfg;
    double meniscus, bottom, span, base_h;
    double omega2, sedcoef, diffcoef, velocity, dt_char;
};

// ------------------------------------------------------------ front_scale --

TEST_F(US_GridControlTest, FrontScaleOfAConstantProfileIsInfinite) {
    const std::vector< double > x = grid( 11 );
    const std::vector< double > u( 11, 3.0 );

    EXPECT_FALSE( std::isfinite( US_GridControl::front_scale( x.data(), 11, u.data() ) ) );
}

TEST_F(US_GridControlTest, FrontScaleOfALinearProfileIsTheColumnLength) {
    const std::vector< double > x = grid( 11 );
    std::vector< double >       u( 11 );

    for ( int jj = 0; jj < 11; jj++ ) {
        u[ jj ] = 2.0 * x[ jj ];
    }

    // range / max slope = 2*span / 2 = span
    EXPECT_THAT( US_GridControl::front_scale( x.data(), 11, u.data() ),
                 DoubleNear( span, 1.0e-12 ) );
}

TEST_F(US_GridControlTest, FrontScaleOfAStepIsOneCell) {
    const std::vector< double > x = grid( 11 );
    std::vector< double >       u( 11, 0.0 );

    for ( int jj = 0; jj < 5; jj++ ) {
        u[ jj ] = 1.0;
    }

    EXPECT_THAT( US_GridControl::front_scale( x.data(), 11, u.data() ),
                 DoubleNear( span / 10.0, 1.0e-12 ) );
}

TEST_F(US_GridControlTest, FrontScaleIsIndependentOfTheProfileAmplitude) {
    const std::vector< double > x = grid( 21 );
    std::vector< double >       weak( 21, 0.0 );
    std::vector< double >       loud( 21, 0.0 );

    for ( int jj = 0; jj < 8; jj++ ) {
        weak[ jj ] = 1.0e-5;
        loud[ jj ] = 1.0e+3;
    }

    EXPECT_THAT( US_GridControl::front_scale( x.data(), 21, weak.data() ),
                 DoubleNear( US_GridControl::front_scale( x.data(), 21, loud.data() ),
                             1.0e-12 ) );
}

// The narrowest feature must win no matter where in the cell it sits, so that
// a boundary at the bottom is treated exactly like a band at the meniscus.
TEST_F(US_GridControlTest, FrontScaleFindsTheNarrowestOfSeveralFeatures) {
    const std::vector< double > x = grid( 41 );
    std::vector< double >       u( 41, 0.0 );

    for ( int jj = 4; jj < 12; jj++ ) {         // wide feature near the meniscus
        u[ jj ] = 1.0 * ( jj - 3 ) / 8.0;
    }
    for ( int jj = 12; jj < 30; jj++ ) {
        u[ jj ] = 1.0;
    }
    u[ 30 ] = 0.0;                              // sharp edge near the bottom

    const double cell = span / 40.0;
    EXPECT_THAT( US_GridControl::front_scale( x.data(), 41, u.data() ),
                 DoubleNear( cell, 1.0e-12 ) );
}

TEST_F(US_GridControlTest, FrontScaleHonorsTheNodeStride) {
    // Piecewise quadratic storage: node values at even indices, midpoints odd
    const std::vector< double > x = grid( 5 );
    std::vector< double >       u( 9, 0.0 );

    for ( int jj = 0; jj < 2; jj++ ) {
        u[ jj * 2 ] = 1.0;
    }

    EXPECT_THAT( US_GridControl::front_scale( x.data(), 5, u.data(), 2 ),
                 DoubleNear( span / 4.0, 1.0e-12 ) );
}

// ------------------------------------------------------------- magnitude ---

TEST_F(US_GridControlTest, MagnitudeIsTheValueRange) {
    const double u[ 4 ] = { 1.0, -2.0, 3.5, 0.0 };

    EXPECT_THAT( US_GridControl::magnitude( u, 4 ), DoubleNear( 5.5, 1.0e-12 ) );
}

// ------------------------------------------------------------ min_spacing --

TEST_F(US_GridControlTest, MinSpacingFindsTheNarrowestCell) {
    const double x[ 5 ] = { 5.80, 5.90, 5.91, 6.20, 6.90 };

    EXPECT_THAT( US_GridControl::min_spacing( x, 5 ), DoubleNear( 0.01, 1.0e-12 ) );
}

// ------------------------------------------------------------- resolution --

TEST_F(US_GridControlTest, NoMeasurableFeatureKeepsTheCharacteristicStep) {
    EXPECT_THAT( US_GridControl::step_for_feature( dt_char, qInf(), velocity,
                                                   diffcoef, cfg ),
                 DoubleNear( dt_char, 1.0e-12 ) );
}

TEST_F(US_GridControlTest, WideFeatureKeepsTheCharacteristicStep) {
    EXPECT_THAT( US_GridControl::step_for_feature( dt_char, span, velocity,
                                                   diffcoef, cfg ),
                 DoubleNear( dt_char, 1.0e-12 ) );
}

TEST_F(US_GridControlTest, DiscontinuityShrinksTheStepToTheAllowedFloor) {
    const double dt = US_GridControl::step_for_feature( dt_char, 0.0, velocity,
                                                        diffcoef, cfg );

    EXPECT_THAT( dt, DoubleNear( dt_char / cfg.max_reduction, 1.0e-12 ) );
}

TEST_F(US_GridControlTest, StepRespectsTheRingingBoundOfTheFeature) {
    // Wide enough that the bound bites before the reduction floor does
    const double feature = 1.5e-2;
    const double dt      = US_GridControl::step_for_feature( dt_char, feature,
                                                             velocity, diffcoef, cfg );

    ASSERT_GT( dt, dt_char / cfg.max_reduction );
    EXPECT_LE( diffcoef * dt / ( feature * feature ), cfg.ring_factor + 1.0e-12 );
    EXPECT_LE( velocity * dt / feature, cfg.courant + 1.0e-12 );
    EXPECT_LT( dt, dt_char );
}

// The floor is deliberately allowed to override the bounds: a feature narrower
// than any admissible step can carry would otherwise drive dt to zero.
TEST_F(US_GridControlTest, ReductionFloorOverridesTheBounds) {
    const double feature = 1.0e-4;
    const double dt      = US_GridControl::step_for_feature( dt_char, feature,
                                                             velocity, diffcoef, cfg );

    EXPECT_THAT( dt, DoubleNear( dt_char / cfg.max_reduction, 1.0e-12 ) );
    EXPECT_GT( diffcoef * dt / ( feature * feature ), cfg.ring_factor );
}

// Only the magnitude of the transport speed may enter, so a floating species
// has to be limited exactly like a sedimenting one.
TEST_F(US_GridControlTest, StepIsIndependentOfTheSignOfS) {
    const double v_pos = US_GridControl::front_velocity(  sedcoef, omega2, bottom );
    const double v_neg = US_GridControl::front_velocity( -sedcoef, omega2, bottom );

    EXPECT_THAT( v_neg, DoubleNear( v_pos, 1.0e-24 ) );
    EXPECT_THAT( US_GridControl::step_for_feature( dt_char, 4.0e-3, v_neg, diffcoef, cfg ),
                 DoubleNear( US_GridControl::step_for_feature( dt_char, 4.0e-3, v_pos,
                                                               diffcoef, cfg ),
                             1.0e-12 ) );
}

TEST_F(US_GridControlTest, TransportBoundBitesWhenDiffusionIsNegligible) {
    // A large, fast species: no diffusion to speak of, so the Courant bound
    // is the one that has to hold
    const double feature = 1.0e-3;
    const double dt      = US_GridControl::step_for_feature( dt_char, feature,
                                                             velocity, 0.0, cfg );

    EXPECT_LE( velocity * dt / feature, cfg.courant + 1.0e-12 );
}

TEST_F(US_GridControlTest, StepAndCellSizeAreMutuallyConsistent) {
    const double dt = US_GridControl::step_for_feature( dt_char, 0.0, velocity,
                                                        diffcoef, cfg );
    const double h  = US_GridControl::min_cell( dt, velocity, diffcoef, cfg );

    // The cell size must span the narrowest feature that step can carry
    const double width = US_GridControl::resolvable_feature( dt, velocity,
                                                             diffcoef, cfg );
    EXPECT_THAT( h * cfg.feature_points, DoubleNear( width, 1.0e-12 ) );

    // ... and that width must be one the step is actually allowed to take
    EXPECT_THAT( US_GridControl::step_for_feature( dt_char, width, velocity,
                                                   diffcoef, cfg ),
                 DoubleNear( dt, 1.0e-9 ) );
}

TEST_F(US_GridControlTest, ResolutionRelaxesAsAFrontSpreads) {
    double previous = 0.0;

    for ( double feature = 1.0e-4; feature < span; feature *= 2.0 ) {
        const double dt = US_GridControl::step_for_feature( dt_char, feature,
                                                            velocity, diffcoef, cfg );
        EXPECT_GE( dt, previous );
        EXPECT_LE( dt, dt_char );
        previous = dt;
    }

    EXPECT_THAT( previous, DoubleNear( dt_char, 1.0e-12 ) );
}

// A diffusively spreading front costs only a logarithmic number of steps,
// because both bounds grow with the square of its width.
TEST_F(US_GridControlTest, ASpreadingFrontCostsOnlyALogarithmicNumberOfSteps) {
    double dt    = US_GridControl::step_for_feature( dt_char, 0.0, velocity,
                                                     diffcoef, cfg );
    double t     = 0.0;
    int    steps = 0;

    while ( t < 9000.0  &&  steps < 1000000 ) {
        const double feature = sqrt( 2.0 * diffcoef * qMax( t, 1.0 ) );
        const double want    = US_GridControl::step_for_feature( dt_char, feature,
                                                                 velocity, diffcoef, cfg );
        dt = US_GridControl::relax( dt, want, cfg );
        t += dt;
        steps++;
    }

    // A fixed characteristic step would need about 290; staying below four
    // times that is what makes the controller affordable by default
    EXPECT_LT( steps, 4 * (int)( 9000.0 / dt_char ) );
}

TEST_F(US_GridControlTest, DisabledControllerReproducesTheLegacyResolution) {
    cfg.enabled = false;

    EXPECT_THAT( US_GridControl::step_for_feature( dt_char, 0.0, velocity,
                                                   diffcoef, cfg ),
                 DoubleNear( dt_char, 1.0e-12 ) );
    EXPECT_THAT( US_GridControl::min_cell( 1.0, velocity, diffcoef, cfg ),
                 DoubleNear( 0.0, 1.0e-15 ) );

    QVector< double > lo;
    QVector< double > hi;
    const std::vector< double > x = grid( 11 );
    const std::vector< double > u( 11, 1.0 );
    EXPECT_EQ( US_GridControl::steep_regions( x.data(), 11, u.data(), 1,
                                              1.0e-3, 0.0, cfg, lo, hi ), 0 );
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

// --------------------------------------------------------- steep_regions ---

TEST_F(US_GridControlTest, SteepRegionsFindAFeatureWhereverItSits) {
    QVector< double > lo;
    QVector< double > hi;
    const std::vector< double > x = grid( 41 );

    for ( int edge = 1; edge < 40; edge += 13 ) {
        std::vector< double > u( 41, 0.0 );

        for ( int jj = 0; jj <= edge; jj++ ) {
            u[ jj ] = 1.0;
        }

        ASSERT_EQ( US_GridControl::steep_regions( x.data(), 41, u.data(), 1,
                                                  span / 400.0, 0.0, cfg, lo, hi ), 1 );
        EXPECT_LE( lo[ 0 ], x[ edge ] );
        EXPECT_GE( hi[ 0 ], x[ edge ] );
    }
}

TEST_F(US_GridControlTest, SteepRegionsReportEveryFeature) {
    QVector< double > lo;
    QVector< double > hi;
    const std::vector< double > x = grid( 61 );
    std::vector< double >       u( 61, 0.0 );

    for ( int jj = 5; jj < 10; jj++ ) {         // a block near the meniscus
        u[ jj ] = 1.0;
    }
    for ( int jj = 45; jj < 50; jj++ ) {        // and another near the bottom
        u[ jj ] = 1.0;
    }

    // Four edges, far enough apart to stay four separate regions
    EXPECT_EQ( US_GridControl::steep_regions( x.data(), 61, u.data(), 1,
                                              span / 600.0, 0.0, cfg, lo, hi ), 4 );
    EXPECT_LT( hi[ 0 ], lo[ 3 ] );
}

TEST_F(US_GridControlTest, SteepRegionsMergeWhenPaddingOverlaps) {
    QVector< double > lo;
    QVector< double > hi;
    const std::vector< double > x = grid( 61 );
    std::vector< double >       u( 61, 0.0 );

    for ( int jj = 5; jj < 10; jj++ ) {
        u[ jj ] = 1.0;
    }
    for ( int jj = 45; jj < 50; jj++ ) {
        u[ jj ] = 1.0;
    }

    EXPECT_EQ( US_GridControl::steep_regions( x.data(), 61, u.data(), 1,
                                              span / 600.0, span, cfg, lo, hi ), 1 );
    EXPECT_THAT( lo[ 0 ], DoubleNear( x[ 0 ],  1.0e-12 ) );
    EXPECT_THAT( hi[ 0 ], DoubleNear( x[ 60 ], 1.0e-12 ) );
}

TEST_F(US_GridControlTest, SteepRegionsIgnoreAProfileTheGridAlreadyResolves) {
    QVector< double > lo;
    QVector< double > hi;
    const std::vector< double > x = grid( 41 );
    std::vector< double >       u( 41 );

    for ( int jj = 0; jj < 41; jj++ ) {
        u[ jj ] = x[ jj ];                      // linear: nothing to resolve
    }

    EXPECT_EQ( US_GridControl::steep_regions( x.data(), 41, u.data(), 1,
                                              span / 400.0, 0.0, cfg, lo, hi ), 0 );
}

// --------------------------------------------------------- points_for_span --

TEST_F(US_GridControlTest, PointsForSpanCoversTheRegion) {
    EXPECT_EQ( US_GridControl::points_for_span( 1.0, 0.1 ), 11 );
    EXPECT_EQ( US_GridControl::points_for_span( 0.0, 0.1 ), 2 );
    EXPECT_EQ( US_GridControl::points_for_span( 1.0, 0.0 ), 2 );
}

// ---------------------------------------------------------- lamella_width --

TEST_F(US_GridControlTest, LamellaWidthMatchesSectorVolume) {
    const double width = US_GridControl::lamella_width( 5.8, 0.015, 1.2, 2.5 );
    const double vol   = ( 2.5 / 360.0 ) * M_PI * 1.2
                       * ( std::pow( 5.8 + width, 2.0 ) - std::pow( 5.8, 2.0 ) );

    EXPECT_THAT( vol, DoubleNear( 0.015, 1.0e-12 ) );
}

TEST_F(US_GridControlTest, LamellaWidthFallsBackOnCenterpieceDefaults) {
    EXPECT_THAT( US_GridControl::lamella_width( 5.8, 0.015, 0.0, 0.0 ),
                 DoubleNear( US_GridControl::lamella_width( 5.8, 0.015, 1.2, 2.5 ),
                             1.0e-12 ) );
}

TEST_F(US_GridControlTest, LamellaWidthIsZeroWithoutABand) {
    EXPECT_THAT( US_GridControl::lamella_width( 5.8, 0.0, 1.2, 2.5 ),
                 DoubleNear( 0.0, 1.0e-15 ) );
}
