//! \file test_us_buffer_component_live.cpp
// US_BufferComponent::getAllFromDB against live servers. It opens its own
// connection from the default database in the settings and the master
// password, so a mocked IUS_DB2 cannot reach it.
//
// The seed cases read the seeded uslims3_test database and change nothing.
// Set US3_TEST_LIVE_DB=1; the local dev-stack defaults can be overridden with
// US3_TEST_SEED_DB_HOST/USER/PASS and US3_TEST_PERSON_GUID/PASSWORD.
//
// The failure case installs stand-in procedures in a scratch database without
// the US3 procedures, and skips when they already exist. Set
//   US3_TEST_DB_HOST      host or host:port
//   US3_TEST_DB_NAME      a scratch database
//   US3_TEST_DB_USER      needs CREATE, DROP and CREATE ROUTINE on that database
//   US3_TEST_DB_PASSWORD
// Its checks are one test case because they share those procedures, and ctest
// runs test cases in parallel.
//
// US_DB2 always connects over TLS, with etc/mysql/ca-cert.pem as its CA and
// no client certificate, so both servers must have TLS on.  Some client
// libraries also check that each server certificate chains to that CA.

#include <gtest/gtest.h>

#include <cmath>
#include <limits>

#include "us_buffer.h"
#include "us_crypto.h"
#include "us_db2.h"
#include "us_settings.h"

#include <QMap>
#include <QString>
#include <QStringList>

namespace
{
#ifndef NO_DB
const QString kMasterPassword = "master password";

// Make the given server the default database, with both passwords encrypted
// under the master password, as the database configuration dialog stores it.
void          setDefaultDatabase( const QString& host,
                                  const QString& name,
                                  const QString& user,
                                  const QString& password,
                                  const QString& personGuid,
                                  const QString& personPassword,
                                  const QString& email )
{
   const QStringList dbPassword   = US_Crypto::encrypt( password, kMasterPassword );
   const QStringList personCipher = US_Crypto::encrypt( personPassword, kMasterPassword );
   const QStringList entry{ "live test",     user,      name, host, dbPassword[0], dbPassword[1], email, personCipher[0],
                            personCipher[1], personGuid };
   US_Settings::set_databases( { entry } );
   US_Settings::set_defaultDB( entry );
}

US_BufferComponent sentinel()
{
   US_BufferComponent c;
   c.componentID = "sentinel";
   c.name        = "previous component";
   return c;
}

class BufferComponentSeedDb : public ::testing::Test
{
   protected:
      void SetUp() override
      {
         if ( qEnvironmentVariableIntValue( "US3_TEST_LIVE_DB" ) != 1 )
         {
            GTEST_SKIP() << "US3_TEST_LIVE_DB not set";
         }
         host           = qEnvironmentVariable( "US3_TEST_SEED_DB_HOST", "127.0.0.1:3307" );
         user           = qEnvironmentVariable( "US3_TEST_SEED_DB_USER", "us3php" );
         password       = qEnvironmentVariable( "US3_TEST_SEED_DB_PASS", "us3phppass" );
         personGuid     = qEnvironmentVariable( "US3_TEST_PERSON_GUID", "00000000-0000-0000-0000-000000000001" );
         personPassword = qEnvironmentVariable( "US3_TEST_PERSON_PASSWORD", "password" );

         QString error;
         ASSERT_TRUE( admin.connectAuthenticated( host, kDatabase, user, password, personGuid, personPassword, error ) )
            << error.toStdString();
         admin.rawQuery( "SELECT email FROM people WHERE personGUID = '" + personGuid + "'" );
         ASSERT_TRUE( admin.next() );
         setDefaultDatabase( host, kDatabase, user, password, personGuid, personPassword, admin.value( 0 ).toString() );
      }

