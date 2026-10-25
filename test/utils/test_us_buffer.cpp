// test_us_buffer.cpp
#include "qt_test_base.h"
#include "mock_us_db2.h"
#include "us_buffer.h"
#include "us_constants.h"
#include <QTemporaryFile>
#include <QTemporaryDir>
#include <QXmlStreamWriter>
#include <cstring>

using ::testing::_;
using ::testing::Return;
using ::testing::Eq;
using ::testing::DoubleEq;
using ::testing::DoubleNear;
using ::testing::StrictMock;
using ::testing::NiceMock;
using ::testing::InSequence;
using ::testing::SetArgReferee;
using ::testing::DoAll;

class US_BufferComponentTest : public QtTestBase {
protected:
    void SetUp() override {
        QtTestBase::SetUp();
        setupValidComponent();
    }

    void setupValidComponent() {
        component.componentID = "1";
        component.name = "NaCl";
        component.unit = "M";
        component.range = "0.0 - 1.0";
        component.grad_form = false;

        // Initialize coefficients
        for (int i = 0; i < 6; i++) {
            component.dens_coeff[i] = 1.0 + i * 0.1;
            component.visc_coeff[i] = 2.0 + i * 0.1;
        }
    }

    US_BufferComponent component;
};

class US_BufferTest : public QtTestBase {
protected:
    void SetUp() override {
        QtTestBase::SetUp();
        setupValidBuffer();
    }

    void setupValidBuffer() {
        buffer.person = "Test User";
        buffer.bufferID = "1";
        buffer.GUID = "test-guid-123";
        buffer.description = "Test Buffer";
        buffer.compressibility = 4.6e-10;
        buffer.pH = 7.4;
        buffer.density = DENS_20W;
        buffer.viscosity = VISC_20W;
        buffer.manual = false;
        buffer.replace_spectrum = false;
        buffer.new_or_changed_spectrum = false;

        // Add a test component
        US_BufferComponent comp;
        comp.componentID = "1";
        comp.name = "NaCl";
        comp.unit = "M";
        for (int i = 0; i < 6; i++) {
            comp.dens_coeff[i] = 1.0 + i * 0.1;
            comp.visc_coeff[i] = 2.0 + i * 0.1;
        }

        buffer.component.append(comp);
        buffer.componentIDs.append("1");
        buffer.concentration.append(0.15);

        // Add test spectrum data
        buffer.extinction[280.0] = 1.5;
        buffer.extinction[250.0] = 2.0;
        buffer.refraction[280.0] = 0.1;
        buffer.fluorescence[350.0] = 0.5;
    }

    QString createTempXmlFile(const QString& content) {
        QTemporaryFile* tempFile = new QTemporaryFile();
        tempFile->setAutoRemove(false);
        if (tempFile->open()) {
            tempFile->write(content.toUtf8());
            tempFile->close();
            return tempFile->fileName();
        }
        return QString();
    }

    US_Buffer buffer;
};

// US_BufferComponent Tests

TEST_F(US_BufferComponentTest, DefaultConstruction) {
US_BufferComponent comp;

EXPECT_TRUE(comp.componentID.isEmpty());
EXPECT_TRUE(comp.name.isEmpty());
EXPECT_TRUE(comp.unit.isEmpty());
EXPECT_TRUE(comp.range.isEmpty());
// grad_form is the only non-QString member with a default initializer.
EXPECT_FALSE(comp.grad_form);
}

TEST_F(US_BufferComponentTest, SaveToDBSuccess) {
auto mockDb = std::make_unique<NiceMock<US_DB2_Mock>>();

// Don't mock the query call - just mock the result methods
EXPECT_CALL(*mockDb, lastErrno())
.WillOnce(Return(IUS_DB2::OK));
EXPECT_CALL(*mockDb, lastInsertID())
.WillOnce(Return(456));

component.name = "Test Component";
int result = component.saveToDB(mockDb.get());

EXPECT_EQ(result, 456);
EXPECT_EQ(component.componentID, "456");
}

