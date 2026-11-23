// test/programs/us_data_publication/test_us_datapub_cli.cpp
#include <gtest/gtest.h>

#include "datapub_test_env.h"

#include "us_datapub_cli.h"
#include "us_datapub_bundle.h"
#include "us_datapub_manifest.h"

#if defined( Q_OS_UNIX )
#include <fcntl.h>
#include <unistd.h>
#endif

/**
 * The command line's export arguments, run against the scratch disk store.
 * Each case runs the front end exactly as main() does and looks at the exit
 * code and, where one was written, at the bundle.
 */
class DataPubCli : public DataPubTestEnv
{
   protected:
      QByteArray savedPassword;

      void SetUp() override
      {
         DataPubTestEnv::SetUp();

         // A password in the environment would answer what these cases ask
         savedPassword = qgetenv( "US3_DB_PASSWORD" );
         qunsetenv( "US3_DB_PASSWORD" );
      }

      void TearDown() override
      {
         if ( ! savedPassword.isEmpty() )
            qputenv( "US3_DB_PASSWORD", savedPassword );

         DataPubTestEnv::TearDown();
      }

      int run( const QStringList& args )
      {
         US_DataPubCLI cli;
         return cli.run( QStringList( "us_data_publication" ) + args );
      }

#if defined( Q_OS_UNIX )
      // Run, and collect what the command line wrote to its error output
      int runCapturing( const QStringList& args, QString& errText )
      {
         QString path  = root + "/stderr.txt";
         int     saved = dup( STDERR_FILENO );
         int     fd    = ::open( QFile::encodeName( path ).constData(),
                                 O_WRONLY | O_CREAT | O_TRUNC, 0600 );
         fflush( stderr );
         dup2( fd, STDERR_FILENO );
         ::close( fd );

         int code;
         {
            US_DataPubCLI cli;
            code = cli.run( QStringList( "us_data_publication" ) + args );
         }

         fflush( stderr );
         dup2( saved, STDERR_FILENO );
         ::close( saved );

         QFile file( path );
         file.open( QIODevice::ReadOnly );
         errText = QString::fromUtf8( file.readAll() );

         return code;
      }
#endif

      // The manifest of a bundle the command line wrote
      bool manifestOf( const QString& bundlePath, US_DataPubManifest& mani )
      {
         US_DataPubBundle bundle;
         QString          error;

         if ( ! bundle.unpack( bundlePath, error ) )  return false;

         return mani.read( bundle.rootPath() + "/"
                           + US_DataPubBundle::manifestName(), error );
      }
};

TEST_F( DataPubCli, AnyLongOptionSelectsTheCommandLine )
{
   EXPECT_FALSE( US_DataPubCLI::wanted( QStringList( "prog" ) ) );
   EXPECT_FALSE( US_DataPubCLI::wanted( QStringList( "prog" ) << "-style"
                                                              << "fusion" ) );
   EXPECT_TRUE ( US_DataPubCLI::wanted( QStringList( "prog" ) << "--mode"
                                                              << "export" ) );

   // A command line without --mode is reported, not taken for the window
   EXPECT_TRUE ( US_DataPubCLI::wanted( QStringList( "prog" ) << "--bundle"
                                                              << "x.tar.gz" ) );
   EXPECT_EQ( run( QStringList() << "--bundle" << root + "/x.tar.gz" ), 2 );
}

TEST_F( DataPubCli, AnOptionIsNotTakenForTheValueOfAnother )
{
   EXPECT_EQ( run( QStringList() << "--mode" << "export" << "--bundle"
                                 << "--dry-run" << "--experiment-id"
                                 << runID ), 2 );

   EXPECT_EQ( run( QStringList() << "--mode" << "export" << "--experiment-id"
                                 << "--dry-run" ), 2 );

   EXPECT_EQ( run( QStringList() << "--mode" << "export" << "--frobnicate" ),
              2 );
}

TEST_F( DataPubCli, AValueGivenWithAnEqualsSignIsTakenAsItIs )
{
   QString bundle = root + "/inline.tar.gz";

   EXPECT_EQ( run( QStringList() << "--mode=export" << "--experiment-id"
                                 << runID << "--comment=--not-an-option"
                                 << "--max-scope=rawData"
                                 << "--bundle=" + bundle ), 0 );

   US_DataPubManifest mani;
   ASSERT_TRUE( manifestOf( bundle, mani ) );
   EXPECT_EQ  ( mani.comment, "--not-an-option" );
}