      const QString kDatabase = "uslims3_test";
      QString       host;
      QString       user;
      QString       password;
      QString       personGuid;
      QString       personPassword;
      US_DB2        admin;
};

TEST_F( BufferComponentSeedDb, ReadsEverySeededComponent )
{
   admin.rawQuery( "SELECT COUNT(*) FROM bufferComponent" );
   ASSERT_TRUE( admin.next() );
   const int stored = admin.value( 0 ).toInt();
   ASSERT_GT( stored, 0 );
   admin.rawQuery( "SELECT bufferComponentID, description, density FROM bufferComponent "
                   "ORDER BY bufferComponentID LIMIT 1" );
   ASSERT_TRUE( admin.next() );
   const QString id      = admin.value( 0 ).toString();
   const QString name    = admin.value( 1 ).toString();
   const double  density = admin.value( 2 ).toString().section( ' ', 0, 0, QString::SectionSkipEmpty ).toDouble();

   QMap<QString, US_BufferComponent> components;
   QString                           error;
   ASSERT_TRUE( US_BufferComponent::getAllFromDB( kMasterPassword, components, &error ) ) << error.toStdString();
   EXPECT_EQ( stored, components.size() );
   ASSERT_TRUE( components.contains( id ) );
   EXPECT_EQ( name, components[id].name );
   EXPECT_DOUBLE_EQ( density, components[id].dens_coeff[0] );
}

TEST_F( BufferComponentSeedDb, AConnectionThatCannotOpenFailsAndKeepsTheList )
{
   QMap<QString, US_BufferComponent> components{ { "sentinel", sentinel() } };
   QString                           error;
   // Another master password cannot decrypt the stored DB password.
   EXPECT_FALSE( US_BufferComponent::getAllFromDB( "another master password", components, &error ) );
   EXPECT_FALSE( error.isEmpty() );
   ASSERT_EQ( 1, components.size() );
   EXPECT_EQ( QString( "previous component" ), components["sentinel"].name );
}

class BufferComponentScratchDb : public ::testing::Test
{
   protected:
      void SetUp() override
      {
         host     = qEnvironmentVariable( "US3_TEST_DB_HOST" );
         name     = qEnvironmentVariable( "US3_TEST_DB_NAME" );
         user     = qEnvironmentVariable( "US3_TEST_DB_USER" );
         password = qEnvironmentVariable( "US3_TEST_DB_PASSWORD" );
         if ( host.isEmpty() || name.isEmpty() || user.isEmpty() )
         {
            GTEST_SKIP() << "US3_TEST_DB_HOST, US3_TEST_DB_NAME and US3_TEST_DB_USER not set";
         }

         QString error;
         ASSERT_TRUE( admin.connect( host, name, user, password, error ) ) << error.toStdString();
         admin.rawQuery( "SELECT COUNT(*) FROM information_schema.ROUTINES WHERE ROUTINE_SCHEMA = DATABASE() "
                         "AND ROUTINE_NAME IN ( 'validate_user', 'get_buffer_component_desc', "
                         "'get_buffer_component_info' )" );
         ASSERT_TRUE( admin.next() );
         if ( admin.value( 0 ).toInt() != 0 )
         {
            GTEST_SKIP() << "US3 procedures already exist in " << name.toStdString();
         }

         // The procedures return a status set, then their data, as the US3 procedures do.
         create( "CREATE TABLE bc_fixture ( id INT PRIMARY KEY, description TEXT, units TEXT, "
                 "viscosity TEXT, density TEXT )" );
         create( "CREATE PROCEDURE validate_user( g TEXT, e TEXT, p TEXT ) "
                 "BEGIN "
                 "  IF p = 'right' THEN "
                 "    SET @US3_ID = 42; "
                 "    SELECT 0 AS status; "
                 "    SELECT 42 AS personID, 'tester@example.com' AS email; "
                 "  ELSE "
                 "    SELECT 401 AS status; "
                 "  END IF; "
                 "END" );
         create( "CREATE PROCEDURE get_buffer_component_desc( g TEXT, p TEXT ) "
                 "BEGIN "
                 "  SELECT 0 AS status; "
                 "  SELECT id, description FROM bc_fixture ORDER BY id; "
                 "END" );
         create( "CREATE PROCEDURE get_buffer_component_info( g TEXT, p TEXT, cid INT ) "
                 "BEGIN "
                 "  SELECT 0 AS status; "
                 "  SELECT units, description, viscosity, density, '' AS c_range, 0 AS gradientForming "
                 "  FROM bc_fixture WHERE id = cid; "
                 "END" );
         setDefaultDatabase( host, name, user, password, "guid-1", "right", "tester@example.com" );
      }