TEST_F(US_BufferComponentTest, SaveToDBError) {
auto mockDb = std::make_unique<NiceMock<US_DB2_Mock>>();

EXPECT_CALL(*mockDb, lastErrno())
.WillRepeatedly(Return(IUS_DB2::DBERROR));

int result = component.saveToDB(mockDb.get());

EXPECT_EQ(result, -1);
}

TEST_F(US_BufferComponentTest, GetInfoFromDBSuccess) {
auto mockDb = std::make_unique<NiceMock<US_DB2_Mock>>();

EXPECT_CALL(*mockDb, next())
.WillOnce(Return(true));

EXPECT_CALL(*mockDb, value(0))
.WillOnce(Return(QVariant("M")));
EXPECT_CALL(*mockDb, value(1))
.WillOnce(Return(QVariant("Salt")));
EXPECT_CALL(*mockDb, value(2))
.WillOnce(Return(QVariant("1.0 1.1 1.2 1.3 1.4 1.5")));
EXPECT_CALL(*mockDb, value(3))
.WillOnce(Return(QVariant("2.0 2.1 2.2 2.3 2.4 2.5")));
EXPECT_CALL(*mockDb, value(4))
.WillOnce(Return(QVariant("0-1")));
EXPECT_CALL(*mockDb, value(5))
.WillOnce(Return(QVariant("false")));

component.componentID = "123";
component.getInfoFromDB(mockDb.get());

EXPECT_EQ(component.unit, "M");
EXPECT_EQ(component.name, "Salt");
}

TEST_F(US_BufferTest, ReadFromDBError) {
auto mockDb = std::make_unique<NiceMock<US_DB2_Mock>>();

EXPECT_CALL(*mockDb, lastErrno())
.WillOnce(Return(IUS_DB2::DBERROR));

US_Buffer testBuffer;
bool result = testBuffer.readFromDB(mockDb.get(), "123");

EXPECT_FALSE(result);
}

// A co-sedimenting component's density and viscosity coefficients survive a
// write to disk and a read back.  The writer used to discard the attribute
// names and keep five decimals, so a gradient buffer saved to disk read back
// with no gradient at all.
TEST_F( US_BufferTest, CosedComponentsSurviveADiskRoundTrip )
{
   US_CosedComponent cs;
   cs.componentID       = "13";
   cs.GUID              = "cosed-cscl-guid";
   cs.name              = "Cesium chloride";
   cs.conc              = 1.5;
   cs.s_coeff           = 1.06e-13;
   cs.d_coeff           = 2.0e-5;
   cs.vbar              = 0.23;
   cs.overlaying        = false;
   const double dens[6] = { 0.0, 0.0, 0.1268641, 0.00127445, -0.0011954, 0.000258866 };
   const double visc[6] = { 0.0, -0.0194104, -0.0407863, 0.0115489, -0.0021774, 0.0 };
   for ( int jj = 0; jj < 6; jj++ )
   {
      cs.dens_coeff[jj] = dens[jj];
      cs.visc_coeff[jj] = visc[jj];
   }
   buffer.cosed_component << cs;

   QTemporaryDir tdir;
   ASSERT_TRUE( tdir.isValid() );
   const QString path = tdir.filePath( "cosed_buffer.xml" );
   ASSERT_TRUE( buffer.writeToDisk( path ) );

   US_Buffer readback;
   ASSERT_TRUE( readback.readFromDisk( path ) );
   ASSERT_EQ( readback.cosed_component.size(), 1 );

   const US_CosedComponent& rc = readback.cosed_component[0];
   EXPECT_EQ( rc.name, cs.name );
   EXPECT_DOUBLE_EQ( rc.conc, cs.conc );
   EXPECT_NEAR( rc.s_coeff, cs.s_coeff, 1.0e-18 );
   EXPECT_NEAR( rc.d_coeff, cs.d_coeff, 1.0e-11 );
   for ( int jj = 0; jj < 6; jj++ )
   {
      EXPECT_NEAR( rc.dens_coeff[jj], dens[jj], 1.0e-12 ) << "density c" << jj;
      EXPECT_NEAR( rc.visc_coeff[jj], visc[jj], 1.0e-12 ) << "viscosity c" << jj;
   }
}