// --include-<scope> asks for that record type; it used to be lost against
// a default scope that was already the whole chain
TEST_F( DataPubCli, IncludeSetsTheScope )
{
   QString bundle = root + "/include.tar.gz";

   ASSERT_EQ( run( QStringList() << "--mode" << "export" << "--project-id"
                                 << "7" << "--include-edits"
                                 << "--bundle" << bundle ), 0 );

   US_DataPubManifest mani;
   ASSERT_TRUE( manifestOf( bundle, mani ) );
   EXPECT_EQ  ( mani.scope, "edits" );
   EXPECT_EQ  ( mani.count( US_DataPub::EditedData ), 1 );
   EXPECT_EQ  ( mani.count( US_DataPub::Model ),      0 );
   EXPECT_FALSE( mani.isDeclared( US_DataPub::Model ) );
}

TEST_F( DataPubCli, IncludeBeyondTheMaximumIsRefused )
{
   EXPECT_EQ( run( QStringList() << "--mode" << "export" << "--experiment-id"
                                 << runID << "--max-scope" << "rawData"
                                 << "--include-models" << "--dry-run" ), 2 );
}

TEST_F( DataPubCli, WithoutAnyScopeTheWholeChainIsExported )
{
   QString bundle = root + "/whole.tar.gz";

   ASSERT_EQ( run( QStringList() << "--mode" << "export" << "--experiment-id"
                                 << runID << "--bundle" << bundle ), 0 );

   US_DataPubManifest mani;
   ASSERT_TRUE( manifestOf( bundle, mani ) );
   EXPECT_EQ  ( mani.scope, "noise" );
   EXPECT_EQ  ( mani.count( US_DataPub::Noise ), 1 );
}

TEST_F( DataPubCli, ARunNamedTwiceIsExportedOnce )
{
   QString bundle = root + "/twice.tar.gz";

   ASSERT_EQ( run( QStringList() << "--mode" << "export" << "--experiment-id"
                                 << runID << "--experiment-id" << runID
                                 << "--max-scope" << "rawData"
                                 << "--bundle" << bundle ), 0 );

   US_DataPubManifest mani;
   ASSERT_TRUE( manifestOf( bundle, mani ) );
   EXPECT_EQ  ( mani.count( US_DataPub::Experiment ), 1 );
}

TEST_F( DataPubCli, ADryRunNeedsNoBundle )
{
   EXPECT_EQ( run( QStringList() << "--mode" << "export" << "--experiment-id"
                                 << runID << "--dry-run" ), 0 );

   // ... but a real export does
   EXPECT_EQ( run( QStringList() << "--mode" << "export" << "--experiment-id"
                                 << runID ), 2 );
}

TEST_F( DataPubCli, AnExportOfNothingIsRefused )
{
   EXPECT_EQ( run( QStringList() << "--mode" << "export" << "--bundle"
                                 << root + "/nothing.tar.gz" ), 2 );
   EXPECT_FALSE( QFile::exists( root + "/nothing.tar.gz" ) );
}

TEST_F( DataPubCli, AnUnknownGuidFailsTheExport )
{
   QString bundle = root + "/ghost.tar.gz";

   EXPECT_EQ( run( QStringList() << "--mode" << "export" << "--experiment-id"
                                 << runID << "--model-guid"
                                 << US_Util::new_guid()
                                 << "--bundle" << bundle ), 1 );
   EXPECT_FALSE( QFile::exists( bundle ) );

   EXPECT_EQ( run( QStringList() << "--mode" << "export" << "--experiment-id"
                                 << runID << "--noise-guid"
                                 << US_Util::new_guid()
                                 << "--bundle" << bundle ), 1 );
   EXPECT_FALSE( QFile::exists( bundle ) );
}

TEST_F( DataPubCli, ARunOfAnotherProjectIsRefused )
{
   EXPECT_EQ( run( QStringList() << "--mode" << "export" << "--experiment-id"
                                 << runID << "--project-guid"
                                 << US_Util::new_guid() << "--dry-run" ), 1 );
}

TEST_F( DataPubCli, ProjectIdAndGuidMustAgree )
{
   EXPECT_EQ( run( QStringList() << "--mode" << "export" << "--project-id"
                                 << "7" << "--project-guid"
                                 << US_Util::new_guid() << "--dry-run" ), 2 );

   EXPECT_EQ( run( QStringList() << "--mode" << "export" << "--project-id"
                                 << "7" << "--project-guid" << projectGUID
                                 << "--max-scope" << "rawData"
                                 << "--dry-run" ), 0 );
}

TEST_F( DataPubCli, ANonInteractiveRunWithoutAPasswordStops )
{
   EXPECT_EQ( run( QStringList() << "--mode" << "export" << "--source" << "db"
                                 << "--experiment-id" << runID
                                 << "--non-interactive" << "--dry-run" ), 2 );

}

