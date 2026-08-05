// test_us_lamm_astfvm.cpp
#include "qt_test_base.h"
#include "us_lamm_astfvm.h"
#include "us_buffer.h"
#include "us_constants.h"
#include "us_dataIO.h"
#include "us_model.h"
#include "us_simparms.h"
#include "us_solvent_exchange.h"
#include <cmath>

static inline double sqr(double v) { return v * v; }

using ::testing::DoubleNear;

//! Solvent exchange ( H/D exchange, ligand binding ) modeling of the ASTFVM
//! Lamm equation solver.  The runs are kept small on purpose: they only have
//! to resolve the boundary well enough to compare sedimentation speeds.
class US_LammAstfvmExchangeTest : public QtTestBase {
protected:
    static constexpr double MENISCUS = 5.90;
    static constexpr double BOTTOM   = 7.20;
    static constexpr int    SPEED    = 45000;
    static constexpr int    DURATION = 3000;  // seconds
    static constexpr int    NSCAN    = 6;

    void SetUp() override {
        QtTestBase::SetUp();
    }

    US_SimulationParameters makeSimparams() {
        US_SimulationParameters sp;
        sp.meniscus          = MENISCUS;
        sp.bottom            = BOTTOM;
        sp.bottom_position   = BOTTOM;
        sp.simpoints         = 100;
        sp.radial_resolution = 0.001;
        sp.temperature       = NORMAL_TEMP;
        sp.meshType          = US_SimulationParameters::ASTFVM;
        sp.band_forming      = false;
        sp.rotorcoeffs[0]    = 0.0;
        sp.rotorcoeffs[1]    = 0.0;

        sp.speed_step[0].rotorspeed = SPEED;
        sp.speed_step[0].duration_hours   = 1;
        sp.speed_step[0].duration_minutes = 0.0;
        sp.speed_step[0].time_first       = 0;
        sp.speed_step[0].time_last        = DURATION;

        // a constant speed time state, one entry per second
        US_SimulationParameters::SimSpeedProf* ssp = &sp.sim_speed_prof[0];
        ssp->rotorspeed   = SPEED;
        ssp->duration     = DURATION;
        ssp->time_b_accel = 0;
        ssp->time_e_accel = 0;
        ssp->time_e_step  = DURATION;
        ssp->rpm_timestate.clear();
        ssp->w2t_timestate.clear();

        const double w2 = sqr( SPEED * M_PI / 30.0 );
        for (int ii = 0; ii <= DURATION; ii++) {
            ssp->rpm_timestate << static_cast<double>(SPEED);
            ssp->w2t_timestate << w2 * static_cast<double>(ii);
        }

        return sp;
    }

    US_DataIO::RawData makeRawData() {
        US_DataIO::RawData rdata;
        rdata.type[0]     = 'R';
        rdata.type[1]     = 'I';
        rdata.cell        = 1;
        rdata.channel     = 'A';
        rdata.description = "exchange test";

        const int npts = static_cast<int>(( BOTTOM - MENISCUS ) / 0.001 ) + 1;
        for (int jj = 0; jj < npts; jj++) {
            rdata.xvalues << MENISCUS + 0.001 * static_cast<double>(jj);
        }

        const double w2   = sqr( SPEED * M_PI / 30.0 );
        const int    tinc = DURATION / NSCAN;

        for (int ii = 1; ii <= NSCAN; ii++) {
            US_DataIO::Scan scan;
            scan.temperature = NORMAL_TEMP;
            scan.rpm         = SPEED;
            scan.seconds     = static_cast<double>(ii * tinc);
            scan.omega2t     = w2 * scan.seconds;
            scan.wavelength  = 280.0;
            scan.plateau     = 0.0;
            scan.delta_r     = 0.001;
            scan.rvalues.fill(0.0, npts);
            rdata.scanData << scan;
        }

        return rdata;
    }

    US_Buffer makeBuffer() {
        US_Buffer buf;
        buf.density         = DENS_20W;
        buf.viscosity       = VISC_20W;
        buf.compressibility = 0.0;
        buf.manual          = false;
        return buf;
    }

