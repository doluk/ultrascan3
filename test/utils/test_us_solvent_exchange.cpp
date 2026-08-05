// test_us_solvent_exchange.cpp
#include "qt_test_base.h"
#include "us_solvent_exchange.h"
#include "us_model.h"
#include "us_constants.h"
#include <QXmlStreamReader>
#include <QXmlStreamWriter>
#include <cmath>

using ::testing::DoubleNear;

class US_SolventExchangeTest : public QtTestBase {
protected:
    void SetUp() override {
        QtTestBase::SetUp();
        exch = US_SolventExchange();
    }

    US_SolventExchange exch;

    // an H/D exchange of a 50 kDa protein with 350 exchangeable hydrogens
    US_SolventExchange hdx() {
        US_SolventExchange se;
        se.type        = US_SolventExchange::ISOTOPE;
        se.kinetics    = US_SolventExchange::FIRST_ORDER;
        se.ligand      = "Deuterium oxide";
        se.sites       = 350.0;
        se.dmass       = DMASS_H2D;
        se.vbar_bound  = 0.0;
        se.rate        = 0.01;
        se.k_d         = 1.0;
        se.ligand_ref  = CONC_WATER;
        return se;
    }

    // cesium ions binding to a capsid with 10000 binding sites
    US_SolventExchange csbind() {
        US_SolventExchange se;
        se.type        = US_SolventExchange::BINDING;
        se.kinetics    = US_SolventExchange::FIRST_ORDER;
        se.ligand      = "Cesium chloride";
        se.sites       = 10000.0;
        se.dmass       = DMASS_CS;
        se.vbar_bound  = 0.0;
        se.rate        = 0.5;      // k_off
        se.k_d         = 0.5;      // 0.5 M
        se.ligand_ref  = 1.0;      // work in molar units
        return se;
    }
};

TEST_F(US_SolventExchangeTest, DefaultIsInactive) {
    EXPECT_EQ(exch.type, US_SolventExchange::NONE);
    EXPECT_FALSE(exch.active());
    EXPECT_FALSE(exch.preequilibrated());

    // an inactive description must leave s and D untouched
    EXPECT_THAT(exch.buoyancy(1.0, 1.2, 50000.0, 0.72),
                DoubleNear(1.0 - 0.72 * 1.2, 1.0e-12));
    EXPECT_THAT(exch.friction_ratio(1.0, 50000.0, 0.72), DoubleNear(1.0, 1.0e-12));
    EXPECT_THAT(exch.mass_ratio(1.0, 50000.0), DoubleNear(1.0, 1.0e-12));
}

TEST_F(US_SolventExchangeTest, ActiveNeedsSites) {
    exch = hdx();
    EXPECT_TRUE(exch.active());

    exch.sites = 0.0;
    EXPECT_FALSE(exch.active());
}

TEST_F(US_SolventExchangeTest, IsotopeEquilibriumFollowsSolventFraction) {
    exch = hdx();

    // without fractionation the occupancy equals the solvent isotope fraction
    EXPECT_THAT(exch.occupancy_eq(0.0),               DoubleNear(0.0,  1.0e-12));
    EXPECT_THAT(exch.occupancy_eq(0.25 * CONC_WATER), DoubleNear(0.25, 1.0e-12));
    EXPECT_THAT(exch.occupancy_eq(CONC_WATER),        DoubleNear(1.0,  1.0e-12));

    // levels above the reference concentration saturate
    EXPECT_THAT(exch.occupancy_eq(2.0 * CONC_WATER),  DoubleNear(1.0,  1.0e-12));
}

TEST_F(US_SolventExchangeTest, IsotopeFractionationShiftsEquilibrium) {
    exch     = hdx();
    exch.k_d = 3.0;   // the isotope is enriched threefold on the analyte

    // theta = phi * f / ( phi * f + 1 - f )
    const double expect = 3.0 * 0.5 / ( 3.0 * 0.5 + 0.5 );
    EXPECT_THAT(exch.occupancy_eq(0.5 * CONC_WATER), DoubleNear(expect, 1.0e-12));

    // and the observed rate stays consistent with the two rate constants
    EXPECT_THAT(exch.rate_obs(0.5 * CONC_WATER),
                DoubleNear(exch.rate * ( 3.0 * 0.5 + 0.5 ), 1.0e-12));
}

