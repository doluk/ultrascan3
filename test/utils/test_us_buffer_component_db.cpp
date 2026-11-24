//! \file test_us_buffer_component_db.cpp
#include "mock_us_db2.h"
#include "qt_test_base.h"
#include "us_buffer.h"

#include <QUuid>

#include <algorithm>
#include <cmath>
#include <limits>

using ::testing::_;
using ::testing::AtLeast;
using ::testing::Eq;
using ::testing::InSequence;
using ::testing::Invoke;
using ::testing::Matcher;
using ::testing::Return;
using ::testing::StrictMock;

// getAllFromDB opens its own DB connection and is not unit tested.

namespace
{
class BufferComponentDb : public QtTestBase
{
   protected:
      US_BufferComponent      component;
      StrictMock<US_DB2_Mock> db;

      // Coefficients of 1-Propanol from etc/bufferComponents.xml.
      void                    setCoefficients()
      {
         const double density[]{ 0.99820, 0.0, -1.21245, 2.81044, -7.63148, 53.39717 };
         const double viscosity[]{ 1.00725, -119.10622, 44.35231, 7.82088, -31.16201, 0.0 };
         for ( int i = 0; i < 6; ++i )
         {
            component.dens_coeff[i] = density[i];
            component.visc_coeff[i] = viscosity[i];
         }
      }

      void expectRow( const QString& range, const QString& flag )
      {
         expectColumns( "-1 0 2.5 3e-2 4 5", "10 20 30 40 50 60 legacy range", range, flag );
      }
      void expectColumns( const QString& viscosity, const QString& density, const QString& range, const QString& flag )
      {
         const QStringList query{ "get_buffer_component_info", "73" };
         InSequence        sequence;
         EXPECT_CALL( db, query( Matcher<const QStringList&>( Eq( query ) ) ) );
         EXPECT_CALL( db, lastErrno() ).WillOnce( Return( IUS_DB2::OK ) );
         EXPECT_CALL( db, next() ).WillOnce( Return( true ) );
         EXPECT_CALL( db, value( 0 ) ).WillOnce( Return( QVariant( "mM" ) ) );
         EXPECT_CALL( db, value( 1 ) ).WillOnce( Return( QVariant( "Salt & sugar" ) ) );
         EXPECT_CALL( db, value( 2 ) ).WillOnce( Return( QVariant( viscosity ) ) );
         EXPECT_CALL( db, value( 3 ) ).WillOnce( Return( QVariant( density ) ) );
         EXPECT_CALL( db, value( 4 ) ).WillOnce( Return( QVariant( range ) ) );
         EXPECT_CALL( db, value( 5 ) ).WillOnce( Return( QVariant( flag ) ) );
      }
      // A component whose every field differs from any row the tests return.
      void prepareStale()
      {
         component.componentID = "73";
         component.unit        = "stale unit";
         component.name        = "stale";
         component.range       = "stale range";
         component.grad_form   = true;
         for ( int i = 0; i < 6; ++i )
         {
            component.visc_coeff[i] = component.dens_coeff[i] = -999;
         }
      }
      void checkStale()
      {
         EXPECT_EQ( QString( "73" ), component.componentID );
         EXPECT_EQ( QString( "stale unit" ), component.unit );
         EXPECT_EQ( QString( "stale" ), component.name );
         EXPECT_EQ( QString( "stale range" ), component.range );
         EXPECT_TRUE( component.grad_form );
         for ( int i = 0; i < 6; ++i )
         {
            SCOPED_TRACE( i );
            EXPECT_EQ( -999, component.visc_coeff[i] );
            EXPECT_EQ( -999, component.dens_coeff[i] );
         }
      }
      void checkRow( const QString& range, bool gradient )
      {
         EXPECT_EQ( QString( "73" ), component.componentID );
         EXPECT_EQ( QString( "mM" ), component.unit );
         EXPECT_EQ( QString( "Salt & sugar" ), component.name );
         EXPECT_EQ( range, component.range );
         EXPECT_EQ( gradient, component.grad_form );
         const double viscosity[]{ -1, 0, 2.5, 0.03, 4, 5 };
         const double density[]{ 10, 20, 30, 40, 50, 60 };
         for ( int i = 0; i < 6; ++i )
         {
            SCOPED_TRACE( i );
            EXPECT_DOUBLE_EQ( viscosity[i], component.visc_coeff[i] );
            EXPECT_DOUBLE_EQ( density[i], component.dens_coeff[i] );
         }
      }
};

TEST_F( BufferComponentDb, MapsAllColumnsAndPrefersExplicitRange )
{
   component.componentID = "73";
   expectRow( "0 to 9", "1" );
   ASSERT_TRUE( component.getInfoFromDB( &db ) );
   checkRow( "0 to 9", true );
}

TEST_F( BufferComponentDb, UsesLegacyDensitySuffixWhenRangeIsEmpty )
{
   component.componentID = "73";
   expectRow( "", "T" );
   ASSERT_TRUE( component.getInfoFromDB( &db ) );
   checkRow( "legacy range", true );
}

TEST_F( BufferComponentDb, ReplacesExistingValuesIncludingTrueGradientFlag )
{
   component.componentID = "73";
   component.name        = "stale";
   component.range       = "stale";
   component.grad_form   = true;
   for ( int i = 0; i < 6; ++i )
   {
      component.visc_coeff[i] = component.dens_coeff[i] = -999;
   }
   expectRow( "explicit", "0" );
   ASSERT_TRUE( component.getInfoFromDB( &db ) );
   checkRow( "explicit", false );
}

/* The bufferComponent table and its three procedures, as the server runs
   them, on a mock.  create_buffer_component inserts with an explicit ID of
   COUNT(*) + 1, which fails on a taken ID as the primary key does, and
   lastInsertID() answers what the connection inserted last -- not that
   row, since an explicit ID does not set LAST_INSERT_ID().  A patched
   server inserts at MAX + 1 and reports it.  get_buffer_component_desc
   answers NOROWS for an empty table, get_buffer_component_info NOROWS for
   a missing ID, as the real procedures do. */
class ComponentTable
{
   public:
      struct Row
      {
            QString unit, name, viscosity, density, range, gradient;
      };