    US_Model makeModel(double svalue, const US_SolventExchange& exch) {
        US_Model model;
        US_Model::SimulationComponent sc;
        sc.name                 = "capsid";
        sc.analyteGUID          = "00000000-0000-0000-0000-000000000001";
        sc.vbar20               = 0.70;
        sc.mw                   = 3.7e+06;
        sc.s                    = svalue;
        sc.D                    = 1.5e-07;
        sc.f_f0                 = 1.2;
        sc.signal_concentration = 1.0;
        sc.molar_concentration  = 0.0;
        sc.exchange             = exch;
        model.components << sc;

        return model;
    }

    // run one simulation and return the radial profile of the last scan
    QVector<double> simulate(double svalue, const US_SolventExchange& exch) {
        US_SimulationParameters sp    = makeSimparams();
        US_DataIO::RawData      rdata = makeRawData();
        US_Model                model = makeModel(svalue, exch);

        US_LammAstfvm solver(model, sp);
        solver.setMovieFlag(false);
        solver.set_buffer(makeBuffer());

        EXPECT_EQ(solver.calculate(rdata), 0);

        return rdata.scanData.last().rvalues;
    }

    // radius of the center of mass of a radial profile
    double centerOfMass(const US_DataIO::RawData& rdata, const QVector<double>& conc) {
        double csum = 0.0;
        double rsum = 0.0;

        for (int jj = 0; jj < conc.size(); jj++) {
            csum += conc[jj];
            rsum += conc[jj] * rdata.xvalues[jj];
        }

        return ( csum > 0.0 ) ? ( rsum / csum ) : 0.0;
    }

    // integral of C * r * dr, conserved by the Lamm equation
    double totalMass(const US_DataIO::RawData& rdata, const QVector<double>& conc) {
        double csum = 0.0;

        for (int jj = 1; jj < conc.size(); jj++) {
            csum += ( conc[jj] + conc[jj - 1] ) * 0.5
                  * ( sqr( rdata.xvalues[jj] ) - sqr( rdata.xvalues[jj - 1] ) );
        }

        return csum;
    }

    // a cesium binding description with an occupancy driven excess mass
    US_SolventExchange binding(double occupancy_conc) {
        US_SolventExchange se;
        se.type        = US_SolventExchange::BINDING;
        se.kinetics    = US_SolventExchange::EQUILIBRIUM;
        se.sites       = 3915.0;             // ~14 % excess mass when saturated
        se.dmass       = DMASS_CS;
        se.vbar_bound  = 0.0;
        se.k_d         = 0.10;
        se.ligand_ref  = 1.0;
        se.ligand_conc = occupancy_conc;
        se.theta0      = 0.0;
        return se;
    }
};

// an analyte whose sites are never occupied must sediment exactly like an
// analyte without an exchange description at all
TEST_F(US_LammAstfvmExchangeTest, UnoccupiedExchangeMatchesPlainRun) {
    US_SolventExchange exch = binding(0.0);       // no ligand in the solvent
    exch.kinetics           = US_SolventExchange::FIRST_ORDER;
    exch.rate               = 1.0e-03;

    const QVector<double> plain    = simulate(1.0e-12, US_SolventExchange());
    const QVector<double> unbound  = simulate(1.0e-12, exch);

    ASSERT_EQ(plain.size(), unbound.size());

    double cmax = 0.0;
    for (int jj = 0; jj < plain.size(); jj++) {
        cmax = qMax(cmax, qAbs(plain[jj]));
    }
    ASSERT_GT(cmax, 0.1);

    for (int jj = 0; jj < plain.size(); jj++) {
        EXPECT_NEAR(unbound[jj], plain[jj], 1.0e-10 * cmax);
    }
}

// a saturated analyte must sediment like an analyte whose sedimentation
// coefficient carries the buoyancy of the bound mass
TEST_F(US_LammAstfvmExchangeTest, SaturatedExchangeMatchesEquivalentS) {
    const double svalue = 1.0e-12;
    const double vbar   = 0.70;

    US_SolventExchange exch = binding(1.0e+04);   // far above k_d: theta = 1
    const double excess = exch.sites * exch.dmass / 3.7e+06;
    const double buoy0  = 1.0 - vbar * DENS_20W;
    const double sfact  = ( buoy0 + excess ) / buoy0;

    ASSERT_GT(sfact, 1.10);                       // the effect must be visible

    US_DataIO::RawData rdata = makeRawData();

    const QVector<double> bound  = simulate(svalue,          exch);
    const QVector<double> equiv  = simulate(svalue * sfact,  US_SolventExchange());
    const QVector<double> plain  = simulate(svalue,          US_SolventExchange());

    const double cm_bound = centerOfMass(rdata, bound);
    const double cm_equiv = centerOfMass(rdata, equiv);
    const double cm_plain = centerOfMass(rdata, plain);

    // the saturated analyte moves like the equivalent, faster analyte: the
    // remaining difference is the discretization, the two runs use a different
    // time step and mesh because their sedimentation coefficients differ
    EXPECT_NEAR(cm_bound, cm_equiv, 0.01);

    // ... and clearly faster than the analyte without bound mass
    EXPECT_GT(cm_bound - cm_plain, 0.05);
}