TEST_F(US_SolventExchangeTest, BindingFollowsLangmuirIsotherm) {
    exch = csbind();

    EXPECT_THAT(exch.occupancy_eq(0.0), DoubleNear(0.0, 1.0e-12));
    EXPECT_THAT(exch.occupancy_eq(0.5), DoubleNear(0.5, 1.0e-12)); // L = k_d
    EXPECT_THAT(exch.occupancy_eq(1.5), DoubleNear(0.75, 1.0e-12));

    // k_obs = k_on * L + k_off with k_on = k_off / k_d
    EXPECT_THAT(exch.rate_obs(1.5), DoubleNear(0.5 * ( 1.0 + 1.5 / 0.5 ), 1.0e-12));
}

TEST_F(US_SolventExchangeTest, RelaxationIsExponential) {
    exch = hdx();

    const double lig   = 0.8 * CONC_WATER;
    const double teq   = exch.occupancy_eq(lig);
    const double kobs  = exch.rate_obs(lig);
    const double dtime = 30.0;

    const double theta = exch.relax(0.0, lig, dtime);

    EXPECT_THAT(theta, DoubleNear(teq * ( 1.0 - exp( -kobs * dtime ) ), 1.0e-12));

    // repeated steps reach the same state as one long step
    double stepped = 0.0;
    for (int ii = 0; ii < 10; ii++) {
        stepped = exch.relax(stepped, lig, dtime);
    }
    EXPECT_THAT(stepped, DoubleNear(teq * ( 1.0 - exp( -kobs * 10.0 * dtime ) ), 1.0e-12));
}

TEST_F(US_SolventExchangeTest, EquilibriumKineticsIsInstantaneous) {
    exch          = hdx();
    exch.kinetics = US_SolventExchange::EQUILIBRIUM;

    const double lig = 0.4 * CONC_WATER;
    EXPECT_THAT(exch.relax(0.0, lig, 1.0e-6),
                DoubleNear(exch.occupancy_eq(lig), 1.0e-12));
}

TEST_F(US_SolventExchangeTest, ZeroRateFreezesTheOccupancy) {
    exch      = hdx();
    exch.rate = 0.0;

    EXPECT_THAT(exch.relax(0.3, CONC_WATER, 1000.0), DoubleNear(0.3, 1.0e-12));
}

TEST_F(US_SolventExchangeTest, ExchangeIncreasesTheBuoyantMass) {
    exch = hdx();

    const double mw   = 50000.0;
    const double vbar = 0.72;
    const double rho  = 1.1;

    const double buoy0 = exch.buoyancy(0.0, rho, mw, vbar);
    const double buoy1 = exch.buoyancy(1.0, rho, mw, vbar);

    // an unexchanged analyte reproduces the Svedberg buoyancy term
    EXPECT_THAT(buoy0, DoubleNear(1.0 - vbar * rho, 1.0e-12));

    // mass without volume: the buoyancy grows by the excess mass fraction
    const double excess = 350.0 * DMASS_H2D / mw;
    EXPECT_THAT(buoy1, DoubleNear(1.0 - vbar * rho + excess, 1.0e-12));
    EXPECT_GT(buoy1, buoy0);

    // and the friction is unaffected as long as no volume is added
    EXPECT_THAT(exch.friction_ratio(1.0, mw, vbar), DoubleNear(1.0, 1.0e-12));
    EXPECT_THAT(exch.mass_ratio(1.0, mw), DoubleNear(1.0 + excess, 1.0e-12));
}