      QMap<int, Row> rows;
      int            staleInsertId = 10;  //!< What the connection inserted last
      bool           patched       = false;
      bool           insertNothing = false;  //!< A create that answers OK and stores nothing
      QStringList    created;               //!< The arguments of each create

      void install( ::testing::NiceMock<US_DB2_Mock>& db )
      {
         using ::testing::Matcher;
         ON_CALL( db, query( Matcher<const QStringList&>( _ ) ) )
            .WillByDefault( Invoke( [this]( const QStringList& q ) { answer( q ); } ) );
         ON_CALL( db, statusQuery( Matcher<const QStringList&>( _ ) ) )
            .WillByDefault( Invoke( [this]( const QStringList& q ) { return create( q ); } ) );
         ON_CALL( db, lastErrno() ).WillByDefault( Invoke( [this]() { return status; } ) );
         ON_CALL( db, lastError() ).WillByDefault( Invoke( [this]() { return message; } ) );
         ON_CALL( db, lastInsertID() ).WillByDefault( Invoke( [this]() { return staleInsertId; } ) );
         ON_CALL( db, next() ).WillByDefault( Invoke( [this]() { return ++cursor < result.size(); } ) );
         ON_CALL( db, value( _ ) )
            .WillByDefault( Invoke( [this]( unsigned ix ) { return QVariant( result.value( cursor ).value( ix ) ); } ) );
      }

      void add( int id, const QString& name )
      {
         rows[id] = Row{ "mM", name, "1 0 0 0 0 0", "1 0 0 0 0 0", "", "0" };
      }

   private:
      int                status = IUS_DB2::OK;
      QString            message;
      QList<QStringList> result;
      int                cursor = -1;

      void set( int code, const QString& text, const QList<QStringList>& rowsOut = {} )
      {
         status  = code;
         message = text;
         result  = rowsOut;
         cursor  = -1;
      }