      void TearDown() override
      {
         if ( created )
         {
            admin.rawQuery( "DROP PROCEDURE IF EXISTS get_buffer_component_info" );
            admin.rawQuery( "DROP PROCEDURE IF EXISTS get_buffer_component_desc" );
            admin.rawQuery( "DROP PROCEDURE IF EXISTS validate_user" );
            admin.rawQuery( "DROP TABLE IF EXISTS bc_fixture" );
         }
      }

      void create( const QString& statement )
      {
         created = true;
         admin.rawQuery( statement );
         ASSERT_TRUE( admin.lastError().isEmpty() ) << admin.lastError().toStdString();
      }

      QString host;
      QString name;
      QString user;
      QString password;
      US_DB2  admin;
      bool    created = false;
};

TEST_F( BufferComponentScratchDb, UnreadableComponentsAndAFailedListAreReported )
{
   const QString coefficients = "1 2 3 4 5 6";
   admin.rawQuery( "INSERT INTO bc_fixture VALUES ( 1, 'Readable', 'mM', '" + coefficients + "', '" + coefficients +
                   "' ), ( 2, 'Unreadable', 'mM', '1 2 x 4 5 6', '" + coefficients + "' )" );
   ASSERT_TRUE( admin.lastError().isEmpty() ) << admin.lastError().toStdString();
   {
      SCOPED_TRACE( "a component that cannot be read is left out" );
      QMap<QString, US_BufferComponent> components;
      QString                           error;
      EXPECT_TRUE( US_BufferComponent::getAllFromDB( kMasterPassword, components, &error ) ) << error.toStdString();
      ASSERT_EQ( 1, components.size() );
      EXPECT_EQ( QString( "Readable" ), components["1"].name );
   }

   admin.rawQuery( "DELETE FROM bc_fixture WHERE id = 1" );
   ASSERT_TRUE( admin.lastError().isEmpty() ) << admin.lastError().toStdString();
   {
      SCOPED_TRACE( "no component can be read" );
      QMap<QString, US_BufferComponent> components{ { "sentinel", sentinel() } };
      QString                           error;
      EXPECT_FALSE( US_BufferComponent::getAllFromDB( kMasterPassword, components, &error ) );
      EXPECT_EQ( QString( "none of the 1 buffer components could be read" ), error );
      ASSERT_EQ( 1, components.size() );
      EXPECT_TRUE( components.contains( "sentinel" ) );
   }

   admin.rawQuery( "DROP PROCEDURE get_buffer_component_desc" );
   ASSERT_TRUE( admin.lastError().isEmpty() ) << admin.lastError().toStdString();
   {
      SCOPED_TRACE( "the component list cannot be queried" );
      QMap<QString, US_BufferComponent> components{ { "sentinel", sentinel() } };
      QString                           error;
      EXPECT_FALSE( US_BufferComponent::getAllFromDB( kMasterPassword, components, &error ) );
      EXPECT_TRUE( error.contains( "get_buffer_component_desc" ) ) << error.toStdString();
      ASSERT_EQ( 1, components.size() );
      EXPECT_TRUE( components.contains( "sentinel" ) );
   }
}

/* US_BufferComponent::saveToDB against the real create_buffer_component.

   The procedure servers run inserts with an explicit ID of COUNT(*) + 1 and
   reports LAST_INSERT_ID(), which that insert does not set: it answers what
   the connection inserted last.  These cases set that to an existing
   component first, as an earlier insert on the connection would, and the
   save must still name the row it made.  They need the US3 schema and an
   investigator of userlevel 2 or more:
     US3_TEST_DB_HOST, US3_TEST_DB_NAME, US3_TEST_DB_USER, US3_TEST_DB_PASSWORD,
     US3_TEST_DB_PERSON_GUID, US3_TEST_DB_PERSON_PW
   They add rows to bufferComponent and remove them again.
*/
class BufferComponentUs3Db : public ::testing::Test
{
   protected:
      void SetUp() override
      {
         const QString host       = qEnvironmentVariable( "US3_TEST_DB_HOST" );
         const QString name       = qEnvironmentVariable( "US3_TEST_DB_NAME" );
         const QString user       = qEnvironmentVariable( "US3_TEST_DB_USER" );
         const QString password   = qEnvironmentVariable( "US3_TEST_DB_PASSWORD" );
         const QString personGuid = qEnvironmentVariable( "US3_TEST_DB_PERSON_GUID" );
         const QString personPw   = qEnvironmentVariable( "US3_TEST_DB_PERSON_PW" );
         if ( host.isEmpty() || name.isEmpty() || user.isEmpty() || personGuid.isEmpty() )
         {
            GTEST_SKIP() << "US3_TEST_DB_HOST, US3_TEST_DB_NAME, US3_TEST_DB_USER and US3_TEST_DB_PERSON_GUID not set";
         }

         QString error;
         ASSERT_TRUE( admin.connect( host, name, user, password, error ) ) << error.toStdString();
         admin.rawQuery( "SELECT COUNT(*) FROM information_schema.ROUTINES WHERE ROUTINE_SCHEMA = DATABASE() "
                         "AND ROUTINE_NAME IN ( 'create_buffer_component', 'get_buffer_component_desc', "
                         "'get_buffer_component_info' )" );
         ASSERT_TRUE( admin.next() );
         if ( admin.value( 0 ).toInt() != 3 )
         {
            GTEST_SKIP() << "the US3 buffer component procedures are not in " << name.toStdString();
         }
         ASSERT_TRUE( person.connectAuthenticated( host, name, user, password, personGuid, personPw, error ) )
            << error.toStdString();
         removeTestRows();

         component.name  = kName;
         component.unit  = "mM";
         component.range = "0-100 mM";
         for ( int i = 0; i < 6; ++i )
         {
            component.dens_coeff[i] = 0.5 + i;
            component.visc_coeff[i] = 1.25 * i;
         }
      }