TEST_F(US_SolventExchangeTest, BuoyancyStaysFiniteAtTheIsopycnicPoint) {
    exch = csbind();

    const double mw   = 3.7e+06;   // capsid
    const double vbar = 0.70;
    const double rho  = 1.0 / vbar; // neutrally buoyant without bound cesium

    EXPECT_THAT(exch.buoyancy(0.0, rho, mw, vbar), DoubleNear(0.0, 1.0e-12));

    // bound cesium moves the analyte off the isopycnic point
    EXPECT_GT(exch.buoyancy(0.5, rho, mw, vbar), 0.0);
}

TEST_F(US_SolventExchangeTest, BoundVolumeIncreasesTheFriction) {
    exch            = csbind();
    exch.vbar_bound = 0.30;

    const double mw   = 3.7e+06;
    const double vbar = 0.70;
    const double dv   = 0.5 * 10000.0 * DMASS_CS * 0.30 / ( mw * vbar );

    EXPECT_THAT(exch.friction_ratio(0.5, mw, vbar), DoubleNear(cbrt(1.0 + dv), 1.0e-12));
    EXPECT_GT(exch.friction_ratio(0.5, mw, vbar), 1.0);
}

TEST_F(US_SolventExchangeTest, NoMolarMassLeavesTheAnalyteUnchanged) {
    exch = hdx();

    EXPECT_THAT(exch.buoyancy(1.0, 1.1, 0.0, 0.72), DoubleNear(1.0 - 0.72 * 1.1, 1.0e-12));
    EXPECT_THAT(exch.friction_ratio(1.0, 0.0, 0.72), DoubleNear(1.0, 1.0e-12));
}

TEST_F(US_SolventExchangeTest, XmlAttributesRoundTrip) {
    exch             = csbind();
    exch.vbar_bound  = 0.25;
    exch.ligand_conc = 1.7;
    exch.theta0      = -1.0;

    QString     xmlstr;
    QXmlStreamWriter xmlw(&xmlstr);
    xmlw.writeStartElement("analyte");
    exch.write_attributes(xmlw);
    xmlw.writeEndElement();

    US_SolventExchange read;
    QXmlStreamReader xmlr(xmlstr);
    while (!xmlr.atEnd()) {
        xmlr.readNext();
        if (xmlr.isStartElement() && xmlr.name().toString() == "analyte") {
            read.read_attributes(xmlr.attributes());
        }
    }

    EXPECT_EQ(read, exch);
    EXPECT_TRUE(read.preequilibrated());
}

TEST_F(US_SolventExchangeTest, NoAttributesKeepTheDefaults) {
    QString     xmlstr;
    QXmlStreamWriter xmlw(&xmlstr);
    xmlw.writeStartElement("analyte");
    xmlw.writeAttribute("name", "no exchange");
    exch.write_attributes(xmlw);      // must not write anything
    xmlw.writeEndElement();

    EXPECT_FALSE(xmlstr.contains("exch_"));

    US_SolventExchange read;
    read.sites = 99.0;                // must be left alone by a model without exchange
    QXmlStreamReader xmlr(xmlstr);
    while (!xmlr.atEnd()) {
        xmlr.readNext();
        if (xmlr.isStartElement() && xmlr.name().toString() == "analyte") {
            read.read_attributes(xmlr.attributes());
        }
    }

    EXPECT_EQ(read.type, US_SolventExchange::NONE);
    EXPECT_DOUBLE_EQ(read.sites, 99.0);
}

TEST_F(US_SolventExchangeTest, ModelComponentCarriesTheExchange) {
    US_Model model;
    US_Model::SimulationComponent sc;
    sc.name     = "AAV capsid";
    sc.mw       = 3.7e+06;
    sc.s        = 1.0e-12;
    sc.D        = 1.0e-07;
    sc.exchange = csbind();
    model.components << sc;

    QTemporaryDir tdir;
    ASSERT_TRUE(tdir.isValid());
    const QString fname = tdir.filePath("model.xml");

    ASSERT_EQ(model.write(fname), 0);

    US_Model loaded;
    ASSERT_EQ(loaded.load(fname), 0);
    ASSERT_EQ(loaded.components.size(), 1);

    EXPECT_EQ(loaded.components[0].exchange, sc.exchange);
    EXPECT_TRUE(loaded.components[0].exchange.active());
}