      void answer( const QStringList& q )
      {
         if ( q.value( 0 ) == "get_buffer_component_desc" )
         {
            QList<QStringList> out;
            for ( auto it = rows.cbegin(); it != rows.cend(); ++it )
            {
               out << QStringList{ QString::number( it.key() ), it.value().name };
            }
            // Ordered by description, as the procedure orders them
            std::sort( out.begin(), out.end(), []( const QStringList& a, const QStringList& b ) { return a[1] < b[1]; } );
            set( out.isEmpty() ? IUS_DB2::NOROWS : IUS_DB2::OK, out.isEmpty() ? "MySQL: no rows returned" : "", out );
         }
         else if ( q.value( 0 ) == "get_buffer_component_info" )
         {
            const int id = q.value( 1 ).toInt();
            if ( !rows.contains( id ) )
            {
               set( IUS_DB2::NOROWS, "MySQL: no rows returned" );
               return;
            }
            const Row& r = rows[id];
            set( IUS_DB2::OK, "", { { r.unit, r.name, r.viscosity, r.density, r.range, r.gradient } } );
         }
         else
         {
            set( IUS_DB2::DBERROR, "unexpected query " + q.value( 0 ) );
         }
      }

      int create( const QStringList& q )
      {
         if ( q.value( 0 ) != "create_buffer_component" || q.size() != 8 )
         {
            set( IUS_DB2::DBERROR, "unexpected call " + q.join( "," ) );
            return status;
         }
         created = q;
         if ( insertNothing )
         {
            set( IUS_DB2::OK, "" );
            return status;
         }
         const int id = patched ? ( rows.isEmpty() ? 1 : rows.lastKey() + 1 ) : rows.size() + 1;
         if ( rows.contains( id ) )
         {
            set( IUS_DB2::DBERROR, QString( "MySQL error: Duplicate entry '%1' for key 'PRIMARY'" ).arg( id ) );
            return status;
         }
         // create_buffer_component( name, unit, range, grad_form, density, viscosity )
         rows[id] = Row{ q[3], q[2], q[7], q[6], q[4], q[5] };
         if ( patched )
         {
            staleInsertId = id;
         }
         set( IUS_DB2::OK, "" );
         return status;
      }
};

class BufferComponentSave : public QtTestBase
{
   protected:
      US_BufferComponent                    component;
      ::testing::NiceMock<US_DB2_Mock>      db;
      ComponentTable                        table;