      void TearDown() override
      {
         if ( admin.isConnected() )
         {
            removeTestRows();
         }
      }

      void removeTestRows()
      {
         admin.rawQuery( "DELETE FROM bufferComponent WHERE description IN ( '" + kName + "', '" + kFiller + "' )" );
      }

      //! The description of a component, or "" when there is none
      QString descriptionOf( int id )
      {
         admin.rawQuery( "SELECT description FROM bufferComponent WHERE bufferComponentID = " + QString::number( id ) );
         return admin.next() ? admin.value( 0 ).toString() : QString();
      }

      //! What COUNT(*) + 1 is now
      int nextByCount()
      {
         admin.rawQuery( "SELECT COUNT(*) + 1 FROM bufferComponent" );
         return admin.next() ? admin.value( 0 ).toInt() : -1;
      }

      bool idTaken( int id ) { return !descriptionOf( id ).isEmpty(); }

      const QString      kName   = "us3 test component (saveToDB)";
      const QString      kFiller = "us3 test component (gap filler)";
      US_DB2             admin;
      US_DB2             person;
      US_BufferComponent component;
};

TEST_F( BufferComponentUs3Db, TheSaveNamesTheRowItMadeWhateverTheConnectionInsertedBefore )
{
   if ( idTaken( nextByCount() ) )
   {
      GTEST_SKIP() << "bufferComponent has a gap in its IDs; the old procedure cannot insert there";
   }

   // An earlier insert on this connection: component 1 is somebody else's
   person.rawQuery( "SELECT LAST_INSERT_ID( 1 )" );
   ASSERT_TRUE( person.lastError().isEmpty() ) << person.lastError().toStdString();
   const QString other = descriptionOf( 1 );
   ASSERT_FALSE( other.isEmpty() );

   QString   error;
   const int id = component.saveToDB( &person, &error );
   ASSERT_GT( id, 0 ) << error.toStdString();
   EXPECT_NE( 1, id );
   EXPECT_EQ( QString::number( id ), component.componentID );
   EXPECT_EQ( kName, descriptionOf( id ) );
   EXPECT_EQ( other, descriptionOf( 1 ) );

   US_BufferComponent stored;
   stored.componentID = component.componentID;
   ASSERT_TRUE( stored.getInfoFromDB( &person ) );
   EXPECT_TRUE( stored.sameDefinition( component ) );
}

// A coefficient that is not a number used to be stored as "nan", in a row
// nothing could use; now nothing is written
TEST_F( BufferComponentUs3Db, ACoefficientThatIsNotFiniteWritesNoRow )
{
   if ( idTaken( nextByCount() ) )
   {
      GTEST_SKIP() << "bufferComponent has a gap in its IDs; the old procedure cannot insert there";
   }

   component.dens_coeff[ 2 ] = std::nan( "" );
   QString error;
   EXPECT_EQ( -1, component.saveToDB( &person, &error ) );
   EXPECT_TRUE( error.contains( "not a finite number" ) ) << error.toStdString();
   EXPECT_TRUE( component.componentID.isEmpty() );

   component.dens_coeff[ 2 ] = 2.5;
   component.visc_coeff[ 5 ] = std::numeric_limits< double >::infinity();
   EXPECT_EQ( -1, component.saveToDB( &person, &error ) );
   EXPECT_TRUE( error.contains( "not a finite number" ) ) << error.toStdString();

   admin.rawQuery( "SELECT COUNT(*) FROM bufferComponent WHERE description = '" + kName + "'" );
   ASSERT_TRUE( admin.next() );
   EXPECT_EQ( 0, admin.value( 0 ).toInt() );
}

// A gap makes the old procedure's COUNT(*) + 1 an ID that is taken.  The
// save must then fail and say so, or -- on a server whose procedure picks a
// free ID -- name its own row; never the row in the way.
TEST_F( BufferComponentUs3Db, AGapInTheIdsIsReportedNotLinkedToTheRowInTheWay )
{
   admin.rawQuery( "SELECT MAX( bufferComponentID ) + 1, COUNT(*) FROM bufferComponent" );
   ASSERT_TRUE( admin.next() );
   const int gap   = admin.value( 0 ).toInt();
   const int count = admin.value( 1 ).toInt();
   // One row past the gap is where COUNT(*) + 1 lands after the filler
   const int blocker = ( count + 2 > gap ) ? count + 2 : gap;
   admin.rawQuery( QString( "INSERT INTO bufferComponent ( bufferComponentID, units, description, viscosity, "
                            "density, c_range, gradientForming ) VALUES ( %1, 'mM', '%2', '1 0 0 0 0 0', "
                            "'1 0 0 0 0 0', '', 0 )" )
                      .arg( blocker )
                      .arg( kFiller ) );
   ASSERT_TRUE( admin.lastError().isEmpty() ) << admin.lastError().toStdString();
   person.rawQuery( QString( "SELECT LAST_INSERT_ID( %1 )" ).arg( blocker ) );

   QString   error;
   const int id = component.saveToDB( &person, &error );
   EXPECT_NE( blocker, id );
   EXPECT_EQ( kFiller, descriptionOf( blocker ) );
   if ( id > 0 )
   {
      EXPECT_EQ( kName, descriptionOf( id ) );
   }
   else
   {
      EXPECT_FALSE( error.isEmpty() );
      EXPECT_TRUE( component.componentID.isEmpty() );
   }
}
#endif
} // namespace