// A password file that cannot be read is a failure, not a usage error, and
// it is reported once
TEST_F( DataPubCli, AnUnreadablePasswordFileFailsWithOneError )
{
#if defined( Q_OS_UNIX )
   QString errText;

   EXPECT_EQ( runCapturing( QStringList() << "--mode" << "export"
                            << "--source" << "db" << "--experiment-id" << runID
                            << "--db-password-file" << root + "/no_such_file"
                            << "--dry-run", errText ), 1 );
   EXPECT_EQ( errText.count( "Error:" ), 1 ) << errText.toStdString();
   EXPECT_TRUE( errText.contains( "no_such_file" ) ) << errText.toStdString();

   // A directory opens, but cannot be read
   EXPECT_EQ( runCapturing( QStringList() << "--mode" << "export"
                            << "--source" << "db" << "--experiment-id" << runID
                            << "--db-password-file" << root
                            << "--dry-run", errText ), 1 );
   EXPECT_EQ( errText.count( "Error:" ), 1 ) << errText.toStdString();
#else
   GTEST_SKIP() << "the error output is captured on Unix only";
#endif
}

TEST_F( DataPubCli, APasswordFileThatIsADirectoryIsNotRead )
{
   QString pw, warning, error;

   EXPECT_FALSE( US_DataPubCLI::readPasswordFile( root, pw, warning, error ) );
   EXPECT_FALSE( error.isEmpty() );
   EXPECT_TRUE ( pw.isEmpty() );
}

// --include-<scope> selects the scope also when --max-scope caps it
TEST_F( DataPubCli, IncludeCountsUnderAMaxScope )
{
   QString bundle = root + "/include.tar.gz";

   ASSERT_EQ( run( QStringList() << "--mode" << "export" << "--experiment-id"
                                 << runID << "--max-scope" << "noise"
                                 << "--include-solutions"
                                 << "--bundle" << bundle ), 0 );

   US_DataPubManifest mani;
   ASSERT_TRUE( manifestOf( bundle, mani ) );
   EXPECT_EQ( mani.scope, QString( "solutions" ) );
   EXPECT_EQ( mani.count( US_DataPub::EditedData ), 0 );
   EXPECT_EQ( mani.count( US_DataPub::Model ), 0 );

   // Beyond the cap is still a usage error, and so is a cap that is no scope
   EXPECT_EQ( run( QStringList() << "--mode" << "export" << "--experiment-id"
                                 << runID << "--max-scope" << "rawData"
                                 << "--include-models" << "--dry-run" ), 2 );
   EXPECT_EQ( run( QStringList() << "--mode" << "export" << "--experiment-id"
                                 << runID << "--max-scope" << "everything"
                                 << "--include-models" << "--dry-run" ), 2 );
}

// A run ID made only of digits is the run ID, not another run's database ID
TEST_F( DataPubCli, AnExactRunIdWinsOverADatabaseId )
{
   QList< US_DataPubCatalog::Run > runs;
   US_DataPubCatalog::Run          one;
   one.runID = "12345";
   one.id    = "7";
   US_DataPubCatalog::Run          two;
   two.runID = "other_run";
   two.id    = "12345";
   runs << two << one;

   QString why;
   EXPECT_EQ( US_DataPubCLI::resolveRun( runs, "12345", why ),
              QString( "12345" ) );
   EXPECT_TRUE( why.contains( "other_run" ) ) << why.toStdString();

   EXPECT_EQ( US_DataPubCLI::resolveRun( runs, "7", why ), QString( "12345" ) );
   EXPECT_TRUE( why.isEmpty() );

   // An ID two runs share names neither
   US_DataPubCatalog::Run three;
   three.runID = "third_run";
   three.id    = "7";
   runs << three;
   EXPECT_TRUE( US_DataPubCLI::resolveRun( runs, "7", why ).isEmpty() );
   EXPECT_TRUE( why.contains( "third_run" ) ) << why.toStdString();

   EXPECT_TRUE( US_DataPubCLI::resolveRun( runs, "nothing", why ).isEmpty() );
   EXPECT_FALSE( why.isEmpty() );
}