      void SetUp() override
      {
         QtTestBase::SetUp();
         table.install( db );
         component.componentID = "73";
         component.name        = "Salt & sugar";
         component.unit        = "mM";
         component.range       = "0 to 9";
         // Coefficients of 1-Propanol from etc/bufferComponents.xml.
         const double density[]{ 0.99820, 0.0, -1.21245, 2.81044, -7.63148, 53.39717 };
         const double viscosity[]{ 1.00725, -119.10622, 44.35231, 7.82088, -31.16201, 0.0 };
         for ( int i = 0; i < 6; ++i )
         {
            component.dens_coeff[i] = density[i];
            component.visc_coeff[i] = viscosity[i];
         }
      }
};

TEST_F( BufferComponentSave, SendsTheDefinitionAndReturnsTheRowItMadeNotTheStaleInsertId )
{
   for ( bool gradient : { false, true } )
   {
      SCOPED_TRACE( gradient );
      table.rows.clear();
      table.add( 1, "Cadmium chloride" );
      table.add( 2, "Tris" );
      table.add( 3, "Glycerol" );
      // The connection's last insert was component 2's -- or anything else
      table.staleInsertId = 2;
      component.componentID = "73";
      component.grad_form   = gradient;

      QString error;
      EXPECT_EQ( 4, component.saveToDB( &db, &error ) ) << error.toStdString();
      EXPECT_EQ( QString( "4" ), component.componentID );

      // create_buffer_component( p_personGUID, p_password, p_componentGUID, p_name, p_unit,
      // p_range, p_grad_form, p_density, p_viscosity ): the query layer supplies the
      // first two, so the call carries the other seven.
      const QString componentGuid = table.created.value( 1 );
      EXPECT_FALSE( QUuid( componentGuid ).isNull() );
      EXPECT_EQ( ( QStringList{ "create_buffer_component", componentGuid, "Salt & sugar", "mM", "0 to 9",
                                gradient ? "1" : "0", "0.99820 0.00000 -1.21245 2.81044 -7.63148 53.39717",
                                "1.00725 -119.10622 44.35231 7.82088 -31.16201 0.00000" } ),
                 table.created );
   }
}

TEST_F( BufferComponentSave, AnEmptyTableIsNoError )
{
   QString error;
   EXPECT_EQ( 1, component.saveToDB( &db, &error ) ) << error.toStdString();
   EXPECT_EQ( QString( "1" ), component.componentID );
}

TEST_F( BufferComponentSave, APatchedServerIsReadTheSameWay )
{
   table.patched = true;
   table.add( 1, "Cadmium chloride" );
   table.add( 5, "Tris" );
   QString error;
   EXPECT_EQ( 6, component.saveToDB( &db, &error ) ) << error.toStdString();
   EXPECT_EQ( QString( "6" ), component.componentID );
}

// With a gap in the IDs, COUNT(*) + 1 is taken: the server refuses, and
// that is what the caller hears -- not the ID of the row in the way
TEST_F( BufferComponentSave, AnIdTheOldProcedureCollidesWithFailsClearly )
{
   table.add( 1, "Cadmium chloride" );
   table.add( 3, "Tris" );
   QString error;
   EXPECT_EQ( -1, component.saveToDB( &db, &error ) );
   EXPECT_EQ( QString( "73" ), component.componentID );
   EXPECT_TRUE( error.contains( "Duplicate entry '3'" ) ) << error.toStdString();
   EXPECT_EQ( QString( "Tris" ), table.rows[3].name );
}

TEST_F( BufferComponentSave, ACreateThatLeavesNoSuchRowFails )
{
   table.add( 1, "Cadmium chloride" );
   table.insertNothing = true;
   table.staleInsertId = 1;
   QString error;
   EXPECT_EQ( -1, component.saveToDB( &db, &error ) );
   EXPECT_EQ( QString( "73" ), component.componentID );
   EXPECT_TRUE( error.contains( "no new component" ) ) << error.toStdString();
}

// A new row that is not this component -- made by someone else meanwhile,
// or stored other than it was sent -- is not taken for it
TEST_F( BufferComponentSave, ANewRowWithAnotherDefinitionIsNotTaken )
{
   table.add( 1, "Cadmium chloride" );
   EXPECT_CALL( db, statusQuery( ::testing::Matcher<const QStringList&>( _ ) ) )
      .WillOnce( Invoke(
         [this]( const QStringList& )
         {
            table.add( 2, "Someone else's" );
            return IUS_DB2::OK;
         } ) );
   QString error;
   EXPECT_EQ( -1, component.saveToDB( &db, &error ) );
   EXPECT_EQ( QString( "73" ), component.componentID );
}

TEST_F( BufferComponentSave, ARefusedCreateKeepsTheIdAndSaysWhy )
{
   EXPECT_CALL( db, statusQuery( ::testing::Matcher<const QStringList&>( _ ) ) ).WillOnce( Return( IUS_DB2::DBERROR ) );
   EXPECT_CALL( db, lastErrno() ).WillOnce( Return( IUS_DB2::NOROWS ) ).WillRepeatedly( Return( IUS_DB2::DBERROR ) );
   EXPECT_CALL( db, lastError() ).WillRepeatedly( Return( QString( "MySQL: you do not have permission" ) ) );
   EXPECT_CALL( db, lastInsertID() ).Times( 0 );
   QString error;
   EXPECT_EQ( -1, component.saveToDB( &db, &error ) );
   EXPECT_EQ( QString( "73" ), component.componentID );
   EXPECT_TRUE( error.contains( "permission" ) ) << error.toStdString();
}

TEST_F( BufferComponentSave, AListThatCannotBeReadIsReported )
{
   EXPECT_CALL( db, query( ::testing::Matcher<const QStringList&>( _ ) ) ).WillOnce( Return() );
   EXPECT_CALL( db, lastErrno() ).WillRepeatedly( Return( IUS_DB2::DBERROR ) );
   EXPECT_CALL( db, statusQuery( ::testing::Matcher<const QStringList&>( _ ) ) ).Times( 0 );
   QString error;
   EXPECT_EQ( -1, component.saveToDB( &db, &error ) );
   EXPECT_FALSE( error.isEmpty() );
}

TEST_F( BufferComponentSave, SavedCoefficientsReadBackUnchanged )
{
   QString error;
   ASSERT_EQ( 1, component.saveToDB( &db, &error ) ) << error.toStdString();

   US_BufferComponent loaded;
   loaded.componentID = "1";
   ASSERT_TRUE( loaded.getInfoFromDB( &db ) );
   EXPECT_TRUE( loaded.sameDefinition( component ) );
   for ( int i = 0; i < 6; ++i )
   {
      SCOPED_TRACE( i );
      EXPECT_DOUBLE_EQ( component.dens_coeff[i], loaded.dens_coeff[i] );
      EXPECT_DOUBLE_EQ( component.visc_coeff[i], loaded.visc_coeff[i] );
   }
   EXPECT_EQ( QString( "0 to 9" ), loaded.range );
}

// "nan" or "inf" in the table helps no one: the component is refused
// before anything is written
TEST_F( BufferComponentSave, ACoefficientThatIsNotFiniteIsRefusedBeforeTheCreate )
{
   table.add( 1, "Cadmium chloride" );
   const double bad[]{ std::nan( "" ), std::numeric_limits< double >::infinity(),
                       -std::numeric_limits< double >::infinity() };
   for ( double value : bad )
   {
      for ( bool density : { true, false } )
      {
         SCOPED_TRACE( density );
         US_BufferComponent broken = component;
         ( density ? broken.dens_coeff : broken.visc_coeff )[ 4 ] = value;
         EXPECT_CALL( db, statusQuery( ::testing::Matcher<const QStringList&>( _ ) ) ).Times( 0 );
         QString error;
         EXPECT_EQ( -1, broken.saveToDB( &db, &error ) );
         EXPECT_EQ( QString( "73" ), broken.componentID );
         EXPECT_TRUE( error.contains( "not a finite number" ) ) << error.toStdString();
         EXPECT_TRUE( error.contains( density ? "density" : "viscosity" ) ) << error.toStdString();
         EXPECT_TRUE( table.created.isEmpty() );
         EXPECT_EQ( 1, table.rows.size() );
         ::testing::Mock::VerifyAndClearExpectations( &db );
         table.install( db );
      }
   }
}

// Two clients creating the same component at once both find the same row
TEST_F( BufferComponentSave, OfSeveralNewRowsWithTheDefinitionTheHighestIdIsTaken )
{
   table.add( 1, "Cadmium chloride" );
   EXPECT_CALL( db, statusQuery( ::testing::Matcher<const QStringList&>( _ ) ) )
      .WillOnce( Invoke(
         [this]( const QStringList& q )
         {
            for ( int id : { 9, 10, 3 } )
               table.rows[ id ] = ComponentTable::Row{ q[3], q[2], q[7], q[6], q[4], q[5] };
            return IUS_DB2::OK;
         } ) );
   QString error;
   EXPECT_EQ( 10, component.saveToDB( &db, &error ) ) << error.toStdString();
   EXPECT_EQ( QString( "10" ), component.componentID );
}

// The rule every caller that matches a component uses
TEST( BufferComponentDefinition, MatchingIdIsTheHighestNumericIdWithTheDefinition )
{
   US_BufferComponent want;
   want.name  = "Tris";
   want.unit  = "mM";
   want.range = "0-1 M";
   want.dens_coeff[0] = 0.99820;

   US_BufferComponent other = want;
   other.name = "Tris base";

   US_BufferComponent spaced = want;
   spaced.range = "0-1 M ";

   QMap< QString, US_BufferComponent > table;
   EXPECT_TRUE( US_BufferComponent::matchingID( table, want ).isEmpty() );

   table.insert( "100", other );
   table.insert( "7", spaced );
   EXPECT_TRUE( US_BufferComponent::matchingID( table, want ).isEmpty() )
      << "a range that differs in its spaces is another definition";

   // A QMap keeps "10" before "9": the order it is read in is not the rule
   table.insert( "9", want );
   table.insert( "10", want );
   table.insert( "2", want );
   EXPECT_EQ( QString( "10" ), US_BufferComponent::matchingID( table, want ) );
}

TEST( BufferComponentDefinition, ComparesWhatIsStored )
{
   US_BufferComponent a;
   a.name = "Tris";
   a.unit = "mM";
   a.dens_coeff[0] = 0.998201;
   US_BufferComponent b = a;
   b.componentID = "9";
   b.dens_coeff[0] = 0.998202;  // the same to five decimals
   EXPECT_TRUE( a.sameDefinition( b ) );
   b.dens_coeff[0] = 0.99830;
   EXPECT_FALSE( a.sameDefinition( b ) );
   b = a;
   b.range = "0-1 M";
   EXPECT_FALSE( a.sameDefinition( b ) );
   b = a;
   b.grad_form = true;
   EXPECT_FALSE( a.sameDefinition( b ) );
   b = a;
   b.unit = "M";
   EXPECT_FALSE( a.sameDefinition( b ) );
}


TEST_F( BufferComponentDb, ToleratesExtraSpacesBetweenCoefficients )
{
   component.componentID = "73";
   expectColumns( " -1  0 2.5 3e-2 4 5 ", "  10 20  30 40 50 60  legacy  range", "", "1" );
   ASSERT_TRUE( component.getInfoFromDB( &db ) );
   checkRow( "legacy range", true );
}

TEST_F( BufferComponentDb, FailedOrEmptyQueryReadsNoColumnsAndKeepsTheComponent )
{
   for ( int status : { IUS_DB2::DBERROR, IUS_DB2::NOROWS, IUS_DB2::NOT_CONNECTED } )
   {
      SCOPED_TRACE( status );
      prepareStale();
      {
         InSequence sequence;
         EXPECT_CALL( db, query( Matcher<const QStringList&>( Eq( QStringList{ "get_buffer_component_info", "73" } ) ) ) );
         EXPECT_CALL( db, lastErrno() ).WillOnce( Return( status ) );
      }
      EXPECT_CALL( db, next() ).Times( 0 );
      EXPECT_CALL( db, value( _ ) ).Times( 0 );
      EXPECT_FALSE( component.getInfoFromDB( &db ) );
      checkStale();
      ASSERT_TRUE( ::testing::Mock::VerifyAndClearExpectations( &db ) );
   }
}

TEST_F( BufferComponentDb, MissingRowReadsNoColumnsAndKeepsTheComponent )
{
   prepareStale();
   {
      InSequence sequence;
      EXPECT_CALL( db, query( Matcher<const QStringList&>( Eq( QStringList{ "get_buffer_component_info", "73" } ) ) ) );
      EXPECT_CALL( db, lastErrno() ).WillOnce( Return( IUS_DB2::OK ) );
      EXPECT_CALL( db, next() ).WillOnce( Return( false ) );
   }
   EXPECT_CALL( db, value( _ ) ).Times( 0 );
   EXPECT_FALSE( component.getInfoFromDB( &db ) );
   checkStale();
}

TEST_F( BufferComponentDb, MalformedCoefficientsKeepTheComponent )
{
   const QString six = "1 2 3 4 5 6";
   const struct
   {
         const char* label;
         QString     viscosity;
         QString     density;
   } rows[]{
      { "empty viscosity", "", six },
      { "empty density", six, "" },
      { "five viscosity", "1 2 3 4 5", six },
      { "five density", six, "1 2 3 4 5" },
      { "seven viscosity", "1 2 3 4 5 6 7", six },
      { "non-numeric viscosity", "1 2 x 4 5 6", six },
      { "non-numeric density", six, "1 2 3 4 5 six" },
      // The pre-fix writer ran the coefficients together after a leading space.
      { "unseparated", " 1.000002.000003.000004.000005.000006.00000", " 1.000002.000003.000004.000005.000006.00000" },
   };
   for ( const auto& row : rows )
   {
      SCOPED_TRACE( row.label );
      prepareStale();
      expectColumns( row.viscosity, row.density, "0 to 1", "0" );
      EXPECT_FALSE( component.getInfoFromDB( &db ) );
      checkStale();
      ASSERT_TRUE( ::testing::Mock::VerifyAndClearExpectations( &db ) );
   }
}
}