// a finite exchange rate has to place the analyte between the two limits
TEST_F(US_LammAstfvmExchangeTest, KineticsGiveIntermediateSedimentation) {
    const double svalue = 1.0e-12;

    // ten times the dissociation constant: most, but not all, sites occupied
    US_SolventExchange fast = binding(1.0);

    US_SolventExchange slow = fast;
    slow.kinetics = US_SolventExchange::FIRST_ORDER;
    slow.rate     = 3.0e-05;      // k_off; the sites fill up during the run

    // k_obs = k_off * ( 1 + L / k_d ) has to be slow on the time scale of the run
    ASSERT_LT(slow.rate_obs(slow.ligand_conc) * static_cast<double>(DURATION), 1.5);
    ASSERT_GT(slow.rate_obs(slow.ligand_conc) * static_cast<double>(DURATION), 0.5);

    US_DataIO::RawData rdata = makeRawData();

    const QVector<double> immediate = simulate(svalue, fast);
    const QVector<double> gradual   = simulate(svalue, slow);
    const QVector<double> never     = simulate(svalue, US_SolventExchange());

    const double cm_immediate = centerOfMass(rdata, immediate);
    const double cm_gradual   = centerOfMass(rdata, gradual);
    const double cm_never     = centerOfMass(rdata, never);

    EXPECT_GT(cm_gradual, cm_never     + 1.0e-03);
    EXPECT_LT(cm_gradual, cm_immediate - 1.0e-03);
}

// transporting the exchange label must not create or destroy analyte: a
// uniform occupancy has to stay uniform, so the analyte has to behave exactly
// like one whose sedimentation coefficient carries that occupancy
TEST_F(US_LammAstfvmExchangeTest, UniformOccupancyIsTransportedExactly) {
    const double svalue = 1.0e-12;
    const double vbar   = 0.70;

    US_SolventExchange exch = binding(1.0);
    exch.kinetics = US_SolventExchange::FIRST_ORDER;
    exch.rate     = 0.0;         // frozen: the occupancy can only be transported
    exch.theta0   = 0.5;

    const double excess = 0.5 * exch.sites * exch.dmass / 3.7e+06;
    const double buoy0  = 1.0 - vbar * DENS_20W;
    const double sfact  = ( buoy0 + excess ) / buoy0;

    US_DataIO::RawData rdata  = makeRawData();
    US_DataIO::RawData rexch  = makeRawData();
    US_DataIO::RawData requiv = makeRawData();

    {
        US_SimulationParameters sp    = makeSimparams();
        US_Model                model = makeModel(svalue, exch);
        US_LammAstfvm           solver(model, sp);
        solver.setMovieFlag(false);
        solver.set_buffer(makeBuffer());
        ASSERT_EQ(solver.calculate(rexch), 0);
    }
    {
        US_SimulationParameters sp    = makeSimparams();
        US_Model                model = makeModel(svalue * sfact, US_SolventExchange());
        US_LammAstfvm           solver(model, sp);
        solver.setMovieFlag(false);
        solver.set_buffer(makeBuffer());
        ASSERT_EQ(solver.calculate(requiv), 0);
    }

    ASSERT_GT(totalMass(rdata, rexch.scanData[0].rvalues), 0.0);

    double moved = 0.0;

    for (int ii = 0; ii < rexch.scanData.size(); ii++) {
        // an occupancy drifting away from its uniform value would change the
        // sedimentation coefficient and show up as a growing offset here
        const double cm_exch  = centerOfMass(rdata, rexch .scanData[ii].rvalues);
        const double cm_equiv = centerOfMass(rdata, requiv.scanData[ii].rvalues);

        EXPECT_NEAR(cm_exch, cm_equiv, 0.01);

        moved = cm_exch - centerOfMass(rdata, rexch.scanData[0].rvalues);
    }

    ASSERT_GT(moved, 0.1);   // the boundary has to move for this to mean anything
}