// An option of the other mode would be ignored without a word
TEST_F( DataPubCli, OptionsOfTheOtherModeAreRefused )
{
   EXPECT_EQ( run( QStringList() << "--mode" << "export" << "--experiment-id"
                                 << runID << "--target" << "disk"
                                 << "--dry-run" ), 2 );
   EXPECT_EQ( run( QStringList() << "--mode" << "export" << "--experiment-id"
                                 << runID << "--on-conflict-solutions"
                                 << "rename" << "--dry-run" ), 2 );
   EXPECT_EQ( run( QStringList() << "--mode" << "export" << "--experiment-id"
                                 << runID << "--strict-hardware"
                                 << "--dry-run" ), 2 );

   QString bundle = root + "/no_such_bundle.tar.gz";

   EXPECT_EQ( run( QStringList() << "--mode" << "import" << "--bundle"
                                 << bundle << "--model-guid"
                                 << US_Util::new_guid() ), 2 );
   EXPECT_EQ( run( QStringList() << "--mode" << "import" << "--bundle"
                                 << bundle << "--include-models" ), 2 );
   EXPECT_EQ( run( QStringList() << "--mode" << "import" << "--bundle"
                                 << bundle << "--source" << "db" ), 2 );

   // What both modes take is taken by both
   EXPECT_EQ( run( QStringList() << "--mode" << "export" << "--experiment-id"
                                 << runID << "--project-guid" << projectGUID
                                 << "--max-scope" << "rawData"
                                 << "--dry-run" ), 0 );
   EXPECT_EQ( run( QStringList() << "--mode" << "import" << "--bundle"
                                 << bundle << "--target" << "disk" ), 1 );
}

// Only the line end is taken off a password; spaces belong to it
TEST_F( DataPubCli, APasswordKeepsEverythingButItsLineEnd )
{
   EXPECT_EQ( US_DataPubCLI::chopLineEnd( "secret\n" ),     "secret" );
   EXPECT_EQ( US_DataPubCLI::chopLineEnd( "secret\r\n" ),   "secret" );
   EXPECT_EQ( US_DataPubCLI::chopLineEnd( " secret \n" ),   " secret " );
   EXPECT_EQ( US_DataPubCLI::chopLineEnd( "secret\n\n" ),   "secret\n" );
   EXPECT_EQ( US_DataPubCLI::chopLineEnd( "secret" ),       "secret" );

   QString path = root + "/pw.txt";
   QFile   file( path );
   ASSERT_TRUE( file.open( QIODevice::WriteOnly ) );
   file.write( " pass word \r\nsecond line\n" );
   file.close();
   ASSERT_TRUE( QFile::setPermissions( path, QFileDevice::ReadOwner |
                                             QFileDevice::WriteOwner ) );

   QString pw, warning, error;
   ASSERT_TRUE( US_DataPubCLI::readPasswordFile( path, pw, warning, error ) )
      << error.toStdString();
   EXPECT_EQ  ( pw, " pass word " );
   EXPECT_TRUE( warning.isEmpty() ) << warning.toStdString();

   EXPECT_FALSE( US_DataPubCLI::readPasswordFile( root + "/none", pw, warning,
                                                  error ) );
   EXPECT_FALSE( error.isEmpty() );
}

#if defined( Q_OS_UNIX )
TEST_F( DataPubCli, APasswordFileOthersCanReadIsWarnedAbout )
{
   QString path = root + "/pw_open.txt";
   QFile   file( path );
   ASSERT_TRUE( file.open( QIODevice::WriteOnly ) );
   file.write( "secret\n" );
   file.close();
   ASSERT_TRUE( QFile::setPermissions( path, QFileDevice::ReadOwner |
                                             QFileDevice::WriteOwner |
                                             QFileDevice::ReadGroup |
                                             QFileDevice::ReadOther ) );

   QString pw, warning, error;
   ASSERT_TRUE( US_DataPubCLI::readPasswordFile( path, pw, warning, error ) );
   EXPECT_EQ  ( pw, "secret" );
   EXPECT_TRUE( warning.contains( "other users" ) ) << warning.toStdString();
}
#endif

// A password from the file or the environment is taken: the run goes on to
// the database (and fails there, with no server) rather than stopping for
// want of a password
TEST_F( DataPubCli, APasswordFromAFileOrTheEnvironmentIsTaken )
{
   QString path = root + "/pw_ok.txt";
   QFile   file( path );
   ASSERT_TRUE( file.open( QIODevice::WriteOnly ) );
   file.write( "secret\n" );
   file.close();
   QFile::setPermissions( path, QFileDevice::ReadOwner |
                                QFileDevice::WriteOwner );

   EXPECT_EQ( run( QStringList() << "--mode" << "export" << "--source" << "db"
                                 << "--experiment-id" << runID
                                 << "--db-password-file" << path
                                 << "--non-interactive" << "--dry-run" ), 1 );

   qputenv( "US3_DB_PASSWORD", "secret\n" );
   int code = run( QStringList() << "--mode" << "export" << "--source" << "db"
                                 << "--experiment-id" << runID
                                 << "--non-interactive" << "--dry-run" );
   qunsetenv( "US3_DB_PASSWORD" );

   EXPECT_EQ( code, 1 );
}