// an exchange description supplied by the caller overrides the model
TEST_F(US_LammAstfvmExchangeTest, CallerOverridesTheModelDescription) {
    const double svalue = 1.0e-12;

    US_SimulationParameters sp    = makeSimparams();
    US_DataIO::RawData      rdata = makeRawData();
    US_Model                model = makeModel(svalue, US_SolventExchange());

    US_LammAstfvm solver(model, sp);
    solver.setMovieFlag(false);
    solver.set_buffer(makeBuffer());
    solver.set_solvent_exchange(binding(1.0e+04));

    ASSERT_EQ(solver.calculate(rdata), 0);

    US_DataIO::RawData plain_data = makeRawData();
    const QVector<double> plain   = simulate(svalue, US_SolventExchange());

    EXPECT_GT(centerOfMass(rdata, rdata.scanData.last().rvalues),
              centerOfMass(plain_data, plain) + 1.0e-02);
}

// the co-sedimenting gradient of a single buffer component, interpolated in
// both time and radius, drives the local equilibrium of the exchange
TEST_F(US_LammAstfvmExchangeTest, CosedComponentConcentrationInterpolation) {
    US_LammAstfvm::CosedData cdata;

    US_Model cmodel;
    US_Model::SimulationComponent csc;
    csc.name        = "Deuterium oxide";
    csc.analyteGUID = "00000000-0000-0000-0000-0000000000d2";
    cmodel.components << csc;
    cdata.model    = cmodel;
    cdata.is_empty = false;

    // a linear gradient in radius that doubles between the two scans
    US_DataIO::RawData gdata;
    gdata.xvalues << 5.0 << 6.0 << 7.0;

    for (int ii = 0; ii < 2; ii++) {
        US_DataIO::Scan scan;
        scan.seconds = 100.0 * static_cast<double>(ii + 1);
        scan.rvalues << 1.0 * ( ii + 1 ) << 2.0 * ( ii + 1 ) << 3.0 * ( ii + 1 );
        gdata.scanData << scan;
    }
    cdata.cosed_comp_data[csc.analyteGUID] = gdata;

    double xval[3] = { 5.5, 6.0, 6.5 };
    double conc[3] = { 0.0, 0.0, 0.0 };

    // at the first scan time the profile is reproduced exactly
    ASSERT_TRUE(cdata.InterpolateComp("Deuterium oxide", 3, xval, 100.0, conc));
    EXPECT_THAT(conc[0], DoubleNear(1.5, 1.0e-12));
    EXPECT_THAT(conc[1], DoubleNear(2.0, 1.0e-12));
    EXPECT_THAT(conc[2], DoubleNear(2.5, 1.0e-12));

    // half way between the scans everything is scaled by 1.5
    ASSERT_TRUE(cdata.InterpolateComp("Deuterium oxide", 3, xval, 150.0, conc));
    EXPECT_THAT(conc[0], DoubleNear(1.5 * 1.5, 1.0e-12));
    EXPECT_THAT(conc[1], DoubleNear(2.0 * 1.5, 1.0e-12));
    EXPECT_THAT(conc[2], DoubleNear(2.5 * 1.5, 1.0e-12));

    // beyond the last scan the last profile is held
    ASSERT_TRUE(cdata.InterpolateComp("Deuterium oxide", 3, xval, 1.0e+05, conc));
    EXPECT_THAT(conc[1], DoubleNear(4.0, 1.0e-12));

    // radial positions outside the gradient are clamped to its limits
    double xout[2] = { 1.0, 9.0 };
    double cout[2] = { 0.0, 0.0 };
    ASSERT_TRUE(cdata.InterpolateComp("Deuterium oxide", 2, xout, 100.0, cout));
    EXPECT_THAT(cout[0], DoubleNear(1.0, 1.0e-12));
    EXPECT_THAT(cout[1], DoubleNear(3.0, 1.0e-12));

    // an unknown component leaves the caller to fall back to a uniform solvent
    EXPECT_FALSE(cdata.InterpolateComp("Sucrose", 3, xval, 100.0, conc));
}
